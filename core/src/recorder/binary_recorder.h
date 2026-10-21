#pragma once

/**
 * @file binary_recorder.h
 * @brief 高频业务数据的二进制录制器（设计见 docs/logging-design.md 第 7、8 节）。
 *
 * 定位：行情 tick 等高频数据是"数据录制"问题，不进诊断日志队列——文本化会
 * 打爆异步队列并把关键错误日志冲掉。本模块独立缓冲、独立文件、独立线程落盘。
 *
 * 文件格式（全部小端）：
 *   文件头 32B：magic u32 | formatVersion u16 | headerBytes u16 |
 *                sessionHash u64 | tradingDay u32 | tzOffsetMinutes i32 |
 *                reserved u64
 *   记录帧：length u32 | type u16 | flags u16 | timestampNs i64 | payload[length]
 *
 * 超上限防线（前三层在本模块，第四层降级策略由业务实现）：
 *   1. 内存缓冲满 → 覆盖最老帧，dropped 计数；
 *   2. 单文件满（maxFileBytes）→ rotation 切新文件；
 *   3. 目录总量/过期 → 启动与轮转时 LRU 清理；
 *   4. 大行情写入跟不上 → 业务读取 stats() 的 dropped 增长后自行降级采样。
 *
 * 使用方式：
 * @code{.cpp}
 * svk::recorder::BinaryRecorder recorder;
 * svk::recorder::BinaryRecorder::Config config;
 * config.directory = filesDir + "/quotes/20261021";
 * config.sessionId = svk::log::currentSessionId();
 * recorder.start(config);
 * recorder.write(kTickType, &tick, sizeof(tick));   // 业务线程
 * recorder.stop();
 * @endcode
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "log.h"

namespace svk {
namespace recorder {

namespace detail {

inline void appendU16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

inline void appendU32(std::vector<uint8_t>& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
    }
}

inline void appendU64(std::vector<uint8_t>& out, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
    }
}

inline uint64_t fnv1a64(const std::string& text) {
    uint64_t hash = 14695981039346656037ull;
    for (const char ch : text) {
        hash ^= static_cast<uint64_t>(static_cast<uint8_t>(ch));
        hash *= 1099511628211ull;
    }
    return hash;
}

/** 落盘写接口：⑤ 用 ofstream 实现，⑦ 增加 mmap 实现（崩溃保全）。 */
class IFileWriter {
public:
    virtual ~IFileWriter() = default;
    virtual bool open(const std::string& path) = 0;
    virtual bool write(const void* data, size_t bytes) = 0;
    virtual void flush() = 0;
    virtual void close() = 0;
    virtual size_t size() const = 0;
};

class OfstreamWriter final : public IFileWriter {
public:
    bool open(const std::string& path) override {
        out_.open(path, std::ios::binary | std::ios::trunc);
        size_ = 0;
        return out_.is_open();
    }

    bool write(const void* data, size_t bytes) override {
        out_.write(static_cast<const char*>(data),
                   static_cast<std::streamsize>(bytes));
        if (!out_) {
            return false;
        }
        size_ += bytes;
        return true;
    }

    void flush() override {
        if (out_.is_open()) {
            out_.flush();
        }
    }

    void close() override {
        if (out_.is_open()) {
            out_.flush();
            out_.close();
        }
    }

    size_t size() const override { return size_; }

private:
    std::ofstream out_;
    size_t size_ = 0;
};

} // namespace detail

class BinaryRecorder {
public:
    struct Config {
        std::string directory;
        std::string filePrefix = "records";
        std::string fileExtension = ".qlog";
        uint32_t magic = 0x53564B51;  // 'SVKQ'
        uint16_t formatVersion = 1;
        std::string sessionId;        // 与诊断日志同一 session，便于时间线对齐
        uint32_t tradingDay = 0;      // yyyymmdd；0 表示不填
        int32_t tzOffsetMinutes = 0;
        size_t bufferedBytesLimit = 1 << 20;   // 内存缓冲上限，满则覆盖最老
        size_t flushBytes = 64 << 10;          // 攒够即落盘
        int flushIntervalMs = 1000;            // 或到点落盘
        size_t maxFileBytes = 50ull << 20;     // 单文件上限，超出轮转
        size_t maxTotalBytes = 500ull << 20;   // 目录总量上限，0=不限制
        int maxFileAgeDays = 3;                // 过期删除，<=0 不按时间删
        int statsIntervalMs = 60000;           // recorder_stats 汇总周期
    };

    struct Stats {
        uint64_t writtenRecords = 0;
        uint64_t writtenBytes = 0;
        uint64_t droppedRecords = 0;
        uint64_t droppedBytes = 0;
        uint64_t filesCreated = 0;
        uint64_t filesDeleted = 0;
    };

    BinaryRecorder() = default;
    BinaryRecorder(const BinaryRecorder&) = delete;
    BinaryRecorder& operator=(const BinaryRecorder&) = delete;
    ~BinaryRecorder() { stop(); }

