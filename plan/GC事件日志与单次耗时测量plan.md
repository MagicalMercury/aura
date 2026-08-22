# 详细实施方案：GC 事件日志与单次耗时测量

> 工作流：3（准备实现 plan——详细实施方案，含完整代码）
> 日期：2026-08-11
> 上游：`plan/GC事件日志与单次耗时测量plan.md`（工作流 2 定稿，已审查通过）
> 方案决策：Go 纯 env 主通道（`AURA_GC_LOG`）+ Java 标签/级别细粒度 + gctrace 三阶段耗时格式
> 本文件将作为工作流 4 的 change.md 素材（含完整实现代码）

---

## 一、目标

1. 每次 GC 记录事件（触发时机、总耗时、对象/字节变化、回收量），环形缓冲保留最近 64 条
2. `AURA_GC_LOG` 环境变量按**标签 + 级别**过滤输出到 stderr（默认全 Off 零开销）
3. 并发路径输出 gctrace 风格三阶段耗时（STW 根扫描 + 并发标记 + STW 收尾）
4. `gc_stats()` 聚合统计补最近一次 GC 耗时字段

## 二、源码现状（接口契约，逐项核对）

| # | 事实 | 位置 |
| - | ---- | ---- |
| 1 | `GcHeap() = default`（默认构造）；单例 `g_gcHeap`（init_priority(101) 全局构造，main 前完成，getenv 安全） | gc.h L290 / gc.cpp L29-35 |
| 2 | `Stats` 内联结构（无耗时字段）；`getStats() const` | gc.h L234-250 / safepoint.cpp L240-262 |
| 3 | `gc_stats_string()`（fmtBytes 格式化，512B 缓冲） | safepoint.cpp L265-290 |
| 4 | **GC 执行体 A**：单线程分支——flushTlab → needFullGc 判定 → concurrent ? `startConcurrentGc()` : minorGc/mixedGc/majorGc/sweepLargePages → return | safepoint.cpp L80-117 |
| 5 | **GC 执行体 B**：多线程 initiator——抢权 → 等 stopped → needFullGc 判定 → concurrent ? `startConcurrentGc()` : 四件套 → 唤醒 | safepoint.cpp L120-168 |
| 6 | **GC 执行体 C**：`startConcurrentGc()`——级别判定(pendingGcKind_) → waitForRootThreadsStopped+scanRootsOnly（roots）→ 释放线程+Marking → runMarkPhase（mark）→ Finalize+waitForRootThreadsStopped → finalizeMarking（finalize）→ 唤醒+Idle | safepoint.cpp L302-381 |
| 7 | 活跃字节口径：`youngBytes_ + oldBytes_`；活跃对象：`youngObjects_.size() + oldObjects_.size()` | gc.h 成员 |
| 8 | 级别判定优先级（并发路径）：young≥kYoungThreshold/2→0，shouldCompactMedium→1，old≥kOldThreshold→2，否则→3 | safepoint.cpp L304-312 |
| 9 | `std::atomic<bool> markingInProgress_` / `std::atomic<GcPhase> phase_` / 现有 `[GC]` 打印（STW DEADLOCK） | gc.h L589-590 / safepoint.cpp L134 |

## 三、详细实施步骤（含完整代码）

### Step 1：gc.h —— 日志类型/事件结构/成员/方法声明 + Stats 扩展

**1a. `Stats` 加最近耗时字段**（gc.h L248 后）：

```cpp
        size_t mixedGcCount;     // Mixed GC 次数
        uint64_t lastGcMicros;   // 最近一次 GC 总耗时（µs，0=尚无 GC）
```

**1b. GcHeap 新增（public 区，gc.h L290 附近）**：

