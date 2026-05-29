#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>

#include "msm_allocator.h"

// =============================================================================
// Schema hash — contract C2 enforcement test (silent cross-language corruption
// guard). See PLAN.md §2.1.A C2 and §3.5.5.
//
// Goal: verify that msm_alloc_with_schema / msm_get_with_schema enforce
// byte-identical layout agreement between writer and reader, aborting on
// mismatch rather than silently corrupting data.
//
// The actual abort path is tested via EXPECT_DEATH (Google Test). Other tests
// verify the happy path and inspection API.
// =============================================================================

TEST(MsmSchemaHash, Fnv1aIsDeterministic)
{
    std::uint64_t h1 = msm_schema_hash_fnv1a("Graph{nodes:int32, edges:array<Edge>}");
    std::uint64_t h2 = msm_schema_hash_fnv1a("Graph{nodes:int32, edges:array<Edge>}");
    EXPECT_EQ(h1, h2);
    EXPECT_NE(h1, 0u) << "FNV-1a should never return 0 for non-empty input";
}

TEST(MsmSchemaHash, Fnv1aIsSensitiveToContent)
{
    std::uint64_t h1 = msm_schema_hash_fnv1a("schema_v1");
    std::uint64_t h2 = msm_schema_hash_fnv1a("schema_v2");
    EXPECT_NE(h1, h2);
}

TEST(MsmSchemaHash, Fnv1aEmptyAndNull)
{
    EXPECT_EQ(msm_schema_hash_fnv1a(nullptr), 0u);
    // FNV-1a of empty string is the offset basis, never 0.
    EXPECT_EQ(msm_schema_hash_fnv1a(""), 14695981039346656037ULL);
}

TEST(MsmSchemaHash, AllocWithSchemaAndGetMatching)
{
    std::uint64_t h = msm_schema_hash_fnv1a("test_schema_v1");
    auto* p1 = msm_alloc_with_schema("schema_test_match", 64, h, nullptr);
    ASSERT_NE(p1, nullptr);

    auto* p2 = msm_get_with_schema("schema_test_match", h);
    EXPECT_EQ(p1, p2) << "Same name + matching hash should return the same pointer";

    EXPECT_EQ(msm_get_schema_hash("schema_test_match"), h);

    msm_free("schema_test_match");
}

TEST(MsmSchemaHash, GetSchemaHashUnknownReturnsZero)
{
    EXPECT_EQ(msm_get_schema_hash("nonexistent_segment_xyz"), 0u);
}

TEST(MsmSchemaHash, LegacyAllocHasZeroSchema)
{
    auto* p = msm_alloc("schema_test_legacy", 32, nullptr);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(msm_get_schema_hash("schema_test_legacy"), 0u);

    // get_with_schema with hash=0 succeeds (legacy compat)
    EXPECT_EQ(msm_get_with_schema("schema_test_legacy", 0), p);

    msm_free("schema_test_legacy");
}

TEST(MsmSchemaHash, GetWithSchemaNotFoundReturnsNull)
{
    EXPECT_EQ(msm_get_with_schema("nonexistent_segment_xyz", 0x12345678), nullptr);
}

// Death test: mismatch must abort with diagnostic. Only enabled when
// GTEST_HAS_DEATH_TEST is supported (Linux/macOS). On Windows this is skipped.
#if GTEST_HAS_DEATH_TEST
TEST(MsmSchemaHashDeathTest, MismatchAborts)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");

    std::uint64_t h_correct = msm_schema_hash_fnv1a("schema_A");
    auto* p = msm_alloc_with_schema("schema_test_mismatch", 32, h_correct, nullptr);
    ASSERT_NE(p, nullptr);

    std::uint64_t h_wrong = msm_schema_hash_fnv1a("schema_B");
    EXPECT_DEATH(
        msm_get_with_schema("schema_test_mismatch", h_wrong),
        "schema hash mismatch"
    );

    // Cleanup happens implicitly when EXPECT_DEATH's forked process exits.
    // Parent process still has the registry entry — free it.
    msm_free("schema_test_mismatch");
}
#endif
