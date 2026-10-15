cmake_minimum_required(VERSION 3.22.1)

project(estarx_vulkan_minimal LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- shaders：glslc 编译 → SPIR-V 转 uint32 数组 → 内嵌头（免运行时读文件） ----
find_program(GLSLC_EXECUTABLE glslc REQUIRED)

set(EVK_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(EVK_SHADER_HEADER "${EVK_GENERATED_DIR}/assets/ui_shaders.h")
file(MAKE_DIRECTORY "${EVK_GENERATED_DIR}/assets")

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
            -DOUTPUT_HEADER=${EVK_SHADER_HEADER}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_shaders.cmake"
    COMMAND_ERROR_IS_FATAL ANY
)

# ---- 引擎核心静态库（平台无关） ----
add_library(evk_core STATIC
    core/src/vulkan_renderer.cpp
    core/src/gpu/vulkan_context.cpp
    core/src/gpu/swapchain.cpp
    core/src/gpu/ui_pipeline.cpp
    core/src/gpu/texture_cache.cpp
    core/src/ui/canvas.cpp
)

target_include_directories(evk_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/core/src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/glm
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/spdlog/include
    PRIVATE
        ${EVK_GENERATED_DIR}
)

# ---- 桌面端薄壳（Win32 验证壳）：窗口 → surface → 每帧三角形 ----
# Android 不走这里：samples/android 的 CMake 直接把 core 源码编进 .so，
# Vulkan 头与 libvulkan 来自 NDK。
if(NOT ANDROID)
    find_package(Vulkan REQUIRED)
    target_link_libraries(evk_core PUBLIC Vulkan::Vulkan)

    add_executable(evk_triangle_desktop
        platform/desktop/main.cpp
    )
    target_include_directories(evk_triangle_desktop PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(evk_triangle_desktop PRIVATE evk_core)
endif()
