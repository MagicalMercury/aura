# Plan：sync 锁族 v1.1 实施方案（RWMutex / Once）

> 来源：[plan/mutex_plan.md](file:///d:/you/Aura/plan/mutex_plan.md) §4
> 范围：sync.RWMutex / sync.Once，复用 v1.0 的 LockStmt，零 AST/Parser 改动
> 关键约束：所有锁族 Guard 必须 safepoint 感知（v1.0 死锁修复确立，见 [plan/done/gc_mutex_deadlock_fix_report.md](file:///d:/you/Aura/plan/done/gc_mutex_deadlock_fix_report.md)）
> 注：WaitGroup 已从 v1.1 移除（sync_thread_context 自动等待使其冗余，推到 v1.2 重新设计）

---

## 一、runtime/builtin/mutex.h（续写，新增 RWMutex/Once）

在现有 `Mutex` 之后、`} // namespace aura_rt` 之前追加：

```cpp
// ============================================================
// RWMutex — 读写锁（多读单写）
//
// 用法：lock (rw.r()) { } / lock (rw.w()) { }
// 关键设计（v1.0 死锁修复确立）：
//   1. Inner::m 用 std::timed_mutex（非 std::mutex），支持 try_lock 轮询
//   2. 删除 cv：避免 TSan 误报 + STW 死锁
//   3. Guard 构造用 try_lock 轮询 + gc_safepoint()
//   4. GcRootHandle<RWMutex*> 防 compact 搬迁悬垂 + locked_ 标志防 TSan 误报
// ============================================================
struct RWMutex : GcObject {
    struct Inner {
        std::timed_mutex m;
        std::atomic<int> readers{0};
        std::atomic<bool> writer_active{false};
        std::atomic<int> waiting_writers{0};  // 等待中的 writer 数量（写优先，防 reader starve writer）
    };
    Inner* inner_;  // 间接指针，指向 new 出的 Inner

    static const TypeDescriptor _desc;

    // 读锁守卫：多读并发，与读互斥不与写互斥
    class ReadGuard {
    public:
        explicit ReadGuard(RWMutex* rw)
            : rw_(rw), gcRoot_(rw_), locked_(false) {
            // try_lock 轮询 + safepoint 响应 STW
            while (true) {
                if (rw_->inner_->m.try_lock()) {
                    // 写优先：若有 writer 等待或活跃，reader 让出
                    if (!rw_->inner_->writer_active.load(std::memory_order_acquire)
                        && rw_->inner_->waiting_writers.load(std::memory_order_acquire) == 0) {
                        rw_->inner_->readers.fetch_add(1, std::memory_order_acq_rel);
                        rw_->inner_->m.unlock();
                        locked_ = true;
                        return;
                    }
                    rw_->inner_->m.unlock();
                }
                gc_safepoint();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        ~ReadGuard() {
            if (locked_ && rw_) {
                rw_->inner_->readers.fetch_sub(1, std::memory_order_acq_rel);
                locked_ = false;
            }
        }
        ReadGuard(ReadGuard&& o) noexcept
            : rw_(o.rw_), gcRoot_(rw_), locked_(o.locked_) {
            gcRoot_.rebind(rw_);
            o.rw_ = nullptr;
            o.locked_ = false;
        }
        ReadGuard(const ReadGuard&) = delete;
        ReadGuard& operator=(const ReadGuard&) = delete;
        ReadGuard& operator=(ReadGuard&&) = delete;
    private:
        RWMutex* rw_;
        GcRootHandle<RWMutex*> gcRoot_;
        bool locked_;
    };

    // 写锁守卫：独占，与读写都互斥
    class WriteGuard {
    public:
        explicit WriteGuard(RWMutex* rw)
            : rw_(rw), gcRoot_(rw_), locked_(false) {
            // 标记 writer 等待中，让新 reader 让出（写优先，防 starve）
            rw_->inner_->waiting_writers.fetch_add(1, std::memory_order_acq_rel);
            while (true) {
                if (rw_->inner_->m.try_lock()) {
                    if (rw_->inner_->readers.load(std::memory_order_acquire) == 0
                        && !rw_->inner_->writer_active.load(std::memory_order_acquire)) {
                        rw_->inner_->writer_active.store(true, std::memory_order_release);
                        rw_->inner_->m.unlock();
                        locked_ = true;
                        // 已获取写锁，退出"等待中"状态
                        rw_->inner_->waiting_writers.fetch_sub(1, std::memory_order_acq_rel);
                        return;
                    }
                    rw_->inner_->m.unlock();
                }
                gc_safepoint();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        ~WriteGuard() {
            if (locked_ && rw_) {
                rw_->inner_->writer_active.store(false, std::memory_order_release);
                locked_ = false;
            }
        }
        WriteGuard(WriteGuard&& o) noexcept
            : rw_(o.rw_), gcRoot_(rw_), locked_(o.locked_) {
            gcRoot_.rebind(rw_);
            o.rw_ = nullptr;
            o.locked_ = false;
        }
        WriteGuard(const WriteGuard&) = delete;
        WriteGuard& operator=(const WriteGuard&) = delete;
        WriteGuard& operator=(WriteGuard&&) = delete;
    private:
        RWMutex* rw_;
        GcRootHandle<RWMutex*> gcRoot_;
        bool locked_;
    };

    // r()/w() 返回锁视图（临时对象，由 __acquire_lock 消费）
    // 构造时即获取锁，move 到 _guard 后由 _guard 析构释放
    ReadGuard r() { return ReadGuard(this); }
    WriteGuard w() { return WriteGuard(this); }
};

// __acquire_lock 重载：消费 r()/w() 返回的临时 Guard
inline RWMutex::ReadGuard __acquire_lock(RWMutex::ReadGuard&& v) {
    return std::move(v);
}
inline RWMutex::WriteGuard __acquire_lock(RWMutex::WriteGuard&& v) {
    return std::move(v);
}

// 工厂函数：sync.RWMutex() 构造调用生成
inline RWMutex* make_rwmutex() {
    auto* rw = static_cast<RWMutex*>(
        GcHeap::instance().alloc(sizeof(RWMutex), &RWMutex::_desc));
    rw->inner_ = new RWMutex::Inner();
    return rw;
}

// ============================================================
// Once — 一次性执行
//
// 用法：lock (once) { ... }（首次执行块体，后续跳过）
// 关键设计：不用 std::call_once（内部 mutex 阻塞，无法响应 STW），
//          改用 atomic<bool> + timed_mutex 双检查 + try_lock 轮询
// ============================================================
struct Once : GcObject {
    std::timed_mutex* m_;        // 间接指针（timed_mutex 不可移动）
    std::atomic<bool>* done_;    // 间接指针（atomic<bool> 不可移动）

    static const TypeDescriptor _desc;

    template <typename F>
    void do_(F&& f) {
        // fast path：已完成直接返回（无锁）
        if (done_->load(std::memory_order_acquire)) return;

        // 慢路径：try_lock 轮询 + safepoint 响应 STW
        while (!m_->try_lock()) {
            if (done_->load(std::memory_order_acquire)) return;
            gc_safepoint();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // RAII 守卫：异常安全，确保 f() 抛异常时 m_ 也能 unlock
        // 否则 m_ 永久持有锁 → 其他线程死锁在 try_lock 轮询
        std::lock_guard<std::timed_mutex> lk(*m_, std::adopt_lock);

        // 双检查：持锁后再检查 done_
        if (!done_->load(std::memory_order_acquire)) {
            f();
            done_->store(true, std::memory_order_release);
        }
    }
};

// 工厂函数：sync.Once() 构造调用生成
inline Once* make_once() {
    auto* o = static_cast<Once*>(
        GcHeap::instance().alloc(sizeof(Once), &Once::_desc));
    o->m_ = new std::timed_mutex();
    o->done_ = new std::atomic<bool>(false);
    return o;
}

// 注：WaitGroup 已从 v1.1 移除，推到 v1.2 重新设计
// 原因：sync_thread_context 析构已自动 waitGroup（thread_pool.cpp:201-203），
//       sync thread 块结束即等待所有 spawn 完成，WaitGroup 在此设计下冗余。
//       v1.2 将重新设计（可能引入全局 spawn，让 WaitGroup 成为等待机制）。
```

---

## 二、runtime/builtin/mutex.cpp（新增三个终结器 + TypeDescriptor）

在现有 `Mutex::_desc` 定义之后追加：

```cpp
// 终结器：GC 回收 RWMutex 时释放间接持有的 Inner
static void rwmutex_finalizer(GcObject* o) {
    auto* rw = static_cast<RWMutex*>(o);
    delete rw->inner_;
    rw->inner_ = nullptr;
}

// 终结器：GC 回收 Once 时释放 m_ 和 done_
static void once_finalizer(GcObject* o) {
    auto* once = static_cast<Once*>(o);
    delete once->m_;
    delete once->done_;
    once->m_ = nullptr;
    once->done_ = nullptr;
}

const TypeDescriptor RWMutex::_desc = {
    sizeof(RWMutex), 0, nullptr, 0, nullptr, rwmutex_finalizer
};
const TypeDescriptor Once::_desc = {
    sizeof(Once), 0, nullptr, 0, nullptr, once_finalizer
};
```

---

## 三、src/Sema/BuiltinRegistry.h（init() 注册新类型/方法/构造函数）

### 3.1 types_ 初始化列表追加（在 `{"Mutex", ...}` 之后）

```cpp
{"RWMutex",         {"RWMutex",         true, true, BuiltinPrim::Other, "aura_rt::RWMutex*"}},
{"Once",            {"Once",            true, true, BuiltinPrim::Other, "aura_rt::Once*"}},
// 虚拟类型：r()/w() 返回的锁视图，仅用于 Sema 类型推断和 L1 检查
// 不是堆类型，用户不能直接声明
{"RWMutexReadView", {"RWMutexReadView", false, false, BuiltinPrim::Other, "aura_rt::RWMutex::ReadGuard"}},
{"RWMutexWriteView",{"RWMutexWriteView",false, false, BuiltinPrim::Other, "aura_rt::RWMutex::WriteGuard"}},
```

### 3.2 methods_ 初始化列表追加（在 channel 方法之后）

```cpp
// --- RWMutex 方法：r()/w() 返回锁视图（无参数）---
{"RWMutex", "r", {}, ReturnTypeInfo::Named("RWMutexReadView")},
{"RWMutex", "w", {}, ReturnTypeInfo::Named("RWMutexWriteView")},
```

### 3.3 functions_ 初始化列表追加（在 `{"sync.Mutex", ...}` 之后）

```cpp
{"sync.RWMutex",   {}, ReturnTypeInfo::Named("RWMutex")},
{"sync.Once",      {}, ReturnTypeInfo::Named("Once")},
```

---

## 四、src/Sema/Checker/StmtChecker.cpp（checkLockStmt L1 扩展）

替换 `checkLockStmt` 的 L1 检查部分（第 355-384 行）：

```cpp
void SemAnalyzer::checkLockStmt(const LockStmt& stmt) {
    // L1: lockExpr 类型检查（v1.1 扩展为 Mutex/RWMutexReadView/RWMutexWriteView/Once）
    if (stmt.lockExpr) {
        auto lockTy = inferExpr(*stmt.lockExpr);
        if (!lockTy) {
            error(*stmt.lockExpr, "cannot infer lock expression type");
            return;
        }
        // 识别合法锁类型
        bool isLockType = false;
        if (auto* gs = dynamic_cast<const GenericSemType*>(lockTy.get())) {
            if (gs->name == "Mutex" || gs->name == "RWMutexReadView"
                || gs->name == "RWMutexWriteView" || gs->name == "Once") {
                isLockType = true;
            }
        }
        if (!isLockType) {
            error(*stmt.lockExpr,
                "lock requires sync.Mutex/RWMutex.r()/.w()/Once, got '"
                + lockTy->toString() + "'");
            return;
        }
        // 标注 lockExpr 的 inferredType（供 CodeGen 读取分派）
        const_cast<ASTNode*>(stmt.lockExpr.get())->inferredType = lockTy.get();
        typeStore_.push_back(std::move(lockTy));
    }

    // 进入 lock 块：设置标志，检查 body
    bool oldInLock = inLockBlock_;
    inLockBlock_ = true;
    if (stmt.body) checkBlock(*stmt.body);
    inLockBlock_ = oldInLock;
}
```

---

## 五、src/CodeGen/StmtGen.cpp（genLockStmt 增加 Once 分派）

替换 `genLockStmt`（第 843-854 行）：

```cpp
void CodeGenerator::genLockStmt(std::ostream& cpp, const LockStmt& stmt,
                                  bool /*isCoroutine*/) {
    std::string lockExpr = stmt.lockExpr ? genExpr(*stmt.lockExpr, false) : "";

    // 由 Sema 推断的 lockExpr 类型分派
    std::string typeName;
    if (stmt.lockExpr && stmt.lockExpr->inferredType) {
        if (auto* gs = dynamic_cast<const GenericSemType*>(stmt.lockExpr->inferredType)) {
            typeName = gs->name;
        }
    }

    if (typeName == "Once") {
        // lock (once) { body } → once->do_([&] { body })
        writeLine(cpp, lockExpr + "->do_([&] {");
        indentLevel_++;
        if (stmt.body) genBlock(cpp, *stmt.body, false);
        indentLevel_--;
        writeLine(cpp, "});");
        return;
    }

    // 默认：Mutex / RWMutexReadView / RWMutexWriteView —— RAII guard
    cpp << indentStr() << "{\n";
    indentLevel_++;
    writeLine(cpp, "auto _guard = aura_rt::__acquire_lock(" + lockExpr + ");");
    if (stmt.body) genBlock(cpp, *stmt.body, false);
    indentLevel_--;
    cpp << indentStr() << "}\n";
}
```

注：`GenericSemType` 头文件需要在 StmtGen.cpp 中可见。若未 include，需确认 `#include "../Sema/SemType.h"` 或通过其他头文件间接包含（StmtChecker.cpp 已用此类型，通常已包含）。

---

## 六、src/CodeGen/ExprGen.cpp（sync.RWMutex/Once 构造调用）

在 `genMethodCall` 开头的 sync.Mutex 特判之后（第 654 行后），追加两个构造分派：

```cpp
// sync.RWMutex() / sync.Once() 构造特殊处理
if (auto* id = dynamic_cast<const Identifier*>(e.object.get())) {
    if (id->name == "sync" && e.args.empty()) {
        if (e.method == "Mutex") {
            return "aura_rt::make_mutex()";
        }
        if (e.method == "RWMutex") {
            return "aura_rt::make_rwmutex()";
        }
        if (e.method == "Once") {
            return "aura_rt::make_once()";
        }
    }
}
```

将原第 651-654 行的 `if (id->name == "sync" && e.method == "Mutex" ...)` 替换为上述扩展版本。

注：`rw.r()` / `rw.w()` 走通用方法调用路径，无需特殊处理（生成 `rw->r()` 等）。

---

## 七、example/test.aura（测试代码）

```aura
fun main(io: Io) {
    # === Test 1: Once 并发一次性执行 ===
    io.println("=== Test 1: Once ===")
    let once = sync.Once()
    let m = sync.Mutex()
    let counter = [0]

    sync thread(max = 4) {
        for i in range(100) {
            spawn (io: Io, once: sync.Once, m: sync.Mutex, counter: [int], i: int) {
                lock (once) {
                    lock (m) {
                        counter.append(i)
                    }
                    io.println_sync("init once, i=" + i)
                }
            }
        }
    }

    lock (m) {
        io.println_sync("Test1: counter.len=" + counter.len() + " (once executed)")
    }

    # === Test 2: RWMutex 读写锁 ===
    io.println("=== Test 2: RWMutex ===")
    let rw = sync.RWMutex()
    let data = [0]

    sync thread(max = 4) {
        for i in range(100) {
            spawn (rw: sync.RWMutex, data: [int], i: int) {
                lock (rw.w()) {
                    data.append(i)
                }
            }
        }
    }

    lock (rw.r()) {
        io.println_sync("Test2: data.len=" + data.len() + " (expect 100)")
    }

    # === Test 3: RWMutex 公平性（写优先，防 reader starve writer）===
    io.println("=== Test 3: RWMutex fairness ===")
    let rw2 = sync.RWMutex()
    let writeCount = [0]

    sync thread(max = 4) {
        # 持续读 + 偶尔写，验证 writer 不会被 reader starve
        for i in range(500) {
            spawn (rw: sync.RWMutex, writeCount: [int], i: int) {
                if (i % 50 == 0) {
                    lock (rw.w()) {
                        writeCount.append(i)
                    }
                } else {
                    lock (rw.r()) {
                        # 读操作，空 body（仅验证 reader 能进入）
                    }
                }
            }
        }
    }

    io.println_sync("Test3: writeCount.len=" + writeCount.len() + " (expect 10, writer not starved)")

    # === Test 4: 并发 GC 压力测试（Mutex + RWMutex + Once 混合）===
    io.println("=== Test 4: concurrent GC stress ===")
    let m4 = sync.Mutex()
    let rw4 = sync.RWMutex()
    let once4 = sync.Once()
    let counter4 = [0]

    sync thread(max = 4) {
        for i in range(1000) {
            spawn (m: sync.Mutex, rw: sync.RWMutex, once: sync.Once, counter: [int], i: int) {
                lock (m) {
                    counter.append(i)
                }
                if (i % 100 == 0) {
                    lock (rw.w()) {
                        # 写锁压力
                    }
                    lock (once) {
                        # Once 在 GC 压力下也只执行一次
                        counter.append(-1)
                    }
                    gc_force()
                }
            }
        }
    }

    lock (m4) {
        io.println_sync("Test4: counter4.len=" + counter4.len() + " (expect 1000+10, once executed 10 times but only first counted)")
    }

    io.println_sync("=== All tests done ===")
}
```

**验收**：
- Test1：输出 100 行 `init once, i=N`（每个 i 仅一次），`counter.len=100`
- Test2：`data.len=100`（100 个 spawn，写锁串行 append）
- Test3：`writeCount.len=10`（500 中 10 个 writer，写优先防 starve）
- Test4：`counter4.len` ≥ 1000（1000 个 append + Once 仅首次执行）
- 5 次连续运行无死锁、无 abort、无 ASAN 报错

---

## 八、实施步骤

| 步骤 | 模块 | 文件 | 依赖 |
|:---|:---|:---|:---|
| 1 | 运行时 | `runtime/builtin/mutex.h` 续写 RWMutex（含写优先公平性）/ Once | v1.0 完成 |
| 2 | 运行时 | `runtime/builtin/mutex.cpp` 续写两个终结器 + TypeDescriptor | 步骤 1 |
| 3 | Sema | `src/Sema/BuiltinRegistry.h` 注册类型/方法/构造函数 | 步骤 1 |
| 4 | Sema | `src/Sema/Checker/StmtChecker.cpp` checkLockStmt L1 扩展 | 步骤 3 |
| 5 | CodeGen | `src/CodeGen/StmtGen.cpp` genLockStmt 增加 Once 分派 | 步骤 4 |
| 6 | CodeGen | `src/CodeGen/ExprGen.cpp` sync.RWMutex/Once 构造调用 | 步骤 3 |
| 7 | **Safepoint 验收** | `Select-String gc_safepoint runtime/builtin/mutex.h` 期望 Count ≥ 4 | 步骤 1-6 |
| 8 | 构建 | `cmake --build build` + `cmake --build runtime/build` | 步骤 1-7 |
| 9 | 测试 | `example/test.aura` 写入测试代码，编译运行 | 步骤 8 |
| 10 | **稳定性验收** | 连续 5 次运行无死锁、无 abort、无 TSan 警告 | 步骤 9 |

---

## 九、潜在风险与应对

### 9.1 RWMutex 公平性问题（已修复）
- **风险**：原方案 reader 可能 starve writer（reader 持续进入时 writer 无法获取）
- **修复**：Inner 新增 `waiting_writers` 原子计数器，实现写优先：
  - WriteGuard 构造时 `waiting_writers++`，获取锁后 `waiting_writers--`
  - ReadGuard 进入前检查 `waiting_writers == 0`，若有 writer 等待则让出
  - 效果：writer 等待时新 reader 会让出，writer 必然能获取锁（不会 starve）

### 9.2 Once 的 do_ 模板与 GC
- **风险**：`once->do_([&] { body })` 中 lambda 捕获的变量若触发 GC，可能 compact 搬迁
- **应对**：body 内部的 GC 指针变量已由 CodeGen 包装为 GcRootHandle（与 v1.0 一致），无需特殊处理

### 9.3 GenericSemType 头文件依赖
- **风险**：StmtGen.cpp 中 `dynamic_cast<const GenericSemType*>` 需要头文件可见
- **应对**：若编译报错，添加 `#include "../Sema/SemType.h"`；StmtChecker.cpp 已使用此类型，通常已通过间接包含可见

### 9.4 ReadGuard/WriteGuard GcRootHandle 悬垂（已修复）
- **风险**：原方案 `gcRoot_(rw)` 引用构造函数参数 `rw`（栈临时变量），构造函数结束后 `rw` 销毁，`gcRoot_.ptr_` 悬垂。GC compact 时 `updateAllReferences` 解引用悬垂指针 → UB
- **修复**：改为 `gcRoot_(rw_)` 引用成员 `rw_`（生命周期与 Guard 一致）。移动构造同样改为 `gcRoot_(rw_)`，并调用 `gcRoot_.rebind(rw_)` 重新绑定
- **对比**：v1.0 [Mutex::Guard](file:///d:/you/Aura/runtime/builtin/mutex.h#L44) 已正确使用 `gcRoot_(m_)` 引用成员

### 9.5 Once::do_ 异常不安全 → 永久死锁（已修复）
- **风险**：原方案 `f()` 抛异常时 `m_->unlock()` 不执行 → `m_` 永久持有锁 → 其他线程死锁在 `while (!m_->try_lock())` 轮询
- **修复**：用 `std::lock_guard<std::timed_mutex> lk(*m_, std::adopt_lock)` RAII 保护，确保异常时也能 unlock。`adopt_lock` 表示已持有锁，构造时不再次获取，析构时释放
- **关键**：`lock_guard` 必须在 `try_lock` 成功后、`f()` 调用前构造

### 9.6 WaitGroup 设计冗余（已移除）
- **问题**：`sync_thread_context` 析构已自动调用 `waitGroup`（[thread_pool.cpp:201-203](file:///d:/you/Aura/runtime/thread_pool.cpp#L201)），sync thread 块结束即等待所有 spawn 完成，WaitGroup 在此设计下冗余
- **应对**：v1.1 移除 WaitGroup，推到 v1.2 重新设计（可能引入全局 spawn，让 WaitGroup 成为等待机制）

---

## 十、总改动量估算

| 文件 | 改动行数 |
|:---|:---|
| runtime/builtin/mutex.h | +130 行（RWMutex 含写优先 + Once） |
| runtime/builtin/mutex.cpp | +20 行（2 个终结器 + TypeDescriptor） |
| src/Sema/BuiltinRegistry.h | +10 行（types/methods/functions 追加） |
| src/Sema/Checker/StmtChecker.cpp | +10 行（L1 扩展） |
| src/CodeGen/StmtGen.cpp | +15 行（Once 分派） |
| src/CodeGen/ExprGen.cpp | +8 行（2 个构造调用） |
| example/test.aura | +90 行（测试代码） |
| **合计** | **~280 行** |
