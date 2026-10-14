// Range-hopping integration test: three windows, one transmission in each,
// all of which must be found and recorded while the scanner hops.
#include "engine/scanner.h"
#include "sim_source.h"
#include "util/log.h"

#include <cstdio>
#include <filesystem>
#include <thread>

std::vector<SimSignal> SimSource::signals;

int main()
{
    std::filesystem::remove_all("hoptest_rec");
    // One tuner hears one window at a time, so transmissions are staggered.
    SimSource::signals = {{118.500e6, 7.0, 3.0, 0.05}, {120.100e6, 12.0, 3.0, 0.05}, {121.700e6, 17.0, 3.0, 0.05},
                          {120.600e6, 21.5, 0.2, 0.05} /* too short: discarded */};
    Config c;
    c.device = "sim";
    c.sampleRate = 2.4e6;
    std::string err;
    parseRanges("118-123.5", c.ranges, err);
    c.recordingsDir = "hoptest_rec";
    c.mp3Auto = false;
    RecordingStore store;
    store.setRoot(c.recordingsDir);
    Scanner sc(store);
    if (!sc.start(c, err)) { std::printf("start failed: %s\n", err.c_str()); return 1; }
    for (int i = 0; i < 25; ++i)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        auto st = sc.status();
        std::printf("t=%2d window %d/%zu live %zu det %llu hist0 ", i + 1, st.windowIdx, st.windows.size(), st.live.size(),
                    (unsigned long long)st.detections);
        Scanner::History h;
        sc.history(0, h);
        std::printf("%zu rows\n", h.timeMs.size());
    }
    sc.stop();
    RecordingStore::Query q;
    q.kind = 0;
    int total = 0;
    auto items = store.list(q, total);
    int ok = 0;
    for (double f : {118.500e6, 120.100e6, 121.700e6})
        for (auto& r : items)
            if (std::fabs(r.freqHz - f) < 1000) { ++ok; std::printf("found %.4f MHz %d ms\n", r.freqHz / 1e6, r.durationMs); break; }
    std::printf("%d/3 transmissions recorded, %d total -> %s\n", ok, total, ok == 3 && total == 3 ? "PASS" : "FAIL");
    return ok == 3 && total == 3 ? 0 : 1;
}
