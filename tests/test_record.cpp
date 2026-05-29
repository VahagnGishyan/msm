#include <gtest/gtest.h>
#include "types.hpp"
#include "member.hpp"
#include "record.hpp"
#include "segment.hpp"
#include <cstring>

using Player = msm::record<
    msm::member<"hp",    msm::int32>,
    msm::member<"pos_x", msm::float64>,
    msm::member<"alive", msm::boolean>
>;

TEST(MsmRecord, SlotSize)
{
    // header(8) + int32(8) + float64(16) + boolean(8) = 40
    EXPECT_EQ(Player::slot_size, 40);
}

TEST(MsmRecord, WriteAndReadFields)
{
    std::int8_t buf[40] = {};
    Player player{ buf };

    player.get<"hp">() = 100;
    player.get<"pos_x">() = 3.14;
    player.get<"alive">() = true;

    EXPECT_EQ(static_cast<std::int32_t>(player.get<"hp">()), 100);
    EXPECT_DOUBLE_EQ(static_cast<double>(player.get<"pos_x">()), 3.14);
    EXPECT_TRUE(static_cast<bool>(player.get<"alive">()));
}

TEST(MsmRecord, FieldsDoNotOverlap)
{
    std::int8_t buf[40] = {};
    Player player{ buf };

    player.get<"hp">() = 999;
    player.get<"pos_x">() = 1.5;
    player.get<"alive">() = false;

    // Verify each field still holds its value
    EXPECT_EQ(static_cast<std::int32_t>(player.get<"hp">()), 999);
    EXPECT_DOUBLE_EQ(static_cast<double>(player.get<"pos_x">()), 1.5);
    EXPECT_FALSE(static_cast<bool>(player.get<"alive">()));
}

using Nested = msm::record<
    msm::member<"player", Player>,
    msm::member<"score",  msm::int32>
>;

TEST(MsmRecord, NestedRecord)
{
    // header(8) + Player(40) + int32(8) = 56
    EXPECT_EQ(Nested::slot_size, 56);

    std::int8_t buf[56] = {};
    Nested nested{ buf };

    nested.get<"player">().get<"hp">() = 50;
    nested.get<"score">() = 1000;

    EXPECT_EQ(static_cast<std::int32_t>(nested.get<"player">().get<"hp">()), 50);
    EXPECT_EQ(static_cast<std::int32_t>(nested.get<"score">()), 1000);
}

TEST(MsmRecord, FieldCount)
{
    EXPECT_EQ(Player::field_count, 3);
    EXPECT_EQ(Nested::field_count, 2);
}

// ─── Memory layout verification ──────────────────────────────────────────────
// Verify that record header and field values are at correct byte offsets.

TEST(MsmRecord, MemoryLayoutVerification)
{
    // Player: header(8) + int32(8) + float64(16) + boolean(8) = 40 bytes
    std::int8_t buf[40] = {};
    Player player{ buf };

    // Write values
    player.get<"hp">() = 42;
    player.get<"pos_x">() = 9.81;
    player.get<"alive">() = true;

    // Verify header area is at offset 0 (8 bytes, currently zeroed)
    // Header: [is_const:1][type_id:1][field_count:2][reserved:4]
    // We don't write header yet, but verify fields start at offset 8

    // Field "hp" (int32): at offset 8, value at +4 within slot = byte 12
    std::int32_t hp_raw;
    std::memcpy(&hp_raw, buf + 8 + 4, 4);  // offset 8 (after header) + 4 (int32 payload offset)
    EXPECT_EQ(hp_raw, 42);

    // Field "pos_x" (float64): at offset 16, value at +8 within slot = byte 24
    double pos_raw;
    std::memcpy(&pos_raw, buf + 16 + 8, 8);  // offset 16 (8+8) + 8 (float64 payload offset)
    EXPECT_DOUBLE_EQ(pos_raw, 9.81);

    // Field "alive" (boolean): at offset 32, value at +4 within slot = byte 36
    std::int32_t alive_raw;
    std::memcpy(&alive_raw, buf + 32 + 4, 4);  // offset 32 (8+8+16) + 4 (bool payload offset)
    EXPECT_EQ(alive_raw, 1);

    // Verify we can read back through the API
    EXPECT_EQ(static_cast<std::int32_t>(player.get<"hp">()), 42);
    EXPECT_DOUBLE_EQ(static_cast<double>(player.get<"pos_x">()), 9.81);
    EXPECT_TRUE(static_cast<bool>(player.get<"alive">()));

    // Verify total layout size
    EXPECT_EQ(Player::slot_size, 40u);
    EXPECT_EQ(Player::header_size, 8u);
    EXPECT_EQ(Player::field_count, 3u);
}

// ─── Segment-based layout verification ───────────────────────────────────────
// Same test but through segment (allocator-managed memory).

TEST(MsmRecord, SegmentMemoryLayout)
{
    auto player = msm::segment::create<Player>("layout_test");

    player.get<"hp">() = 777;
    player.get<"pos_x">() = 2.718;
    player.get<"alive">() = false;

    // Get raw pointer and verify layout
    std::int8_t* raw = msm::segment::get_base("layout_test");
    ASSERT_NE(raw, nullptr);

    // hp at offset 8+4 = 12
    std::int32_t hp;
    std::memcpy(&hp, raw + 12, 4);
    EXPECT_EQ(hp, 777);

    // pos_x at offset 16+8 = 24
    double pos;
    std::memcpy(&pos, raw + 24, 8);
    EXPECT_DOUBLE_EQ(pos, 2.718);

    // alive at offset 32+4 = 36
    std::int32_t alive;
    std::memcpy(&alive, raw + 36, 4);
    EXPECT_EQ(alive, 0);  // false

    // Open from other side and verify
    auto player2 = msm::segment::open<Player>("layout_test");
    EXPECT_EQ(static_cast<std::int32_t>(player2.get<"hp">()), 777);
    EXPECT_DOUBLE_EQ(static_cast<double>(player2.get<"pos_x">()), 2.718);
    EXPECT_FALSE(static_cast<bool>(player2.get<"alive">()));

    msm::segment::close("layout_test");
}
