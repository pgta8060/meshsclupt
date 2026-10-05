// SculptCore — O(1)-reset "visited" marker used to de-duplicate indices.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace sculpt {

// Marks indices as visited without clearing an array per query: each pass
// bumps a generation counter instead. Not thread-safe; one per worker.
class VisitSet {
public:
    // Starts a new pass over a domain of `size` indices.
    void begin(std::size_t size) {
        if (stamps_.size() < size) stamps_.resize(size, 0u);
        if (++generation_ == 0u) {  // Wrapped: stale stamps could alias, so reset.
            std::fill(stamps_.begin(), stamps_.end(), 0u);
            generation_ = 1u;
        }
    }

    // Returns true the first time `i` is seen in the current pass.
    bool visit(std::uint32_t i) {
        if (stamps_[i] == generation_) return false;
        stamps_[i] = generation_;
        return true;
    }

    bool visited(std::uint32_t i) const { return stamps_[i] == generation_; }

    void release() {
        std::vector<std::uint32_t>().swap(stamps_);
        generation_ = 0u;
    }

private:
    std::vector<std::uint32_t> stamps_;
    std::uint32_t generation_ = 0u;
};

}  // namespace sculpt
