#include "sat/sstv.h"

#include <algorithm>
#include <cmath>

namespace {

enum Layout { kMartin, kScottie, kRobot36, kRobot72, kPd };

struct Mode
{
    const char* name;
    int vis;
    Layout layout;
    int w, h;
    double syncMs, chMs, lineMs; // chMs: scan time of one colour / luma channel
};

const Mode kModes[] = {
    {"PD50", 93, kPd, 320, 256, 20.0, 91.520, 388.160},
    {"PD90", 99, kPd, 320, 256, 20.0, 170.240, 703.040},
    {"PD120", 95, kPd, 640, 496, 20.0, 121.600, 508.480},
    {"PD160", 98, kPd, 512, 400, 20.0, 195.584, 804.416},
    {"PD180", 96, kPd, 640, 496, 20.0, 183.040, 754.240},
    {"PD240", 97, kPd, 640, 496, 20.0, 244.480, 1000.000},
    {"PD290", 94, kPd, 800, 616, 20.0, 228.800, 937.280},
    {"Robot 36", 8, kRobot36, 320, 240, 9.0, 88.0, 150.0},
    {"Robot 72", 12, kRobot72, 320, 240, 9.0, 138.0, 300.0},
    {"Martin M1", 44, kMartin, 320, 256, 4.862, 146.432, 446.446},
    {"Martin M2", 40, kMartin, 320, 256, 4.862, 73.216, 226.798},
    {"Scottie S1", 60, kScottie, 320, 256, 9.0, 138.240, 428.220},
    {"Scottie S2", 56, kScottie, 320, 256, 9.0, 88.064, 277.692},
    {"Scottie DX", 76, kScottie, 320, 256, 9.0, 345.600, 1050.300},
};
constexpr int kNumModes = (int)(sizeof(kModes) / sizeof(kModes[0]));

int scanLines(const Mode& m) { return m.layout == kPd ? m.h / 2 : m.h; }

constexpr float kSyncThresh = 1350.f;   // sync 1200 Hz, black 1500 Hz

inline uint8_t clamp8(double v) { return (uint8_t)std::clamp((int)std::lround(v), 0, 255); }

void ycc(uint8_t y, uint8_t cr, uint8_t cb, uint8_t* rgb)
{
    double Y = y, R = cr - 128.0, B = cb - 128.0;
    rgb[0] = clamp8(Y + 1.402 * R);
    rgb[1] = clamp8(Y - 0.344 * B - 0.714 * R);
    rgb[2] = clamp8(Y + 1.772 * B);
}

float median(std::deque<float>::const_iterator b, int n)
{
    float v[64];
    n = std::min(n, 64);
    for (int i = 0; i < n; ++i) v[i] = *(b + i);
    std::nth_element(v, v + n / 2, v + n);
    return v[n / 2];
}

} // namespace

SstvDecoder::SstvDecoder(double audioRate) : inRate_(audioRate)
{
    decim_ = std::max(1, (int)std::floor(audioRate / 11000.0));
    rate_ = audioRate / decim_;
    ncoInc_ = 2 * M_PI * 1900.0 / audioRate;
    // Keep +-1.2 kHz around 1900 Hz (1100..2300 Hz), reject the mixing image
    // at -3 kHz and beyond.
    lp_ = designLowPass(audioRate, 1500.0, (int)std::ceil(5.5 * audioRate / 1200.0) | 1);
    line_.assign(lp_.size() * 2, cf(0, 0));
    msLen_ = std::max(1, (int)std::lround(rate_ / 1000.0));
    cum_.push_back(0.0);
}

std::vector<std::string> SstvDecoder::modeNames()
{
    std::vector<std::string> v;
    for (auto& m : kModes) v.push_back(m.name);
    return v;
}

std::string SstvDecoder::modeName() const { return mode_ >= 0 ? kModes[mode_].name : ""; }
int SstvDecoder::rowsTotal() const { return mode_ >= 0 ? kModes[mode_].h : 0; }

bool SstvDecoder::take(Result& r)
{
    if (done_.empty()) return false;
    r = std::move(done_.front());
    done_.pop_front();
    return true;
}

