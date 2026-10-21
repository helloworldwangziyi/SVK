#pragma once

/**
 * @file log.h
 * @brief svk 日志封装，底层使用 third_party/spdlog（header-only）。
 *
 * core 提供便携默认（stdout），也可在平台终端 sink 的基础上追加完整文件 sink：
 * @code{.cpp}
 * svk::log::init("estarx");                                     // stdout（测试/默认）
 * svk::log::init(androidLogger);                                // 仅 Android logcat
 * svk::log::initWithFile(androidLogger, {filesDir + "/logs"});  // logcat + 文件
 * @endcode
 *
 * 所有项目日志统一输出：
 *   [svk][<feature>][<event> key=value ...]
 *
 * 使用方式：
 * @code{.cpp}
 * svk::log::init("estarx");
 * SVK_LOGI("renderer", "initialized width={} height={}", w, h);
 * @endcode
 *
 * 高频事件用限速变体，防止刷屏淹没关键日志：
 * @code{.cpp}
 * SVK_LOGW_ONCE("vulkan", "validation_unsupported layer={}", name);   // 只写一次
 * SVK_LOGI_THROTTLE("quote", 5000, "alive ticks={}", n);              // 按调用点限速
 * SVK_LOGW_THROTTLE_KEY("quote", instrumentId, 60000, "seq_gap ..."); // 按合约限速
 * @endcode
 * 限速窗口内压制的条数，在下一次放行时以 suppressed=N 附在正文末尾。
 */

#include <atomic>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <ctime>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <spdlog/async.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_sinks.h>

namespace svk {
namespace log {

struct LogFileConfig {
    std::string directory;
    std::string filePrefix = "estarx-vulkan";
    spdlog::level::level_enum terminalLevel = spdlog::level::debug;
    spdlog::level::level_enum fileLevel = spdlog::level::debug;
    spdlog::level::level_enum flushOnLevel = spdlog::level::info;
    bool async = true;
    size_t asyncQueueSize = 8192;
    size_t asyncThreadCount = 1;
    spdlog::async_overflow_policy asyncOverflowPolicy =
        spdlog::async_overflow_policy::overrun_oldest;
};

namespace detail {

inline std::string& currentLogDirectoryStorage() {
    static std::string value;
    return value;
}

inline std::string& currentLogFilePathStorage() {
    static std::string value;
    return value;
}

inline std::shared_ptr<spdlog::sinks::basic_file_sink_mt>& currentFileSinkStorage() {
    static std::shared_ptr<spdlog::sinks::basic_file_sink_mt> value;
    return value;
}

inline bool& currentAsyncStorage() {
    static bool value = false;
    return value;
}

inline const char* overflowPolicyName(spdlog::async_overflow_policy policy) {
    switch (policy) {
        case spdlog::async_overflow_policy::block: return "block";
        case spdlog::async_overflow_policy::overrun_oldest: return "overrun_oldest";
        case spdlog::async_overflow_policy::discard_new: return "discard_new";
    }
    return "unknown";
}

inline std::string joinPath(const std::string& directory, const std::string& filename) {
    if (directory.empty() || directory.back() == '/') {
        return directory + filename;
    }
    return directory + "/" + filename;
}

inline std::string sanitizeFilePrefix(std::string prefix) {
    for (char& ch : prefix) {
        const auto value = static_cast<unsigned char>(ch);
        if (!std::isalnum(value) && ch != '-' && ch != '_' && ch != '.') {
            ch = '_';
        }
    }
    return prefix.empty() ? "estarx-vulkan" : prefix;
}

inline std::string makeLogFilePath(const LogFileConfig& config) {
    const auto now = std::chrono::system_clock::now();
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch()) %
                        1000;
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif

