// kv_store_test.cpp – GoogleTest suite for KvStore (Milestone 3)
#include "kv/kv_store.h"

#include <string>
#include <vector>
#include <gtest/gtest.h>

namespace aegis {
namespace {

// ─────────────────────────────────────────────────────────────────────────────
// Test 1 – Put and Get round-trip
// ─────────────────────────────────────────────────────────────────────────────
TEST(KvStoreTest, PutAndGet) {
    KvStore kv;
    KvCommand put{KvCommandType::kPut, 0, 0, "foo", "bar"};
    auto r = kv.apply(put);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.version, 1u);

    KvCommand get{KvCommandType::kGet, 0, 1, "foo", ""};
    auto gr = kv.apply(get);
    EXPECT_TRUE(gr.ok);
    EXPECT_EQ(gr.value, "bar");
    EXPECT_EQ(gr.version, 1u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2 – Delete removes the key; version resets to 0
// ─────────────────────────────────────────────────────────────────────────────
TEST(KvStoreTest, DeleteRemovesKey) {
    KvStore kv;
    kv.apply({KvCommandType::kPut, 0, 0, "k", "v"});

    KvCommand del{KvCommandType::kDelete, 0, 0, "k", ""};
    auto r = kv.apply(del);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(kv.size(), 0u);

    // Get on a deleted key returns ok=false.
    auto gr = kv.apply({KvCommandType::kGet, 0, 0, "k", ""});
    EXPECT_FALSE(gr.ok);
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 3 – Version increments on each Put
// ─────────────────────────────────────────────────────────────────────────────
TEST(KvStoreTest, VersionIncrementsOnPut) {
    KvStore kv;
    for (uint64_t i = 1; i <= 5; ++i) {
        auto r = kv.apply({KvCommandType::kPut, 0, 0, "counter", "x"});
        EXPECT_EQ(r.version, i) << "iteration " << i;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 4 – Session dedup: same (sessionId, seqNo) returns cached result
// ─────────────────────────────────────────────────────────────────────────────
TEST(KvStoreTest, SessionDedupReturnsCache) {
    KvStore kv;
    KvCommand put{KvCommandType::kPut, /*session=*/42, /*seq=*/1, "x", "first"};
    auto r1 = kv.apply(put);
    EXPECT_TRUE(r1.ok);
    EXPECT_EQ(r1.version, 1u);

    // Replay same command – must NOT mutate state.
    auto r2 = kv.apply(put);
    EXPECT_EQ(r2.version, r1.version);
    EXPECT_EQ(kv.version("x"), 1u);  // still version 1
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 5 – KvCommand encode / decode round-trip
// ─────────────────────────────────────────────────────────────────────────────
TEST(KvStoreTest, CommandEncodeDecodeRoundtrip) {
    KvCommand orig{KvCommandType::kPut, 99u, 7u, "mykey", "myvalue"};
    auto bytes = orig.encode();
    ASSERT_FALSE(bytes.empty());

    KvCommand decoded;
    ASSERT_TRUE(KvCommand::decode(bytes.data(), bytes.size(), decoded));
    EXPECT_EQ(decoded.type,      orig.type);
    EXPECT_EQ(decoded.sessionId, orig.sessionId);
    EXPECT_EQ(decoded.seqNo,     orig.seqNo);
    EXPECT_EQ(decoded.key,       orig.key);
    EXPECT_EQ(decoded.value,     orig.value);
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 6 – Snapshot / restore preserves full state
// ─────────────────────────────────────────────────────────────────────────────
TEST(KvStoreTest, SnapshotRestoreRoundtrip) {
    KvStore src;
    src.apply({KvCommandType::kPut, 1u, 1u, "a", "apple"});
    src.apply({KvCommandType::kPut, 1u, 2u, "b", "banana"});
    src.apply({KvCommandType::kPut, 2u, 1u, "c", "cherry"});

    auto snap = src.snapshot();
    ASSERT_FALSE(snap.empty());

    KvStore dst;
    ASSERT_TRUE(dst.restore(snap.data(), snap.size()));

    EXPECT_EQ(dst.size(), 3u);
    EXPECT_EQ(dst.get("a").value_or(""), "apple");
    EXPECT_EQ(dst.get("b").value_or(""), "banana");
    EXPECT_EQ(dst.get("c").value_or(""), "cherry");

    // Session cache must also be restored – replaying a command on dst should
    // return the cached result without mutating the store.
    KvCommand dup{KvCommandType::kPut, 1u, 1u, "a", "CHANGED"};
    auto r = dst.apply(dup);
    EXPECT_EQ(r.version, 1u);             // cached, not bumped
    EXPECT_EQ(dst.get("a").value_or(""), "apple");  // value unchanged
}

}  // namespace
}  // namespace aegis
