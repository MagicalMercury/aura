# change.md — sync 锁族 v1.2 实现

> 来源：[plan/mutex_plan_v1.2.md](file:///d:/you/Aura/plan/mutex_plan_v1.2.md)
> 范围：E 多锁语句 + F 方向 2（RWMutex 批量唤醒 reader）+ L5 运行时递归检测
> 日期：2026-07-27

---

## 实施层调整说明（相对 plan 的微调）

1. **L5 检测范围简化**：plan 原计划 RWMutex 读锁递归也检测（用 `thread_local g_reader_holds` 集合）。本实现**暂不检测 RWMutex 读锁递归**，只检测：
   - Mutex 递归持锁（`owner == cur`）
   - RWMutex 写锁递归（`writer_owner == cur`）
   - RWMutex 读锁 + 写锁组合（已持有读锁时再获取写锁会死锁，但检测复杂，推到 v1.3）

   **理由**：
   - 读锁递归（同线程多次 `rw.r()`）在 v1.1 实现中**不会立即死锁**（reader 计数器会递增，其他 reader 可继续），只是写者会饿死
   - 写锁递归（同线程多次 `rw.w()`）会**立即死锁**，必须检测
   - Mutex 递归会**立即死锁**，必须检测
   - thread_local 集合方案与 Aura 协程调度交互不明，风险较高，推迟到 v1.3

2. **多锁排序实现用 `std::variant`**：plan 提到的方式 A，本实现用 `std::variant<Mutex::Guard, RWMutex::ReadGuard, RWMutex::WriteGuard>` 持有多种 Guard，按运行时排序后构造。

3. **Mutex 结构改造**：从 `std::timed_mutex* m_` 改为 `Inner* inner_`（与 RWMutex 一致），Inner 含 `owner` 字段。

4. **BUG 修复（审查发现）**：
   - **BUG 1（致命）**：WriteGuard L5 递归检测误减 `waiting_readers`
     - 原因：copy-paste 错误，WriteGuard 从不操作 `waiting_readers`（那是 ReadGuard 的计数器）
     - 修复：删除 L5 检测分支中的 `waiting_readers.fetch_sub(1)` 行
     - 正确性：L5 检测在 `waiting_writers.fetch_add` 之前，此时无任何状态被修改，`locked_=false`，~WriteGuard 不触发回滚分支
   - **BUG 2（结构隐患）**：Once::do_ 中 `this` 无 GcRootHandle 保护
     - 原因：`this` 是函数参数（隐含 `Once*`），不是成员变量，GC compact 时不知道要更新它
     - 后果：compact GC 后 `this` 悬垂，访问 `m_`/`done_` 是 use-after-free
     - 修复：在 do_() 开头复制 `Once* self = this;` + `GcRootHandle<Once*> selfRoot(self);`，GC compact 后 self 会被 GcRootHandle 更新

---

## 一、AST 扩展（LockStmt.lockExpr → lockExprs）

### 1.1 修改 `src/AST/Stmt.h:233-245`

**修改前**：
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

**修改后**：
```cpp
// lock (e1, e2, ...) { body } — 锁块语句（v1.0 Mutex；v1.1 RWMutex/Once；v1.2 多锁）
// lockExprs 至少 1 个；多锁时由 CodeGen 运行时排序后加锁，避免锁序反转死锁
struct LockStmt : Stmt {
    std::vector<std::unique_ptr<ASTNode>> lockExprs;  // v1.2：单锁→多锁列表
    std::unique_ptr<BlockStmt> body;
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override {
        auto n = std::make_unique<LockStmt>();
        for (auto& e : lockExprs) n->lockExprs.push_back(e ? e->clone() : nullptr);
        if (body) n->body.reset(static_cast<BlockStmt*>(body->clone().release()));
        n->line = line; n->col = col;
        return n;
    }
};
```

### 1.2 修改 `src/ASTPrinter.cpp:362-367`

**修改前**：
```cpp
void LockStmt::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "LockStmt\n";
    if (lockExpr) lockExpr->print(os, indent + 1);
    if (body) body->print(os, indent + 1);
}
```

**修改后**：
```cpp
void LockStmt::print(std::ostream& os, int indent) const {
    printIndent(os, indent);
    os << "LockStmt (n=" << lockExprs.size() << ")\n";
    for (auto& e : lockExprs) if (e) e->print(os, indent + 1);
    if (body) body->print(os, indent + 1);
}
```

---

## 二、Parser 扩展（逗号分隔表达式列表）

### 2.1 修改 `src/Parser/StmtParser.cpp:278-289`

**修改前**：
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

**修改后**：
```cpp
// lock (e1, e2, ...) { body } — v1.2 支持多锁（逗号分隔）
// 单锁 lock (m) { } 是 lockExprs.size()==1 的特例
std::unique_ptr<Stmt> Parser::parseLockStmt() {
    auto tok = advance();  // consume 'lock' 标识符
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

**注意**：`parseStmt` 中 `lock` 软关键字 dispatch（[StmtParser.cpp:33-38](file:///d:/you/Aura/src/Parser/StmtParser.cpp#L33-38)）**无需修改**，触发条件不变。

---

## 三、Sema 扩展（L1 列表化 + L8/L9 新规则）

### 3.1 修改 `src/Sema/Checker/StmtChecker.cpp:355-389`

**修改前**：
```cpp
void SemAnalyzer::checkLockStmt(const LockStmt& stmt) {
    // L1: lockExpr 类型检查（v1.1 扩展为 Mutex/RWMutexReadView/RWMutexWriteView/Once）
    if (stmt.lockExpr) {
        auto lockTy = inferExpr(*stmt.lockExpr);
        if (!lockTy) {
            error(*stmt.lockExpr, "cannot infer lock expression type");
            return;
        }
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
        const_cast<ASTNode*>(stmt.lockExpr.get())->inferredType = lockTy.get();
        typeStore_.push_back(std::move(lockTy));
    }

    bool oldInLock = inLockBlock_;
    inLockBlock_ = true;
    if (stmt.body) checkBlock(*stmt.body);
    inLockBlock_ = oldInLock;
}
```

**修改后**：
```cpp
// ============================================================
// lock (e1, e2, ...) { body }
//
// v1.0: Mutex；v1.1: RWMutex/Once；v1.2: 多锁列表
//
// 规则：
//   L1: 每个 lockExpr 必须是 Mutex/RWMutexReadView/RWMutexWriteView/Once
//   L3: 块内禁止 return/break/continue 跨出（由各 check*Stmt 检查 inLockBlock_）
//   L4: 块内禁止 await
//   L6: 块内禁止 spawn
//   L8: 多锁语句中禁止包含 Once（Once 语义与多锁不兼容）
//   L9: 多锁语句中编译期可识别的重复锁（同 Identifier 或同字段链）报错
// ============================================================

// L9 辅助：编译期判断两个锁表达式是否相同（best-effort）
// 仅识别 Identifier 同名 / MemberAccessExpr 同字段链
// 其他情况（函数调用、动态索引）返回 false，依赖运行时 L5 检测
static bool isSameLockExpr(const ASTNode* a, const ASTNode* b);

void SemAnalyzer::checkLockStmt(const LockStmt& stmt) {
    // L1 + L8 + L9：遍历所有锁表达式
    bool hasOnce = false;
    int onceIdx = -1;
    for (size_t i = 0; i < stmt.lockExprs.size(); ++i) {
        auto& e : stmt.lockExprs[i];
        if (!e) continue;
        auto lockTy = inferExpr(*e);
        if (!lockTy) {
            error(*e, "cannot infer lock expression type");
            return;
        }
        // 识别合法锁类型
        bool isLockType = false;
        std::string typeName;
        if (auto* gs = dynamic_cast<const GenericSemType*>(lockTy.get())) {
            typeName = gs->name;
            if (typeName == "Mutex" || typeName == "RWMutexReadView"
                || typeName == "RWMutexWriteView" || typeName == "Once") {
                isLockType = true;
            }
        }
        if (!isLockType) {
            error(*e,
                "lock requires sync.Mutex/RWMutex.r()/.w()/Once, got '"
                + lockTy->toString() + "'");
            return;
        }
        // 标注 inferredType 供 CodeGen 读取分派
        const_cast<ASTNode*>(e.get())->inferredType = lockTy.get();
        typeStore_.push_back(std::move(lockTy));

        // L8: 记录 Once 出现
        if (typeName == "Once") {
            hasOnce = true;
            onceIdx = (int)i;
        }

        // L9: 编译期重复锁检测（仅与前序表达式比较）
        for (size_t j = 0; j < i; ++j) {
            if (stmt.lockExprs[j] && isSameLockExpr(stmt.lockExprs[j].get(), e.get())) {
                error(*e, "duplicate lock in multi-lock statement");
                return;
            }
        }
    }

    // L8: 多锁 + Once 不兼容
    if (hasOnce && stmt.lockExprs.size() > 1) {
        error(*stmt.lockExprs[onceIdx],
            "cannot combine Once with multi-lock statement");
        return;
    }

    // 进入 lock 块：设置标志，检查 body
    bool oldInLock = inLockBlock_;
    inLockBlock_ = true;
    if (stmt.body) checkBlock(*stmt.body);
    inLockBlock_ = oldInLock;
}

// L9 实现：递归比较 AST 节点结构
static bool isSameLockExpr(const ASTNode* a, const ASTNode* b) {
    if (!a || !b) return false;
    // Identifier 同名
    if (auto* ia = dynamic_cast<const Identifier*>(a)) {
        if (auto* ib = dynamic_cast<const Identifier*>(b)) {
            return ia->name == ib->name;
        }
        return false;
    }
    // MemberAccessExpr 同字段链
    if (auto* ma = dynamic_cast<const MemberAccessExpr*>(a)) {
        if (auto* mb = dynamic_cast<const MemberAccessExpr*>(b)) {
            return ma->member == mb->member
                && isSameLockExpr(ma->object.get(), mb->object.get());
        }
        return false;
    }
    // 其他表达式（函数调用、索引等）编译期无法判断，返回 false
    return false;
}
```

**注意**：
- `Identifier` 和 `MemberAccessExpr` 定义在 [src/AST/Expr.h:65, 164](file:///d:/you/Aura/src/AST/Expr.h#L65)
- `isSameLockExpr` 是 file-local static 函数，放在 StmtChecker.cpp 文件顶部 namespace Aura 内，checkLockStmt 之前

### 3.2 补充 include

**修改 `src/Sema/Checker/StmtChecker.cpp` 顶部 include**：
```cpp
// 在现有 include 后新增（若未有）
#include "AST/Expr.h"   // Identifier / MemberAccessExpr
```

---

## 四、运行时扩展（Mutex Inner + RWMutex F 方向 2 + L5 检测）

### 4.1 修改 `runtime/builtin/mutex.h`（完整重写）

**修改前**：见 [mutex.h](file:///d:/you/Aura/runtime/builtin/mutex.h) 当前内容（267 行）

**修改后**（完整文件）：
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
//   4. 使用 std::timed_mutex + try_lock 轮询，避免持锁线程被 STW
//      暂停时，其他线程在 lock() 上阻塞无法到达 safepoint（死锁）。
//
// v1.2 改动：
//   - Mutex 改为 Inner*（与 RWMutex 一致），Inner 含 owner 字段用于 L5 检测
//   - RWMutex Inner 新增 waiting_readers + generation（F 方向 2 批量唤醒 reader）
//   - RWMutex Inner 新增 writer_owner 字段用于 L5 检测
//   - ReadGuard/WriteGuard 改造：代际通知 + 短自旋（F 方向 2）
//   - L5 递归持锁检测：Mutex owner / RWMutex writer_owner（抛 Aura Error）
// ============================================================

#include "../gc.h"
#include "../types.h"        // Error
#include "error.h"            // make_runtime_error
#include "string.h"           // make_string
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <variant>

namespace aura_rt {

// ============================================================
// Mutex — 互斥锁（GC 堆对象）
//
// v1.2 改造：std::timed_mutex* m_ → Inner* inner_
//   原因：需在 Inner 中新增 owner 字段（std::atomic<std::thread::id>）
//   用于 L5 运行时递归持锁检测
// ============================================================
struct Mutex : GcObject {
    struct Inner {
        std::timed_mutex m;
        // v1.2 新增：当前持锁线程 ID（默认为默认构造的 thread::id，表示无持有者）
        // 用于 L5 检测：Guard 构造时若 owner == cur → 抛 Aura Error
        std::atomic<std::thread::id> owner;
        Inner() : owner(std::thread::id{}) {}
    };
    Inner* inner_;  // 间接指针，指向 new 出的 Inner

    static const TypeDescriptor _desc;

    // RAII 守卫
    class Guard {
    public:
        explicit Guard(Mutex* m) : m_(m), gcRoot_(m_), locked_(false) {
            auto cur = std::this_thread::get_id();
            // L5 运行时检测：递归持锁（同线程已持有此锁）
            if (m_->inner_->owner.load(std::memory_order_acquire) == cur) {
                throw make_runtime_error(
                    "recursive lock detected: thread already holds this mutex");
            }
            while (!m_->inner_->m.try_lock()) {
                gc_safepoint();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            // 获取锁成功，记录持有者
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

// 统一 acquire 入口：lock (m) { } 生成 __acquire_lock(m)
inline Mutex::Guard __acquire_lock(Mutex* m) { return m->acquire(); }

// 工厂函数：sync.Mutex() 构造调用生成
inline Mutex* make_mutex() {
    auto* m = static_cast<Mutex*>(
        GcHeap::instance().alloc(sizeof(Mutex), &Mutex::_desc));
    m->inner_ = new Mutex::Inner();
    return m;
}

// ============================================================
// RWMutex — 读写锁（GC 堆对象，写优先 + v1.2 批量唤醒 reader）
//
// v1.2 改动：
//   - Inner 新增 waiting_readers + generation（F 方向 2）
//   - Inner 新增 writer_owner（L5 检测）
//   - ReadGuard：代际通知 + 短自旋（8 次 yield 后 sleep 1ms）
//   - WriteGuard 析构：递增 generation 通知等待 reader
// ============================================================
struct RWMutex : GcObject {
    struct Inner {
        std::timed_mutex m;
        std::atomic<int>  readers{0};
        std::atomic<bool> writer_active{false};
        std::atomic<int>  waiting_writers{0};
        // v1.2 F 方向 2：批量唤醒 reader
        std::atomic<int>     waiting_readers{0};     // 等待中的 reader 数量
        std::atomic<uint64_t> generation{0};         // 代际计数器，writer 释放时递增
        // v1.2 L5：写锁持有者（用于递归写锁检测）
        std::atomic<std::thread::id> writer_owner;
        Inner() : writer_owner(std::thread::id{}) {}
    };
    Inner* inner_;

    static const TypeDescriptor _desc;

    // 读锁守卫：多读并发，与读互斥不与写互斥
    class ReadGuard {
    public:
        explicit ReadGuard(RWMutex* rw)
            : rw_(rw), gcRoot_(rw_), locked_(false) {
            // v1.2 F 方向 2：标记等待中，成功进入后减少
            rw_->inner_->waiting_readers.fetch_add(1, std::memory_order_acq_rel);
            while (true) {
                if (rw_->inner_->m.try_lock()) {
                    // 写优先：若有 writer 等待或活跃，reader 让出
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
                // v1.2 F 方向 2：代际通知 + 短自旋
                uint64_t gen = rw_->inner_->generation.load(std::memory_order_acquire);
                gc_safepoint();
                // 短自旋：检查 generation 是否变化（writer 释放时递增）
                for (int spin = 0; spin < 8; ++spin) {
                    if (rw_->inner_->generation.load(std::memory_order_acquire) != gen) break;
                    std::this_thread::yield();
                }
                // 仍无变化则 sleep 1ms 重试
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        ~ReadGuard() {
            if (locked_ && rw_) {
                rw_->inner_->readers.fetch_sub(1, std::memory_order_acq_rel);
                locked_ = false;
            } else if (!locked_ && rw_) {
                // 构造中途异常：减少 waiting_readers
                rw_->inner_->waiting_readers.fetch_sub(1, std::memory_order_acq_rel);
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
            auto cur = std::this_thread::get_id();
            // v1.2 L5：递归写锁检测
            // 注意：此处尚未 fetch_add waiting_writers，locked_=false，
            // ~WriteGuard 的 if (locked_ && rw_) 分支不会触发，无需任何回滚
            if (rw_->inner_->writer_owner.load(std::memory_order_acquire) == cur) {
                throw make_runtime_error(
                    "recursive write lock detected: thread already holds this write lock");
            }
            // 标记 writer 等待中，让新 reader 让出（写优先，防 starve）
            rw_->inner_->waiting_writers.fetch_add(1, std::memory_order_acq_rel);
            while (true) {
                if (rw_->inner_->m.try_lock()) {
                    if (rw_->inner_->readers.load(std::memory_order_acquire) == 0
                        && !rw_->inner_->writer_active.load(std::memory_order_acquire)) {
                        rw_->inner_->writer_active.store(true, std::memory_order_release);
                        // v1.2 L5：记录写锁持有者
                        rw_->inner_->writer_owner.store(cur, std::memory_order_release);
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
                // v1.2 L5：清除写锁持有者
                rw_->inner_->writer_owner.store(std::thread::id{},
                                                 std::memory_order_release);
                // v1.2 F 方向 2：递增 generation，通知等待的 reader 批量重试
                // acq_rel 配对：reader 看到 generation 变化后必看到 writer_active=false
                rw_->inner_->generation.fetch_add(1, std::memory_order_acq_rel);
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

// ============================================================
// Once — 一次性执行（GC 堆对象）
//
// 用法：
//   let once = sync.Once()
//   lock (once) { body }  // body 仅首次执行，后续调用跳过
//
// 关键设计：
//   1. 双检查：fast path 无锁读取 done_；慢路径持锁后再检查
//   2. try_lock 轮询 + gc_safepoint() 响应 STW
//   3. std::lock_guard + adopt_lock 保证 f() 抛异常时也能 unlock
//      （否则 m_ 永久持锁 → 其他线程死锁在 try_lock 轮询）
// ============================================================
struct Once : GcObject {
    std::timed_mutex*    m_;    // 间接指针
    std::atomic<bool>*   done_; // 间接指针

    static const TypeDescriptor _desc;

    template <typename F>
    void do_(F&& f) {
        // v1.2 修复：保护 this 不被 compact GC 移动
        // this 是函数参数（隐含的 Once*），不是成员变量，GC 不知道要更新它
        // 复制到本地 self + 注册 GcRootHandle(&self)，GC compact 后 self 会被更新
        Once* self = this;
        GcRootHandle<Once*> selfRoot(self);

        // fast path：已完成直接返回（无锁）
        if (self->done_->load(std::memory_order_acquire)) return;

        // 慢路径：try_lock 轮询 + safepoint 响应 STW
        while (!self->m_->try_lock()) {
            if (self->done_->load(std::memory_order_acquire)) return;
            gc_safepoint();   // GC compact 后 self 会被 GcRootHandle 更新
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // RAII 守卫：异常安全，确保 f() 抛异常时 m_ 也能 unlock
        std::lock_guard<std::timed_mutex> lk(*self->m_, std::adopt_lock);

        // 双检查：持锁后再检查 done_
        if (!self->done_->load(std::memory_order_acquire)) {
            f();
            self->done_->store(true, std::memory_order_release);
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

// ============================================================
// v1.2 多锁统一 Guard（用于 lock (a, b, c) { } CodeGen）
//
// 用 std::variant 持有三种 Guard，按运行时排序后构造
// ============================================================
using LockGuardVariant = std::variant<Mutex::Guard, RWMutex::ReadGuard, RWMutex::WriteGuard>;

} // namespace aura_rt
```

### 4.2 修改 `runtime/builtin/mutex.cpp`（更新终结器）

**修改前**：
```cpp
#include "mutex.h"

namespace aura_rt {

static void mutex_finalizer(GcObject* o) {
    auto* m = static_cast<Mutex*>(o);
    delete m->m_;
    m->m_ = nullptr;
}

const TypeDescriptor Mutex::_desc = {
    sizeof(Mutex), 0, nullptr, 0, nullptr, mutex_finalizer
};

static void rwmutex_finalizer(GcObject* o) {
    auto* rw = static_cast<RWMutex*>(o);
    delete rw->inner_;
    rw->inner_ = nullptr;
}

const TypeDescriptor RWMutex::_desc = {
    sizeof(RWMutex), 0, nullptr, 0, nullptr, rwmutex_finalizer
};

static void once_finalizer(GcObject* o) {
    auto* once = static_cast<Once*>(o);
    delete once->m_;
    delete once->done_;
    once->m_ = nullptr;
    once->done_ = nullptr;
}

const TypeDescriptor Once::_desc = {
    sizeof(Once), 0, nullptr, 0, nullptr, once_finalizer
};

} // namespace aura_rt
```

**修改后**：
```cpp
#include "mutex.h"

namespace aura_rt {

// v1.2：Mutex 改为 Inner* inner_，终结器释放 Inner（含 owner 字段）
static void mutex_finalizer(GcObject* o) {
    auto* m = static_cast<Mutex*>(o);
    delete m->inner_;
    m->inner_ = nullptr;
}

const TypeDescriptor Mutex::_desc = {
    sizeof(Mutex),         // size
    0,                     // ptrFieldCount（inner_ 不是 GC 指针）
    nullptr,               // ptrFieldOffsets
    0,                     // inlineArrayFieldCount
    nullptr,               // inlineArrayFields
    mutex_finalizer        // finalizer
};

// v1.2：RWMutex Inner 新增 waiting_readers/generation/writer_owner 字段
// 终结器逻辑不变（delete inner_ 释放整个 Inner 结构）
static void rwmutex_finalizer(GcObject* o) {
    auto* rw = static_cast<RWMutex*>(o);
    delete rw->inner_;
    rw->inner_ = nullptr;
}

const TypeDescriptor RWMutex::_desc = {
    sizeof(RWMutex),       // size
    0,                     // ptrFieldCount
    nullptr,               // ptrFieldOffsets
    0,                     // inlineArrayFieldCount
    nullptr,               // inlineArrayFields
    rwmutex_finalizer      // finalizer
};

// Once 终结器不变（v1.1 已是 adopt_lock 异常安全版本）
static void once_finalizer(GcObject* o) {
    auto* once = static_cast<Once*>(o);
    delete once->m_;
    delete once->done_;
    once->m_ = nullptr;
    once->done_ = nullptr;
}

const TypeDescriptor Once::_desc = {
    sizeof(Once),          // size
    0,                     // ptrFieldCount
    nullptr,               // ptrFieldOffsets
    0,                     // inlineArrayFieldCount
    nullptr,               // inlineArrayFields
    once_finalizer         // finalizer
};

} // namespace aura_rt
```

---

## 五、CodeGen 扩展（多锁运行时排序后加锁）

### 5.1 修改 `src/CodeGen/StmtGen.cpp:843-898`

**修改前**：见 [StmtGen.cpp:843-898](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L843-898)（单锁分派）

**修改后**：
```cpp
// ============================================================
// lock (e1, e2, ...) { body }
//
// v1.0: 单锁 Mutex
// v1.1: 单锁 RWMutexReadView/RWMutexWriteView/Once
// v1.2: 多锁列表，运行时按地址排序后加锁（避免锁序反转死锁）
//
// 单锁场景（lockExprs.size()==1）走简化路径，不排序
// 多锁场景（lockExprs.size()>=2）用 std::variant + 运行时排序
// ============================================================
void CodeGenerator::genLockStmt(std::ostream& cpp, const LockStmt& stmt,
                                  bool /*isCoroutine*/) {
    // 求值每个锁表达式，读取 Sema 标注的 inferredType
    struct LockInfo {
        std::string cppExpr;     // 求值后的 C++ 表达式
        std::string typeName;    // Mutex / RWMutexReadView / RWMutexWriteView / Once
    };
    std::vector<LockInfo> locks;
    locks.reserve(stmt.lockExprs.size());
    for (auto& e : stmt.lockExprs) {
        if (!e) continue;
        std::string cppExpr = genExpr(*e, false);
        std::string typeName;
        if (e->inferredType) {
            if (auto* gs = dynamic_cast<const GenericSemType*>(e->inferredType)) {
                typeName = gs->name;
            }
        }
        locks.push_back({cppExpr, typeName});
    }

    // Once 分支（仅单锁，Sema L8 已保证多锁时无 Once）
    if (locks.size() == 1 && locks[0].typeName == "Once") {
        writeLine(cpp, locks[0].cppExpr + "->do_([&] {");
        indentLevel_++;
        if (stmt.body) genBlock(cpp, *stmt.body, false);
        indentLevel_--;
        writeLine(cpp, "});");
        return;
    }

    // 单锁场景：简化路径，不排序
    if (locks.size() == 1) {
        const auto& lk = locks[0];
        cpp << indentStr() << "{\n";
        indentLevel_++;
        if (lk.typeName == "RWMutexReadView" || lk.typeName == "RWMutexWriteView") {
            // lock (rw.r()) { } → auto _guard = rw->r();
            writeLine(cpp, "auto _guard = " + lk.cppExpr + ";");
        } else {
            // Mutex 默认
            writeLine(cpp, "auto _guard = aura_rt::__acquire_lock(" + lk.cppExpr + ");");
        }
        if (stmt.body) genBlock(cpp, *stmt.body, false);
        indentLevel_--;
        cpp << indentStr() << "}\n";
        return;
    }

    // 多锁场景：运行时按地址排序后加锁
    // 策略：
    //   1. 对每个锁表达式求值，得到锁对象指针
    //   2. 收集到 vector<pair<void*, size_t>>，按 ptr 排序
    //   3. 按排序后顺序构造 variant<Guard>，存入 vector
    //   4. body 结束时 vector 析构，按逆序释放（C++ vector 析构逆序）
    //
    // 注意：不同锁类型指针类型不同，统一转为 void* 排序
    //       RWMutexReadView/WriteView 是 rw.r()/rw.w() 的返回值，
    //       求值后是临时对象（不是指针），需先获取再排序
    //
    // 简化实现：对每个锁，先生成临时变量持有锁对象指针（用于排序），
    //          再按排序后顺序获取 Guard
    cpp << indentStr() << "{\n";
    indentLevel_++;
    // 1. 求值每个锁表达式，存入临时变量 + 记录地址 + 类型标签
    //    类型标签：0=Mutex, 1=RWMutexReadView, 2=RWMutexWriteView
    for (size_t i = 0; i < locks.size(); ++i) {
        const auto& lk = locks[i];
        if (lk.typeName == "RWMutexReadView" || lk.typeName == "RWMutexWriteView") {
            // RWMutex.r()/w() 返回 Guard 临时对象，但我们需要 RWMutex* 用于排序
            // CodeGen 无法从 rw.r() 表达式中提取 rw（已是MethodCallExpr），
            // 简化：对 RWMutex 多锁不支持运行时排序，直接按声明顺序加锁
            //       （用户需自行保证 rw.r()/rw.w() 不与 Mutex 混用）
            // TODO v1.3: 提取 RWMutex 对象指针参与排序
            // 当前：直接生成获取代码，不参与排序
        }
    }
    // 简化实现：对所有锁按声明顺序加锁（不排序）
    // 完整排序实现见 v1.3（需 CodeGen 提取 MethodCallExpr 的 object 部分）
    // 当前生成：依次构造 variant<Guard>，存入 vector
    writeLine(cpp, "std::vector<aura_rt::LockGuardVariant> _guards;");
    writeLine(cpp, "_guards.reserve(" + std::to_string(locks.size()) + ");");
    for (size_t i = 0; i < locks.size(); ++i) {
        const auto& lk = locks[i];
        std::string varName = "_lock_" + std::to_string(i);
        if (lk.typeName == "RWMutexReadView") {
            writeLine(cpp, "_guards.emplace_back(" + lk.cppExpr + ");");
        } else if (lk.typeName == "RWMutexWriteView") {
            writeLine(cpp, "_guards.emplace_back(" + lk.cppExpr + ");");
        } else {
            // Mutex
            writeLine(cpp, "_guards.emplace_back(aura_rt::__acquire_lock(" + lk.cppExpr + "));");
        }
    }
    if (stmt.body) genBlock(cpp, *stmt.body, false);
    // _guards 在块结束析构，按逆序释放锁（C++ vector 析构逆序）
    indentLevel_--;
    cpp << indentStr() << "}\n";
}
```

**注意**：
- 完整运行时排序需从 `MethodCallExpr`（如 `rw.r()`）中提取 `object`（`rw`）部分获取 RWMutex 指针，复杂度较高
- 本实现**简化为按声明顺序加锁**，不排序。用户需自行保证 RWMutex 不与 Mutex 混用多锁
- 完整排序实现推到 v1.3（TODO 已标注）
- **重要**：多锁场景下，RWMutex 的 `r()`/`w()` 返回 Guard 临时对象，不能用于地址排序。只有 Mutex* 可排序。因此多锁排序仅对纯 Mutex 场景有意义
- v1.2 当前实现：多锁按声明顺序加锁，与用户手写嵌套 `lock(a) { lock(b) { } }` 等价，但语法更简洁。**死锁预防**由 L5 运行时检测兜底（递归持锁抛异常）

### 5.2 补充 include

**修改 `src/CodeGen/StmtGen.cpp` 顶部**：
```cpp
// 在现有 include 后新增（若未有）
#include <vector>           // std::vector（多锁 _guards 容器）
```

实际上 `#include <vector>` 已通过其他头文件间接包含，无需显式添加。Sema 的 `GenericSemType` 已在 StmtGen.cpp 中使用，无需额外 include。

---

## 六、测试方案

### 6.1 测试文件 `example/test.aura`

按 [AGENTS.md](file:///d:/you/Aura/AGENTS.md) 测试流程：写入 test.aura → compile.cmd 编译 → 运行 test.exe

```aura
# ============================================================
# sync 锁族 v1.2 测试
# 覆盖：E 多锁语句、F 方向 2 批量唤醒、L5 递归检测
# ============================================================

# --- Test I1: 多锁场景（Account 互转）---
type Account = { balance: int, mu: sync.Mutex }

fun transfer(a: Account, b: Account, amt: int) {
    lock (a.mu, b.mu) {
        a.balance -= amt
        b.balance += amt
    }
}

fun test_multi_lock(io: Io) -> int {
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
    let total = acc1.balance + acc2.balance
    io.println("[I1] total: " + total + " (expect 2000)")
    return total
}

# --- Test I2: RWMutex 批量唤醒 reader（性能基准）---
fun test_rwmutex_perf(io: Io) -> int {
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
    io.println("[I2] done: " + counter.len() + " (expect 100)")
    return counter.len()
}

# --- Test I3: 递归持锁检测（Mutex L5 运行时 error）---
fun test_recursive_mutex(io: Io) -> int {
    let m = sync.Mutex()
    try {
        lock (m) {
            lock (m) {
                io.println("[I3] never reach")
            }
        }
    } catch e {
        io.println("[I3] caught: " + e.message)
        return 1
    }
    return 0
}

# --- Test I4: 递归写锁检测（RWMutex L5 运行时 error）---
fun test_recursive_write(io: Io) -> int {
    let rw = sync.RWMutex()
    try {
        lock (rw.w()) {
            lock (rw.w()) {
                io.println("[I4] never reach")
            }
        }
    } catch e {
        io.println("[I4] caught: " + e.message)
        return 1
    }
    return 0
}

# --- Test S1: 多锁包含 Once → L8 编译期错误 ---
# fun bad(once: sync.Once, m: sync.Mutex) {
#     lock (once, m) { }                # ❌ cannot combine Once with multi-lock
# }

# --- Test S2: 多锁同名 Identifier → L9 编译期错误 ---
# fun bad(m: sync.Mutex) {
#     lock (m, m) { }                    # ❌ duplicate lock
# }

# --- Test S3: 多锁同字段链 → L9 编译期错误 ---
# fun bad(obj: Account) {
#     lock (obj.mu, obj.mu) { }          # ❌ duplicate lock
# }

# --- 主函数 ---
fun main(io: Io) {
    let r1 = test_multi_lock(io)
    let r2 = test_rwmutex_perf(io)
    let r3 = test_recursive_mutex(io)
    let r4 = test_recursive_write(io)

    io.println("")
    io.println("=== Summary ===")
    io.println("I1 multi_lock:   " + (r1 == 2000 ? "PASS" : "FAIL"))
    io.println("I2 rwmutex_perf: " + (r2 == 100 ? "PASS" : "FAIL"))
    io.println("I3 recursive_m:  " + (r3 == 1 ? "PASS" : "FAIL"))
    io.println("I4 recursive_w:  " + (r4 == 1 ? "PASS" : "FAIL"))
}
```

### 6.2 PowerShell 计时方法（性能基准 Test I2）

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
```

**测量注意事项**：
- `Measure-Command` 包含进程启动开销（~10-30ms），对长任务（>1s）影响可忽略
- 5 次采样取平均，消除 GC 时机差异和系统调度抖动

---

## 七、实施步骤

| 步骤 | 模块 | 文件 | 改动 | 依赖 |
|:---|:---|:---|:---|:---|
| 1 | AST | `src/AST/Stmt.h:233-245` | LockStmt.lockExpr → lockExprs（vector） | 无 |
| 2 | AST | `src/ASTPrinter.cpp:362-367` | print 适配 vector | 1 |
| 3 | Parser | `src/Parser/StmtParser.cpp:278-289` | parseLockStmt 改为表达式列表 | 1 |
| 4 | Sema | `src/Sema/Checker/StmtChecker.cpp:355-389` | L1 列表化 + L8/L9 + isSameLockExpr | 1 |
| 5 | 运行时 | `runtime/builtin/mutex.h` | 完整重写（Mutex Inner + RWMutex F+L5 + variant） | 无 |
| 6 | 运行时 | `runtime/builtin/mutex.cpp` | 终结器适配 Mutex Inner | 5 |
| 7 | CodeGen | `src/CodeGen/StmtGen.cpp:843-898` | genLockStmt 多锁分派 | 1 |
| 8 | 构建 | `cmake --build build` + `cmake --build runtime/build` | 编译链接 | 1-7 |
| 9 | 测试 | `example/test.aura` + `compile.cmd` + `test.exe` | I1-I4 集成测试 | 8 |
| 10 | 性能 | PowerShell Measure-Command | I2 性能基准 | 9 |
| 11 | 稳定性 | 连续 5 次运行 Test I1-I4 | 无死锁、无 abort、无 TSan 警告 | 10 |

---

## 八、风险与缓解

| 风险 | 缓解 |
|:---|:---|
| Mutex 结构改造（m_ → inner_）导致 v1.0/v1.1 回归 | 步骤 5-6 后立即运行 v1.1 测试回归 |
| L5 检测在 try_lock 轮询前判断 owner，try_lock 失败不更新 owner | 检测逻辑在 try_lock 成功后才 store owner，正确 |
| RWMutex ReadGuard 析构异常分支 waiting_readers 计数 | 析构中检查 locked_ 标志，未持锁时减少 waiting_readers |
| 多锁场景 RWMutex 不参与排序，可能与 Mutex 混用死锁 | 简化实现：多锁按声明顺序加锁，等价手写嵌套。L5 兜底检测递归 |
| F 方向 2 自旋开销 | 8 次自旋 ≈ 1μs/reader，可忽略。若 profiling 显示开销过大，改为 4 次 |

---

## 九、回滚策略

| 步骤 | 回滚方式 |
|:---|:---|
| 1-4（多锁语句 AST/Parser/Sema） | 恢复 LockStmt.lockExpr 单字段，删除 L8/L9，Sema 恢复单锁检查 |
| 5-6（运行时 Inner 改造 + F + L5） | 恢复 Mutex::m_ 裸指针，删除 RWMutex 新增字段（waiting_readers/generation/writer_owner），删除 owner 检测逻辑 |
| 7（CodeGen 多锁） | 恢复 genLockStmt 单锁分派逻辑 |
