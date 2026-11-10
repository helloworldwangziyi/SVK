find_package(Threads REQUIRED)

add_executable(test_log test_log.cpp)
target_include_directories(test_log PRIVATE
    ${CMAKE_SOURCE_DIR}/core/src
    ${CMAKE_SOURCE_DIR}/third_party/spdlog/include
)
target_link_libraries(test_log PRIVATE Threads::Threads)

add_test(NAME test_log COMMAND test_log)

add_executable(test_surface_probe test_surface_probe.cpp)
target_include_directories(test_surface_probe PRIVATE
    ${CMAKE_SOURCE_DIR}/samples/triangle
)
target_link_libraries(test_surface_probe PRIVATE svk_core)

add_test(NAME test_surface_probe COMMAND test_surface_probe)
