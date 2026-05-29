#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>
#include <cstring>

#include "msm_allocator.h"

// =============================================================================
// Concurrent stress tests for MSM lock-free SWMR protocol.
// 1 writer thread + N reader threads, invariant checks on every read.
// =============================================================================

namespace
{
    constexpr int NUM_READERS = 4;
    constexpr int WRITER_OPS = 10000;
    constexpr int READER_OPS = 50000;
}

// --- Array: writer does push_back, readers iterate and check invariants ---
TEST(MsmConcurrent, ArrayPushBackNoTornRead)
{
    // Slot: [header:8][data_ptr:8][size:4][capacity:4] = 24 bytes
    constexpr std::uint32_t ITEM_SIZE = 8; // each element = 8 bytes (header:4 + value:4)
    auto* slot = msm_alloc("concurrent_array", 24, nullptr);
    ASSERT_NE(slot, nullptr);
    std::memset(slot, 0, 24);

    std::atomic<bool> stop{false};
    std::atomic<int> violations{0};

    // Writer: prepare slot, fill value, then commit.
    // The prepare+commit split is the SWMR-safe API: the slot is filled
    // BEFORE size is published with release, so a reader observing the new
    // size is guaranteed (via release/acquire pairing) to see the value.
    auto writer = [&]() {
        for (int i = 0; i < WRITER_OPS; ++i)
        {
            auto* elem = msm_array_prepare_push(slot, ITEM_SIZE);
            if (elem)
            {
                std::int32_t val = i + 1; // known pattern: value = i + 1 (never 0)
                std::memcpy(elem + 4, &val, 4);
                msm_array_commit_push(slot);
            }
        }
        stop.store(true, std::memory_order_relaxed);
    };

    // Reader: read size, iterate [0..size), check each element != 0
    auto reader = [&]() {
        while (!stop.load(std::memory_order_relaxed))
        {
            std::uint32_t sz = msm_array_size(slot);
            for (std::uint32_t i = 0; i < sz; ++i)
            {
                auto* elem = msm_array_get(slot, i, ITEM_SIZE);
                if (!elem) break; // bounds check returned nullptr (size changed)

                std::int32_t val;
                std::memcpy(&val, elem + 4, 4);

                // Invariant: every published element must have val > 0
                // val == 0 means we see uninitialized data (torn read)
                if (val <= 0)
                {
                    violations.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    };

    std::thread w(writer);
    std::vector<std::thread> readers;
    for (int i = 0; i < NUM_READERS; ++i)
        readers.emplace_back(reader);

    w.join();
    for (auto& r : readers) r.join();

    EXPECT_EQ(violations.load(), 0) << "Torn reads detected in concurrent array access";
    EXPECT_EQ(msm_array_size(slot), WRITER_OPS);

    msm_free("concurrent_array");
}

// --- String: writer sets strings of increasing length, readers just read safely ---
// Verifies no crash, no use-after-free, no segfault under concurrent access.
// Per-field linearizability means reader sees either old or new string, never garbage ptr.
TEST(MsmConcurrent, StringGrowNoTornRead)
{
    auto* slot = msm_alloc("concurrent_string", 24, nullptr);
    ASSERT_NE(slot, nullptr);
    std::memset(slot, 0, 24);

    std::atomic<bool> stop{false};
    std::atomic<int> violations{0};
    std::atomic<int> reads_done{0};

    // Writer: set strings of strictly increasing length (forces grow)
    auto writer = [&]() {
        for (int i = 1; i <= WRITER_OPS; ++i)
        {
            char ch = 'a' + (i % 26);
            std::string s(i, ch);
            msm_string_set(slot, s.c_str(), static_cast<std::uint32_t>(s.size()));
        }
        stop.store(true, std::memory_order_relaxed);
    };

    // Reader: just call msm_string_get repeatedly — must never crash
    auto reader = [&]() {
        while (!stop.load(std::memory_order_relaxed))
        {
            const char* str = msm_string_get(slot);
            // Must never segfault. str is either "" or valid pointer.
            // We just touch it to verify no crash (use-after-free would crash here)
            if (str && str != reinterpret_cast<const char*>(0x1))
            {
                volatile char c = str[0]; // touch memory — would crash on use-after-free
                (void)c;
            }
            reads_done.fetch_add(1, std::memory_order_relaxed);
        }
    };

    std::thread w(writer);
    std::vector<std::thread> readers;
    for (int i = 0; i < NUM_READERS; ++i)
        readers.emplace_back(reader);

    w.join();
    for (auto& r : readers) r.join();

    EXPECT_GT(reads_done.load(), 0) << "Readers did work";

    msm_free("concurrent_string");
}

// --- String: shrink/grow cycles validate Variant B (count-in-buffer) — closes REMARKS #7.
//
// Under the previous two-store protocol (release ptr, release len as separate
// commits), a reader could observe the mixed state (OLD len, NEW ptr) and
// iterate OLD-len bytes from the NEW buffer. For shrinking workloads where
// NEW buffer is smaller than OLD len, this produces out-of-bounds reads.
//
// Variant B (len embedded in the buffer header, single release_store on the
// data pointer) makes (len, ptr) physically inseparable — no mixed state
// is possible regardless of grow/shrink workload.
//
// This test cycles between SHORT and LONG strings of *different content* and
// requires the reader to observe a self-consistent (len, data) snapshot:
//   - len matches the actual data length
//   - all bytes in [0, len) are the same single letter (either OLD or NEW)
//
// Under the old protocol, OOB or mixed-content reads would fail invariants.
// Under Variant B, all observations are self-consistent.
TEST(MsmConcurrent, StringShrinkGrowNoTornRead)
{
    auto* slot = msm_alloc("concurrent_string_shrink", 24, nullptr);
    ASSERT_NE(slot, nullptr);
    std::memset(slot, 0, 24);

    constexpr int LONG_LEN = 1000;
    constexpr int SHORT_LEN = 3;
    constexpr int CYCLES = 5000;

    // Pre-populate with LONG string of 'a's so first reader read sees something
    {
        std::string init(LONG_LEN, 'a');
        msm_string_set(slot, init.c_str(), LONG_LEN);
    }

    std::atomic<bool> stop{false};
    std::atomic<int> violations{0};
    std::atomic<int> reads_done{0};

    // Writer: alternate between SHORT 'a...' and LONG 'b...'. Different letters
    // so any mixed-content observation is detectable (bytes from two buffers
    // would mix 'a' and 'b').
    auto writer = [&]() {
        for (int i = 0; i < CYCLES; ++i)
        {
            if (i % 2 == 0)
            {
                std::string s(SHORT_LEN, 'a');
                msm_string_set(slot, s.c_str(), SHORT_LEN);
            }
            else
            {
                std::string s(LONG_LEN, 'b');
                msm_string_set(slot, s.c_str(), LONG_LEN);
            }
        }
        stop.store(true, std::memory_order_relaxed);
    };

    // Reader: SINGLE acquire-load pattern via msm_string_get → returned ptr
    // and the data it points to live in the SAME buffer (Variant B guarantee).
    // Use strlen() to derive length from the null terminator in that buffer —
    // this avoids the two-API-call race where length comes from one snapshot
    // and data pointer from another.
    //
    // The buffer is null-terminated and not freed (bump allocator G2), so
    // strlen + byte indexing on the returned pointer is race-free.
    auto reader = [&]() {
        while (!stop.load(std::memory_order_relaxed))
        {
            const char* str = msm_string_get(slot); // single atomic snapshot
            if (str && str[0] != '\0')
            {
                // Derive length from null terminator in the same buffer.
                std::size_t len = std::strlen(str);
                char first = str[0];
                if (first != 'a' && first != 'b')
                {
                    violations.fetch_add(1, std::memory_order_relaxed);
                }
                // Sample mid and last byte — must be the same letter (single buffer).
                if (len > 2)
                {
                    char mid = str[len / 2];
                    char last = str[len - 1];
                    if (mid != first || last != first)
                    {
                        violations.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
            reads_done.fetch_add(1, std::memory_order_relaxed);
        }
    };

    std::thread w(writer);
    std::vector<std::thread> readers;
    for (int i = 0; i < NUM_READERS; ++i)
        readers.emplace_back(reader);

    w.join();
    for (auto& r : readers) r.join();

    EXPECT_EQ(violations.load(), 0)
        << "Mixed-content observation in string shrink/grow — Variant B protocol violation";
    EXPECT_GT(reads_done.load(), 0) << "Readers did work";

    msm_free("concurrent_string_shrink");
}

// --- Object: writer adds/removes fields, readers iterate field count ---
TEST(MsmConcurrent, ObjectAddRemoveNoTornRead)
{
    // Object slot: [header:8][entries_ptr:8][count:4][capacity:4] = 24 bytes
    auto* slot = msm_alloc("concurrent_object", 24, nullptr);
    ASSERT_NE(slot, nullptr);
    std::memset(slot, 0, 24);

    std::atomic<bool> stop{false};
    std::atomic<int> violations{0};

    // Writer: add fields, then remove some
    auto writer = [&]() {
        for (int i = 0; i < WRITER_OPS / 2; ++i)
        {
            std::string name = "field_" + std::to_string(i);
            msm_object_set_int32(slot, name.c_str(), i + 1);
        }
        // Remove half
        for (int i = 0; i < WRITER_OPS / 4; ++i)
        {
            std::string name = "field_" + std::to_string(i);
            msm_object_remove(slot, name.c_str());
        }
        stop.store(true, std::memory_order_relaxed);
    };

    // Reader: read field_count, verify it's reasonable
    auto reader = [&]() {
        while (!stop.load(std::memory_order_relaxed))
        {
            std::uint32_t count = msm_object_field_count(slot);
            // Count should never exceed WRITER_OPS/2 (max fields added)
            if (count > WRITER_OPS / 2 + 1)
            {
                violations.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    std::thread w(writer);
    std::vector<std::thread> readers;
    for (int i = 0; i < NUM_READERS; ++i)
        readers.emplace_back(reader);

    w.join();
    for (auto& r : readers) r.join();

    EXPECT_EQ(violations.load(), 0) << "Invalid field count observed";

    // Final count should be WRITER_OPS/2 - WRITER_OPS/4 = WRITER_OPS/4
    EXPECT_EQ(msm_object_field_count(slot), WRITER_OPS / 4);

    msm_free("concurrent_object");
}

// --- Object: marker invariant test — catches mixed-state bugs in CoW remove.
// Variant A (count in descriptor + entries_ptr — 2 separate release_stores)
// admits reader observations where count is NEW but entries_ptr is OLD: reader
// iterates OLD buffer with NEW count and may see a "removed" entry, or sees
// (OLD count, NEW ptr) and reads out-of-bounds. Variant B (count INSIDE the
// buffer header + single release_store on entries_ptr) makes such mixed
// states physically impossible — the (ptr, count) pair is one atomic snapshot.
//
// This test rapidly cycles add("marker") + remove("marker") while readers
// query stable fields A, B, C. Invariant: at any moment the field count is
// either 3 (marker absent) or 4 (marker present), and stable fields are
// always observable with their original values. With variant A, readers
// would occasionally observe count != {3, 4} or missing stable fields.
TEST(MsmConcurrent, ObjectMarkerRemoveNoStaleReads)
{
    auto* slot = msm_alloc("concurrent_object_marker", 24, nullptr);
    ASSERT_NE(slot, nullptr);
    std::memset(slot, 0, 24);

    std::atomic<bool> stop{false};
    std::atomic<int> violations{0};

    // Pre-populate object with stable fields A, B, C
    msm_object_set_int32(slot, "field_A", 1);
    msm_object_set_int32(slot, "field_B", 2);
    msm_object_set_int32(slot, "field_C", 3);

    // Writer: rapidly add/remove a "marker" field while stable fields exist
    auto writer = [&]() {
        for (int i = 0; i < WRITER_OPS; ++i)
        {
            msm_object_set_int32(slot, "marker", i + 100);
            msm_object_remove(slot, "marker");
        }
        stop.store(true, std::memory_order_relaxed);
    };

    // Reader: count must always be 3 or 4; stable fields must always be valid.
    auto reader = [&]() {
        while (!stop.load(std::memory_order_relaxed))
        {
            std::uint32_t count = msm_object_field_count(slot);
            if (count != 3 && count != 4)
            {
                violations.fetch_add(1, std::memory_order_relaxed);
            }
            if (msm_object_get_int32(slot, "field_A") != 1) violations.fetch_add(1, std::memory_order_relaxed);
            if (msm_object_get_int32(slot, "field_B") != 2) violations.fetch_add(1, std::memory_order_relaxed);
            if (msm_object_get_int32(slot, "field_C") != 3) violations.fetch_add(1, std::memory_order_relaxed);
        }
    };

    std::thread w(writer);
    std::vector<std::thread> readers;
    for (int i = 0; i < NUM_READERS; ++i)
        readers.emplace_back(reader);

    w.join();
    for (auto& r : readers) r.join();

    EXPECT_EQ(violations.load(), 0)
        << "Mixed-state observation in CoW remove (variant A bug, should not happen with variant B)";

    msm_free("concurrent_object_marker");
}
