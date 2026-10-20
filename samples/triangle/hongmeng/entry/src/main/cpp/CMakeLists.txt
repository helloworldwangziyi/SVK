cmake_minimum_required(VERSION 3.16)

# Triangle 的鸿蒙工程：把 core、平台壳和 App 源文件
# 直接编成 libestarx_vulkan.so（XComponent 按 libraryname 加载）。
project(estarx_vulkan_minimal_sample)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 仓库根目录（本文件位于 samples/triangle/hongmeng/entry/src/main/cpp/）
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
    ${SVP_ROOT}/platform/hongmeng/cpp/hongmeng_napi_bridge.cpp
    ${SVP_ROOT}/platform/hongmeng/cpp/hongmeng_vulkan_platform.cpp
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

if (OHOS)
    # vulkan.h 依此宏包含 vulkan_ohos.h（vkCreateSurfaceOHOS 等声明随之可见）。
    target_compile_definitions(estarx_vulkan PRIVATE VK_USE_PLATFORM_OHOS=1)
endif()

find_library(ace-ndk-lib ace_ndk.z)       # XComponent NDK（OH_NativeXComponent_*）
find_library(ace-napi-lib ace_napi.z)     # NAPI 运行时
find_library(hilog-lib hilog_ndk.z)       # OH_LOG_Print（日志 sink）
find_library(ability-runtime-lib ability_runtime) # filesDir（文件日志目录）
find_library(vulkan-lib vulkan)

target_link_libraries(estarx_vulkan
    ${ace-ndk-lib}
    ${ace-napi-lib}
    ${hilog-lib}
    ${ability-runtime-lib}
    ${vulkan-lib}
)
