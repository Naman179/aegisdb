// wal_test.cpp – GoogleTest suite for the AegisDB Write-Ahead Log (Milestone 2)
#include "wal/wal.h"

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace aegis {
namespace {

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

// Returns a unique temp directory path under /tmp for the given test name.
// The directory is created in WalTest::SetUp and removed in TearDown.
class WalTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Use a unique path per test so parallel runs don't collide.
        dir_ = "/tmp/wal_test_" + std::to_string(::testing::UnitTest::GetInstance()
                                                       ->current_test_info()
                                                       ->line());
        // Remove leftovers from a previous failed run.
        std::filesystem::remove_all(dir_);
    }

    void TearDown() override {
        std::filesystem::remove_all(dir_);
    }

    std::string dir_;
};

// Convenience: collect all records from a freshly-opened WalReader.
static std::vector<WalRecord> replayAll(const std::string& dir) {
    auto reader = WalReader::open(dir);
    if (!reader) return {};
    std::vector<WalRecord> recs;
    WalRecord rec;
    while (reader->next(&rec)) recs.push_back(std::move(rec));
    return recs;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 1 – Append and replay a single record
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(WalTest, AppendAndReplayOneRecord) {
    {
        auto w = WalWriter::open(dir_);
        ASSERT_NE(w, nullptr);
        ASSERT_TRUE(w->append(WalRecordType::kData, "hello"));
        ASSERT_TRUE(w->sync());
    }

    auto recs = replayAll(dir_);
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_EQ(recs[0].type, WalRecordType::kData);
    EXPECT_EQ(std::string(recs[0].data.begin(), recs[0].data.end()), "hello");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2 – Append multiple records; verify order
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(WalTest, AppendMultipleRecordsInOrder) {
    const std::vector<std::string> payloads = {"alpha", "beta", "gamma",
                                                "delta", "epsilon"};
    {
        auto w = WalWriter::open(dir_);
        ASSERT_NE(w, nullptr);
        for (auto& p : payloads) {
            ASSERT_TRUE(w->append(WalRecordType::kData, p));
        }
        ASSERT_TRUE(w->sync());
    }

    auto recs = replayAll(dir_);
    ASSERT_EQ(recs.size(), payloads.size());
    for (size_t i = 0; i < payloads.size(); ++i) {
        EXPECT_EQ(std::string(recs[i].data.begin(), recs[i].data.end()),
                  payloads[i])
            << "record " << i;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 3 – Segment rollover: writing past maxSegmentBytes creates a 2nd file
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(WalTest, SegmentRollover) {
    // Tiny max segment size to force rollover after the first few records.
    const uint64_t kTinyMax = 64;  // 64 bytes
    const size_t   kRecords = 10;

    {
        auto w = WalWriter::open(dir_, kTinyMax);
        ASSERT_NE(w, nullptr);
        for (size_t i = 0; i < kRecords; ++i) {
            ASSERT_TRUE(w->append(WalRecordType::kData, "roll"));
        }
        ASSERT_TRUE(w->sync());
        // After rollover at least one more segment must exist.
        EXPECT_GT(w->currentSegmentId(), 0u);
    }

    // Replay must recover all records across all segments.
    auto recs = replayAll(dir_);
    ASSERT_EQ(recs.size(), kRecords);
    for (auto& r : recs) {
        EXPECT_EQ(std::string(r.data.begin(), r.data.end()), "roll");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 4 – CRC mismatch in a record terminates replay (torn write simulation)
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(WalTest, CrcMismatchStopsReplay) {
    // Write two valid records.
    {
        auto w = WalWriter::open(dir_);
        ASSERT_NE(w, nullptr);
        ASSERT_TRUE(w->append(WalRecordType::kData, "good1"));
        ASSERT_TRUE(w->append(WalRecordType::kData, "good2"));
        ASSERT_TRUE(w->sync());
    }

    // Corrupt the first byte of the second record's CRC in the segment file.
    // The segment is named "wal-0000000000000000.seg".
    std::string segPath = dir_ + "/wal-0000000000000000.seg";
    {
        // A record is: crc(4) + type(1) + len(4) + "good1"(5) = 14 bytes.
        // Second record starts at byte 14.  Corrupt byte 14 (first CRC byte).
        FILE* f = fopen(segPath.c_str(), "r+b");
        ASSERT_NE(f, nullptr);
        fseek(f, 14, SEEK_SET);
        uint8_t bad = 0xDE;
        fwrite(&bad, 1, 1, f);
        fclose(f);
    }

    auto reader = WalReader::open(dir_);
    ASSERT_NE(reader, nullptr);

    WalRecord rec;
    // First record is intact.
    ASSERT_TRUE(reader->next(&rec));
    EXPECT_EQ(std::string(rec.data.begin(), rec.data.end()), "good1");
    // Second record has bad CRC – reader should stop.
    EXPECT_FALSE(reader->next(&rec));
    EXPECT_FALSE(reader->ok());
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 5 – Replay on an empty WAL directory returns nothing
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(WalTest, EmptyWalReplaysNothing) {
    // Create an empty WAL (open + close without writing).
    { auto w = WalWriter::open(dir_); ASSERT_NE(w, nullptr); }

    auto recs = replayAll(dir_);
    EXPECT_TRUE(recs.empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 6 – Reopen writer and append more; replay delivers all records
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(WalTest, AppendAfterReopen) {
    {
        auto w = WalWriter::open(dir_);
        ASSERT_NE(w, nullptr);
        ASSERT_TRUE(w->append(WalRecordType::kData, "first"));
        ASSERT_TRUE(w->sync());
    }
    {
        // Reopen: writer must continue in a NEW segment (id 1).
        auto w = WalWriter::open(dir_);
        ASSERT_NE(w, nullptr);
        ASSERT_TRUE(w->append(WalRecordType::kData, "second"));
        ASSERT_TRUE(w->sync());
    }

    auto recs = replayAll(dir_);
    ASSERT_EQ(recs.size(), 2u);
    EXPECT_EQ(std::string(recs[0].data.begin(), recs[0].data.end()), "first");
    EXPECT_EQ(std::string(recs[1].data.begin(), recs[1].data.end()), "second");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 7 – Record types are preserved on round-trip
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(WalTest, RecordTypesRoundtrip) {
    {
        auto w = WalWriter::open(dir_);
        ASSERT_NE(w, nullptr);
        ASSERT_TRUE(w->append(WalRecordType::kData,       "d"));
        ASSERT_TRUE(w->append(WalRecordType::kCheckpoint, "c"));
        ASSERT_TRUE(w->append(WalRecordType::kBarrier,    "b"));
        ASSERT_TRUE(w->sync());
    }

    auto recs = replayAll(dir_);
    ASSERT_EQ(recs.size(), 3u);
    EXPECT_EQ(recs[0].type, WalRecordType::kData);
    EXPECT_EQ(recs[1].type, WalRecordType::kCheckpoint);
    EXPECT_EQ(recs[2].type, WalRecordType::kBarrier);
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 8 – Zero-length payload record survives round-trip
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(WalTest, ZeroLengthPayloadRoundtrip) {
    {
        auto w = WalWriter::open(dir_);
        ASSERT_NE(w, nullptr);
        ASSERT_TRUE(w->append(WalRecordType::kBarrier,
                               static_cast<const uint8_t*>(nullptr), 0));
        ASSERT_TRUE(w->sync());
    }

    auto recs = replayAll(dir_);
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_EQ(recs[0].type, WalRecordType::kBarrier);
    EXPECT_TRUE(recs[0].data.empty());
}

}  // namespace
}  // namespace aegis
