#pragma once

#include <chrono>
#include <cctype>
#include <cstddef>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/spdlog.h>

namespace svk::log {

struct LogFileConfig {
    std::string directory;
    std::string filePrefix = "estarx-vulkan";
    spdlog::level::level_enum terminalLevel = spdlog::level::debug;
    spdlog::level::level_enum fileLevel = spdlog::level::debug;
    size_t maxFileSizeBytes = 10 * 1024 * 1024;
};

namespace detail {

inline std::string sanitizeFilePrefix(std::string prefix) {
    for (char& ch : prefix) {
        const auto value = static_cast<unsigned char>(ch);
        if (!std::isalnum(value) && ch != '-' && ch != '_' && ch != '.') {
            ch = '_';
        }
    }
    return prefix.empty() ? "estarx-vulkan" : prefix;
}

inline std::string dateString(std::chrono::system_clock::time_point timePoint) {
    const std::time_t time = std::chrono::system_clock::to_time_t(timePoint);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    std::ostringstream value;
    value << std::put_time(&local, "%Y-%m-%d");
    return value.str();
}

class DailyFileSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    DailyFileSink(std::string directory, std::string filePrefix,
                  size_t maxFileSizeBytes)
        : directory_(std::move(directory)),
          filePrefix_(sanitizeFilePrefix(std::move(filePrefix))),
          maxFileSizeBytes_(maxFileSizeBytes) {
        std::filesystem::create_directories(directory_);
        openDay(dateString(std::chrono::system_clock::now()));
    }

    std::string currentPath() {
        std::lock_guard<std::mutex> lock(mutex_);
        return currentPath_;
    }

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        spdlog::memory_buf_t formatted;
        formatter_->format(msg, formatted);

        const std::string date = dateString(msg.time);
        if (date != date_) {
            openDay(date);
        }
        const size_t bytes = formatted.size();
        if (currentSize_ > 0 && maxFileSizeBytes_ > 0 &&
            (currentSize_ >= maxFileSizeBytes_ ||
             bytes > maxFileSizeBytes_ - currentSize_)) {
            openPart(date_, part_ + 1);
        }
        if (!file_.is_open()) {
            throw spdlog::spdlog_ex("Log file is not open: " + currentPath_);
        }
        file_.write(formatted.data(), static_cast<std::streamsize>(bytes));
        if (!file_) {
            throw spdlog::spdlog_ex("Failed to write log file: " + currentPath_);
        }
        currentSize_ += bytes;
    }

    void flush_() override {
        if (file_.is_open()) {
            file_.flush();
        }
    }

private:
    std::string pathFor(const std::string& date, size_t part) const {
        std::ostringstream filename;
        filename << filePrefix_ << '-' << date;
        if (part > 0) {
            filename << '-' << std::setfill('0') << std::setw(3) << part;
        }
        filename << ".log";
        return (std::filesystem::path(directory_) / filename.str()).string();
    }

    void openPart(const std::string& date, size_t part) {
        if (file_.is_open()) {
            file_.close();
        }
        date_ = date;
        part_ = part;
        currentPath_ = pathFor(date, part);
        currentSize_ = 0;
        std::error_code ec;
        if (std::filesystem::exists(currentPath_, ec)) {
            currentSize_ = static_cast<size_t>(
                std::filesystem::file_size(currentPath_, ec));
            if (ec) {
                currentSize_ = 0;
            }
        }
        file_.open(currentPath_, std::ios::binary | std::ios::app);
        if (!file_.is_open()) {
            throw spdlog::spdlog_ex("Failed to open log file: " + currentPath_);
        }
    }

    void openDay(const std::string& date) {
        size_t part = 0;
        while (true) {
            const std::string path = pathFor(date, part);
            std::error_code ec;
            const bool exists = std::filesystem::exists(path, ec);
            const size_t size = exists ? static_cast<size_t>(
                std::filesystem::file_size(path, ec)) : 0;
            if (ec || !exists || size == 0 || maxFileSizeBytes_ == 0 ||
                size < maxFileSizeBytes_) {
                openPart(date, part);
                return;
            }
            ++part;
        }
    }

    std::string directory_;
    std::string filePrefix_;
    size_t maxFileSizeBytes_;
    std::string date_;
    size_t part_ = 0;
    std::string currentPath_;
    size_t currentSize_ = 0;
    std::ofstream file_;
};

