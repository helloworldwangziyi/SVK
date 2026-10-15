cmake_minimum_required(VERSION 3.22.1)

# 与母项目同款约定：从 cpp 目录回溯到项目根，把 core 源码 + 平台壳
# 直接编成一个 SHARED 库 libestarx_vulkan.so（Java 侧 System.loadLibrary）。
project(estarx_vulkan_minimal_sample)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# cpp → main → src → app → android → samples → 项目根
set(EVK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../..)

find_program(GLSLC_EXECUTABLE glslc REQUIRED)

set(EVK_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(EVK_SHADER_HEADER "${EVK_GENERATED_DIR}/assets/ui_shaders.h")
file(MAKE_DIRECTORY "${EVK_GENERATED_DIR}/assets")

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${EVK_ROOT}/shaders/ui_vertex.vert"
    "${EVK_ROOT}/shaders/ui_fragment.frag"
    "${EVK_ROOT}/cmake/embed_shaders.cmake"
)

execute_process(
    COMMAND "${CMAKE_COMMAND}"
            -DGLSLC_EXECUTABLE=${GLSLC_EXECUTABLE}
            -DSHADER_NAME=ui
            -DVERT_SRC=${EVK_ROOT}/shaders/ui_vertex.vert
            -DFRAG_SRC=${EVK_ROOT}/shaders/ui_fragment.frag
            -DOUTPUT_HEADER=${EVK_SHADER_HEADER}
            -P "${EVK_ROOT}/cmake/embed_shaders.cmake"
    COMMAND_ERROR_IS_FATAL ANY
)

add_library(estarx_vulkan SHARED
    ${EVK_ROOT}/platform/android/cpp/android_jni_bridge.cpp
    ${EVK_ROOT}/platform/android/cpp/android_vulkan_platform.cpp
    ${EVK_ROOT}/core/src/vulkan_renderer.cpp
    ${EVK_ROOT}/core/src/gpu/vulkan_context.cpp
    ${EVK_ROOT}/core/src/gpu/swapchain.cpp
    ${EVK_ROOT}/core/src/gpu/ui_pipeline.cpp
    ${EVK_ROOT}/core/src/gpu/texture_cache.cpp
    ${EVK_ROOT}/core/src/ui/canvas.cpp
)

target_include_directories(estarx_vulkan
    PRIVATE
        ${EVK_ROOT}/core/src
        ${EVK_ROOT}
        ${EVK_ROOT}/third_party/spdlog/include
        ${EVK_ROOT}/third_party/glm
        ${EVK_GENERATED_DIR}
)

# 必须：否则 vulkan.h 不声明 android surface 类型与 vkCreateAndroidSurfaceKHR。
target_compile_definitions(estarx_vulkan PRIVATE VK_USE_PLATFORM_ANDROID_KHR)

find_library(log-lib log)
find_library(android-lib android)
find_library(vulkan-lib vulkan)

target_link_libraries(estarx_vulkan
    ${log-lib}
    ${android-lib}
    ${vulkan-lib}
)
