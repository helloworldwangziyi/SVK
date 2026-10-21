/**
 * @file test_log.cpp
 * @brief 日志练习起点：输出到终端，并统计耗时；覆盖限速宏的放行与压制行为。
 *
 * 运行：./build/host/test/test_log，或 cd build/host && ctest -R test_log
 */

#include <chrono>
#include <cstdio>
#include <thread>

#include "log.h"


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

int main() {

    // 学习 spdlog 的基本用法，输出到终端，并统计耗时。
    test_spdlog_base_use();

    // 限速宏：ONCE / THROTTLE / THROTTLE_KEY 的放行与压制行为。
    test_log_throttle();

    return 0;
}
