#include <gtest/gtest.h>
#include "segment.hpp"
#include "types.hpp"
#include "member.hpp"
#include "record.hpp"
#include "array.hpp"
#include "string.hpp"

// ═══════════════════════════════════════════════════════════════════════════════
// Coverage for types NOT tested via segment in test_direct_types.cpp:
//   - float32 segment
//   - int64 segment
//   - array<float32>
//   - array<int64>
//   - array<boolean>
//   - nested record (record inside record)
//   - array<record> with field access
//   - record with all field types combined
// ═══════════════════════════════════════════════════════════════════════════════

// ─── Float32 via segment ─────────────────────────────────────────────────────

TEST(MsmCoverage, Float32Segment)
{
    auto val = msm::segment::create<msm::float32>("cov_f32");
    val = 2.5f;
    EXPECT_FLOAT_EQ(static_cast<float>(val), 2.5f);

    auto val2 = msm::segment::open<msm::float32>("cov_f32");
    EXPECT_FLOAT_EQ(static_cast<float>(val2), 2.5f);

    val2 = -0.001f;
    EXPECT_FLOAT_EQ(static_cast<float>(val), -0.001f);

    msm::segment::close("cov_f32");
}

// ─── Int64 via segment ───────────────────────────────────────────────────────

TEST(MsmCoverage, Int64Segment)
{
    auto val = msm::segment::create<msm::int64>("cov_i64");
    val = 9'000'000'000LL;
    EXPECT_EQ(static_cast<std::int64_t>(val), 9'000'000'000LL);

    auto val2 = msm::segment::open<msm::int64>("cov_i64");
    EXPECT_EQ(static_cast<std::int64_t>(val2), 9'000'000'000LL);

    val2 = -1LL;
    EXPECT_EQ(static_cast<std::int64_t>(val), -1LL);

    msm::segment::close("cov_i64");
}

// ─── Array<float32> ──────────────────────────────────────────────────────────

TEST(MsmCoverage, ArrayFloat32)
{
    auto arr = msm::segment::create<msm::array<msm::float32>>("cov_arr_f32");

    arr.push_back(1.0f);
    arr.push_back(2.5f);
    arr.push_back(-3.7f);

    EXPECT_EQ(arr.size(), 3);
    EXPECT_FLOAT_EQ(static_cast<float>(arr[0]), 1.0f);
    EXPECT_FLOAT_EQ(static_cast<float>(arr[1]), 2.5f);
    EXPECT_FLOAT_EQ(static_cast<float>(arr[2]), -3.7f);

    // Mutate
    arr[1] = 99.9f;
    EXPECT_FLOAT_EQ(static_cast<float>(arr[1]), 99.9f);

    msm::segment::close("cov_arr_f32");
}

// ─── Array<int64> ────────────────────────────────────────────────────────────

TEST(MsmCoverage, ArrayInt64)
{
    auto arr = msm::segment::create<msm::array<msm::int64>>("cov_arr_i64");

    arr.push_back(100'000'000'000LL);
    arr.push_back(-42LL);
    arr.push_back(0LL);

    EXPECT_EQ(arr.size(), 3);
    EXPECT_EQ(static_cast<std::int64_t>(arr[0]), 100'000'000'000LL);
    EXPECT_EQ(static_cast<std::int64_t>(arr[1]), -42LL);
    EXPECT_EQ(static_cast<std::int64_t>(arr[2]), 0LL);

    msm::segment::close("cov_arr_i64");
}

// ─── Array<boolean> ──────────────────────────────────────────────────────────

TEST(MsmCoverage, ArrayBoolean)
{
    auto arr = msm::segment::create<msm::array<msm::boolean>>("cov_arr_bool");

    arr.push_back(true);
    arr.push_back(false);
    arr.push_back(true);

    EXPECT_EQ(arr.size(), 3);
    EXPECT_TRUE(static_cast<bool>(arr[0]));
    EXPECT_FALSE(static_cast<bool>(arr[1]));
    EXPECT_TRUE(static_cast<bool>(arr[2]));

    // Mutate
    arr[0] = false;
    EXPECT_FALSE(static_cast<bool>(arr[0]));

    msm::segment::close("cov_arr_bool");
}

// ─── Nested record (record inside record) ────────────────────────────────────

using Inner = msm::record<
    msm::member<"x", msm::float32>,
    msm::member<"y", msm::float32>
>;

using Outer = msm::record<
    msm::member<"id",    msm::int32>,
    msm::member<"point", Inner>
>;

TEST(MsmCoverage, NestedRecord)
{
    auto outer = msm::segment::create<Outer>("cov_nested");

    outer.get<"id">() = 7;
    auto point = outer.get<"point">();
    point.get<"x">() = 1.5f;
    point.get<"y">() = -2.5f;

    EXPECT_EQ(static_cast<std::int32_t>(outer.get<"id">()), 7);
    EXPECT_FLOAT_EQ(static_cast<float>(point.get<"x">()), 1.5f);
    EXPECT_FLOAT_EQ(static_cast<float>(point.get<"y">()), -2.5f);

    // Open from "other side"
    auto outer2 = msm::segment::open<Outer>("cov_nested");
    auto point2 = outer2.get<"point">();
    EXPECT_FLOAT_EQ(static_cast<float>(point2.get<"x">()), 1.5f);

    // Mutate nested field
    point2.get<"x">() = 100.0f;
    EXPECT_FLOAT_EQ(static_cast<float>(point.get<"x">()), 100.0f);

    msm::segment::close("cov_nested");
}

// ─── Array<record> with field access ─────────────────────────────────────────

using Item = msm::record<
    msm::member<"name_id", msm::int32>,
    msm::member<"value",   msm::float64>
>;

TEST(MsmCoverage, ArrayOfRecords)
{
    auto arr = msm::segment::create<msm::array<Item>>("cov_arr_rec");

    arr.reserve(10);

    // Emplace items
    auto item0 = arr.emplace_back();
    item0.get<"name_id">() = 1;
    item0.get<"value">() = 3.14;

    auto item1 = arr.emplace_back();
    item1.get<"name_id">() = 2;
    item1.get<"value">() = 2.718;

    auto item2 = arr.emplace_back();
    item2.get<"name_id">() = 3;
    item2.get<"value">() = 1.414;

    EXPECT_EQ(arr.size(), 3);

    // Read back
    EXPECT_EQ(static_cast<std::int32_t>(arr[0].get<"name_id">()), 1);
    EXPECT_DOUBLE_EQ(static_cast<double>(arr[0].get<"value">()), 3.14);
    EXPECT_EQ(static_cast<std::int32_t>(arr[1].get<"name_id">()), 2);
    EXPECT_DOUBLE_EQ(static_cast<double>(arr[2].get<"value">()), 1.414);

    // Mutate through array access
    arr[1].get<"value">() = 99.99;
    EXPECT_DOUBLE_EQ(static_cast<double>(arr[1].get<"value">()), 99.99);

    msm::segment::close("cov_arr_rec");
}

// ─── Record with ALL field types ─────────────────────────────────────────────

using AllTypes = msm::record<
    msm::member<"b",   msm::boolean>,
    msm::member<"i32", msm::int32>,
    msm::member<"f32", msm::float32>,
    msm::member<"i64", msm::int64>,
    msm::member<"f64", msm::float64>
>;

TEST(MsmCoverage, RecordAllPrimitiveTypes)
{
    auto rec = msm::segment::create<AllTypes>("cov_all_types");

    rec.get<"b">() = true;
    rec.get<"i32">() = -42;
    rec.get<"f32">() = 6.28f;
    rec.get<"i64">() = 1'000'000'000'000LL;
    rec.get<"f64">() = 1.23456789012345;

    EXPECT_TRUE(static_cast<bool>(rec.get<"b">()));
    EXPECT_EQ(static_cast<std::int32_t>(rec.get<"i32">()), -42);
    EXPECT_FLOAT_EQ(static_cast<float>(rec.get<"f32">()), 6.28f);
    EXPECT_EQ(static_cast<std::int64_t>(rec.get<"i64">()), 1'000'000'000'000LL);
    EXPECT_DOUBLE_EQ(static_cast<double>(rec.get<"f64">()), 1.23456789012345);

    // Open and verify
    auto rec2 = msm::segment::open<AllTypes>("cov_all_types");
    EXPECT_TRUE(static_cast<bool>(rec2.get<"b">()));
    EXPECT_EQ(static_cast<std::int32_t>(rec2.get<"i32">()), -42);
    EXPECT_FLOAT_EQ(static_cast<float>(rec2.get<"f32">()), 6.28f);
    EXPECT_EQ(static_cast<std::int64_t>(rec2.get<"i64">()), 1'000'000'000'000LL);
    EXPECT_DOUBLE_EQ(static_cast<double>(rec2.get<"f64">()), 1.23456789012345);

    msm::segment::close("cov_all_types");
}

// ─── Record with array field ─────────────────────────────────────────────────

using WithArray = msm::record<
    msm::member<"count", msm::int32>,
    msm::member<"data",  msm::array<msm::int32>>
>;

TEST(MsmCoverage, RecordWithArrayField)
{
    auto rec = msm::segment::create<WithArray>("cov_rec_arr");

    rec.get<"count">() = 5;
    auto data = rec.get<"data">();
    data.reserve(5);
    for (int i = 0; i < 5; ++i)
        data.push_back(i * 10);

    EXPECT_EQ(static_cast<std::int32_t>(rec.get<"count">()), 5);
    EXPECT_EQ(data.size(), 5);
    EXPECT_EQ(static_cast<std::int32_t>(data[0]), 0);
    EXPECT_EQ(static_cast<std::int32_t>(data[4]), 40);

    // Open and verify
    auto rec2 = msm::segment::open<WithArray>("cov_rec_arr");
    auto data2 = rec2.get<"data">();
    EXPECT_EQ(data2.size(), 5);
    EXPECT_EQ(static_cast<std::int32_t>(data2[2]), 20);

    msm::segment::close("cov_rec_arr");
}

// ─── Array clear and reuse ───────────────────────────────────────────────────

TEST(MsmCoverage, ArrayClearAndReuse)
{
    auto arr = msm::segment::create<msm::array<msm::int32>>("cov_clear");

    arr.push_back(1);
    arr.push_back(2);
    arr.push_back(3);
    EXPECT_EQ(arr.size(), 3);

    arr.clear();
    EXPECT_EQ(arr.size(), 0);

    // Reuse after clear
    arr.push_back(100);
    EXPECT_EQ(arr.size(), 1);
    EXPECT_EQ(static_cast<std::int32_t>(arr[0]), 100);

    msm::segment::close("cov_clear");
}

// ─── Array reserve and capacity ──────────────────────────────────────────────

TEST(MsmCoverage, ArrayReserveCapacity)
{
    auto arr = msm::segment::create<msm::array<msm::float64>>("cov_reserve");

    EXPECT_EQ(arr.size(), 0);
    EXPECT_EQ(arr.capacity(), 0);

    arr.reserve(100);
    EXPECT_EQ(arr.capacity(), 100);
    EXPECT_EQ(arr.size(), 0);

    for (int i = 0; i < 50; ++i)
        arr.push_back(static_cast<double>(i));

    EXPECT_EQ(arr.size(), 50);
    EXPECT_EQ(arr.capacity(), 100); // no realloc needed

    msm::segment::close("cov_reserve");
}

// ─── Emplace_back with args (record shorthand) ──────────────────────────────

using Edge = msm::record<
    msm::member<"from",   msm::int32>,
    msm::member<"to",     msm::int32>,
    msm::member<"weight", msm::int32>
>;

TEST(MsmCoverage, ArrayEmplaceBackWithArgs)
{
    auto arr = msm::segment::create<msm::array<Edge>>("cov_emplace_args");

    arr.emplace_back(0, 1, 10);
    arr.emplace_back(1, 2, 20);
    arr.emplace_back(2, 3, 30);

    EXPECT_EQ(arr.size(), 3);
    EXPECT_EQ(static_cast<std::int32_t>(arr[0].get<"from">()), 0);
    EXPECT_EQ(static_cast<std::int32_t>(arr[0].get<"to">()), 1);
    EXPECT_EQ(static_cast<std::int32_t>(arr[0].get<"weight">()), 10);
    EXPECT_EQ(static_cast<std::int32_t>(arr[2].get<"weight">()), 30);

    msm::segment::close("cov_emplace_args");
}


// ═══════════════════════════════════════════════════════════════════════════════
// String tests
// ═══════════════════════════════════════════════════════════════════════════════

// ─── String via segment: create, set, get ────────────────────────────────────

TEST(MsmCoverage, StringSegment)
{
    auto str = msm::segment::create<msm::string>("cov_str");

    // Initially empty
    EXPECT_EQ(str.length(), 0u);
    EXPECT_TRUE(str.empty());
    EXPECT_STREQ(str.c_str(), "");

    // Set value
    str = "hello world";
    EXPECT_EQ(str.length(), 11u);
    EXPECT_FALSE(str.empty());
    EXPECT_STREQ(str.c_str(), "hello world");

    // Open from "other side"
    auto str2 = msm::segment::open<msm::string>("cov_str");
    EXPECT_STREQ(str2.c_str(), "hello world");
    EXPECT_EQ(str2.length(), 11u);

    // Mutate from other side
    str2 = "changed";
    EXPECT_STREQ(str.c_str(), "changed");
    EXPECT_EQ(str.length(), 7u);

    msm::segment::close("cov_str");
}

// ─── String in record ────────────────────────────────────────────────────────

using PersonRecord = msm::record<
    msm::member<"id",   msm::int32>,
    msm::member<"name", msm::string>,
    msm::member<"age",  msm::int32>
>;

TEST(MsmCoverage, StringInRecord)
{
    auto person = msm::segment::create<PersonRecord>("cov_str_rec");

    person.get<"id">() = 42;
    person.get<"name">() = "Alice";
    person.get<"age">() = 30;

    EXPECT_EQ(static_cast<std::int32_t>(person.get<"id">()), 42);
    EXPECT_STREQ(person.get<"name">().c_str(), "Alice");
    EXPECT_EQ(person.get<"name">().length(), 5u);
    EXPECT_EQ(static_cast<std::int32_t>(person.get<"age">()), 30);

    // Open from other side
    auto person2 = msm::segment::open<PersonRecord>("cov_str_rec");
    EXPECT_STREQ(person2.get<"name">().c_str(), "Alice");

    // Mutate string field
    person2.get<"name">() = "Bob";
    EXPECT_STREQ(person.get<"name">().c_str(), "Bob");
    EXPECT_EQ(person.get<"name">().length(), 3u);

    msm::segment::close("cov_str_rec");
}

// ─── String reassign (grow) ──────────────────────────────────────────────────

TEST(MsmCoverage, StringReassign)
{
    auto str = msm::segment::create<msm::string>("cov_str_grow");

    // Start with short string
    str = "hi";
    EXPECT_STREQ(str.c_str(), "hi");
    EXPECT_EQ(str.length(), 2u);

    // Reassign to longer string (triggers buffer growth)
    str = "this is a much longer string that should trigger reallocation of the internal buffer";
    EXPECT_STREQ(str.c_str(), "this is a much longer string that should trigger reallocation of the internal buffer");
    EXPECT_EQ(str.length(), 84u);

    // Reassign back to short
    str = "x";
    EXPECT_STREQ(str.c_str(), "x");
    EXPECT_EQ(str.length(), 1u);

    // Clear
    str.clear();
    EXPECT_EQ(str.length(), 0u);
    EXPECT_TRUE(str.empty());

    // Set after clear
    str = "after clear";
    EXPECT_STREQ(str.c_str(), "after clear");

    msm::segment::close("cov_str_grow");
}

