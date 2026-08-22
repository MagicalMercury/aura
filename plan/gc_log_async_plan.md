# GC 日志输出异步化 — 实施方案（工作流 3 成稿）

> 状态：**已按审查修复（P1-P6），待审查，通过后进入工作流 4（change.md）**
> 审查轮次：第 1 轮（2026-08-21），源码行号均已实际检索验证

## 一、目标

将 GC 线程中 `logGcEvent()` 的同步 `fprintf(stderr, ...)`（每次 GC 事件最多 6 次串行调用，含 stderr 全局锁 + 系统调用）改为异步输出：GC 线程 `snprintf` 格式化到栈缓冲 → 无锁入队 → 独立 Logger 线程批量 `fwrite(stderr)`，通过 `cv.notify` 唤醒消费者。

## 二、源码现状（已验证行号）

### 2.1 GC 日志输出点（safepoint.cpp:355-388）

`logGcEvent()` 是唯一的 GC 热路径日志函数，6 次 fprintf 实际行号：359 / 362 / 365 / 366 / 375 / 384。

- `fmtBytes`（safepoint.cpp:307）与 `gcKindName`（safepoint.cpp:321）为**文件内 static 函数**，重写后同文件可直接复用。
- `logGcEvent` 唯一调用点：`recordGcEvent` 尾部（safepoint.cpp:352）。

### 2.2 非热路径 fprintf（保持同步，不异步化）

| 文件:行号 | 内容 | 原因 |
|-----------|------|------|
| `gc.cpp:118` | `[GC] warning: unknown AURA_GC_LOG tag` | 构造时调用（parseGcLogEnv 内），Logger 尚未启动 |
| `safepoint.cpp:154` | `[GC] *** STW DEADLOCK ***` | 错误路径，紧接 `std::abort()` |
| `safepoint.cpp:530` | `[GC] *** ROOT STOP TIMEOUT ***` | 错误路径，紧接 `std::abort()` |

### 2.3 生命周期挂载点

| 位置 | 文件:行号 | 现状 |
|------|----------|------|
| `GcHeap::GcHeap()` | gc.cpp:128-133 | 解析 `AURA_GC_LOG` + 设置时间基准，**末尾追加 Logger 启动** |
| `GcHeap::~GcHeap()` | gc.cpp:145-150 | **当前为空体**（含"不释放 GC 页"注释），**首行插入 shutdown** |
| `g_gcHeap` 定义 | gc.cpp:77 | `[[gnu::init_priority(101)]] GcHeap g_gcHeap;` ← 析构顺序分析关键（见 §七-5） |
| `gcLogLevels_` | gc.h:654 | `uint8_t gcLogLevels_[kTagCount] = {}` 默认全 Off |
| `kTagCount` | gc.h:278 | `enum GcLogTag : uint8_t { kTagGc=0, kTagPhase, kTagMemory, kTagTrigger, kTagCount }` |

### 2.4 CMakeLists.txt（L46-63）

显式源文件列表。新文件均为 header-only（.h），**无需修改**。

## 三、审查发现的问题与修复（本轮成稿已全部融入）

