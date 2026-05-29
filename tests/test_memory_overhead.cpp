#include <gtest/gtest.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <sys/resource.h>
#include <thread>
#include <unistd.h>

#include "msm_allocator.h"

// Memory overhead tests measure RSS deltas around controlled workloads.
// Under sanitizers (TSAN especially) RSS includes shadow memory and other
// instrumentation overhead, which dwarfs the actual allocation footprint
// and makes the bounds asserted below meaningless. Skip these tests when
// any sanitizer is detected.
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__) || defined(__has_feature)
#  if defined(__has_feature)
#    if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer) || __has_feature(memory_sanitizer)
#      define MSM_UNDER_SANITIZER 1
#    endif
#  endif
#  if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
#    define MSM_UNDER_SANITIZER 1
#  endif
#endif

#ifndef MSM_UNDER_SANITIZER
#  define MSM_UNDER_SANITIZER 0
#endif

#define MSM_SKIP_UNDER_SANITIZER() \
    do { if (MSM_UNDER_SANITIZER) GTEST_SKIP() << "skipped under sanitizer (RSS dominated by shadow memory)"; } while (0)

// =============================================================================
// Memory overhead measurement (PLAN.md §3.7.3).
//
// Purpose: quantify the bump-allocator "never free during segment lifetime"
// policy in concrete numbers, and assert known bounds to detect regressions.
//
// Methodology:
//   1. Measure baseline RSS (resident set size) before workload
//   2. Run workload (controlled mix of ops)
//   3. Measure RSS after workload
//   4. Compute overhead vs theoretical minimum (sum of live data sizes)
//   5. Assert overhead <= documented bound for each pattern
//
// Patterns measured:
//   A. Append-only grow (array push_back to steady-state cap)
//       Expected: overhead < 2× of peak live data (geometric series)
//   B. String set with monotonically increasing length (always CoW)
//       Expected: overhead grows linearly with operations (Variant B cost)
//   C. Object add-then-remove batch (the adversarial CoW pattern)
//       Expected: O(N²) memory growth — bound is workload-specific
//
// Numbers cited in articles/msm-concurrency/MEMORY-RECLAMATION.md and
// articles/msm-concurrency/valgrind-leak.log. This test asserts a regression
// bound so future protocol changes that worsen memory overhead are caught.
// =============================================================================

namespace
{
    // Returns process RSS in bytes via /proc/self/statm. Linux-only.
    std::size_t current_rss_bytes()
    {
        FILE* f = std::fopen("/proc/self/statm", "r");
        if (!f) return 0;
        long size_pages = 0, resident_pages = 0;
        int matched = std::fscanf(f, "%ld %ld", &size_pages, &resident_pages);
        std::fclose(f);
        if (matched != 2) return 0;
        return static_cast<std::size_t>(resident_pages) * static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    }
}

// --- Pattern A: append-only grow ---------------------------------------------
// Array of int32 grown to 100K elements. Doubling capacity gives
// 1 + 2 + 4 + ... + 65536 = 131K leaked element-slots before final 100K.
// Theoretical max live = 100K * 8 bytes = 800 KB.
// Leak from grow series: sum(2^i * 8) for i=0..16 ~= 1 MB.
// Empirical (measured): ~2.8 MB RSS delta — includes malloc metadata, page
// alignment, and minor runtime overhead.
// Assert bound: < 5 MB — regression protection (2x of empirical baseline).
TEST(MsmMemoryOverhead, AppendOnlyGrow_Under_5MB)
{
    constexpr std::uint32_t N = 100000;
    constexpr std::uint32_t ITEM_SIZE = 8;
    constexpr std::size_t LIVE_BYTES = N * ITEM_SIZE; // 800 KB
    constexpr std::size_t MAX_OVERHEAD = 5 * 1024 * 1024; // 5 MB

    MSM_SKIP_UNDER_SANITIZER();
    std::size_t rss_before = current_rss_bytes();

    auto* slot = msm_alloc("mem_append", 24, nullptr);
    ASSERT_NE(slot, nullptr);
    std::memset(slot, 0, 24);

    for (std::uint32_t i = 0; i < N; ++i)
    {
        auto* elem = msm_array_prepare_push(slot, ITEM_SIZE);
        ASSERT_NE(elem, nullptr);
        std::int32_t v = static_cast<std::int32_t>(i);
        std::memcpy(elem + 4, &v, 4);
        msm_array_commit_push(slot);
    }

    std::size_t rss_after = current_rss_bytes();
    std::size_t delta = (rss_after > rss_before) ? (rss_after - rss_before) : 0;

    std::printf("[MemOverhead.AppendOnlyGrow] live=%zu B, RSS-delta=%zu B, overhead=%.2fx\n",
                LIVE_BYTES, delta, static_cast<double>(delta) / LIVE_BYTES);

    EXPECT_LE(delta, MAX_OVERHEAD)
        << "Append-only grow exceeded 5 MB overhead (regression suspected — empirical baseline ~2.8 MB)";
    EXPECT_EQ(msm_array_size(slot), N);

    msm_free("mem_append");
}

