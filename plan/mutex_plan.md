# Plan：sync 锁族 + `lock` 块语句实施方案

> 来源：[mutex_issue.md](file:///d:/you/Aura/plan/done/mutex_issue.md)（已审核通过）
> 类型：详细实施方案（plan）
> 日期：2026-07-24（v1.0 已实施），2026-07-26（v1.1 已实施并修正）
> 状态：v1.0 / v1.1 已完成
> 关联：[mutex_plan_v1.2.md](file:///d:/you/Aura/plan/mutex_plan_v1.2.md)（v1.2 及之后规划），
>       [sync_thread_plan.md](file:///d:/you/Aura/plan/sync_thread_plan.md)，
>       [plan/done/gc_mutex_deadlock_fix_report.md](file:///d:/you/Aura/plan/done/gc_mutex_deadlock_fix_report.md)

---

## 一、背景与设计决策

### 1.1 问题背景

sync thread 语句允许多个 spawn 任务并行执行。当任务通过指针共享 GC 对象（如 `Array<T>*`、`GcString*` 或用户自定义 record）并修改时，存在数据竞争。当前 sync_thread_plan 明确写着：

> ❌ `sync thread` 块内的 GC 暂停只在 L2/L3 safepoint 触发（非并发 GC）
> spawn 参数值传递；GC 对象通过指针共享时**用户需自行加锁**（v1 不提供语言级锁）

这是个矛盾：sync thread 提供了并行能力，却没提供任何加锁机制。本 plan 解决该缺口。

设计已与用户确认采用**双轨方案**：channel<T>（CSP，独立 plan）+ sync 锁族（本 plan）。

### 1.2 用户决策记录（2026-07-24 对话）

1. **采用方案 C（`lock` 块语句）** 而非方案 B（RAII Guard）
   - 核心理由：**块语句天然禁止跨函数持有锁**，消除一类误用
   - 块语句作用域明确，意图清晰
2. **锁不止一种**：v1 应规划锁族（Mutex / RWMutex / Once / WaitGroup），而非仅 Mutex
   - 本 plan 将锁族作为整体设计，分阶段实施
3. **统一 `lock` 语法，括号内表达式区分锁类型/模式**（二次修正）
   - 用户原意示例：`lock (sync.rwmutex.r) { }`
   - 实施形式：`lock (m)` / `lock (rw.r())` / `lock (rw.w())`
   - 优势：一个 `lock` 关键字覆盖所有锁类型，模式由 `.r()`/`.w()` 表达，无需 rlock/wlock 软关键字
   - 扩展性：新增锁类型只需实现 `.r()`/`.w()` 方法，**零 AST/Parser 改动**
4. **Once/WaitGroup 也用 lock 语法**（三次修正）
   - 用户要求：sync.Once、sync.WaitGroup 也要相同的 lock 逻辑，不用函数式 API
   - 设计：`lock (once) { }` → call_once 包裹；`lock (wg) { }` → add(1)/done() 包裹
   - 优势：所有 sync 原语**统一用 `lock` 块**，用户无需记忆不同 API
   - 实例化必要性：锁是共享状态对象（mutex 状态/once 标志/wg 计数器），必须构造实例供多 spawn 共享；这与 Go 的值类型零值可用不同，Aura 的 GC 对象需显式构造

> **2026-07-26 更新**：WaitGroup 在 v1.1 实施过程中发现与 `sync_thread_context` 析构自动 waitGroup 冗余，已从 v1.1 移除，推迟到 v1.2 重新设计。详见 [mutex_plan_v1.2.md](file:///d:/you/Aura/plan/mutex_plan_v1.2.md) §A。

### 1.3 方案对比与选择

| 方案 | 语法 | 忘记 unlock | 跨函数持锁 | 异常安全 | 实现成本 | 结论 |
|:---|:---|:---|:---|:---|:---|:---|
| A 手动 lock/unlock | `m.lock(); ...; m.unlock()` | ❌ 会忘 | ❌ 允许 | ❌ 无 defer | 低 | 不推荐 |
| B RAII Guard | `let g = m.lock()` | ✅ | ❌ 可 move | ✅ | 中 | 不推荐（违反禁止跨函数） |
| **C lock 块语句** | `lock (m) { }` | ✅ | ✅ 禁止 | ✅ | 中 | **推荐** |
| D B+C 双重 | 两者皆有 | ✅ | ❌ guard 仍可 move | ✅ | 高 | 不推荐（过度设计） |

**选择方案 C**：块语句结束自动 unlock（RAII），词法作用域强制锁不能跨函数边界，等价于"语法受限的方案 B"。

### 1.4 设计目标

1. **线程安全**：保护临界区，防止数据竞争
2. **无忘记 unlock 风险**：块语句结束自动 unlock，RAII 保证
3. **禁止跨函数持有锁**：`lock (m) { }` 块不能跨函数，消除"锁泄漏到调用者"的误用
4. **与 Aura 体系一致**：与 GcRootHandle 的 RAII 模式一致
5. **spawn 友好**：能作为 spawn 参数传递（值传递语义，传指针）
6. **GC 兼容**：锁本身可作为 GC 对象，生命周期由 GC 管理
7. **覆盖常见并发模式**：互斥、读写分离、一次性初始化

---

## 二、锁族规划

### 2.1 锁种类清单

| 类型 | 用途 | 对应 C++ 原语 | v1 状态 |
|:---|:---|:---|:---|
| **sync.Mutex** | 互斥锁（独占） | `std::timed_mutex` + 间接指针 | ✅ v1.0 已实施 |
| **sync.RWMutex** | 读写锁（多读单写，写优先） | `std::timed_mutex` + `atomic<int>` + 间接指针 | ✅ v1.1 已实施 |
| **sync.Once** | 一次性执行 | `std::timed_mutex` + `atomic<bool>` + 间接指针 | ✅ v1.1 已实施 |
| sync.WaitGroup | 等待一组任务完成 | — | ⏸ 推迟到 v1.2（见 [mutex_plan_v1.2.md](file:///d:/you/Aura/plan/mutex_plan_v1.2.md) §A） |
| sync.Cond | 条件变量 | `std::condition_variable`（需 safepoint 轮询） | ⏸ 推迟到 v1.3（见 [mutex_plan_v1.2.md](file:///d:/you/Aura/plan/mutex_plan_v1.2.md) §D） |
| sync.Semaphore | 信号量 | `std::counting_semaphore`（需 try_acquire_for 轮询） | ⏸ 推迟到 v1.3（见 [mutex_plan_v1.2.md](file:///d:/you/Aura/plan/mutex_plan_v1.2.md) §D） |

**关键约束（v1.0 死锁修复确立，详见 [plan/done/gc_mutex_deadlock_fix_report.md](file:///d:/you/Aura/plan/done/gc_mutex_deadlock_fix_report.md)）**：

1. **必须用 `timed_mutex` + `try_lock` 轮询**：所有 Guard 构造禁止使用 `mutex::lock()` 阻塞获取。持锁线程被 GC STW 暂停时，阻塞 lock() 的线程无法到达 safepoint → 死锁。
2. **必须用原子轮询替代 `cv.wait/wait_for`**：GCC 11 TSan 对 `pthread_cond_timedwait` 追踪有 bug（误报 "double lock of a mutex"），且阻塞期间无法响应 STW。
3. **必须有 `GcRootHandle<T*>` + `locked_` 标志**：防 compact 搬迁悬垂 + 避免 TSan 误报 "unlock of an unlocked mutex"。
4. **MinGW `std::shared_mutex` bug**（msys2/MINGW-packages#25193）：RWMutex 用 `std::timed_mutex` + 读者计数模拟，规避该 bug。

### 2.2 统一语法

所有锁都用 `lock (lockExpr) { }` 块。模式由 lockExpr 表达式自描述：

```aura
lock (m) { }           # Mutex 独占
lock (rw.r()) { }      # RWMutex 读
lock (rw.w()) { }      # RWMutex 写
lock (once) { }        # 一次性执行（块体可能跳过）
```

**关键设计**：括号内的 `lockExpr` 是普通表达式，由 Sema 推断其类型。lockExpr 类型决定 lock 语义（独占/读/写/一次）。新增锁类型只需运行时实现对应语义，**零 AST/Parser 改动**（v1.0 的 LockStmt 设计已覆盖所有场景）。

### 2.3 v1 范围与分期

- **v1.0**：sync.Mutex + `lock` 块语句（AST/Parser/Sema/CodeGen 全链路）— ✅ 已完成
- **v1.1**：sync.RWMutex / sync.Once（**复用 v1.0 的 LockStmt，零 AST/Parser 改动**，仅运行时 + BuiltinRegistry + CodeGen 分派）— ✅ 已完成
- **v1.2 及之后**：见 [mutex_plan_v1.2.md](file:///d:/you/Aura/plan/mutex_plan_v1.2.md)

---

## 三、v1.0 详细实施方案

**范围**：仅 sync.Mutex + `lock (m) { }` 块语句，全链路打通。

### 3.1 AST：新增 LockStmt 节点

**文件**：`src/AST/Stmt.h`、`src/AST/Stmt.cpp`

**设计**：统一节点，无 Mode 枚举（模式由 lockExpr 表达式表达）。

```cpp
struct LockStmt : Stmt {
    std::unique_ptr<ASTNode> lockExpr;   // 锁表达式，求值为 LockView
    std::unique_ptr<BlockStmt> body;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<LockStmt>();
        if (lockExpr) n->lockExpr = lockExpr->clone();
        if (body) n->body.reset(static_cast<BlockStmt*>(body->clone().release()));
        n->line = line; n->col = col;
        return n;
    }
};
```

**设计要点**：
- `lockExpr` 是任意表达式（支持 `m` / `rw.r()` / `obj.mutex` / `get_lock()`），求值后须为锁类型
- `body` 是块语句，作用域独立
- **无 Mode 字段**：模式（独占/读/写/一次）由 lockExpr 求值结果的类型决定
- **类型信息**：`lockExpr->inferredType` 由 Sema 标注（ASTNode 已有该字段，见 [ASTNode.h:21](file:///d:/you/Aura/src/AST/ASTNode.h#L21)），CodeGen 直接读取分派

**位置**：在 [Stmt.h:218 SyncStmt](file:///d:/you/Aura/src/AST/Stmt.h#L218) 之后、SpawnStmt 之前新增（保持 sync 族语句聚集）。

### 3.2 Parser：parseLockStmt

**文件**：`src/Parser/StmtParser.cpp`

**软关键字**：仅 `lock`（不引入 rlock/wlock），仍按普通标识符解析，在语句起始位置上下文识别。

```cpp
std::unique_ptr<Stmt> Parser::parseLockStmt() {
    auto tok = consume();  // 'lock' 标识符
    auto stmt = std::make_unique<LockStmt>();
    setNodePos(stmt.get(), tok);

    expect(TokType::LParen, "expected '(' after lock");
    stmt->lockExpr = parseExpr();   // 任意表达式：m / rw.r() / obj.mu ...
    expect(TokType::RParen, "expected ')'");

    stmt->body = parseBlock();
    return stmt;
}
```

**触发位置**：在 [StmtParser.cpp:30 parseStmt](file:///d:/you/Aura/src/Parser/StmtParser.cpp#L30) 的调度链中新增：

```cpp
// 在现有 sync/spawn 分支后
if (peek().type == TokType::Identifier && peek().text == "lock"
    && peekNext().type == TokType::LParen) {
    return parseLockStmt();
}
```

**触发条件**：语句起始位置遇到 `lock` 标识符 + 后续 `(` → 解析为 LockStmt。否则按普通标识符处理（如 `let lock = ...`）。

**优势**：模式（读/写）由括号内表达式的语义决定（`.r()`/`.w()` 方法调用），Parser 层完全不感知锁类型，新增锁类型无需改 Parser。

### 3.3 Sema：checkLockStmt

**文件**：`src/Sema/Checker/StmtChecker.cpp`

**新增规则**（v1.0 仅需 L1/L3/L4/L6）：

| 规则 | 说明 | 错误信息 |
|:---|:---|:---|
| **L1** | `lockExpr` 求值结果必须是锁类型：`Mutex*`（v1.0 仅此；v1.1 扩展 RWMutex.r()/.w()/Once） | "lock requires sync.Mutex" |
| **L3** | `lock` 块内禁止 `return`/`break`/`continue` 跨出块 | "cannot return/break/continue out of lock block" |
| **L4** | `lock` 块内禁止 `await`（会阻塞其他线程，违反 STW 协议） | "cannot await inside lock block" |
| **L6** | `lock` 块内禁止 `spawn`（spawn 不应持锁） | "cannot spawn inside lock block" |

**实现**（参考 [checkSyncStmt:221](file:///d:/you/Aura/src/Sema/Checker/StmtChecker.cpp#L221) 的 inSyncThreadBlock_ 模式）：

```cpp
void SemAnalyzer::checkLockStmt(const LockStmt& stmt) {
    // L1: lockExpr 类型检查
    if (stmt.lockExpr) {
        auto lockTy = inferExpr(*stmt.lockExpr);
        // v1.0 仅允许 Mutex*；v1.1 扩展
        if (!isMutexType(lockTy)) {
            error(*stmt.lockExpr, "lock requires sync.Mutex");
        }
    }

    // 进入 lock 块：设置标志
    bool oldInLock = inLockBlock_;
    inLockBlock_ = true;
    if (stmt.body) checkBlock(*stmt.body);
    inLockBlock_ = oldInLock;
}
```

**新增字段**（SemAnalyzer 类）：

```cpp
bool inLockBlock_ = false;  // 当前是否在 lock 块内（参考 inSyncThreadBlock_）
```

**L3 实现**：在 `checkReturnStmt`/`checkBreakStmt`/`checkContinueStmt` 中检查 `if (inLockBlock_)` 报错。

**L4 实现**：在 await 表达式检查中检查 `if (inLockBlock_)` 报错。

**L6 实现**：在 [checkSpawnStmt:292](file:///d:/you/Aura/src/Sema/Checker/StmtChecker.cpp#L292) 中检查 `if (inLockBlock_)` 报错。

**调度**：在 StmtChecker 的 genStmt 对应 dispatch 处新增 `if (auto* l = dynamic_cast<const LockStmt*>(&stmt)) { checkLockStmt(*l); return; }`。

### 3.4 CodeGen：genLockStmt

**文件**：`src/CodeGen/StmtGen.cpp`

**生成策略**（v1.0 仅 Mutex 分支）：生成 RAII guard，生命周期限制在块作用域内。

```cpp
void CodeGenerator::genLockStmt(std::ostream& cpp, const LockStmt& stmt,
                                  bool isCoroutine) {
    // isCoroutine 必须为 false（L4 已禁 await，块内非协程）
    // v1.0: 仅 Mutex 分支；v1.1 扩展 Once/RWMutex 分派
    std::string lockExpr = genExpr(*stmt.lockExpr, false);

    cpp << indentStr() << "{\n";
    indentLevel_++;
    writeLine(cpp, "auto _guard = aura_rt::__acquire_lock(" + lockExpr + ");");
    if (stmt.body) genBlock(cpp, *stmt.body, false);
    // _guard 在块结束析构，自动 unlock
    indentLevel_--;
    cpp << indentStr() << "}\n";
}
```

**调度**：在 [genStmt:22](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L22) 的 dispatch 链中新增（SyncStmt 分支后）：

```cpp
if (auto* l = dynamic_cast<const LockStmt*>(&stmt))
    { genLockStmt(cpp, *l, isCoroutine); return; }
```

**关键**：guard 名为 `_guard`（预留名），用户不可见、不可引用。lockExpr 的类型由 Sema 推断（`lockExpr->inferredType`），v1.1 CodeGen 通过该类型分派不同生成策略。

### 3.5 运行时：Mutex + 间接指针 + 终结器

**新增文件**：`runtime/builtin/mutex.h`、`runtime/builtin/mutex.cpp`

#### 3.5.1 关键设计：间接持有不可移动原语

`std::timed_mutex` / `std::atomic<bool>` / `std::atomic<int>` 不可移动（删除了移动构造）。但 GcObject 在 compact GC 时会被 `memcpy` 搬迁到新页——这会破坏内嵌同步原语的内部状态（持锁状态丢失、临界区数据结构损坏）。

**解决方案**：Mutex/RWMutex/Once 内部用**裸指针**指向 `new` 分配的同步原语。GC compact 时 GcObject 主体搬迁，但指针指向的堆原语不动。原语的生命周期与 GC 对象一致，但**需要终结器释放**（见 §3.5.3）。

#### 3.5.2 mutex.h（v1.0 实际实施代码）

**关键**：v1.0 初版用 `std::mutex` + 阻塞 `lock()`，在 test_gc_mutex Test 4（并发 lock + GC 压力测试）触发死锁，已修复（见 [plan/done/gc_mutex_deadlock_fix_report.md](file:///d:/you/Aura/plan/done/gc_mutex_deadlock_fix_report.md) Bug 2/6）。最终实施代码如下：

```cpp
#pragma once
#include "../gc.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace aura_rt {

struct Mutex : GcObject {
    std::timed_mutex* m_;  // 间接指针（timed_mutex 不可移动；用 timed_mutex 支持 try_lock）

    static const TypeDescriptor _desc;

    // Guard 关键设计（v1.0 死锁修复确立）：
    //   1. GcRootHandle<Mutex*> gcRoot_：持锁期间防 compact 搬迁悬垂
    //   2. try_lock() 轮询 + gc_safepoint()：避免持锁线程被 STW 暂停时本线程阻塞
    //   3. locked_ 标志：避免 TSan 误报 "unlock of an unlocked mutex"
    class Guard {
    public:
        explicit Guard(Mutex* m) : m_(m), gcRoot_(m_), locked_(false) {
            while (!m_->m_->try_lock()) {
                gc_safepoint();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            locked_ = true;
        }
        ~Guard() {
            if (locked_ && m_) {
                m_->m_->unlock();
                locked_ = false;
            }
        }
        Guard(Guard&& o) noexcept : m_(o.m_), gcRoot_(m_), locked_(o.locked_) {
            gcRoot_.rebind(m_);
            o.m_ = nullptr;
            o.locked_ = false;
        }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        Guard& operator=(Guard&&) = delete;
    private:
        Mutex* m_;
        GcRootHandle<Mutex*> gcRoot_;
        bool locked_;
    };

    Guard acquire() { return Guard(this); }
};

inline Mutex::Guard __acquire_lock(Mutex* m) { return m->acquire(); }

inline Mutex* make_mutex() {
    auto* m = static_cast<Mutex*>(
        GcHeap::instance().alloc(sizeof(Mutex), &Mutex::_desc));
    m->m_ = new std::timed_mutex();
    return m;
}

} // namespace aura_rt
```

**与初版差异**：
- `std::mutex*` → `std::timed_mutex*`（支持 try_lock）
- `m_->m_->lock()` → `try_lock()` 轮询 + `gc_safepoint()` + `sleep_for(1ms)`
- 新增 `gcRoot_`（防 compact 搬迁悬垂）
- 新增 `locked_` 标志（防 TSan 误报）
- 移动构造正确处理 `gcRoot_.rebind(m_)` + `o.locked_ = false`
- 工厂函数 `make_mutex()` 已并入 §3.5.2 代码块末尾（`m->m_ = new std::timed_mutex()`）

#### 3.5.3 mutex.cpp + 终结器

**关键**：`m_` 是裸指针，指向**非 GC 对象**（独立 new 的堆内存）。GC **不应追踪**该指针（ptrFieldCount=0）。原语释放由终结器完成。

```cpp
// runtime/builtin/mutex.cpp
#include "mutex.h"

namespace aura_rt {

// 终结器：GC 回收 Mutex 时释放间接持有的 std::timed_mutex
// 安全性：finalizer 在 GC STW 期间执行，所有 mutator 线程已暂停，
//        可安全 delete 同步原语（无并发访问）
static void mutex_finalizer(GcObject* o) {
    auto* m = static_cast<Mutex*>(o);
    delete m->m_;
    m->m_ = nullptr;
}

const TypeDescriptor Mutex::_desc = {
    sizeof(Mutex),         // size
    0,                     // ptrFieldCount（m_ 不是 GC 指针）
    nullptr,               // ptrFieldOffsets
    0,                     // inlineArrayFieldCount
    nullptr,               // inlineArrayFields
    mutex_finalizer        // finalizer
};

} // namespace aura_rt
```

**前置依赖**：Aura GC 已支持终结器（见 [types.h:91 finalizer 字段](file:///d:/you/Aura/runtime/types.h#L91)，[gc.cpp:687/770/781 已调用 finalizer](file:///d:/you/Aura/runtime/gc.cpp#L687)）。本 plan 直接复用，无需新增 GC 机制。

**安全性**：
- finalizer 在 GC STW 期间执行，所有 mutator 线程已暂停，`delete` 同步原语时无并发访问
- 不在 finalizer 中获取锁（避免 STW 期间死锁），仅 `delete` 堆内存

#### 3.5.4 构建集成

**修改**：`runtime/aura_rt.h` 新增 `#include "builtin/mutex.h"`

**修改**：`runtime/CMakeLists.txt` 源文件列表新增 `builtin/mutex.cpp`

### 3.6 BuiltinRegistry 注册

**文件**：`src/Sema/BuiltinRegistry.h` [init():205](file:///d:/you/Aura/src/Sema/BuiltinRegistry.h#L205)

```cpp
// types_ 新增（v1.0）：
{"Mutex", {"Mutex", true, true, BuiltinPrim::Other, "aura_rt::Mutex*"}},

// functions_ 新增（v1.0 构造函数）：
{"sync.Mutex", {}, ReturnTypeInfo::Named("Mutex")},
```

**注意**：`Mutex` 不注册 `lock`/`unlock` 方法（用户不能直接调用 `m.lock()`，必须用 `lock (m) { }` 块）。这强制了安全用法。

### 3.7 sync.Mutex() 构造调用 CodeGen

**修改**：`src/CodeGen/ExprGen.cpp`（处理 `sync.Mutex()` 全局函数调用）

当 BuiltinRegistry 查到 `sync.Mutex` 是构造函数时，生成：

```cpp
aura_rt::Mutex* _tmp = aura_rt::make_mutex();
aura_rt::GcRootHandle<aura_rt::Mutex*> _name(_tmp);
```

**复用现有机制**：参考 `channel` 构造函数的处理（[BuiltinRegistry.h:264 channel 构造](file:///d:/you/Aura/src/Sema/BuiltinRegistry.h#L264)），`sync.Mutex` 走相同的"全局函数返回堆对象 + GcRootHandle 保护"路径。

### 3.8 v1.0 测试

**测试文件**：`example/test.aura`

```aura
fun main(io: Io) {
    let counter = [0]
    let m = sync.Mutex()

    sync thread(max = 4) {
        for i in range(1000) {
            spawn (io: Io, m: sync.Mutex, counter: [int]) {
                lock (m) {
                    counter.append(i)
                }
            }
        }
    }

    io.println("count: " + counter.len())   # 输出 1000
}
```

**验收**：
- 输出 `count: 1000`（无丢失、无重复）
- 无 crash、无 ASAN 报错
- 多次运行结果一致

**Sema 检查测试**：

```aura
fun bad(m: sync.Mutex) {
    lock (m) {
        return  # 错误：cannot return out of lock block
    }
}

fun bad2() {
    lock (42) { }  # 错误：lock requires sync.Mutex
}
```

### 3.9 v1.0 实施步骤

| 步骤 | 模块 | 文件 | 依赖 |
|:---|:---|:---|:---|
| 1 | 运行时 | `runtime/builtin/mutex.h`（新增）, `mutex.cpp`（新增） | 无 |
| 2 | 运行时 | `runtime/aura_rt.h`, `runtime/CMakeLists.txt` | 步骤 1 |
| 3 | Sema | `src/Sema/BuiltinRegistry.h` init() 注册 Mutex 类型 + sync.Mutex 构造 | 步骤 1 |
| 4 | AST | `src/AST/Stmt.h` 新增 LockStmt, `src/AST/Stmt.cpp` print | 无 |
| 5 | Parser | `src/Parser/StmtParser.cpp` parseLockStmt + dispatch | 步骤 4 |
| 6 | Sema | `src/Sema/Checker/StmtChecker.cpp` checkLockStmt + L1/L3/L4/L6 + inLockBlock_ 字段 | 步骤 4 |
| 7 | CodeGen | `src/CodeGen/StmtGen.cpp` genLockStmt + dispatch | 步骤 4 |
| 8 | CodeGen | `src/CodeGen/ExprGen.cpp` sync.Mutex() 构造调用 | 步骤 3 |
| 9 | 构建 | `cmake --build build` + `cmake --build runtime/build` | 步骤 1-8 |
| 10 | 测试 | `example/test.aura` 写入测试，`compile.cmd` 编译，运行 `test.exe` | 步骤 9 |

**总改动**：~220 行

---

## 四、v1.1 详细实施方案

**范围**：sync.RWMutex / sync.Once（**复用 v1.0 的 LockStmt，零 AST/Parser 改动**）。

> **历史说明**：原 v1.1 计划包含 WaitGroup，实施过程中发现 `sync_thread_context` 析构已自动 `waitGroup`（[thread_pool.cpp:201-203](file:///d:/you/Aura/runtime/thread_pool.cpp#L201-203)），sync thread 块结束即等待所有 spawn 完成，WaitGroup 在此设计下冗余。已从 v1.1 移除，推迟到 v1.2 重新设计（详见 [mutex_plan_v1.2.md](file:///d:/you/Aura/plan/mutex_plan_v1.2.md) §A）。

### 4.1 RWMutex（读写锁，写优先）

#### 4.1.1 用法

```aura
let rw = sync.RWMutex()
lock (rw.r()) { ... }   # 读临界区（多读并发）
lock (rw.w()) { ... }   # 写临界区（独占）
```

#### 4.1.2 运行时实现

**修改**：`runtime/builtin/mutex.h`（续）

**关键设计（v1.0 死锁修复确立 + v1.1 写优先增强）**：
- `Inner::m` 用 `std::timed_mutex`（非 `std::mutex`），支持 `try_lock()` 轮询
- **删除 `std::condition_variable cv`**：避免 TSan 误报 + STW 死锁
- 所有 Guard 构造/析构用 `try_lock()` 轮询 + `gc_safepoint()` 响应 STW
- 所有 Guard 持有 `GcRootHandle<RWMutex*>` 防 compact 搬迁悬垂 + `locked_` 标志防 TSan 误报
- **v1.1 新增 `waiting_writers` 原子计数器实现写优先**：writer 等待时新 reader 让出，避免 writer 被 reader starve

```cpp
struct RWMutex : GcObject {
    struct Inner {
        std::timed_mutex m;          // ← timed_mutex（非 mutex），支持 try_lock
        std::atomic<int> readers{0};
        std::atomic<bool> writer_active{false};
        // v1.1：等待中的 writer 数量（写优先：reader 看到 writer 等待时让出，
        // 防止持续进入的 reader 把 writer 饿死）
        std::atomic<int> waiting_writers{0};
    };
    Inner* inner_;  // 指向 new 出的 Inner

    static const TypeDescriptor _desc;

    // 读锁守卫：多读并发，与读互斥不与写互斥
    class ReadGuard {
    public:
        explicit ReadGuard(RWMutex* rw)
            : rw_(rw), gcRoot_(rw_), locked_(false) {
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
        GcRootHandle<RWMutex*> gcRoot_;  // 防 compact 搬迁悬垂
        bool locked_;                     // 防 TSan 误报
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

    ReadGuard  r() { return ReadGuard(this); }
    WriteGuard w() { return WriteGuard(this); }
};

// 工厂函数：sync.RWMutex() 构造调用生成
inline RWMutex* make_rwmutex() {
    auto* rw = static_cast<RWMutex*>(
        GcHeap::instance().alloc(sizeof(RWMutex), &RWMutex::_desc));
    rw->inner_ = new RWMutex::Inner();
    return rw;
}
```

**要点**：
- **写优先机制**：`waiting_writers` 原子计数器，writer 等待时递增；`ReadGuard` 进入前检查 `waiting_writers == 0`，有 writer 等待时让出
- `writer_active` 用 `atomic<bool>`，避免读 ReadGuard 时持锁（只需 acquire load）
- 若 `try_lock` 失败立即 `gc_safepoint()`，将 STW 延迟控制在 ~1ms 内

#### 4.1.3 BuiltinRegistry 注册

```cpp
// types_ 新增：
{"RWMutex", {"RWMutex", true, true, BuiltinPrim::Other, "aura_rt::RWMutex*"}},

// functions_ 新增（构造）：
{"sync.RWMutex", {}, ReturnTypeInfo::Named("RWMutex")},

// methods_ 新增（r()/w() 返回锁视图）：
{"RWMutex", "r", {}, ReturnTypeInfo::Named("RWMutexReadView")},   // Sema 层虚拟类型
{"RWMutex", "w", {}, ReturnTypeInfo::Named("RWMutexWriteView")},
```

#### 4.1.4 CodeGen 分派

**修改**：`src/CodeGen/StmtGen.cpp` genLockStmt 增加类型分派（v1.1 实际实施）：

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

    if (typeName == "RWMutexReadView") {
        // lock (rw.r()) { body } → auto _guard = rw->r();
        cpp << indentStr() << "{\n";
        indentLevel_++;
        writeLine(cpp, "auto _guard = " + lockExpr + ";");
        if (stmt.body) genBlock(cpp, *stmt.body, false);
        indentLevel_--;
        cpp << indentStr() << "}\n";
        return;
    }

    if (typeName == "RWMutexWriteView") {
        // lock (rw.w()) { body } → auto _guard = rw->w();
        cpp << indentStr() << "{\n";
        indentLevel_++;
        writeLine(cpp, "auto _guard = " + lockExpr + ";");
        if (stmt.body) genBlock(cpp, *stmt.body, false);
        indentLevel_--;
        cpp << indentStr() << "}\n";
        return;
    }

    // 默认：Mutex —— RAII guard
    cpp << indentStr() << "{\n";
    indentLevel_++;
    writeLine(cpp, "auto _guard = aura_rt::__acquire_lock(" + lockExpr + ");");
    if (stmt.body) genBlock(cpp, *stmt.body, false);
    indentLevel_--;
    cpp << indentStr() << "}\n";
}
```

### 4.2 Once（一次性执行）

#### 4.2.1 用法

```aura
let once = sync.Once()
lock (once) {              # 首次进入执行块体；后续 lock(once) 跳过整个块
    # 初始化代码
}
```

#### 4.2.2 运行时实现

**关键设计**：原方案用 `std::call_once`，但 `call_once` 内部用 mutex 同步，等待线程会阻塞，阻塞期间无法响应 STW（与 v1.0 Mutex::Guard `lock()` 同样的死锁模式）。

**改用** `atomic<bool>` + `timed_mutex` 双检查 + try_lock 轮询 + `std::lock_guard` + `adopt_lock` 异常安全：

```cpp
struct Once : GcObject {
    std::timed_mutex*    m_;    // 间接指针
    std::atomic<bool>*   done_; // 间接指针

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
        // 否则 m_ 永久持锁 → 其他线程死锁在 try_lock 轮询
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
```

**与初版差异**：
- 改用 `std::lock_guard<std::timed_mutex> lk(*m_, std::adopt_lock)` 替代裸 `m_->unlock()`
- 理由：若 `f()` 抛异常，裸 `unlock()` 不执行，`m_` 永久持锁 → 其他线程死锁在 try_lock 轮询

**要点**：
- fast path（已初始化）完全无锁，性能与原 `call_once` fast path 一致
- 慢路径用 try_lock 轮询，避免阻塞期间无法响应 STW
- `m_` / `done_` 均为间接指针，由终结器释放（见 §4.3）

#### 4.2.3 BuiltinRegistry 注册

```cpp
// types_ 新增：
{"Once", {"Once", true, true, BuiltinPrim::Other, "aura_rt::Once*"}},

// functions_ 新增（构造）：
{"sync.Once", {}, ReturnTypeInfo::Named("Once")},
```

**注意**：`Once` 不注册任何方法（`lock (once) { }` 是唯一用法，内部 call_once）。

### 4.3 终结器扩展

**修改**：`runtime/builtin/mutex.cpp` 新增两个终结器（Once 释放 `m_` + `done_`）：

```cpp
static void rwmutex_finalizer(GcObject* o) {
    auto* rw = static_cast<RWMutex*>(o);
    delete rw->inner_;
    rw->inner_ = nullptr;
}

static void once_finalizer(GcObject* o) {
    auto* once = static_cast<Once*>(o);
    delete once->m_;
    delete once->done_;     // ← Once 改用 atomic<bool> + timed_mutex 后需释放
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

**安全性**：finalizer 在 GC STW 期间执行，所有 mutator 线程已暂停，`delete` 同步原语时无并发访问。不在 finalizer 中获取锁（避免 STW 期间死锁），仅 `delete` 堆内存。

### 4.4 Sema L1 扩展

**修改**：`src/Sema/Checker/StmtChecker.cpp` checkLockStmt 的 L1 规则扩展（v1.1 实际实施）：

```cpp
// v1.1: lockExpr 类型扩展为 Mutex*/RWMutexReadView/RWMutexWriteView/Once*
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
```

### 4.5 v1.1 测试

**Once 测试**：

```aura
fun main(io: Io) {
    let config : [int] = []
    let once = sync.Once()

    sync thread(max = 4) {
        for i in range(1000) {
            spawn (config: [int], once: sync.Once, i: int) {
                lock (once) {                       # 仅首次进入 body
                    config.append(i)
                    config.append(42)
                }
            }
        }
    }

    io.println("config.len=" + config.len())         # 2（仅首次执行）
}
```

**RWMutex 测试**：

```aura
fun main(io: Io) {
    let cache : [int] = []
    let rw = sync.RWMutex()

    sync thread(max = 4) {
        for i in range(500) {
            spawn (cache: [int], rw: sync.RWMutex, i: int) {
                if (i % 50 == 0) {
                    lock (rw.w()) {                 # 写锁：独占
                        cache.append(i)
                    }
                } else {
                    lock (rw.r()) {                 # 读锁：多读并发
                        let _ = cache.len()
                    }
                }
            }
        }
    }
}
```

**验收**：
- Once：`config.len=2`（仅首次执行）
- RWMutex：无死锁、无 crash、无 ASAN 报错
- 连续 5 次运行无死锁、无 abort、无 TSan 警告（参考 v1.0 验收流程）

### 4.6 v1.1 实施步骤

| 步骤 | 模块 | 文件 | 依赖 |
|:---|:---|:---|:---|
| 1 | 运行时 | `runtime/builtin/mutex.h` 续写 RWMutex/Once（全部 safepoint 感知 + waiting_writers 写优先 + adopt_lock 异常安全） | v1.0 完成 |
| 2 | 运行时 | `runtime/builtin/mutex.cpp` 续写两个终结器（Once 释放 `m_` + `done_`） + TypeDescriptor | 步骤 1 |
| 3 | Sema | `src/Sema/BuiltinRegistry.h` 注册两个类型 + 构造 + r/w 方法 | 步骤 1 |
| 4 | Sema | `src/Sema/Checker/StmtChecker.cpp` L1 扩展（Mutex/RWMutexReadView/RWMutexWriteView/Once） | v1.0 步骤 6 |
| 5 | CodeGen | `src/CodeGen/StmtGen.cpp` genLockStmt 增加 Once/RWMutexReadView/RWMutexWriteView 分派 | v1.0 步骤 7 |
| 6 | CodeGen | `src/CodeGen/ExprGen.cpp` sync.RWMutex/Once 构造调用 | 步骤 3 |
| 7 | **Safepoint 验收** | `Select-String -Path runtime\builtin\mutex.h -Pattern "gc_safepoint\(\)" \| Measure-Object \| Select-Object Count` 期望 Count ≥ 4（Mutex/RWMutex.r/.w/Once） | 步骤 1-6 |
| 8 | 构建 | `cmake --build build` + `cmake --build runtime/build` | 步骤 1-7 |
| 9 | 测试 | `example/test.aura` 写入 Once/RWMutex 并发压力测试（≥1000 spawn + 频繁 GC） | 步骤 8 |
| 10 | **稳定性验收** | 连续 5 次运行无死锁、无 abort、无 TSan 警告（参考 v1.0 验收流程） | 步骤 9 |

**Safepoint 验收检查点（强制）**：
- 步骤 7：每个 Guard 构造必须走过 `try_lock` 或 `sleep_for` 轮询路径时调用 `gc_safepoint()`
- 步骤 10：必须运行并发压力测试（≥1000 spawn + 频繁 `gc_force_major()`），连续 5 次无死锁、无 abort、无 TSan 警告

**总改动**：v1.1 增量 ~250 行（含 Guard 增加 `gcRoot_` + `locked_` + safepoint 轮询逻辑 + waiting_writers + adopt_lock）

---

## 五、可能遇到的问题

### 5.1 MinGW shared_mutex bug（已规避）

`std::shared_mutex` 在 MinGW 下有 bug（msys2/MINGW-packages#25193，`lock_shared` 断言）。RWMutex 实现用 `std::timed_mutex` + 读者计数 + `std::atomic<bool>` 模拟（无 cv），规避该 bug。同时这也满足 §5.6 的 safepoint 感知要求（`timed_mutex` 支持 `try_lock()` 轮询）。

### 5.2 compact GC 损坏同步原语（已规避）

`std::timed_mutex`/`std::atomic<bool>`/`std::atomic<int>` 不可移动，但 GcObject compact 时会被 memcpy 搬迁。解决方案：所有锁族对象用间接指针持有同步原语（见 §3.5.1），终结器释放（见 §3.5.3 和 §4.3）。`Mutex::Guard` 内部还持有 `GcRootHandle<Mutex*>`，使 m_ 进入 GC roots，compact 时 `GcHeap::updateAllReferences` 会自动更新 m_ 指向新地址（见 §5.7）。

### 5.3 finalizer 中死锁风险（已规避）

finalizer 在 GC STW 期间执行，所有 mutator 线程已暂停。**不在 finalizer 中获取锁**（避免 STW 期间死锁），仅 `delete` 堆内存。

### 5.4 lockExpr 类型推断

`lockExpr->inferredType` 由 Sema 标注（[ASTNode.h:21](file:///d:/you/Aura/src/AST/ASTNode.h#L21)）。v1.1 CodeGen 通过该字段分派 Once/RWMutex。需确认 Sema 对方法调用（`rw.r()`）的返回类型推断正确（返回 RWMutexReadView/RWMutexWriteView 虚拟类型）。

### 5.5 `lock` 软关键字歧义

`lock` 作为普通标识符仍可用（如 `let lock = ...`）。Parser 仅在语句起始位置 + 后续 `(` 时识别为 LockStmt。需测试 `let lock = sync.Mutex(); lock (lock) { }` 这类同名场景。

### 5.6 STW safepoint 感知原则（v1.0 死锁修复确立）

**所有锁族对象的 Guard 构造、wait 操作必须 safepoint 感知**，否则会重蹈 v1.0 test_gc_mutex Test 4 死锁覆辙。详见 [plan/done/gc_mutex_deadlock_fix_report.md](file:///d:/you/Aura/plan/done/gc_mutex_deadlock_fix_report.md)。

具体规范：

1. **Guard 构造**：禁止用 `mutex::lock()` 阻塞获取，必须用 `try_lock()` 轮询 + `gc_safepoint()` 响应 STW
   - 参考 v1.0 已实施代码：[runtime/builtin/mutex.h](file:///d:/you/Aura/runtime/builtin/mutex.h) `Mutex::Guard` 实现
   - 每次轮询失败后 `gc_safepoint()` + `sleep_for(1ms)` 重试，将 STW 延迟控制在 ~1ms 内

2. **wait 操作**：禁止用 `cv.wait/wait_for`，必须用 `unlock + sleep_for(1ms) + lock` 轮询 + `gc_safepoint()`
   - 参考 v1.0 已实施代码：[runtime/gc.cpp](file:///d:/you/Aura/runtime/gc.cpp) `safepoint()` 非 initiator 等待路径
   - 原因：①GCC 11 TSan 对 `pthread_cond_timedwait` 追踪有 bug（误报 "double lock of a mutex"） ②阻塞期间无法响应 STW

3. **信号量获取**：禁止用 `sem.acquire()`，必须用 `try_acquire_for(1ms)` 轮询 + `gc_safepoint()`
   - 参考 v1.0 已实施代码：[runtime/thread_pool.cpp](file:///d:/you/Aura/runtime/thread_pool.cpp) `sync_thread_context::submit` 修复

4. **跨锁序检查**：长时间持有锁时禁止调用 `gc_safepoint()`（会形成锁序反转）
   - 参考 v1.0 已实施代码：[runtime/thread_pool.cpp](file:///d:/you/Aura/runtime/thread_pool.cpp) `waitGroup` 改为原子轮询，避免 `groupM_ ↔ all_stopped_m_` 锁序环

### 5.7 Guard 安全规范（v1.0 死锁修复确立）

所有 Guard 必须满足以下两条规范：

1. **`locked_` 标志**：显式记录锁所有权状态
   - 避免 TSan 误报 "unlock of an unlocked mutex"（GCC 11 TSan 对 `pthread_mutex_timedlock` 内部状态追踪有 bug）
   - 在移动构造中正确处理：源 Guard 的 `locked_` 置 false，目标 Guard 接管所有权
   - 参考 v1.0 已实施代码：[runtime/builtin/mutex.h](file:///d:/you/Aura/runtime/builtin/mutex.h) `Mutex::Guard::locked_` 字段

2. **`GcRootHandle<T*>`**：持锁期间 GC 对象可能被 compact 搬迁，Guard 内部必须持有 `GcRootHandle` 引用 m_，使 m_ 进入 GC roots
   - compact 时 `GcHeap::updateAllReferences` 会自动更新 m_ 指向新地址
   - 移动构造后必须调用 `gcRoot_.rebind(m_)` 重新绑定到新地址
   - 参考 v1.0 已实施代码：[runtime/builtin/mutex.h](file:///d:/you/Aura/runtime/builtin/mutex.h) `Mutex::Guard::gcRoot_` 字段

### 5.8 RWMutex 不可重入

`RWMutex` 不支持可重入（同线程重复 `rw.r()` 会死锁）。这是 v1.1 的已知限制，符合 Go 语义。若需可重入，推到 v1.2+。

---

## 六、v1.2 及之后规划

见 [mutex_plan_v1.2.md](file:///d:/you/Aura/plan/mutex_plan_v1.2.md)。

v1.2 范围概要：
- **A. WaitGroup 重新设计**（推迟到具体场景需要时实施，可能配合 thread 版 channel）
- **E. 多锁语句** `lock (a.mu, b.mu) { }`（CodeGen 自动按地址排序后加锁）
- **F. RWMutex 公平性进一步优化**（具体方向待定）
- **L5 嵌套同锁检测**（改为运行时 error，因编译期无法完全覆盖别名情况）

v1.3 范围概要：
- **D. sync.Cond / sync.Semaphore**（需 safepoint 轮询改造）
