# Plan：sync 锁族 + `lock` 块语句实施方案

> 来源：[mutex_issue.md](file:///d:/you/Aura/plan/mutex_issue.md)（已审核通过）
> 类型：详细实施方案（plan）
> 日期：2026-07-24（v1.0 已实施），2026-07-26（v1.1+ 根据 v1.0 死锁修复调整）
> 状态：v1.0 已完成；v1.1+ 已根据 v1.0 死锁修复教训调整（等待审查）
> 关联：[sync_thread_plan.md](file:///d:/you/Aura/plan/sync_thread_plan.md)，
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
7. **覆盖常见并发模式**：互斥、读写分离、一次性初始化、等待组

---

## 二、锁族规划

### 2.1 锁种类清单

| 类型 | 用途 | 对应 C++ 原语 | v1 优先级 |
|:---|:---|:---|:---|
| **sync.Mutex** | 互斥锁（独占） | `std::timed_mutex` + 间接指针 | P0（v1.0 已实施） |
| **sync.RWMutex** | 读写锁（多读单写） | `std::timed_mutex` + `atomic<int>` + 间接指针 | P1（v1.1） |
| **sync.Once** | 一次性执行 | `std::timed_mutex` + `atomic<bool>` + 间接指针 | P1（v1.1） |
| **sync.WaitGroup** | 等待一组任务完成 | `atomic<int>` + 间接指针（无 mutex/cv） | P1（v1.1） |
| sync.Cond | 条件变量 | `std::condition_variable`（需 safepoint 轮询） | P3（远期） |
| sync.Semaphore | 信号量 | `std::counting_semaphore`（需 try_acquire_for 轮询） | P3（远期） |

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
lock (wg) { }          # WaitGroup 登记（add(1)/done() 包裹）
```

**关键设计**：括号内的 `lockExpr` 是普通表达式，由 Sema 推断其类型。lockExpr 类型决定 lock 语义（独占/读/写/一次/登记）。新增锁类型只需运行时实现对应语义，**零 AST/Parser 改动**（v1.0 的 LockStmt 设计已覆盖所有场景）。

### 2.3 v1 范围与分期

- **v1.0（本 plan 详细范围）**：sync.Mutex + `lock` 块语句（AST/Parser/Sema/CodeGen 全链路）
- **v1.1**：sync.RWMutex / sync.Once / sync.WaitGroup（**复用 v1.0 的 LockStmt，零 AST/Parser 改动**，仅运行时 + BuiltinRegistry + CodeGen 分派）
- **远期**：sync.Cond / sync.Semaphore（同样复用 lock 块语法）

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
- **无 Mode 字段**：模式（独占/读/写/一次/登记）由 lockExpr 求值结果的类型决定
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
| **L1** | `lockExpr` 求值结果必须是锁类型：`Mutex*`（v1.0 仅此；v1.1 扩展 RWMutex.r()/.w()/Once/WaitGroup） | "lock requires sync.Mutex" |
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
    // v1.0: 仅 Mutex 分支；v1.1 扩展 Once/WaitGroup/RWMutex 分派
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

**解决方案**：Mutex/RWMutex/Once/WaitGroup 内部用**裸指针**指向 `new` 分配的同步原语。GC compact 时 GcObject 主体搬迁，但指针指向的堆原语不动。原语的生命周期与 GC 对象一致，但**需要终结器释放**（见 §3.5.3）。

#### 3.5.2 mutex.h（v1.0 实际实施代码）

**关键**：v1.0 初版用 `std::mutex` + 阻塞 `lock()`，在 test_gc_mutex Test 4（并发 lock + GC 压力测试）触发死锁，已修复（见 [plan/done/gc_mutex_deadlock_fix_report.md](file:///d:/you/Aura/plan/done/gc_mutex_deadlock_fix_report.md) Bug 2/6）。最终实施代码如下：

```cpp
#pragma once
#include "../gc.h"
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

#### 3.5.5 构建集成

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

**范围**：sync.RWMutex / sync.Once / sync.WaitGroup，**复用 v1.0 的 LockStmt，零 AST/Parser 改动**。

### 4.1 RWMutex（读写锁）

#### 4.1.1 用法

```aura
let rw = sync.RWMutex()
lock (rw.r()) { ... }   # 读临界区（多读并发）
lock (rw.w()) { ... }   # 写临界区（独占）
```

#### 4.1.2 运行时实现

**修改**：`runtime/builtin/mutex.h`（续）

**关键设计（v1.0 死锁修复确立）**：
- `Inner::m` 用 `std::timed_mutex`（非 `std::mutex`），支持 `try_lock()` 轮询
- **删除 `std::condition_variable cv`**：避免 TSan 误报 + STW 死锁
- 所有 Guard 构造/析构用 `try_lock()` 轮询 + `gc_safepoint()` 响应 STW
- 所有 Guard 持有 `GcRootHandle<RWMutex*>` 防 compact 搬迁悬垂 + `locked_` 标志防 TSan 误报

```cpp
struct RWMutex : GcObject {
    struct Inner {
        std::timed_mutex m;          // ← timed_mutex（非 mutex），支持 try_lock
        std::atomic<int> readers{0};
        std::atomic<bool> writer_active{false};
        // 注：删除 cv（避免 TSan 误报 + STW 死锁）
    };
    Inner* inner_;  // 指向 new 出的 Inner

    static const TypeDescriptor _desc;

    class ReadGuard {
    public:
        explicit ReadGuard(RWMutex* rw)
            : rw_(rw), gcRoot_(rw), locked_(false) {
            // try_lock 轮询 + safepoint 响应 STW
            while (true) {
                if (rw_->inner_->m.try_lock()) {
                    if (!rw_->inner_->writer_active.load(std::memory_order_acquire)) {
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
            : rw_(o.rw_), gcRoot_(o.rw_), locked_(o.locked_) {
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

    class WriteGuard {
    public:
        explicit WriteGuard(RWMutex* rw)
            : rw_(rw), gcRoot_(rw), locked_(false) {
            // try_lock 轮询 + safepoint 响应 STW
            while (true) {
                if (rw_->inner_->m.try_lock()) {
                    if (rw_->inner_->readers.load(std::memory_order_acquire) == 0
                        && !rw_->inner_->writer_active.load(std::memory_order_acquire)) {
                        rw_->inner_->writer_active.store(true, std::memory_order_release);
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
        ~WriteGuard() {
            if (locked_ && rw_) {
                rw_->inner_->writer_active.store(false, std::memory_order_release);
                locked_ = false;
            }
        }
        WriteGuard(WriteGuard&& o) noexcept
            : rw_(o.rw_), gcRoot_(o.rw_), locked_(o.locked_) {
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
    ReadGuard r() { return ReadGuard(this); }
    WriteGuard w() { return WriteGuard(this); }
};

// __acquire_lock 重载：消费 r()/w() 返回的临时 Guard
inline RWMutex::ReadGuard __acquire_lock(RWMutex::ReadGuard&& v) {
    return std::move(v);  // 已在 r() 构造时 acquire
}
inline RWMutex::WriteGuard __acquire_lock(RWMutex::WriteGuard&& v) {
    return std::move(v);  // 已在 w() 构造时 acquire
}
```

**要点**：
- 读优先级 vs 写优先级：当前实现公平性较弱（reader 可能 starve writer），v1.2 可引入排队计数器
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

**改用** `atomic<bool>` + `timed_mutex` 双检查 + try_lock 轮询：

```cpp
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
        // 双检查：持锁后再检查 done_
        if (!done_->load(std::memory_order_acquire)) {
            f();
            done_->store(true, std::memory_order_release);
        }
        m_->unlock();
    }
};
```

**工厂函数**：

```cpp
inline Once* make_once() {
    auto* o = static_cast<Once*>(
        GcHeap::instance().alloc(sizeof(Once), &Once::_desc));
    o->m_ = new std::timed_mutex();
    o->done_ = new std::atomic<bool>(false);
    return o;
}
```

**要点**：
- fast path（已初始化）完全无锁，性能与原 `call_once` fast path 一致
- 慢路径用 try_lock 轮询，避免阻塞期间无法响应 STW
- `m_` / `done_` 均为间接指针，由终结器释放（见 §4.4）

#### 4.2.3 CodeGen 分派扩展

**修改**：`src/CodeGen/StmtGen.cpp` genLockStmt 增加类型分派：

```cpp
void CodeGenerator::genLockStmt(std::ostream& cpp, const LockStmt& stmt,
                                  bool isCoroutine) {
    std::string lockExpr = genExpr(*stmt.lockExpr, false);
    // 由 Sema 推断的 lockExpr 类型分派
    auto* lockTy = stmt.lockExpr->inferredType;
    std::string typeName = lockTy ? lockTy->name : "";

    if (typeName == "Once") {
        // lock (once) { body } → once->do_([&]{ body })
        writeLine(cpp, lockExpr + "->do_([&] {");
        indentLevel_++;
        if (stmt.body) genBlock(cpp, *stmt.body, false);
        indentLevel_--;
        writeLine(cpp, "});");
        return;
    }
    // ... WaitGroup 分派见 4.3.3
    // 默认：Mutex / RWMutex.r() / RWMutex.w() —— RAII guard
    cpp << indentStr() << "{\n";
    indentLevel_++;
    writeLine(cpp, "auto _guard = aura_rt::__acquire_lock(" + lockExpr + ");");
    if (stmt.body) genBlock(cpp, *stmt.body, false);
    indentLevel_--;
    cpp << indentStr() << "}\n";
}
```

#### 4.2.4 BuiltinRegistry 注册

```cpp
// types_ 新增：
{"Once", {"Once", true, true, BuiltinPrim::Other, "aura_rt::Once*"}},

// functions_ 新增（构造）：
{"sync.Once", {}, ReturnTypeInfo::Named("Once")},
```

**注意**：`Once` 不注册任何方法（`lock (once) { }` 是唯一用法，内部 call_once）。

### 4.3 WaitGroup（等待组）

#### 4.3.1 用法

```aura
let wg = sync.WaitGroup()
sync thread(max = 4) {
    for i in range(100) {
        spawn (wg: sync.WaitGroup) {
            lock (wg) {       # 进入：wg.add(1)
                # 任务体
            }                  # 离开：wg.done()
        }
    }
}
wg.wait()                     # 等待所有 lock(wg) 块完成
```

#### 4.3.2 运行时实现

**关键设计（v1.0 死锁修复确立）**：
- 原方案用 `mutex m` + `cv.wait()`，问题：①持 `inner_->m` 期间触发 STW 会形成 `waitGroup::m_ ↔ all_stopped_m_` 锁序反转（Bug 5） ②`cv.wait()` 触发 TSan 误报（Bug 8）
- **改用纯原子轮询**：删除 mutex 和 cv，直接轮询 `atomic<int>` + `gc_safepoint()` 响应 STW（参考 v1.0 [thread_pool.cpp](file:///d:/you/Aura/runtime/thread_pool.cpp) `ThreadPool::waitGroup` 修复模式）

```cpp
struct WaitGroup : GcObject {
    struct Inner {
        std::atomic<int> count{0};
        // 注：删除 mutex 和 cv（避免 TSan 误报 + STW 死锁）
    };
    Inner* inner_;  // 间接指针

    static const TypeDescriptor _desc;

    void add(int n = 1) {
        inner_->count.fetch_add(n, std::memory_order_acq_rel);
    }

    void done() {
        // 仅原子减 1，无需通知 cv（wait 用轮询）
        inner_->count.fetch_sub(1, std::memory_order_acq_rel);
    }

    void wait() {
        // 原子轮询 + safepoint 响应 STW（参考 v1.0 ThreadPool::waitGroup 修复）
        // 注：不用 cv.wait — GCC 11 TSan 对 pthread_cond_timedwait 的
        //     mutex 释放/重获追踪有 bug，会误报 "double lock of a mutex"
        while (inner_->count.load(std::memory_order_acquire) > 0) {
            gc_safepoint();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
};
```

**工厂函数**：

```cpp
inline WaitGroup* make_waitgroup() {
    auto* wg = static_cast<WaitGroup*>(
        GcHeap::instance().alloc(sizeof(WaitGroup), &WaitGroup::_desc));
    wg->inner_ = new WaitGroup::Inner();
    return wg;
}
```

**要点**：
- `add`/`done` 全部用原子操作，无锁，无数据竞争
- `wait` 轮询间隔 1ms，将 STW 延迟控制在 ~1ms 内
- 无 mutex 持有，避免锁序反转风险

#### 4.3.3 CodeGen 分派扩展

```cpp
if (typeName == "WaitGroup") {
    // lock (wg) { body } → { wg->add(1); auto _g = finally([&]{ wg->done(); }); body }
    cpp << indentStr() << "{\n";
    indentLevel_++;
    writeLine(cpp, lockExpr + "->add(1);");
    writeLine(cpp, "auto _guard = aura_rt::__finally([&]{ " + lockExpr + "->done(); });");
    if (stmt.body) genBlock(cpp, *stmt.body, false);
    indentLevel_--;
    cpp << indentStr() << "}\n";
    return;
}
```

**`__finally` 辅助**（runtime/builtin/mutex.h）：

```cpp
template <typename F>
class Finally {
public:
    explicit Finally(F&& f) : f_(std::move(f)), active_(true) {}
    ~Finally() { if (active_) f_(); }
    Finally(Finally&& o) noexcept : f_(std::move(o.f_)), active_(o.active_) { o.active_ = false; }
    Finally(const Finally&) = delete;
private:
    F f_;
    bool active_;
};
template <typename F> Finally(F) -> Finally<F>;
template <typename F>
auto __finally(F&& f) { return Finally<std::decay_t<F>>(std::forward<F>(f)); }
```

#### 4.3.4 BuiltinRegistry 注册

```cpp
// types_ 新增：
{"WaitGroup", {"WaitGroup", true, true, BuiltinPrim::Other, "aura_rt::WaitGroup*"}},

// functions_ 新增（构造）：
{"sync.WaitGroup", {}, ReturnTypeInfo::Named("WaitGroup")},

// methods_ 新增：
{"WaitGroup", "wait", {}, ReturnTypeInfo::None()},
{"WaitGroup", "add", {"int"}, ReturnTypeInfo::None()},
```

**注意**：`WaitGroup` 注册 `wait()` 和 `add(n)`（`add` 仅罕见批量场景；常规 `lock (wg) { }` 自动 add(1)/done()，`done` 不暴露给用户）。

### 4.4 终结器扩展

**修改**：`runtime/builtin/mutex.cpp` 新增三个终结器（Once 因新增 `done_` 间接指针，需补释放）：

```cpp
static void rwmutex_finalizer(GcObject* o) {
    auto* rw = static_cast<RWMutex*>(o);
    delete rw->inner_;
    rw->inner_ = nullptr;
}
static void once_finalizer(GcObject* o) {
    auto* once = static_cast<Once*>(o);
    delete once->m_;
    delete once->done_;     // ← 新增：Once 改用 atomic<bool> + timed_mutex 后需释放
    once->m_ = nullptr;
    once->done_ = nullptr;
}
static void waitgroup_finalizer(GcObject* o) {
    auto* wg = static_cast<WaitGroup*>(o);
    delete wg->inner_;
    wg->inner_ = nullptr;
}

const TypeDescriptor RWMutex::_desc = { sizeof(RWMutex), 0, nullptr, 0, nullptr, rwmutex_finalizer };
const TypeDescriptor Once::_desc = { sizeof(Once), 0, nullptr, 0, nullptr, once_finalizer };
const TypeDescriptor WaitGroup::_desc = { sizeof(WaitGroup), 0, nullptr, 0, nullptr, waitgroup_finalizer };
```

**安全性**：finalizer 在 GC STW 期间执行，所有 mutator 线程已暂停，`delete` 同步原语时无并发访问。不在 finalizer 中获取锁（避免 STW 期间死锁），仅 `delete` 堆内存。

### 4.5 Sema L1 扩展

**修改**：`src/Sema/Checker/StmtChecker.cpp` checkLockStmt 的 L1 规则扩展：

```cpp
// v1.1: lockExpr 类型扩展为 Mutex*/RWMutexReadView/RWMutexWriteView/Once*/WaitGroup*
if (!isMutexType(lockTy) && !isRWMutexViewType(lockTy)
    && !isOnceType(lockTy) && !isWaitGroupType(lockTy)) {
    error(*stmt.lockExpr, "lock requires sync.Mutex/RWMutex.r()/.w()/Once/WaitGroup");
}
```

新增 L7 规则：

| 规则 | 说明 | 错误信息 |
|:---|:---|:---|
| **L5** | 禁止嵌套同一把锁（best-effort，仅对 Mutex/RWMutex） | "nested lock on same mutex may deadlock"（警告） |
| **L7** | `lock (wg) { }` 内禁止再嵌套 `lock (wg) { }`（同一 WaitGroup） | "nested lock on same WaitGroup" |

### 4.6 v1.1 测试

**Once 测试**：

```aura
fun main(io: Io) {
    let once = sync.Once()

    sync thread(max = 4) {
        for i in range(100) {
            spawn (io: Io, once: sync.Once, i: int) {
                lock (once) {
                    io.println("init once, i=" + i)   # 仅输出一次
                }
            }
        }
    }

    io.println("done")
}
```

**WaitGroup 测试**：

```aura
fun main(io: Io) {
    let wg = sync.WaitGroup()
    let counter = [0]
    let m = sync.Mutex()

    sync thread(max = 4) {
        for i in range(1000) {
            spawn (wg: sync.WaitGroup, m: sync.Mutex, counter: [int], i: int) {
                lock (wg) {           # add(1) 进入
                    lock (m) {
                        counter.append(i)
                    }
                }                    # done() 离开
            }
        }
    }

    wg.wait()                         # 等待所有 spawn 完成
    io.println("count: " + counter.len())   # 输出 1000
}
```

**验收**：
- Once：仅输出一行 `init once, i=N`（N 为首次进入的 i）
- WaitGroup：输出 `count: 1000`，`wg.wait()` 后所有 spawn 已完成
- 无 crash、无 ASAN 报错

### 4.7 v1.1 实施步骤

| 步骤 | 模块 | 文件 | 依赖 |
|:---|:---|:---|:---|
| 1 | 运行时 | `runtime/builtin/mutex.h` 续写 RWMutex/Once/WaitGroup（全部 safepoint 感知） + __finally | v1.0 完成 |
| 2 | 运行时 | `runtime/builtin/mutex.cpp` 续写三个终结器（Once 释放 `m_` + `done_`） + TypeDescriptor | 步骤 1 |
| 3 | Sema | `src/Sema/BuiltinRegistry.h` 注册三个类型 + 构造 + r/w/wait/add 方法 | 步骤 1 |
| 4 | Sema | `src/Sema/Checker/StmtChecker.cpp` L1 扩展 + L5/L7 | v1.0 步骤 6 |
| 5 | CodeGen | `src/CodeGen/StmtGen.cpp` genLockStmt 增加 Once/WaitGroup 分派 | v1.0 步骤 7 |
| 6 | CodeGen | `src/CodeGen/ExprGen.cpp` sync.RWMutex/Once/WaitGroup 构造调用 | 步骤 3 |
| 7 | **Safepoint 验收** | `grep -n "gc_safepoint" runtime/builtin/mutex.h` 验证至少 4 处（Mutex/RWMutex.r/.w/Once/WaitGroup.wait） | 步骤 1-6 |
| 8 | 构建 | `cmake --build build` + `cmake --build runtime/build` | 步骤 1-7 |
| 9 | 测试 | `example/test.aura` 写入 Once/WaitGroup 并发压力测试（≥1000 spawn + 频繁 GC） | 步骤 8 |
| 10 | **稳定性验收** | 连续 5 次运行无死锁、无 abort、无 TSan 警告（参考 v1.0 验收流程） | 步骤 9 |

**Safepoint 验收检查点（强制）**：
- 步骤 7：每个 Guard 构造、wait 操作必须走过 `try_lock` 或 `sleep_for` 轮询路径时调用 `gc_safepoint()`。可用以下命令验证：
  ```powershell
  Select-String -Path runtime\builtin\mutex.h -Pattern "gc_safepoint\(\)" | Measure-Object | Select-Object Count
  # 期望 Count ≥ 4（Mutex/RWMutex.r/.w/Once/WaitGroup.wait）
  ```
- 步骤 10：必须运行 `test_gc_mutex.aura` 等价的并发压力测试（≥1000 spawn + 频繁 `gc_force_major()`），连续 5 次无死锁、无 abort、无 TSan 警告。

**总改动**：v1.1 增量 ~250 行（比原方案 +50 行，因 Guard 增加 `gcRoot_` + `locked_` + safepoint 轮询逻辑）

---

## 五、细粒度锁模式

细粒度锁（fine-grained locking）指"锁与被保护的数据关联，而非全局一把锁"，以提升并发度。Aura 的锁族天然支持三种细粒度模式：

### 5.1 模式 A：record 内嵌 Mutex 字段（per-instance lock）

最常见形式——每个数据实例自带一把锁，不同实例之间不互斥。

```aura
record Account {
    balance: int
    mu: sync.Mutex
}

fun main(io: Io) {
    let a = Account { balance: 100, mu: sync.Mutex() }
    let b = Account { balance: 200, mu: sync.Mutex() }

    sync thread(max = 4) {
        spawn (a: Account) {
            lock (a.mu) { a.balance -= 10 }
        }
        spawn (b: Account) {
            lock (b.mu) { b.balance += 10 }
        }
    }
}
```

- 每个 Account 实例的 mu 独立，`a` 和 `b` 可并发操作
- Mutex 作为 record 字段：record 是 GC 对象，compact 时会搬迁，但 §3.5.1 的间接指针设计保证 mutex 状态不损坏
- **这就是间接指针设计的核心收益**：record 内嵌的 Mutex 在 compact 后仍指向同一块堆 mutex

### 5.2 模式 B：分段锁（striped lock）

预先分配 N 把锁，按 key 哈希选锁。适用于共享 map / 缓存的并发访问。

```aura
fun main(io: Io) {
    # 16 把锁的分段锁池
    let stripes = [
        sync.Mutex(), sync.Mutex(), sync.Mutex(), sync.Mutex(),
        sync.Mutex(), sync.Mutex(), sync.Mutex(), sync.Mutex(),
        sync.Mutex(), sync.Mutex(), sync.Mutex(), sync.Mutex(),
        sync.Mutex(), sync.Mutex(), sync.Mutex(), sync.Mutex(),
    ]

    sync thread(max = 8) {
        for i in range(1000) {
            spawn (stripes: [sync.Mutex], i: int) {
                let s = stripes[i % 16]
                lock (s) {
                    # 细粒度：只持有一段锁，其他段可并发
                }
            }
        }
    }
}
```

- 16 把锁 → 理论并发度 16
- 无需语言级支持，纯库模式（Array<Mutex> + hash 选锁）
- Aura 的 `Array<T>` 已支持 GC 对象元素，Mutex 可直接放入数组

### 5.3 模式 C：per-object 锁（语言级，未来扩展）

类似 Java 的 `synchronized(obj)`，每个对象可选挂载一把锁（lazy attach）。**v1 不实施**，作为未来扩展。

- 优势：语法更简洁，无需显式声明 mu 字段
- 劣势：GcObject 头部需扩展锁指针（8 字节开销，破坏 32 字节头部约束）
- **替代方案**：v1 用模式 A（record 内嵌 mu）达到同等效果

### 5.4 死锁预防：多锁排序

细粒度锁的常见陷阱是"多锁顺序不一致导致死锁"。Aura v1 **不做语言级死锁检测**（仅 L5 best-effort 警告），由用户负责。

**推荐实践**：多锁场景按地址或 id 排序后加锁：

```aura
fun transfer(a: Account, b: Account, amt: int) {
    # 按地址排序，避免 A→B 和 B→A 两个方向同时加锁
    let (first, second) = if (&a < &b) { (a, b) } else { (b, a) }
    lock (first.mu) {
        lock (second.mu) {
            first.balance -= amt
            second.balance += amt
        }
    }
}
```

**未来扩展**（v2+）：可引入 `lock (a.mu, b.mu) { }` 多锁语句，由 CodeGen 自动排序后加锁，消除人为错误。本 plan 不实施。

---

## 六、未来演进

### 6.1 sync.Cond / sync.Semaphore（远期）

```aura
# Cond
let cond = sync.Cond(m)
lock (cond) { cond.wait(); ... }     # 释放锁并等待
lock (cond) { cond.notify(); }       # 唤醒等待者

# Semaphore
let sem = sync.Semaphore(5)
lock (sem) { }                       # acquire/release（与 Mutex 同构）
```

- sync.Cond：内部 `std::condition_variable`，复用 lock 块语法
- sync.Semaphore：内部 `std::counting_semaphore`，acquire/release 与 Mutex 同构

### 6.2 若用户后续需要跨函数持有锁

**不推荐**：跨函数持有锁是已知的死锁高发模式。

**替代方案**：将"持锁操作"封装为闭包，传入 `with_lock(m, fn() { ... })` 函数。语义等价于 lock 块，但可跨函数传递闭包。这仍保证锁在闭包执行完毕后释放，不会泄漏。

---

## 七、锁族完整规划表（参考）

| 类型 | 构造 | 用法 | v1 | 依赖 | STW 安全 |
|:---|:---|:---|:---|:---|:---|
| sync.Mutex | `sync.Mutex()` | `lock (m) { }` | ✅ v1.0 已实施 | sync thread | ✅ try_lock 轮询 |
| sync.RWMutex | `sync.RWMutex()` | `lock (rw.r()) { }` / `lock (rw.w()) { }` | ✅ v1.1 | sync.Mutex | ✅ try_lock 轮询 |
| sync.Once | `sync.Once()` | `lock (once) { }` | ✅ v1.1 | 无 | ✅ atomic + try_lock 双检查 |
| sync.WaitGroup | `sync.WaitGroup()` | `lock (wg) { }` + `wg.wait()` | ✅ v1.1 | sync thread | ✅ 原子轮询，无 mutex/cv |
| sync.Cond | `sync.Cond(m)` | `lock (cond) { cond.wait/notify }` | 远期 | sync.Mutex | ⚠️ 需 safepoint 轮询改造 |
| sync.Semaphore | `sync.Semaphore(n)` | `lock (sem) { }` | 远期 | 无 | ⚠️ 需 try_acquire_for 轮询 |

**统一语法**：所有 sync 原语都用 `lock (lockExpr) { }` 块。lockExpr 类型决定 lock 语义（独占/读/写/一次/登记）。新增锁类型只需运行时实现对应语义，**零 AST/Parser 改动**（v1.0 的 LockStmt 设计已覆盖所有场景）。

**STW 安全**：所有 v1 锁族对象的 Guard 构造和 wait 操作必须 safepoint 感知（见 §8.6），否则会重蹈 v1.0 test_gc_mutex Test 4 死锁覆辙。远期 sync.Cond/Semaphore 同样需要改造。

---

## 八、可能遇到的问题

### 8.1 MinGW shared_mutex bug（已规避）

`std::shared_mutex` 在 MinGW 下有 bug（msys2/MINGW-packages#25193，`lock_shared` 断言）。RWMutex 实现用 `std::timed_mutex` + 读者计数 + `std::atomic<bool>` 模拟（无 cv），规避该 bug。同时这也满足 §8.6 的 safepoint 感知要求（`timed_mutex` 支持 `try_lock()` 轮询）。

### 8.2 compact GC 损坏同步原语（已规避）

`std::timed_mutex`/`std::atomic<bool>`/`std::atomic<int>` 不可移动，但 GcObject compact 时会被 memcpy 搬迁。解决方案：所有锁族对象用间接指针持有同步原语（见 §3.5.1），终结器释放（见 §3.5.3 和 §4.4）。`Mutex::Guard` 内部还持有 `GcRootHandle<Mutex*>`，使 m_ 进入 GC roots，compact 时 `GcHeap::updateAllReferences` 会自动更新 m_ 指向新地址（见 §8.7）。

### 8.3 finalizer 中死锁风险（已规避）

finalizer 在 GC STW 期间执行，所有 mutator 线程已暂停。**不在 finalizer 中获取锁**（避免 STW 期间死锁），仅 `delete` 堆内存。

### 8.4 lockExpr 类型推断

`lockExpr->inferredType` 由 Sema 标注（[ASTNode.h:21](file:///d:/you/Aura/src/AST/ASTNode.h#L21)）。v1.1 CodeGen 通过该字段分派 Once/WaitGroup/RWMutex。需确认 Sema 对方法调用（`rw.r()`）的返回类型推断正确（返回 RWMutexReadView/RWMutexWriteView 虚拟类型）。

### 8.5 `lock` 软关键字歧义

`lock` 作为普通标识符仍可用（如 `let lock = ...`）。Parser 仅在语句起始位置 + 后续 `(` 时识别为 LockStmt。需测试 `let lock = sync.Mutex(); lock (lock) { }` 这类同名场景。

### 8.6 STW safepoint 感知原则（v1.0 死锁修复确立）

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

### 8.7 Guard 安全规范（v1.0 死锁修复确立）

所有 Guard 必须满足以下两条规范：

1. **`locked_` 标志**：显式记录锁所有权状态
   - 避免 TSan 误报 "unlock of an unlocked mutex"（GCC 11 TSan 对 `pthread_mutex_timedlock` 内部状态追踪有 bug）
   - 在移动构造中正确处理：源 Guard 的 `locked_` 置 false，目标 Guard 接管所有权
   - 参考 v1.0 已实施代码：[runtime/builtin/mutex.h](file:///d:/you/Aura/runtime/builtin/mutex.h#L72) `Mutex::Guard::locked_` 字段

2. **`GcRootHandle<T*>`**：持锁期间 GC 对象可能被 compact 搬迁，Guard 内部必须持有 `GcRootHandle` 引用 m_，使 m_ 进入 GC roots
   - compact 时 `GcHeap::updateAllReferences` 会自动更新 m_ 指向新地址
   - 移动构造后必须调用 `gcRoot_.rebind(m_)` 重新绑定到新地址
   - 参考 v1.0 已实施代码：[runtime/builtin/mutex.h](file:///d:/you/Aura/runtime/builtin/mutex.h#L70) `Mutex::Guard::gcRoot_` 字段

---

## 九、若审核通过

1. 写入 TODO.txt §十 新增 P2 项：
   ```
   [ ] P2  sync 锁族 + lock 块语句（统一语法）
         - plan：plan/mutex_plan.md [2026-07-24 审核通过]
         - 方案：C（统一 lock 块语句，禁止跨函数持有锁）
         - 语法（统一）：
             lock (m) { ... }          # Mutex 独占（v1.0）
             lock (rw.r()) { ... }     # RWMutex 读（v1.1）
             lock (rw.w()) { ... }     # RWMutex 写（v1.1）
             lock (once) { ... }       # Once 一次性执行（v1.1）
             lock (wg) { ... }         # WaitGroup 登记（v1.1）
         - 分期：
             v1.0: sync.Mutex + LockStmt AST/Parser/Sema/CodeGen 全链路
             v1.1: RWMutex + Once + WaitGroup（复用 LockStmt，零 AST 改动）
         - 依赖：sync thread 实施完成 ✅
         - 文件：runtime/builtin/mutex.h(新增), mutex.cpp(新增),
                 src/AST/Stmt.h, src/Parser/StmtParser.cpp,
                 src/Sema/Checker/StmtChecker.cpp, src/Sema/BuiltinRegistry.h,
                 src/CodeGen/StmtGen.cpp, src/CodeGen/ExprGen.cpp
   ```

2. 按 §3.9 v1.0 实施步骤开始编码
3. v1.0 完成后，按 §4.7 v1.1 实施步骤续写