| # | 严重度 | 问题 | 修复方案 |
| - | ------ | ---- | -------- |
| P1 | 严重 | 原 §6.5 静态析构顺序断言**说反**：`g_gcHeap`（init_priority(101)）先构造→后析构；Meyers singleton `GcLogWriter` 在 GcHeap 构造期首次构造（晚）→**先析构**。`~GcHeap()` 调 `instance().shutdown()` 为 use-after-destroy（UB）| 弃用 Meyers singleton：改为自由函数 `gcLogStart/gcLogEnqueue/gcLogShutdown` + 函数内 static **裸指针**（平凡析构类型，无析构顺序问题）；`GcLogWriter` 对象堆上 new/delete，生命周期与 GcHeap 严格嵌套（§4.2） |
| P2 | 严重 | `kEntrySize=256` 小于合并日志最坏长度（info ~140 + phase ~105 + trigger ~90 ≈ **330+ 字节**；`gc*=trace` 三段全启用即触发）→ 尾部截断 | `kEntrySize=512`（与栈缓冲对齐）；`kCapacity=512`（内存 ≈260KB，堆分配，Logger 启动才分配） |
| P3 | 严重 | `n += snprintf(...)` 无 clamp：截断时 snprintf 返回"期望长度" > 剩余空间 → n 越过缓冲容量后 `sizeof(buf)-n` **下溢成巨大 size_t → 越界写** | 新增 `appendFmt` 辅助函数（va_list + clamp 到实际写入数，§五-Step 4） |
| P4 | 中 | `tryPopBatch` 部分消费：`copyLen < e.len` 时条目剩余部分静默丢弃且 rpos 已跳过（当前参数恰好不触发，属脆弱耦合） | 放不下整条时 `break` 留待下一批 |
| P5 | 低 | 测试路径为外部 Linux 环境路径 | 改写为本仓库约定流程（§八） |
| P6 | 信息 | 环满降级 fwrite 与 Logger 线程 fwrite 并发，行可能交错 | 可接受：stderr 的 fwrite 线程安全（CRT 锁），仅极端高峰降级路径受影响；方案中注明 |

## 四、设计（修订版）

### 4.1 架构

```
┌──────────────────────────────────────────────────────────┐
│  GC 线程（生产者，STW 关键路径）                            │
│                                                           │
│  logGcEvent(ev)                                           │
│    ├── appendFmt × N 段 → char buf[512]（无锁格式化）      │
│    └── gcLogEnqueue(buf, n)                               │
│          ├── tryPush() 无锁原子入队 SPSC Ring              │
│          │     └── 成功 → lock(wakeM_) + notify_one + unlock │
│          └── 失败（环满/未启动）→ fwrite(stderr) 降级（保底不丢日志）│
└──────────────────────────────────────────────────────────┘
                        │
                        ▼
┌──────────────────────────────────────────────────────────┐
│  Logger 线程（消费者，GcHeap 生命周期内常驻，不参与 safepoint）│
│                                                           │
│  run()                                                    │
│    loop:                                                  │
│      n = tryPopBatch(batch, 16KB, 32条)  ← 无锁批量取     │
│      if n > 0: fwrite(stderr) + continue（立即取下一批）   │
│      else: waitForData(500ms)  ← cv.wait_for（防丢失唤醒） │
│                                                           │
│  shutdown(): shouldExit → notify → join → drainAll        │
└──────────────────────────────────────────────────────────┘
```

### 4.2 生命周期管理（P1 修复核心）

**不用 Meyers singleton**。`gc_log_writer.h` 提供三个自由函数（`aura_rt` 命名空间，inline）：

```cpp
gcLogStart()        // GcHeap 构造尾调用（任一日志标签启用时）：new + start，幂等
gcLogEnqueue(...)   // logGcEvent 调用：入队或降级同步输出
gcLogShutdown()     // ~GcHeap 首行调用：shutdown + delete + 置空，幂等
```

内部通过 `gcLogWriterSlot()`（函数内 static **裸指针**）共享实例：

- 函数内 static 指针是**平凡析构**类型——不注册静态析构，访问无顺序问题；
- `GcLogWriter` 对象本身在堆上，new/delete 与 GcHeap 构造/析构严格配对嵌套，与 `init_priority` / 跨 TU 析构顺序**完全解耦**；
- inline 函数的函数局部 static 全程序唯一（C++ 标准），多 TU 共享同一实例。

### 4.3 SPSC Ring 正确性

数据传输（生产→消费）完全无锁（原子 acquire/release）：

- 生产者写 slot 后 `writePos_.store(release)` → 消费者 `load(acquire)` 后读 slot；
- 消费者读完 slot 后 `readPos_.store(release)` → 生产者 `load(acquire)` 后才复用 slot；
- `wakeM_` mutex 仅用于消费者睡眠/生产者唤醒协调（防丢失唤醒），**不保护数据**。

防丢失唤醒三种时序均正确（消费者持锁后二次检查 `writePos != readPos` 则不等待）。

### 4.4 为何不用 ThreadChannel

