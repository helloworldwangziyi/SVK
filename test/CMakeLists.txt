find_package(Threads REQUIRED)

add_executable(test_log test_log.cpp)
target_include_directories(test_log PRIVATE
    ${CMAKE_SOURCE_DIR}/core/src
    ${CMAKE_SOURCE_DIR}/third_party/spdlog/include
)
target_link_libraries(test_log PRIVATE Threads::Threads)

add_test(NAME test_log COMMAND test_log)
