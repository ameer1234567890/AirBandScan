// SSTV decoder (ISS ARISS events, CubeSats, amateur radio).
//
// FM receiver audio in; images out. The audio is mixed down around 1900 Hz,
// low-passed and turned into an instantaneous-frequency trace. A VIS header
// (1900 Hz leader, 1200 Hz start bit, 7 data bits + parity at 30 ms, stop
// bit) selects the mode; each line is then located from its 1200 Hz sync
// pulse and a least-squares fit of the sync times corrects slant (clock
// error of the transmitter). Pixels are the mean frequency over their slot,
// 1500 Hz = black .. 2300 Hz = white.
//
// Modes: PD50/90/120/160/180/240/290 (ISS: PD120, PD180), Robot 36/72,
// Martin M1/M2, Scottie S1/S2/DX.
#pragma once

#include "dsp/channelizer.h"
#include "sat/image.h"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

class SstvDecoder
{
public:
    struct Result
    {
        Image img;
        std::string mode;
        double startSec = 0;   // audio time of the VIS end
        int rows = 0, totalRows = 0;
        bool complete = false;
    };

    explicit SstvDecoder(double audioRate);

    void process(const float* audio, int n);
    void flush();                       // end of input: emit a partial image

    bool busy() const { return mode_ >= 0; }
    std::string modeName() const;
    int rowsDone() const { return rowsDone_; }
    int rowsTotal() const;
    Image partial() const { return img_; }
    double timeSec() const { return (double)(base_ + (int64_t)fq_.size()) / rate_; }
    // Recent audio frequency (Hz, smoothed) for a tuning indicator.
    float lastFreq() const { return smooth_; }

    bool take(Result& r);               // pops a finished image

    static std::vector<std::string> modeNames();

private:
    void detectVis();
    void startImage(int mode, double visEnd);
    void decodeLines();
    void decodeLine(int n, double sync);
    void finish(bool complete);
    double meanFreq(double t0, double t1) const;  // times in trace samples
    uint8_t pixel(double t0, double t1) const;
    bool findSync(double pred, double win, double& pos, double& conf) const;
    void trim();

    double inRate_, rate_;
    int decim_ = 1, phase_ = 0;
    // mixer + low-pass
    double nco_ = 0, ncoInc_ = 0;
    std::vector<float> lp_;
    std::vector<cf> line_;
    int linePos_ = 0;
    cf last_{1.f, 0.f};
    float smooth_ = 0.f;

    // frequency trace, absolute sample index = base_ + i
    int64_t base_ = 0;
    std::vector<float> fq_;
    std::vector<double> cum_;           // prefix sums of fq_
    // 1 ms averages for VIS detection
    std::deque<float> ms_;
    double msAcc_ = 0;
    int msN_ = 0, msLen_ = 1;
    int64_t msCount_ = 0;
    int visBestErr_ = 1 << 30;
    int visMode_ = -1;
    int64_t visEndMs_ = 0, visSeenMs_ = 0;

    // current image
    int mode_ = -1;
    double visEnd_ = 0;                 // trace sample index
    int nextLine_ = 0, rowsDone_ = 0, badSyncs_ = 0;
    std::vector<std::pair<int, double>> syncs_; // accepted (line, position)
    double fitA_ = 0, fitB_ = 0;        // sync(n) = A + B n
    Image img_;
    std::vector<uint8_t> chromaPrev_;   // Robot36: previous line's chroma
    bool chromaPrevCr_ = false;
    std::vector<uint8_t> yPrev_;
    std::deque<Result> done_;
};