| 维度 | ThreadChannel | SPSC Ring + cv |
|------|---------------|----------------|
| GC 安全 | `GcObject` 子类，GC 线程引用会自引用循环 | 纯 C++ 对象，安全 |
| safepoint | `send()` 满时调 `gc_safepoint()` → GC 线程死锁 | `tryPush()` 不阻塞，不触发 safepoint |
| 根注册 | 每次 send 构造 `GcRootHandle` 污染根集 | 无根注册 |
| 锁 | mutex + cv 每次 send | 仅 notify 时短暂持锁 |
| 内存 | deque 动态分配 | 固定环形，零分配 |

## 五、实施步骤（含完整实现代码）

### Step 1：新建 `runtime/gc/gc_log_queue.h`（~110 行）

```cpp
#pragma once
// ============================================================
// aura_rt/gc/gc_log_queue.h — GC 日志无锁 SPSC 环形队列
//
// 单生产者（GC 线程）单消费者（Logger 线程）：
//   - 数据传输完全无锁（writePos_/readPos_ acquire/release）
//   - wakeM_/wakeCv_ 仅用于消费者睡眠/生产者唤醒协调（防丢失唤醒），
//     不保护 entries_ 数据
// 拆分自 gc_log_async_plan（GC 日志异步化）。
// ============================================================
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>

namespace aura_rt {

class GcLogQueue {
public:
    // P2 修复：单条合并日志最坏 ~330 字节（info+phase+trigger 全启用），
    // 256 会截断；512 与 logGcEvent 栈缓冲对齐
    static constexpr size_t kCapacity  = 512;  // 512 个槽位（内存 ≈260KB）
    static constexpr size_t kEntrySize = 512;  // 每条日志最大 512 字节

    // 生产者（GC 线程）：无锁 push + notify
    bool tryPush(const char* data, size_t len) {
        size_t pos  = writePos_.load(std::memory_order_relaxed);
        size_t next = (pos + 1) % kCapacity;
        if (next == readPos_.load(std::memory_order_acquire))
            return false;  // 环满
        Entry& e = entries_[pos];
        e.len = std::min(len, kEntrySize);
        std::memcpy(e.data, data, e.len);
        writePos_.store(next, std::memory_order_release);
        {  // 唤醒 Logger（持锁防丢失唤醒）
            std::lock_guard<std::mutex> lk(wakeM_);
            wakeCv_.notify_one();
        }
        return true;
    }

    // 消费者（Logger 线程）：无锁批量 pop
    // P4 修复：放不下整条时停止（留待下一批），杜绝部分消费丢尾
    size_t tryPopBatch(char* out, size_t maxOut, size_t maxEntries) {
        size_t total = 0, count = 0;
        size_t rpos = readPos_.load(std::memory_order_relaxed);
        while (count < maxEntries && total < maxOut) {
            if (rpos == writePos_.load(std::memory_order_acquire)) break;
            Entry& e = entries_[rpos];
            if (e.len > maxOut - total) break;  // 剩余空间放不下整条：留下一批
            std::memcpy(out + total, e.data, e.len);
            total += e.len;
            count++;
            rpos = (rpos + 1) % kCapacity;
        }
        readPos_.store(rpos, std::memory_order_release);
        return total;
    }

    // 消费者等待（持锁后二次检查，防丢失唤醒）
    void waitForData(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(wakeM_);
        if (writePos_.load(std::memory_order_acquire) !=
            readPos_.load(std::memory_order_relaxed))
            return;  // 二次检查有数据
        wakeCv_.wait_for(lk, timeout);
    }

    void notifyShutdown() {
        std::lock_guard<std::mutex> lk(wakeM_);
        wakeCv_.notify_all();
    }

private:
    struct Entry { size_t len; char data[kEntrySize]; };
    alignas(64) std::atomic<size_t> writePos_{0};
    alignas(64) std::atomic<size_t> readPos_{0};
    Entry entries_[kCapacity];
    std::mutex wakeM_;              // 仅防丢失唤醒，不保护 entries_
    std::condition_variable wakeCv_;
};

} // namespace aura_rt
```

**验收标准**：编译通过；单线程 `tryPush` + `tryPopBatch` 往返数据一致；长度 > 512 的输入截断为 512。

