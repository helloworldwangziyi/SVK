/**
 * @file test_crypto.cpp
 * @brief 合规能力回归：ChaCha20 标准测试向量、敏感字段脱敏、
 * 落盘加密 roundtrip、日志归集。
 *
 * 运行：./build/host/test/test_crypto，或 cd build/host && ctest -R test_crypto
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "log/log.h"
#include "log/log_crypto.h"

static int g_failures = 0;

#define CHECK_TRUE(cond) \
    do { \
        if (!(cond)) { \
            ++g_failures; \
            std::printf("CHECK 失败: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        } \
    } while (0)

namespace fs = std::filesystem;

static std::string readFileToString(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// RFC 8439 §2.4.2：key=00..1f、nonce=000000090000004a00000000、counter=1
// 时密钥流前 16 字节为 10f1e7e4d13b5915500fdd1fa32071c4。
static void test_chacha20_rfc_vector() {
    uint8_t key[32];
    for (int i = 0; i < 32; ++i) {
        key[i] = static_cast<uint8_t>(i);
    }
    const uint8_t nonce[12] = {0x00, 0x00, 0x00, 0x09, 0x00, 0x00,
                               0x00, 0x4a, 0x00, 0x00, 0x00, 0x00};
    svk::log::detail::ChaCha20 cipher;
    cipher.init(key, nonce, 1);
    const uint8_t zeros[16] = {};
    uint8_t out[16];
    cipher.crypt(zeros, out, sizeof(out));
    const uint8_t expect[16] = {0x10, 0xf1, 0xe7, 0xe4, 0xd1, 0x3b,
                                0x59, 0x15, 0x50, 0x0f, 0xdd, 0x1f,
                                0xa3, 0x20, 0x71, 0xc4};
    CHECK_TRUE(std::memcmp(out, expect, sizeof(out)) == 0);

    // 流连续性：分段 crypt 与一次 crypt 结果一致。
    svk::log::detail::ChaCha20 a;
    svk::log::detail::ChaCha20 b;
    a.init(key, nonce, 1);
    b.init(key, nonce, 1);
    const uint8_t plain[100] = {};
    uint8_t oneShot[100];
    uint8_t split[100];
    a.crypt(plain, oneShot, sizeof(oneShot));
    b.crypt(plain, split, 37);
    b.crypt(plain + 37, split + 37, sizeof(split) - 37);
    CHECK_TRUE(std::memcmp(oneShot, split, sizeof(oneShot)) == 0);
    std::printf("--- chacha20：RFC 8439 向量与流连续性校验完成 ---\n");
}

static void test_masking() {
    using svk::log::detail::maskSensitiveInPlace;
    std::string line =
        "login phone=13812345678 token=ab12cd34ef56 note=visible cvv=123";
    maskSensitiveInPlace(line);
    CHECK_TRUE(line.find("13812345678") == std::string::npos);
    CHECK_TRUE(line.find("phone=138******78") != std::string::npos);
    CHECK_TRUE(line.find("token=ab1*******56") != std::string::npos);
    CHECK_TRUE(line.find("cvv=***") == std::string::npos);  // cvv 不在清单
    CHECK_TRUE(line.find("note=visible") != std::string::npos);

    // "_key" 后缀命中：access_token 应被掩码。
    std::string suffix = "auth access_token=zzz999000111 ok=1";
    maskSensitiveInPlace(suffix);
    CHECK_TRUE(suffix.find("zzz999000111") == std::string::npos);
    CHECK_TRUE(suffix.find("ok=1") != std::string::npos);

    // 开关关闭时不动正文。
    svk::log::setMaskingEnabled(false);
    std::string off = "phone=13812345678";
    maskSensitiveInPlace(off);
    CHECK_TRUE(off.find("13812345678") != std::string::npos);
    svk::log::setMaskingEnabled(true);
    std::printf("--- 脱敏：key=value 掩码规则校验完成 ---\n");
}

// 加密落盘：原文不含明文标记，decryptFileToFile 还原后可见（且已脱敏）。
static void test_encrypted_sink_roundtrip() {
    const std::string dir = (fs::temp_directory_path() / "svk_test_crypto").string();
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string keyHex =
        "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";

    svk::log::init("svk_test_crypto");
    svk::log::LogFileConfig config;
    config.directory = dir;
    config.filePrefix = "test-enc";
    config.async = false;
    config.fileEncryptionKeyHex = keyHex;
    CHECK_TRUE(svk::log::initWithFile(spdlog::get("svk_test_crypto"), config));
    SVK_LOGI("practice", "secret_marker phone=13812345678 value={}", 42);
    svk::log::flush();
    const std::string encPath = svk::log::currentLogFilePath();
    svk::log::shutdown();

    const std::string raw = readFileToString(encPath);
    CHECK_TRUE(raw.rfind("SVKENC01", 0) == 0);          // 明文头只有 magic+nonce
    CHECK_TRUE(raw.find("secret_marker") == std::string::npos);

    const std::string decPath = dir + "/decrypted.log";
    CHECK_TRUE(svk::log::decryptFileToFile(encPath, decPath, keyHex));
    const std::string plain = readFileToString(decPath);
    CHECK_TRUE(plain.find("secret_marker") != std::string::npos);
    // 加密与脱敏正交：脱敏在格式化后、加密前发生，解密可见掩码结果。
    CHECK_TRUE(plain.find("13812345678") == std::string::npos);
    CHECK_TRUE(plain.find("phone=138******78") != std::string::npos);

    fs::remove_all(dir);
    std::printf("--- 加密落盘：密文/解密/脱敏叠加校验完成 ---\n");
}

static void test_collect() {
    const std::string dir = (fs::temp_directory_path() / "svk_test_collect_src").string();
    const std::string dest = (fs::temp_directory_path() / "svk_test_collect_dest").string();
    fs::remove_all(dir);
    fs::remove_all(dest);
    fs::create_directories(dir);
    {
        std::ofstream(dir + "/a-20240101-000000-000.log") << "aaa";
        std::ofstream(dir + "/b-20240102-000000-000.log") << "bbbb";
        std::ofstream(dir + "/c.txt") << "not a log";
    }
    // b 文件回拨到 3 天前；只收最近 1 天 → 只剩 a。
    std::error_code ec;
    fs::last_write_time(dir + "/b-20240102-000000-000.log",
                        fs::file_time_type::clock::now() -
                            std::chrono::hours(24LL * 3),
                        ec);

    const int64_t nowMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    svk::log::LogCollectOptions options;
    options.directory = dir;
    options.destDir = dest;
    options.mtimeFromMs = nowMs - 24LL * 3600 * 1000;
    options.mtimeToMs = nowMs + 1000;
    options.manifestNote = "feedback=FB-123";
    std::string err;
    const size_t copied = svk::log::collectLogFiles(options, &err);
    CHECK_TRUE(copied == 1);
    CHECK_TRUE(fs::exists(dest + "/a-20240101-000000-000.log"));
    CHECK_TRUE(!fs::exists(dest + "/b-20240102-000000-000.log"));
    const std::string manifest = readFileToString(dest + "/manifest.txt");
    CHECK_TRUE(manifest.find("feedback=FB-123") != std::string::npos);
    CHECK_TRUE(manifest.find("files=1") != std::string::npos);

    fs::remove_all(dir);
    fs::remove_all(dest);
    std::printf("--- 日志归集：时间窗过滤与 manifest 校验完成 ---\n");
}

int main() {
    test_chacha20_rfc_vector();
    test_masking();
    test_encrypted_sink_roundtrip();
    test_collect();

    if (g_failures > 0) {
        std::printf("\n%d 项 CHECK 失败\n", g_failures);
        return 1;
    }
    std::printf("\n全部 CHECK 通过\n");
    return 0;
}
