# Issue：sync 锁族设计与 `lock` 块语句

> 来源：sync_thread_plan 实施时用户提出"锁和通道都需要"
> 类型：设计决策 issue
> 日期：2026-07-24
> 状态：草案（待审查）
> 关联：[sync_thread_plan.md](file:///d:/you/Aura/plan/sync_thread_plan.md)、[thread_pool_work_stealing_issue.md](file:///d:/you/Aura/plan/thread_pool_work_stealing_issue.md)

---

## 一、背景

sync thread 语句允许多个 spawn 任务并行执行。当任务通过指针共享 GC 对象（如 `Array<T>*`、`GcString*` 或用户自定义 record）并修改时，存在数据竞争。当前 sync_thread_plan 明确写着：

> ❌ `sync thread` 块内的 GC 暂停只在 L2/L3 safepoint 触发（非并发 GC）
> spawn 参数值传递；GC 对象通过指针共享时**用户需自行加锁**（v1 不提供语言级锁）

这是个矛盾：sync thread 提供了并行能力，却没提供任何加锁机制。本 issue 解决该缺口。

设计已与用户确认采用**双轨方案**：channel<T>（CSP，独立 plan）+ sync 锁族（本 issue）。

### 1.1 用户决策（2026-07-24 对话修正）

1. **采用方案 C（`lock` 块语句）** 而非方案 B（RAII Guard）
   - 核心理由：**块语句天然禁止跨函数持有锁**，消除一类误用
   - 块语句作用域明确，意图清晰
2. **锁不止一种**：v1 应规划锁族（Mutex / RWMutex / Once / WaitGroup），而非仅 Mutex
   - 本 issue 将锁族作为整体设计，分阶段实施
3. **统一 `lock` 语法，括号内表达式区分锁类型/模式**（2026-07-24 二次修正）
   - 用户原意示例：`lock (sync.rwmutex.r) { }`
   - 实施形式：`lock (m)` / `lock (rw.r())` / `lock (rw.w())`
   - 优势：一个 `lock` 关键字覆盖所有锁类型，模式由 `.r()`/`.w()` 表达，无需 rlock/wlock 软关键字
   - 扩展性：新增锁类型只需实现 `.r()`/`.w()` 方法，**零 AST/Parser 改动**
4. **Once/WaitGroup 也用 lock 语法**（2026-07-24 三次修正）
   - 用户要求：sync.Once、sync.WaitGroup 也要相同的 lock 逻辑，不用函数式 API
   - 设计：`lock (once) { }` → call_once 包裹；`lock (wg) { }` → add(1)/done() 包裹
   - 优势：所有 sync 原语**统一用 `lock` 块**，用户无需记忆不同 API
   - 实例化必要性：锁是共享状态对象（mutex 状态/once 标志/wg 计数器），必须构造实例供多 spawn 共享；这与 Go 的值类型零值可用不同，Aura 的 GC 对象需显式构造

---

## 二、设计目标

1. **线程安全**：保护临界区，防止数据竞争
2. **无忘记 unlock 风险**：块语句结束自动 unlock，RAII 保证
3. **禁止跨函数持有锁**：`lock (m) { }` 块不能跨函数，消除"锁泄漏到调用者"的误用
4. **与 Aura 体系一致**：与 GcRootHandle 的 RAII 模式一致
5. **spawn 友好**：能作为 spawn 参数传递（值传递语义，传指针）
6. **GC 兼容**：锁本身可作为 GC 对象，生命周期由 GC 管理
7. **覆盖常见并发模式**：互斥、读写分离、一次性初始化、等待组

---

## 三、锁族规划

### 3.1 锁种类清单

| 类型 | 用途 | 对应 C++ 原语 | v1 优先级 |
|:---|:---|:---|:---|
| **sync.Mutex** | 互斥锁（独占） | `std::mutex` | P0（必做） |
| **sync.RWMutex** | 读写锁（多读单写） | `std::shared_mutex`* | P1（重要） |
| **sync.Once** | 一次性执行 | `std::call_once` | P1（重要） |
| **sync.WaitGroup** | 等待一组任务完成 | `std::atomic<int>` + cv | P1（重要） |
| sync.Cond | 条件变量 | `std::condition_variable` | P3（远期） |
| sync.Semaphore | 信号量 | `std::counting_semaphore` | P3（远期） |

\* 注意：MinGW 下 `std::shared_mutex` 有 bug（msys2/MINGW-packages#25193），RWMutex 实现需规避（用 `std::mutex` + 读者计数模拟，或改用 `std::condition_variable` + 状态标志）。

**统一语法**：所有锁都用 `lock (lockExpr) { }` 块。模式由 lockExpr 表达式自描述：
- `lock (m) { }` — Mutex 独占
- `lock (rw.r()) { }` — RWMutex 读
- `lock (rw.w()) { }` — RWMutex 写
- `lock (once) { }` — 一次性执行（块体可能跳过）
- `lock (wg) { }` — WaitGroup 登记（add(1)/done() 包裹）

### 3.2 v1 范围与分期

- **v1.0（本 issue 范围）**：sync.Mutex + `lock` 块语句（AST/Parser/Sema/CodeGen 全链路）
- **v1.1**：sync.RWMutex / sync.Once / sync.WaitGroup（**复用 v1.0 的 LockStmt，零 AST/Parser 改动**，仅运行时 + BuiltinRegistry + CodeGen 分派）
- **远期**：sync.Cond / sync.Semaphore（同样复用 lock 块语法）

---

## 四、方案对比

### 方案 A：手动 lock/unlock（Go 风格）

```aura
let m = sync.Mutex()
m.lock()
// 临界区
m.unlock()
```

- ✅ 简单直接
- ❌ 忘记 unlock 会死锁
- ❌ Aura 无 defer，异常路径无法保证 unlock
- ❌ 允许跨函数持有锁（锁泄漏到调用者，易死锁）
- ❌ 与 Aura RAII 哲学不一致

### 方案 B：RAII Guard

```aura
let m = sync.Mutex()
let guard = m.lock()   // 构造时 lock
// 临界区
// guard 析构自动 unlock
```

- ✅ 无忘记 unlock 风险
- ✅ 异常安全
- ❌ **guard 可通过 move 跨函数持有锁**（违反"禁止跨函数持有"原则）
- ❌ guard 变量名"无用"（仅用于生命周期）
- ❌ guard 是隐藏临时类型，类型系统需处理

### 方案 C：`lock` 块语句（推荐，统一语法）

**核心设计**：统一用 `lock` 关键字，括号内是"锁表达式"，通过表达式本身区分锁类型与模式。

```aura
let m = sync.Mutex()
lock (m) {              // Mutex 独占
    // 临界区
}

let rw = sync.RWMutex()
lock (rw.r()) {        // RWMutex 读模式（多读并发）
    // 读临界区
}
lock (rw.w()) {        // RWMutex 写模式（独占）
    // 写临界区
}
```

**设计要点**：
- 括号内是任意表达式，求值结果为"锁视图"（LockView）
- `Mutex` 实例直接作为独占视图（隐式转换）
- `RWMutex` 通过 `.r()` / `.w()` 方法返回读/写视图
- **统一关键字**：不再需要 `rlock`/`wlock` 软关键字，所有锁都用 `lock`

- ✅ **作用域明确，块结束自动 unlock**
- ✅ **天然禁止跨函数持有锁**（块语句不能跨函数边界）
- ✅ **语法统一**：一个 `lock` 关键字覆盖所有锁类型，括号内表达式自描述
- ✅ **扩展性极佳**：新增锁类型只需新增 `.r()`/`.w()` 类方法，**零 AST/Parser 改动**
- ✅ 语法清晰，意图明显
- ✅ 异常安全（C++ 栈展开保证 unlock）
- ❌ 需新增 AST 节点 `LockStmt`
- ❌ 需 Parser/Sema/CodeGen 全链路改动
- ❌ 不能跨函数持有锁（但这正是安全特性，不是缺陷）

### 方案 D：B + C 双重提供

同时提供 guard 和块语句。

- ✅ 灵活，覆盖所有场景
- ❌ 实现成本最高
- ❌ 破坏"禁止跨函数持有锁"原则（guard 仍可 move）
- ❌ 过度设计

---

## 五、推荐方案：C（`lock` 块语句）

### 5.1 推荐理由

1. **安全特性优先**：禁止跨函数持有锁是**安全特性**而非缺陷。块语句强制锁的生命周期局限于词法作用域，消除"锁泄漏到调用者"导致死锁的整类 bug。
2. **意图清晰**：`lock (m) { ... }` 一眼可见临界区边界，比"guard 变量在何处析构"更直观。
3. **语法统一**：一个 `lock` 关键字覆盖所有锁类型。括号内表达式自描述锁类型与模式（`m` 独占 / `rw.r()` 读 / `rw.w()` 写），无需 `rlock`/`wlock` 等多个软关键字。
4. **扩展性极佳**：新增锁类型只需在运行时实现 `.r()`/`.w()` 等方法返回对应视图，**零 AST/Parser/Sema 改动**（LockStmt 不变，仅 lockExpr 求值结果不同）。
5. **与 Aura 哲学一致**：Aura 已有 `sync { }` / `sync thread { }` 块语句，`lock (m) { }` 风格统一。
6. **全链路改动可接受**：AST/Parser/Sema/CodeGen 改动量与 sync thread 相比小得多（无新运行时线程模型），约 ~200 行。

### 5.2 方案 C 的代价

- 需新增 `LockStmt` AST 节点（含锁表达式、块体）
- Parser 需识别 `lock` 软关键字（仅一个，不引入 rlock/wlock）
- Sema 需校验 `lockExpr` 求值结果为锁视图类型（LockView 接口）
- CodeGen 需生成 RAII wrapper（内部仍用 guard，但词法受限）

**关键洞察**：方案 C 在 CodeGen 层**仍生成 guard**，只是 guard 的生命周期被**词法约束**在块内，无法逃逸。这等价于"语法受限的方案 B"，既保留了 RAII 的异常安全，又获得了块语句的词法约束。

**统一语法的关键**：括号内的 `lockExpr` 是普通表达式，由 Sema 推断其类型。Mutex 实例 → 独占视图；RWMutex.r() → 读视图；RWMutex.w() → 写视图。未来新增锁类型（如 Sem）只需实现对应方法，语法层面零改动。

---

## 六、详细设计

### 6.1 类型设计

#### sync.Mutex（互斥锁）

```aura
let m = sync.Mutex()       // 构造
lock (m) { ... }            // 独占临界区
```

- `Mutex : GcObject`，内部封装 `std::mutex`
- 无 GC 指针字段（`std::mutex` 非 GC 对象）
- Mutex 实例直接作为锁视图（隐式 LockView），`lock (m) { }` 独占

#### sync.RWMutex（读写锁，v1.1）

```aura
let rw = sync.RWMutex()
lock (rw.r()) { ... }      // 读临界区（多读并发）
lock (rw.w()) { ... }      // 写临界区（独占）
```

- `RWMutex : GcObject`，内部用 `std::mutex` + 读者计数 + cv 模拟（规避 MinGW shared_mutex bug）
- `r()` / `w()` 方法返回读/写视图（LockView），由 `lock` 块消费
- **统一 `lock` 关键字**：不再有 `rlock`/`wlock`，模式由 `.r()`/`.w()` 表达

#### sync.Once（一次性执行，v1.1）

```aura
let once = sync.Once()
lock (once) {              // 首次进入执行块体；后续 lock(once) 跳过整个块
    // 初始化代码
}
```

- `Once : GcObject`，内部 `std::once_flag` + `std::call_once`
- **lock 语义**：`lock (once) { }` 等价于"首次执行块体，后续跳过"
- acquire 语义特殊：首次 acquire 成功并执行块体；已执行后 acquire 返回"空 guard"，块体**不执行**
- 这统一了语法：所有 sync 原语都用 `lock (...) { }`，不再需要 `once.do_(fn)` 函数式 API

#### sync.WaitGroup（等待组，v1.1）

```aura
let wg = sync.WaitGroup()
sync thread(max = 4) {
    for i in range(100) {
        spawn (wg: sync.WaitGroup) {
            lock (wg) {       // 进入：wg.add(1)
                // 任务体
            }                  // 离开：wg.done()
        }
    }
}
wg.wait()                     // 等待所有 lock(wg) 块完成
```

- `WaitGroup : GcObject`，内部 `std::atomic<int>` + `std::condition_variable`
- **lock 语义**：`lock (wg) { }` = add(1) 进入 + done() 离开的语法糖
- `wg.wait()` 仍是普通方法调用（在 lock 块外等待，非持锁）
- `wg.add(n)` 仅在批量预添加时用（罕见），常规场景 `lock (wg) { }` 自动 add(1)/done()

**统一设计要点**：
- 所有 sync 原语（Mutex / RWMutex / Once / WaitGroup）都用 `lock (lockExpr) { }` 块语法
- lockExpr 求值结果的类型决定 lock 语义（独占 / 读 / 写 / 一次 / 等待组登记）
- 用户**无需记忆不同 API**（`m.lock()` / `once.do_()` / `wg.add/done()`），统一用 lock 块
- 函数式 API（`do_`/`add`/`done`）不再暴露给用户，仅内部实现

### 6.2 AST 设计

**新增节点**：`LockStmt`（统一，无 Mode 枚举——模式由 lockExpr 表达式表达）

```cpp
struct LockStmt : Stmt {
    std::unique_ptr<ASTNode> lockExpr;   // 锁表达式，求值为 LockView
    std::unique_ptr<BlockStmt> body;
    // line/col 继承自 Stmt
};
```

**设计要点**：
- `lockExpr` 是任意表达式（支持 `m` / `rw.r()` / `obj.mutex` / `get_lock()`），求值后须为 LockView
- `body` 是块语句，作用域独立
- **无 Mode 字段**：模式（独占/读/写）由 lockExpr 求值结果的类型决定
  - `lock (m) { }`        → lockExpr 求值为 Mutex*，独占
  - `lock (rw.r()) { }`   → lockExpr 求值为 ReadView，读模式
  - `lock (rw.w()) { }`   → lockExpr 求值为 WriteView，写模式

### 6.3 Parser

**软关键字**：仅 `lock`（不再有 rlock/wlock），仍按普通标识符解析，在语句起始位置上下文识别。

```cpp
std::unique_ptr<ASTNode> Parser::parseLockStmt() {
    auto tok = consume();  // 'lock'
    auto* stmt = new LockStmt();
    stmt->line = tok.line;
    stmt->col = tok.col;

    expect(TokType::LParen);
    stmt->lockExpr = parseExpr();   // 任意表达式：m / rw.r() / get_lock() ...
    expect(TokType::RParen);

    stmt->body = parseBlockStmt();
    return std::unique_ptr<ASTNode>(stmt);
}
```

**触发条件**：语句起始位置遇到 `lock` 标识符 + 后续 `(` → 解析为 LockStmt。否则按普通标识符处理（如 `let lock = ...`）。

**优势**：模式（读/写）由括号内表达式的语义决定（`.r()`/`.w()` 方法调用），Parser 层完全不感知锁类型，新增锁类型无需改 Parser。

### 6.4 Sema 检查

**新增规则**：

| 规则 | 说明 | 错误信息 |
|:---|:---|:---|
| **L1** | `lockExpr` 求值结果必须是锁类型：`Mutex*` / `RWMutex.r()` / `RWMutex.w()` / `Once*` / `WaitGroup*` | "lock requires sync.Mutex/RWMutex.r()/.w()/Once/WaitGroup" |
| **L3** | `lock` 块内禁止 `return`/`break`/`continue` 跨出块 | "cannot return/break/continue out of lock block" |
| **L4** | `lock` 块内禁止 `await` | "cannot await inside lock block"（会阻塞其他线程） |
| **L5** | 禁止嵌套同一把锁（死锁检测，best-effort，仅对 Mutex/RWMutex） | "nested lock on same mutex may deadlock"（警告） |
| **L6** | `lock` 块内禁止 `spawn`（spawn 不应持锁，WaitGroup 的 spawn 应在 lock 块外提交） | "cannot spawn inside lock block" |
| **L7** | `lock (wg) { }` 内禁止再嵌套 `lock (wg) { }`（同一 WaitGroup） | "nested lock on same WaitGroup" |

**L1 实现要点**：
- Mutex* / Once* / WaitGroup* 直接作为合法 lock 目标
- RWMutex.r() / RWMutex.w() 的返回类型由 BuiltinRegistry 注册（RWMutexReadView / RWMutexWriteView）
- 其他类型（int / string / Array 等）报错

**L6 说明**：WaitGroup 的典型用法是 `spawn { lock(wg) { ... } }`，spawn 在 lock 块**外**，lock 块在 spawn 体内。禁止的是"lock 块内再 spawn"，避免持锁时创建任务。

**R3 实现**：通过作用域栈标记 `inLockBlock_`，在 `checkReturnStmt`/`checkBreakStmt`/`checkContinueStmt` 中检查。

### 6.5 CodeGen

**生成策略**：根据 lockExpr 类型分派不同生成模式。Mutex/RWMutex 走 RAII guard；Once 走 call_once；WaitGroup 走 add/done 包裹。

```cpp
void CodeGenerator::genLockStmt(std::ostream& cpp, const LockStmt& stmt,
                                  bool isCoroutine) {
    // isCoroutine 必须为 false（L4 已禁 await，块内非协程）
    // 由 Sema 推断 lockExpr 的类型，CodeGen 据此分派：
    //   Mutex*        → RAII guard
    //   RWMutex.r()   → ReadGuard
    //   RWMutex.w()   → WriteGuard
    //   Once*         → call_once 包裹（块体可能不执行）
    //   WaitGroup*    → add(1)/done() 包裹
    auto lockType = semaTypeOf(*stmt.lockExpr);  // Sema 已推断
    std::string lockExpr = genExpr(*stmt.lockExpr, false);

    if (lockType == "Once") {
        // lock (once) { body } → once->do_([&]{ body })
        writeLine(cpp, lockExpr + "->do_([&] {");
        indentLevel_++;
        if (stmt.body) genBlock(cpp, *stmt.body, false);
        indentLevel_--;
        writeLine(cpp, "});");
        return;
    }
    if (lockType == "WaitGroup") {
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
    // 默认：Mutex / RWMutex.r() / RWMutex.w() —— RAII guard
    cpp << indentStr() << "{\n";
    indentLevel_++;
    writeLine(cpp, "auto _guard = aura_rt::__acquire_lock(" + lockExpr + ");");
    if (stmt.body) genBlock(cpp, *stmt.body, false);
    indentLevel_--;
    cpp << indentStr() << "}\n";
}
```

**关键差异**：
- **Mutex/RWMutex**：guard 构造时 acquire，析构时 release，块体**总是执行**
- **Once**：块体包在 `do_([&]{ ... })` 内，由 `call_once` 决定是否执行（**可能跳过**）
- **WaitGroup**：块体**总是执行**，前后包裹 add(1)/done()

**`__finally` 辅助**（用于 WaitGroup 的 done() 包裹）：

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

**运行时统一 acquire 入口**（Mutex / RWMutex.r() / RWMutex.w()）：

```cpp
// runtime/builtin/mutex.h
struct Mutex : GcObject {
    std::mutex m_;
    class Guard {
    public:
        explicit Guard(Mutex* m) : m_(m) { m_->m_.lock(); }
        ~Guard() { if (m_) m_->m_.unlock(); }
        Guard(Guard&&) noexcept;
        Guard(const Guard&) = delete;
    private:
        Mutex* m_;
    };
    Guard acquire() { return Guard(this); }
};
inline Mutex::Guard __acquire_lock(Mutex* m) { return m->acquire(); }
// RWMutex.r()/.w() 的重载略（见 6.6）
```

**关键**：guard 名为 `_guard`（预留名），用户不可见、不可引用。Sema 推断 lockExpr 类型后，CodeGen 通过类型分派选择正确的生成策略。

### 6.6 运行时实现

**新增文件**：`runtime/builtin/mutex.h`

**关键设计：间接持有不可移动原语**

`std::mutex` / `std::condition_variable` / `std::once_flag` 不可移动（删除了移动构造）。但 GcObject 在 compact GC 时会被 `memcpy` 搬迁到新页——这会破坏内嵌同步原语的内部状态（持锁状态丢失、临界区数据结构损坏），是当前设计的一个潜在 bug。

**解决方案**：Mutex/RWMutex/Once/WaitGroup 内部用**裸指针**指向 `new` 分配的同步原语。GC compact 时 GcObject 主体搬迁，但指针指向的堆原语不动。原语的生命周期与 GC 对象一致，但**需要终结器释放**（见 §6.9）。

```cpp
#pragma once
#include "../gc.h"
#include <atomic>
#include <condition_variable>
#include <mutex>

namespace aura_rt {

// ============================================================
// Mutex — 互斥锁（GC 堆对象）
//
// 用法：lock (m) { ... }
// 关键：std::mutex 不可移动，GcObject compact 时会被 memcpy 搬迁，
//      直接内嵌会损坏。用间接指针规避：
//      Mutex 主体搬迁 → m_ 指针的值被正确拷贝 → 指向同一块堆 mutex
// ============================================================
struct Mutex : GcObject {
    std::mutex* m_;  // 指向 new 出的 mutex（非 GC 对象，独立堆分配）

    static const TypeDescriptor _desc;

    // 构造由工厂函数完成：m_ = new std::mutex()
    // 析构由终结器完成：delete m_（见 §6.9）

    class Guard {
    public:
        explicit Guard(Mutex* m) : m_(m) { m_->m_->lock(); }
        ~Guard() { if (m_) m_->m_->unlock(); }
        Guard(Guard&& o) noexcept : m_(o.m_) { o.m_ = nullptr; }
        Guard(const Guard&) = delete;
    private:
        Mutex* m_;
    };

    Guard acquire() { return Guard(this); }
};

inline Mutex::Guard __acquire_lock(Mutex* m) { return m->acquire(); }

// ============================================================
// RWMutex — 读写锁（GC 堆对象，v1.1）
//
// 用法：lock (rw.r()) { ... } / lock (rw.w()) { ... }
// 同样用间接指针规避 compact memcpy 问题
// ============================================================
struct RWMutex : GcObject {
    struct Inner {
        std::mutex m;
        std::condition_variable cv;
        std::atomic<int> readers{0};
        bool writer_active = false;
    };
    Inner* inner_;  // 指向 new 出的 Inner

    static const TypeDescriptor _desc;

    class ReadGuard {
    public:
        explicit ReadGuard(RWMutex* rw);
        ~ReadGuard();
        ReadGuard(ReadGuard&&) noexcept;
    private:
        RWMutex* rw_;
    };

    class WriteGuard {
    public:
        explicit WriteGuard(RWMutex* rw);
        ~WriteGuard();
        WriteGuard(WriteGuard&&) noexcept;
    private:
        RWMutex* rw_;
    };

    // r()/w() 返回锁视图（临时对象，由 __acquire_lock 消费）
    ReadGuard r() { return ReadGuard(this); }
    WriteGuard w() { return WriteGuard(this); }
};

inline RWMutex::ReadGuard __acquire_lock(RWMutex::ReadGuard&& v) {
    return std::move(v);  // 已在 r() 构造时 acquire
}

inline RWMutex::WriteGuard __acquire_lock(RWMutex::WriteGuard&& v) {
    return std::move(v);  // 已在 w() 构造时 acquire
}

} // namespace aura_rt
```

**注意**：ReadGuard/WriteGuard 的 acquire 在 `r()`/`w()` 返回时即完成（构造函数内 lock），`__acquire_lock` 仅做 move 转发，将临时视图绑定到 `_guard` 变量。这样统一了"Mutex 直接用"和"RWMutex 通过 .r()/.w() 用"两种形式。

#### sync.Once 运行时（v1.1）

```cpp
// runtime/builtin/mutex.h（续）
struct Once : GcObject {
    std::once_flag* flag_;  // 间接指针（once_flag 不可移动）

    static const TypeDescriptor _desc;

    template <typename F>
    void do_(F&& f) {
        std::call_once(*flag_, std::forward<F>(f));
    }
};
```

- 间接持有 once_flag，规避 compact memcpy 问题

#### sync.WaitGroup 运行时（v1.1）

```cpp
// runtime/builtin/mutex.h（续）
struct WaitGroup : GcObject {
    struct Inner {
        std::atomic<int> count{0};
        std::mutex m;
        std::condition_variable cv;
    };
    Inner* inner_;  // 间接指针（mutex/cv 不可移动）

    static const TypeDescriptor _desc;

    void add(int n = 1) { inner_->count.fetch_add(n, std::memory_order_acq_rel); }

    void done() {
        if (inner_->count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            std::lock_guard<std::mutex> lk(inner_->m);
            inner_->cv.notify_all();
        }
    }

    void wait() {
        std::unique_lock<std::mutex> lk(inner_->m);
        inner_->cv.wait(lk, [&]{ return inner_->count.load(std::memory_order_acquire) == 0; });
    }
};
```

- 间接持有 mutex + cv，规避 compact memcpy 问题
- `lock (wg) { }` 由 CodeGen 生成 `add(1)` + `__finally(done)` 包裹
- `wg.wait()` 是普通方法调用（在 lock 块外）

#### 工厂函数（分配 GC 对象 + new 不可移动原语）

```cpp
inline Mutex* make_mutex() {
    auto* m = static_cast<Mutex*>(
        GcHeap::instance().alloc(sizeof(Mutex), &Mutex::_desc));
    m->m_ = new std::mutex();          // 独立堆分配
    return m;
}
inline RWMutex* make_rwmutex() {
    auto* rw = static_cast<RWMutex*>(
        GcHeap::instance().alloc(sizeof(RWMutex), &RWMutex::_desc));
    rw->inner_ = new RWMutex::Inner();
    return rw;
}
inline Once* make_once() {
    auto* o = static_cast<Once*>(
        GcHeap::instance().alloc(sizeof(Once), &Once::_desc));
    o->flag_ = new std::once_flag();
    return o;
}
inline WaitGroup* make_waitgroup() {
    auto* wg = static_cast<WaitGroup*>(
        GcHeap::instance().alloc(sizeof(WaitGroup), &WaitGroup::_desc));
    wg->inner_ = new WaitGroup::Inner();
    return wg;
}
```

#### TypeDescriptor（无 GC 指针字段）

**关键**：`m_` / `inner_` / `flag_` 是裸指针，指向**非 GC 对象**（独立 new 的堆内存）。GC **不应追踪**这些指针（ptrFieldCount=0）。原语释放由终结器完成（见 §6.9）。

```cpp
const TypeDescriptor Mutex::_desc     = { sizeof(Mutex),     0, nullptr };
const TypeDescriptor RWMutex::_desc   = { sizeof(RWMutex),   0, nullptr };
const TypeDescriptor Once::_desc     = { sizeof(Once),      0, nullptr };
const TypeDescriptor WaitGroup::_desc = { sizeof(WaitGroup), 0, nullptr };
```

**新增文件**：`runtime/builtin/mutex.cpp`

```cpp
#include "mutex.h"

namespace aura_rt {

// TypeDescriptor（已在 §6.6 定义，此处仅声明引用）
// GC 不追踪 m_/inner_/flag_ 裸指针（非 GC 对象）

// ============================================================
// 终结器：GC 回收锁对象时释放间接持有的同步原语
// ============================================================
// Aura GC 已支持终结器（见 project_memory：Finalizers are needed
// to properly close resource handles during GC）。锁族注册终结器：
//   Mutex::_desc.finalizer     = [](GcObject* o){ delete static_cast<Mutex*>(o)->m_; }
//   RWMutex::_desc.finalizer   = [](GcObject* o){ delete static_cast<RWMutex*>(o)->inner_; }
//   Once::_desc.finalizer      = [](GcObject* o){ delete static_cast<Once*>(o)->flag_; }
//   WaitGroup::_desc.finalizer = [](GcObject* o){ delete static_cast<WaitGroup*>(o)->inner_; }
//
// 注意：finalizer 在 GC STW 期间执行，此时其他线程已暂停，
//      可安全 delete 同步原语（无并发访问）。

} // namespace aura_rt
```

### 6.7 终结器注册（关键：避免同步原语泄漏）

锁族对象通过间接指针持有 `new` 出的同步原语（mutex/cv/once_flag）。这些原语**不是 GC 对象**，GC 不会追踪或回收它们。需要注册终结器，在 GC 回收锁对象时 `delete` 间接原语，避免内存泄漏。

**修改**：`runtime/builtin/mutex.cpp` 中 TypeDescriptor 定义

```cpp
// 终结器函数类型：void(*)(GcObject*)
static void mutex_finalizer(GcObject* o) {
    delete static_cast<Mutex*>(o)->m_;
    static_cast<Mutex*>(o)->m_ = nullptr;
}
static void rwmutex_finalizer(GcObject* o) {
    delete static_cast<RWMutex*>(o)->inner_;
    static_cast<RWMutex*>(o)->inner_ = nullptr;
}
static void once_finalizer(GcObject* o) {
    delete static_cast<Once*>(o)->flag_;
    static_cast<Once*>(o)->flag_ = nullptr;
}
static void waitgroup_finalizer(GcObject* o) {
    delete static_cast<WaitGroup*>(o)->inner_;
    static_cast<WaitGroup*>(o)->inner_ = nullptr;
}

const TypeDescriptor Mutex::_desc = {
    sizeof(Mutex), 0, nullptr, mutex_finalizer
};
const TypeDescriptor RWMutex::_desc = {
    sizeof(RWMutex), 0, nullptr, rwmutex_finalizer
};
const TypeDescriptor Once::_desc = {
    sizeof(Once), 0, nullptr, once_finalizer
};
const TypeDescriptor WaitGroup::_desc = {
    sizeof(WaitGroup), 0, nullptr, waitgroup_finalizer
};
```

**前置依赖**：Aura GC 已支持终结器（见 project_memory: "Finalizers are needed to properly close resource handles during GC"）。本 issue 直接复用，无需新增 GC 机制。

**安全性**：
- finalizer 在 GC STW 期间执行，所有 mutator 线程已暂停，`delete` 同步原语时无并发访问
- 不在 finalizer 中获取锁（避免 STW 期间死锁），仅 `delete` 堆内存

### 6.8 BuiltinRegistry 注册

**修改**：`src/Sema/BuiltinRegistry.h` `init()`

```cpp
// types_ 新增：
{"Mutex",     {"Mutex",     true, true, BuiltinPrim::Other, "aura_rt::Mutex*"}},
{"RWMutex",   {"RWMutex",   true, true, BuiltinPrim::Other, "aura_rt::RWMutex*"}},  // v1.1
{"Once",      {"Once",      true, true, BuiltinPrim::Other, "aura_rt::Once*"}},     // v1.1
{"WaitGroup", {"WaitGroup", true, true, BuiltinPrim::Other, "aura_rt::WaitGroup*"}}, // v1.1

// functions_ 新增（构造）：
{"sync.Mutex",     {}, ReturnTypeInfo::Named("Mutex")},
{"sync.RWMutex",   {}, ReturnTypeInfo::Named("RWMutex")},     // v1.1
{"sync.Once",      {}, ReturnTypeInfo::Named("Once")},        // v1.1
{"sync.WaitGroup", {}, ReturnTypeInfo::Named("WaitGroup")},   // v1.1

// methods_ 新增：
// RWMutex 的 r()/w() 返回锁视图（v1.1）
{"RWMutex", "r", {}, ReturnTypeInfo::Named("RWMutexReadView")},   // Sema 层虚拟类型
{"RWMutex", "w", {}, ReturnTypeInfo::Named("RWMutexWriteView")},
// WaitGroup 的 wait() 是普通方法（在 lock 块外调用，非持锁）（v1.1）
{"WaitGroup", "wait", {}, ReturnTypeInfo::None()},
// WaitGroup 的 add(n) 仅批量预添加用（罕见，lock(wg) 自动 add(1)/done()）（v1.1）
{"WaitGroup", "add", {"int"}, ReturnTypeInfo::None()},
```

**注意**：
- `Mutex` 不注册 `lock`/`unlock` 方法（必须用 `lock (m) { }` 块，强制安全用法）
- `RWMutex` 注册 `r()`/`w()`，但不注册 `lock`/`unlock`（模式必须通过 `.r()`/`.w()` 表达）
- `Once` 不注册任何方法（`lock (once) { }` 是唯一用法，内部 call_once）
- `WaitGroup` 注册 `wait()` 和 `add(n)`（`add` 仅罕见批量场景；常规 `lock (wg) { }` 自动 add(1)/done()，`done` 不暴露给用户）
- `RWMutexReadView` / `RWMutexWriteView` 是 Sema 层虚拟类型，仅用于类型检查

### 6.9 与 sync thread 集成

Mutex 作为堆对象，通过 `spawn` 参数值传递 `Mutex*` 指针。多个任务拿到同一个 `Mutex*`，通过 `lock (m) { }` 块互斥访问共享资源。

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

    io.println("count: " + counter.len())  // 输出 1000
}
```

**无额外改动**：sync thread 已支持 GC 对象指针传递，Mutex 自动适用。

---

## 七、实施影响评估

### 7.1 改动规模

| 模块 | 文件 | 改动 |
|:---|:---|:---|
| 运行时 v1.0 | `runtime/builtin/mutex.h`（新增）, `mutex.cpp`（新增） | Mutex + 间接指针 + 终结器 ~100 行 |
| 运行时 v1.1 | 同上（续） | RWMutex + Once + WaitGroup + 终结器 ~180 行 |
| 运行时 | `runtime/aura_rt.h`, `runtime/CMakeLists.txt` | include + 源文件 |
| AST | `src/AST/Stmt.h`, `src/AST/Stmt.cpp` | 新增 LockStmt 节点 ~30 行（仅 v1.0） |
| Parser | `src/Parser/StmtParser.cpp` | parseLockStmt ~25 行（仅 v1.0） |
| Sema | `src/Sema/Checker/StmtChecker.cpp` | checkLockStmt + L1-L7 规则 ~60 行（v1.0+v1.1） |
| Sema | `src/Sema/BuiltinRegistry.h` | 类型 + 构造 + r/w/wait/add 注册 ~15 行（v1.0+v1.1） |
| CodeGen | `src/CodeGen/StmtGen.cpp` | genLockStmt + 类型分派 ~50 行（v1.0+v1.1） |

**总改动**：v1.0 ~220 行，v1.1 增量 ~200 行（总计 ~420 行，但分期实施）

### 7.2 依赖关系

- **依赖**：sync thread 实施完成（需要多线程环境）✅ 已完成（2026-07-24）
- **依赖**：Aura GC 终结器机制（见 §6.7，已在 project_memory 中记录支持）✅
- **无依赖**：channel<T>（独立功能，可并行实施）

### 7.3 测试方案

**Mutex 测试**：

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

    io.println("count: " + counter.len())   // 输出 1000
}
```