    /** 幂等：重复调用先 stop 再按新配置启动。 */
    bool start(const Config& config) {
        stop();
        if (config.directory.empty() || config.bufferedBytesLimit == 0) {
            return false;
        }
        std::error_code ec;
        std::filesystem::create_directories(config.directory, ec);
        if (ec) {
            return false;
        }
        config_ = config;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            frames_.clear();
            bufferedBytes_ = 0;
            stopRequested_ = false;
            flushReq_ = flushDone_ = 0;
        }
        fileSeq_ = 0;
        resetStats();
        pruneDirectory();
        if (!openNewFile()) {
            return false;
        }
        running_ = true;
        writer_ = std::thread([this] { writerLoop(); });
        SVK_LOGI("recorder", "recorder_started dir={} session={}",
                 config_.directory, config_.sessionId);
        return true;
    }

    void stop() {
        if (!running_) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopRequested_ = true;
            ++flushReq_;
        }
        cv_.notify_one();
        if (writer_.joinable()) {
            writer_.join();
        }
        if (fileWriter_) {
            fileWriter_->flush();
            fileWriter_->close();
            fileWriter_.reset();
        }
        running_ = false;
        SVK_LOGI("recorder", "recorder_stopped written={} dropped={}",
                 stats_.writtenRecords.load(), stats_.droppedRecords.load());
    }

    /** 业务线程调用：只写内存缓冲；缓冲满覆盖最老帧并计数。 */
    bool write(uint16_t type, const void* payload, size_t bytes) {
        if (!running_) {
            return false;
        }
        const size_t frameBytes = kFrameHeaderBytes + bytes;
        if (frameBytes > config_.bufferedBytesLimit) {
            stats_.droppedRecords.fetch_add(1, std::memory_order_relaxed);
            stats_.droppedBytes.fetch_add(frameBytes, std::memory_order_relaxed);
            return false;
        }
        std::vector<uint8_t> frame;
        frame.reserve(frameBytes);
        detail::appendU32(frame, static_cast<uint32_t>(bytes));
        detail::appendU16(frame, type);
        detail::appendU16(frame, 0);
        detail::appendU64(frame, static_cast<uint64_t>(nowNs()));
        const auto* data = static_cast<const uint8_t*>(payload);
        frame.insert(frame.end(), data, data + bytes);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            while (bufferedBytes_ + frameBytes > config_.bufferedBytesLimit &&
                   !frames_.empty()) {
                bufferedBytes_ -= frames_.front().size();
                stats_.droppedRecords.fetch_add(1, std::memory_order_relaxed);
                stats_.droppedBytes.fetch_add(frames_.front().size(),
                                              std::memory_order_relaxed);
                frames_.pop_front();
            }
            frames_.push_back(std::move(frame));
            bufferedBytes_ += frameBytes;
        }
        if (bufferedBytes_ >= config_.flushBytes) {
            cv_.notify_one();
        }
        return true;
    }

    /** POD 便捷写法：write(type, tick)。 */
    template <typename T>
    bool write(uint16_t type, const T& pod) {
        return write(type, &pod, sizeof(T));
    }

    /** 让写盘线程把当前缓冲写完并 flush 文件后返回。 */
    void flush() {
        if (!running_) {
            return;
        }
        uint64_t gen = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            gen = ++flushReq_;
        }
        cv_.notify_one();
        std::unique_lock<std::mutex> lock(mutex_);
        cvDone_.wait(lock, [&] { return flushDone_ >= gen; });
    }

    Stats stats() const {
        Stats snapshot;
        snapshot.writtenRecords = stats_.writtenRecords.load();
        snapshot.writtenBytes = stats_.writtenBytes.load();
        snapshot.droppedRecords = stats_.droppedRecords.load();
        snapshot.droppedBytes = stats_.droppedBytes.load();
        snapshot.filesCreated = stats_.filesCreated.load();
        snapshot.filesDeleted = stats_.filesDeleted.load();
        return snapshot;
    }

    std::string currentFilePath() const { return currentFilePath_; }

    /** 缓冲是否正在丢弃（业务可据此降级采样，即第四层防线）。 */
    bool isDropping() const {
        return stats_.droppedRecords.load(std::memory_order_relaxed) > 0;
    }

private:
    static constexpr size_t kFileHeaderBytes = 32;
    static constexpr size_t kFrameHeaderBytes = 16;

    struct AtomicStats {
        std::atomic<uint64_t> writtenRecords{0};
        std::atomic<uint64_t> writtenBytes{0};
        std::atomic<uint64_t> droppedRecords{0};
        std::atomic<uint64_t> droppedBytes{0};
        std::atomic<uint64_t> filesCreated{0};
        std::atomic<uint64_t> filesDeleted{0};
    };

    void resetStats() {
        stats_.writtenRecords.store(0);
        stats_.writtenBytes.store(0);
        stats_.droppedRecords.store(0);
        stats_.droppedBytes.store(0);
        stats_.filesCreated.store(0);
        stats_.filesDeleted.store(0);
    }

    static int64_t nowNs() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    std::string makeFilePath() {
        const auto now = std::chrono::system_clock::now();
        const auto millis =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()) %
            1000;
        const std::time_t time = std::chrono::system_clock::to_time_t(now);
        std::tm local{};
