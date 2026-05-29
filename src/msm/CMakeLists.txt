cmake_minimum_required(VERSION 3.20)
project(msm CXX)

add_library(${PROJECT_NAME} SHARED
    capi.cpp
    msm_allocator.cpp
)

target_include_directories(${PROJECT_NAME} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# MSM_ALLOCATOR controls SHARE_LIB export macro in sharelib.hpp
target_compile_definitions(${PROJECT_NAME} PRIVATE MSM_ALLOCATOR)

set_target_properties(${PROJECT_NAME} PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)
