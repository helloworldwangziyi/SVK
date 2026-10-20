/**
 * @file test_log.cpp
 * @brief core/src/log.h 的功能与性能测试。
 *
 * 功能覆盖：消息三段式格式、级别过滤、文件 sink 创建/复用/内容落盘、
 * 空目录回退、同步模式 err 级立即 flush、init(tag) 幂等。
 * 性能覆盖：空调用开销（null sink）、同步文件吞吐、异步文件吞吐与零丢失校验。
 *
 * 运行：直接执行 ./build/host/test_log，或 ctest -R test_log。
 * 失败时进程返回非 0。
 */

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/sinks/sink.h>

#include "log.h"

namespace {

int g_checks = 0;
int g_failures = 0;
std::filesystem::path g_baseDir;

#define CHECK(cond)                                                     \
    do {                                                                \
        ++g_checks;                                                     \
        if (!(cond)) {                                                  \
            ++g_failures;                                               \
            std::printf("  FAIL %s:%d  CHECK(%s)\n", __FILE__,          \
                        __LINE__, #cond);                               \
        }                                                               \
    } while (0)

void section(const char* name) { std::printf("\n== %s ==\n", name); }

/// 捕获每条消息的 payload（即 [svp][feature][event ...] 正文），不经过 sink pattern。
class CaptureSink : public spdlog::sinks::base_sink<std::mutex> {
protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        messages_.emplace_back(msg.payload.data(), msg.payload.size());
    }
    void flush_() override {}

public:
    size_t size() {
        std::lock_guard<std::mutex> lock(mutex_);
        return messages_.size();
    }
    std::string back() {
        std::lock_guard<std::mutex> lock(mutex_);
        return messages_.empty() ? std::string() : messages_.back();
    }

private:
    std::vector<std::string> messages_;
};

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

size_t countOccurrences(const std::string& haystack, const std::string& needle) {
    size_t count = 0;
    size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

using Clock = std::chrono::steady_clock;

double elapsedMs(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

void reportPerf(const char* name, int count, double ms) {
    const double perSec = count / (ms / 1000.0);
    const double usPerMsg = ms * 1000.0 / count;
    std::printf("  %-26s %9d 条  %9.1f ms  %12.0f msg/s  %6.3f us/条\n", name,
                count, ms, perSec, usPerMsg);
}

// ---------------- 功能测试 ----------------

void testFormatAndLevelFilter() {
    section("功能：消息格式与级别过滤");
    auto sink = std::make_shared<CaptureSink>();
    auto logger = std::make_shared<spdlog::logger>("capture", sink);
    svp::log::init(logger);
    CHECK(spdlog::default_logger_raw() == logger.get());

    SVP_LOGI("test_feature", "event_name key={}", 42);
    CHECK(sink->size() == 1);
    CHECK(sink->back() == "[svp][test_feature][event_name key=42]");

    logger->set_level(spdlog::level::warn);
    SVP_LOGD("test_feature", "dropped_debug={}", 1);
    SVP_LOGW("test_feature", "kept_warn={}", 2);
    CHECK(sink->size() == 2);
    CHECK(sink->back() == "[svp][test_feature][kept_warn=2]");
    logger->set_level(spdlog::level::debug);

    svp::log::shutdown();
    CHECK(sink->size() == 2);
}

void testFileSinkCreateReuseAndContent() {
    section("功能：文件 sink 创建、复用与落盘内容");
    namespace fs = std::filesystem;
    const fs::path dir = g_baseDir / "file_sink";

    svp::log::LogFileConfig config;
    config.directory = dir.string();
    config.filePrefix = "svp-test";
    config.async = true;

    auto terminal = std::make_shared<spdlog::logger>(
        "term_async", std::make_shared<CaptureSink>());
    std::string path1;
    CHECK(svp::log::initWithFile(terminal, config, &path1));
    CHECK(!path1.empty());
    CHECK(svp::log::hasFileSink());
    CHECK(svp::log::currentLogFilePath() == path1);

    SVP_LOGI("file_test", "write_check seq={}", 1);

    // 同目录重复调用复用同一文件，不新建。
    std::string path2;
    CHECK(svp::log::initWithFile(terminal, config, &path2));
    CHECK(path2 == path1);
    size_t logFiles = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() == ".log") {
            ++logFiles;
        }
    }
    CHECK(logFiles == 1);

    svp::log::shutdown();  // 排空异步队列后再校验文件
    CHECK(!svp::log::hasFileSink());
    CHECK(svp::log::currentLogFilePath().empty());

    const std::string content = readFile(path1);
    CHECK(content.find("[svp][file_test][write_check seq=1]") !=
          std::string::npos);
    CHECK(content.find("[svp][log][async_started") != std::string::npos);
}

void testSyncErrorFlush() {
    section("功能：同步模式 err 级立即 flush");
    const auto dir = g_baseDir / "sync_err";

    svp::log::LogFileConfig config;
    config.directory = dir.string();
    config.async = false;

    auto terminal = std::make_shared<spdlog::logger>(
        "term_sync", std::make_shared<CaptureSink>());
    std::string path;
    CHECK(svp::log::initWithFile(terminal, config, &path));

    SVP_LOGE("file_test", "err_flush_check code={}", 7);
    // 不显式调用 flush：write() 对 err 及以上级别应已落盘。
    const std::string content = readFile(path);
    CHECK(content.find("[svp][file_test][err_flush_check code=7]") !=
          std::string::npos);

    svp::log::shutdown();
}

void testEmptyDirectoryFallback() {
    section("功能：空目录回退为终端输出");
    auto sink = std::make_shared<CaptureSink>();
    auto terminal = std::make_shared<spdlog::logger>("term_empty", sink);

    svp::log::LogFileConfig config;  // directory 留空
    CHECK(!svp::log::initWithFile(terminal, config));
    CHECK(!svp::log::hasFileSink());
    CHECK(svp::log::currentLogFilePath().empty());
    // 回退原因应作为 warn 写入终端 sink。
    CHECK(sink->size() == 1);
    CHECK(sink->back().find("[svp][log][file_sink_disabled") !=
          std::string::npos);

    svp::log::shutdown();
}

void testInitTagIdempotent() {
    section("功能：init(tag) 幂等");
    svp::log::init("svp_test_tag");
    const spdlog::logger* first = spdlog::default_logger_raw();
    CHECK(first != nullptr);
    CHECK(first && first->name() == "svp_test_tag");

    svp::log::init("svp_test_tag");
    CHECK(spdlog::default_logger_raw() == first);

    svp::log::shutdown();
}

// ---------------- 性能测试 ----------------

void perfNullSink() {
    section("性能：调用与格式化开销（null sink，无 I/O）");
    auto logger = std::make_shared<spdlog::logger>(
        "perf_null", std::make_shared<spdlog::sinks::null_sink_mt>());
    svp::log::init(logger);

    constexpr int kWarmup = 10000;
    for (int i = 0; i < kWarmup; ++i) {
        SVP_LOGI("warmup", "write seq={} value={}", i, i * 2);
    }
    constexpr int kCount = 1000000;
    const auto begin = Clock::now();
    for (int i = 0; i < kCount; ++i) {
        SVP_LOGI("perf_null", "write seq={} value={}", i, i * 2);
    }
    const auto end = Clock::now();
    reportPerf("null_sink 纯调用开销", kCount, elapsedMs(begin, end));

    svp::log::shutdown();
}

void perfSyncFile() {
    section("性能：同步文件写入吞吐");
    const auto dir = g_baseDir / "perf_sync";
    svp::log::LogFileConfig config;
    config.directory = dir.string();
    config.async = false;
    // 关闭逐条 flush，测量纯格式化 + 写入吞吐。
    config.flushOnLevel = spdlog::level::off;

    auto terminal = std::make_shared<spdlog::logger>(
        "perf_sync_term", std::make_shared<spdlog::sinks::null_sink_mt>());
    std::string path;
    CHECK(svp::log::initWithFile(terminal, config, &path));

    constexpr int kCount = 200000;
    const auto begin = Clock::now();
    for (int i = 0; i < kCount; ++i) {
        SVP_LOGI("perf_sync", "write seq={} value={}", i, i * 2);
    }
    const auto end = Clock::now();
    reportPerf("sync_file 同步写文件", kCount, elapsedMs(begin, end));

    svp::log::shutdown();
    const std::string content = readFile(path);
    CHECK(countOccurrences(content, "[svp][perf_sync][") ==
          static_cast<size_t>(kCount));
}

void perfAsyncFile() {
    section("性能：异步文件写入吞吐与零丢失");
    const auto dir = g_baseDir / "perf_async";
    svp::log::LogFileConfig config;
    config.directory = dir.string();
    config.async = true;
    config.asyncQueueSize = 65536;
    // block：队列满时调用方等待，保证一条不丢，可校验端到端完整性。
    config.asyncOverflowPolicy = spdlog::async_overflow_policy::block;
    config.flushOnLevel = spdlog::level::off;

    auto terminal = std::make_shared<spdlog::logger>(
        "perf_async_term", std::make_shared<spdlog::sinks::null_sink_mt>());
    std::string path;
    CHECK(svp::log::initWithFile(terminal, config, &path));

    constexpr int kCount = 1000000;
    const auto begin = Clock::now();
    for (int i = 0; i < kCount; ++i) {
        SVP_LOGI("perf_async", "write seq={} value={}", i, i * 2);
    }
    const auto enqueued = Clock::now();
    reportPerf("async_file 调用方入队", kCount, elapsedMs(begin, enqueued));

    const size_t dropped = svp::log::asyncDroppedCount();
    const size_t queued = svp::log::asyncQueuedCount();
    std::printf("  入队完成时队列滞留 %zu 条，丢弃 %zu 条（block 策略应为 0）\n",
                queued, dropped);
    CHECK(dropped == 0);

    svp::log::shutdown();  // 析构线程池时排空队列
    const auto drained = Clock::now();
    reportPerf("async_file 含落盘总耗时", kCount, elapsedMs(begin, drained));

    const std::string content = readFile(path);
    CHECK(countOccurrences(content, "[svp][perf_async][") ==
          static_cast<size_t>(kCount));
}

} // namespace

int main() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    g_baseDir = std::filesystem::temp_directory_path() / "svp_log_test" /
                std::to_string(stamp);
    std::printf("日志测试输出目录：%s\n", g_baseDir.string().c_str());

    testFormatAndLevelFilter();
    testFileSinkCreateReuseAndContent();
    testSyncErrorFlush();
    testEmptyDirectoryFallback();
    testInitTagIdempotent();

    perfNullSink();
    perfSyncFile();
    perfAsyncFile();

    // 示例：真实 stdout 输出效果。
    svp::log::init("svp_test");
    SVP_LOGI("demo", "log_demo message={}", "hello");
    svp::log::shutdown();

    std::error_code ec;
    std::filesystem::remove_all(g_baseDir, ec);

    std::printf("\n========================================\n");
    std::printf("共 %d 项检查，失败 %d 项 → %s\n", g_checks, g_failures,
                g_failures == 0 ? "PASS" : "FAIL");
    return g_failures == 0 ? 0 : 1;
}