**Once 测试**（v1.1）：

```aura
fun main(io: Io) {
    let once = sync.Once()

    sync thread(max = 4) {
        for i in range(100) {
            spawn (io: Io, once: sync.Once, i: int) {
                lock (once) {
                    io.println("init once, i=" + i)   // 仅输出一次
                }
            }
        }
    }

    io.println("done")
}
```

**WaitGroup 测试**（v1.1）：

```aura
fun main(io: Io) {
    let wg = sync.WaitGroup()
    let counter = [0]
    let m = sync.Mutex()

    sync thread(max = 4) {
        for i in range(1000) {
            spawn (wg: sync.WaitGroup, m: sync.Mutex, counter: [int], i: int) {
                lock (wg) {           // add(1) 进入
                    lock (m) {
                        counter.append(i)
                    }
                }                    // done() 离开
            }
        }
    }

    wg.wait()                         // 等待所有 spawn 完成
    io.println("count: " + counter.len())   // 输出 1000
}
```

**验收**：
- Mutex：输出 `count: 1000`（无丢失、无重复）
- Once：仅输出一行 `init once, i=N`（N 为首次进入的 i）
- WaitGroup：输出 `count: 1000`，`wg.wait()` 后所有 spawn 已完成
- 无 crash、无 ASAN 报错
- 多次运行结果一致

