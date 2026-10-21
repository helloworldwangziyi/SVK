# 日志系统设计：从渲染引擎到期货金融 App

本文记录 `svk` 日志体系的演进设计：以当前 `core/src/log.h` 为起点，
逐步适配移动端渲染引擎的高频场景，以及后续期货金融类 App 的可靠性与合规要求。
实现按文末路线图分阶段推进，每阶段完成后更新状态。

## 1. 背景与目标

- 渲染引擎侧：渲染循环中失败重试、swapchain 重建等事件可能一帧爆发数百条，
  需要限速与聚合；排查问题需要日志与具体帧关联。
- 金融业务侧：行情订阅几十合约时 tick 频率约 100 条/秒，文本化后每小时上百 MB；
  下单、成交等审计日志不允丢弃；日志含账号、持仓等敏感信息，有合规要求。

## 2. 核心原则

1. **分层**：引擎与业务日志可丢（`overrun_oldest`），审计日志不可丢
   （`block` + 独立文件），两者不共用队列策略。
2. **日志 ≠ 数据**：行情 tick 是数据录制问题，不进诊断日志队列——否则会打爆
   异步队列，`overrun_oldest` 把崩溃前的关键 Error 冲掉，且文本体积是二进制的
   5~10 倍。
3. **日志 ≠ 追踪**：帧耗时分析走平台 trace（Android ATrace / iOS os_signpost /
   鸿蒙 HiTrace），不往日志系统里塞。
4. **丢可以，要丢得明白**：每一处丢弃都有计数，并周期性浮到诊断日志。

## 3. 总体结构

```text
svk::log（诊断日志，现有 log.h 演进）
 ├─ 引擎通道   overrun_oldest，可丢
 ├─ 业务通道   overrun_oldest，可丢
 └─ 审计通道   block 策略、独立文件、独立保留期（下单/成交/登录/资金变动）

QuoteRecorder（行情数据，业务层独立模块）
 ├─ 二进制环形缓冲 → 后台批量落盘（后续 mmap 保崩溃现场）
 ├─ 按交易日分目录 + 单文件 rotation + 总量 LRU 清理
 └─ QuoteReader：按交易日/时段/合约过滤，导出 CSV/JSON，供复盘回放
```

诊断日志与行情录制通过同一个 session id 和同一套时钟关联，出问题时
两边时间线可以对上。

## 4. 现状盘点（已具备）

`core/src/log.h` 当前能力（详见 docs/engine-core.md 第 10 节）：

- header-only spdlog 封装；`[svk][<feature>][<event> key=value]` 结构化正文。
- 终端 sink（Logcat / HiLog / stdout）+ 可追加文件 sink；有界异步队列
  （默认 8192），溢出计数并在 flush 时补写 `async_queue_overrun`。
- `should_log` 前置拦截，级别不满足时不执行 fmt 格式化。
- Error 级别入队后调用线程同步等待一次 flush；挂起/退出时 `flush()` 排空队列。

## 5. 诊断日志增强

### 5.1 限速原语

- `SVK_LOG*_ONCE(feature, ...)`：同一调用点进程生命周期只写一次
  （如驱动不支持的特性告警）。
- `SVK_LOG*_THROTTLE(feature, interval_ms, ...)`：同一调用点在时间窗内只放行
  第一条，窗口内压制的条数在下一次放行时以 `suppressed=N` 附在正文末尾。
- `SVK_LOG*_THROTTLE_KEY(feature, key, interval_ms, ...)`：按 key 各自限速，
  典型 key 为合约代码——避免 100 个合约的同类告警互相淹没或刷屏。

行情事件日志只用这些原语记"状态与异常"：连接生命周期、订阅、活性心跳
（如每 5s 一条 `alive ticks={} latency_ms={}`）、seq 跳变、延迟超阈值。

### 5.2 帧号关联

引擎单线程模型下，renderer 每帧写 `thread_local` 当前 `frame_seq`，
`write()` 自动附加 `frame=` 字段，掉帧/重建类日志直接定位到帧。

### 5.3 编译期与运行时级别

- 编译期：`SVK_ACTIVE_LEVEL` 宏（类 `SPDLOG_ACTIVE_LEVEL`），release 砍掉 V/D
  的调用本身。
- 运行时：服务端下发或调试面板临时把 `fileLevel` 降到 Debug 排障，结束后恢复。

### 5.4 启动 session 头

每次冷启动写一条 session 日志：App 版本、引擎版本、git hash、机型、系统版本、
时区、session id。售后拿到文件即可对齐环境。

### 5.5 壳日志入库

