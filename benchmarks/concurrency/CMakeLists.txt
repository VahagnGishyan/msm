cmake_minimum_required(VERSION 3.20)
project(msm-concurrency-bench CXX)

# Benchmark used for paper §3.2 (scalability) and §3.4 (comparison vs std::atomic /
# std::mutex / std::shared_mutex / pthread_rwlock_t).
add_executable(${PROJECT_NAME}
    main.cpp
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    msm
    pthread
)

target_compile_options(${PROJECT_NAME} PRIVATE
    "$<$<CONFIG:Release>:-O3>"
    "$<$<CONFIG:Debug>:-O1>"
)

set_target_properties(${PROJECT_NAME} PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)
