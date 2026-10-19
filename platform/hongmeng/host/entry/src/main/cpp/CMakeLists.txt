cmake_minimum_required(VERSION 3.16)

# 共享鸿蒙宿主：把 core、平台壳和当前 App 源文件
# 直接编成 libestarx_vulkan.so（XComponent 按 libraryname 加载）。
project(estarx_vulkan_minimal_sample)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(EVK_ROOT "@EVK_ROOT@")
set(EVK_APP_DIR "@EVK_APP_DIR@")
set(EVK_APP_SOURCES
    @EVK_APP_CMAKE_SOURCES@
)

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
    ${EVK_APP_SOURCES}
    ${EVK_ROOT}/platform/hongmeng/cpp/hongmeng_napi_bridge.cpp
    ${EVK_ROOT}/platform/hongmeng/cpp/hongmeng_vulkan_platform.cpp
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
        ${EVK_APP_DIR}
        ${EVK_ROOT}/third_party/spdlog/include
        ${EVK_ROOT}/third_party/glm
        ${EVK_GENERATED_DIR}
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
