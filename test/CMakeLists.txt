# host 侧测试：只依赖 header-only 的 core/src/log.h 与 spdlog，不链接 svk_core，
# 因此不需要 Vulkan SDK，可在任何桌面环境独立构建运行。
find_package(Threads REQUIRED)

add_executable(test_log test_log.cpp)
target_include_directories(test_log PRIVATE
    ${CMAKE_SOURCE_DIR}/core/src
    ${CMAKE_SOURCE_DIR}/third_party/spdlog/include
)
target_link_libraries(test_log PRIVATE Threads::Threads)

add_test(NAME test_log COMMAND test_log)

add_executable(test_recorder test_recorder.cpp)
target_include_directories(test_recorder PRIVATE
    ${CMAKE_SOURCE_DIR}/core/src
    ${CMAKE_SOURCE_DIR}/third_party/spdlog/include
)
target_link_libraries(test_recorder PRIVATE Threads::Threads)

add_test(NAME test_recorder COMMAND test_recorder)

add_executable(test_crypto test_crypto.cpp)
target_include_directories(test_crypto PRIVATE
    ${CMAKE_SOURCE_DIR}/core/src
    ${CMAKE_SOURCE_DIR}/third_party/spdlog/include
)
target_link_libraries(test_crypto PRIVATE Threads::Threads)

add_test(NAME test_crypto COMMAND test_crypto)
