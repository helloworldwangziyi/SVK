# host 侧测试：只依赖 header-only 的 core/src/log.h 与 spdlog，不链接 svp_core，
# 因此不需要 Vulkan SDK，可在任何桌面环境独立构建运行。
find_package(Threads REQUIRED)

add_executable(test_log test_log.cpp)
target_include_directories(test_log PRIVATE
    ${CMAKE_SOURCE_DIR}/core/src
    ${CMAKE_SOURCE_DIR}/third_party/spdlog/include
)
target_link_libraries(test_log PRIVATE Threads::Threads)

add_test(NAME test_log COMMAND test_log)