    std::ostringstream filename;
    filename << sanitizeFilePrefix(config.filePrefix) << '-'
             << std::put_time(&local, "%Y%m%d-%H%M%S") << '-' << std::setfill('0')
             << std::setw(3) << millis.count() << ".log";
    return joinPath(config.directory, filename.str());
}

/** 调用点级限速状态：每个 THROTTLE 宏展开在调用点自带一份 static。 */
struct ThrottleState {
    std::atomic<int64_t> lastPassMs{0};
    std::atomic<uint32_t> suppressed{0};
};

inline int64_t steadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/**
 * 调用点级限速：interval 窗口内的重复调用被拒绝并计数；窗口外第一次调用放行，
 * 并经 suppressedOut 带回自上次放行以来被压制的条数。多线程并发放行时，
 * 竞争失败者按被压制处理。
 */
inline bool throttleTryPass(ThrottleState& state, std::chrono::milliseconds interval,
                            uint32_t* suppressedOut) {
    const int64_t nowMs = steadyNowMs();
    int64_t last = state.lastPassMs.load(std::memory_order_relaxed);
    if (last != 0 && nowMs - last < interval.count()) {
        state.suppressed.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!state.lastPassMs.compare_exchange_strong(last, nowMs,
                                                  std::memory_order_relaxed)) {
        state.suppressed.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    *suppressedOut = state.suppressed.exchange(0, std::memory_order_relaxed);
    return true;
}

/**
 * 按 key 限速（典型 key：合约代码），各 key 独立计时。key 集合需有界
 * （如订阅的合约数），条目不淘汰；用于无界 key 时必须另行清理。
 */
inline bool throttleKeyTryPass(const std::string& key,
                               std::chrono::milliseconds interval,
                               uint32_t* suppressedOut) {
    struct Entry {
        int64_t lastPassMs = 0;
        uint32_t suppressed = 0;
    };
    static std::mutex s_mutex;
    static std::unordered_map<std::string, Entry> s_entries;

    std::lock_guard<std::mutex> lock(s_mutex);
    Entry& entry = s_entries[key];
    const int64_t nowMs = steadyNowMs();
    if (entry.lastPassMs != 0 && nowMs - entry.lastPassMs < interval.count()) {
        ++entry.suppressed;
        return false;
    }
    entry.lastPassMs = nowMs;
    *suppressedOut = entry.suppressed;
    entry.suppressed = 0;
    return true;
}

/** 已格式化正文的统一出口：级别拦截、三段式前缀、Error 同步 flush。 */
inline void writeFormatted(spdlog::level::level_enum level,
                           spdlog::string_view_t feature,
                           const std::string& detail) {
    spdlog::logger* logger = spdlog::default_logger_raw();
    if (!logger || !logger->should_log(level)) {
        return;
    }
    logger->log(level, "[svk][{}][{}]", feature, detail);
    if (level >= spdlog::level::err) {
        try {
            logger->flush();
        } catch (...) {
        }
    }
}

} // namespace detail

/**
 * @brief 把平台壳构造好的 logger 设为默认 logger（统一格式与级别）。
 * @param logger 平台壳构造好的 spdlog logger；为空则不做任何事
 */
inline void init(const std::shared_ptr<spdlog::logger>& logger) {
    if (!logger) {
        return;
    }
    // sink/系统日志负责时间、级别、进程与 tag；消息正文严格从 [svk] 开始。
    logger->set_pattern("%v");
    logger->set_level(spdlog::level::debug);
    spdlog::set_default_logger(logger);
    detail::currentLogDirectoryStorage().clear();
    detail::currentLogFilePathStorage().clear();
    detail::currentFileSinkStorage().reset();
    detail::currentAsyncStorage() = false;
}

/**
 * @brief 便携默认：stdout。桌面、测试及未定制 sink 的平台直接使用；
 * 同名 logger 重复 init 只生效一次。
 * @param tag logger 名（重复调用时按它复用已有 logger）
 */
inline void init(const std::string& tag = "estarx") {
    auto logger = spdlog::get(tag);
    if (!logger) {
        logger = spdlog::stdout_logger_mt(tag);
    }
    init(logger);
}

inline bool hasFileSink();

/**
 * @brief 保留平台终端 sink，并追加完整文件 sink。
 *
 * 同一目录重复调用时复用现有文件，不会因 Surface 重建或重复初始化而创建
 * 多个日志文件。文件创建失败时回退为终端 logger 并返回 false。
 */
inline bool initWithFile(const std::shared_ptr<spdlog::logger>& terminalLogger,
                         const LogFileConfig& config,
                         std::string* createdFilePath = nullptr) {
    if (createdFilePath) {
        createdFilePath->clear();
    }
    if (!terminalLogger) {
        return false;
    }
    if (config.directory.empty()) {
        init(terminalLogger);
        spdlog::warn("[svk][log][file_sink_disabled reason=no_directory]");
        return false;
    }
    if (detail::currentLogDirectoryStorage() == config.directory &&
        detail::currentAsyncStorage() == config.async && hasFileSink()) {
        if (createdFilePath) {
            *createdFilePath = detail::currentLogFilePathStorage();
        }
        return true;
    }

    std::vector<spdlog::sink_ptr> sinks;
    sinks.reserve(terminalLogger->sinks().size() + 1);
    for (const auto& sink : terminalLogger->sinks()) {
        if (sink == detail::currentFileSinkStorage()) {
            continue;
        }
        sink->set_pattern("%v");
        sink->set_level(config.terminalLevel);
        sinks.push_back(sink);
    }
    if (sinks.empty()) {
        init(terminalLogger);
        return false;
    }

    const std::string filePath = detail::makeLogFilePath(config);
    try {
        auto fileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
            filePath, false);
        fileSink->set_pattern("%Y-%m-%d %H:%M:%S.%e [%l] [thread %t] %v");
        fileSink->set_level(config.fileLevel);
        sinks.push_back(fileSink);

        std::shared_ptr<spdlog::logger> logger;
        if (config.async) {
            auto pool = spdlog::thread_pool();
            if (!pool) {
                spdlog::init_thread_pool(config.asyncQueueSize,
                                         config.asyncThreadCount);
                pool = spdlog::thread_pool();
            }
            logger = std::make_shared<spdlog::async_logger>(
                terminalLogger->name(), sinks.begin(), sinks.end(), pool,
                config.asyncOverflowPolicy);
        } else {
            logger = std::make_shared<spdlog::logger>(terminalLogger->name(),
                                                      sinks.begin(), sinks.end());
        }

        const auto loggerLevel = config.terminalLevel < config.fileLevel
                                     ? config.terminalLevel
                                     : config.fileLevel;
        logger->set_level(loggerLevel);
        logger->flush_on(config.flushOnLevel);
        spdlog::set_default_logger(logger);

        detail::currentLogDirectoryStorage() = config.directory;
        detail::currentLogFilePathStorage() = filePath;
        detail::currentFileSinkStorage() = fileSink;
        detail::currentAsyncStorage() = config.async;
        if (createdFilePath) {
            *createdFilePath = filePath;
        }
        if (config.async) {
            spdlog::info(
                "[svk][log][async_started queue_size={} threads={} overflow={}]",
                config.asyncQueueSize, config.asyncThreadCount,
                detail::overflowPolicyName(config.asyncOverflowPolicy));
        }
        return true;
    } catch (const std::exception& ex) {
        init(terminalLogger);
        spdlog::warn("[svk][log][file_sink_disabled path={} reason={}]", filePath,
                     ex.what());
        return false;
    }
}

inline bool hasFileSink() {
    const auto& fileSink = detail::currentFileSinkStorage();
    spdlog::logger* logger = spdlog::default_logger_raw();
    if (!fileSink || !logger) {
        return false;
    }
    for (const auto& sink : logger->sinks()) {
        if (sink == fileSink) {
            return true;
        }
    }
    return false;
}

inline const std::string& currentLogFilePath() {
    return detail::currentLogFilePathStorage();
}

inline size_t asyncQueuedCount() {
    const auto pool = spdlog::thread_pool();
    return pool ? pool->queue_size() : 0;
}

inline size_t asyncOverrunCount() {
    const auto pool = spdlog::thread_pool();
    return pool ? pool->overrun_counter() : 0;
}

inline size_t asyncDiscardCount() {
    const auto pool = spdlog::thread_pool();
    return pool ? pool->discard_counter() : 0;
}

inline size_t asyncDroppedCount() {
    return asyncOverrunCount() + asyncDiscardCount();
}

inline void resetAsyncDropCounters() {
    if (const auto pool = spdlog::thread_pool()) {
        pool->reset_overrun_counter();
        pool->reset_discard_counter();
    }
}

inline void flush() noexcept {
    try {
        spdlog::logger* logger = spdlog::default_logger_raw();
        if (logger) {
            logger->flush();
        }
        const size_t dropped = asyncDroppedCount();
        if (logger && dropped > 0) {
            spdlog::warn("[svk][log][async_queue_overrun count={}]", dropped);
            logger->flush();
            resetAsyncDropCounters();
        }
    } catch (...) {
    }
}

inline void shutdown() noexcept {
    flush();
    try {
        spdlog::shutdown();
    } catch (...) {
    }
    detail::currentLogDirectoryStorage().clear();
    detail::currentLogFilePathStorage().clear();
    detail::currentFileSinkStorage().reset();
    detail::currentAsyncStorage() = false;
}

/**
 * @brief 写一条结构化项目日志。
 *
 * feature 是稳定、可过滤的功能域；format 的第一个 token 是稳定事件名，
 * 后续动态字段使用 key=value。统一封装确保 C++ 日志不会漏掉三段式前缀。
 */
template <typename... Args>
inline void write(spdlog::level::level_enum level,
                  spdlog::string_view_t feature,
                  spdlog::format_string_t<Args...> format,
                  Args&&... args) {
    spdlog::logger* logger = spdlog::default_logger_raw();
    if (!logger || !logger->should_log(level)) {
        return;
    }
    detail::writeFormatted(
        level, feature,
        spdlog::fmt_lib::format(format, std::forward<Args>(args)...));
}

/**
 * @brief 限速写日志：同一调用点在 interval 内只放行第一条；下一次放行时
 * 把窗口内被压制的条数以 suppressed=N 附在正文末尾。
 * @param state 调用点状态，由 THROTTLE 宏的调用点 static 提供
 */
template <typename... Args>
inline void writeThrottled(detail::ThrottleState& state,
                           std::chrono::milliseconds interval,
                           spdlog::level::level_enum level,
                           spdlog::string_view_t feature,
                           spdlog::format_string_t<Args...> format,
                           Args&&... args) {
    spdlog::logger* logger = spdlog::default_logger_raw();
    if (!logger || !logger->should_log(level)) {
        return;
    }
    uint32_t suppressed = 0;
    if (!detail::throttleTryPass(state, interval, &suppressed)) {
        return;
    }
    std::string detail =
        spdlog::fmt_lib::format(format, std::forward<Args>(args)...);
    if (suppressed > 0) {
        detail += " suppressed=" + std::to_string(suppressed);
    }
    detail::writeFormatted(level, feature, detail);
}

/**
 * @brief 按 key 限速写日志：各 key 独立计时，互不影响（典型：按合约代码
 * 限速同类告警）。key 集合需有界。
 */
template <typename... Args>
inline void writeThrottledKey(spdlog::string_view_t key,
                              std::chrono::milliseconds interval,
                              spdlog::level::level_enum level,
                              spdlog::string_view_t feature,
                              spdlog::format_string_t<Args...> format,
                              Args&&... args) {
    spdlog::logger* logger = spdlog::default_logger_raw();
    if (!logger || !logger->should_log(level)) {
        return;
    }
    uint32_t suppressed = 0;
    if (!detail::throttleKeyTryPass(std::string(key.data(), key.size()), interval,
                                    &suppressed)) {
        return;
    }
    std::string detail =
        spdlog::fmt_lib::format(format, std::forward<Args>(args)...);
    if (suppressed > 0) {
        detail += " suppressed=" + std::to_string(suppressed);
    }
    detail::writeFormatted(level, feature, detail);
}

} // namespace log
} // namespace svk