**Sema 检查测试**：

```aura
// L3: 禁止 return 跨出 lock 块
fun bad(m: sync.Mutex) {
    lock (m) {
        return  // 错误：cannot return out of lock block
    }
}

// L4: 禁止 await
fun bad2(m: sync.Mutex, io: Io) {
    lock (m) {
        io.println("hi")
        // await someAsync()  // 错误：cannot await inside lock block
    }
}

// L6: 禁止 spawn
fun bad3(m: sync.Mutex) {
    lock (m) {
        spawn (io: Io) { }  // 错误：cannot spawn inside lock block
    }
}

// L1: 类型错误
fun bad4() {
    lock (42) { }          // 错误：lock requires sync.Mutex/...
}
```

---

## 八、未来演进

### 8.1 sync.RWMutex（v1.1，已设计）

```aura
let rw = sync.RWMutex()
lock (rw.r()) { /* 读 */ }
lock (rw.w()) { /* 写 */ }
```

- 实现：`std::mutex` + `std::atomic<int> readers` + `std::condition_variable`（规避 MinGW shared_mutex bug）
- AST：复用 `LockStmt`，**零 AST/Parser 改动**
- 仅需 BuiltinRegistry 注册 `RWMutex.r()`/`w()` 方法

### 8.2 sync.Once / sync.WaitGroup（v1.1，已设计）

