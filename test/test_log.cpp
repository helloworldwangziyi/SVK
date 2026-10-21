/**
 * @file test_log.cpp
 * @brief 日志练习与回归：终端输出与耗时统计、限速宏行为、审计通道文件分离。
 *
 * 运行：./build/host/test/test_log，或 cd build/host && ctest -R test_log
 */

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "log.h"

static int g_failures = 0;

#define CHECK_TRUE(cond) \
    do { \
        if (!(cond)) { \
            ++g_failures; \
            std::printf("CHECK 失败: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        } \
    } while (0)

static std::string readFileToString(const std::string& path) {
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static std::string findFileWithPrefix(const std::string& dir,
                                      const std::string& prefix) {
    namespace fs = std::filesystem;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.is_regular_file() &&
            entry.path().filename().string().rfind(prefix, 0) == 0) {
            return entry.path().string();
        }
    }
    return "";
}

static size_t countFilesWithPrefix(const std::string& dir,
                                   const std::string& prefix) {
    namespace fs = std::filesystem;
    size_t count = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.is_regular_file() &&
            entry.path().filename().string().rfind(prefix, 0) == 0) {
            ++count;
        }
    }
    return count;
}

static void writeFakeFile(const std::string& path, size_t bytes, int ageDays) {
    namespace fs = std::filesystem;
    std::ofstream out(path);
    out << std::string(bytes, 'x');
    out.close();
    if (ageDays > 0) {
        std::error_code ec;
        fs::last_write_time(path, fs::file_time_type::clock::now() -
                                      std::chrono::hours(24LL * ageDays),
                            ec);
    }
}

void test_spdlog_base_use(){
    svk::log::init("svk_test_base_use");

    SVK_LOGD("practice", "level_debug value={}", 1);
    SVK_LOGI("practice", "level_info value={}", 2);
    SVK_LOGW("practice", "level_warn value={}", 3);
    SVK_LOGE("practice", "level_err value={}", 4);

    // 统计循环写 100万 条的耗时。终端 I/O 是主要开销，这本身就是第一个观察点。
    constexpr int kCount = 1000000;
    const auto begin = std::chrono::steady_clock::now();
    for(int i = 0; i < kCount; ++i){
        SVK_LOGI("practice", "timing seq={} value={}", i, i * 2);
    }
    const auto end = std::chrono::steady_clock::now();

    const double ms = std::chrono::duration<double, std::milli>(end - begin).count();

    std::printf("\n%d 条日志，耗时 %.1f ms，约 %.0f 条/秒\n", kCount, ms, kCount / (ms / 1000.0));

    svk::log::shutdown();
}