### Step 2：新建 `runtime/gc/gc_log_writer.h`（~120 行）

```cpp
#pragma once
// ============================================================
// aura_rt/gc/gc_log_writer.h — GC 异步日志 Logger 线程
//
// P1 修复（生命周期）：不用 Meyers singleton——g_gcHeap 带
// init_priority(101) 先构造后析构，Meyers 对象构造晚则析构早，
// ~GcHeap 再调 shutdown 将 use-after-destroy。改为：
//   - gcLogWriterSlot()：函数内 static 裸指针（平凡析构，
//     不注册静态析构，访问无顺序问题；inline 函数的局部
//     static 全程序唯一）
//   - GcLogWriter 对象堆上 new/delete，由 gcLogStart/gcLogShutdown
//     配对管理，生命周期与 GcHeap 严格嵌套
// ============================================================
#include "gc_log_queue.h"
#include <atomic>
#include <cstdio>
#include <thread>

namespace aura_rt {

class GcLogWriter {
public:
    void start() {
        if (started_.exchange(true)) return;
        enabled_ = true;
        thread_ = std::thread([this] { run(); });
    }

    // GC 线程调用：无锁入队；未启动/环满时降级同步输出（保底不丢日志）。
    // 注：降级 fwrite 与 Logger 线程的 fwrite 并发——stderr 的 fwrite
    // 线程安全（CRT 内部锁），极端高峰下行可能交错，可接受（罕见路径）
    void enqueue(const char* data, size_t len) {
        if (!enabled_.load(std::memory_order_relaxed) ||
            !queue_.tryPush(data, len)) {
            std::fwrite(data, 1, len, stderr);  // 降级保底
        }
    }

    void shutdown() {
        if (!started_.exchange(false)) return;
        shouldExit_ = true;
        queue_.notifyShutdown();  // 唤醒等待中的 Logger
        if (thread_.joinable()) thread_.join();
        drainAll();               // 拗干拗余日志
        enabled_ = false;
    }

private:
    GcLogQueue queue_;
    std::thread thread_;
    std::atomic<bool> started_{false};
    std::atomic<bool> shouldExit_{false};
    std::atomic<bool> enabled_{false};

    void run() {
        constexpr size_t kBatchBytes = 32 * GcLogQueue::kEntrySize;  // 16KB
        char batch[kBatchBytes];
        while (!shouldExit_.load(std::memory_order_relaxed)) {
            size_t n = queue_.tryPopBatch(batch, kBatchBytes, 32);
            if (n > 0) {
                std::fwrite(batch, 1, n, stderr);
                continue;  // 立即取下一批
            }
            queue_.waitForData(std::chrono::milliseconds(500));
        }
    }

    void drainAll() {
        constexpr size_t kBatchBytes = 32 * GcLogQueue::kEntrySize;
        char batch[kBatchBytes];
        for (;;) {
            size_t n = queue_.tryPopBatch(batch, kBatchBytes, 32);
            if (n == 0) break;
            std::fwrite(batch, 1, n, stderr);
        }
    }
};

// ---- 对外接口（GcHeap 构造/析构、logGcEvent 调用）----

inline GcLogWriter*& gcLogWriterSlot() {
    static GcLogWriter* w = nullptr;  // 平凡析构指针：无静态析构顺序问题
    return w;
}

// GcHeap 构造尾调用（任一日志标签启用时）：幂等
inline void gcLogStart() {
    GcLogWriter*& w = gcLogWriterSlot();
    if (!w) w = new GcLogWriter();
    w->start();
}

// logGcEvent 调用：入队或降级同步输出
inline void gcLogEnqueue(const char* data, size_t len) {
    GcLogWriter* w = gcLogWriterSlot();
    if (w) w->enqueue(data, len);
    else   std::fwrite(data, 1, len, stderr);  // 未启动（理论不可达）：同步保底
}

// ~GcHeap 首行调用：幂等（未启动时零开销）
inline void gcLogShutdown() {
    GcLogWriter*& w = gcLogWriterSlot();
    if (!w) return;
    w->shutdown();
    delete w;
    w = nullptr;
}

} // namespace aura_rt
```