见 §6.1 / §6.6，均复用 `lock` 块语法：
- `lock (once) { }` → call_once 包裹（块体可能跳过）
- `lock (wg) { }` → add(1)/done() 包裹（块体总执行）

### 8.3 sync.Cond / sync.Semaphore（远期）

- sync.Cond：`lock (cond) { cond.wait(); ... }` 或 `lock (cond) { cond.notify(); }`
- sync.Semaphore：`lock (sem) { }` = acquire/release（与 Mutex 同构）

### 8.4 若用户后续需要跨函数持有锁

**不推荐**：跨函数持有锁是已知的死锁高发模式。

**替代方案**：将"持锁操作"封装为闭包，传入 `with_lock(m, fn() { ... })` 函数。语义等价于 lock 块，但可跨函数传递闭包。这仍保证锁在闭包执行完毕后释放，不会泄漏。

---

## 九、决策建议

**推荐采用方案 C（统一 `lock` 块语句）**，v1 仅实现 sync.Mutex：

1. **安全优先**：禁止跨函数持有锁是安全特性，消除一类死锁
2. **语法统一**：一个 `lock` 关键字覆盖所有锁类型，括号内表达式自描述（`m` / `rw.r()` / `rw.w()`）
3. **扩展性极佳**：未来 RWMutex / 新锁类型只需 BuiltinRegistry 注册 `.r()`/`.w()` 方法，**零 AST/Parser 改动**
4. **与 Aura 哲学一致**：与 `sync { }` / `sync thread { }` 块语句风格统一
5. **改动可控**：~220 行，远小于 sync thread 的 ~650 行

