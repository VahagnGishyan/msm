#include <gtest/gtest.h>
#include "segment.hpp"
#include "types.hpp"
#include "member.hpp"
#include "record.hpp"
#include "array.hpp"

using Simple = msm::record<
    msm::member<"x", msm::int32>,
    msm::member<"y", msm::int32>
>;

TEST(MsmSegment, CreateAndOpen)
{
    auto rec = msm::segment::create<Simple>("seg_test_1");
    rec.get<"x">() = 10;
    rec.get<"y">() = 20;

    auto rec2 = msm::segment::open<Simple>("seg_test_1");
    EXPECT_EQ(static_cast<std::int32_t>(rec2.get<"x">()), 10);
    EXPECT_EQ(static_cast<std::int32_t>(rec2.get<"y">()), 20);

    msm::segment::close("seg_test_1");
}

TEST(MsmSegment, OpenNonexistentThrows)
{
    EXPECT_THROW(msm::segment::open<Simple>("does_not_exist"), std::runtime_error);
}

TEST(MsmSegment, CloseFreesMemory)
{
    msm::segment::create<Simple>("seg_close_test");
    msm::segment::close("seg_close_test");

    // After close, get should return null
    auto* ptr = msm::segment::get_base("seg_close_test");
    EXPECT_EQ(ptr, nullptr);
}

TEST(MsmSegment, GetBase)
{
    auto rec = msm::segment::create<Simple>("seg_base_test");
    auto* base = msm::segment::get_base("seg_base_test");
    EXPECT_EQ(base, rec.ptr_);
    msm::segment::close("seg_base_test");
}

using WithArray = msm::record<
    msm::member<"count", msm::int32>,
    msm::member<"items", msm::array<msm::int32>>
>;

TEST(MsmSegment, CloseAutoDestroysArrayData)
{
    auto rec = msm::segment::create<WithArray>("seg_array_cleanup");
    auto items = rec.get<"items">();
    items.push_back(1);
    items.push_back(2);
    items.push_back(3);

    // close should call deallocate which frees array data block
    msm::segment::close("seg_array_cleanup");

    // If we get here without crash/leak, destructor worked
    auto* ptr = msm::segment::get_base("seg_array_cleanup");
    EXPECT_EQ(ptr, nullptr);
}