void SstvDecoder::process(const float* audio, int n)
{
    const int L = (int)lp_.size();
    for (int i = 0; i < n; ++i)
    {
        cf x(audio[i] * (float)std::cos(nco_), -audio[i] * (float)std::sin(nco_));
        nco_ += ncoInc_;
        if (nco_ > 2 * M_PI) nco_ -= 2 * M_PI;
        line_[(size_t)linePos_] = x;
        line_[(size_t)(linePos_ + L)] = x;
        if (++linePos_ >= L) linePos_ = 0;
        if (++phase_ < decim_) continue;
        phase_ = 0;
        const cf* p = &line_[(size_t)linePos_];
        float re = 0, im = 0;
        for (int k = 0; k < L; ++k)
        {
            re += p[k].real() * lp_[(size_t)(L - 1 - k)];
            im += p[k].imag() * lp_[(size_t)(L - 1 - k)];
        }
        cf z(re, im);
        float f = 1900.f + std::arg(z * std::conj(last_)) * (float)(rate_ / (2 * M_PI));
        last_ = z;
        f = std::clamp(f, 900.f, 2700.f);
        smooth_ += (f - smooth_) * 0.002f;
        fq_.push_back(f);
        cum_.push_back(cum_.back() + f);

        msAcc_ += f;
        if (++msN_ >= msLen_)
        {
            ms_.push_back((float)(msAcc_ / msN_));
            if (ms_.size() > 700) ms_.pop_front();
            msAcc_ = 0;
            msN_ = 0;
            ++msCount_;
            if (mode_ < 0) detectVis();
        }
    }
    if (mode_ >= 0) decodeLines();
    trim();
}

void SstvDecoder::detectVis()
{
    // ms_ holds 1 ms averages, newest last. The stop bit ends now.
    const int need = 10 + 250 + 300;
    int N = (int)ms_.size();
    if (N < need) return;
    int s = N - 300;                      // start bit's first ms
    auto at = ms_.cbegin();
    auto seg = [&](int a, int len) { return median(at + a, len); };

    // Leader: mean of 250 ms must be ~1900 Hz.
    double lead = 0;
    for (int k = s - 260; k < s - 10; ++k) lead += *(at + k);
    lead /= 250.0;
    if (std::fabs(lead - 1900.0) > 50.0) return;
    if (std::fabs(seg(s + 5, 20) - 1200.f) > 70.f) return;     // start bit
    if (std::fabs(seg(s + 275, 20) - 1200.f) > 70.f) return;   // stop bit
    int code = 0, ones = 0;
    for (int b = 0; b < 8; ++b)
    {
        float f = seg(s + 30 + 30 * b + 5, 20);
        int bit;
        if (f > 1030.f && f < 1170.f) bit = 1;
        else if (f > 1230.f && f < 1370.f) bit = 0;
        else return;
        ones += bit;
        if (b < 7) code |= bit << b;
    }
    if (ones & 1) return;                 // even parity
    int mode = -1;
    for (int m = 0; m < kNumModes; ++m)
        if (kModes[m].vis == code) mode = m;
    if (mode < 0) return;

    // Timing: the leader -> start bit edge is sharp; find it to the sample.
    int64_t msAbs0 = msCount_ - N;       // absolute ms index of ms_[0]
    int64_t guess = (msAbs0 + s) * msLen_;
    double edge = (double)guess;
    float bestScore = -1e9f;
    int half = msLen_ * 2;
    // The pattern can match ~10 ms early (the stop bit runs into a 1200 Hz sync).
    for (int64_t t = guess - 25 * msLen_; t <= guess + 25 * msLen_; ++t)
    {
        double before = meanFreq((double)(t - half), (double)t);
        double after = meanFreq((double)t, (double)(t + half));
        float sc = (float)(before - after); // 1900 -> 1200: largest drop
        if (sc > bestScore) { bestScore = sc; edge = (double)t; }
    }
    startImage(mode, edge + 300.0 * rate_ / 1000.0);
}

void SstvDecoder::startImage(int mode, double visEnd)
{
    const Mode& m = kModes[mode];
    mode_ = mode;
    visEnd_ = visEnd;
    nextLine_ = 0;
    rowsDone_ = 0;
    badSyncs_ = 0;
    syncs_.clear();
    img_ = Image(m.w, m.h, 3, 0);
    yPrev_.clear();
    chromaPrev_.clear();
    double r = rate_ / 1000.0;
    // First sync pulse: right after the VIS, except Scottie (an extra 9 ms
    // start sync, then separator, green, separator, blue before the sync).
    fitA_ = visEnd + (m.layout == kScottie ? (9.0 + 3.0 + 2.0 * m.chMs) * r : 0.0);
    fitB_ = m.lineMs * r;
}

