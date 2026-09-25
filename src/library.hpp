#pragma once
#include "music.hpp"
#include <unordered_set>

namespace music {
// Owned by the analysis/storage worker. The catalogue contains metadata only.
class LibraryStore {
public:
    explicit LibraryStore(std::filesystem::path directory) : directory_(std::move(directory)) {}
    bool scan();
    size_t size() const { return entries_.size(); }
    bool add(const Clip& clip);
    bool reject(const Clip& clip);
    std::vector<ClipPtr> workingSet(double bpm, const std::vector<ClipPtr>& pinned,
                                    const std::vector<ClipPtr>& previous);
private:
    std::filesystem::path directory_;
    std::vector<ClipPtr> entries_;
    std::unordered_set<std::string> bannedCaptures_;
    std::mt19937 random_{std::random_device{}()};
};
}