inline std::shared_ptr<DailyFileSink>& fileSinkStorage() {
    static std::shared_ptr<DailyFileSink> value;
    return value;
}

inline LogFileConfig& fileConfigStorage() {
    static LogFileConfig value;
    return value;
}

inline bool sameConfig(const LogFileConfig& a, const LogFileConfig& b) {
    return a.directory == b.directory && a.filePrefix == b.filePrefix &&
           a.terminalLevel == b.terminalLevel && a.fileLevel == b.fileLevel &&
           a.maxFileSizeBytes == b.maxFileSizeBytes;
}

} // namespace detail

inline void init(const std::shared_ptr<spdlog::logger>& logger) {
    if (!logger) {
        return;
    }
    logger->set_pattern("%v");
    logger->set_level(spdlog::level::debug);
    spdlog::set_default_logger(logger);
    detail::fileSinkStorage().reset();
}

inline void init(const std::string& tag = "estarx") {
    auto logger = spdlog::get(tag);
    if (!logger) {
        logger = spdlog::stdout_logger_mt(tag);
    }
    init(logger);
}

inline bool hasFileSink() {
    const auto& fileSink = detail::fileSinkStorage();
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

inline std::string currentLogFilePath() {
    const auto& fileSink = detail::fileSinkStorage();
    return fileSink ? fileSink->currentPath() : std::string{};
}

inline bool initWithFile(const std::shared_ptr<spdlog::logger>& terminalLogger,
                         const LogFileConfig& config,
                         std::string* createdFilePath = nullptr) {
    if (createdFilePath) {
        createdFilePath->clear();
    }
    if (!terminalLogger) {
        return false;
    }
    if (!config.directory.empty() && hasFileSink() &&
        detail::sameConfig(detail::fileConfigStorage(), config)) {
        if (createdFilePath) {
            *createdFilePath = currentLogFilePath();
        }
        return true;
    }

    std::vector<spdlog::sink_ptr> sinks;
    for (const auto& sink : terminalLogger->sinks()) {
        if (sink != detail::fileSinkStorage()) {
            sink->set_pattern("%v");
            sink->set_level(config.terminalLevel);
            sinks.push_back(sink);
        }
    }
    if (sinks.empty()) {
        return false;
    }
    auto terminalOnly = std::make_shared<spdlog::logger>(
        terminalLogger->name(), sinks.begin(), sinks.end());
    terminalOnly->set_level(config.terminalLevel);
    if (config.directory.empty()) {
        init(terminalOnly);
        return false;
    }

    try {
        auto fileSink = std::make_shared<detail::DailyFileSink>(
            config.directory, config.filePrefix, config.maxFileSizeBytes);
        fileSink->set_pattern("%Y-%m-%d %H:%M:%S.%e [%l] [thread %t] %v");
        fileSink->set_level(config.fileLevel);
        sinks.push_back(fileSink);
        auto logger = std::make_shared<spdlog::logger>(
            terminalLogger->name(), sinks.begin(), sinks.end());
        logger->set_level(config.terminalLevel < config.fileLevel
                              ? config.terminalLevel
                              : config.fileLevel);
        logger->flush_on(spdlog::level::err);
        spdlog::set_default_logger(logger);
        detail::fileSinkStorage() = fileSink;
        detail::fileConfigStorage() = config;
        if (createdFilePath) {
            *createdFilePath = fileSink->currentPath();
        }
        return true;
    } catch (const std::exception& ex) {
        init(terminalOnly);
        spdlog::warn("[svk][log][file_sink_disabled reason={}]", ex.what());
        return false;
    }
}

inline void setLevel(spdlog::level::level_enum level) {
    spdlog::logger* logger = spdlog::default_logger_raw();
    if (!logger) {
        return;
    }
    logger->set_level(level);
    for (const auto& sink : logger->sinks()) {
        sink->set_level(level);
    }
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
    detail::fileSinkStorage().reset();
}

template <typename... Args>
inline void write(spdlog::level::level_enum level,
                  spdlog::string_view_t feature,
                  spdlog::format_string_t<Args...> format,
                  Args&&... args) {
    spdlog::logger* logger = spdlog::default_logger_raw();
    if (!logger || !logger->should_log(level)) {
        return;
    }
    logger->log(level, "[svk][{}][{}]", feature,
                spdlog::fmt_lib::format(format, std::forward<Args>(args)...));
}

} // namespace svk::log

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
