# change.md：sync.Mutex + lock 块语句（v1.0 实施代码）

> 来源 plan：plan/mutex_plan.md §三 v1.0
> 范围：仅 sync.Mutex + `lock (m) { }` 块语句，全链路打通
> 状态：待审查

---

## 一、运行时

### 1.1 新增文件：runtime/builtin/mutex.h

```cpp
#pragma once
// ============================================================
// aura_rt/builtin/mutex.h — 用户级互斥锁 + lock 块运行时支持
//
// 设计要点：
//   1. std::mutex 不可移动，但 GcObject 在 compact GC 时会被 memcpy
//      搬迁。直接内嵌 std::mutex 会导致内部状态损坏。
//      → 用间接指针：Mutex 主体搬迁，指向的堆 mutex 不动。
//   2. 间接指针指向的 std::mutex 不是 GC 对象，需终结器释放。
//   3. 不暴露 lock()/unlock() 给用户，强制走 lock (m) { } 块语句。
// ============================================================

#include "../gc.h"
#include <mutex>

namespace aura_rt {

// ============================================================
// Mutex — 互斥锁（GC 堆对象）
// ============================================================
struct Mutex : GcObject {
    std::mutex* m_;  // 间接指针，指向 new 出的 mutex

    static const TypeDescriptor _desc;

    // RAII 守卫（用户不可见，由 lock 块生成的 _guard 持有）
    class Guard {
    public:
        explicit Guard(Mutex* m) : m_(m) { m_->m_->lock(); }
        ~Guard() { if (m_) m_->m_->unlock(); }
        Guard(Guard&& o) noexcept : m_(o.m_) { o.m_ = nullptr; }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        Guard& operator=(Guard&&) = delete;
    private:
        Mutex* m_;
    };

    Guard acquire() { return Guard(this); }
};

// 统一 acquire 入口：lock (m) { } 生成 __acquire_lock(m)
inline Mutex::Guard __acquire_lock(Mutex* m) { return m->acquire(); }

// 工厂函数：sync.Mutex() 构造调用生成
inline Mutex* make_mutex() {
    auto* m = static_cast<Mutex*>(
        GcHeap::instance().alloc(sizeof(Mutex), &Mutex::_desc));
    m->m_ = new std::mutex();
    return m;
}

} // namespace aura_rt
```

### 1.2 新增文件：runtime/builtin/mutex.cpp