bool SstvDecoder::findSync(double pred, double win, double& pos, double& conf) const
{
    // Locate the sync pulse by its trailing edge (1200 Hz -> porch): the
    // leading edge can be hidden, e.g. by the VIS stop bit before line 0.
    const Mode& m = kModes[mode_];
    int L = std::max(2, (int)std::lround(m.syncMs * rate_ / 1000.0));
    int h = std::max(2, std::min(L, (int)std::lround(3.0 * rate_ / 1000.0)));
    int64_t t0 = (int64_t)std::floor(pred + L - win), t1 = (int64_t)std::ceil(pred + L + win);
    if (t0 - h < base_) t0 = base_ + h;
    if (t1 + h > base_ + (int64_t)fq_.size()) return false;
    auto low = [&](int64_t k) { return fq_[(size_t)(k - base_)] < kSyncThresh ? 1 : 0; };
    int before = 0, after = 0;
    for (int64_t k = t0 - h; k < t0; ++k) before += low(k);
    for (int64_t k = t0; k < t0 + h; ++k) after += low(k);
    int best = -(1 << 30);
    int64_t bs = t0, be = t0;
    for (int64_t t = t0; t <= t1; ++t)
    {
        int sc = before - after;
        if (sc > best) { best = sc; bs = be = t; }
        else if (sc == best) be = t;
        before += low(t) - low(t - h);
        after += low(t + h) - low(t);
    }
    pos = 0.5 * (double)(bs + be) - L;
    conf = (double)best / h;
    return true;
}

void SstvDecoder::decodeLines()
{
    while (mode_ >= 0)
    {
        const Mode& m = kModes[mode_];
        double r = rate_ / 1000.0;
        int n = nextLine_;
        double pred = fitA_ + fitB_ * n;
        double win = std::min(10.0, 0.05 * m.lineMs) * r;
        // Wait until the whole line (and the sync search window) has arrived.
        if (pred + m.lineMs * r + win + 2 * m.syncMs * r > (double)(base_ + (int64_t)fq_.size())) return;

        double pos, conf;
        bool found = findSync(pred, win, pos, conf);
        double tol = syncs_.size() >= 3 ? 2.5 * r : win;
        if (found && conf > 0.6 && std::fabs(pos - pred) <= tol)
        {
            syncs_.push_back({n, pos});
            badSyncs_ = 0;
            // Least squares sync(n) = A + B n over accepted lines (slant fix);
            // the slope only once enough lines are in, the offset right away.
            size_t k = syncs_.size();
            double sx = 0, sy = 0, sxx = 0, sxy = 0;
            for (auto& [ln, p] : syncs_)
            {
                sx += ln; sy += p; sxx += (double)ln * ln; sxy += ln * p;
            }
            double den = k * sxx - sx * sx;
            double B = fitB_;
            if (k >= 8 && den > 0)
            {
                double b = (k * sxy - sx * sy) / den;
                if (std::fabs(b / (m.lineMs * r) - 1.0) < 0.005) B = b; // within 0.5 %
            }
            fitB_ = B;
            fitA_ = (sy - B * sx) / k;
        }
        else
            ++badSyncs_;

        decodeLine(n, fitA_ + fitB_ * n);
        ++nextLine_;
        if (nextLine_ >= scanLines(m)) { finish(true); return; }
        if (badSyncs_ >= 20) { finish(false); return; } // signal lost
    }
}

double SstvDecoder::meanFreq(double t0, double t1) const
{
    int64_t n = (int64_t)fq_.size();
    int64_t i0 = std::clamp((int64_t)std::floor(t0) - base_, (int64_t)0, n - 1);
    int64_t i1 = std::clamp((int64_t)std::floor(t1) - base_, i0 + 1, n);
    if (n <= 0) return 1500.0;
    return (cum_[(size_t)i1] - cum_[(size_t)i0]) / (double)(i1 - i0);
}

uint8_t SstvDecoder::pixel(double t0, double t1) const
{
    return clamp8((meanFreq(t0, t1) - 1500.0) * 255.0 / 800.0);
}

