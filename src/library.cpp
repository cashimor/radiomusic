#include "library.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace music {
bool LibraryStore::scan() {
    entries_.clear();
    bannedCaptures_.clear();
    std::ifstream bans(directory_ / "rejected-captures.txt");
    std::string capture;
    while (bans >> std::quoted(capture)) bannedCaptures_.insert(capture);
    std::error_code ec; std::filesystem::create_directories(directory_, ec);
    if (ec) return false;
    for (std::filesystem::directory_iterator it(directory_, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() != ".wav") continue;
        auto entry = loadClip(it->path(), true);
        if (entry && !bannedCaptures_.count(entry->captureId)) entries_.push_back(entry);
    }
    if (ec) return false;
    pruneToDuration();
    return true;
}
bool LibraryStore::add(const Clip& clip) {
    if (bannedCaptures_.count(clip.captureId) || std::filesystem::exists(directory_ / (clip.id + ".rejected")) || entries_.size() >= maxStoredClips || !saveClip(directory_, clip)) return false;
    auto metadata = loadClip(directory_ / (clip.id + ".wav"), true);
    if (!metadata) return false;
    entries_.push_back(metadata);
    pruneToDuration();
    return true;
}
void LibraryStore::pruneToDuration() {
    struct Capture {
        std::filesystem::file_time_type oldest = std::filesystem::file_time_type::max();
        double seconds = 0;
        std::vector<std::string> ids;
    };
    std::map<std::string, Capture> captures;
    double totalSeconds = 0;
    for (const auto& entry : entries_) {
        if (!entry || entry->bpm <= 0 || entry->beats <= 0) continue;
        const double seconds = entry->beats * 60.0 / entry->bpm;
        auto& capture = captures[entry->captureId.empty() ? entry->id : entry->captureId];
        capture.seconds += seconds;
        capture.ids.push_back(entry->id);
        totalSeconds += seconds;
        std::error_code ec;
        const auto modified = std::filesystem::last_write_time(directory_ / (entry->id + ".wav"), ec);
        if (ec) capture.oldest = std::filesystem::file_time_type::min();
        else capture.oldest = std::min(capture.oldest, modified);
    }
    if (totalSeconds <= maxStoredAudioSeconds) return;

    std::vector<std::pair<std::string, Capture*>> oldestFirst;
    oldestFirst.reserve(captures.size());
    for (auto& [captureId, capture] : captures) oldestFirst.emplace_back(captureId, &capture);
    std::sort(oldestFirst.begin(), oldestFirst.end(), [](const auto& a, const auto& b) {
        if (a.second->oldest != b.second->oldest) return a.second->oldest < b.second->oldest;
        return a.first < b.first;
    });

    for (const auto& item : oldestFirst) {
        if (totalSeconds <= maxStoredAudioSeconds) break;
        const auto& captureId = item.first;
        const auto* capture = item.second;
        if (protectedCaptures_.count(captureId)) continue;
        bool removedAll = true;
        for (const auto& id : capture->ids) {
            for (const char* extension : {".wav", ".txt", ".rejected"}) {
                std::error_code ec;
                std::filesystem::remove(directory_ / (id + extension), ec);
                if (ec) removedAll = false;
            }
        }
        if (!removedAll) continue;
        entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [&](const auto& entry) {
            return entry && entry->captureId == captureId;
        }), entries_.end());
        totalSeconds -= capture->seconds;
    }
}
bool LibraryStore::reject(const Clip& clip) {
    bannedCaptures_.insert(clip.captureId);
    std::ofstream bans(directory_ / "rejected-captures.txt", std::ios::app);
    bans << std::quoted(clip.captureId) << '\n'; bans.close();
    bool good = bool(bans);
    std::vector<std::string> ids{clip.id};
    for (const auto& entry : entries_) if (entry->captureId == clip.captureId && entry->id != clip.id) ids.push_back(entry->id);
    for (const auto& id : ids) {
        std::ofstream tombstone(directory_ / (id + ".rejected"));
        tombstone << "Rejected by listener\n"; tombstone.close();
        good = bool(tombstone) && good;
        std::error_code ec;
        std::filesystem::remove(directory_ / (id + ".wav"), ec); good = !ec && good;
        std::filesystem::remove(directory_ / (id + ".txt"), ec); good = !ec && good;
    }
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [&](const auto& c) { return c->captureId == clip.captureId; }), entries_.end());
    return good;
}
std::vector<ClipPtr> LibraryStore::workingSet(double bpm, const std::vector<ClipPtr>& pinned,
                                             const std::vector<ClipPtr>& previous) {
    std::vector<ClipPtr> result;
    std::unordered_map<std::string, ClipPtr> cached;
    for (const auto& clip : previous) if (!clip->rejected) cached[clip->id] = clip;
    std::unordered_set<std::string> ids, captures;
    for (const auto& clip : pinned) if (!clip->rejected && !bannedCaptures_.count(clip->captureId) && ids.insert(clip->id).second) {
        result.push_back(clip); captures.insert(clip->captureId);
    }
    // A sequence and its transition use at most eight unique sources, below the working-set bound.
    if (result.size() >= maxResidentClips) return result;
    std::vector<size_t> order(entries_.size()); std::iota(order.begin(), order.end(), 0);
    std::shuffle(order.begin(), order.end(), random_);
    // Tempo/key matches first; rotate the rest so a new tempo remains possible after rejection.
    auto rank = [&](const ClipPtr& c) { return bpm > 0 && compatibleTempo(c->bpm, bpm) ? 1 : 0; };
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return rank(entries_[a]) > rank(entries_[b]); });
    for (int tier = 1; tier >= 0 && result.size() < maxResidentClips; --tier)
    for (int pass = 0; pass < 2 && result.size() < maxResidentClips; ++pass) {
        for (size_t index : order) {
            auto& entry = entries_[index];
            if (rank(entry) != tier) continue;
            if (ids.count(entry->id) || (pass == 0 && captures.count(entry->captureId))) continue;
            auto found = cached.find(entry->id);
            auto clip = found == cached.end() ? loadClip(directory_ / (entry->id + ".wav")) : found->second;
            if (!clip || clip->rejected) continue;
            prepareBands(*clip);
            // Cache upgraded key estimates without rewriting a user's old recordings.
            entry->key = clip->key; entry->keyConfidence = clip->keyConfidence;
            result.push_back(clip); ids.insert(clip->id); captures.insert(clip->captureId);
            if (result.size() >= maxResidentClips) break;
        }
    }
    return result;
}
}
