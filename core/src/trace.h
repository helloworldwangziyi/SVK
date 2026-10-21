#pragma once

/**
 * @file trace.h
 * @brief 帧耗时/卡顿追踪宏（设计见 docs/logging-design.md 第 10 节）。
 *
 * 日志 ≠ 追踪：帧耗时分析走平台 trace 系统，不进日志队列。
 * `SVK_TRACE_SCOPE(name)` 在作用域内标记一个耗时区间：
 *   - Android：映射到 ATrace（定义 SVK_TRACE_ENABLE 且 __ANDROID__ 时生效），
 *     用 systrace/Perfetto 查看；
 *   - iOS / 鸿蒙：暂为空实现，后续在平台层接 os_signpost / HiTrace
 *     （需要 ObjC 与 ArkTS 侧配合，见平台桥接代码）；
 *   - 其他（含 host 测试）：空实现，零开销。
 *
 * 用法：
 * @code{.cpp}
 * void drawFrame() {
 *     SVK_TRACE_SCOPE("svk.drawFrame");
 *     ...
 * }
 * @endcode
 */

#if defined(SVK_TRACE_ENABLE) && defined(__ANDROID__)
#include <android/trace.h>

namespace svk {
namespace trace {

struct Scope {
    explicit Scope(const char* name) { ATrace_beginSection(name); }
    ~Scope() { ATrace_endSection(); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

} // namespace trace
} // namespace svk

#define SVK_TRACE_SCOPE(name) ::svk::trace::Scope svkTraceScope_(name)
#else
#define SVK_TRACE_SCOPE(name) \
    do { \
    } while (0)
#endif