void SstvDecoder::decodeLine(int n, double S)
{
    const Mode& m = kModes[mode_];
    const double r = rate_ / 1000.0;
    const int W = m.w;
    auto scan = [&](double start, double durMs, std::vector<uint8_t>& out) {
        out.resize((size_t)W);
        double px = durMs * r / W;
        for (int x = 0; x < W; ++x) out[(size_t)x] = pixel(start + x * px, start + (x + 1) * px);
    };
    std::vector<uint8_t> a, b, c, d;
    auto putRgb = [&](int row, const std::vector<uint8_t>& R, const std::vector<uint8_t>& G,
                      const std::vector<uint8_t>& B) {
        if (row >= m.h) return;
        uint8_t* p = img_.row(row);
        for (int x = 0; x < W; ++x)
        {
            p[3 * x] = R[(size_t)x];
            p[3 * x + 1] = G[(size_t)x];
            p[3 * x + 2] = B[(size_t)x];
        }
        rowsDone_ = std::max(rowsDone_, row + 1);
    };
    auto putYcc = [&](int row, const std::vector<uint8_t>& Y, const std::vector<uint8_t>& Cr,
                      const std::vector<uint8_t>& Cb) {
        if (row >= m.h) return;
        uint8_t* p = img_.row(row);
        for (int x = 0; x < W; ++x) ycc(Y[(size_t)x], Cr[(size_t)x], Cb[(size_t)x], p + 3 * x);
        rowsDone_ = std::max(rowsDone_, row + 1);
    };

    switch (m.layout)
    {
    case kMartin:
    {
        double g = S + (m.syncMs + 0.572) * r;
        double bl = g + (m.chMs + 0.572) * r;
        double rd = bl + (m.chMs + 0.572) * r;
        scan(g, m.chMs, a);
        scan(bl, m.chMs, b);
        scan(rd, m.chMs, c);
        putRgb(n, c, a, b);
        break;
    }
    case kScottie:
        scan(S - (2 * m.chMs + 1.5) * r, m.chMs, a); // green
        scan(S - m.chMs * r, m.chMs, b);             // blue
        scan(S + 10.5 * r, m.chMs, c);               // red
        putRgb(n, c, a, b);
        break;
    case kRobot72:
        scan(S + 12.0 * r, 138.0, a);
        scan(S + 156.0 * r, 69.0, b);  // R-Y
        scan(S + 231.0 * r, 69.0, c);  // B-Y
        putYcc(n, a, b, c);
        break;
    case kRobot36:
    {
        // Y, then R-Y on even and B-Y on odd lines; the separator before the
        // chroma (1500 / 2300 Hz) says which one this line carries.
        scan(S + 12.0 * r, 88.0, a);
        bool isCb = meanFreq(S + 100.5 * r, S + 104.0 * r) > 1900.0;
        scan(S + 106.0 * r, 44.0, b);
        std::vector<uint8_t> neutral((size_t)W, 128);
        if (!chromaPrev_.empty() && chromaPrevCr_ == isCb)
        {
            const auto& cr = isCb ? chromaPrev_ : b;
            const auto& cb = isCb ? b : chromaPrev_;
            if (n > 0) putYcc(n - 1, yPrev_, cr, cb);
            putYcc(n, a, cr, cb);
        }
        else
            putYcc(n, a, isCb ? neutral : b, isCb ? b : neutral);
        yPrev_ = a;
        chromaPrev_ = b;
        chromaPrevCr_ = !isCb;
        break;
    }
    case kPd:
        scan(S + (m.syncMs + 2.08) * r, m.chMs, a);
        scan(S + (m.syncMs + 2.08 + m.chMs) * r, m.chMs, b);
        scan(S + (m.syncMs + 2.08 + 2 * m.chMs) * r, m.chMs, c);
        scan(S + (m.syncMs + 2.08 + 3 * m.chMs) * r, m.chMs, d);
        putYcc(2 * n, a, b, c);
        putYcc(2 * n + 1, d, b, c);
        break;
    }
}

void SstvDecoder::finish(bool complete)
{
    const Mode& m = kModes[mode_];
    if (rowsDone_ >= 16)
    {
        Result res;
        res.mode = m.name;
        res.startSec = visEnd_ / rate_;
        res.rows = rowsDone_;
        res.totalRows = m.h;
        res.complete = complete;
        res.img = img_;
        if (!complete)
        {
            res.img.h = rowsDone_;
            res.img.px.resize((size_t)rowsDone_ * m.w * 3);
        }
        done_.push_back(std::move(res));
    }
    mode_ = -1;
    ms_.clear();
    syncs_.clear();
}

void SstvDecoder::flush()
{
    if (mode_ < 0) return;
    decodeLines();
    if (mode_ >= 0) finish(false);
}

void SstvDecoder::trim()
{
    // Keep what the current line may still need, or ~1 s while idle.
    int64_t keep;
    if (mode_ >= 0)
    {
        const Mode& m = kModes[mode_];
        double r = rate_ / 1000.0;
        keep = (int64_t)(fitA_ + fitB_ * nextLine_ - (2 * m.chMs + 60.0) * r);
    }
    else
        keep = base_ + (int64_t)fq_.size() - (int64_t)rate_;
    int64_t drop = keep - base_;
    if (drop < (int64_t)(rate_ * 30)) return; // trim in big steps
    double off = cum_[(size_t)drop];
    fq_.erase(fq_.begin(), fq_.begin() + drop);
    cum_.erase(cum_.begin(), cum_.begin() + drop);
    for (auto& v : cum_) v -= off;
    base_ += drop;
}
