# 多线程：`sync thread` 语句实施方案

> 来源：用户指示 — sync 块是协程（单线程协作式），多线程需要独立的 `sync thread` 语句
> 关键字策略：`thread` 为**软关键字**（contextual keyword），非保留字
> 日期：2026-07-18 草案 / 2026-07-24 详细实施方案 / 2026-07-24 实施完成
> 状态：✅ 已完成（v1，含 TLAB + 全部 GC 并发数据竞争修复）
> 前置：[thread_pool_work_stealing_issue.md](file:///d:/you/Aura/plan/thread_pool_work_stealing_issue.md) 已审核通过（方案 A FIFO 全局队列）
> 关键决策：
>   - **进程级全局线程池**（非块级临时池）
>   - **无界 sync thread 默认上限 = hardware_concurrency**（强制保护）
>   - **safepoint 三层插入**：L1 分配点（已有）/ L2 循环回边（新增）/ L3 任务边界（新增）
>   - **TLAB（Thread-Local Allocation Buffer）**：每线程独立分配缓冲，无锁快路径（详见 plan/done/TLAB_implementation.md）

---

## 实施完成总结（2026-07-24）

### 完成内容
- ✅ Phase 0：L2 循环回边 safepoint 插入（genWhileStmt/genForStmt/genLoopStmt/genSyncForStmt）
- ✅ Phase 1：AST + Parser（SyncStmt::isThread + thread 软关键字识别）
- ✅ Phase 2：Sema 检查（R1-R4 全部实现）
- ✅ Phase 3：运行时线程池（ThreadPool + sync_thread_context + L3 safepoint）
- ✅ Phase 4：CodeGen（genSyncThreadStmt + genSpawnAsThread + ioSync_ 强制）
- ✅ TLAB：每线程独立分配缓冲（详见 plan/done/TLAB_implementation.md）

### 配套修复（多线程稳定性）
1. **GC 并发数据竞争**：
   - compactSuspendedCount_ → std::atomic<int>
   - gcPending_ → std::atomic<bool>
   - oomInit_ → std::atomic<bool>（call_once 不可行，递归自死锁）
   - 新增 stackRootsM_ 互斥锁保护 stackRoots_
   - 新增 rememberedSetM_ 互斥锁保护 rememberedSet_
2. **roots_ (unordered_set) 并发 UAF**：新增 rootsM_ 互斥锁
3. **intern_string 持锁 alloc 导致 STW 死锁**：改为"不持锁 alloc"模式（读锁查找 → 解锁 alloc → 写锁 double-check insert）
4. **cout 无锁并发**：新增 g_coutM 互斥锁保护 println/println_sync
5. **线程池 ensureStarted 无锁初始化**：用 std::call_once + ensureStartedOnce_ 保护
6. **MinGW std::shared_mutex bug**：g_internMutex 从 shared_mutex 改为 mutex
   - 参考：https://github.com/msys2/MINGW-packages/issues/25193
   - 现象：lock_shared() 抛 "__ret == 0" 断言
   - 修复：读路径改用独占锁（find() 耗时极小）
7. **worker 空闲时不响应 GC**：workerLoop 用 cv_.wait_for 超时循环，定期释放锁并调用 gc_safepoint

### 验证结果
test.aura 全部 3 个测试通过，退出码 0：
- 测试 1：基本多线程（无界）2 行 "hello from thread N"
- 测试 2：有界并发 max=4，10 行 "task N"（0-9）
- 测试 3：GC 压力 max=2，100 行 "string N"（0-99）+ done
- 输出顺序有少量交错（如 string 5 在 string 4 前），属多线程并发正常现象，无数据损坏

### 已知限制（v1）
- ❌ 不支持 `sync thread` 嵌套
- ❌ 不支持 `sync thread` 内 `await`
- ❌ 不支持 `sync thread` 内调用返回 `task<T>` 的函数
- ❌ 不支持 `sync thread` 内 `return` / `break` / `continue`
- ❌ `sync thread` 块内的 GC 暂停只在 L2/L3 safepoint 触发（非并发 GC）
- ❌ 无界 `sync thread` 任务数受 `hardware_concurrency` 限制（防止资源耗尽）

### 后续工作
- P3：线程池 work-stealing 任务队列（待基准测试触发，详见 plan/thread_pool_work_stealing_issue.md）
- P3：channel<T> 跨线程通信（详见 plan/channel_thread_issue.md，⏳ 计划中未来文件 plan/channel_thread_plan.md 待创建）
- 审计报告剩余项（P0-P3）：见 out.txt

---

## 一、总体架构

### 1.1 执行模型对比

| 语句 | 执行模型 | 并行性 | 阻塞点 | GC 协作 |
|:---|:---|:---|:---|:---|
| `sync { spawn { ... } }` | 协程（C++20 coroutine） | ❌ 单线程协作式 | `co_await when_all` | 栈根 GcRootHandle |
| `sync thread { ... }` | **OS 线程**（全局线程池） | ✅ 真正并行 | `thread_pool::wait_all()` | **每线程独立 GC 根 + STW** |
| `sync thread(max=N) { ... }` | OS 线程 + 信号量限流 | ✅ 真正并行 | 同上 | 同上 |

### 1.2 进程级全局线程池设计

**为什么不用块级临时池**：
- 避免 `sync thread` 块结束时反复创建/销毁线程的开销
- 多个 `sync thread` 块可复用已存在的工作线程
- 与 GC stop-the-world 集中管理（一次性暂停所有工作线程）

**架构示意**：

```
┌─────────────────────────────────────────────────────────────┐
│              ThreadPool (全局单例, 进程级)                  │
│                                                             │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐  │
│  │ Worker 0 │  │ Worker 1 │  │ Worker 2 │  │ Worker N │  │
│  │ (常驻)   │  │ (常驻)   │  │ (常驻)   │  │ (常驻)   │  │
│  └─────┬────┘  └─────┬────┘  └─────┬────┘  └─────┬────┘  │
│        │             │             │             │         │
│        └──────┬──────┴─────────────┴─────────────┘         │
│               ▼                                             │
│       ┌───────────────┐                                     │
│       │ FIFO 全局队列 │  ← spawn 提交 task                  │
│       │  + mutex      │                                     │
│       └───────┬───────┘                                     │
│               │                                             │
│        condition_variable                                   │
│        notify_one/all                                       │
└─────────────────────────────────────────────────────────────┘
                      ▲
                      │
   sync thread { spawn ... } → thread_pool::submit(task)
   sync thread 块结束      → thread_pool::wait_all(group_id)
```

**关键参数**：
- 工作线程数：默认 `hardware_concurrency`，用户可通过 `sync thread(max=N)` 限制并发数（不增加工作线程）
- 队列：单一 `std::deque<std::function<void()>>` + `std::mutex`（方案 A，见 issue 决策）
- 无界保护：无 `max` 时，`sync thread` 块的 spawn 任务数 ≤ `hardware_concurrency`，超出报错

### 1.3 safepoint 三层插入策略

| 层级 | 位置 | 作用 | 现状 |
|:---|:---|:---|:---|
| **L1** | 内存分配点（`gc_alloc` 内） | 分配时检测 `gcPending_`，触发 GC | ✅ 已有（[gc.cpp:60-68 tryAlloc](file:///d:/you/Aura/runtime/gc.cpp#L60)） |
| **L2** | 循环回边（for/while/loop 末尾） | 防止长循环阻塞 GC 暂停 | ❌ 新增（单线程也受益） |
| **L3** | 任务边界（线程池 worker 取下一个 task 前） | 多线程下确保 worker 能被暂停 | ❌ 新增（仅多线程） |

**为什么选 L2 循环回边而非函数调用点**：
- 函数调用点插入开销大（每次调用都检查 `gcPending_`）
- 循环回边是热点，但检查只是一个 atomic load（`gcPending_` 为 `std::atomic<bool>`）
- 长循环是阻塞 GC 的主要场景，L2 直接命中
- L3 在线程池 worker 循环顶部检查，覆盖非循环的 CPU 密集任务

---

## 二、AST 设计

### 2.1 复用 `SyncStmt`，新增 `isThread` 标志

**文件**：[src/AST/Stmt.h](file:///d:/you/Aura/src/AST/Stmt.h)

```cpp
struct SyncStmt : Stmt {
    std::unique_ptr<BlockStmt> body;
    std::unique_ptr<ASTNode> maxExpr;
    bool isThread = false;  // 新增：true 表示 sync thread，false 表示 sync
    // ... 现有字段不变
};
```

**理由**：语法结构一致（body + 可选 maxExpr），Parser 只需检测 `thread` 软关键字并设标志，CodeGen 根据 `isThread` 分派。

### 2.2 `thread` 软关键字解析

**文件**：[src/Parser/StmtParser.cpp](file:///d:/you/Aura/src/Parser/StmtParser.cpp) `parseSyncStmt`

```cpp
std::unique_ptr<ASTNode> Parser::parseSyncStmt() {
    auto syncTok = consume();  // 'sync'
    auto* stmt = new SyncStmt();
    stmt->line = syncTok.line;
    stmt->col = syncTok.col;

    // 检测 thread 软关键字
    if (check(TokType::Ident) && current().text == "thread") {
        consume();  // 'thread'
        stmt->isThread = true;
    }

    // 统一处理 (max=N)
    if (match(TokType::LParen)) {
        consume();
        expectIdent("max");
        match(TokType::Assign);
        stmt->maxExpr = parseExpr();
        expect(TokType::RParen);
    }

    stmt->body = parseBlockStmt();
    return std::unique_ptr<ASTNode>(stmt);
}
```

**影响**：Lexer 无需改动，`thread` 仍按普通标识符解析。

### 2.3 AST 打印支持

**文件**：[src/AST/Stmt.cpp](file:///d:/you/Aura/src/AST/Stmt.cpp) `SyncStmt::print`

在打印 `sync` 后追加 `thread`（若 `isThread`）。

---

## 三、Sema 检查

### 3.1 新增检查规则

| 规则 | 说明 | 错误信息 |
|:---|:---|:---|
| **R1** | `sync thread` 不能嵌套 `sync thread` | "nested sync thread not allowed" |
| **R2** | `sync thread` 内禁止 `await` | "cannot use await inside sync thread block" |
| **R3** | `sync thread` 内的 `spawn` 必须显式传参 | "spawn in sync thread must have explicit params" |
| **R4** | `sync thread(max=N)` 中 N 必须是正整数 | "max must be positive integer" |
| **R5** | 无界 `sync thread` 的 spawn 任务数 ≤ hardware_concurrency | "unbounded sync thread: task count exceeds hardware_concurrency"（运行时检查） |

### 3.2 Sema 实现

**文件**：[src/Sema/SemAnalyzer.h](file:///d:/you/Aura/src/Sema/SemAnalyzer.h) 新增成员：

```cpp
bool inSyncThreadBlock_ = false;  // 当前是否在 sync thread 块内
```

**文件**：[src/Sema/Checker/StmtChecker.cpp](file:///d:/you/Aura/src/Sema/Checker/StmtChecker.cpp) `checkSyncStmt`：

```cpp
void SemAnalyzer::checkSyncStmt(const SyncStmt& s) {
    if (s.isThread) {
        // R1: 禁止嵌套
        if (inSyncThreadBlock_) {
            error(s, "nested sync thread not allowed");
            return;
        }
        bool old = inSyncThreadBlock_;
        inSyncThreadBlock_ = true;
        checkBlock(*s.body);
        inSyncThreadBlock_ = old;

        // R4: maxExpr 类型检查
        if (s.maxExpr) {
            auto t = inferExpr(*s.maxExpr);
            if (!isIntType(*t)) error(*s.maxExpr, "max must be positive integer");
        }
        return;
    }
    // ... 原有 sync 检查逻辑（不动）
}
```

**R2 实现**：在 `checkAwaitExpr` 中检查 `inSyncThreadBlock_`，若为 true 则报错。

**R3 实现**：在 `checkSpawnStmt` 中检查 `inSyncThreadBlock_`，若为 true 且 `stmt.params.empty()` 则报错。

---

## 四、运行时设计

### 4.1 全局线程池

**新增文件**：`runtime/thread_pool.h`

```cpp
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace aura_rt {

// 全局线程池（进程级单例）
class ThreadPool {
public:
    static ThreadPool& instance();

    // 启动工作线程（懒初始化，首次 submit 时调用）
    void ensureStarted(size_t workerCount = 0);  // 0 = hardware_concurrency

    // 提交任务到 FIFO 全局队列
    void submit(std::function<void()> task);

    // 等待当前 group 的所有任务完成
    // group_id 由 beginGroup() 返回，submit 时关联
    uint64_t beginGroup();
    void submitInGroup(uint64_t groupId, std::function<void()> task);
    void waitGroup(uint64_t groupId);

    // 关闭（进程退出时调用）
    void shutdown();

    // worker 数量
    size_t workerCount() const { return workers_.size(); }

private:
    ThreadPool() = default;
    ~ThreadPool();

    void workerLoop(size_t idx);

    std::vector<std::thread> workers_;
    std::deque<std::pair<uint64_t, std::function<void()>>> tasks_;  // (groupId, task)
    std::mutex m_;
    std::condition_variable cv_;
    bool stop_ = false;

    // group 等待机制
    std::atomic<uint64_t> nextGroupId_{0};
    std::unordered_map<uint64_t, std::atomic<int>> groupPending_;  // groupId → 未完成任务数
    std::mutex groupM_;
    std::condition_variable groupCv_;
};

} // namespace aura_rt
```

### 4.2 worker 循环（含 L3 safepoint）

**新增文件**：`runtime/thread_pool.cpp`

```cpp
#include "thread_pool.h"
#include "gc.h"

namespace aura_rt {

ThreadPool& ThreadPool::instance() {
    static ThreadPool pool;
    return pool;
}

void ThreadPool::ensureStarted(size_t workerCount) {
    if (!workers_.empty()) return;
    if (workerCount == 0) workerCount = std::thread::hardware_concurrency();
    if (workerCount == 0) workerCount = 4;  // fallback
    workers_.reserve(workerCount);
    for (size_t i = 0; i < workerCount; ++i) {
        workers_.emplace_back([this, i]{ workerLoop(i); });
    }
}

void ThreadPool::submit(std::function<void()> task) {
    ensureStarted();
    {
        std::lock_guard<std::mutex> lk(m_);
        tasks_.emplace_back(0, std::move(task));  // groupId=0 表示无 group
    }
    cv_.notify_one();
}

uint64_t ThreadPool::beginGroup() {
    uint64_t id = nextGroupId_.fetch_add(1);
    std::lock_guard<std::mutex> lk(groupM_);
    groupPending_[id] = 0;
    return id;
}

void ThreadPool::submitInGroup(uint64_t groupId, std::function<void()> task) {
    ensureStarted();
    {
        std::lock_guard<std::mutex> lk(m_);
        tasks_.emplace_back(groupId, std::move(task));
    }
    {
        std::lock_guard<std::mutex> lk(groupM_);
        if (groupPending_.count(groupId))
            groupPending_[groupId].fetch_add(1);
    }
    cv_.notify_one();
}

void ThreadPool::waitGroup(uint64_t groupId) {
    std::unique_lock<std::mutex> lk(groupM_);
    groupCv_.wait(lk, [&]{
        auto it = groupPending_.find(groupId);
        return it == groupPending_.end() || it->second.load() == 0;
    });
    groupPending_.erase(groupId);
}

void ThreadPool::workerLoop(size_t idx) {
    // 注册到 GC（用于 STW 暂停）
    GcHeap::instance().registerThread(std::this_thread::get_id());

    while (true) {
        std::pair<uint64_t, std::function<void()>> task;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [&]{ return stop_ || !tasks_.empty(); });
            if (stop_ && tasks_.empty()) break;
            task = std::move(tasks_.front());
            tasks_.pop_front();
        }

        // L3 safepoint：取下一个 task 前检查 GC 暂停请求
        gc_safepoint();

        try {
            task.second();
        } catch (...) {
            // 异常由 group 聚合机制处理（见 4.3）
            // 简单实现：记录到 group 的异常列表
        }

        // 通知 group 完成
        if (task.first != 0) {
            std::lock_guard<std::mutex> lk(groupM_);
            auto it = groupPending_.find(task.first);
            if (it != groupPending_.end() && it->second.fetch_sub(1) == 1) {
                groupCv_.notify_all();
            }
        }
    }

    GcHeap::instance().unregisterThread(std::this_thread::get_id());
}

void ThreadPool::shutdown() {
    {
        std::lock_guard<std::mutex> lk(m_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
    workers_.clear();
}

ThreadPool::~ThreadPool() {
    shutdown();
}

} // namespace aura_rt
```

### 4.3 异常聚合（简化版）

为简化 v1 实现，异常处理采用**首个异常传播**策略：

```cpp
// sync_thread_context：块级辅助类
class sync_thread_context {
public:
    sync_thread_context() : groupId_(ThreadPool::instance().beginGroup()) {}
    ~sync_thread_context() { ThreadPool::instance().waitGroup(groupId_); }

    void submit(std::function<void()> task) {
        ThreadPool::instance().submitInGroup(groupId_, std::move(task));
    }

private:
    uint64_t groupId_;
};
```

**异常传播**：worker 内 `try/catch` 捕获异常后存入 `std::vector<std::exception_ptr>`，`waitGroup` 后检查并 `std::rethrow_exception` 第一个。

### 4.4 GC stop-the-world 集成

**已有实现**（[runtime/gc.cpp:184-224 safepoint](file:///d:/you/Aura/runtime/gc.cpp#L184)）：
- `GcHeap::safepoint()` 检查 `gcPending_`
- 单线程：直接执行 GC
- 多线程：抢 GC 锁 → 等待所有线程到达 safepoint → 执行 GC → 唤醒

**已有 API**（[runtime/gc.h:184-185](file:///d:/you/Aura/runtime/gc.h#L184)）：
```cpp
void registerThread(std::thread::id id);
void unregisterThread(std::thread::id id);
```

**worker 集成**：见 4.2 的 `workerLoop`，已在入口注册、出口注销。

### 4.5 L2 循环回边 safepoint 插入

**文件**：[src/CodeGen/StmtGen.cpp](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp)

修改 `genWhileStmt`、`genForStmt`、`genLoopStmt`，在循环体末尾（`}` 之前）插入 `aura_rt::gc_safepoint();`：

**genWhileStmt**（当前 [StmtGen.cpp:376-381](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L376)）：

```cpp
void CodeGenerator::genWhileStmt(std::ostream& cpp, const WhileStmt& stmt,
                                  bool isCoroutine) {
    cpp << indentStr() << "while (" << genExpr(*stmt.condition, isCoroutine) << ") {\n";
    if (stmt.body) genBlock(cpp, *stmt.body, isCoroutine);
    writeLine(cpp, "aura_rt::gc_safepoint();");  // L2 safepoint
    cpp << indentStr() << "}\n";
}
```

**genForStmt**（当前 [StmtGen.cpp:383-436](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L383)）：

所有分支（range 1参/2参/3参、channel、默认数组遍历）在 `}` 前插入：

```cpp
    if (stmt.body) genBlock(cpp, *stmt.body, isCoroutine);
    writeLine(cpp, "aura_rt::gc_safepoint();");  // L2 safepoint
    cpp << indentStr() << "}\n";
```

**genLoopStmt**（当前 [StmtGen.cpp:438-443](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L438)）：

```cpp
void CodeGenerator::genLoopStmt(std::ostream& cpp, const LoopStmt& stmt,
                                 bool isCoroutine) {
    cpp << indentStr() << "while (true) {\n";
    if (stmt.body) genBlock(cpp, *stmt.body, isCoroutine);
    writeLine(cpp, "aura_rt::gc_safepoint();");  // L2 safepoint
    cpp << indentStr() << "}\n";
}
```

**性能考量**：`gc_safepoint()` 在单线程下仅检查 `gcPending_`（`bool` 读取，[gc.cpp:185](file:///d:/you/Aura/runtime/gc.cpp#L185)），无 GC 请求时直接 return，开销极低。

**注意**：`genSyncForStmt` 内的 for 循环（[StmtGen.cpp:603-671](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L603)）也需要插入 L2 safepoint，位置在 `indentLevel_--` 之前。

---

## 五、CodeGen 设计

### 5.1 新增标志

**文件**：[src/CodeGen/CodeGen.h](file:///d:/you/Aura/src/CodeGen/CodeGen.h)（在 [L417 `insideSpawn_`](file:///d:/you/Aura/src/CodeGen/CodeGen.h#L417) 附近新增）：

```cpp
bool inSyncThreadBlock_ = false;  // 当前是否在 sync thread 块内（控制 spawn 生成分派）
```

### 5.2 `genSyncStmt` 分派 `isThread`

**文件**：[src/CodeGen/StmtGen.cpp](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp)（修改 [genSyncStmt L578-601](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L578)）：

```cpp
void CodeGenerator::genSyncStmt(std::ostream& cpp, const SyncStmt& stmt,
                                 bool /*isCoroutine*/) {
    if (stmt.isThread) {
        genSyncThreadStmt(cpp, stmt);
        return;
    }
    // ... 原有 sync 协程逻辑（不动）
}

void CodeGenerator::genSyncThreadStmt(std::ostream& cpp, const SyncStmt& stmt) {
    cpp << indentStr() << "{\n";
    indentLevel_++;

    // 无界保护：默认上限 = hardware_concurrency
    // 用户可通过 max=N 限制并发数（但不增加工作线程）
    std::string maxN = stmt.maxExpr ? genExpr(*stmt.maxExpr, false) : "0";
    writeLine(cpp, "aura_rt::sync_thread_context _stx;");
    if (stmt.maxExpr) {
        writeLine(cpp, "aura_rt::ThreadPool::instance().ensureStarted(" + maxN + ");");
    } else {
        writeLine(cpp, "aura_rt::ThreadPool::instance().ensureStarted();");
    }

    // 生成块体（spawn 会被分派到 genSpawnAsThread）
    bool oldInSyncThread = inSyncThreadBlock_;
    inSyncThreadBlock_ = true;
    if (stmt.body) genBlock(cpp, *stmt.body, false);  // 非协程！
    inSyncThreadBlock_ = oldInSyncThread;

    // 块结束：等待所有线程完成，触发 GC
    writeLine(cpp, "aura_rt::gc_safepoint();");
    // sync_thread_context 析构会调用 waitGroup
    indentLevel_--;
    cpp << indentStr() << "}\n";
}
```

### 5.3 `genSpawnStmt` 分派

**文件**：[src/CodeGen/StmtGen.cpp](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp)（修改 [genSpawnStmt L673-767](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L673)）：

```cpp
void CodeGenerator::genSpawnStmt(std::ostream& cpp, const SpawnStmt& stmt,
                                  bool /*isCoroutine*/) {
    if (inSyncThreadBlock_) {
        genSpawnAsThread(cpp, stmt);
        return;
    }
    // ... 原有协程 spawn 逻辑（不动）
}

void CodeGenerator::genSpawnAsThread(std::ostream& cpp, const SpawnStmt& stmt) {
    // 生成 lambda + submit 到线程池
    // 关键：捕获列表为空 []，参数显式传值（与协程 spawn 一致，避免悬垂引用）
    // 关键：强制 ioSync_ = true（sync thread 内不能用 co_await）
    // 关键：worker 入口/出口由 ThreadPool 管理，GC registerThread 已在 workerLoop 完成

    bool oldIoSync = ioSync_;
    ioSync_ = true;  // 强制 _sync 版本

    writeLine(cpp, "_stx.submit([](");
    // 参数列表：用户声明的显式参数
    for (size_t i = 0; i < stmt.params.size(); ++i) {
        if (i > 0) cpp << ", ";
        cpp << (stmt.params[i].type ? mapParamType(*stmt.params[i].type) : "auto")
            << " " << safeName(stmt.params[i].name);
    }
    // 自动追加 io（若用户未声明）
    bool hasIo = false;
    for (auto& p : stmt.params) if (p.name == "io") hasIo = true;
    if (!hasIo) cpp << ", aura_rt::Io& io";
    cpp << ") {");

    indentLevel_++;
    insideSpawn_ = true;
    for (auto& s : stmt.body)
        if (s) genStmt(cpp, *s, false);  // 非协程！
    insideSpawn_ = false;
    writeLine(cpp, "});");
    indentLevel_--;

    // 调用参数
    writeLine_noIndent(cpp, "}(");
    // ... 实参列表（与原有 genSpawnStmt 一致）
    cpp << "));\n";

    ioSync_ = oldIoSync;
}
```

### 5.4 关键约束实现

#### 约束 1：`io.xxx` 必须用 `_sync` 版本

在 `genSpawnAsThread` 中设置 `ioSync_ = true`，`genMethodCall` 检测到该标志后会生成 `io.println_sync(...)` 而非 `io.println(...)`。

**文件**：[src/CodeGen/ExprGen.cpp](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp)（已有 `ioSync_` 判定逻辑，无需修改）。

#### 约束 2：GC 根必须线程局部

**现状确认**：`GcRootHandle` 的 `roots_` 是 `std::unordered_set<GcRootHandle<GcObject*>*>`（[gc.h:301](file:///d:/you/Aura/runtime/gc.h#L301)），所有线程共享同一个集合。

**设计**：每个 worker 线程在自己的栈上创建 `GcRootHandle`，自动注册到全局 `roots_`。GC 在 STW 时遍历 `roots_`，由于所有 worker 已暂停，访问安全。

**线程局部分配**（TLAB）：当前 `bumpAlloc` 是全局的，多线程并发分配需要加锁。v1 采用**全局 mutex 保护 bumpAlloc**（简单但性能低），未来可优化为 TLAB。

**修改**：[runtime/gc.cpp bumpAlloc L120-132](file:///d:/you/Aura/runtime/gc.cpp#L120) 加 `std::lock_guard<std::mutex>` 保护。

#### 约束 3：无界 sync thread 资源保护

**运行时检查**：`sync_thread_context` 内部计数 spawn 次数，若超过 `hardware_concurrency`（无 max 时）抛出 `RuntimeError`。

```cpp
class sync_thread_context {
    std::atomic<int> spawnCount_{0};
    int maxAllowed_;  // = maxExpr ? maxExpr : hardware_concurrency
public:
    void submit(std::function<void()> task) {
        if (spawnCount_.fetch_add(1) >= maxAllowed_) {
            spawnCount_.fetch_sub(1);
            throw std::runtime_error("sync thread: task count exceeds limit");
        }
        ThreadPool::instance().submitInGroup(groupId_, std::move(task));
    }
};
```

---

## 六、实施步骤（分阶段）

### Phase 0: L2 循环回边 safepoint（前置，单线程也受益）

| Step | 内容 | 文件 |
|:---|:---|:---|
| 0.1 | `genWhileStmt` 体末插入 `gc_safepoint()` | [StmtGen.cpp:376-381](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L376) |
| 0.2 | `genForStmt` 所有分支体末插入 | [StmtGen.cpp:383-436](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L383) |
| 0.3 | `genLoopStmt` 体末插入 | [StmtGen.cpp:438-443](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L438) |
| 0.4 | `genSyncForStmt` 内 for 循环体末插入 | [StmtGen.cpp:603-671](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L603) |

**验收**：现有测试全部通过，长循环场景 GC 能及时触发。

**依赖**：无（可独立完成）。

### Phase 1: AST + Parser

| Step | 内容 | 文件 |
|:---|:---|:---|
| 1.1 | `SyncStmt` 加 `bool isThread = false` | [AST/Stmt.h](file:///d:/you/Aura/src/AST/Stmt.h) |
| 1.2 | `parseSyncStmt` 检测 `thread` 软关键字 | [Parser/StmtParser.cpp](file:///d:/you/Aura/src/Parser/StmtParser.cpp) |
| 1.3 | `print` 方法支持 `sync thread` | [AST/Stmt.cpp](file:///d:/you/Aura/src/AST/Stmt.cpp) |

**验收**：能解析 `sync thread { ... }` 和 `sync thread(max=N) { ... }`，AST 输出正确。

**依赖**：无。

### Phase 2: Sema 检查

| Step | 内容 | 文件 |
|:---|:---|:---|
| 2.1 | `SemAnalyzer` 加 `inSyncThreadBlock_` 标志 | [Sema/SemAnalyzer.h](file:///d:/you/Aura/src/Sema/SemAnalyzer.h) |
| 2.2 | `checkSyncStmt` 分派 `isThread` 分支 + R1/R4 | [Sema/Checker/StmtChecker.cpp](file:///d:/you/Aura/src/Sema/Checker/StmtChecker.cpp) |
| 2.3 | `checkAwaitExpr` 加 R2 检查 | [Sema/Checker/StmtChecker.cpp](file:///d:/you/Aura/src/Sema/Checker/StmtChecker.cpp) |
| 2.4 | `checkSpawnStmt` 加 R3 检查 | [Sema/Checker/StmtChecker.cpp](file:///d:/you/Aura/src/Sema/Checker/StmtChecker.cpp) |

**验收**：嵌套 sync thread 报错；sync thread 内 await 报错；spawn 无参数报错。

**依赖**：Phase 1。

### Phase 3: 运行时线程池

| Step | 内容 | 文件 |
|:---|:---|:---|
| 3.1 | 实现 `ThreadPool` 类（FIFO 队列 + worker 循环 + L3 safepoint） | runtime/thread_pool.h（新增）, runtime/thread_pool.cpp（新增） |
| 3.2 | 实现 `sync_thread_context`（group + 无界保护） | runtime/thread_pool.h |
| 3.3 | `bumpAlloc` 加 mutex 保护（多线程并发分配） | [runtime/gc.cpp:120-132](file:///d:/you/Aura/runtime/gc.cpp#L120) |
| 3.4 | CMakeLists.txt 加入 thread_pool.cpp | [runtime/CMakeLists.txt](file:///d:/you/Aura/runtime/CMakeLists.txt) |

**验收**：单元测试多线程 submit + waitGroup；多线程 + GC 不 crash。

**依赖**：Phase 0（L2 safepoint 已就位，GC 可及时暂停）。

### Phase 4: CodeGen

| Step | 内容 | 文件 |
|:---|:---|:---|
| 4.1 | `CodeGenerator` 加 `inSyncThreadBlock_` 标志 | [CodeGen.h:417 附近](file:///d:/you/Aura/src/CodeGen/CodeGen.h#L417) |
| 4.2 | `genSyncStmt` 分派 `isThread` → `genSyncThreadStmt` | [StmtGen.cpp:578-601](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L578) |
| 4.3 | `genSpawnStmt` 分派 → `genSpawnAsThread` | [StmtGen.cpp:673-767](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L673) |
| 4.4 | `genSpawnAsThread` 实现（强制 ioSync_ + submit） | 同上 |
| 4.5 | #include "thread_pool.h" 到生成的 C++ 头部 | [CodeGen.cpp preamble](file:///d:/you/Aura/src/CodeGen/CodeGen.cpp) |

**验收**：编译并运行以下 Aura 代码：

```aura
fun main(io: Io) {
    let arr = [1, 2, 3, 4, 5]
    sync thread(max = 3) {
        for n in arr {
            spawn (io: Io, n: int) {
                io.println("processing " + n)
            }
        }
    }
}
```

输出：5 行 "processing N"（顺序可能不同，但都出现）。

**依赖**：Phase 1, 2, 3。

### Phase 5: channel<T> 跨线程（后置，独立 plan）

| Step | 内容 |
|:---|:---|
| 5.1 | 实现 `aura_rt::Channel<T>`（mutex + cv + 关闭广播） |
| 5.2 | Parser + Sema + CodeGen 支持 `channel<T>` 类型 |
| 5.3 | `for val in ch` 在 sync thread 中生成阻塞 receive 循环 |

**验收**：生产者-消费者模式跨线程工作。

**依赖**：Phase 4。详见 `plan/channel_thread_issue.md`（issue 已起草，⏳ 详细实施方案 plan/channel_thread_plan.md 待创建）。

---

## 七、实施顺序与依赖关系

```
Phase 0 (L2 safepoint) ──┐
                         ├──> Phase 3 (线程池) ──┐
Phase 1 (AST/Parser) ────┤                       │
                         ├──> Phase 2 (Sema) ───┤──> Phase 4 (CodeGen) ──> Phase 5 (channel)
                         │                       │
                         └───────────────────────┘
```

**并行机会**：
- Phase 0 与 Phase 1/2 可并行（互不依赖）
- Phase 3 与 Phase 1/2 可并行（接口已定义）
- Phase 4 必须在 1/2/3 全部完成后

**关键路径**：Phase 0 → Phase 3 → Phase 4

---

## 八、风险与应对

### 8.1 主要风险

| 风险 | 影响 | 应对 |
|:---|:---|:---|
| **GC 多线程崩溃** | worker 线程访问移动后的对象 | L2/L3 safepoint 确保 STW；bumpAlloc 加锁；Phase 3 单元测试覆盖 |
| **bumpAlloc 锁竞争** | 多线程分配性能下降 | v1 接受；未来引入 TLAB（见 [TODO.txt §五 TLAB](file:///d:/you/Aura/TODO.txt)） |
| **io._sync 版本不完整** | 编译失败 | 已确认所有 io 方法有 _sync 版本（[io.h](file:///d:/you/Aura/runtime/builtin/io.h)） |
| **数据竞争** | GC 对象跨线程修改 | spawn 参数值传递；GC 对象通过指针共享时用户需自行加锁（v1 不提供语言级锁） |
| **线程池死锁** | waitGroup 永不返回 | worker 内 try/catch 确保 task 异常不阻塞；shutdown 时 notify_all |

### 8.2 明确限制（v1）

- ❌ 不支持 `sync thread` 嵌套
- ❌ 不支持 `sync thread` 内 `await`
- ❌ 不支持 `sync thread` 内调用返回 `task<T>` 的函数
- ❌ 不支持 `sync thread` 内 `return` / `break` / `continue`
- ❌ `sync thread` 块内的 GC 暂停只在 L2/L3 safepoint 触发（非并发 GC）
- ❌ 无界 `sync thread` 任务数受 `hardware_concurrency` 限制（防止资源耗尽）

---

## 九、测试方案

### 9.1 单元测试（Phase 3）

```cpp
// runtime/test_thread_pool.cpp
void test_basic_submit() {
    auto& pool = ThreadPool::instance();
    std::atomic<int> counter{0};
    auto gid = pool.beginGroup();
    for (int i = 0; i < 100; ++i) {
        pool.submitInGroup(gid, [&]{ counter++; });
    }
    pool.waitGroup(gid);
    assert(counter == 100);
}

void test_gc_stress() {
    auto& pool = ThreadPool::instance();
    auto gid = pool.beginGroup();
    for (int i = 0; i < 1000; ++i) {
        pool.submitInGroup(gid, [&]{
            // 触发 GC 分配
            auto s = aura_rt::make_string("test");
            (void)s;
        });
    }
    pool.waitGroup(gid);
    // 不 crash 即通过
}
```

### 9.2 集成测试（Phase 4）

**文件**：`example/test.aura`（按 [AGENTS.md 项目约定](file:///d:/you/Aura/AGENTS.md#L7)）

```aura
fun main(io: Io) {
    // 测试 1：基本多线程
    sync thread {
        spawn (io: Io) { io.println("hello from thread 1") }
        spawn (io: Io) { io.println("hello from thread 2") }
    }

    // 测试 2：有界并发
    sync thread(max = 4) {
        for i in range(10) {
            spawn (io: Io, i: int) {
                io.println("task " + i)
            }
        }
    }

    // 测试 3：GC 压力（多线程分配）
    sync thread(max = 2) {
        for i in range(100) {
            spawn (io: Io, i: int) {
                let s = "string " + i
                io.println(s)
            }
        }
    }
}
```

**验收标准**：
- 所有测试输出正确行数（顺序可能不同）
- 无 crash、无 ASAN 报错
- `gc_stats()` 显示 GC 正常触发

---

## 十、与 TODO.txt 的对应

本 plan 对应 [TODO.txt §十](file:///d:/you/Aura/TODO.txt) 的：

```
[ ] P1  sync thread 多线程语句
      - plan：plan/sync_thread_plan.md（本文件）
      - 前置：L2 循环回边 safepoint（Phase 0）
      - 文件：src/AST/Stmt.h, src/Parser/StmtParser.cpp,
              src/Sema/Checker/StmtChecker.cpp, src/CodeGen/StmtGen.cpp,
              runtime/thread_pool.h (新增), runtime/thread_pool.cpp (新增),
              runtime/gc.h, runtime/gc.cpp
```

实施完成后，[TODO.txt §十](file:///d:/you/Aura/TODO.txt) P1 项标记为 `[x]`。

---

## 十一、总结

**核心设计决策**：
1. **进程级全局线程池**：复用工作线程，避免反复创建/销毁
2. **FIFO 全局队列**：方案 A，实现简单，覆盖 Aura 主用例（见 issue 决策）
3. **safepoint 三层插入**：L1 分配点 + L2 循环回边 + L3 任务边界，确保 GC 及时暂停
4. **无界保护**：默认上限 = `hardware_concurrency`，防止资源耗尽
5. **worker 自动注册 GC**：`workerLoop` 入口 `registerThread`，出口 `unregisterThread`
6. **spawn 强制 ioSync_**：sync thread 内不能用 co_await，必须用 _sync 版本

**改动规模预估**：
- AST/Parser：~60 行
- Sema：~80 行
- CodeGen：~200 行（含 Phase 0 safepoint 插入）
- 运行时：~250 行（ThreadPool + sync_thread_context + bumpAlloc 加锁）
- 测试：~80 行

**前置依赖**：无（GC STW 已在 [gc.cpp:184-224](file:///d:/you/Aura/runtime/gc.cpp#L184) 实现，Phase 0 的 L2 safepoint 是增强而非前置）。
