#include <gtest/gtest.h>
#include "segment.hpp"
#include "object.hpp"

// ═══════════════════════════════════════════════════════════════════════════════
// msm::object tests
// ═══════════════════════════════════════════════════════════════════════════════

// ─── Basic set/get all types ─────────────────────────────────────────────────

TEST(MsmObject, BasicSetGet)
{
    auto obj = msm::segment::create<msm::object>("obj_basic");

    obj["hp"] = 100;
    obj["speed"] = 3.14;
    obj["name"] = "Alice";
    obj["alive"] = true;
    obj["big"] = static_cast<std::int64_t>(9000000000LL);

    EXPECT_EQ(obj["hp"].as_int32(), 100);
    EXPECT_DOUBLE_EQ(obj["speed"].as_float64(), 3.14);
    EXPECT_STREQ(obj["name"].as_string(), "Alice");
    EXPECT_TRUE(obj["alive"].as_bool());
    EXPECT_EQ(obj["big"].as_int64(), 9000000000LL);

    EXPECT_EQ(obj.field_count(), 5u);

    msm::segment::close("obj_basic");
}

// ─── Overwrite same type ─────────────────────────────────────────────────────

TEST(MsmObject, OverwriteSameType)
{
    auto obj = msm::segment::create<msm::object>("obj_overwrite");

    obj["hp"] = 100;
    EXPECT_EQ(obj["hp"].as_int32(), 100);

    obj["hp"] = 80;
    EXPECT_EQ(obj["hp"].as_int32(), 80);

    obj["hp"] = -1;
    EXPECT_EQ(obj["hp"].as_int32(), -1);

    obj["name"] = "Bob";
    obj["name"] = "Charlie";
    obj["name"] = "Dave";
    EXPECT_STREQ(obj["name"].as_string(), "Dave");

    EXPECT_EQ(obj.field_count(), 2u);

    msm::segment::close("obj_overwrite");
}

// ─── Remove field ────────────────────────────────────────────────────────────

TEST(MsmObject, Remove)
{
    auto obj = msm::segment::create<msm::object>("obj_remove");

    obj["a"] = 1;
    obj["b"] = 2;
    obj["c"] = 3;
    obj["d"] = 4;
    EXPECT_EQ(obj.field_count(), 4u);

    EXPECT_TRUE(obj.remove("b"));
    EXPECT_EQ(obj.field_count(), 3u);
    EXPECT_FALSE(obj.has("b"));
    EXPECT_TRUE(obj.has("a"));
    EXPECT_TRUE(obj.has("c"));
    EXPECT_TRUE(obj.has("d"));

    EXPECT_TRUE(obj.remove("a"));
    EXPECT_EQ(obj.field_count(), 2u);

    // Remove non-existent
    EXPECT_FALSE(obj.remove("xyz"));
    EXPECT_EQ(obj.field_count(), 2u);

    msm::segment::close("obj_remove");
}

// ─── Has / field_count / empty ───────────────────────────────────────────────

TEST(MsmObject, Query)
{
    auto obj = msm::segment::create<msm::object>("obj_query");

    EXPECT_EQ(obj.field_count(), 0u);
    EXPECT_FALSE(obj.has("x"));

    obj["x"] = 42;
    EXPECT_TRUE(obj.has("x"));
    EXPECT_FALSE(obj.has("y"));
    EXPECT_EQ(obj.field_count(), 1u);

    obj["y"] = 99;
    EXPECT_EQ(obj.field_count(), 2u);

    msm::segment::close("obj_query");
}

// ─── Shared access (open from other side) ────────────────────────────────────