/// 日志宏：第一个参数必须是功能域，后续参数遵循 spdlog 格式串。
#define SVK_LOGV(feature, ...) \
    ::svk::log::write(spdlog::level::trace, feature, __VA_ARGS__)
#define SVK_LOGD(feature, ...) \
    ::svk::log::write(spdlog::level::debug, feature, __VA_ARGS__)
#define SVK_LOGI(feature, ...) \
    ::svk::log::write(spdlog::level::info, feature, __VA_ARGS__)
#define SVK_LOGW(feature, ...) \
    ::svk::log::write(spdlog::level::warn, feature, __VA_ARGS__)
#define SVK_LOGE(feature, ...) \
    ::svk::log::write(spdlog::level::err, feature, __VA_ARGS__)

/// ONCE：同一调用点在整个进程生命周期只写一次（如驱动不支持的特性告警）。
#define SVK_LOGV_ONCE(feature, ...) \
    do { \
        static std::atomic<bool> svkLogOnce_ = false; \
        if (!svkLogOnce_.exchange(true)) { \
            ::svk::log::write(spdlog::level::trace, feature, __VA_ARGS__); \
        } \
    } while (0)
#define SVK_LOGD_ONCE(feature, ...) \
    do { \
        static std::atomic<bool> svkLogOnce_ = false; \
        if (!svkLogOnce_.exchange(true)) { \
            ::svk::log::write(spdlog::level::debug, feature, __VA_ARGS__); \
        } \
    } while (0)
