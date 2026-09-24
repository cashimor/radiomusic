#include "engine.hpp"
#include "library.hpp"
#include <algorithm>
#include <chrono>

void Engine::analysisLoop() {
    music::LibraryStore store(directory_);
    bool scanned = store.scan();
    auto refresh = [&] {
        std::vector<music::ClipPtr> pinned, previous;
        double bpm; int key;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pinned = pinned_; previous = library_; bpm = state_.bpm; key = state_.key;
        }
        auto working = store.workingSet(bpm, key, pinned, previous);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            working.erase(std::remove_if(working.begin(), working.end(), [&](const auto& c) {
                if (rejectedIds_.count(c->id) || rejectedCaptures_.count(c->captureId)) c->rejected = true;
                return c->rejected.load();
            }), working.end());
            library_ = std::move(working); state_.librarySize = store.size(); state_.residentClips = library_.size();
        }
    };
    refresh();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.activity = scanned ? L"Library loaded. Gathering audio and rotating the working selection." : L"Cannot read the library folder. Check its permissions.";
    }
    auto nextRefresh = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    unsigned serial = 0;
    while (true) {
        std::vector<music::Frame> audio;
        std::vector<music::ClipPtr> erased;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait_until(lock, nextRefresh, [&] { return !running_ || !jobs_.empty() || !erase_.empty(); });
            erased.swap(erase_);
            if (running_ && !jobs_.empty()) { audio = std::move(jobs_.front()); jobs_.pop_front(); }
        }
        for (const auto& clip : erased) {
            bool removed = store.reject(*clip);
            std::lock_guard<std::mutex> lock(mutex_); state_.librarySize = store.size();
            if (!removed) state_.activity = L"Rejected in this session, but some saved files could not be removed.";
        }
        if (!running_) break;
        if (!audio.empty()) {
            if (store.size() >= music::maxStoredClips) {
                std::lock_guard<std::mutex> lock(mutex_);
                state_.activity = L"Library full (1,024 recordings). Reject material to make room; existing audio is preserved.";
            } else {
                { std::lock_guard<std::mutex> lock(mutex_); state_.activity = L"Analyzing tempo and key for new source recordings..."; }
                auto a = music::analyze(audio);
                if (!running_) continue; // Drain pending bans before the worker exits.
                auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                auto capture = std::to_string(millis) + "-" + std::to_string(serial++);
                auto clips = music::extract(audio, a, capture, source_);
                size_t added = 0; bool saved = true;
                for (const auto& clip : clips) {
                    if (store.size() >= music::maxStoredClips) break;
                    if (store.add(*clip)) ++added; else saved = false;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    state_.confidence = a.confidence; state_.librarySize = store.size();
                    state_.activity = !saved ? L"Could not save new recordings. Check free space and permissions." :
                        added ? L"Saved " + std::to_wstring(added) + L" source recordings. Remixing their bands and beats." :
                        L"No clear beat grid in that capture. Continuing to listen.";
                }
            }
        }
        if (!audio.empty() || !erased.empty() || std::chrono::steady_clock::now() >= nextRefresh) {
            refresh(); nextRefresh = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        }
    }
}