**不推荐方案 A（手动 lock/unlock）**：忘记 unlock 风险高，Aura 无 defer。
**不推荐方案 B（RAII Guard）**：guard 可 move 跨函数，违反"禁止跨函数持有"原则。
**不推荐方案 D（B+C 双重）**：破坏安全保证，过度设计。

---

## 十、锁族完整规划（参考）

| 类型 | 构造 | 用法 | v1 | 依赖 |
|:---|:---|:---|:---|:---|
| sync.Mutex | `sync.Mutex()` | `lock (m) { }` | ✅ v1.0 | sync thread |
| sync.RWMutex | `sync.RWMutex()` | `lock (rw.r()) { }` / `lock (rw.w()) { }` | ✅ v1.1 | sync.Mutex |
| sync.Once | `sync.Once()` | `lock (once) { }` | ✅ v1.1 | 无 |
| sync.WaitGroup | `sync.WaitGroup()` | `lock (wg) { }` + `wg.wait()` | ✅ v1.1 | sync thread |
| sync.Cond | `sync.Cond(m)` | `lock (cond) { cond.wait/notify }` | 远期 | sync.Mutex |
| sync.Semaphore | `sync.Semaphore(n)` | `lock (sem) { }` | 远期 | 无 |

**统一语法**：所有 sync 原语都用 `lock (lockExpr) { }` 块。lockExpr 类型决定 lock 语义（独占/读/写/一次/登记）。新增锁类型只需运行时实现对应语义，**零 AST/Parser 改动**（v1.0 的 LockStmt 设计已覆盖所有场景）。

