#include <gtest/gtest.h>
#include "segment.hpp"
#include "types.hpp"
#include "array.hpp"

// ─── Direct primitive segments (no record wrapper) ───────────────────────────

TEST(MsmDirect, Int32Segment)
{
    auto val = msm::segment::create<msm::int32>("direct_int32");
    val = 42;
    EXPECT_EQ(static_cast<std::int32_t>(val), 42);

    auto val2 = msm::segment::open<msm::int32>("direct_int32");
    EXPECT_EQ(static_cast<std::int32_t>(val2), 42);

    val2 = -100;
    EXPECT_EQ(static_cast<std::int32_t>(val), -100); // same memory

    msm::segment::close("direct_int32");
}

TEST(MsmDirect, Float64Segment)
{
    auto val = msm::segment::create<msm::float64>("direct_f64");
    val = 3.14159;
    EXPECT_DOUBLE_EQ(static_cast<double>(val), 3.14159);

    auto val2 = msm::segment::open<msm::float64>("direct_f64");
    EXPECT_DOUBLE_EQ(static_cast<double>(val2), 3.14159);

    msm::segment::close("direct_f64");
}

TEST(MsmDirect, BooleanSegment)
{
    auto val = msm::segment::create<msm::boolean>("direct_bool");
    val = true;
    EXPECT_TRUE(static_cast<bool>(val));

    val = false;
    auto val2 = msm::segment::open<msm::boolean>("direct_bool");
    EXPECT_FALSE(static_cast<bool>(val2));

    msm::segment::close("direct_bool");
}

// ─── Direct array segments (no record wrapper) ──────────────────────────────

TEST(MsmDirect, ArrayInt32Segment)
{
    auto arr = msm::segment::create<msm::array<msm::int32>>("direct_arr_i32");

    arr.push_back(10);
    arr.push_back(20);
    arr.push_back(30);

    EXPECT_EQ(arr.size(), 3);
    EXPECT_EQ(static_cast<std::int32_t>(arr[0]), 10);
    EXPECT_EQ(static_cast<std::int32_t>(arr[1]), 20);
    EXPECT_EQ(static_cast<std::int32_t>(arr[2]), 30);

    // Open from "other side"
    auto arr2 = msm::segment::open<msm::array<msm::int32>>("direct_arr_i32");
    EXPECT_EQ(arr2.size(), 3);
    EXPECT_EQ(static_cast<std::int32_t>(arr2[0]), 10);

    // Mutate through arr2, visible in arr
    arr2[0] = 99;
    EXPECT_EQ(static_cast<std::int32_t>(arr[0]), 99);

    msm::segment::close("direct_arr_i32");
}

TEST(MsmDirect, ArrayFloat64Segment)
{
    auto arr = msm::segment::create<msm::array<msm::float64>>("direct_arr_f64");

    arr.push_back(1.1);
    arr.push_back(2.2);
    arr.push_back(3.3);

    EXPECT_EQ(arr.size(), 3);
    EXPECT_NEAR(static_cast<double>(arr[0]), 1.1, 0.001);
    EXPECT_NEAR(static_cast<double>(arr[1]), 2.2, 0.001);
    EXPECT_NEAR(static_cast<double>(arr[2]), 3.3, 0.001);

    msm::segment::close("direct_arr_f64");
}

TEST(MsmDirect, ArrayGrowAndIterate)
{
    auto arr = msm::segment::create<msm::array<msm::int32>>("direct_grow");

    for (int i = 0; i < 100; ++i)
        arr.push_back(i * 2);

    EXPECT_EQ(arr.size(), 100);

    int sum = 0;
    for (auto v : arr)
        sum += static_cast<std::int32_t>(v);

    // sum of 0,2,4,...,198 = 2*(0+1+...+99) = 2*4950 = 9900
    EXPECT_EQ(sum, 9900);

    msm::segment::close("direct_grow");
}
