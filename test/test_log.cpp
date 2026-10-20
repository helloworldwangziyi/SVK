/**
 * @file test_log.cpp
 * @brief 日志练习起点：输出到终端，并统计耗时。
 *
 * 运行：./build/host/test/test_log，或 cd build/host && ctest -R test_log
 */

#include <chrono>
#include <cstdio>

#include "log.h"


void test_spdlog_base_use(){
    svp::log::init("svp_test_base_use");

    SVP_LOGD("practice", "level_debug value={}", 1);
    SVP_LOGI("practice", "level_info value={}", 2);
    SVP_LOGW("practice", "level_warn value={}", 3);
    SVP_LOGE("practice", "level_err value={}", 4);

    // 统计循环写 100万 条的耗时。终端 I/O 是主要开销，这本身就是第一个观察点。
    constexpr int kCount = 1000000;
    const auto begin = std::chrono::steady_clock::now();
    for(int i = 0; i < kCount; ++i){
        SVP_LOGI("practice", "timing seq={} value={}", i, i * 2);
    }
    const auto end = std::chrono::steady_clock::now();

    const double ms = std::chrono::duration<double, std::milli>(end - begin).count();

    std::printf("\n%d 条日志，耗时 %.1f ms，约 %.0f 条/秒\n", kCount, ms, kCount / (ms / 1000.0));

    svp::log::shutdown();
}

int main() {

    // 学习 spdlog 的基本用法，输出到终端，并统计耗时。
    test_spdlog_base_use();

    return 0;
}
