#pragma once

/**
 * @file log.h
 * @brief evk 日志封装，底层使用 third_party/spdlog（header-only）。
 *
 * core 提供便携默认（stdout），也可在平台终端 sink 的基础上追加完整文件 sink：
 * @code{.cpp}
 * evk::log::init("estarx");                                     // stdout（测试/默认）
 * evk::log::init(androidLogger);                                // 仅 Android logcat
 * evk::log::initWithFile(androidLogger, {filesDir + "/logs"});  // logcat + 文件
 * @endcode
 *
 * 所有项目日志统一输出：
 *   [evk][<feature>][<event> key=value ...]
 *
 * 使用方式：
 * @code{.cpp}
 * evk::log::init("estarx");
 * EVK_LOGI("renderer", "initialized width={} height={}", w, h);
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

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_sinks.h>

namespace evk {
namespace log {

struct LogFileConfig {
    std::string directory;
    std::string filePrefix = "estarx-vulkan";
    spdlog::level::level_enum terminalLevel = spdlog::level::debug;
    spdlog::level::level_enum fileLevel = spdlog::level::debug;
    spdlog::level::level_enum flushOnLevel = spdlog::level::info;
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
    // sink/系统日志负责时间、级别、进程与 tag；消息正文严格从 [evk] 开始。
    logger->set_pattern("%v");
    logger->set_level(spdlog::level::debug);
    spdlog::set_default_logger(logger);
    detail::currentLogDirectoryStorage().clear();
    detail::currentLogFilePathStorage().clear();
    detail::currentFileSinkStorage().reset();
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
        spdlog::warn("[evk][log][file_sink_disabled reason=no_directory]");
        return false;
    }
    if (detail::currentLogDirectoryStorage() == config.directory && hasFileSink()) {
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

        auto logger = std::make_shared<spdlog::logger>(terminalLogger->name(),
                                                       sinks.begin(), sinks.end());
        const auto loggerLevel = config.terminalLevel < config.fileLevel
                                     ? config.terminalLevel
                                     : config.fileLevel;
        logger->set_level(loggerLevel);
        logger->flush_on(config.flushOnLevel);
        spdlog::set_default_logger(logger);

        detail::currentLogDirectoryStorage() = config.directory;
        detail::currentLogFilePathStorage() = filePath;
        detail::currentFileSinkStorage() = fileSink;
        if (createdFilePath) {
            *createdFilePath = filePath;
        }
        return true;
    } catch (const std::exception& ex) {
        init(terminalLogger);
        spdlog::warn("[evk][log][file_sink_disabled path={} reason={}]", filePath,
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

inline void flush() noexcept {
    try {
        if (spdlog::logger* logger = spdlog::default_logger_raw()) {
            logger->flush();
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
    logger->log(level, "[evk][{}][{}]", feature, detail);
}

} // namespace log
} // namespace evk

/// 日志宏：第一个参数必须是功能域，后续参数遵循 spdlog 格式串。
#define EVK_LOGV(feature, ...) \
    ::evk::log::write(spdlog::level::trace, feature, __VA_ARGS__)
#define EVK_LOGD(feature, ...) \
    ::evk::log::write(spdlog::level::debug, feature, __VA_ARGS__)
#define EVK_LOGI(feature, ...) \
    ::evk::log::write(spdlog::level::info, feature, __VA_ARGS__)
#define EVK_LOGW(feature, ...) \
    ::evk::log::write(spdlog::level::warn, feature, __VA_ARGS__)
#define EVK_LOGE(feature, ...) \
    ::evk::log::write(spdlog::level::err, feature, __VA_ARGS__)
