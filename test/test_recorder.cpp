/**
 * @file test_recorder.cpp
 * @brief BinaryRecorder 写路径回归：基本写入、轮转、缓冲覆盖丢弃、启动清理。
 *
 * 运行：./build/host/test/test_recorder，或 cd build/host && ctest -R test_recorder
 */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

#if defined(__unix__) || defined(__APPLE__)
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "recorder/binary_reader.h"
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

// roundtrip：写 300 条交替 type 的 tick，读回校验数量、seq 完整性、
// 类型过滤、CSV 导出行数、损坏尾部容错。
static void test_reader_roundtrip() {
    const std::string dir = (fs::temp_directory_path() / "svk_test_reader").string();
    fs::remove_all(dir);

    svk::recorder::BinaryRecorder recorder;
    svk::recorder::BinaryRecorder::Config config;
    config.directory = dir;
    config.filePrefix = "quotes";
    config.sessionId = "reader-test";
    config.tradingDay = 20261021;
    config.flushBytes = 256;
    config.flushIntervalMs = 20;
    config.maxFileBytes = 2 * 1024;
    CHECK_TRUE(recorder.start(config));

    constexpr int kCount = 300;
    for (int i = 0; i < kCount; ++i) {
        const Tick tick = makeTick(i);
        recorder.write(i % 2 == 0 ? 1 : 2, tick);
    }
    recorder.flush();
    recorder.stop();

    const auto files =
        svk::recorder::BinaryReader::listFiles(dir, ".qlog");
    CHECK_TRUE(files.size() >= 2);

    // 全量读回：条数、payload 完整、文件头字段正确。
    std::vector<uint64_t> seqs;
    for (const auto& path : files) {
        svk::recorder::FileHeaderView header;
        const int64_t n = svk::recorder::BinaryReader::readFile(
            path, {},
            [&](const svk::recorder::RecordView& rec) {
                CHECK_TRUE(rec.payloadBytes == sizeof(Tick));
                Tick tick{};
                std::memcpy(&tick, rec.payload, sizeof(Tick));
                seqs.push_back(tick.seq);
                return true;
            },
            &header);
        CHECK_TRUE(n > 0);
        CHECK_TRUE(header.magic == 0x53564B51);
        CHECK_TRUE(header.tradingDay == 20261021);
    }
    CHECK_TRUE(seqs.size() == static_cast<size_t>(kCount));
    std::sort(seqs.begin(), seqs.end());
    for (size_t i = 0; i < seqs.size(); ++i) {
        CHECK_TRUE(seqs[i] == i);
    }

    // 类型过滤：type=2 恰好 150 条，且 seq 都是奇数。
    svk::recorder::RecordFilter typeFilter;
    typeFilter.types = {2};
    int type2Count = 0;
    for (const auto& path : files) {
        svk::recorder::BinaryReader::readFile(
            path, typeFilter,
            [&](const svk::recorder::RecordView& rec) {
                Tick tick{};
                std::memcpy(&tick, rec.payload, sizeof(Tick));
                CHECK_TRUE(tick.seq % 2 == 1);
                ++type2Count;
                return true;
            });
    }
    CHECK_TRUE(type2Count == kCount / 2);

    // CSV 导出：行数 = 该文件记录数 + 表头。
    const int64_t fileRecords = svk::recorder::BinaryReader::readFile(
        files.front(), {}, [](const svk::recorder::RecordView&) {
            return true;
        });
    const std::string csvPath = dir + "/export.csv";
    CHECK_TRUE(svk::recorder::BinaryReader::exportCsv(
        files.front(), {}, csvPath,
        [](const svk::recorder::RecordView& rec) {
            Tick tick{};
            std::memcpy(&tick, rec.payload, sizeof(Tick));
            return std::to_string(tick.seq) + "," +
                   std::to_string(tick.price);
        }));
    std::ifstream csvIn(csvPath);
    const std::string csvContent((std::istreambuf_iterator<char>(csvIn)),
                                 std::istreambuf_iterator<char>());
    const auto lineCount = static_cast<int64_t>(
        std::count(csvContent.begin(), csvContent.end(), '\n'));
    CHECK_TRUE(lineCount == fileRecords + 1);

    // 损坏尾部：复制一个文件并追加 10 字节残帧，读回完整记录并报告残留。
    const std::string corruptPath = dir + "/corrupt.qlog";
    fs::copy_file(files.back(), corruptPath);
    {
        std::ofstream out(corruptPath, std::ios::binary | std::ios::app);
        out << "partial-fr";
    }
    uint64_t corrupted = 0;
    const int64_t beforeCorrupt = svk::recorder::BinaryReader::readFile(
        files.back(), {},
        [](const svk::recorder::RecordView&) { return true; });
    const int64_t afterCorrupt = svk::recorder::BinaryReader::readFile(
        corruptPath, {},
        [](const svk::recorder::RecordView&) { return true; }, nullptr,
        &corrupted);
    CHECK_TRUE(afterCorrupt == beforeCorrupt);
    CHECK_TRUE(corrupted == 10);

    fs::remove_all(dir);
    std::printf("--- reader：roundtrip / 过滤 / 导出 / 容错校验完成 ---\n");
}