void test_log_throttle(){
    svk::log::init("svk_test_throttle");

    // ONCE：同一调用点只写第一条，i=1/2 被丢弃。
    std::printf("\n--- ONCE：循环 3 次只应看到 i=0 一条 ---\n");
    for(int i = 0; i < 3; ++i){
        SVK_LOGI_ONCE("practice", "once_event i={}", i);
    }

    // THROTTLE：100ms 窗口内连写 5 次只放行第 1 条；同一调用点越过窗口后
    // 再次执行，放行并补 suppressed=4。限速状态跟随调用点，所以用 lambda
    // 保证两次触发的是同一处宏展开。
    std::printf("--- THROTTLE：连写 5 次只放行第 1 条，sleep 后同调用点放行并带 suppressed=4 ---\n");
    auto heartbeat = [&](int round){
        SVK_LOGI_THROTTLE("practice", 100, "throttle_event round={}", round);
    };
    for(int i = 0; i < 5; ++i){
        heartbeat(i);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    heartbeat(5);

    // THROTTLE_KEY：ag2406 与 rb2406 各自独立限速，互不影响。
    std::printf("--- THROTTLE_KEY：两个合约各自限速，sleep 后各自带 suppressed=2 ---\n");
    for(int i = 0; i < 3; ++i){
        SVK_LOGW_THROTTLE_KEY("quote", "ag2406", 100,
                              "seq_gap expect={} got={}", 10 + i, 11 + i);
        SVK_LOGW_THROTTLE_KEY("quote", "rb2406", 100,
                              "seq_gap expect={} got={}", 20 + i, 22 + i);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    SVK_LOGW_THROTTLE_KEY("quote", "ag2406", 100, "seq_gap expect=13 got=14");
    SVK_LOGW_THROTTLE_KEY("quote", "rb2406", 100, "seq_gap expect=23 got=25");

    svk::log::shutdown();
}

void test_log_audit_channel(){
    namespace fs = std::filesystem;
    const std::string dir =
        (fs::temp_directory_path() / "svk_test_audit_channel").string();
    fs::remove_all(dir);
    fs::create_directories(dir);

    svk::log::init("svk_test_audit");
    svk::log::LogFileConfig config;
    config.directory = dir;
    config.filePrefix = "test-main";
    config.auditFilePrefix = "test-audit";
    const bool ok = svk::log::initWithFile(spdlog::get("svk_test_audit"), config);
    CHECK_TRUE(ok);

    SVK_LOGI("practice", "engine_line_marker value={}", 1);
    SVK_ALOGI("trade", "order_submit instrument=ag2406 price={}", 3450.5);
    SVK_ALOGW("account", "login_done user=u1001");

    svk::log::flush();
    CHECK_TRUE(!svk::log::currentAuditLogFilePath().empty());
    svk::log::shutdown();

    // 主文件有引擎日志、没有审计日志；审计文件恰好相反。
    const std::string mainPath = findFileWithPrefix(dir, "test-main");
    const std::string auditPath = findFileWithPrefix(dir, "test-audit");
    CHECK_TRUE(!mainPath.empty());
    CHECK_TRUE(!auditPath.empty());
    const std::string mainContent = readFileToString(mainPath);
    const std::string auditContent = readFileToString(auditPath);
    CHECK_TRUE(mainContent.find("engine_line_marker") != std::string::npos);
    CHECK_TRUE(mainContent.find("order_submit") == std::string::npos);
    CHECK_TRUE(auditContent.find("order_submit") != std::string::npos);
    CHECK_TRUE(auditContent.find("login_done") != std::string::npos);
    CHECK_TRUE(auditContent.find("engine_line_marker") == std::string::npos);

    fs::remove_all(dir);
    std::printf("--- 审计通道：主/审计文件分离校验完成 ---\n");
}

void test_log_rotation_prune(){
    namespace fs = std::filesystem;

    // 轮转：单文件 1KB 上限，写约 4KB 应切出 name.N.log。
    {
        const std::string dir =
            (fs::temp_directory_path() / "svk_test_rotation").string();
        fs::remove_all(dir);
        fs::create_directories(dir);
        svk::log::init("svk_test_rotation");
        svk::log::LogFileConfig config;
        config.directory = dir;
        config.filePrefix = "test-rot";
        config.async = false;
        config.maxFileSizeBytes = 1024;
        config.maxRotatedFiles = 8;
        CHECK_TRUE(svk::log::initWithFile(spdlog::get("svk_test_rotation"),
                                          config));
        for (int i = 0; i < 100; ++i) {
            SVK_LOGI("practice", "rotation_line seq={} payload=xxxxxxxxxxxx", i);
        }
        svk::log::flush();
        svk::log::shutdown();
        CHECK_TRUE(countFilesWithPrefix(dir, "test-rot") >= 2);
        fs::remove_all(dir);
    }

    // 过期清理：10 天前的旧文件在 maxFileAgeDays=7 下应被删除。
    {
        const std::string dir =
            (fs::temp_directory_path() / "svk_test_prune_age").string();
        fs::remove_all(dir);
        fs::create_directories(dir);
        const std::string oldFile = dir + "/test-age-20200101-000000-000.log";
        writeFakeFile(oldFile, 128, 10);
        svk::log::init("svk_test_prune_age");
        svk::log::LogFileConfig config;
        config.directory = dir;
        config.filePrefix = "test-age";
        config.maxFileAgeDays = 7;
        CHECK_TRUE(svk::log::initWithFile(spdlog::get("svk_test_prune_age"),
                                          config));
        svk::log::shutdown();
        CHECK_TRUE(!fs::exists(oldFile));
        fs::remove_all(dir);
    }

    // 总量清理：3 个 3KB 文件、上限 5KB，最老的两个应被删除。
    {
        const std::string dir =
            (fs::temp_directory_path() / "svk_test_prune_total").string();
        fs::remove_all(dir);
        fs::create_directories(dir);
        writeFakeFile(dir + "/test-cap-20240101-000000-000.log", 3072, 3);
        writeFakeFile(dir + "/test-cap-20240102-000000-000.log", 3072, 2);
        writeFakeFile(dir + "/test-cap-20240103-000000-000.log", 3072, 1);
        svk::log::init("svk_test_prune_total");
        svk::log::LogFileConfig config;
        config.directory = dir;
        config.filePrefix = "test-cap";
        config.maxFileAgeDays = 0;  // 关闭按时间删除，只看总量
        config.maxTotalBytes = 5 * 1024;
        CHECK_TRUE(svk::log::initWithFile(spdlog::get("svk_test_prune_total"),
                                          config));
        svk::log::shutdown();
        CHECK_TRUE(
            !fs::exists(dir + "/test-cap-20240101-000000-000.log"));
        CHECK_TRUE(
            !fs::exists(dir + "/test-cap-20240102-000000-000.log"));
        CHECK_TRUE(
            fs::exists(dir + "/test-cap-20240103-000000-000.log"));
        fs::remove_all(dir);
    }
    std::printf("--- 轮转与清理：rotation / 过期 / 总量校验完成 ---\n");
}

int main() {

    // 学习 spdlog 的基本用法，输出到终端，并统计耗时。
    test_spdlog_base_use();

    // 限速宏：ONCE / THROTTLE / THROTTLE_KEY 的放行与压制行为。
    test_log_throttle();

    // 审计通道：独立文件、与主文件内容隔离。
    test_log_audit_channel();

    // 轮转与清理：单文件上限切分、过期删除、总量上限。
    test_log_rotation_prune();

    if (g_failures > 0) {
        std::printf("\n%d 项 CHECK 失败\n", g_failures);
        return 1;
    }
    std::printf("\n全部 CHECK 通过\n");
    return 0;
}