```cpp
public:
    // ---- GC 事件日志（P3：AURA_GC_LOG 标签+级别细粒度，默认 Off）----
    enum class GcLogLevel : uint8_t { Off = 0, Error, Warning, Info, Debug, Trace };
    enum GcLogTag : uint8_t { kTagGc = 0, kTagPhase, kTagMemory, kTagTrigger, kTagCount };
    // 单次 GC 事件（三个执行体计时构造；环形缓冲保留最近 64 条）
    struct GcEvent {
        uint8_t  kind;           // 0=minor 1=mixed 2=major 3=sweepLarge 4=concurrent
        uint8_t  trigger;        // 0=young阈值 1=碎片率 2=old阈值 3=sweepLarge
        uint64_t startMicros;    // 相对进程启动（steady_clock）
        uint64_t rootsMicros;    // 阶段1：STW 根扫描（并发路径）
        uint64_t markMicros;     // 阶段2：并发标记 / STW 标记
        uint64_t finalizeMicros; // 阶段3：收尾 / STW sweep+compact
        uint64_t totalMicros;    // 总耗时
        size_t   liveBefore, liveAfter;    // 对象数
        size_t   bytesBefore, bytesAfter;  // 活跃字节数（young+old）
        size_t   freedBytes;               // 回收字节（bytesBefore-bytesAfter）
    };
    void logGcEvent(const GcEvent& ev);    // 按标签/级别过滤输出 stderr
    void setGcEventTrigger(uint8_t t) { lastTrigger_ = t; }  // 供执行体记录触发原因
    void parseGcLogEnv();                  // 构造时解析 AURA_GC_LOG
    // gc_events() 可选扩展（工作流 4 阶段 2 决定）：返回最近事件文本
    // GcString* gc_events_string();
```

**1c. private 成员（gc.h 成员区，L597 附近）**：

```cpp
    // ---- GC 事件日志状态 ----
    uint8_t               gcLogLevels_[kTagCount] = {};  // 每标签级别（默认 Off）
    uint8_t               lastTrigger_ = 0;              // 本次 GC 触发原因（执行体判定后设置）
    std::deque<GcEvent>   gcEvents_;                     // 环形缓冲
    std::mutex            gcEventsM_;
```

**1d. 构造函数改显式**（gc.h L290）：

```cpp
    GcHeap();   // 原 GcHeap() = default；定义移 gc.cpp（构造时解析 AURA_GC_LOG）
```

**1e. 方法声明（private 区）**：

```cpp
    void recordGcEvent(uint8_t kind, uint64_t t0, uint64_t tRoots, uint64_t tMark, uint64_t tEnd,
                       size_t liveBefore, size_t bytesBefore);
    // 统一构造 GcEvent：t0 入口、tRoots 根扫描完、tMark 标记完、tEnd 收尾完（µs）
    // liveBefore/bytesBefore 必须为 GC 执行前的值（执行体在 GC 前取样传入）
```

### Step 2：gc.cpp —— GcHeap 构造 + env 解析

```cpp
// ============================================================
// GcHeap 构造 / GC 事件日志 env 解析
// ============================================================
static uint8_t parseGcLogLevel(const std::string& s) {
    if (s == "trace")  return static_cast<uint8_t>(GcHeap::GcLogLevel::Trace);
    if (s == "debug")  return static_cast<uint8_t>(GcHeap::GcLogLevel::Debug);
    if (s == "info")   return static_cast<uint8_t>(GcHeap::GcLogLevel::Info);
    if (s == "warn" || s == "warning") return static_cast<uint8_t>(GcHeap::GcLogLevel::Warning);
    if (s == "error")  return static_cast<uint8_t>(GcHeap::GcLogLevel::Error);
    if (s == "off")    return static_cast<uint8_t>(GcHeap::GcLogLevel::Off);
    return static_cast<uint8_t>(GcHeap::GcLogLevel::Info);  // 未知级别默认 info
}

void GcHeap::parseGcLogEnv() {
    const char* e = std::getenv("AURA_GC_LOG");
    if (!e || !*e) return;  // 未设置 → 全 Off（零开销）
    std::string s(e);
    size_t pos = 0;
    for (;;) {
        size_t comma = s.find(',', pos);
        std::string item = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        size_t eq = item.find('=');
        std::string tag = eq == std::string::npos ? item : item.substr(0, eq);
        std::string lvl = eq == std::string::npos ? "info" : item.substr(eq + 1);
        uint8_t lv = parseGcLogLevel(lvl);
        bool all = !tag.empty() && tag.back() == '*';  // gc* 通配子树
        if (all) tag.pop_back();
        if (tag == "gc") {
            gcLogLevels_[kTagGc] = lv;
            if (all) { gcLogLevels_[kTagPhase] = lv; gcLogLevels_[kTagMemory] = lv; gcLogLevels_[kTagTrigger] = lv; }
        } else if (tag == "gc/phase")   gcLogLevels_[kTagPhase]   = lv;
        else if (tag == "gc/memory")    gcLogLevels_[kTagMemory]  = lv;
        else if (tag == "gc/trigger")   gcLogLevels_[kTagTrigger] = lv;
        else std::fprintf(stderr, "[GC] warning: unknown AURA_GC_LOG tag '%s'\n", tag.c_str());
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    // 子标签已设置但 gc 未设置时，gc 默认 info（对标 Java -Xlog 语义）
    if (gcLogLevels_[kTagGc] == 0 &&
        (gcLogLevels_[kTagPhase] || gcLogLevels_[kTagMemory] || gcLogLevels_[kTagTrigger]))
        gcLogLevels_[kTagGc] = static_cast<uint8_t>(GcLogLevel::Info);
}

GcHeap::GcHeap() { parseGcLogEnv(); }
```