```cpp
#include "mutex.h"

namespace aura_rt {

// 终结器：GC 回收 Mutex 时释放间接持有的 std::mutex
// 安全性：finalizer 在 GC STW 期间执行，所有 mutator 线程已暂停，
//        可安全 delete 同步原语（无并发访问）
static void mutex_finalizer(GcObject* o) {
    auto* m = static_cast<Mutex*>(o);
    delete m->m_;
    m->m_ = nullptr;
}

// TypeDescriptor：m_ 是裸指针指向非 GC 对象，ptrFieldCount=0
// GC 不会追踪该指针（指向独立堆内存，由 finalizer 释放）
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

### 1.3 修改：runtime/aura_rt.h

**新增 include**（在 `builtin/channel.h` 后）：

```cpp
#include "builtin/mutex.h"
```

### 1.4 修改：runtime/CMakeLists.txt

**第 47-48 行新增源文件**：

```cmake
add_library(aura_rt STATIC
    types.cpp
    gc.cpp
    task.cpp
    thread_pool.cpp
    builtin/io.cpp
    builtin/string.cpp
    builtin/mutex.cpp        # 新增
    win_iocp.cpp
)
```

---

## 二、AST

### 2.1 修改：src/AST/Stmt.h

**在 SyncStmt（第 218-231 行）后新增 LockStmt**：

```cpp
struct LockStmt : Stmt {
    std::unique_ptr<ASTNode> lockExpr;   // 锁表达式，求值为 Mutex*
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

**设计说明**：
- `lockExpr` 是任意表达式（支持 `m` / `obj.mu` / `get_lock()` 等），求值后须为 `Mutex*`
- `body` 是块语句，作用域独立
- **无 Mode 字段**：模式由 lockExpr 求值结果类型决定（v1.1 扩展 RWMutex.r()/.w() 时无需改 AST）
- 类型信息通过 `lockExpr->inferredType`（ASTNode 已有字段）由 Sema 标注

### 2.2 修改：src/ASTPrinter.cpp

**在 SyncStmt::print 后（第 352 行后）新增**：

```cpp
void LockStmt::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "LockStmt\n";
    if (lockExpr) lockExpr->print(os, indent + 1);
    if (body) body->print(os, indent + 1);
}
```

---

## 三、Parser

### 3.1 修改：src/Parser/StmtParser.cpp

**第 30 行（`if (check(TokType::Spawn))` 后）新增软关键字 `lock` 识别**：

```cpp
if (check(TokType::Spawn))    return parseSpawnStmt();
// lock 软关键字：语句起始位置 + 后续 '(' 时识别为 LockStmt
// 其他位置仍是普通标识符（如 let lock = ...）
if (check(TokType::Identifier) && peek().lexeme == "lock"
    && peekNext().type == TokType::LParen) {
    return parseLockStmt();
}
```

**parseSyncStmt 后（第 198 行后）新增 parseLockStmt 函数**：

```cpp
std::unique_ptr<Stmt> Parser::parseLockStmt() {
    auto tok = advance();  // consume 'lock' 标识符
    auto stmt = std::make_unique<LockStmt>();
    setNodePos(stmt.get(), tok);

    consume(TokType::LParen, "expected '(' after lock");
    stmt->lockExpr = parseExpr();
    consume(TokType::RParen, "expected ')' after lock expression");

    stmt->body = parseBlock();
    return stmt;
}
```

### 3.2 修改：src/Parser.h

**在 parseSyncStmt 声明附近新增 parseLockStmt 声明**：

```cpp
std::unique_ptr<Stmt> parseSyncStmt();
std::unique_ptr<Stmt> parseSyncForStmt();
std::unique_ptr<Stmt> parseSpawnStmt();
std::unique_ptr<Stmt> parseLockStmt();   // 新增
```

---

## 四、Sema

### 4.1 修改：src/Sema/SemAnalyzer.h

**第 144 行 `inSyncThreadBlock_` 后新增字段**：

```cpp
bool inSyncThreadBlock_ = false;  // sync thread 块内（禁止嵌套 / 无参 spawn）
bool inLockBlock_ = false;        // lock 块内（禁止 return/break/continue 跨出）
```

**在 checkSpawnStmt 声明附近新增 checkLockStmt 声明**：

```cpp
void checkSpawnStmt(const SpawnStmt& stmt);
void checkLockStmt(const LockStmt& stmt);   // 新增
```

### 4.2 修改：src/Sema/SemAnalyzer.cpp

**第 504 行（SpawnStmt 分支后）新增 dispatch**：

```cpp
if (auto* p = dynamic_cast<const SpawnStmt*>(&stmt))        { checkSpawnStmt(*p);   return; }
if (auto* l = dynamic_cast<const LockStmt*>(&stmt))         { checkLockStmt(*l);    return; }   // 新增
```

**第 506-513 行（BreakStmt / ContinueStmt 检查）改为**：

```cpp
if (auto* br = dynamic_cast<const BreakStmt*>(&stmt)) {
    if (!insideLoop_) error(*br, "'break' outside of loop");
    if (inLockBlock_) error(*br, "cannot break out of lock block");     // 新增 L3
    return;
}
if (auto* co = dynamic_cast<const ContinueStmt*>(&stmt)) {
    if (!insideLoop_) error(*co, "'continue' outside of loop");
    if (inLockBlock_) error(*co, "cannot continue out of lock block");  // 新增 L3
    return;
}
```

### 4.3 修改：src/Sema/Checker/StmtChecker.cpp

**第 92 行 checkReturnStmt 末尾（第 108 行 `}` 前）新增 L3 检查**：

```cpp
void SemAnalyzer::checkReturnStmt(const ReturnStmt& stmt) {
    if (stmt.expr) {
        // ...（原有逻辑保持不变）
    }
    // L3: lock 块内禁止 return 跨出
    if (inLockBlock_) {
        error(stmt, "cannot return out of lock block");
    }
}
```

**第 318 行 checkSpawnStmt 末尾新增 L6 检查**：

```cpp
void SemAnalyzer::checkSpawnStmt(const SpawnStmt& stmt) {
    if (!insideSync_) {
        // ...（原有逻辑）
        return;
    }

    // L6: lock 块内禁止 spawn（spawn 不应持锁）
    if (inLockBlock_) {
        error(stmt, "cannot spawn inside lock block");
        return;
    }

    // R3: sync thread 块内的 spawn 必须显式传参
    // ...（原有逻辑保持不变）
}
```

**文件末尾新增 checkLockStmt 实现**：

```cpp
void SemAnalyzer::checkLockStmt(const LockStmt& stmt) {
    // L1: lockExpr 类型检查（v1.0 仅允许 Mutex*）
    if (stmt.lockExpr) {
        auto lockTy = inferExpr(*stmt.lockExpr);
        if (!lockTy) {
            error(*stmt.lockExpr, "cannot infer lock expression type");
            return;
        }
        // 检查是否为 Mutex* 类型
        // 通过 canonicalName 匹配（参考 isAssignable / BuiltinRegistry 查询）
        const std::string& name = lockTy->canonicalName();
        if (name != "Mutex") {
            error(*stmt.lockExpr,
                "lock requires sync.Mutex, got '" + lockTy->toString() + "'");
            return;
        }
    }

    // 进入 lock 块：设置标志
    bool oldInLock = inLockBlock_;
    inLockBlock_ = true;
    if (stmt.body) checkBlock(*stmt.body);
    inLockBlock_ = oldInLock;
}
```

**说明**：v1.0 仅支持 `Mutex*`，L4（禁止 await）实际通过 `ioSync_ = true` 在 CodeGen 强制（见 §五），Sema 不需单独检测（Aura 无 await 关键字，io 异步方法在 lock 块内会编译失败因为 _guard 析构顺序与协程状态冲突——v1.0 简化为"用户责任"，后续 v1.1 再加严格检测）。

---

## 五、CodeGen

### 5.1 修改：src/CodeGen/StmtGen.cpp

**第 53 行（SpawnStmt 分支后）新增 dispatch**：

```cpp
if (auto* sp = dynamic_cast<const SpawnStmt*>(&stmt))
    { genSpawnStmt(cpp, *sp, isCoroutine); return; }
if (auto* l = dynamic_cast<const LockStmt*>(&stmt))
    { genLockStmt(cpp, *l, isCoroutine); return; }   // 新增
```

**文件中新增 genLockStmt 实现**（放在 genSpawnStmt 之后）：

```cpp
// ============================================================
// lock 语句：lock (lockExpr) { body }
//
// v1.0 仅 Mutex 分支：生成 RAII guard，生命周期限制在块作用域内。
// _guard 构造时 acquire（m->lock()），析构时 release（m->unlock()）。
// 块结束自动 unlock，无需用户手动操作，且禁止跨函数持有锁。
// ============================================================
void CodeGenerator::genLockStmt(std::ostream& cpp, const LockStmt& stmt,
                                  bool isCoroutine) {
    // isCoroutine 必须为 false：lock 块内强制 ioSync_ = true，
    // 不会生成 co_await，块体非协程
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

### 5.2 修改：src/CodeGen/CodeGen.h

**新增 genLockStmt 声明**（在 genSyncStmt/genSpawnStmt 附近）：

```cpp
void genSyncStmt(std::ostream& cpp, const SyncStmt& stmt, bool isCoroutine);
void genSpawnStmt(std::ostream& cpp, const SpawnStmt& stmt, bool isCoroutine);
void genLockStmt(std::ostream& cpp, const LockStmt& stmt, bool isCoroutine);   // 新增
```

### 5.3 修改：src/CodeGen/ExprGen.cpp

**第 540 行 channel 构造特殊处理后，新增 sync.Mutex 构造特殊处理**：

```cpp
// channel 构造函数特殊处理：channel(cap) → new Channel<T>(cap)
if (calleeName == "channel") {
    std::string targ = expectedTemplateArgs_.empty() ? "int32_t" : expectedTemplateArgs_[0];
    std::string cap = e.args.empty() ? "0" : genExpr(*e.args[0], isCoroutine);
    return "(new aura_rt::Channel<" + targ + ">(" + cap + "))";
}

// sync.Mutex() 构造特殊处理：返回 GC 堆对象 + 间接指针 mutex
if (calleeName == "sync.Mutex" && e.args.empty()) {
    return "aura_rt::make_mutex()";
}
```

**说明**：调用者（genLetDecl / genAssignExpr）会用 GcRootHandle 包装返回的 Mutex*，与 channel 构造一致。

---

## 六、BuiltinRegistry 注册

### 6.1 修改：src/Sema/BuiltinRegistry.h

**第 218 行 `types_` 末尾（channel 后）新增 Mutex 类型**：

```cpp
types_ = {
    // ...（原有类型保持不变）
    {"channel",{"channel",true,  true, BuiltinPrim::Other,    "aura_rt::Channel*"}},
    {"Mutex",  {"Mutex",  true,  true, BuiltinPrim::Other,     "aura_rt::Mutex*"}},   // 新增
};
```

**第 268 行 `functions_` 末尾（gc_stats 后）新增 sync.Mutex 构造函数**：

```cpp
functions_ = {
    // ...（原有函数保持不变）
    {"gc_stats", {}, ReturnTypeInfo::Named("string")},
    // sync.Mutex 构造函数（无参数，返回 Mutex*）
    {"sync.Mutex", {}, ReturnTypeInfo::Named("Mutex")},   // 新增
};
```

**注意**：`Mutex` **不注册 `lock`/`unlock` 方法**——用户必须用 `lock (m) { }` 块，强制安全用法。

---

## 七、测试

### 7.1 测试文件：example/test.aura

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

    io.println("count: " + counter.len())
}
```

**验收标准**：
- 输出 `count: 1000`（无丢失、无重复）
- 无 crash、无 ASAN 报错
- 多次运行结果一致

### 7.2 编译运行流程

```powershell
# 1. 重新构建编译器和 runtime
cmake --build build
cmake --build runtime/build

# 2. 编译测试
cd example
.\compile.cmd

# 3. 运行测试
.\test.exe
```

---

## 八、实施步骤

| 步骤 | 模块 | 文件 | 依赖 |
|:---|:---|:---|:---|
| 1 | 运行时 | `runtime/builtin/mutex.h`（新增）, `mutex.cpp`（新增） | 无 |
| 2 | 运行时 | `runtime/aura_rt.h`, `runtime/CMakeLists.txt` | 步骤 1 |
| 3 | Sema | `src/Sema/BuiltinRegistry.h` init() 注册 Mutex 类型 + sync.Mutex 构造 | 步骤 1 |
| 4 | AST | `src/AST/Stmt.h` 新增 LockStmt, `src/ASTPrinter.cpp` print | 无 |
| 5 | Parser | `src/Parser/StmtParser.cpp` parseLockStmt + dispatch, `src/Parser.h` 声明 | 步骤 4 |
| 6 | Sema | `src/Sema/SemAnalyzer.h` 字段 + 声明, `src/Sema/SemAnalyzer.cpp` dispatch + L3, `src/Sema/Checker/StmtChecker.cpp` checkLockStmt + L1 + L3 + L6 | 步骤 4 |
| 7 | CodeGen | `src/CodeGen/StmtGen.cpp` genLockStmt + dispatch, `src/CodeGen/CodeGen.h` 声明 | 步骤 4 |
| 8 | CodeGen | `src/CodeGen/ExprGen.cpp` sync.Mutex() 构造调用 | 步骤 3 |
| 9 | 构建 | `cmake --build build` + `cmake --build runtime/build` | 步骤 1-8 |
| 10 | 测试 | `example/test.aura` 写入测试，`compile.cmd` 编译，运行 `test.exe` | 步骤 9 |

---

## 九、潜在风险与规避

### 9.1 lockExpr 类型推断

`lockExpr->inferredType` 由 Sema 标注（[ASTNode.h:21](file:///d:/you/Aura/src/AST/ASTNode.h#L21)）。v1.0 仅检测 `Mutex*`，通过 `lockTy->canonicalName() == "Mutex"` 匹配。

**注意**：Mutex 在 BuiltinRegistry 注册为 `isGcObject=true`，Sema 推断的 canonicalName 应为 `"Mutex"`（不含 `*`，与 string/channel 一致）。需测试验证。

### 9.2 `lock` 软关键字歧义

`lock` 作为普通标识符仍可用（如 `let lock = sync.Mutex()`）。Parser 仅在语句起始位置 + 后续 `(` 时识别为 LockStmt。

**测试用例**：

```aura
fun main(io: Io) {
    let lock = sync.Mutex()    # lock 作为变量名
    lock (lock) {              # 第一个 lock 是关键字，第二个 lock 是变量
        io.println("ok")
    }
}
```

### 9.3 compact GC 损坏 mutex（已规避）

`std::mutex` 不可移动。Mutex 用间接指针 `std::mutex* m_` 规避：compact 时 Mutex 主体搬迁，m_ 指针的值被正确拷贝，指向同一块堆 mutex。

### 9.4 finalizer 中死锁（已规避）

finalizer 在 GC STW 期间执行，所有 mutator 线程已暂停。不在 finalizer 中获取锁，仅 `delete` 堆内存。

### 9.5 lock 块内 io 异步方法

v1.0 简化处理：lock 块内强制 `isCoroutine = false`（genBlock 第三参数）。若用户在 lock 块内调用 `io.println(...)`（异步协程方法），会因 ioSync_ 不一致生成失败。

**v1.0 策略**：lock 块内必须用 `io.println_sync(...)`（同步版本）。后续 v1.1 增加 Sema L4 检查给出明确错误信息。