#define SVK_LOGI_ONCE(feature, ...) \
    do { \
        static std::atomic<bool> svkLogOnce_ = false; \
        if (!svkLogOnce_.exchange(true)) { \
            ::svk::log::write(spdlog::level::info, feature, __VA_ARGS__); \
        } \
    } while (0)
#define SVK_LOGW_ONCE(feature, ...) \
    do { \
        static std::atomic<bool> svkLogOnce_ = false; \
        if (!svkLogOnce_.exchange(true)) { \
            ::svk::log::write(spdlog::level::warn, feature, __VA_ARGS__); \
        } \
    } while (0)
#define SVK_LOGE_ONCE(feature, ...) \
    do { \
        static std::atomic<bool> svkLogOnce_ = false; \
        if (!svkLogOnce_.exchange(true)) { \
            ::svk::log::write(spdlog::level::err, feature, __VA_ARGS__); \
        } \
    } while (0)

/// THROTTLE：同一调用点在 intervalMs 窗口内只写第一条，窗口内压制的条数
/// 在下一次放行时以 suppressed=N 附在正文末尾（如活性心跳、重试刷屏）。
#define SVK_LOGV_THROTTLE(feature, intervalMs, ...) \
    do { \
        static ::svk::log::detail::ThrottleState svkThrottleState_; \
        ::svk::log::writeThrottled(svkThrottleState_, \
                                   std::chrono::milliseconds(intervalMs), \
                                   spdlog::level::trace, feature, __VA_ARGS__); \
    } while (0)