**验收标准**：`gcLogStart()` 后 Logger 线程后台运行，`gcLogEnqueue` 数据后 ~1ms 内 stderr 可见；`gcLogShutdown()` 后线程退出且拗余日志全部输出；重复调用 start/shutdown 幂等。

### Step 3：修改 `runtime/gc/gc.cpp`（~10 行改动）

**3a. 顶部新增 include**

```cpp
#include "gc_log_writer.h"
```

**3b. 构造函数末尾（gc.cpp:128-133）追加 Logger 启动**

```cpp
GcHeap::GcHeap() {
    parseGcLogEnv();
    // 进程启动基准（全局构造在 main 前，接近进程启动）
    gcStartBaseMicros_ = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    // 新增：任一日志标签启用时启动异步 Logger 线程（gcLogStart 幂等）
    for (int i = 0; i < kTagCount; ++i) {
        if (gcLogLevels_[i] > 0) { gcLogStart(); break; }
    }
}
```

**3c. 析构函数首行（gc.cpp:145-150）插入 shutdown**

```cpp
GcHeap::~GcHeap() {
    // 新增：关闭异步 Logger + drain 拗余日志（幂等；未启动时零开销）
    gcLogShutdown();

    // 进程退出时不主动释放 GC 页。
    // 原因：静态析构顺序不确定，其他对象（协程帧 / std::vector 等）
    // 可能仍在引用 GC 页中的内存，freeAllPages() 会导致 use-after-free。
    // 让操作系统在进程退出时统一回收所有内存。
}
```

### Step 4：修改 `runtime/gc/safepoint.cpp` — `logGcEvent()`（~45 行改动）

**4a. 顶部新增 include**

```cpp
#include "gc_log_writer.h"
#include <cstdarg>   // appendFmt 的 va_list
```

**4b. 在 `fmtBytes`（L307 附近）旁新增 appendFmt 辅助**

```cpp
// 追加式格式化（logGcEvent 专用）：
// P3 修复——snprintf 截断时返回"期望写入长度"（> 剩余空间），必须 clamp
// 到实际写入数，否则 n 越过 buf 容量后 sizeof(buf)-n 下溢 → 越界写
static int appendFmt(char* buf, int n, size_t cap, const char* fmt, ...) {
    if (static_cast<size_t>(n) >= cap - 1) return n;  // 已满：丢弃本段
    va_list ap;
    va_start(ap, fmt);
    int ret = std::vsnprintf(buf + n, cap - static_cast<size_t>(n), fmt, ap);
    va_end(ap);
    if (ret < 0) return n;  // 编码错误：丢弃本段
    size_t avail = cap - 1 - static_cast<size_t>(n);
    size_t written = static_cast<size_t>(ret) < avail
                         ? static_cast<size_t>(ret) : avail;
    return n + static_cast<int>(written);
}
```

**4c. 替换 `logGcEvent` 函数体（L355-388）**

格式串与原实现**逐字一致**（保证输出与同步版逐字节兼容，测试可对比行数）：

