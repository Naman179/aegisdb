#include "shard/hash_ring.h"

#include <algorithm>
#include <set>

namespace aegis {

uint64_t hash64(std::string_view s) {
    uint64_t h = 1469598103934665603ULL;  // FNV offset basis
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;            // FNV prime
    }
    // splitmix64 finalizer
    h ^= h >> 30; h *= 0xbf58476d1ce4e5b9ULL;
    h ^= h >> 27; h *= 0x94d049bb133111ebULL;
    return h ^ (h >> 31);
}

void HashRing::addGroup(int group, int vnodes) {
    removeGroup(group);  // idempotent: re-adding replaces the old points
    for (int v = 0; v < vnodes; ++v) {
        points_.push_back(
            Point{hash64("g" + std::to_string(group) + "#" + std::to_string(v)), group});
    }
    std::sort(points_.begin(), points_.end());
}

void HashRing::removeGroup(int group) {
    points_.erase(std::remove_if(points_.begin(), points_.end(),
                                 [group](const Point& p) { return p.group == group; }),
                  points_.end());
}

int HashRing::groupForHash(uint64_t h) const {
    if (points_.empty()) return -1;
    auto it = std::lower_bound(points_.begin(), points_.end(), Point{h, -1});
    return (it == points_.end() ? points_.front() : *it).group;  // wrap around
}

int HashRing::groupFor(std::string_view key) const {
    return groupForHash(hash64(key));
}

std::vector<int> HashRing::groups() const {
    std::set<int> g;
    for (const auto& p : points_) g.insert(p.group);
    return {g.begin(), g.end()};
}

std::vector<MovedRange> HashRing::movedRanges(const HashRing& oldRing,
                                              const HashRing& newRing) {
    std::vector<MovedRange> out;
    if (oldRing.empty() || newRing.empty()) return out;

    // Every arc between consecutive boundary points of the union has a single
    // owner in each ring, so comparing owners at the arc's end is enough.
    std::set<uint64_t> bounds;
    for (const auto& p : oldRing.points_) bounds.insert(p.hash);
    for (const auto& p : newRing.points_) bounds.insert(p.hash);

    uint64_t prev = *bounds.rbegin();  // the first arc wraps from the last point
    for (uint64_t b : bounds) {
        int from = oldRing.groupForHash(b);
        int to   = newRing.groupForHash(b);
        if (from != to) {
            // Merge with the previous moved arc when contiguous and same direction.
            if (!out.empty() && out.back().hi == prev &&
                out.back().fromGroup == from && out.back().toGroup == to) {
                out.back().hi = b;
            } else {
                out.push_back(MovedRange{prev, b, from, to});
            }
        }
        prev = b;
    }
    return out;
}

}  // namespace aegis