TEST(MsmObject, SharedAccess)
{
    auto obj = msm::segment::create<msm::object>("obj_shared");

    obj["score"] = 999;
    obj["label"] = "test";

    // Open from "other side"
    auto obj2 = msm::segment::open<msm::object>("obj_shared");
    EXPECT_EQ(obj2["score"].as_int32(), 999);
    EXPECT_STREQ(obj2["label"].as_string(), "test");

    // Mutate from other side
    obj2["score"] = 1000;
    EXPECT_EQ(obj["score"].as_int32(), 1000);

    // Add field from other side
    obj2["new_field"] = 77;
    EXPECT_TRUE(obj.has("new_field"));
    EXPECT_EQ(obj["new_field"].as_int32(), 77);

    msm::segment::close("obj_shared");
}

// ─── Implicit conversion operators ──────────────────────────────────────────

TEST(MsmObject, ImplicitConversion)
{
    auto obj = msm::segment::create<msm::object>("obj_conv");

    obj["i"] = 42;
    obj["d"] = 2.718;
    obj["s"] = "hello";
    obj["b"] = true;

    // Implicit conversion via target type
    std::int32_t i = obj["i"];
    double d = obj["d"];
    const char* s = obj["s"];
    bool b = obj["b"];

    EXPECT_EQ(i, 42);
    EXPECT_DOUBLE_EQ(d, 2.718);
    EXPECT_STREQ(s, "hello");
    EXPECT_TRUE(b);

    msm::segment::close("obj_conv");
}

// ─── String overwrite (grow/shrink) ──────────────────────────────────────────

TEST(MsmObject, StringGrowShrink)
{
    auto obj = msm::segment::create<msm::object>("obj_str_grow");

    obj["msg"] = "hi";
    EXPECT_STREQ(obj["msg"].as_string(), "hi");

    obj["msg"] = "this is a much longer string that should trigger buffer reallocation";
    EXPECT_STREQ(obj["msg"].as_string(), "this is a much longer string that should trigger buffer reallocation");

    obj["msg"] = "x";
    EXPECT_STREQ(obj["msg"].as_string(), "x");

    msm::segment::close("obj_str_grow");
}

// ─── Many fields (grow entries buffer) ───────────────────────────────────────

TEST(MsmObject, ManyFields)
{
    auto obj = msm::segment::create<msm::object>("obj_many");

    // Add 20 fields (triggers multiple buffer grows: 4 → 8 → 16 → 32)
    for (int i = 0; i < 20; ++i)
    {
        char name[16];
        std::snprintf(name, sizeof(name), "field_%d", i);
        msm_object_set_int32(obj.ptr_, name, i * 10);
    }

    EXPECT_EQ(obj.field_count(), 20u);

    // Verify all
    for (int i = 0; i < 20; ++i)
    {
        char name[16];
        std::snprintf(name, sizeof(name), "field_%d", i);
        EXPECT_EQ(msm_object_get_int32(obj.ptr_, name), i * 10);
    }

    msm::segment::close("obj_many");
}


// ─── Nested object (chaining operator[]) ─────────────────────────────────────

TEST(MsmObject, NestedObject)
{
    auto obj = msm::segment::create<msm::object>("obj_nested");

    // Create nested structure: obj["player"]["pos"]["x"] = 10.0
    obj["player"]["name"] = "Hero";
    obj["player"]["hp"] = 100;
    obj["player"]["pos"]["x"] = 10.5;
    obj["player"]["pos"]["y"] = 20.3;

    obj["enemy"]["name"] = "Goblin";
    obj["enemy"]["hp"] = 50;

    // Read back
    EXPECT_STREQ(obj["player"]["name"].as_string(), "Hero");
    EXPECT_EQ(obj["player"]["hp"].as_int32(), 100);
    EXPECT_DOUBLE_EQ(obj["player"]["pos"]["x"].as_float64(), 10.5);
    EXPECT_DOUBLE_EQ(obj["player"]["pos"]["y"].as_float64(), 20.3);
    EXPECT_STREQ(obj["enemy"]["name"].as_string(), "Goblin");
    EXPECT_EQ(obj["enemy"]["hp"].as_int32(), 50);

    // Top-level has 2 fields (player, enemy)
    EXPECT_EQ(obj.field_count(), 2u);

    msm::segment::close("obj_nested");
}