### Step 3：safepoint.cpp —— 事件记录/输出 + 三个执行体计时挂载 + stats 扩展

**3a. 事件记录与输出（safepoint.cpp 新增，getStats 之前）**：

```cpp
// ============================================================
// GC 事件日志（P3）
// ============================================================
static const char* gcKindName(uint8_t kind) {
    static const char* names[] = { "minor", "mixed", "major", "sweepLarge", "concurrent" };
    return kind <= 4 ? names[kind] : "?";
}

// 统一构造 GcEvent 入环形缓冲 + 按标签/级别输出
void GcHeap::recordGcEvent(uint8_t kind, uint64_t t0, uint64_t tRoots,
                           uint64_t tMark, uint64_t tEnd,
                           size_t liveBefore, size_t bytesBefore) {
    GcEvent ev;
    ev.kind           = kind;
    ev.trigger        = lastTrigger_;
    ev.startMicros    = t0;
    ev.rootsMicros    = tRoots - t0;
    ev.markMicros     = tMark - tRoots;
    ev.finalizeMicros = tEnd - tMark;
    ev.totalMicros    = tEnd - t0;
    // liveBefore/bytesBefore 为执行体在 GC 前取样的值；
    // record 时点 GC 已完成，youngObjects_/youngBytes_ 等已是"GC 后"状态
    ev.liveBefore   = liveBefore;
    ev.liveAfter    = youngObjects_.size() + oldObjects_.size();
    ev.bytesBefore  = bytesBefore;
    ev.bytesAfter   = youngBytes_ + oldBytes_;
    ev.freedBytes   = bytesBefore > ev.bytesAfter ? bytesBefore - ev.bytesAfter : 0;
    {
        std::lock_guard<std::mutex> lk(gcEventsM_);
        gcEvents_.push_back(ev);
        while (gcEvents_.size() > 64) gcEvents_.pop_front();
    }
    lastGcMicros_.store(ev.totalMicros, std::memory_order_release);
    logGcEvent(ev);
}

void GcHeap::logGcEvent(const GcEvent& ev) {
    // gc 主行（info）
    if (gcLogLevels_[kTagGc] >= static_cast<uint8_t>(GcLogLevel::Info)) {
        char a1[32], a2[32], f[32];
        std::fprintf(stderr, "[GC][info] %s #%zu @%.3fs: ", gcKindName(ev.kind),
                     gcCount_ + minorGcCount_ + mixedGcCount_ + 1, ev.startMicros / 1e6);
        if (ev.kind == 4)  // 并发路径：三阶段
            std::fprintf(stderr, "%.2f+%.2f+%.2f ms clock",
                         ev.rootsMicros / 1000.0, ev.markMicros / 1000.0, ev.finalizeMicros / 1000.0);
        else
            std::fprintf(stderr, "%.2f ms clock", ev.totalMicros / 1000.0);
        std::fprintf(stderr, ", live %zu->%zu (%s->%s), freed %s\n",
                     ev.liveBefore, ev.liveAfter,
                     fmtBytes(ev.bytesBefore, a1, sizeof(a1)),
                     fmtBytes(ev.bytesAfter,  a2, sizeof(a2)),
                     fmtBytes(ev.freedBytes,  f,  sizeof(f)));
    }
    // gc/phase（debug，仅并发路径有阶段分解）
    if (ev.kind == 4 && gcLogLevels_[kTagPhase] >= static_cast<uint8_t>(GcLogLevel::Debug)) {
        std::fprintf(stderr, "[GC][debug][phase] concurrent: roots=%.2fms mark=%.2fms finalize=%.2fms\n",
                     ev.rootsMicros / 1000.0, ev.markMicros / 1000.0, ev.finalizeMicros / 1000.0);
    }
    // gc/trigger（debug）
    if (gcLogLevels_[kTagTrigger] >= static_cast<uint8_t>(GcLogLevel::Debug)) {
        static const char* trig[] = { "youngBytes>=threshold", "compactMedium", "oldBytes>=threshold", "sweepLargePages" };
        std::fprintf(stderr, "[GC][debug][trigger] %s #%zu: %s\n", gcKindName(ev.kind),
                     gcCount_ + minorGcCount_ + mixedGcCount_ + 1,
                     ev.trigger <= 3 ? trig[ev.trigger] : "forceGc");
    }
}
```

