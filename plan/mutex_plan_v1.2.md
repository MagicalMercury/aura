# Plan：sync 锁族 v1.2 详细实施方案

> 类型：详细实施方案（plan，已根据源码细化分析）
> 日期：2026-07-27
> 状态：等待审查
> 前置：[mutex_plan.md](file:///d:/you/Aura/plan/mutex_plan.md)（v1.0 / v1.1 已完成）
> 关联：[plan/done/gc_mutex_deadlock_fix_report.md](file:///d:/you/Aura/plan/done/gc_mutex_deadlock_fix_report.md)（STW 死锁修复经验）
> 关联 TODO：[TODO.txt](file:///d:/you/Aura/TODO.txt) §十（v1.2 进行中、WaitGroup v1.3、Cond/Semaphore v1.3）

---

## 4.1 元数据

- **Plan 标题**：sync 锁族 v1.2 演进（多锁语句 + RWMutex 批量唤醒 + L5 运行时递归检测）
- **作者**：Agent
- **日期**：2026-07-27
- **相关模块**：
  - `src/AST/Stmt.h`、`src/AST/Stmt.cpp`（LockStmt 节点扩展）
  - `src/Parser/StmtParser.cpp`（parseLockStmt 多表达式列表）
  - `src/Sema/Checker/StmtChecker.cpp`（L1/L8/L9 编译期规则）
  - `src/CodeGen/StmtGen.cpp`（genLockStmt 运行时排序后加锁）
  - `runtime/builtin/mutex.h`、`runtime/builtin/mutex.cpp`（Inner 字段扩展 + Guard 递归检测 + reader 唤醒机制）

## 4.2 目标

v1.2 在 v1.0/v1.1 已完成的 Mutex/RWMutex/Once 基础上引入三项增强：
1. **多锁语句** `lock (a.mu, b.mu) { }`，由 CodeGen 自动按地址排序后加锁，消除人为锁序反转死锁
2. **RWMutex 批量唤醒 reader**（方向 2），writer 释放后让等待的 reader 尽快批量进入读临界区，提升高并发读吞吐
3. **L5 嵌套同锁检测**改为运行时 error，覆盖编译期无法识别的别名递归持锁场景

## 4.3 当前状态摘要（来自源码分析）

### 4.3.1 Codebase Scan

| 文件 | 职责 | 关键代码位置 |
|:---|:---|:---|
| [runtime/builtin/mutex.h](file:///d:/you/Aura/runtime/builtin/mutex.h) | Mutex/RWMutex/Once 运行时实现 | L26-77 Mutex / L98-201 RWMutex / L224-251 Once |
| [runtime/builtin/mutex.cpp](file:///d:/you/Aura/runtime/builtin/mutex.cpp) | 终结器 + TypeDescriptor | L8-12 mutex_finalizer / L26-30 rwmutex_finalizer / L42-48 once_finalizer |
| [src/AST/Stmt.h](file:///d:/you/Aura/src/AST/Stmt.h#L233) | LockStmt 节点 | L233-245 `lockExpr`（单数 `unique_ptr<ASTNode>`） |
| [src/Parser/StmtParser.cpp](file:///d:/you/Aura/src/Parser/StmtParser.cpp) | parseLockStmt | L35-37 dispatch / L278-289 实现（调用 `parseExpr()` 单表达式） |
| [src/Sema/Checker/StmtChecker.cpp](file:///d:/you/Aura/src/Sema/Checker/StmtChecker.cpp#L355) | checkLockStmt | L355-389 L1 规则（Mutex/RWMutexReadView/RWMutexWriteView/Once） |
| [src/CodeGen/StmtGen.cpp](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L843) | genLockStmt | L843-898 按 `lockExpr->inferredType` 分派 Once/RWMutexReadView/RWMutexWriteView/Mutex |
| [src/Sema/BuiltinRegistry.h](file:///d:/you/Aura/src/Sema/BuiltinRegistry.h) | 类型/方法/构造注册 | L218-225 types_ / L262-264 r()/w() / L278-281 sync.Mutex/RWMutex/Once |

### 4.3.2 关键接口契约（v1.1 现状）

**LockStmt AST（当前单锁）**：
```cpp
struct LockStmt : Stmt {
    std::unique_ptr<ASTNode> lockExpr;   // 单锁表达式
    std::unique_ptr<BlockStmt> body;
    // clone() 见 Stmt.h:238-244
};
```

**genLockStmt CodeGen（当前分派）**：
```cpp
// StmtGen.cpp:843
void CodeGenerator::genLockStmt(std::ostream& cpp, const LockStmt& stmt, bool) {
    std::string lockExpr = genExpr(*stmt.lockExpr, false);
    std::string typeName = /* 从 stmt.lockExpr->inferredType 读取 */;
    if (typeName == "Once")              { /* 生成 once->do_([&] { body }); */ }
    else if (typeName == "RWMutexReadView")  { /* 生成 { auto _guard = expr; body; } */ }
    else if (typeName == "RWMutexWriteView") { /* 同上 */ }
    else { /* 默认 Mutex: { auto _guard = __acquire_lock(expr); body; } */ }
}
```

**RWMutex Inner（当前字段）**：
```cpp
// mutex.h:99-106
struct Inner {
    std::timed_mutex m;
    std::atomic<int>  readers{0};
    std::atomic<bool> writer_active{false};
    std::atomic<int>  waiting_writers{0};
};
```

### 4.3.3 痛点 / 技术债

1. **多锁顺序不一致死锁**：当前只能手写嵌套 `lock (a) { lock (b) { } }`，用户需自行保证加锁顺序一致，易出错
2. **reader 唤醒延迟**：writer 释放后，等待的 reader 仍需 sleep 1ms 后才重试，高并发读场景吞吐受限
3. **递归持锁无法检测**：别名情况（`let m2 = m; lock (m) { lock (m2) { } }`）编译期无法识别，运行时直接死锁

## 4.4 提议变更

### 4.4.1 E. 多锁语句 `lock (a.mu, b.mu) { }`

#### 4.4.1.1 AST 扩展（方案 1：扩展现有 lockExpr 为列表）

**What**：`LockStmt.lockExpr` 从 `unique_ptr<ASTNode>` 改为 `std::vector<std::unique_ptr<ASTNode>>`

**Where**：[src/AST/Stmt.h:233-245](file:///d:/you/Aura/src/AST/Stmt.h#L233-245)、[src/AST/Stmt.cpp](file:///d:/you/Aura/src/AST/Stmt.cpp)（print 实现）

**Why**：
- 向后兼容单锁场景（列表大小为 1 时退化为单锁）
- CodeGen 统一走"列表排序后加锁"路径，逻辑清晰
- 不引入冗余字段（方案 2 `lockExpr + lockExprs` 会让 AST 冗余）

**AST 节点设计**：
```cpp
struct LockStmt : Stmt {
    std::vector<std::unique_ptr<ASTNode>> lockExprs;  // 1 个或多个锁表达式
    std::unique_ptr<BlockStmt> body;
    // clone() 遍历 lockExprs 逐个 clone
};
```

#### 4.4.1.2 Parser 扩展

**What**：`parseLockStmt` 改为解析逗号分隔的表达式列表

**Where**：[src/Parser/StmtParser.cpp:278-289](file:///d:/you/Aura/src/Parser/StmtParser.cpp#L278-289)

**Why**：支持 `lock (e1, e2, e3) { }` 语法

**解析逻辑**：
```cpp
std::unique_ptr<Stmt> Parser::parseLockStmt() {
    auto tok = advance();  // consume 'lock'
    auto stmt = std::make_unique<LockStmt>();
    setNodePos(stmt.get(), tok);

    consume(TokType::LParen, "expected '(' after lock");
    stmt->lockExprs.push_back(parseExpr());
    while (match(TokType::Comma)) {
        stmt->lockExprs.push_back(parseExpr());
    }
    consume(TokType::RParen, "expected ')' after lock expression list");

    stmt->body = parseBlock();
    return stmt;
}
```

**注意**：Aura Parser 没有 `parseExprList`，参考 [StmtParser.cpp:264](file:///d:/you/Aura/src/Parser/StmtParser.cpp#L264) 已有的 `while (match(Comma))` 模式。

#### 4.4.1.3 Sema 扩展

**What**：L1 规则扩展为列表检查，新增 L8/L9 规则

**Where**：[src/Sema/Checker/StmtChecker.cpp:355-389](file:///d:/you/Aura/src/Sema/Checker/StmtChecker.cpp#L355-389)

**规则表**：

| 规则 | 说明 | 错误信息 |
|:---|:---|:---|
| **L1**（扩展） | 每个 lockExpr 都必须是合法锁类型（Mutex/RWMutexReadView/RWMutexWriteView/Once） | "lock requires sync.Mutex/RWMutex.r()/.w()/Once" |
| **L8**（新增） | 多锁语句中禁止包含 `Once` 类型（Once 语义与多锁不兼容） | "cannot combine Once with multi-lock statement" |
| **L9**（新增） | 多锁语句中编译期可识别的重复锁（同 Identifier 或同字段链 AST 节点同一）报错 | "duplicate lock in multi-lock statement" |

**L9 识别规则**（仅 best-effort，运行时 L5 是强保证）：
- 若两个 lockExpr 都是 `IdentifierExpr` 且 name 相同 → 重复
- 若两个 lockExpr 都是 `MemberExpr` 且对象链完全相同 → 重复
- 其他情况（函数调用、动态索引）跳过编译期检查，依赖运行时 L5

**L9 实现要点**：
- 在 Sema 中实现 `isSameLockExpr(a, b)` 辅助函数，递归比较 AST 节点结构
- 仅对编译期可识别的相同性报错，对不确定的情况保持沉默（避免误报）

#### 4.4.1.4 CodeGen 自动排序（方式 A：运行时排序）

**What**：所有锁表达式求值后，按地址（指针值）升序排序，依次加锁

**Where**：[src/CodeGen/StmtGen.cpp:843-898](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L843-898)

**Why**：
- 排序需在运行时完成（指针值运行时才知道）
- 锁数量通常 ≤ 4，排序开销可忽略（< 10ns）
- 自动消除锁序反转死锁

**CodeGen 生成代码结构**（伪代码）：
```cpp
{
    // 1. 求值每个锁表达式，得到锁对象指针 + 类型
    auto _l0 = expr0;  // 类型由 Sema 推断后 CodeGen 已知
    auto _l1 = expr1;
    // ... 更多锁

    // 2. 收集到数组并按地址排序
    std::pair<void*, int> _locks[] = {
        {(void*)_l0, 0},
        {(void*)_l1, 1},
        // ...
    };
    std::sort(_locks, _locks + N,
        [](const auto& a, const auto& b) { return a.first < b.first; });

    // 3. 按排序后顺序依次构造 Guard（用 std::variant 持有多种 Guard 类型，
    //    或统一为 Mutex::Guard*/RWMutex::ReadGuard* 等指针）
    // 4. 块结束按逆序析构（LIFO 释放，C++ 自动处理）
    auto _g0 = /* 按 _locks[0].second 索引选择对应的 acquire 函数 */;
    auto _g1 = /* 按 _locks[1].second 索引选择对应的 acquire 函数 */;

    // 5. 生成 body
    body
}
```

**实现细节**：
- 由于 Aura 的 Guard 类型不同（`Mutex::Guard`/`RWMutex::ReadGuard`/`RWMutex::WriteGuard`），需用 `std::variant` 或类型擦除（`std::function<void()>` 释放器）统一持有
- 推荐 `std::variant<Mutex::Guard, RWMutex::ReadGuard, RWMutex::WriteGuard>`，加锁时 `emplace<N>(acquire(...))`，析构时自动调用对应析构

#### 4.4.1.5 兼容性：单锁场景

单锁 `lock (m) { }` 仍合法，AST 中 `lockExprs.size() == 1`，CodeGen 走简化路径（不排序，直接构造单个 Guard），避免不必要的运行时排序开销。

### 4.4.2 F. RWMutex 批量唤醒 reader（方向 2）

#### 4.4.2.1 当前问题

| 问题 | 描述 | 影响 |
|:---|:---|:---|
| reader 唤醒延迟 | writer 释放后，等待的 reader 仍需 sleep 1ms 后才重试 | 平均延迟 0.5ms，最坏 1ms |
| reader 串行 try_lock | 多个等待 reader 串行 try_lock，无显式协调 | 高并发读场景吞吐受限 |

#### 4.4.2.2 方向 2 设计

**What**：在 Inner 中新增 `std::atomic<int> waiting_readers` 计数器 + 代际计数器 `generation`，writer 释放时递增 generation 通知等待 reader

**Where**：[runtime/builtin/mutex.h:99-106](file:///d:/you/Aura/runtime/builtin/mutex.h#L99-106) Inner 结构、[mutex.h:112-151](file:///d:/you/Aura/runtime/builtin/mutex.h#L112-151) ReadGuard

**Why**：
- 不引入 `std::condition_variable`（违反 v1.0 死锁修复原则，TSan bug + STW 阻塞）
- 用代际计数器让等待 reader 在下次轮询时立即看到 writer 释放，无需 sleep 完整 1ms
- 缩短 sleep_for(1ms) → sleep_for(0ms)（让出 CPU 但立即重试），generation 变化时立即进入

**Inner 字段扩展**：
```cpp
struct Inner {
    std::timed_mutex m;
    std::atomic<int>  readers{0};
    std::atomic<bool> writer_active{false};
    std::atomic<int>  waiting_writers{0};
    // v1.2 新增（方向 2）：
    std::atomic<int>  waiting_readers{0};   // 等待中的 reader 数量
    std::atomic<uint64_t> generation{0};   // 代际计数器，writer 释放时递增
};
```

**ReadGuard 改造**（关键逻辑）：
```cpp
explicit ReadGuard(RWMutex* rw) : rw_(rw), gcRoot_(rw_), locked_(false) {
    while (true) {
        if (rw_->inner_->m.try_lock()) {
            if (!rw_->inner_->writer_active.load(std::memory_order_acquire)
                && rw_->inner_->waiting_writers.load(std::memory_order_acquire) == 0) {
                rw_->inner_->readers.fetch_add(1, std::memory_order_acq_rel);
                rw_->inner_->m.unlock();
                locked_ = true;
                // 成功进入：减少 waiting_readers
                rw_->inner_->waiting_readers.fetch_sub(1, std::memory_order_acq_rel);
                return;
            }
            rw_->inner_->m.unlock();
        }
        // 让出前记录代际
        uint64_t gen = rw_->inner_->generation.load(std::memory_order_acquire);
        gc_safepoint();
        // 短自旋：检查 generation 是否变化（writer 释放时递增）
        // 若变化则立即重试，否则 sleep 1ms 重试
        for (int spin = 0; spin < 8; ++spin) {
            if (rw_->inner_->generation.load(std::memory_order_acquire) != gen) break;
            std::this_thread::yield();
        }
        // 仍无变化则 sleep 1ms
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
```

**关键设计**：
- ReadGuard 进入前 `waiting_readers.fetch_add(1)`（标记等待），成功进入后 `fetch_sub(1)`
- WriteGuard 析构时：
  - 先 `writer_active.store(false)`
  - 然后 `generation.fetch_add(1)`（通知所有等待 reader）
  - 若 `waiting_readers > 0`，下次 reader 轮询时立即看到 generation 变化，无需等满 1ms

**WriteGuard 析构改造**：
```cpp
~WriteGuard() {
    if (locked_ && rw_) {
        rw_->inner_->writer_active.store(false, std::memory_order_release);
        // v1.2 新增（方向 2）：通知等待的 reader 批量重试
        // acquire/release 配对：reader 看到 generation 变化后必看到 writer_active=false
        rw_->inner_->generation.fetch_add(1, std::memory_order_acq_rel);
        locked_ = false;
    }
}
```

**内存序保证**：
- writer：`writer_active.store(false, release)` → `generation.fetch_add(1, acq_rel)`
- reader：`generation.load(acquire)` 看到 writer 的 fetch_add → 后续 `writer_active.load(acquire)` 必看到 false（release/acquire 配对）

#### 4.4.2.3 性能权衡

| 指标 | v1.1（轮询 1ms） | v1.2（代际+短自旋） | 改进 |
|:---|:---|:---|:---|
| reader 平均延迟 | 0.5ms | < 0.1ms（自旋 8 次 ≈ 1μs） | 5× |
| reader 最坏延迟 | 1ms | < 0.1ms | 10× |
| 高并发读吞吐 | 受 sleep 限制 | 自旋快速响应 | 显著提升 |
| CPU 开销 | sleep 让出 CPU | 自旋 8 次（~1μs） | 略增 |

**适用场景**：高并发读多写少（如缓存场景），reader 等待 writer 释放时延迟敏感。

**不适用场景**：reader 极少等待（写极少），自旋开销无收益。但自旋仅 8 次（~1μs），可忽略。

### 4.4.3 L5 嵌套同锁检测（运行时 error）

#### 4.4.3.1 编译期能力分析（结论）

**编译期无法完全覆盖**，原因：
- Aura 无跨函数别名分析
- Aura 无指针逃逸分析
- 只能识别"同名直接嵌套"和"同字段链嵌套"两种最浅情况

**编译期 best-effort 检查（L9 已在 §4.4.1.3 处理多锁重复）**：
- 同函数内嵌套 `lock (m) { lock (m) { } }` → L5 编译期 warning（非 error，避免误报）
- 但实际实现简单到可以跳过编译期检查，直接依赖运行时

**推荐方案**：**只做运行时检测，不做编译期 L5 检查**，简化实现。

#### 4.4.3.2 运行时检测实现

**What**：Mutex/RWMutex 的 Inner 新增 `std::atomic<std::thread::id> owner` 字段，Guard 构造时检测递归持锁

**Where**：
- [runtime/builtin/mutex.h:26-77](file:///d:/you/Aura/runtime/builtin/mutex.h#L26-77) Mutex 结构
- [runtime/builtin/mutex.h:98-201](file:///d:/you/Aura/runtime/builtin/mutex.h#L98-201) RWMutex 结构
- [runtime/builtin/mutex.cpp](file:///d:/you/Aura/runtime/builtin/mutex.cpp) 终结器（如需更新 Inner 字段）

**Mutex 改造**：
```cpp
struct Mutex : GcObject {
    struct Inner {
        std::timed_mutex m;
        std::atomic<std::thread::id> owner;  // v1.2 新增：当前持锁线程
    };
    Inner* inner_;  // 改为间接指针持有 Inner（与 RWMutex 一致）

    class Guard {
    public:
        explicit Guard(Mutex* m) : m_(m), gcRoot_(m_), locked_(false) {
            auto cur = std::this_thread::get_id();
            // L5 运行时检测：递归持锁
            if (m_->inner_->owner.load(std::memory_order_acquire) == cur) {
                throw make_runtime_error(
                    "recursive lock detected: thread already holds this mutex");
            }
            while (!m_->inner_->m.try_lock()) {
                gc_safepoint();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            m_->inner_->owner.store(cur, std::memory_order_release);
            locked_ = true;
        }
        ~Guard() {
            if (locked_ && m_) {
                m_->inner_->owner.store(std::thread::id{},
                                         std::memory_order_release);
                m_->inner_->m.unlock();
                locked_ = false;
            }
        }
        // ... 移动构造同 v1.1，但需重置 owner
    };
};
```

**Mutex 结构变化**：从 `std::timed_mutex* m_`（裸指针）改为 `Inner* inner_`，与 RWMutex 一致。

**RWMutex 改造**：
```cpp
struct Inner {
    std::timed_mutex m;
    std::atomic<int>  readers{0};
    std::atomic<bool> writer_active{false};
    std::atomic<int>  waiting_writers{0};
    std::atomic<int>  waiting_readers{0};     // v1.2 F 方向 2
    std::atomic<uint64_t> generation{0};     // v1.2 F 方向 2
    // v1.2 L5 新增：
    std::atomic<std::thread::id> writer_owner;                // 写锁持有者
    // reader_owners 用 thread_local 集合（避免 Inner 持有 std::vector 的同步开销）
    // 简化方案：用 thread_local 计数器 + RWMutex* 集合
};
```

**RWMutex L5 检测简化方案**（避免 vector 同步开销）：
- 用 `thread_local std::unordered_map<RWMutex*, int> g_reader_holds` 记录当前线程持有的读锁
- ReadGuard 构造时检查 `g_reader_holds[this] > 0` 或 `writer_owner == cur`
- WriteGuard 构造时检查 `writer_owner == cur` 或 `g_reader_holds[this] > 0`

**优点**：
- 无锁检查（thread_local）
- 无 Inner 字段开销
- 自动支持可重入检测（同线程多次 r() 会发现）

**缺点**：
- thread_local 全局状态（与 Aura 的 GC roots 全局状态一致，可接受）
- ReadGuard 析构时需更新 `g_reader_holds[this]--`，注意异常安全

#### 4.4.3.3 错误处理

**抛 Aura 专用的 `Error` 对象**（非 abort，非 `std::runtime_error`），允许用户 `try/catch` 处理。

参考 [runtime/builtin/error.h:53](file:///d:/you/Aura/runtime/builtin/error.h#L53) 已有的 `make_runtime_error` 工厂函数，以及 [runtime/builtin/array.h:153](file:///d:/you/Aura/runtime/builtin/array.h#L153) `throw Error{...}` 用法：

```cpp
// 运行时层抛出（使用 make_runtime_error 工厂函数）
throw make_runtime_error(
    "recursive lock detected: thread already holds this lock");
```

**Aura 层用法**（用户代码可捕获）：
```aura
try {
    lock (m) {
        lock (m) { }       # ❌ 抛 RuntimeError
    }
} catch e {
    io.println("caught: " + e.message)  # caught: recursive lock detected: ...
    io.println("kind: " + e.kind)       # kind: RuntimeError
}
```

**理由**：
- Aura 有专门的 `Error` 类型（`{ kind: string, message: string, ... }`），C++ 层抛 `Error` 对象会被 Aura 的 `try/catch` 正确捕获
- 与 [runtime/builtin/io.cpp:59](file:///d:/you/Aura/runtime/builtin/io.cpp#L59) `throw Error(make_string("io_error"), ...)` 等已有运行时抛错模式一致
- 与 [runtime/builtin/array.h:153](file:///d:/you/Aura/runtime/builtin/array.h#L153) `throw Error{make_string("IndexError"), ...}` 一致
- 不用 `std::runtime_error`（无法被 Aura try/catch 捕获）
- 不用 `abort()`（导致进程退出无法调试，且不经过 stack unwinding）

#### 4.4.3.4 与 L9 的协同

| 场景 | L9 编译期 | L5 运行时 | 结果 |
|:---|:---|:---|:---|
| `lock (m) { lock (m) { } }` 同名嵌套 | ❌ 不报（编译期不做 L5） | ✅ 抛异常 | 运行时检测 |
| `lock (m) { lock (m2) { } }` 别名 | ❌ 无法识别 | ✅ 抛异常 | 运行时检测 |
| `lock (a.mu, a.mu) { }` 多锁同名 | ✅ 报错（L9） | — | 编译期拦截 |
| `lock (a.mu, b.mu) { }` 别名多锁 | ❌ 无法识别 | ✅ 抛异常 | 运行时检测 |

## 4.5 影响分析

### 4.5.1 受影响组件

| 组件 | 影响 | 兼容性 |
|:---|:---|:---|
| AST `LockStmt` | 字段从 `lockExpr` 改为 `lockExprs` | ⚠️ **BREAKING**（AST 节点结构变化，但仅内部使用） |
| Parser `parseLockStmt` | 改为表达式列表解析 | ✅ 向后兼容（单锁仍合法） |
| Sema `checkLockStmt` | L1 扩展为列表 + L8/L9 新规则 | ✅ 向后兼容 |
| CodeGen `genLockStmt` | 改为运行时排序后加锁 | ✅ 向后兼容（单锁走简化路径） |
| Mutex 结构 | `m_` 改为 `inner_`（含 owner 字段） | ⚠️ **BREAKING**（终结器需更新） |
| RWMutex Inner | 新增 4 个字段 | ✅ 向后兼容（仅字段增加） |
| Mutex/RWMutex Guard | 构造时新增 L5 检测 | ✅ 向后兼容（新增检测，不改变成功路径） |

### 4.5.2 ⚠️ BREAKING 变更说明

1. **LockStmt AST 字段改名**：`lockExpr` → `lockExprs`（`unique_ptr<ASTNode>` → `std::vector<...>`）
   - 影响：所有读取 `stmt.lockExpr` 的代码（Parser/Sema/CodeGen/AST print/clone）
   - 迁移：3 处（Parser L284 / Sema L357-381 / CodeGen L845/851）

2. **Mutex 结构改为 Inner**：`std::timed_mutex* m_` → `Inner* inner_`
   - 影响：Mutex 的 Guard 构造/析构、终结器、工厂函数、TypeDescriptor
   - 迁移：4 处（mutex.h L26-88 / mutex.cpp L8-23 L56-57）

## 4.6 边界条件处理策略

| 边界条件 | 当前处理 | 计划处理 | 测试策略 |
|:---|:---|:---|:---|
| **空锁列表** `lock () { }` | 不支持 | Parser 报错 "expected expression" | 单元测试：编译期错误 |
| **单锁场景** `lock (m) { }` | ✅ v1.1 已支持 | CodeGen 走简化路径，不排序 | 回归测试 |
| **多锁中包含 Once** | 不支持 | L8 报错 "cannot combine Once with multi-lock" | 单元测试：编译期错误 |
| **多锁同名 Identifier** | 不支持 | L9 报错 "duplicate lock" | 单元测试：编译期错误 |
| **多锁别名运行时相同** | 无法检测 | L5 运行时抛 `Error{RuntimeError, ...}` | 集成测试：try/catch 捕获 |
| **递归持锁（同线程）** | 死锁 | L5 运行时抛 `Error{RuntimeError, ...}` | 集成测试：try/catch 捕获 |
| **多锁排序后死锁** | — | 排序后无锁序环（按地址升序） | 证明：地址是全序关系，无环 |
| **Guard 析构异常** | 不抛异常 | 析构中不抛异常（C++ 规范） | 静态保证 |
| **reader 自旋期间 GC STW** | — | 自旋中调用 `gc_safepoint()` 响应 STW | 已有模式（参考 v1.1） |
| **owner 字段在 compact GC 后** | — | `Inner` 是间接指针，不受 compact 影响 | 已有模式（参考 v1.1） |
| **thread_local g_reader_holds 生命周期** | — | 线程退出时自动析构，RWMutex 析构时不应有线程持有读锁 | 静态约束（用户错误时检测） |
| **多锁排序稳定性** | — | `std::sort` 不稳定，但地址唯一无影响 | 单元测试 |

## 4.7 测试方案

### 4.7.1 单元测试（Sema 编译期检查）

```aura
# Test S1: 空锁列表 → 编译错误
fun bad() { lock () { } }              # ❌ expected expression

# Test S2: 多锁包含 Once → L8 报错
fun bad(once: sync.Once, m: sync.Mutex) {
    lock (once, m) { }                # ❌ cannot combine Once with multi-lock
}

# Test S3: 多锁同名 Identifier → L9 报错
fun bad(m: sync.Mutex) {
    lock (m, m) { }                   # ❌ duplicate lock
}

# Test S4: 多锁同字段链 → L9 报错
fun bad(obj: Account) {
    lock (obj.mu, obj.mu) { }         # ❌ duplicate lock
}

# Test S5: 多锁别名（编译期无法识别）→ 通过编译
fun ok(m: sync.Mutex) {
    let m2 = m
    lock (m, m2) { }                  # ✅ 编译通过，运行时 L5 检测
}
```

### 4.7.2 集成测试（运行时行为）

```aura
# Test I1: 多锁并发场景（Account 互转）
type Account = { balance: int, mu: sync.Mutex }

fun transfer(a: Account, b: Account, amt: int) {
    lock (a.mu, b.mu) {
        a.balance -= amt
        b.balance += amt
    }
}

fun main(io: Io) {
    let acc1 : Account = { balance = 1000, mu = sync.Mutex() }
    let acc2 : Account = { balance = 1000, mu = sync.Mutex() }

    sync thread(max = 4) {
        for i in range(1000) {
            spawn (acc1: Account, acc2: Account, i: int) {
                if (i % 2 == 0) {
                    transfer(acc1, acc2, 1)
                } else {
                    transfer(acc2, acc1, 1)
                }
            }
        }
    }

    # 验收：总额守恒（2000），无死锁
    io.println("total: " + (acc1.balance + acc2.balance))  # 2000
}

# Test I2: RWMutex 批量唤醒 reader 性能（基准测试）
fun main(io: Io) {
    let rw = sync.RWMutex()
    let counter = [0]

    sync thread(max = 8) {
        for i in range(10000) {
            spawn (rw: sync.RWMutex, counter: [int], i: int) {
                if (i % 100 == 0) {
                    lock (rw.w()) { counter.append(i) }
                } else {
                    lock (rw.r()) { let _ = counter.len() }
                }
            }
        }
    }
    io.println("done: " + counter.len())
}

# Test I3: 递归持锁检测（L5 运行时 error）
fun main(io: Io) {
    let m = sync.Mutex()
    try {
        lock (m) {
            lock (m) {                # ❌ Error{RuntimeError, "recursive lock detected..."}
                io.println("never")
            }
        }
    } catch e {
        io.println("caught: " + e.message)  # caught: recursive lock detected...
    }
}

# Test I4: RWMutex 递归读锁检测（L5 运行时 error）
fun main(io: Io) {
    let rw = sync.RWMutex()
    try {
        lock (rw.r()) {
            lock (rw.r()) {          # ❌ Error{RuntimeError, "recursive lock detected..."}
                io.println("never")
            }
        }
    } catch e {
        io.println("caught: " + e.message)
    }
}
```

### 4.7.3 稳定性验收

- **连续 5 次**运行 Test I1 / I2 / I3 / I4
- 无死锁、无 abort、无 TSan 警告
- Test I1 总额守恒（2000）
- Test I2 完成且无 crash
- Test I3/I4 正确捕获 Aura `Error{RuntimeError, ...}` 并打印 caught 信息

### 4.7.4 性能基准（Test I2 扩展）

| 指标 | v1.1（轮询 1ms） | v1.2（代际+自旋） | 期望改进 |
|:---|:---|:---|:---|
| 10000 spawn 完成时间 | 基准 | < 基准 × 0.8 | ≥ 20% |
| reader 平均延迟 | 0.5ms | < 0.1ms | 5× |

> 若改进 < 20%，需重新评估 F 方向 2 的收益是否值得复杂度。

#### 4.7.4.1 PowerShell 计时方法

使用 `Measure-Command` 测量 `test.exe` 执行时间（参考 [AGENTS.md](file:///d:/you/Aura/AGENTS.md) 测试流程：`compile.cmd` 编译 → 运行 `test.exe`）：

```powershell
# 单次测量
Measure-Command { .\example\test.exe } | Select-Object TotalMilliseconds

# 5 次平均测量（消除抖动）
$results = 1..5 | ForEach-Object {
    (Measure-Command { .\example\test.exe } | Select-Object -ExpandProperty TotalMilliseconds)
}
$avg = ($results | Measure-Object -Average).Average
Write-Host "v1.2 average: $avg ms"
Write-Host "samples: $($results -join ', ') ms"

# v1.1 vs v1.2 对比（需在 v1.1 代码上跑一次基线，再在 v1.2 代码上跑一次）
# 基线（v1.1）：
#   $baseline = (1..5 | ForEach-Object { (Measure-Command { .\example\test.exe }).TotalMilliseconds } | Measure-Object -Average).Average
# v1.2：
#   $v12 = (1..5 | ForEach-Object { (Measure-Command { .\example\test.exe }).TotalMilliseconds } | Measure-Object -Average).Average
# 改进比 = ($baseline - $v12) / $baseline * 100%
```

**测量注意事项**：
- `Measure-Command` 包含进程启动开销（~10-30ms），对长任务（>1s）影响可忽略，对短任务需扣除
- 5 次采样取平均，消除 GC 时机差异和系统调度抖动
- 同一台机器、同样负载下测量，避免后台进程干扰
- 若结果波动 > 20%，需增加采样数到 10 次

#### 4.7.4.2 性能基准测试程序（Test I2-bench）

```aura
# 单独的性能基准测试，输出执行时间到 stderr（避免影响 stdout 验证）
fun main(io: Io) {
    let rw = sync.RWMutex()
    let counter = [0]

    sync thread(max = 8) {
        for i in range(10000) {
            spawn (rw: sync.RWMutex, counter: [int], i: int) {
                if (i % 100 == 0) {
                    lock (rw.w()) { counter.append(i) }
                } else {
                    lock (rw.r()) { let _ = counter.len() }
                }
            }
        }
    }
    # 不打印 counter.len()，避免 IO 开销影响计时
    # 计时由外层 PowerShell Measure-Command 完成
}
```

## 4.8 实施步骤（有序）

### 4.8.1 依赖关系

```
步骤 1（AST 扩展）─┬─→ 步骤 2（Parser）─→ 步骤 3（Sema L1/L8/L9）
                  │
                  └─→ 步骤 4（CodeGen 多锁排序）

步骤 5（Mutex Inner 改造）─┬─→ 步骤 6（RWMutex Inner 扩展 + F 方向 2）
                          │
                          └─→ 步骤 7（L5 运行时检测）

步骤 8（构建）─→ 步骤 9（单元测试）─→ 步骤 10（集成测试）─→ 步骤 11（稳定性验收）
```

### 4.8.2 步骤明细

| 步骤 | 模块 | 文件 | 改动内容 | 依赖 | 验证 |
|:---|:---|:---|:---|:---|:---|
| 1 | AST | `src/AST/Stmt.h:233-245`、`src/AST/Stmt.cpp` | `LockStmt.lockExpr` → `lockExprs`（vector），更新 `clone()` 和 `print()` | 无 | 编译通过 |
| 2 | Parser | `src/Parser/StmtParser.cpp:278-289` | `parseLockStmt` 改为逗号分隔表达式列表 | 1 | `lock (a, b) { }` 可解析 |
| 3 | Sema | `src/Sema/Checker/StmtChecker.cpp:355-389` | L1 扩展为列表检查 + 新增 L8/L9 规则 + `isSameLockExpr` 辅助函数 | 1 | Test S1-S5 编译期错误正确 |
| 4 | CodeGen | `src/CodeGen/StmtGen.cpp:843-898` | `genLockStmt` 改为运行时排序后加锁（用 `std::variant` 持有多种 Guard） | 1 | Test I1 通过 |
| 5 | 运行时 | `runtime/builtin/mutex.h:26-88`、`mutex.cpp:8-23` | Mutex 改为 `Inner* inner_`（含 owner 字段），更新 Guard/终结器/工厂 | 无 | v1.1 单锁测试回归通过 |
| 6 | 运行时 | `runtime/builtin/mutex.h:99-201` | RWMutex Inner 新增 `waiting_readers` + `generation` 字段，ReadGuard/WriteGuard 改造（代际通知 + 短自旋） | 5 | Test I2 性能改进 ≥ 20% |
| 7 | 运行时 | `runtime/builtin/mutex.h`、新增 `runtime/builtin/lock_detect.h` | L5 检测：Mutex owner 字段 + RWMutex thread_local g_reader_holds 集合 | 5, 6 | Test I3/I4 正确捕获异常 |
| 8 | 构建 | `cmake --build build` + `cmake --build runtime/build` | 编译链接 | 1-7 | 无错误无警告 |
| 9 | 单元测试 | `example/test.aura` | Test S1-S5 编译期错误验证 | 8 | 编译期错误信息正确 |
| 10 | 集成测试 | `example/test.aura` | Test I1-I4 运行时行为验证 | 8 | 全部通过 |
| 11 | 稳定性 | 连续 5 次运行 Test I1-I4 | 无死锁、无 abort、无 TSan 警告 | 10 | 5/5 通过 |

**总改动预估**：
- AST/Parser/Sema/CodeGen: ~250 行（多锁语句链路）
- runtime/mutex.h/cpp: ~150 行（Inner 改造 + F 方向 2 + L5 检测）
- 测试代码: ~100 行
- 合计: ~500 行

### 4.8.3 回滚策略

| 步骤 | 回滚方式 |
|:---|:---|
| 1-4（多锁语句） | 恢复 `LockStmt.lockExpr` 单字段，删除 L8/L9，CodeGen 回退到单锁分派 |
| 5-6（Inner 改造 + F 方向 2） | 恢复 `Mutex::m_` 裸指针，删除 RWMutex 新增字段 |
| 7（L5 检测） | 删除 owner 字段读写 + thread_local 集合（独立于其他改动） |

## 4.9 风险与缓解

### 4.9.1 已知风险

| 风险 | 影响 | 缓解 |
|:---|:---|:---|
| **F 方向 2 自旋开销** | 高并发读场景 CPU 略增（8 次自旋 ≈ 1μs/reader） | 自旋次数限制为 8，可配置；若 profiling 显示开销过大，改为 4 次 |
| **L5 thread_local 集合内存泄漏** | 线程退出时未清理 g_reader_holds | thread_local 自动析构 unordered_map，但若 RWMutex 在线程退出前析构需检查 |
| **多锁排序的 ABI 兼容** | `std::variant` 在 C++17 才稳定 | Aura 已用 C++20，无问题 |
| **L9 误报** | 编译期 best-effiff 检查可能误报（如 `lock (a.mu, b.mu)` 实际不同对象但同字段链） | L9 仅对完全相同的 AST 节点报错，避免误报 |
| **Mutex 结构改为 Inner 的回归风险** | v1.0/v1.1 测试可能因结构变化失败 | 步骤 5 后立即运行 v1.1 测试回归 |

### 4.9.2 假设

1. Aura 用户不依赖 `std::thread::id` 的具体值（仅用于比较）
2. Aura GC compact 不会在 Guard 构造中途触发（已有 `GcRootHandle` 保护）
3. 多锁语句锁数量通常 ≤ 4（排序开销可忽略）

### 4.9.3 未知问题

1. F 方向 2 的代际计数器在高并发写场景（writer 频繁释放）是否会有 `generation` 溢出？
   - `uint64_t` 每秒 100 万次递增可用 5800 年，无溢出风险
2. L5 的 thread_local g_reader_holds 在协程切换时是否正确？
   - Aura 协程是单线程协作式，thread_local 在协程间共享，需确认协程持锁期间不让出
   - 已有约束：lock 块内禁止 await（L4），所以协程不会在持锁期间切换，thread_local 安全

---

## 五、A. WaitGroup 重新设计（暂不实施）

> 已转入 [TODO.txt](file:///d:/you/Aura/TODO.txt) §十独立条目（P3，待场景验证）。本节仅保留设计要点供未来参考。

### 5.1 推迟原因

详见 [TODO.txt](file:///d:/you/Aura/TODO.txt) §十 "sync.WaitGroup 重新设计" 条目。

### 5.2 触发条件

满足任一即重新启动：
1. 跨 sync 块等待
2. 提前等待某批 spawn
3. thread 版 channel 配合

### 5.3 候选 API 方向

待场景验证后确定，详见 TODO.txt 条目。

---

## 六、D. sync.Cond / sync.Semaphore（v1.3 远期）

> 已转入 [TODO.txt](file:///d:/you/Aura/TODO.txt) §十独立条目（P3，v1.3）。本节仅保留设计要点供未来参考。

### 6.1 推迟原因

详见 [TODO.txt](file:///d:/you/Aura/TODO.txt) §十 "sync.Cond / sync.Semaphore" 条目。

### 6.2 v1.3 实施要点

- sync.Cond：atomic<bool> notified 标志 + 轮询避免丢失唤醒
- sync.Semaphore：复用 thread_pool 内部 counting_semaphore + try_acquire_for 模式

---

## 七、参考

- [mutex_plan.md](file:///d:/you/Aura/plan/mutex_plan.md)：v1.0 / v1.1 完整实施方案
- [plan/done/gc_mutex_deadlock_fix_report.md](file:///d:/you/Aura/plan/done/gc_mutex_deadlock_fix_report.md)：v1.0 死锁修复经验（STW safepoint 原则）
- [TODO.txt](file:///d:/you/Aura/TODO.txt) §十：v1.2/v1.3/WaitGroup 进度跟踪
- [channel_thread_issue.md](file:///d:/you/Aura/plan/channel_thread_issue.md)：thread 版 channel 设计（可能触发 WaitGroup 需求）
