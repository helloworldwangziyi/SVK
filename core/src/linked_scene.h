#pragma once

#include "linked_layout.h"

#include <string>

namespace svk {

// 在创建任何平台对象前完整校验组合，失败时不得修改现有有效布局。
SurfaceResult validateLinkedSceneSpec(const LinkedSceneSpec& spec);

// 仅用于 JNI/NAPI 配置传输，不是运行时 DSL。字段由 App 的 C++ 配置生成。
std::string linkedSceneSpecToJson(const LinkedSceneSpec& spec);

} // namespace svk