**3b. 私有成员补充**（gc.h Step 1c 基础上加）：

```cpp
    std::atomic<uint64_t>     lastGcMicros_{0};  // 最近一次 GC 耗时（gc_stats 用）
```

**3c. getStats 补字段**（safepoint.cpp L260 后）：

```cpp
    s.mixedGcCount = mixedGcCount_;
    s.lastGcMicros = lastGcMicros_.load(std::memory_order_acquire);
    return s;
```

**3d. gc_stats_string 加耗时**（safepoint.cpp L280-289）：

```cpp
    char buf[576];
    std::snprintf(buf, sizeof(buf),
        "GC: alloc=%s young=%s old=%s gc=%zu minor=%zu mixed=%zu live=%zu pages=%zu "
        "medium=%zu large=%zu freeMed=%zu los=%zu/%s last=%.2fms",
        ...原有参数...,
        s.losObjects, fmtBytes(s.losBytes, lbuf, sizeof(lbuf)),
        s.lastGcMicros / 1000.0);
```

**3e. 执行体 A（单线程分支）计时挂载**（safepoint.cpp L93-104）：

```cpp
        if (needFullGc) {
            if (concurrentGcEnabled_) {
                startConcurrentGc();  // 并发路径内部自行计时记录（见 3g）
            } else {
                auto t0 = std::chrono::steady_clock::now();
                uint64_t us0 = std::chrono::duration_cast<std::chrono::microseconds>(t0.time_since_epoch()).count();
                size_t liveBefore = youngObjects_.size() + oldObjects_.size();
                size_t bytesBefore = youngBytes_ + oldBytes_;
                // 保守版：保留原顺序多段执行（不改变触发语义），kind 按首个触发
                uint8_t kind0 = 3;
                if (youngBytes_ >= kYoungThreshold / 2) { kind0 = 0; setGcEventTrigger(0); minorGc(); }
                if (shouldCompactMedium() && !compactSuspendedCount_.load()) { kind0 = 1; setGcEventTrigger(1); mixedGc(); }
                if (oldBytes_ >= kOldThreshold) { kind0 = 2; setGcEventTrigger(2); majorGc(); }
                if (shouldSweepLargePages() && !compactSuspendedCount_.load()) { kind0 = 3; setGcEventTrigger(3); sweepLargePages(); }
                auto te = std::chrono::steady_clock::now();
                uint64_t use = std::chrono::duration_cast<std::chrono::microseconds>(te.time_since_epoch()).count();
                recordGcEvent(kind0, us0, use, use, use, liveBefore, bytesBefore);
            }
        } else if (needCompactOnly) {
            ...原样...
        }
```

> 注：原实现是"顺序执行多段 GC"（可能 minor+mixed 都触发）；**保守版保留多段顺序语义**（首个触发判定 kind 仅用于展示），行为与现状一致——**实施采用保守版**（不改变现有 GC 触发语义）。

**3f. 执行体 B（多线程 initiator）计时挂载**（safepoint.cpp L151-159，同保守版模式）：

