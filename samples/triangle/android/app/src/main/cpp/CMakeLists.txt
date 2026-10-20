cmake_minimum_required(VERSION 3.22.1)

# Triangle 的 Android 工程：把 core、平台壳和 App 源文件
# 直接编成一个 SHARED 库 libestarx_vulkan.so（Java 侧 System.loadLibrary）。
project(estarx_vulkan_minimal_sample)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 仓库根目录（本文件位于 samples/triangle/android/app/src/main/cpp/）
get_filename_component(SVP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../.." ABSOLUTE)
set(SVP_APP_DIR "${SVP_ROOT}/samples/triangle")
set(SVP_APP_SOURCES
    "${SVP_APP_DIR}/app.cpp"
)

find_program(GLSLC_EXECUTABLE glslc REQUIRED)

set(SVP_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(SVP_SHADER_HEADER "${SVP_GENERATED_DIR}/assets/ui_shaders.h")
file(MAKE_DIRECTORY "${SVP_GENERATED_DIR}/assets")

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${SVP_ROOT}/shaders/ui_vertex.vert"
    "${SVP_ROOT}/shaders/ui_fragment.frag"
    "${SVP_ROOT}/cmake/embed_shaders.cmake"
)

execute_process(
    COMMAND "${CMAKE_COMMAND}"
            -DGLSLC_EXECUTABLE=${GLSLC_EXECUTABLE}
            -DSHADER_NAME=ui
            -DVERT_SRC=${SVP_ROOT}/shaders/ui_vertex.vert
            -DFRAG_SRC=${SVP_ROOT}/shaders/ui_fragment.frag
            -DOUTPUT_HEADER=${SVP_SHADER_HEADER}
            -P "${SVP_ROOT}/cmake/embed_shaders.cmake"
    COMMAND_ERROR_IS_FATAL ANY
)

add_library(estarx_vulkan SHARED
    ${SVP_APP_SOURCES}
    ${SVP_ROOT}/platform/android/cpp/android_jni_bridge.cpp
    ${SVP_ROOT}/platform/android/cpp/android_vulkan_platform.cpp
    ${SVP_ROOT}/core/src/vulkan_renderer.cpp
    ${SVP_ROOT}/core/src/gpu/vulkan_context.cpp
    ${SVP_ROOT}/core/src/gpu/swapchain.cpp
    ${SVP_ROOT}/core/src/gpu/ui_pipeline.cpp
    ${SVP_ROOT}/core/src/gpu/texture_cache.cpp
    ${SVP_ROOT}/core/src/ui/canvas.cpp
)

target_include_directories(estarx_vulkan
    PRIVATE
        ${SVP_ROOT}/core/src
        ${SVP_ROOT}
        ${SVP_APP_DIR}
        ${SVP_ROOT}/third_party/spdlog/include
        ${SVP_ROOT}/third_party/glm
        ${SVP_GENERATED_DIR}
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