// --- Pattern B: string always-CoW under increasing length --------------------
// Each set allocates a new buffer (Variant B always CoW). For lengths
// 1, 2, ..., N the total bytes allocated = N*(N+1)/2 + N*header.
// For N=1000: ~500 KB + headers ~= 600 KB.
// We assert < 5 MB overhead to allow for slack.
TEST(MsmMemoryOverhead, StringGrowAlwaysCoW_Under_5MB)
{
    constexpr int N = 1000;
    constexpr std::size_t MAX_OVERHEAD = 5 * 1024 * 1024;

    MSM_SKIP_UNDER_SANITIZER();
    std::size_t rss_before = current_rss_bytes();

    auto* slot = msm_alloc("mem_string", 24, nullptr);
    ASSERT_NE(slot, nullptr);
    std::memset(slot, 0, 24);

    for (int i = 1; i <= N; ++i)
    {
        std::string s(static_cast<std::size_t>(i), 'a');
        msm_string_set(slot, s.c_str(), static_cast<std::uint32_t>(s.size()));
    }

    std::size_t rss_after = current_rss_bytes();
    std::size_t delta = (rss_after > rss_before) ? (rss_after - rss_before) : 0;

    // Theoretical minimum = current string buffer = N + header.
    std::size_t live_bytes = static_cast<std::size_t>(N) + 8;
    double theoretical_total = static_cast<double>(N) * (N + 1) / 2 + N * 8;

    std::printf("[MemOverhead.StringGrowAlwaysCoW] N=%d, live=%zu B, theoretical-total=%.0f B, RSS-delta=%zu B\n",
                N, live_bytes, theoretical_total, delta);

    EXPECT_LE(delta, MAX_OVERHEAD)
        << "String CoW grow exceeded 5 MB overhead (regression suspected)";

    msm_free("mem_string");
}

// --- Pattern C: object add-then-remove (adversarial CoW) ---------------------
// This is the documented worst-case for bump-allocator. We measure but don't
// assert a tight bound — instead assert a SOFT bound (it's the adversarial
// pattern, so we want to keep the existing growth profile but flag regressions
// that 10x worse than current).
//
// Workload: add 1000 fields then remove first 500. Each remove triggers CoW
// of remaining (1000-i) entries. Expected total leak ~= 5 MB for these sizes.
// Assert: <= 50 MB (generous bound, 10x current).
TEST(MsmMemoryOverhead, ObjectAddRemove_Under_50MB)
{
    constexpr int ADDS = 1000;
    constexpr int REMOVES = 500;
    constexpr std::size_t MAX_OVERHEAD = 50 * 1024 * 1024;

    MSM_SKIP_UNDER_SANITIZER();
    std::size_t rss_before = current_rss_bytes();

    auto* slot = msm_alloc("mem_object", 24, nullptr);
    ASSERT_NE(slot, nullptr);
    std::memset(slot, 0, 24);

    for (int i = 0; i < ADDS; ++i)
    {
        std::string name = "field_" + std::to_string(i);
        msm_object_set_int32(slot, name.c_str(), i + 1);
    }

    EXPECT_EQ(msm_object_field_count(slot), static_cast<std::uint32_t>(ADDS));

    for (int i = 0; i < REMOVES; ++i)
    {
        std::string name = "field_" + std::to_string(i);
        msm_object_remove(slot, name.c_str());
    }

    std::size_t rss_after = current_rss_bytes();
    std::size_t delta = (rss_after > rss_before) ? (rss_after - rss_before) : 0;

    std::printf("[MemOverhead.ObjectAddRemove] adds=%d, removes=%d, RSS-delta=%zu B (%.2f MB)\n",
                ADDS, REMOVES, delta, delta / (1024.0 * 1024.0));

    EXPECT_LE(delta, MAX_OVERHEAD)
        << "Object add+remove exceeded 50 MB overhead — 10x regression vs documented baseline";
    EXPECT_EQ(msm_object_field_count(slot), static_cast<std::uint32_t>(ADDS - REMOVES));

    msm_free("mem_object");
}

// --- Pattern D: free-and-rebuild releases ALL memory ------------------------
// Verifies G2 semantic: msm_free actually releases everything (no permanent
// leaks beyond segment lifetime). RSS after free should not be larger than
// before.
TEST(MsmMemoryOverhead, FreeReleasesAllMemory)
{
    MSM_SKIP_UNDER_SANITIZER();
    std::size_t rss_baseline = current_rss_bytes();

    for (int iter = 0; iter < 5; ++iter)
    {
        auto* slot = msm_alloc("mem_cycle", 24, nullptr);
        ASSERT_NE(slot, nullptr);
        std::memset(slot, 0, 24);

        // Grow array to non-trivial size
        for (int i = 0; i < 10000; ++i)
        {
            auto* e = msm_array_prepare_push(slot, 8);
            std::int32_t v = i;
            std::memcpy(e + 4, &v, 4);
            msm_array_commit_push(slot);
        }

        msm_free("mem_cycle");
    }

    std::size_t rss_after = current_rss_bytes();
    long signed_delta = static_cast<long>(rss_after) - static_cast<long>(rss_baseline);

    std::printf("[MemOverhead.FreeReleasesAllMemory] baseline=%zu B, after-5-cycles=%zu B, delta=%+ld B\n",
                rss_baseline, rss_after, signed_delta);

    // After 5 alloc/free cycles, RSS should not grow significantly.
    // Allow for some allocator fragmentation / runtime overhead but flag
    // anything > 5 MB as suspicious (would suggest a real leak).
    constexpr long MAX_PERSISTENT_GROWTH = 5 * 1024 * 1024;
    EXPECT_LE(signed_delta, MAX_PERSISTENT_GROWTH)
        << "Persistent memory growth after free cycles — possible leak in msm_free";
}