```cpp
        if (needFullGc) {
            if (concurrentGcEnabled_) {
                startConcurrentGc();  // 内部计时
            } else {
                auto t0 = std::chrono::steady_clock::now();
                uint64_t us0 = std::chrono::duration_cast<std::chrono::microseconds>(t0.time_since_epoch()).count();
                size_t liveBefore = youngObjects_.size() + oldObjects_.size();
                size_t bytesBefore = youngBytes_ + oldBytes_;
                uint8_t kind0 = 3;
                if (youngBytes_ >= kYoungThreshold / 2) { kind0 = 0; setGcEventTrigger(0); minorGc(); }
                if (shouldCompactMedium() && !compactSuspendedCount_.load()) { kind0 = 1; setGcEventTrigger(1); mixedGc(); }
                if (oldBytes_ >= kOldThreshold) { kind0 = 2; setGcEventTrigger(2); majorGc(); }
                if (shouldSweepLargePages() && !compactSuspendedCount_.load()) { kind0 = 3; setGcEventTrigger(3); sweepLargePages(); }
                auto te = std::chrono::steady_clock::now();
                uint64_t use = std::chrono::duration_cast<std::chrono::microseconds>(te.time_since_epoch()).count();
                recordGcEvent(kind0, us0, use, use, use, liveBefore, bytesBefore);
            }
        } else if (needCompactOnly) { ...原样... }
```

**3g. 执行体 C（startConcurrentGc）分阶段计时**（safepoint.cpp L302 起，函数级重构——只加计时点，逻辑不动）：

```cpp
void GcHeap::startConcurrentGc() {
    // 计时：t0 入口（级别判定前）
    auto t0 = std::chrono::steady_clock::now();
    uint64_t us0 = std::chrono::duration_cast<std::chrono::microseconds>(t0.time_since_epoch()).count();
    size_t liveBefore = youngObjects_.size() + oldObjects_.size();
    size_t bytesBefore = youngBytes_ + oldBytes_;

    // ---- 级别判定（不变）----
    if (youngBytes_ >= kYoungThreshold / 2) { pendingGcKind_ = 0; setGcEventTrigger(0); }
    else if (shouldCompactMedium() && !compactSuspendedCount_.load()) { pendingGcKind_ = 1; setGcEventTrigger(1); }
    else if (oldBytes_ >= kOldThreshold) { pendingGcKind_ = 2; setGcEventTrigger(2); }
    else { pendingGcKind_ = 3; setGcEventTrigger(3); }

    // ---- 根扫描（不变）----
    waitForRootThreadsStopped();
    scanRootsOnly(false);
    auto tRoots = std::chrono::steady_clock::now();
    uint64_t usRoots = std::chrono::duration_cast<std::chrono::microseconds>(tRoots.time_since_epoch()).count();

    // ---- 释放线程 + Marking 设置（不变）----
    int threadCount;
    { std::lock_guard<std::mutex> lk(threads_m_);
      threadCount = static_cast<int>(registered_threads_.size()); }
    if (threadCount <= 0) threadCount = 1;
    phase_.store(GcPhase::Marking, std::memory_order_release);
    markingInProgress_.store(true, std::memory_order_release);
    { std::lock_guard<std::mutex> lk(all_stopped_m_);
      stopped_threads_.store(0);
      gc_epoch_.fetch_add(1);
      all_stopped_cv_.notify_all(); }

    // ---- 阶段 2：并发标记（不变）----
    runMarkPhase();
    auto tMark = std::chrono::steady_clock::now();
    uint64_t usMark = std::chrono::duration_cast<std::chrono::microseconds>(tMark.time_since_epoch()).count();

    // ---- 阶段 3：收尾（不变）----
    stopped_threads_.store(0);
    phase_.store(GcPhase::Finalize, std::memory_order_release);
    gcPending_.store(true, std::memory_order_release);
    waitForRootThreadsStopped();
    finalizeMarking();
    auto tEnd = std::chrono::steady_clock::now();
    uint64_t usEnd = std::chrono::duration_cast<std::chrono::microseconds>(tEnd.time_since_epoch()).count();
    // 事件记录（kind=4 concurrent）
    recordGcEvent(4, us0, usRoots, usMark, usEnd, liveBefore, bytesBefore);

    // ---- 清标志 + 唤醒（不变）----
    { std::lock_guard<std::mutex> lk(all_stopped_m_);
      stopped_threads_.store(0);
      gc_in_progress_.store(false);
      gc_epoch_.fetch_add(1);
      all_stopped_cv_.notify_all(); }
    phase_.store(GcPhase::Idle, std::memory_order_release);
    gcPending_.store(false);
}
```