#if defined(__unix__) || defined(__APPLE__)
// mmap 崩溃保全：flush 进映射区的数据崩溃后仍在（内核写回脏页）；
// 之后进入内存缓冲但没落盘的 50 条必须丢——这就是崩溃语义的边界。
static void test_recorder_mmap_crash() {
    const std::string dir =
        (fs::temp_directory_path() / "svk_test_recorder_crash").string();
    fs::remove_all(dir);

    std::fflush(stdout);  // fork 前冲掉 stdio 缓冲，避免子进程重复输出
    const pid_t pid = fork();
    CHECK_TRUE(pid >= 0);
    if (pid == 0) {
        svk::recorder::BinaryRecorder recorder;
        svk::recorder::BinaryRecorder::Config config;
        config.directory = dir;
        config.filePrefix = "quotes";
        config.useMmap = true;
        config.flushBytes = 1 << 20;      // 不触发按量落盘
        config.flushIntervalMs = 600000;  // 不触发按时间落盘
        if (!recorder.start(config)) {
            _exit(2);
        }
        svk::log::installCrashHandler();
        for (int i = 0; i < 100; ++i) {
            const Tick tick = makeTick(i);
            recorder.write(1, tick);
        }
        recorder.flush();  // 100 条进入 mmap 映射区
        for (int i = 100; i < 150; ++i) {
            const Tick tick = makeTick(i);
            recorder.write(1, tick);
        }
        // 不 flush、不 stop，直接崩溃。
        std::raise(SIGSEGV);
        _exit(3);
    }
    int status = 0;
    CHECK_TRUE(waitpid(pid, &status, 0) == pid);
    CHECK_TRUE(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);

    const auto files = svk::recorder::BinaryReader::listFiles(dir, ".qlog");
    CHECK_TRUE(files.size() == 1);
    std::vector<uint64_t> seqs;
    svk::recorder::BinaryReader::readFile(
        files.front(), {},
        [&](const svk::recorder::RecordView& rec) {
            Tick tick{};
            std::memcpy(&tick, rec.payload, sizeof(Tick));
            seqs.push_back(tick.seq);
            return true;
        });
    CHECK_TRUE(seqs.size() == 100);
    for (size_t i = 0; i < seqs.size(); ++i) {
        CHECK_TRUE(seqs[i] == i);
    }
    fs::remove_all(dir);
    std::printf("--- recorder：mmap 崩溃数据保全校验完成（保全 %zu 条）---\n",
                seqs.size());
}

// mmap 扩容：写入总量超过 4MB 映射块触发 remap，数据仍完整可读。
static void test_recorder_mmap_grow() {
    const std::string dir =
        (fs::temp_directory_path() / "svk_test_recorder_grow").string();
    fs::remove_all(dir);

    svk::recorder::BinaryRecorder recorder;
    svk::recorder::BinaryRecorder::Config config;
    config.directory = dir;
    config.useMmap = true;
    config.bufferedBytesLimit = 8ull << 20;  // 大于本次写入总量，扩容测试不触发丢弃
    config.flushBytes = 256 << 10;
    config.flushIntervalMs = 50;
    config.maxFileBytes = 16ull << 20;  // 单文件够大，不轮转
    CHECK_TRUE(recorder.start(config));

    constexpr int kCount = 5000;
    const std::string payload(1000, 'g');  // 每帧约 1016B，共约 5MB > 4MB 块
    for (int i = 0; i < kCount; ++i) {
        recorder.write(7, payload.data(), payload.size());
    }
    recorder.flush();
    recorder.stop();
    CHECK_TRUE(recorder.stats().droppedRecords == 0);

    const auto files = svk::recorder::BinaryReader::listFiles(dir, ".qlog");
    CHECK_TRUE(files.size() == 1);
    int64_t readBack = svk::recorder::BinaryReader::readFile(
        files.front(), {},
        [](const svk::recorder::RecordView& rec) {
            CHECK_TRUE(rec.payloadBytes == 1000);
            return true;
        });
    CHECK_TRUE(readBack == kCount);
    fs::remove_all(dir);
    std::printf("--- recorder：mmap 扩容校验完成 ---\n");
}
#endif // __unix__ || __APPLE__

int main() {
    test_write_and_rotation();
    test_buffer_overflow_drops();
    test_prune_on_start();
    test_reader_roundtrip();
#if defined(__unix__) || defined(__APPLE__)
    test_recorder_mmap_crash();
    test_recorder_mmap_grow();
#endif

    if (g_failures > 0) {
        std::printf("\n%d 项 CHECK 失败\n", g_failures);
        return 1;
    }
    std::printf("\n全部 CHECK 通过\n");
    return 0;
}