#define SVK_LOGD_THROTTLE(feature, intervalMs, ...) \
    do { \
        static ::svk::log::detail::ThrottleState svkThrottleState_; \
        ::svk::log::writeThrottled(svkThrottleState_, \
                                   std::chrono::milliseconds(intervalMs), \
                                   spdlog::level::debug, feature, __VA_ARGS__); \
    } while (0)
#define SVK_LOGI_THROTTLE(feature, intervalMs, ...) \
    do { \
        static ::svk::log::detail::ThrottleState svkThrottleState_; \
        ::svk::log::writeThrottled(svkThrottleState_, \
                                   std::chrono::milliseconds(intervalMs), \
                                   spdlog::level::info, feature, __VA_ARGS__); \
    } while (0)
#define SVK_LOGW_THROTTLE(feature, intervalMs, ...) \
    do { \
        static ::svk::log::detail::ThrottleState svkThrottleState_; \
        ::svk::log::writeThrottled(svkThrottleState_, \
                                   std::chrono::milliseconds(intervalMs), \
                                   spdlog::level::warn, feature, __VA_ARGS__); \
    } while (0)
#define SVK_LOGE_THROTTLE(feature, intervalMs, ...) \
    do { \
        static ::svk::log::detail::ThrottleState svkThrottleState_; \
        ::svk::log::writeThrottled(svkThrottleState_, \
                                   std::chrono::milliseconds(intervalMs), \
                                   spdlog::level::err, feature, __VA_ARGS__); \
    } while (0)

/// THROTTLE_KEY：同一 key 在 intervalMs 窗口内只写第一条，各 key 独立计时
/// （如按合约代码限速 seq_gap 告警）。key 集合需有界。
#define SVK_LOGV_THROTTLE_KEY(feature, key, intervalMs, ...) \
    ::svk::log::writeThrottledKey((key), std::chrono::milliseconds(intervalMs), \
                                  spdlog::level::trace, feature, __VA_ARGS__)
#define SVK_LOGD_THROTTLE_KEY(feature, key, intervalMs, ...) \
    ::svk::log::writeThrottledKey((key), std::chrono::milliseconds(intervalMs), \
                                  spdlog::level::debug, feature, __VA_ARGS__)
#define SVK_LOGI_THROTTLE_KEY(feature, key, intervalMs, ...) \
    ::svk::log::writeThrottledKey((key), std::chrono::milliseconds(intervalMs), \
                                  spdlog::level::info, feature, __VA_ARGS__)
#define SVK_LOGW_THROTTLE_KEY(feature, key, intervalMs, ...) \
    ::svk::log::writeThrottledKey((key), std::chrono::milliseconds(intervalMs), \
                                  spdlog::level::warn, feature, __VA_ARGS__)
#define SVK_LOGE_THROTTLE_KEY(feature, key, intervalMs, ...) \
    ::svk::log::writeThrottledKey((key), std::chrono::milliseconds(intervalMs), \
                                  spdlog::level::err, feature, __VA_ARGS__)