> ⚠️ **recordGcEvent 中 kind=4 的序号**：`gcCount_+minorGcCount_+mixedGcCount_+1`——并发路径的收尾 finalizeMarking 已对 pendingGcKind_ 对应计数 +1（case 0/1 补计数），但 `gcCount_`（major 计数）在 finalizeMarking 的 case 2 里才 +1；序号非严格递增，仅作展示——**可接受**（环形缓冲序号无需连续）。若需精确序号，改用内部 `gcEventSeq_` 自增计数器（本方案不引入，保持最小改动）。

### Step 4（可选）：`gc_events()` 内置函数（工作流 4 阶段 2 视需要决定）

注册模式同 `gc_force`（BuiltinRegistry），返回最近 64 条事件文本（按时间序），供 Aura 代码内调试。

## 四、影响分析

| 维度 | 影响 |
| ---- | ---- |
| 行为 | 默认全 Off 时**零行为变化**（gcLogLevels_ 全 0，logGcEvent 短路；recordGcEvent 仅环形缓冲 push，开销 ~ns 级） |
| GC 触发语义 | 保守版保留原"顺序多段执行"（3e 注）——**不改变任何触发/执行顺序** |
| 锁 | 新增 gcEventsM_（仅缓冲 push/pop 用，GC 执行体非热点路径）；无锁序新依赖 |
| 线程安全 | 计时均在 GC 执行者（initiator）线程；输出也在 initiator（无并发写 stderr） |
| 内存 | GcEvent 约 72B × 64 条 ≈ 4.6KB 常驻（可忽略） |
| 编译 | gc.h 需 `<deque>`（已有，P2 引入）；gc.cpp 需 `<cstdlib>`（getenv，已有） |
| 并发路径 | 计时点插入不触碰 phase/停止协议任何原子操作，无竞态 |

## 五、边界条件

| 边界 | 处理 |
| ---- | ---- |
| 未设置 AURA_GC_LOG | parseGcLogEnv 直接 return，全 Off |
| 未知标签 | stderr warning 一次（不 crash） |
| 未知级别 | 默认 info |
| `gc*` 通配 | tag 去尾 `*` 后匹配 gc，子树同级别 |
| 子标签设置但 gc 未设 | gc 默认 info（Java -Xlog 语义） |
| 环形缓冲溢出 | 超 64 pop_front |
| STW 路径阶段字段 | roots=mark=total（单段；格式输出单值 `X ms clock`） |
| 并发路径序号 | 展示用序号可能非连续（见 3g 注，不引入计数器） |
| GC 前 bytes 口径 | 执行体入口（flushTlab 后）youngBytes_+oldBytes_；record 时点已 GC 完，before 取自局部 |

## 六、测试方案

1. **test.aura**：`io.println(gc_stats())`——断言含 `last=...ms` 字段
2. **env 组合**（AURA_GC_LOG 驱动 test.exe）：
   - `gc=info`：stderr 出现 `[GC][info] minor/major/concurrent #... : ... ms clock` 行
   - `gc/phase=debug`：仅 `[GC][debug][phase]` 行（gc 主体不输出——验证标签过滤）
   - `gc*=trace`：gc 主行 + phase + trigger 全输出
   - 未设置：无任何 `[GC][` 输出（默认 Off）
3. **并发路径**：P2 用例运行输出 `concurrent ... a+b+c ms clock`（三阶段）
4. **回归**：scripts/verify_concurrent_gc.ps1 全量（13 项）不回归
5. **ASAN**（可选，改动小可省）：日志路径无新增堆内存

## 七、风险

| 风险 | 应对 |
| ---- | ---- |
| fmtBytes 静态函数位于 safepoint.cpp，logGcEvent 同文件可见 ✓ | 若将来移到其他文件需前置声明 |
| recordGcEvent 的序号/kind 语义与计数耦合 | 展示用，不依赖精确计数；环形缓冲为调试主通道 |
| env 解析在全局构造（init_priority 101）| 仅 getenv + std::string，无依赖（std 可用）——安全 |
| 保守版 vs 判定版行为差异 | 实施采用保守版（保留原多段顺序语义），零行为变化 |
