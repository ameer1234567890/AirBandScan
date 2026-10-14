// On-disk archive of transmissions.
//
//   <dir>/YYYY-MM-DD/<id>.wav   8 kHz mono audio
//   <dir>/YYYY-MM-DD/<id>.mp3   MP3 (voice: on the fly; others: on request)
//   <dir>/YYYY-MM-DD/<id>.spg   narrow-band spectrogram (see SpgHeader)
//   <dir>/YYYY-MM-DD/<id>.meta  key = value metadata
//
// The index is rebuilt from the .meta files at start-up, so the folder can be
// copied, pruned or backed up with ordinary file tools.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct RecordingInfo
{
    std::string id;
    int64_t startMs = 0;      // epoch milliseconds
    int     durationMs = 0;
    double  freqHz = 0.0;
    double  peakDb = 0.0;     // peak level above noise (dB)
    double  levelDbfs = 0.0;  // carrier level
    bool    voice = false;
    bool    manual = false;   // voice flag set by the user
    double  score = 0.0;
    double  voicedFrac = 0.0;
    double  levelSpreadDb = 0.0;
    double  modDepth = 0.0;
    std::string reason;
    bool    hasMp3 = false;
    bool    hasSpg = false;
};

// Binary .spg header (little endian), followed by rows*cols uint8 cells,
// cell value v -> dB = dbMin + v * (dbMax - dbMin) / 255.
#pragma pack(push, 1)
struct SpgHeader
{
    char     magic[4];   // "SPG1"
    uint32_t cols;
    uint32_t rows;
    float    rowsPerSec;
    float    binHz;
    float    dbMin;
    float    dbMax;
    double   centerHz;
};
#pragma pack(pop)

class RecordingStore
{
public:
    void setRoot(const std::string& dir);
    std::string root() const;
    void load();

    // Build a fresh id for a transmission starting at startMs on freqHz.
    std::string makeId(int64_t startMs, double freqHz);
    // Absolute file path for id + extension (".wav", ".mp3", ...). Empty if
    // the id is malformed (protects the web server from path traversal).
    std::string path(const std::string& id, const char* ext) const;

    void add(const RecordingInfo& r); // files must already be written
    bool get(const std::string& id, RecordingInfo& out);

    struct Query
    {
        int  kind = 0;          // 0 = all, 1 = voice, 2 = other
        int  offset = 0;
        int  limit = 100;
        double freqHz = 0.0;    // 0 = any; otherwise +-3 kHz match
        int64_t sinceMs = 0;
    };
    std::vector<RecordingInfo> list(const Query& q, int& total);

    bool remove(const std::string& id);
    bool setVoice(const std::string& id, bool voice);
    // Ensure the MP3 exists (encode from the WAV if needed).
    bool ensureMp3(const std::string& id, int kbps, std::string& err);

    int  cleanup(int retentionDays); // returns number removed
    uint64_t version() const { return version_; }
    void counts(int& voice, int& other);
    std::vector<std::pair<double, int>> frequencies(int kind); // freq -> count

private:
    bool writeMeta(const RecordingInfo& r);
    bool removeLocked(const std::string& id);

    mutable std::mutex mtx_;
    std::string root_ = "recordings";
    std::vector<RecordingInfo> items_; // newest first
    uint64_t version_ = 1;
    int64_t lastIdMs_ = 0;
};
