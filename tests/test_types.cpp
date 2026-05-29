#include <gtest/gtest.h>
#include "types.hpp"
#include <cstring>

TEST(MsmTypes, Int32WriteRead)
{
    std::int8_t buf[8] = {};
    msm::int32 field{ buf };

    field = 42;
    EXPECT_EQ(static_cast<std::int32_t>(field), 42);

    field = -100;
    EXPECT_EQ(static_cast<std::int32_t>(field), -100);
}

TEST(MsmTypes, Float32WriteRead)
{
    std::int8_t buf[8] = {};
    msm::float32 field{ buf };

    field = 3.14f;
    EXPECT_FLOAT_EQ(static_cast<float>(field), 3.14f);
}

TEST(MsmTypes, BooleanWriteRead)
{
    std::int8_t buf[8] = {};
    msm::boolean field{ buf };

    field = true;
    EXPECT_TRUE(static_cast<bool>(field));

    field = false;
    EXPECT_FALSE(static_cast<bool>(field));
}

TEST(MsmTypes, Int64WriteRead)
{
    std::int8_t buf[16] = {};
    msm::int64 field{ buf };

    field = 123456789012345LL;
    EXPECT_EQ(static_cast<std::int64_t>(field), 123456789012345LL);
}

TEST(MsmTypes, Float64WriteRead)
{
    std::int8_t buf[16] = {};
    msm::float64 field{ buf };

    field = 2.718281828;
    EXPECT_DOUBLE_EQ(static_cast<double>(field), 2.718281828);
}

TEST(MsmTypes, SlotSizes)
{
    EXPECT_EQ(msm::boolean::slot_size, 8);
    EXPECT_EQ(msm::int32::slot_size, 8);
    EXPECT_EQ(msm::float32::slot_size, 8);
    EXPECT_EQ(msm::int64::slot_size, 16);
    EXPECT_EQ(msm::float64::slot_size, 16);
}
