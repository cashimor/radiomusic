#pragma once
#include <atomic>
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace music {
constexpr int sampleRate = 44100;
constexpr int loopBeats = 32;
constexpr int steadyPasses = 2;
constexpr int transitionPasses = 1;
constexpr double maxSpeedChange = 0.10;
constexpr size_t maxStoredClips = 1024;
constexpr size_t maxResidentClips = 16;
constexpr double crossoverHz = 150.0;
struct Frame { float l = 0, r = 0; };
struct Analysis {
    double bpm = 0;
    double firstBeat = 0;
    double confidence = 0;
    double rms = 0;
};
struct Clip {
    std::string id;
    std::string source;
    double bpm = 0, confidence = 0;
    int beats = loopBeats;
    // -1 = uncertain. Otherwise C..B major = 0..11, minor = 12..23.
    int key = -1;
    double keyConfidence = 0;
    std::string captureId;
    int firstSourceBar = 0;
    double sourceSeconds = 0;
    std::vector<Frame> audio;
    std::vector<Frame> low, high;
    std::atomic<bool> rejected{false};
};
using ClipPtr = std::shared_ptr<Clip>;
struct KeyEstimate { int key = -1; double confidence = 0; };
KeyEstimate estimateKey(const std::vector<Frame>& audio);
std::string keyName(int key);
bool compatibleKeys(int a, int b);
bool compatibleTempo(double sourceBpm, double targetBpm);
int playbackKey(const Clip& clip, double targetBpm);
// Precomputed on the storage worker; never filters/allocates in the audio renderer.
void prepareBands(Clip& clip);
uint32_t sourceColorId(const std::string& id);
std::string sourceLabel(const std::string& id);
struct BarInfo {
    std::string captureId, clipId;
    int sourceBar = 0, key = -1;
    std::array<int, 4> beats{0, 1, 2, 3};
    bool changed = false;
    std::string lowCaptureId, lowClipId;
    int lowSourceBar = 0;
    std::array<int, 4> lowBeats{0, 1, 2, 3};
    bool split = false;
};
Analysis analyze(const std::vector<Frame>& audio);
std::vector<ClipPtr> extract(const std::vector<Frame>& audio, const Analysis& analysis,
                             const std::string& prefix, const std::string& source);
bool saveClip(const std::filesystem::path& dir, const Clip& clip);
ClipPtr loadClip(const std::filesystem::path& wave, bool metadataOnly = false);
bool writeWave(const std::filesystem::path& path, const std::vector<Frame>& audio);

// No networking, file access or Windows calls in the musical renderer.
class Mixer {
public:
    void requestEvolution() { requested_ = true; }
    void setPlayful(bool value) { if (playful_ != value) requested_ = true; playful_ = value; }
    void setSplit(bool value) { if (split_ != value) requested_ = true; split_ = value; }
    bool reverbSend() const;
    Frame next(const std::vector<ClipPtr>& library);
    std::vector<ClipPtr> audible() const;
    std::vector<ClipPtr> sources() const;
    std::array<BarInfo, 8> bars(bool incoming = false) const;
    int key() const { return current_ ? playbackKey(*current_, bpm_) : -1; }
    double fade() const { return fade_; }
    int passes() const { return repeats_; }
    double bpm() const { return bpm_; }
    double beat() const { return phase_ * loopBeats; }
    bool ready() const { return static_cast<bool>(current_); }
    bool transitioning() const { return changing_; }
private:
    struct Bar { ClipPtr clip; int index = 0; std::array<int, 4> beats{0, 1, 2, 3}; };
    using Arrangement = std::array<Bar, 8>;
    struct Sequence { Arrangement upper{}, bass{}; bool split = false; };
    Arrangement straight(const ClipPtr& clip) const;
    Sequence compose(const std::vector<ClipPtr>& library);
    void evolve(const std::vector<ClipPtr>& library);
    Frame readArrangement(const Arrangement& arrangement, double phase, int band = 0) const;
    Frame readSequence(const Sequence& sequence, double phase) const;
    ClipPtr pick(const std::vector<ClipPtr>& library, bool anyTempo, const ClipPtr& avoid = {});
    Frame read(const ClipPtr& clip, double phase, int band = 0) const;
    ClipPtr current_, incoming_;
    Sequence sequence_{}, nextSequence_{};
    unsigned variations_ = 0;
    bool changing_ = false, playful_ = true, split_ = true;
    std::mt19937 random_{std::random_device{}()};
    double phase_ = 0, bpm_ = 0, fade_ = 0, gain_ = 0;
    int repeats_ = 0, transitionCycles_ = 0;
    bool requested_ = false;
};
}
