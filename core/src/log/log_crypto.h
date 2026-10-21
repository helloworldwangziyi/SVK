#pragma once

/**
 * @file log_crypto.h
 * @brief 日志落盘加密（设计见 docs/logging-design.md 第 9 节）。
 *
 * 定位说明（重要）：这是"防明文检索"的混淆级加密——密钥在客户端，可被逆向，
 * 不等于端到端安全。目的是日志文件不落明文账号/持仓/委托信息，过安全检测
 * 与防止日志外泄直接可读。算法为公开的 ChaCha20 流加密（RFC 8439）。
 *
 * 加密文件格式：8B magic "SVKENC01" + 12B 随机 nonce（明文），
 * 其后全部内容为 ChaCha20 密钥流 XOR 密文。同一把密钥每个文件随机 nonce，
 * 密钥流不复用。开启加密后文件 sink 不做轮转（依赖每次冷启动新文件 +
 * initWithFile 的过期/总量清理兜底）。
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include <spdlog/common.h>
#include <spdlog/details/log_msg.h>
#include <spdlog/sinks/base_sink.h>

namespace svk {
namespace log {

/** 64 个十六进制字符 → 32 字节密钥；格式不符返回 false。 */
inline bool hexToKey(const std::string& keyHex, uint8_t keyOut[32]) {
    if (keyHex.size() != 64) {
        return false;
    }
    auto nibble = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };
    for (int i = 0; i < 32; ++i) {
        const int hi = nibble(keyHex[i * 2]);
        const int lo = nibble(keyHex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        keyOut[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

namespace detail {

/** ChaCha20 流加密（RFC 8439）：crypt 可连续调用，密钥流位置自动前进。 */
class ChaCha20 {
public:
    void init(const uint8_t key[32], const uint8_t nonce[12],
              uint32_t counter) {
        state_[0] = 0x61707865;
        state_[1] = 0x3320646e;
        state_[2] = 0x79622d32;
        state_[3] = 0x6b206574;
        for (int i = 0; i < 8; ++i) {
            state_[4 + i] = readLE32(key + i * 4);
        }
        state_[12] = counter;
        for (int i = 0; i < 3; ++i) {
            state_[13 + i] = readLE32(nonce + i * 4);
        }
        keystreamUsed_ = sizeof(keystream_);
    }

    void crypt(const uint8_t* in, uint8_t* out, size_t bytes) {
        for (size_t i = 0; i < bytes; ++i) {
            if (keystreamUsed_ == sizeof(keystream_)) {
                generateBlock();
            }
            out[i] = static_cast<uint8_t>(in[i] ^ keystream_[keystreamUsed_++]);
        }
    }

private:
    static uint32_t readLE32(const uint8_t* p) {
        return static_cast<uint32_t>(p[0]) |
               (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16) |
               (static_cast<uint32_t>(p[3]) << 24);
    }

    static uint32_t rotl(uint32_t v, int bits) {
        return (v << bits) | (v >> (32 - bits));
    }

    static void quarterRound(uint32_t& a, uint32_t& b, uint32_t& c,
                             uint32_t& d) {
        a += b; d ^= a; d = rotl(d, 16);
        c += d; b ^= c; b = rotl(b, 12);
        a += b; d ^= a; d = rotl(d, 8);
        c += d; b ^= c; b = rotl(b, 7);
    }

    void generateBlock() {
        uint32_t working[16];
        std::memcpy(working, state_, sizeof(working));
        for (int i = 0; i < 10; ++i) {
            quarterRound(working[0], working[4], working[8], working[12]);
            quarterRound(working[1], working[5], working[9], working[13]);
            quarterRound(working[2], working[6], working[10], working[14]);
            quarterRound(working[3], working[7], working[11], working[15]);
            quarterRound(working[0], working[5], working[10], working[15]);
            quarterRound(working[1], working[6], working[11], working[12]);
            quarterRound(working[2], working[7], working[8], working[13]);
            quarterRound(working[3], working[4], working[9], working[14]);
        }
        for (int i = 0; i < 16; ++i) {
            const uint32_t value = working[i] + state_[i];
            keystream_[i * 4 + 0] = static_cast<uint8_t>(value & 0xFF);
            keystream_[i * 4 + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
            keystream_[i * 4 + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
            keystream_[i * 4 + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
        }
        if (++state_[12] == 0) {
            ++state_[13];
        }
        keystreamUsed_ = 0;
    }

    uint32_t state_[16] = {};
    uint8_t keystream_[64] = {};
    size_t keystreamUsed_ = sizeof(keystream_);
};

} // namespace detail

/**
 * 加密文件 sink：格式化后的整条消息经 ChaCha20 加密落盘。
 * 文件头 20B 明文（magic + nonce），其余全是密文。
 * 不做轮转（见文件头注释）；抛 spdlog_ex 与 spdlog 文件 sink 行为一致。
 */
class EncryptingFileSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    explicit EncryptingFileSink(const std::string& path,
                                const uint8_t key[32]) {
        file_ = std::fopen(path.c_str(), "wb");
        if (file_ == nullptr) {
            throw spdlog::spdlog_ex("Failed to open encrypted log file: " + path);
        }
        uint8_t nonce[12];
        std::random_device rd;
        for (auto& byte : nonce) {
            byte = static_cast<uint8_t>(rd());
        }
        std::fwrite(kMagic, 1, sizeof(kMagic), file_);
        std::fwrite(nonce, 1, sizeof(nonce), file_);
        cipher_.init(key, nonce, 0);
    }

    ~EncryptingFileSink() override {
        if (file_ != nullptr) {
            std::fclose(file_);
        }
    }

    static constexpr size_t kPlainHeaderBytes = 20;  // magic 8 + nonce 12

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        spdlog::memory_buf_t formatted;
        formatter_->format(msg, formatted);
        buffer_.resize(formatted.size());
        if (formatted.size() > 0) {
            cipher_.crypt(
                reinterpret_cast<const uint8_t*>(formatted.data()),
                buffer_.data(), formatted.size());
            std::fwrite(buffer_.data(), 1, buffer_.size(), file_);
        }
    }

    void flush_() override {
        if (file_ != nullptr) {
            std::fflush(file_);
        }
    }

private:
    static constexpr char kMagic[8] = {'S', 'V', 'K', 'E', 'N', 'C', '0', '1'};

    FILE* file_ = nullptr;
    detail::ChaCha20 cipher_;
    std::vector<uint8_t> buffer_;
};

/** 解密工具：加密日志文件 → 明文文件，供回捞后在服务端/工具侧还原。 */
inline bool decryptFileToFile(const std::string& inPath,
                              const std::string& outPath,
                              const std::string& keyHex) {
    uint8_t key[32];
    if (!hexToKey(keyHex, key)) {
        return false;
    }
    FILE* in = std::fopen(inPath.c_str(), "rb");
    if (in == nullptr) {
        return false;
    }
    uint8_t header[EncryptingFileSink::kPlainHeaderBytes] = {};
    if (std::fread(header, 1, sizeof(header), in) != sizeof(header) ||
        std::memcmp(header, "SVKENC01", 8) != 0) {
        std::fclose(in);
        return false;
    }
    FILE* out = std::fopen(outPath.c_str(), "wb");
    if (out == nullptr) {
        std::fclose(in);
        return false;
    }
    detail::ChaCha20 cipher;
    cipher.init(key, header + 8, 0);
    std::vector<uint8_t> inBuf(64 << 10);
    std::vector<uint8_t> outBuf(inBuf.size());
    size_t got = 0;
    while ((got = std::fread(inBuf.data(), 1, inBuf.size(), in)) > 0) {
        cipher.crypt(inBuf.data(), outBuf.data(), got);
        std::fwrite(outBuf.data(), 1, got, out);
    }
    std::fclose(in);
    std::fclose(out);
    return true;
}

} // namespace log
} // namespace svk
