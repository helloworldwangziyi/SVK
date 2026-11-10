#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "log/log.h"

static int g_failures = 0;

#define CHECK_TRUE(cond) \
    do { \
        if (!(cond)) { \
            ++g_failures; \
            std::printf("CHECK 失败: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        } \
    } while (0)

static std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream content;
    content << in.rdbuf();
    return content.str();
}

static std::vector<std::filesystem::path> logFiles(
    const std::filesystem::path& directory, const std::string& prefix) {
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.is_regular_file() &&
            entry.path().filename().string().rfind(prefix + "-", 0) == 0) {
            files.push_back(entry.path());
        }
    }
    return files;
}

static void testTerminal() {
    svk::log::init("svk_test_terminal");
    SVK_LOGD("test", "terminal_debug value={}", 1);
    SVK_LOGI("test", "terminal_info value={}", 2);
    SVK_LOGW("test", "terminal_warn value={}", 3);
    SVK_LOGE("test", "terminal_error value={}", 4);
    svk::log::shutdown();
}

static void testDailyFileAndReuse() {
    namespace fs = std::filesystem;
    const fs::path directory = fs::temp_directory_path() / "svk_test_daily";
    fs::remove_all(directory);

    svk::log::init("svk_test_daily");
    svk::log::LogFileConfig config;
    config.directory = directory.string();
    config.filePrefix = "daily";
    CHECK_TRUE(svk::log::initWithFile(spdlog::get("svk_test_daily"), config));
    const std::string firstPath = svk::log::currentLogFilePath();
    const std::string expectedDate =
        svk::log::detail::dateString(std::chrono::system_clock::now());
    CHECK_TRUE(fs::path(firstPath).filename().string() ==
               "daily-" + expectedDate + ".log");

    SVK_LOGI("test", "daily_file_marker");
    CHECK_TRUE(svk::log::initWithFile(spdlog::get("svk_test_daily"), config));
    CHECK_TRUE(svk::log::currentLogFilePath() == firstPath);
    CHECK_TRUE(logFiles(directory, "daily").size() == 1);
    svk::log::shutdown();
    CHECK_TRUE(readFile(firstPath).find("daily_file_marker") != std::string::npos);

    svk::log::init("svk_test_daily_restart");
    CHECK_TRUE(svk::log::initWithFile(spdlog::get("svk_test_daily_restart"), config));
    SVK_LOGI("test", "daily_restart_marker");
    svk::log::shutdown();
    CHECK_TRUE(logFiles(directory, "daily").size() == 1);
    const std::string content = readFile(firstPath);
    CHECK_TRUE(content.find("daily_file_marker") != std::string::npos);
    CHECK_TRUE(content.find("daily_restart_marker") != std::string::npos);
    fs::remove_all(directory);
}

static void testSizeRotation() {
    namespace fs = std::filesystem;
    const fs::path directory = fs::temp_directory_path() / "svk_test_rotation";
    fs::remove_all(directory);

    svk::log::init("svk_test_rotation");
    svk::log::LogFileConfig config;
    config.directory = directory.string();
    config.filePrefix = "rotation";
    config.maxFileSizeBytes = 512;
    CHECK_TRUE(svk::log::initWithFile(spdlog::get("svk_test_rotation"), config));
    for (int i = 0; i < 40; ++i) {
        SVK_LOGI("test", "rotation_marker seq={} payload=xxxxxxxxxxxxxxxx", i);
    }
    svk::log::flush();
    svk::log::shutdown();

    const auto files = logFiles(directory, "rotation");
    CHECK_TRUE(files.size() >= 2);
    const std::string date =
        svk::log::detail::dateString(std::chrono::system_clock::now());
    bool foundFirst = false;
    bool foundLast = false;
    bool foundNumberedPart = false;
    for (const auto& path : files) {
        const std::string name = path.filename().string();
        CHECK_TRUE(name.find("rotation-" + date) == 0);
        foundNumberedPart = foundNumberedPart || name.find("-001.log") != std::string::npos;
        const std::string content = readFile(path);
        foundFirst = foundFirst || content.find("seq=0 ") != std::string::npos;
        foundLast = foundLast || content.find("seq=39 ") != std::string::npos;
    }
    CHECK_TRUE(foundNumberedPart);
    CHECK_TRUE(foundFirst);
    CHECK_TRUE(foundLast);
    fs::remove_all(directory);
}

int main() {
    testTerminal();
    testDailyFileAndReuse();
    testSizeRotation();
    if (g_failures > 0) {
        std::printf("%d 项 CHECK 失败\n", g_failures);
        return 1;
    }
    std::printf("全部 CHECK 通过\n");
    return 0;
}