#if defined(_WIN32)
        localtime_s(&local, &time);
#else
        localtime_r(&time, &local);
#endif
        char name[112];
        std::snprintf(name, sizeof(name),
                      "%s-%04d%02d%02d-%02d%02d%02d-%03d-%03llu%s",
                      config_.filePrefix.c_str(), local.tm_year + 1900,
                      local.tm_mon + 1, local.tm_mday, local.tm_hour,
                      local.tm_min, local.tm_sec,
                      static_cast<int>(millis.count()),
                      static_cast<unsigned long long>(fileSeq_++ % 1000),
                      config_.fileExtension.c_str());
        if (!config_.directory.empty() && config_.directory.back() == '/') {
            return config_.directory + name;
        }
        return config_.directory + "/" + name;
    }

    bool openNewFile() {
        auto writer = std::make_unique<detail::OfstreamWriter>();
        const std::string path = makeFilePath();
        if (!writer->open(path)) {
            SVK_LOGE("recorder", "recorder_open_failed path={}", path);
            return false;
        }
        std::vector<uint8_t> header;
        header.reserve(kFileHeaderBytes);
        detail::appendU32(header, config_.magic);
        detail::appendU16(header, config_.formatVersion);
        detail::appendU16(header, static_cast<uint16_t>(kFileHeaderBytes));
        detail::appendU64(header, detail::fnv1a64(config_.sessionId));
        detail::appendU32(header, config_.tradingDay);
        detail::appendU32(header,
                          static_cast<uint32_t>(config_.tzOffsetMinutes));
        detail::appendU64(header, 0);
        if (!writer->write(header.data(), header.size())) {
            return false;
        }
        fileWriter_ = std::move(writer);
        currentFilePath_ = path;
        stats_.filesCreated.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    void pruneDirectory() {
        uint64_t files = 0;
        uint64_t bytes = 0;
        svk::log::detail::pruneLogDirectory(config_.directory,
                                            config_.filePrefix,
                                            config_.maxTotalBytes,
                                            config_.maxFileAgeDays, &files,
                                            &bytes);
        if (files > 0) {
            stats_.filesDeleted.fetch_add(files, std::memory_order_relaxed);
            SVK_LOGI("recorder", "recorder_pruned files={} bytes={}", files,
                     bytes);
        }
    }

    void writeBatch(const std::deque<std::vector<uint8_t>>& batch) {
        for (const auto& frame : batch) {
            // 只含文件头的空文件不轮转，避免 maxFileBytes 过小时空转。
            if (fileWriter_->size() > kFileHeaderBytes &&
                fileWriter_->size() >= config_.maxFileBytes) {
                rotate();
            }
            if (!fileWriter_->write(frame.data(), frame.size())) {
                stats_.droppedRecords.fetch_add(1, std::memory_order_relaxed);
                stats_.droppedBytes.fetch_add(frame.size(),
                                              std::memory_order_relaxed);
                continue;
            }
            stats_.writtenRecords.fetch_add(1, std::memory_order_relaxed);
            stats_.writtenBytes.fetch_add(frame.size(),
                                          std::memory_order_relaxed);
        }
    }

    void rotate() {
        fileWriter_->flush();
        fileWriter_->close();
        pruneDirectory();
        openNewFile();
    }

    void maybeLogStats() {
        SVK_LOGI_THROTTLE("recorder", config_.statsIntervalMs,
                          "recorder_stats written={} written_bytes={} "
                          "dropped={} dropped_bytes={} files={}",
                          stats_.writtenRecords.load(),
                          stats_.writtenBytes.load(),
                          stats_.droppedRecords.load(),
                          stats_.droppedBytes.load(),
                          stats_.filesCreated.load());
    }

    void writerLoop() {
        for (;;) {
            std::deque<std::vector<uint8_t>> batch;
            uint64_t reqAtStart = 0;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait_for(
                    lock, std::chrono::milliseconds(config_.flushIntervalMs),
                    [&] {
                        return stopRequested_ || flushReq_ != flushDone_ ||
                               bufferedBytes_ >= config_.flushBytes;
                    });
                reqAtStart = flushReq_;
                batch.swap(frames_);
                bufferedBytes_ = 0;
            }
            if (!batch.empty()) {
                writeBatch(batch);
            }
            if (reqAtStart != flushDone_) {
                fileWriter_->flush();
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    flushDone_ = reqAtStart;
                }
                cvDone_.notify_all();
            }
            maybeLogStats();
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopRequested_ && frames_.empty()) {
                break;
            }
        }
    }

    Config config_;
    AtomicStats stats_;
    std::atomic<bool> running_{false};
    std::thread writer_;
    std::unique_ptr<detail::IFileWriter> fileWriter_;
    std::string currentFilePath_;
    uint64_t fileSeq_ = 0;  // 文件名序号：同一毫秒内连续轮转也不重名

    std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable cvDone_;
    std::deque<std::vector<uint8_t>> frames_;
    size_t bufferedBytes_ = 0;
    bool stopRequested_ = false;
    uint64_t flushReq_ = 0;
    uint64_t flushDone_ = 0;
};

} // namespace recorder
} // namespace svk
