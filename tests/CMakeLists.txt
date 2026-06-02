cmake_minimum_required(VERSION 3.20)
project(msm-tests CXX)

find_package(GTest REQUIRED)
include(GoogleTest)

# Single executable that runs every test cited in the paper.
#
# test_array_ops.cpp is left out: it is a cross-language integration test that
# links against the separate array-ops library, which is not part of this
# standalone tree. Add it back once that library is available here.
add_executable(run-all-tests
    test_types.cpp
    test_record.cpp
    test_array.cpp
    test_segment.cpp
    test_direct_types.cpp
    test_all_types_coverage.cpp
    test_object.cpp
    test_concurrent.cpp        # 5 stress tests under TSAN (paper §3.1)
    test_memory_overhead.cpp   # RSS bounds (paper §3.5)
    test_schema_hash.cpp       # Schema-hash death tests
)

target_link_libraries(run-all-tests PRIVATE
    msm
    GTest::gtest
    GTest::gtest_main
)

set_target_properties(run-all-tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

gtest_discover_tests(run-all-tests)
