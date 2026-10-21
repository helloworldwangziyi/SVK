cmake_minimum_required(VERSION 3.22.1)

project(estarx_vulkan_minimal LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- shaders：glslc 编译 → SPIR-V 转 uint32 数组 → 内嵌头（免运行时读文件） ----
find_program(GLSLC_EXECUTABLE glslc REQUIRED)

set(SVK_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(SVK_SHADER_HEADER "${SVK_GENERATED_DIR}/assets/ui_shaders.h")
file(MAKE_DIRECTORY "${SVK_GENERATED_DIR}/assets")

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/ui_vertex.vert"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/ui_fragment.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_shaders.cmake"
)

execute_process(
    COMMAND "${CMAKE_COMMAND}"
            -DGLSLC_EXECUTABLE=${GLSLC_EXECUTABLE}
            -DSHADER_NAME=ui
            -DVERT_SRC=${CMAKE_CURRENT_SOURCE_DIR}/shaders/ui_vertex.vert
            -DFRAG_SRC=${CMAKE_CURRENT_SOURCE_DIR}/shaders/ui_fragment.frag
            -DOUTPUT_HEADER=${SVK_SHADER_HEADER}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_shaders.cmake"
    COMMAND_ERROR_IS_FATAL ANY
)

# ---- 引擎核心静态库（平台无关） ----
add_library(svk_core STATIC
    core/src/vulkan_renderer.cpp
    core/src/gpu/vulkan_context.cpp
    core/src/gpu/swapchain.cpp
    core/src/gpu/ui_pipeline.cpp
    core/src/gpu/texture_cache.cpp
    core/src/ui/canvas.cpp
)

target_include_directories(svk_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/core/src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/glm
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/spdlog/include
    PRIVATE
        ${SVK_GENERATED_DIR}
)

# Android App 使用 NDK 提供的 Vulkan 头与 loader；独立构建 core 时使用 Vulkan SDK。
if(NOT ANDROID)
    find_package(Vulkan REQUIRED)
    target_link_libraries(svk_core PUBLIC Vulkan::Vulkan)
endif()

# ---- host 侧测试 ----
option(SVK_BUILD_TESTS "构建 host 侧测试程序（test/）" ON)
if(SVK_BUILD_TESTS AND NOT ANDROID)
    enable_testing()
    add_subdirectory(test)
endif()
