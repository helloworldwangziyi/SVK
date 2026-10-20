#pragma once

/**
 * @file log.h
 * @brief svp 日志封装，底层使用 third_party/spdlog（header-only）。
 *
 * core 提供便携默认（stdout），也可在平台终端 sink 的基础上追加完整文件 sink：
 * @code{.cpp}
 * svp::log::init("estarx");                                     // stdout（测试/默认）
 * svp::log::init(androidLogger);                                // 仅 Android logcat
 * svp::log::initWithFile(androidLogger, {filesDir + "/logs"});  // logcat + 文件
 * @endcode
 *
 * 所有项目日志统一输出：
 *   [svp][<feature>][<event> key=value ...]
 *
 * 使用方式：
 * @code{.cpp}
 * svp::log::init("estarx");
 * SVP_LOGI("renderer", "initialized width={} height={}", w, h);
 * @endcode
 */

#include <chrono>
#include <cctype>
#include <cstddef>
#include <ctime>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <spdlog/async.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_sinks.h>

namespace svp {
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

} // namespace detail

/**
 * @brief 把平台壳构造好的 logger 设为默认 logger（统一格式与级别）。
 * @param logger 平台壳构造好的 spdlog logger；为空则不做任何事
 */
inline void init(const std::shared_ptr<spdlog::logger>& logger) {
    if (!logger) {
        return;
    }
    // sink/系统日志负责时间、级别、进程与 tag；消息正文严格从 [svp] 开始。
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
        spdlog::warn("[svp][log][file_sink_disabled reason=no_directory]");
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
                "[svp][log][async_started queue_size={} threads={} overflow={}]",
                config.asyncQueueSize, config.asyncThreadCount,
                detail::overflowPolicyName(config.asyncOverflowPolicy));
        }
        return true;
    } catch (const std::exception& ex) {
        init(terminalLogger);
        spdlog::warn("[svp][log][file_sink_disabled path={} reason={}]", filePath,
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
            spdlog::warn("[svp][log][async_queue_overrun count={}]", dropped);
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
    const auto detail = spdlog::fmt_lib::format(
        format, std::forward<Args>(args)...);
    logger->log(level, "[svp][{}][{}]", feature, detail);
    if (level >= spdlog::level::err) {
        try {
            logger->flush();
        } catch (...) {
        }
    }
}

} // namespace log
} // namespace svp

/// 日志宏：第一个参数必须是功能域，后续参数遵循 spdlog 格式串。
#define SVP_LOGV(feature, ...) \
    ::svp::log::write(spdlog::level::trace, feature, __VA_ARGS__)
#define SVP_LOGD(feature, ...) \
    ::svp::log::write(spdlog::level::debug, feature, __VA_ARGS__)
#define SVP_LOGI(feature, ...) \
    ::svp::log::write(spdlog::level::info, feature, __VA_ARGS__)
#define SVP_LOGW(feature, ...) \
    ::svp::log::write(spdlog::level::warn, feature, __VA_ARGS__)
#define SVP_LOGE(feature, ...) \
    ::svp::log::write(spdlog::level::err, feature, __VA_ARGS__)
