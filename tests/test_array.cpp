#include <gtest/gtest.h>
#include "types.hpp"
#include "member.hpp"
#include "record.hpp"
#include "array.hpp"
#include <cstring>

TEST(MsmArray, SlotSize)
{
    // Variant B (since 2026-05-26): size+cap moved into buffer header.
    // Slot contains only [header:8 | data_ptr:8] = 16 bytes.
    EXPECT_EQ((msm::array<msm::int32>::slot_size), 16);
}

TEST(MsmArray, InitiallyEmpty)
{
    std::int8_t buf[24] = {};
    msm::array<msm::int32> arr{ buf };

    EXPECT_EQ(arr.size(), 0);
    EXPECT_EQ(arr.capacity(), 0);
    EXPECT_TRUE(arr.empty());
}

TEST(MsmArray, PushBackPrimitive)
{
    std::int8_t buf[24] = {};
    msm::array<msm::int32> arr{ buf };

    arr.push_back(10);
    arr.push_back(20);
    arr.push_back(30);

    EXPECT_EQ(arr.size(), 3);
    EXPECT_GE(arr.capacity(), 3);

    EXPECT_EQ(static_cast<std::int32_t>(arr[0]), 10);
    EXPECT_EQ(static_cast<std::int32_t>(arr[1]), 20);
    EXPECT_EQ(static_cast<std::int32_t>(arr[2]), 30);

    arr.destroy();
}

TEST(MsmArray, Reserve)
{
    std::int8_t buf[24] = {};
    msm::array<msm::int32> arr{ buf };

    arr.reserve(100);
    EXPECT_GE(arr.capacity(), 100);
    EXPECT_EQ(arr.size(), 0);

    arr.destroy();
}

TEST(MsmArray, GrowsAutomatically)
{
    std::int8_t buf[24] = {};
    msm::array<msm::int32> arr{ buf };

    for (int i = 0; i < 50; ++i)
    {
        arr.push_back(i);
    }

    EXPECT_EQ(arr.size(), 50);
    for (int i = 0; i < 50; ++i)
    {
        EXPECT_EQ(static_cast<std::int32_t>(arr[i]), i);
    }

    arr.destroy();
}

TEST(MsmArray, Iterator)
{
    std::int8_t buf[24] = {};
    msm::array<msm::int32> arr{ buf };

    arr.push_back(1);
    arr.push_back(2);
    arr.push_back(3);

    int sum = 0;
    for (auto v : arr)
    {
        sum += static_cast<std::int32_t>(v);
    }
    EXPECT_EQ(sum, 6);

    arr.destroy();
}

using Edge = msm::record<
    msm::member<"from",   msm::int32>,
    msm::member<"to",     msm::int32>,
    msm::member<"weight", msm::int32>
>;

TEST(MsmArray, EmplaceBackRecord)
{
    std::int8_t buf[24] = {};
    msm::array<Edge> arr{ buf };

    arr.emplace_back(0, 1, 10);
    arr.emplace_back(1, 2, 5);

    EXPECT_EQ(arr.size(), 2);

    Edge e0 = arr[0];
    EXPECT_EQ(static_cast<std::int32_t>(e0.get<"from">()), 0);
    EXPECT_EQ(static_cast<std::int32_t>(e0.get<"to">()), 1);
    EXPECT_EQ(static_cast<std::int32_t>(e0.get<"weight">()), 10);

    Edge e1 = arr[1];
    EXPECT_EQ(static_cast<std::int32_t>(e1.get<"from">()), 1);
    EXPECT_EQ(static_cast<std::int32_t>(e1.get<"to">()), 2);
    EXPECT_EQ(static_cast<std::int32_t>(e1.get<"weight">()), 5);

    arr.destroy();
}

TEST(MsmArray, EmplaceBackNoArgs)
{
    std::int8_t buf[24] = {};
    msm::array<Edge> arr{ buf };

    auto edge = arr.emplace_back();
    edge.get<"from">() = 3;
    edge.get<"to">() = 4;
    edge.get<"weight">() = 7;

    EXPECT_EQ(arr.size(), 1);
    EXPECT_EQ(static_cast<std::int32_t>(arr[0].get<"from">()), 3);

    arr.destroy();
}

TEST(MsmArray, Clear)
{
    std::int8_t buf[24] = {};
    msm::array<msm::int32> arr{ buf };

    arr.push_back(1);
    arr.push_back(2);
    arr.clear();

    EXPECT_EQ(arr.size(), 0);
    EXPECT_GE(arr.capacity(), 2); // memory not freed

    arr.destroy();
}
