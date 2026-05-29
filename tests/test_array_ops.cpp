#include <gtest/gtest.h>
#include "segment.hpp"
#include "types.hpp"
#include "array.hpp"

// Same schema as array-ops library
using IntArray = msm::array<msm::int32>;
using DoubleArray = msm::array<msm::float64>;

// Forward declarations (from array-ops DLL)
extern "C"
{
    std::int64_t array_sum_int32(const char* name);
    void         array_reverse_int32(const char* name);
    void         array_gradient_step(const char* name);
    double       array_sum_squares(const char* name);
}

TEST(ArrayOps, SumInt32)
{
    // C# side: create segment, push data
    auto arr = msm::segment::create<IntArray>("test_sum");
    arr.push_back(10);
    arr.push_back(20);
    arr.push_back(30);

    // C++ side: open by name, compute sum
    std::int64_t sum = array_sum_int32("test_sum");
    EXPECT_EQ(sum, 60);

    msm::segment::close("test_sum");
}

TEST(ArrayOps, ReverseInt32)
{
    auto arr = msm::segment::create<IntArray>("test_rev");
    arr.push_back(1);
    arr.push_back(2);
    arr.push_back(3);
    arr.push_back(4);
    arr.push_back(5);

    array_reverse_int32("test_rev");

    // Verify reversed
    auto arr2 = msm::segment::open<IntArray>("test_rev");
    EXPECT_EQ(static_cast<std::int32_t>(arr2[0]), 5);
    EXPECT_EQ(static_cast<std::int32_t>(arr2[1]), 4);
    EXPECT_EQ(static_cast<std::int32_t>(arr2[2]), 3);
    EXPECT_EQ(static_cast<std::int32_t>(arr2[3]), 2);
    EXPECT_EQ(static_cast<std::int32_t>(arr2[4]), 1);

    msm::segment::close("test_rev");
}

TEST(ArrayOps, GradientStep)
{
    auto arr = msm::segment::create<DoubleArray>("test_grad");
    arr.push_back(1.0);
    arr.push_back(2.0);
    arr.push_back(3.0);

    array_gradient_step("test_grad");

    auto arr2 = msm::segment::open<DoubleArray>("test_grad");
    // 1.0 * 0.99 + 0.01 = 1.0
    // 2.0 * 0.99 + 0.01 = 1.99
    // 3.0 * 0.99 + 0.01 = 2.98
    EXPECT_NEAR(static_cast<double>(arr2[0]), 1.0, 0.001);
    EXPECT_NEAR(static_cast<double>(arr2[1]), 1.99, 0.001);
    EXPECT_NEAR(static_cast<double>(arr2[2]), 2.98, 0.001);

    msm::segment::close("test_grad");
}

TEST(ArrayOps, SumSquares)
{
    auto arr = msm::segment::create<DoubleArray>("test_sq");
    arr.push_back(3.0);
    arr.push_back(4.0);

    double result = array_sum_squares("test_sq");
    // 3^2 + 4^2 = 9 + 16 = 25
    EXPECT_NEAR(result, 25.0, 0.001);

    msm::segment::close("test_sq");
}