```cpp
void GcHeap::logGcEvent(const GcEvent& ev) {
    char buf[512];
    int n = 0;

    // gc 主行（info）
    if (gcLogLevels_[kTagGc] >= static_cast<uint8_t>(GcLogLevel::Info)) {
        char a1[32], a2[32], f[32];
        n = appendFmt(buf, n, sizeof(buf), "[GC][info] %s #%zu @%.3fs: ",
                      gcKindName(ev.kind),
                      gcCount_ + minorGcCount_ + mixedGcCount_ + 1,
                      ev.startMicros / 1e6);
        if (ev.kind == 4)  // 并发路径：三阶段
            n = appendFmt(buf, n, sizeof(buf), "%.2f+%.2f+%.2f ms clock",
                          ev.rootsMicros / 1000.0, ev.markMicros / 1000.0,
                          ev.finalizeMicros / 1000.0);
        else
            n = appendFmt(buf, n, sizeof(buf), "%.2f ms clock",
                          ev.totalMicros / 1000.0);
        n = appendFmt(buf, n, sizeof(buf),
                      ", live %zu->%zu (%s->%s), freed %s\n",
                      ev.liveBefore, ev.liveAfter,
                      fmtBytes(ev.bytesBefore, a1, sizeof(a1)),
                      fmtBytes(ev.bytesAfter,  a2, sizeof(a2)),
                      fmtBytes(ev.freedBytes,  f,  sizeof(f)));
    }

    // gc/phase（debug，仅并发路径有阶段分解）
    // finalize 拆分 wait（STW 停靠等待）与 work（补扫/SATB/回收）——诊断收尾耗时构成
    if (ev.kind == 4 && gcLogLevels_[kTagPhase] >= static_cast<uint8_t>(GcLogLevel::Debug)) {
        n = appendFmt(buf, n, sizeof(buf),
            "[GC][debug][phase] concurrent: roots=%.2fms mark=%.2fms finalize=%.2fms (wait=%.2fms work=%.2fms)\n",
            ev.rootsMicros / 1000.0, ev.markMicros / 1000.0,
            ev.finalizeMicros / 1000.0,
            ev.finalizeWaitMicros / 1000.0,
            (ev.finalizeMicros - ev.finalizeWaitMicros) / 1000.0);
    }

    // gc/trigger（debug）
    if (gcLogLevels_[kTagTrigger] >= static_cast<uint8_t>(GcLogLevel::Debug)) {
        static const char* trig[] = { "youngBytes>=threshold", "compactMedium",
                                      "oldBytes>=threshold", "sweepLargePages" };
        n = appendFmt(buf, n, sizeof(buf), "[GC][debug][trigger] %s #%zu: %s\n",
                      gcKindName(ev.kind),
                      gcCount_ + minorGcCount_ + mixedGcCount_ + 1,
                      ev.trigger <= 3 ? trig[ev.trigger] : "forceGc");
    }

    // 一次性异步输出（或降级同步）
    if (n > 0) gcLogEnqueue(buf, static_cast<size_t>(n));
}
```

**不修改的 fprintf**（保持同步）：gc.cpp:118（构造期 Logger 未启动）、safepoint.cpp:154 与 530（abort 前需立即输出）。

### Step 5：编译

```powershell
cmake --build runtime/build
cmake --build build
```

## 六、文件变更清单

| 文件 | 操作 | 改动量 | 说明 |
|------|------|--------|------|
| `runtime/gc/gc_log_queue.h` | **新建** | ~110 行 | 无锁 SPSC 环形 + cv 唤醒协调（kEntrySize=512 修复 P2，整条消费修复 P4）|
| `runtime/gc/gc_log_writer.h` | **新建** | ~120 行 | Logger 线程 + 自由函数生命周期接口（修复 P1）|
| `runtime/gc/gc.cpp` | 修改 | ~10 行 | 构造启动 + 析构关闭（含 1 个 include）|
| `runtime/gc/safepoint.cpp` | 修改 | ~45 行 | appendFmt 辅助（修复 P3）+ logGcEvent 重写（含 2 个 include）|
| `runtime/CMakeLists.txt` | 不修改 | 0 | header-only，无需加入源文件列表 |
| `runtime/gc/gc.h` | 不修改 | 0 | 无新增成员；接口全部走自由函数 |

**总计**：~230 行新代码 + ~55 行修改。

## 七、边界条件与安全性

### 7.1 环满降级

`tryPush` 失败（512 槽全满）或 Logger 未启动时，`enqueue` 直接 `fwrite(stderr)` 同步输出——不丢日志，代价是偶尔同步 I/O（仅极端高峰）。降级 fwrite 与 Logger 线程并发写 stderr：CRT 的 fwrite 线程安全，行可能交错（可接受，罕见路径）。

### 7.2 日志关闭时零开销

`AURA_GC_LOG` 未设置 → `gcLogLevels_` 全 0 → 构造循环不调 `gcLogStart()`（Logger 线程不启动、不分配 260KB）→ `logGcEvent` 所有 if 不满足 → 不调 `gcLogEnqueue` → `gcLogShutdown` 遇空指针直接返回——全链路零开销。

### 7.3 进程退出完整性