Java/Objective-C/ArkTS 的生命周期日志目前只进 Logcat/控制台/HiLog，不进文件
（鸿蒙 ETS 手写 `[svk]` 前缀同样不落盘）。桥接进 C++ 文件日志，保证排障时
时间线完整。

## 6. 审计通道

- 独立 logger、独立文件、独立保留期；内容：登录、下单、撤单、成交回报、
  资金变动。
- 溢出策略用 `block`（宁可业务等，不可丢审计），队列容量与引擎分开配置。
- 宏形态：`SVK_ALOGI("trade", "order_submit instrument={} price={} ...")`。

## 7. 行情数据录制（QuoteRecorder）

### 7.1 为什么独立

期货行情为约 500ms 切片快照，订阅 50 合约 ≈ 100 tick/s ≈ 文本 30KB/s ≈
每小时上百 MB。必须二进制、独立文件、独立写盘线程。

### 7.2 文件格式

```text
文件头：magic、格式版本、session_id、交易日、时区
记录帧：[len][type][ts_ns][payload]   payload 为协议层原始结构体，带版本号
```

### 7.3 缓冲与写盘

- 业务线程 `write(tick)` 只写内存帧队列（默认 1MB；有界 deque，与环形缓冲
  同等语义），满则覆盖最老帧并计入 `dropped`。
- 后台线程满 64KB 或 1s 批量落盘一次；页面 flush 不过勤，兼顾闪存与电量。
- 崩溃保全：后续换 mmap 写（Mars xlog 同款思路），崩溃时已基本落盘。
- 文件名带毫秒与序号后缀，快速连续轮转不重名。
- 第四层防线落地为业务钩子：`isDropping()` / `stats()` 暴露丢弃压力，
  业务据此降级采样（全量 → 每秒快照 → 只录自选合约）。

### 7.4 文件组织

```text
quotes/<trading_day>/<prefix>-HHMMSS.qlog
```

贴合期货交易日与夜盘切分；单文件到上限（如 50MB）切新文件。

### 7.5 读取（QuoteReader）

按交易日/时间段/合约过滤；导出 CSV/JSON 供分析；回放接口供后续复盘功能复用。

## 8. 超上限的四层防线

| 层 | 触发 | 策略 |
|---|---|---|
| 内存环形缓冲 | 写入突发 > 落盘速度 | 覆盖最老记录，`dropped` 计数 |
| 单文件 | 到大小上限（50MB） | rotation 切新文件 |
| 磁盘总量 | 超上限（如 500MB / 保留 3 天） | 删最老交易日目录，删前记一条诊断日志 |
| 大行情 | 队列持续超限 | 自动降级：全量 → 每秒快照 → 只录自选合约；平稳后恢复 |

所有丢弃与降级事件周期性汇总进诊断日志（如每 60s 一条
`recorder_stats written={} dropped={} disk_mb={}`）。

## 9. 合规（上线金融 App 前必须）

- **脱敏**：约定 key 即触发掩码（`phone=`、`idcard=`、`token=` 等），在
  `write()` 层统一做，不依赖业务自觉。
- **落盘加密**：日志含账号、持仓、委托信息，AES/混淆加密，上报时还原。
- **回捞通道**：打包压缩、按时间段裁剪、带 session 头，经隐私授权后上传。

## 10. 平台追踪

包一层 `SVK_TRACE_SCOPE` 宏接 ATrace / os_signpost / HiTrace，用于帧耗时与
卡顿分析，与日志系统互补。

## 11. 落地路线

| 阶段 | 内容 | 状态 |
|---|---|---|
| ① | log.h 限速原语（ONCE / THROTTLE / THROTTLE_KEY） | 已完成 |
| ② | 多通道分离：审计通道独立文件、同步直写（等价不丢，较 block 队列更简单） | 已完成 |
| ③ | 文件 rotation（单文件上限切分）与过期/总量清理 | 已完成 |
| ④ | 启动 session 头、运行时级别下发（`setLevels`/`setFileLevel`） | 已完成 |
| ⑤ | QuoteRecorder 写路径：有界缓冲 + 后台落盘 + 四层防线（`core/src/recorder/binary_recorder.h`） | 已完成 |
| ⑥ | QuoteReader：过滤、导出、回放 | 未开始 |
| ⑦ | 崩溃保全：signal handler flush → mmap sink | 未开始 |
| ⑧ | 合规：脱敏、落盘加密、回捞上传 | 未开始 |
| ⑨ | 帧号关联、SVK_TRACE_SCOPE 平台追踪 | 未开始 |
