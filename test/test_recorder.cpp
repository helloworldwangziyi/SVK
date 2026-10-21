/**
 * @file test_recorder.cpp
 * @brief BinaryRecorder 写路径回归：基本写入、轮转、缓冲覆盖丢弃、启动清理。
 *
 * 运行：./build/host/test/test_recorder，或 cd build/host && ctest -R test_recorder
 */

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "recorder/binary_recorder.h"

static int g_failures = 0;

#define CHECK_TRUE(cond) \
    do { \
        if (!(cond)) { \
            ++g_failures; \
            std::printf("CHECK 失败: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        } \
    } while (0)

namespace fs = std::filesystem;

struct Tick {
    uint64_t seq;
    double price;
    int32_t volume;
    int32_t direction;
};

static Tick makeTick(uint64_t seq) {
    return Tick{seq, 3400.0 + seq * 0.5, static_cast<int32_t>(seq % 100), 1};
}

static size_t countFilesWithExt(const std::string& dir, const std::string& ext) {
    size_t count = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ext) {
            ++count;
        }
    }
    return count;
}

static uint32_t readMagicLE(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    uint8_t bytes[4] = {0, 0, 0, 0};
    in.read(reinterpret_cast<char*>(bytes), 4);
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
}

// 基本写入 + 轮转：500 条约 40B 的帧、单文件 4KB 上限，应切出多个文件。
static void test_write_and_rotation() {
    const std::string dir = (fs::temp_directory_path() / "svk_test_recorder").string();
    fs::remove_all(dir);

    svk::recorder::BinaryRecorder recorder;
    svk::recorder::BinaryRecorder::Config config;
    config.directory = dir;
    config.filePrefix = "quotes";
    config.sessionId = "20261021-test";
    config.tradingDay = 20261021;
    config.flushBytes = 512;
    config.flushIntervalMs = 50;
    config.maxFileBytes = 4 * 1024;
    CHECK_TRUE(recorder.start(config));
    CHECK_TRUE(!recorder.currentFilePath().empty());

    constexpr int kCount = 500;
    for (int i = 0; i < kCount; ++i) {
        const Tick tick = makeTick(i);
        CHECK_TRUE(recorder.write(1, tick));
    }
    recorder.flush();
    const auto stats = recorder.stats();
    recorder.stop();

    CHECK_TRUE(stats.writtenRecords + stats.droppedRecords == kCount);
    CHECK_TRUE(stats.droppedRecords == 0);
    CHECK_TRUE(stats.filesCreated >= 2);
    CHECK_TRUE(countFilesWithExt(dir, ".qlog") == stats.filesCreated);

    // 每个文件都以 magic 开头（文件头完整）。
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".qlog") {
            CHECK_TRUE(readMagicLE(entry.path().string()) == 0x53564B51);
        }
    }
    fs::remove_all(dir);
    std::printf("--- recorder：基本写入与轮转校验完成 ---\n");
}

// 缓冲防线：上限 4KB、几乎不落盘，1000 条 80B 帧大部分应被覆盖丢弃且计数吻合。
static void test_buffer_overflow_drops() {
    const std::string dir =
        (fs::temp_directory_path() / "svk_test_recorder_drops").string();
    fs::remove_all(dir);

    svk::recorder::BinaryRecorder recorder;
    svk::recorder::BinaryRecorder::Config config;
    config.directory = dir;
    config.bufferedBytesLimit = 4096;
    config.flushBytes = 1 << 20;       // 不触发按量落盘
    config.flushIntervalMs = 600000;   // 也不按时间落盘
    CHECK_TRUE(recorder.start(config));

    constexpr int kCount = 1000;
    const std::string payload(64, 'q');
    for (int i = 0; i < kCount; ++i) {
        recorder.write(1, payload.data(), payload.size());
    }
    CHECK_TRUE(recorder.isDropping());
    recorder.stop();  // stop 会把缓冲里剩余的帧落盘，之后再取统计
    const auto stats = recorder.stats();

    CHECK_TRUE(stats.writtenRecords + stats.droppedRecords == kCount);
    CHECK_TRUE(stats.droppedRecords > 0);
    fs::remove_all(dir);
    std::printf("--- recorder：缓冲覆盖丢弃校验完成（dropped=%llu）---\n",
                static_cast<unsigned long long>(stats.droppedRecords));
}

// 清理防线：启动时删除过期的历史文件。
static void test_prune_on_start() {
    const std::string dir =
        (fs::temp_directory_path() / "svk_test_recorder_prune").string();
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string oldFile = dir + "/quotes-20200101-000000-000.qlog";
    {
        std::ofstream out(oldFile);
        out << "stale";
    }
    std::error_code ec;
    fs::last_write_time(oldFile,
                        fs::file_time_type::clock::now() -
                            std::chrono::hours(24LL * 10),
                        ec);

    svk::recorder::BinaryRecorder recorder;
    svk::recorder::BinaryRecorder::Config config;
    config.directory = dir;
    config.filePrefix = "quotes";
    config.maxFileAgeDays = 3;
    CHECK_TRUE(recorder.start(config));
    recorder.stop();

    CHECK_TRUE(!fs::exists(oldFile));
    CHECK_TRUE(recorder.stats().filesDeleted == 1);
    fs::remove_all(dir);
    std::printf("--- recorder：启动清理校验完成 ---\n");
}

int main() {
    test_write_and_rotation();
    test_buffer_overflow_drops();
    test_prune_on_start();

    if (g_failures > 0) {
        std::printf("\n%d 项 CHECK 失败\n", g_failures);
        return 1;
    }
    std::printf("\n全部 CHECK 通过\n");
    return 0;
}
