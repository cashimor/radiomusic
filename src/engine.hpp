#pragma once
#include "music.hpp"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_set>

struct Snapshot {
    bool running = false, mixing = false, transitioning = false, waiting = false;
    double bpm = 0, beat = 0, captureSeconds = 0, confidence = 0, peak = 0;
    size_t librarySize = 0;
    size_t residentClips = 0;
    size_t usedLoops = 0;
    int key = -1;
    double fade = 0;
    int passes = 0;
    std::array<music::BarInfo, 8> bars{}, incomingBars{};
    uint64_t decodedFrames = 0, renderedFrames = 0, liveUnderruns = 0;
    std::wstring connection = L"Ready to listen", activity = L"Your loop library starts here.";
    std::vector<std::string> audible;
};

class Engine {
public:
    explicit Engine(std::filesystem::path library);
    ~Engine();
    bool start(const std::wstring& url, bool mute = false);
    void stop();
    void evolve();
    void reject();
    void setSplit(bool value) { split_ = value; }
    bool split() const { return split_; }
    void setPlayful(bool value) { playful_ = value; }
    bool playful() const { return playful_; }
    void setReverb(bool value) { reverb_ = value; }
    bool reverb() const { return reverb_; }
    void setVolume(float value) { volume_ = value; }
    void setRadio(bool value) { radio_ = value; }
    bool radio() const { return radio_; }
    Snapshot snapshot();
    const std::filesystem::path& libraryPath() const { return directory_; }
    bool savePreview(const std::filesystem::path& path);
private:
    void networkLoop(std::wstring url);
    void analysisLoop();
    void outputLoop(bool mute);
    void receive(const music::Frame* frames, size_t count);
    void connection(std::wstring text);
    std::filesystem::path directory_;
    std::atomic<bool> running_{false}, evolve_{false}, radio_{false};
    std::atomic<bool> playful_{true}, split_{true}, reverb_{false};
    std::atomic<float> volume_{0.55f};
    std::thread network_, analysis_, output_;
    std::mutex mutex_;
    std::condition_variable wake_;
    Snapshot state_;
    std::vector<music::ClipPtr> library_, audible_, pinned_;
    std::deque<music::Frame> live_;
    std::vector<music::Frame> capture_, preview_;
    std::deque<std::vector<music::Frame>> jobs_;
    std::vector<music::ClipPtr> erase_;
    std::unordered_set<std::string> rejectedIds_;
    std::unordered_set<std::string> rejectedCaptures_;
    std::unordered_set<std::string> usedLoopIds_;
    std::string source_;
};
