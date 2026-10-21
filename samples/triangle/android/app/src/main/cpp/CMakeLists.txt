cmake_minimum_required(VERSION 3.22.1)

# Triangle 的 Android 工程：把 core、平台壳和 App 源文件
# 直接编成一个 SHARED 库 libestarx_vulkan.so（Java 侧 System.loadLibrary）。
project(estarx_vulkan_minimal_sample)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 仓库根目录（本文件位于 samples/triangle/android/app/src/main/cpp/）
get_filename_component(SVK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../.." ABSOLUTE)
set(SVK_APP_DIR "${SVK_ROOT}/samples/triangle")
set(SVK_APP_SOURCES
    "${SVK_APP_DIR}/app.cpp"
)

find_program(GLSLC_EXECUTABLE glslc REQUIRED)

set(SVK_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(SVK_SHADER_HEADER "${SVK_GENERATED_DIR}/assets/ui_shaders.h")
file(MAKE_DIRECTORY "${SVK_GENERATED_DIR}/assets")

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${SVK_ROOT}/shaders/ui_vertex.vert"
    "${SVK_ROOT}/shaders/ui_fragment.frag"
    "${SVK_ROOT}/cmake/embed_shaders.cmake"
)

execute_process(
    COMMAND "${CMAKE_COMMAND}"
            -DGLSLC_EXECUTABLE=${GLSLC_EXECUTABLE}
            -DSHADER_NAME=ui
            -DVERT_SRC=${SVK_ROOT}/shaders/ui_vertex.vert
            -DFRAG_SRC=${SVK_ROOT}/shaders/ui_fragment.frag
            -DOUTPUT_HEADER=${SVK_SHADER_HEADER}
            -P "${SVK_ROOT}/cmake/embed_shaders.cmake"
    COMMAND_ERROR_IS_FATAL ANY
)

add_library(estarx_vulkan SHARED
    ${SVK_APP_SOURCES}
    ${SVK_ROOT}/platform/android/cpp/android_jni_bridge.cpp
    ${SVK_ROOT}/platform/android/cpp/android_vulkan_platform.cpp
    ${SVK_ROOT}/core/src/renderer.cpp
    ${SVK_ROOT}/core/src/gpu/backend_factory.cpp
    ${SVK_ROOT}/core/src/gpu/vulkan/vulkan_context.cpp
    ${SVK_ROOT}/core/src/gpu/vulkan/swapchain.cpp
    ${SVK_ROOT}/core/src/gpu/vulkan/ui_pipeline.cpp
    ${SVK_ROOT}/core/src/gpu/vulkan/texture_cache.cpp
    ${SVK_ROOT}/core/src/gpu/vulkan/vulkan_backend.cpp
    ${SVK_ROOT}/core/src/ui/canvas.cpp
)

target_include_directories(estarx_vulkan
    PRIVATE
        ${SVK_ROOT}/core/src
        ${SVK_ROOT}
        ${SVK_APP_DIR}
        ${SVK_ROOT}/third_party/spdlog/include
        ${SVK_ROOT}/third_party/glm
        ${SVK_GENERATED_DIR}
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