**v1 范围**：v1.0 = sync.Mutex；v1.1 = RWMutex + Once + WaitGroup。均为 lock 块统一语法。

---

## 十一、细粒度锁模式

细粒度锁（fine-grained locking）指"锁与被保护的数据关联，而非全局一把锁"，以提升并发度。Aura 的锁族天然支持三种细粒度模式：

### 11.1 模式 A：record 内嵌 Mutex 字段（per-instance lock）

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
- Mutex 作为 record 字段：record 是 GC 对象，compact 时会搬迁，但 §6.6 的间接指针设计保证 mutex 状态不损坏
- **这就是 §6.6 间接指针设计的核心收益**：record 内嵌的 Mutex 在 compact 后仍指向同一块堆 mutex

### 11.2 模式 B：分段锁（striped lock）

预先分配 N 把锁，按 key 哈希选锁。适用于共享 map / 缓存的并发访问。

```aura
fun main(io: Io) {
    // 16 把锁的分段锁池
    let stripes = [
        sync.Mutex(), sync.Mutex(), sync.Mutex(), sync.Mutex(),
        sync.Mutex(), sync.Mutex(), sync.Mutex(), sync.Mutex(),
        sync.Mutex(), sync.Mutex(), sync.Mutex(), sync.Mutex(),
        sync.Mutex(), sync.Mutex(), sync.Mutex(), sync.Mutex(),
    ]
    let cache: [string]int = [:]   // 假设有 map 类型
    let cacheMu = sync.Mutex()      // 保护 cache 自身（map 不是并发安全）

    sync thread(max = 8) {
        for i in range(1000) {
            spawn (stripes: [sync.Mutex], cache: [string]int, i: int) {
                let key = "k" + (i % 16)
                let s = stripes[i % 16]
                lock (s) {
                    // 细粒度：只持有一段锁，其他段可并发
                    cache.set(key, cache.get(key, 0) + 1)
                }
            }
        }
    }
}
```