`~GcHeap()` 首行 `gcLogShutdown()`：`shouldExit_=true` → `notifyShutdown()` 唤醒 Logger → `join()` 等待退出 → `drainAll()` 拗干拗余 → delete。所有日志在 GcHeap 析构前输出完毕。

### 7.4 Logger 线程不参与 safepoint

Logger 线程是纯 C++ `std::thread`，不注册 `GcRootHandle`、不进入 `threadRootLists_`、不调用 `gc_safepoint()`。GC STW 时无需停靠它，`cv.wait_for` 可安全使用。

### 7.5 静态析构顺序（P1 修复后的正确性）

`g_gcHeap` 为 `[[gnu::init_priority(101)]]` 命名空间级静态（先构造→后析构）。本方案：

- `GcLogWriter` 对象在**堆上**，由 GcHeap 构造函数 new、析构函数 delete——生命周期严格嵌套于 GcHeap 内，与任何静态析构顺序**无关**；
- `gcLogWriterSlot()` 的函数内 static 是**裸指针**（平凡析构）——不注册静态析构，`~GcHeap` 访问它无 use-after-destroy 风险；
- inline 函数局部 static 全程序唯一（C++ 标准），gc.cpp / safepoint.cpp 两个 TU 共享同一实例。

### 7.6 Logger 线程的 stderr 在静态析构期可用性

`~GcHeap()` 运行于静态析构阶段，Logger 线程此时仅调用 `fwrite(stderr)`——C 标准保证 stderr 在静态析构后仍可写（`stderr` 不受 `std::ios_base::Init` 影响）。

## 八、测试方案（本仓库约定）

### 8.1 编译验证

```powershell
cmake --build runtime/build   # 无警告错误（-Wall -Wextra -Wpedantic）
cmake --build build
```

### 8.2 功能验证（example/test.aura + compile.cmd）

test.aura 写 GC 压力用例（大量分配触发多轮 GC，含中页/LOS 分配以覆盖 kind 0-4 各事件路径）：

```powershell
.\compile.cmd          # 非 ASAN 模式编译 test.aura → test.exe
$env:AURA_GC_LOG = "gc*=trace"
.\example\test.exe 2>&1 | Select-String "^\[GC\]" | Measure-Object -Line
```

验证点：
1. **输出完整**：每行以 `\n` 结尾、无截断（P2 回归点——重点看 info+phase+trigger 三段全启用的并发 GC 行）；
2. **格式一致**：与改造前同步版逐行对比，行数与内容一致；
3. **尾部完整**：最后一条 GC 日志在进程退出前出现（drain 生效）。

### 8.3 零开销验证（日志关闭）

```powershell
Remove-Item Env:AURA_GC_LOG   # 确认未设置
.\example\test.exe 2>&1 | Select-String "^\[GC\]" | Measure-Object -Line
# 期望：0 行（Logger 未启动，行为与改造前完全一致）
```

### 8.4 稳定性验证

```powershell
1..10 | ForEach-Object { $env:AURA_GC_LOG="gc*=trace"; .\example\test.exe *> $null; "Run $_: $LASTEXITCODE" }
# 期望：10/10 退出码 0（Logger join 正常，无崩溃无 hang）
```

### 8.5 ASAN 深度验证（可选，排查内存问题）

```powershell
.\ASAN_Test.ps1 example\test.aura   # 无 use-after-free / 内存错误
```

## 九、预期性能

| 场景 | 改造前（同步 fprintf） | 改造后（异步） |
|------|----------------------|--------------|
| 稳态并发 GC finalize | 0.05-0.14ms（含 ~30-90μs 日志 I/O） | 0.02-0.08ms（snprintf ~5μs + enqueue ~1μs） |
| 高峰 GC（wait 主导） | 57.36ms | 基本无差异（瓶颈在 wait） |
| 日志输出延迟 | 0ms（同步即时） | <1ms（notify + 唤醒调度） |
| 20 次 GC 的日志开销 | ~120μs | ~20μs |

关键收益在**稳态并发 GC**：日志 I/O 开销从 GC 线程移到 Logger 线程，finalize 实际工作时间进一步缩短。静态内存代价：Logger 启动时堆分配 ≈260KB（512 槽 × 520B），日志关闭时为零。
