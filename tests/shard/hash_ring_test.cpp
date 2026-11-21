// hash_ring_test.cpp – Milestone 9: consistent hashing with virtual nodes
#include "shard/hash_ring.h"

#include <map>
#include <string>
#include <gtest/gtest.h>

namespace aegis {
namespace {

HashRing makeRing(int groups) {
    HashRing r;
    for (int g = 0; g < groups; ++g) r.addGroup(g);
    return r;
}

std::string key(int i) { return "user:" + std::to_string(i); }

TEST(HashRingTest, GroupForIsDeterministic) {
    HashRing a = makeRing(3), b = makeRing(3);
    for (int i = 0; i < 1000; ++i) EXPECT_EQ(a.groupFor(key(i)), b.groupFor(key(i)));
    EXPECT_EQ(a.pointCount(), 3u * HashRing::kDefaultVnodes);
}

TEST(HashRingTest, EmptyRingOwnsNothing) {
    HashRing r;
    EXPECT_EQ(r.groupFor("k"), -1);
}

TEST(HashRingTest, KeysSpreadEvenly) {
    HashRing r = makeRing(3);
    std::map<int, int> count;
    const int kKeys = 30000;
    for (int i = 0; i < kKeys; ++i) ++count[r.groupFor(key(i))];

    ASSERT_EQ(count.size(), 3u);
    for (auto& [g, c] : count) {
        double share = static_cast<double>(c) / kKeys;
        // 128 vnodes/group gives a relative std-dev of ~9% per group, so allow
        // +/-25% (a single point per group would be wildly worse).
        EXPECT_GT(share, (1.0 / 3) * 0.75) << "group " << g;
        EXPECT_LT(share, (1.0 / 3) * 1.25) << "group " << g;
    }
}

TEST(HashRingTest, AddGroupMovesAboutOneOverN) {
    HashRing before = makeRing(3);
    HashRing after  = makeRing(3);
    after.addGroup(3);

    const int kKeys = 40000;
    int moved = 0;
    for (int i = 0; i < kKeys; ++i) {
        int g0 = before.groupFor(key(i)), g1 = after.groupFor(key(i));
        if (g0 != g1) {
            ++moved;
            EXPECT_EQ(g1, 3) << "keys may only move to the new group";
        }
    }
    double frac = static_cast<double>(moved) / kKeys;
    EXPECT_GT(frac, 0.25 * 0.7);
    EXPECT_LT(frac, 0.25 * 1.3);
}

TEST(HashRingTest, RemoveGroupOnlyMovesItsKeys) {
    HashRing full = makeRing(4);
    HashRing less = makeRing(4);
    less.removeGroup(2);

    for (int i = 0; i < 20000; ++i) {
        int g0 = full.groupFor(key(i)), g1 = less.groupFor(key(i));
        if (g0 != 2) EXPECT_EQ(g0, g1) << "key " << i << " must not move";
        else         EXPECT_NE(g1, 2);
    }
}

TEST(HashRingTest, WrapAroundGoesToFirstPoint) {
    HashRing r = makeRing(3);
    // Find a key hashing past the last point on the ring.
    uint64_t maxPoint = 0;
    for (int g = 0; g < 3; ++g)
        for (int v = 0; v < HashRing::kDefaultVnodes; ++v)
            maxPoint = std::max(maxPoint,
                hash64("g" + std::to_string(g) + "#" + std::to_string(v)));

    int found = -1;
    for (int i = 0; i < 2000000 && found < 0; ++i)
        if (hash64(key(i)) > maxPoint) found = i;
    ASSERT_GE(found, 0) << "no key hashed past the last ring point";

    // The first point on the ring owns the wrap-around arc.
    EXPECT_EQ(r.groupFor(key(found)), r.groupForHash(0));
}

TEST(HashRingTest, MovedRangesMatchKeyMovement) {
    HashRing before = makeRing(3);
    HashRing after  = makeRing(3);
    after.addGroup(3);
    auto ranges = HashRing::movedRanges(before, after);
    ASSERT_FALSE(ranges.empty());

    for (const auto& r : ranges) {
        EXPECT_EQ(r.toGroup, 3);
        EXPECT_NE(r.fromGroup, 3);
    }
    for (int i = 0; i < 20000; ++i) {
        uint64_t h = hash64(key(i));
        bool inRange = false;
        for (const auto& r : ranges) inRange = inRange || r.contains(h);
        EXPECT_EQ(inRange, before.groupForHash(h) != after.groupForHash(h)) << key(i);
    }
}

TEST(HashRingTest, ReAddingGroupIsIdempotent) {
    HashRing r = makeRing(3);
    size_t n = r.pointCount();
    r.addGroup(1);
    EXPECT_EQ(r.pointCount(), n);
}

}  // namespace
}  // namespace aegis
