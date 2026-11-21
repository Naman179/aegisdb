#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// hash_ring.h – Consistent hashing with virtual nodes
//
// Each Raft group owns `vnodes` points on a 64-bit ring. A key belongs to the
// group owning the first point clockwise of hash64(key) (wrapping at the end).
// Adding a group only claims the arcs just before its own points, so about
// 1/N of the keys move; vnodes even out the per-group share.
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace aegis {

// 64-bit FNV-1a followed by the splitmix64 finalizer. The finalizer fixes
// FNV's clustering on near-identical keys such as "user:1", "user:2".
uint64_t hash64(std::string_view s);

// A hash arc (lo, hi] that changes owner between two rings. When lo >= hi the
// arc wraps around the end of the ring.
struct MovedRange {
    uint64_t lo{0};
    uint64_t hi{0};
    int      fromGroup{-1};
    int      toGroup{-1};

    bool contains(uint64_t h) const {
        return lo < hi ? (h > lo && h <= hi) : (h > lo || h <= hi);
    }
};

class HashRing {
public:
    static constexpr int kDefaultVnodes = 128;

    void addGroup(int group, int vnodes = kDefaultVnodes);
    void removeGroup(int group);

    // Owner of `key`, or -1 if the ring is empty.
    int groupFor(std::string_view key) const;
    // Owner of an already-hashed position.
    int groupForHash(uint64_t h) const;

    bool   empty() const { return points_.empty(); }
    size_t pointCount() const { return points_.size(); }
    std::vector<int> groups() const;

    // Arcs whose owner differs between `oldRing` and `newRing`.
    static std::vector<MovedRange> movedRanges(const HashRing& oldRing,
                                               const HashRing& newRing);

private:
    struct Point {
        uint64_t hash;
        int      group;
        bool operator<(const Point& o) const {
            return hash != o.hash ? hash < o.hash : group < o.group;
        }
    };
    std::vector<Point> points_;  // sorted by (hash, group)
};

}  // namespace aegis
