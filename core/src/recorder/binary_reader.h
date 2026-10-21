#pragma once

/**
 * @file binary_reader.h
 * @brief 读取 BinaryRecorder 录制的 .qlog 文件：校验文件头、逐帧遍历、
 * 按时间与类型过滤、导出 CSV（设计见 docs/logging-design.md 第 7.5 节）。
 *
 * 文件格式与编码约定见 binary_recorder.h 头部注释（全部小端）。
 *
 * 使用方式：
 * @code{.cpp}
 * svk::recorder::RecordFilter filter;
 * filter.types = {1};  // 只要 tick
 * svk::recorder::BinaryReader::readFile(path, filter,
 *     [](const svk::recorder::RecordView& rec) {
 *         // rec.payload / rec.payloadBytes / rec.timestampNs / rec.type
 *         return true;  // 返回 false 提前停止
 *     });
 * @endcode
 */

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace svk {
namespace recorder {

/** 文件头解析结果（对应 recorder 写入的 32B 头）。 */
struct FileHeaderView {
    uint32_t magic = 0;
    uint16_t formatVersion = 0;
    uint16_t headerBytes = 0;
    uint64_t sessionHash = 0;
    uint32_t tradingDay = 0;
    int32_t tzOffsetMinutes = 0;
};

/** 单条记录视图；payload 只在回调期间有效。 */
struct RecordView {
    uint16_t type = 0;
    uint16_t flags = 0;
    int64_t timestampNs = 0;
    const uint8_t* payload = nullptr;
    size_t payloadBytes = 0;
};

/** 记录过滤：时间闭区间 + 类型白名单（空 = 全部类型）。 */
struct RecordFilter {
    int64_t tsFromNs = INT64_MIN;
    int64_t tsToNs = INT64_MAX;
    std::vector<uint16_t> types;

    bool matches(uint16_t type, int64_t timestampNs) const {
        if (timestampNs < tsFromNs || timestampNs > tsToNs) {
            return false;
        }
        if (!types.empty() &&
            std::find(types.begin(), types.end(), type) == types.end()) {
            return false;
        }
        return true;
    }
};

class BinaryReader {
public:
    /**
     * @brief 列出目录内指定扩展名的录制文件，按文件名升序
     * （时间戳+序号命名，字典序即创建顺序）。
     */
    static std::vector<std::string> listFiles(const std::string& directory,
                                              const std::string& extension) {
        namespace fs = std::filesystem;
        std::vector<std::string> files;
        std::error_code ec;
        if (!fs::is_directory(directory, ec)) {
            return files;
        }
        for (const auto& entry : fs::directory_iterator(directory, ec)) {
            if (entry.is_regular_file(ec) &&
                entry.path().extension() == extension) {
                files.push_back(entry.path().string());
            }
        }
        std::sort(files.begin(), files.end());
        return files;
    }

    /** @brief 只解析文件头；magic 不符或文件过短返回 false。 */
    static bool readHeader(const std::string& path, FileHeaderView* out) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            return false;
        }
        uint8_t header[kFileHeaderBytes] = {};
        in.read(reinterpret_cast<char*>(header), sizeof(header));
        if (in.gcount() < static_cast<std::streamsize>(sizeof(header))) {
            return false;
        }
        const FileHeaderView view{
            readU32(header + 0),
            readU16(header + 4),
            readU16(header + 6),
            readU64(header + 8),
            readU32(header + 16),
            static_cast<int32_t>(readU32(header + 20)),
        };
        if (view.magic == 0 || view.headerBytes < kFileHeaderBytes) {
            return false;
        }
        if (out) {
            *out = view;
        }
        return true;
    }

    /**
     * @brief 逐帧遍历文件，按 filter 过滤后回调。
     * @return 通过过滤并回调的记录数；-1 表示文件无法读取或 magic 不符。
     * onRecord 返回 false 提前停止（仍返回已回调的条数）。
     * 尾部帧不完整（崩溃截断）时停止遍历，corruptedTailBytes 带回残留字节数。
     */
    static int64_t readFile(
        const std::string& path, const RecordFilter& filter,
        const std::function<bool(const RecordView&)>& onRecord,
        FileHeaderView* headerOut = nullptr,
        uint64_t* corruptedTailBytes = nullptr) {
        FileHeaderView header;
        if (!readHeader(path, &header)) {
            return -1;
        }
        if (headerOut) {
            *headerOut = header;
        }
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in) {
            return -1;
        }
        const auto fileBytes = static_cast<uint64_t>(in.tellg());
        in.seekg(header.headerBytes);

        int64_t matched = 0;
        uint64_t corrupted = 0;
        std::vector<uint8_t> payload;
        for (;;) {
            const auto frameStart = static_cast<uint64_t>(in.tellg());
            uint8_t frameHeader[kFrameHeaderBytes] = {};
            in.read(reinterpret_cast<char*>(frameHeader), sizeof(frameHeader));
            const auto headerGot = in.gcount();
            if (headerGot == 0) {
                break;  // 正常 EOF
            }
            if (headerGot <
                static_cast<std::streamsize>(sizeof(frameHeader))) {
                corrupted = fileBytes - frameStart;  // 半个帧头：崩溃截断
                break;
            }
            const uint32_t length = readU32(frameHeader + 0);
            const RecordView view{
                readU16(frameHeader + 4),
                readU16(frameHeader + 6),
                static_cast<int64_t>(readU64(frameHeader + 8)),
                nullptr,
                length,
            };
            if (frameStart + kFrameHeaderBytes + length > fileBytes) {
                corrupted = fileBytes - frameStart;  // 尾部不完整帧
                break;
            }
            payload.resize(length);
            in.read(reinterpret_cast<char*>(payload.data()),
                    static_cast<std::streamsize>(length));
            if (in.gcount() < static_cast<std::streamsize>(length)) {
                corrupted = fileBytes - frameStart;
                break;
            }
            if (!filter.matches(view.type, view.timestampNs)) {
                continue;
            }
            RecordView complete = view;
            complete.payload = payload.data();
            ++matched;
            if (!onRecord(complete)) {
                break;
            }
        }
        if (corruptedTailBytes) {
            *corruptedTailBytes = corrupted;
        }
        return matched;
    }

    /**
     * @brief 导出 CSV：`timestamp_ns,type,<formatPayload 返回值>`。
     * payload 的字段拆分与转义由 formatPayload 负责。
     */
    static bool exportCsv(
        const std::string& path, const RecordFilter& filter,
        const std::string& outPath,
        const std::function<std::string(const RecordView&)>& formatPayload) {
        std::ofstream out(outPath, std::ios::trunc);
        if (!out) {
            return false;
        }
        out << "timestamp_ns,type,payload\n";
        const int64_t matched =
            readFile(path, filter,
                     [&](const RecordView& rec) {
                         out << rec.timestampNs << ',' << rec.type << ','
                             << formatPayload(rec) << '\n';
                         return true;
                     });
        out.flush();
        return matched >= 0 && out.good();
    }

private:
    static constexpr size_t kFileHeaderBytes = 32;
    static constexpr size_t kFrameHeaderBytes = 16;

    static uint16_t readU16(const uint8_t* p) {
        return static_cast<uint16_t>(p[0] | (p[1] << 8));
    }
    static uint32_t readU32(const uint8_t* p) {
        return static_cast<uint32_t>(p[0]) |
               (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16) |
               (static_cast<uint32_t>(p[3]) << 24);
    }
    static uint64_t readU64(const uint8_t* p) {
        uint64_t value = 0;
        for (int i = 0; i < 8; ++i) {
            value |= static_cast<uint64_t>(p[i]) << (i * 8);
        }
        return value;
    }
};

} // namespace recorder
} // namespace svk