- 16 把锁 → 理论并发度 16
- 无需语言级支持，纯库模式（Array<Mutex> + hash 选锁）
- Aura 的 `Array<T>` 已支持 GC 对象元素，Mutex 可直接放入数组

### 11.3 模式 C：per-object 锁（语言级，未来扩展）

类似 Java 的 `synchronized(obj)`，每个对象可选挂载一把锁（lazy attach）。**v1 不实施**，作为未来扩展。

```aura
fun transfer(a: Account, b: Account, amt: int) {
    lock (a) {              // 直接 lock 对象，而非 lock (a.mu)
        lock (b) {
            a.balance -= amt
            b.balance += amt
        }
    }
}
```

- 优势：语法更简洁，无需显式声明 mu 字段
- 劣势：
  - 每个对象需要 lazy attach 锁（额外内存）
  - GcObject 头部需扩展锁指针（8 字节开销，破坏 32 字节头部约束）
  - 实现复杂，v1 不提供
- **替代方案**：v1 用模式 A（record 内嵌 mu）达到同等效果，仅语法稍冗长

### 11.4 死锁预防：多锁排序

细粒度锁的常见陷阱是"多锁顺序不一致导致死锁"。Aura v1 **不做语言级死锁检测**（仅 L5 best-effort 警告），由用户负责。

**推荐实践**：多锁场景按地址或 id 排序后加锁：

```aura
fun transfer(a: Account, b: Account, amt: int) {
    // 按地址排序，避免 A→B 和 B→A 两个方向同时加锁
    let (first, second) = if (&a < &b) { (a, b) } else { (b, a) }
    lock (first.mu) {
        lock (second.mu) {
            first.balance -= amt
            second.balance += amt
        }
    }
}
```

**未来扩展**（v2+）：可引入 `lock (a.mu, b.mu) { }` 多锁语句，由 CodeGen 自动排序后加锁，消除人为错误。本 issue 不实施。

---

## 十二、若审核通过

1. 写入 TODO.txt §十 新增 P2 项：
   ```
   [ ] P2  sync 锁族 + lock 块语句（统一语法）
         - issue：plan/mutex_issue.md [2026-07-24 审核通过]
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
                 src/CodeGen/StmtGen.cpp
   ```

2. 创建 `plan/mutex_plan.md` 详细实施方案（含 AST/Parser/Sema/CodeGen 全链路细节）
3. 启动 Mutex plan 实施（v1.0 先行，v1.1 紧随）
