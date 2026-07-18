# 多线程：`sync thread` 语句草案

> 来源：用户指示 — sync 块是协程（单线程协作式），多线程需要独立的 `sync thread` 语句
> 关键字策略：`thread` 为**软关键字**（contextual keyword），非保留字
> 日期：2026-07-18
> 状态：草案（待审核）

---

## 一、背景与定位

### 1.1 当前并发模型

| 语句 | 执行模型 | 并行性 | 实现 |
|:---|:---|:---|:---|
| `sync { spawn { ... } }` | 协程（C++20 coroutine） | ❌ 单线程协作式 | [StmtGen.cpp:496-519](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L496) |
| `sync(max=N) { ... }` | 协程 + 信号量 | ❌ 单线程协作式 | [StmtGen.cpp:498-509](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L498) |
| `sync for item in ...` | 协程语法糖 | ❌ 单线程协作式 | [StmtGen.cpp:521+](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L521) |

当前 `sync` 是协程并发（`co_await when_all(tasks)`），**单线程内协作式调度**，I/O 仍然是阻塞的。

### 1.2 为什么需要独立语句

用户明确指出：
- `sync` = 协程（单线程协作式）
- `sync thread` = 多线程（OS 线程，真正并行）

**不能复用 sync 语法的原因**：
1. 协程闭包内捕获的是值副本，跨线程需要考虑数据竞争
2. 协程闭包参数用 `GcRootHandle` 注册到栈根，跨线程需要独立的 GC 根集合
3. 多线程需要 GC stop-the-world（参见 [TODO.txt §五](file:///d:/you/Aura/TODO.txt) GC 多线程暂停）
4. 多线程的异常传播机制不同（std::exception_ptr 跨线程传播 vs 协程内的 try/catch）
5. 多线程的同步原语不同（std::mutex / condition_variable vs 协程的 co_await）

### 1.3 `thread` 软关键字策略

**软关键字**（contextual keyword）：在 `sync thread` 这个特定上下文中作为关键字，其他位置仍可作为标识符。

**影响位置**：
- Lexer：不加 `TokType::KwThread`，仍按普通标识符 `TokType::Ident` 解析
- Parser：在 `parseSyncStmt` 中检测 `sync` 后是否紧跟 `thread` 标识符
- 用户代码仍可使用 `thread` 作为变量名：

```aura
let thread = 42   // ✅ 合法，thread 是软关键字
sync thread { ... }  // ✅ 这里 thread 是关键字
```

---

## 二、语法设计

### 2.1 基本语法

```aura
// 无界多线程：启动所有 spawn，等所有完成
sync thread {
    spawn (io: Io, i: int) {
        io.println("thread " + i)
    }
}

// 有界多线程：最多 N 个线程并发
sync thread(max = 8) {
    for i in 0..1000 {
        spawn (io: Io, i: int) {
            process(io, i)
        }
    }
}
```

### 2.2 与 `sync` 的对比

| 语法 | 执行模型 | 阻塞点 | GC | 异常 |
|:---|:---|:---|:---|:---|
| `sync { ... }` | 协程，co_await when_all | 单线程协作式 | 栈根 GcRootHandle | try/catch 传播 |
| `sync(max=N) { ... }` | 协程 + 信号量 | 单线程协作式 | 同上 | 同上 |
| `sync thread { ... }` | **OS 线程**，join 所有线程 | join | **每线程独立 GC 根** | **std::exception_ptr 跨线程传播** |
| `sync thread(max=N) { ... }` | OS 线程 + 信号量 | join | 同上 | 同上 |

### 2.3 `spawn` 在 `sync thread` 中的语义

`spawn` 在 `sync thread` 块中**必须创建新线程**，而不是协程。

```aura
sync thread {
    spawn (io: Io) { ... }   // → std::thread([io]() { ... })
}
```

### 2.4 不支持的组合

| 语法 | 状态 | 原因 |
|:---|:---|:---|
| `sync thread { spawn { ... } }` 中再嵌套 `sync thread` | ❌ 禁止 | 嵌套线程池管理复杂，无明确用例 |
| `sync thread` 内 `channel<T>` | ✅ 支持 | 跨线程通信的主要方式 |
| `sync thread` 内 `sync { ... }` | ✅ 支持 | 线程内可再用协程 |
| `sync thread` 内 `await` | ❌ 禁止 | await 是协程语义，线程内不能 await |

---

## 三、AST 设计

### 3.1 复用现有 `SyncStmt`，新增 `isThread` 标志

```cpp
// AST/Stmt.h
struct SyncStmt : Stmt {
    std::unique_ptr<BlockStmt> body;
    std::unique_ptr<ASTNode> maxExpr;
    bool isThread = false;  // 新增：true 表示 sync thread，false 表示 sync
    // ...
};
```

**为什么复用而不是新增节点**：
- 语法结构完全一致（body + 可选 maxExpr）
- Parser 只需在 `sync` 后检测 `thread` 标识符并设标志
- CodeGen 根据 `isThread` 分派到不同代码路径

### 3.2 Parser 修改

```cpp
// Parser/StmtParser.cpp parseSyncStmt()
// 当前：
//   'sync' ('(' 'max' '=' expr ')')? block
// 新增：
//   'sync' ('thread' ('(' 'max' '=' expr ')')?)? block  // 新分支
//   | 'sync' ('(' 'max' '=' expr ')')? block             // 旧分支

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

---

## 四、Sema 检查

### 4.1 新增检查规则

| 规则 | 说明 |
|:---|:---|:---|
| **R1**: `sync thread` 不能嵌套 `sync thread` | 编译错误："nested sync thread not allowed" |
| **R2**: `sync thread` 内禁止 await | 编译错误："cannot use await inside sync thread block" |
| **R3**: `sync thread` 内的 `spawn` 必须显式传参 | 复用 [concurrency_lang_spec.md §1.3](file:///d:/you/Aura/plan/concurrency_lang_spec.md) 的 spawn 检查规则 |
| **R4**: `sync thread(max=N)` 中 N 必须是编译期常量或运行时正整数 | 编译错误："max must be positive integer" |

### 4.2 Sema 实现位置

```cpp
// Sema/Checker/StmtChecker.cpp checkSyncStmt
void SemAnalyzer::checkSyncStmt(const SyncStmt& s) {
    if (s.isThread) {
        if (inSyncThreadBlock_) {
            error(s, "nested sync thread not allowed");
            return;
        }
        // 设置标志，检查内部语句
        bool oldInSyncThread = inSyncThreadBlock_;
        inSyncThreadBlock_ = true;
        checkBlock(*s.body);
        inSyncThreadBlock_ = oldInSyncThread;

        // 检查 maxExpr 类型
        if (s.maxExpr) {
            auto t = inferExpr(*s.maxExpr);
            if (!isIntType(*t)) error(*s.maxExpr, "max must be integer");
        }
        return;
    }
    // ... 原有 sync 检查逻辑
}
```

---

## 五、CodeGen 设计

### 5.1 `sync thread` 的生成代码

**无界版本**（`sync thread { ... }`）：

```cpp
// Aura 源码：
sync thread {
    spawn (io: Io, i: int) { io.println(i) }
}

// 生成的 C++：
{
    std::vector<std::thread> _threads;
    std::vector<std::exception_ptr> _excs;
    std::mutex _excs_mutex;
    // spawn 语句生成：
    _threads.emplace_back([io, i]() {
        try {
            // spawn body
            io.println_sync(i);  // 注意：thread 内必须用 _sync 版本
        } catch (...) {
            std::lock_guard<std::mutex> lock(_excs_mutex);
            _excs.push_back(std::current_exception());
        }
    });
    // sync thread 块结束
    for (auto& t : _threads) t.join();
    // 异常聚合
    if (!_excs.empty()) {
        std::rethrow_exception(_excs[0]);
    }
    aura_rt::gc_safepoint();
}
```

**有界版本**（`sync thread(max=N) { ... }`）：

```cpp
// 生成：
{
    std::counting_semaphore<N> _sem(N);  // C++20
    std::vector<std::thread> _threads;
    std::vector<std::exception_ptr> _excs;
    std::mutex _excs_mutex;

    // 每个 spawn 前 acquire，线程结束后 release
    _sem.acquire();
    _threads.emplace_back([io, i, &_sem]() {
        struct SemGuard {
            std::counting_semaphore<>* s;
            ~SemGuard() { s->release(); }
        } guard{&_sem};
        try {
            io.println_sync(i);
        } catch (...) {
            std::lock_guard<std::mutex> lock(_excs_mutex);
            _excs.push_back(std::current_exception());
        }
    });

    for (auto& t : _threads) t.join();
    if (!_excs.empty()) std::rethrow_exception(_excs[0]);
    aura_rt::gc_safepoint();
}
```

**注意**：`N` 如果是运行时值，不能用 `std::counting_semaphore<N>`（编译期常量），需要改用 `std::counting_semaphore<>`（默认 max=INT_MAX）+ `release/acquire` 控制：

```cpp
std::counting_semaphore<> _sem(0);  // 初始 0
// spawn 前：
while (_sem.try_acquire()) {}  // 等到 acquire 成功表示有 slot
_sem.release(1);  // 预占 slot
// 线程结束后 _sem.release(1) 表示归还 slot
```

实际上更简单的方案是用 `std::atomic<int> running{0}` + `std::condition_variable`：

```cpp
std::atomic<int> _running{0};
const int _max = N;
std::condition_variable _cv;
std::mutex _cv_m;
// spawn 前：
{
    std::unique_lock<std::mutex> lk(_cv_m);
    _cv.wait(lk, [&]{ return _running < _max; });
    _running++;
}
// 线程结束：
{
    std::lock_guard<std::mutex> lk(_cv_m);
    _running--;
}
_cv.notify_one();
```

### 5.2 `spawn` 在 `sync thread` 中的生成代码

当前 [StmtGen.cpp](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp) `genSpawnStmt` 生成 `task<void>` 并 push 到 `_tasks`，在 `sync thread` 中需要分派到不同代码路径。

**修改**：在 CodeGenerator 中新增 `inSyncThreadBlock_` 标志，`genSpawnStmt` 检查该标志：

```cpp
// CodeGen/CodeGen.h
class CodeGenerator {
    bool inSyncThreadBlock_ = false;  // 新增
    // ...
};

// CodeGen/StmtGen.cpp genSpawnStmt
void CodeGenerator::genSpawnStmt(std::ostream& cpp, const SpawnStmt& stmt) {
    if (inSyncThreadBlock_) {
        genSpawnAsThread(cpp, stmt);   // 新分支
    } else {
        genSpawnAsCoroutine(cpp, stmt);  // 原有逻辑
    }
}

void CodeGenerator::genSpawnAsThread(std::ostream& cpp, const SpawnStmt& stmt) {
    // 生成 _threads.emplace_back([captures]() { ... })
    // 关键：捕获列表使用值副本（spawn 参数已经声明为值传递）
    // 关键：必须用 _sync 版本的 io 方法
    // 关键：必须注册线程局部 GC 根
}
```

### 5.3 关键约束

#### 约束 1：`sync thread` 内的 `io.xxx` 必须用 `_sync` 版本

**原因**：`io.println` 返回 `task<void>`，需要 `co_await`；但 `sync thread` 内不能 `co_await`，所以必须用 `io.println_sync`。

**实现**：在 `genSpawnAsThread` 中强制 `ioSync_ = true`（仅在此闭包范围内）。

#### 约束 2：GC 根必须线程局部

**问题**：当前 [gc.h](file:///d:/you/Aura/runtime/gc.h) 的 `GcRootHandle` 是 thread_local 还是全局？需要确认。

**设计**：`GcRootHandle` 必须是 `thread_local`，每个线程的 GC 根独立管理。GC 在 stop-the-world 时需要遍历所有线程的根。

**新增**：`GcHeap::registerThread()` / `unregisterThread()`，在线程启动/退出时调用，用于 GC 追踪所有活跃线程。

#### 约束 3：`channel<T>` 必须线程安全

**设计**：`channel<T>` 的实现需要：
- `std::mutex` 保护环形缓冲
- `std::condition_variable` 用于 send/receive 挂起
- 关闭时唤醒所有等待的线程

---

## 六、运行时支持

### 6.1 新增运行时类型

```cpp
// runtime/sync_thread.h（新增）
namespace aura_rt {

// 多线程同步块上下文
class sync_thread_context {
public:
    explicit sync_thread_context(int max = 0);  // 0 = 无界
    ~sync_thread_context();

    // 注册一个线程函数（返回 void）
    template <typename F>
    void spawn(F&& f);

    // 等待所有线程完成，聚合异常
    void wait_all();

private:
    int max_;
    std::atomic<int> running_{0};
    std::condition_variable cv_;
    std::mutex cv_m_;
    std::vector<std::thread> threads_;
    std::vector<std::exception_ptr> excs_;
    std::mutex excs_m_;
};

} // namespace aura_rt
```

### 6.2 GC 集成

```cpp
// runtime/gc.h 新增
class GcHeap {
public:
    // 线程注册（用于 GC stop-the-world）
    void registerThread(std::thread::id id);
    void unregisterThread(std::thread::id id);

    // GC 暂停时：通知所有注册线程进入 safepoint
    void requestSafepointAll();
    void waitForAllSafepoints();

private:
    std::mutex threads_m_;
    std::vector<std::thread::id> registered_threads_;
    std::atomic<bool> gc_pending_{false};
    std::condition_variable safepoint_cv_;
    std::atomic<int> stopped_threads_{0};
};
```

---

## 七、实施步骤

### Phase 1: AST + Parser（不动 CodeGen，仅解析）

| Step | 内容 | 文件 |
|:---|:---|:---|
| 1.1 | `SyncStmt` 加 `bool isThread = false` 字段 | [AST/Stmt.h:218-229](file:///d:/you/Aura/src/AST/Stmt.h#L218) |
| 1.2 | `parseSyncStmt` 检测 `thread` 软关键字 | [Parser/StmtParser.cpp](file:///d:/you/Aura/src/Parser/StmtParser.cpp) |
| 1.3 | `print` 方法支持打印 `sync thread` | [AST/Stmt.cpp](file:///d:/you/Aura/src/AST/Stmt.cpp) |
| 1.4 | 验证：解析后 AST 结构正确 | - |

**验收**：能解析 `sync thread { ... }` 和 `sync thread(max=N) { ... }`，AST 输出正确。

### Phase 2: Sema 检查

| Step | 内容 | 文件 |
|:---|:---|:---|
| 2.1 | `SemAnalyzer` 加 `inSyncThreadBlock_` 标志 | [Sema/SemAnalyzer.h](file:///d:/you/Aura/src/Sema/SemAnalyzer.h) |
| 2.2 | `checkSyncStmt` 分派 `isThread` 分支 | [Sema/Checker/StmtChecker.cpp](file:///d:/you/Aura/src/Sema/Checker/StmtChecker.cpp) |
| 2.3 | 实现 R1-R4 规则 | 同上 |
| 2.4 | spawn 在 sync thread 中的参数检查 | [Sema/Checker/StmtChecker.cpp](file:///d:/you/Aura/src/Sema/Checker/StmtChecker.cpp) |

**验收**：嵌套 sync thread 报错；sync thread 内 await 报错。

### Phase 3: 运行时支持

| Step | 内容 | 文件 |
|:---|:---|:---|
| 3.1 | 实现 `sync_thread_context` 类 | runtime/sync_thread.h（新增） |
| 3.2 | GC 加 `registerThread` / `unregisterThread` | [runtime/gc.h](file:///d:/you/Aura/runtime/gc.h) + [runtime/gc.cpp](file:///d:/you/Aura/runtime/gc.cpp) |
| 3.3 | GC stop-the-world 机制（safepoint） | 同上 |
| 3.4 | `GcRootHandle` 改为 thread_local（如非已实现） | [runtime/gc.h:43-62](file:///d:/you/Aura/runtime/gc.h#L43) |

**验收**：单元测试多线程 + GC 不 crash。

### Phase 4: CodeGen

| Step | 内容 | 文件 |
|:---|:---|:---|
| 4.1 | CodeGenerator 加 `inSyncThreadBlock_` 标志 | [CodeGen/CodeGen.h:403](file:///d:/you/Aura/src/CodeGen/CodeGen.h#L403) |
| 4.2 | `genSyncStmt` 分派 `isThread` 分支 | [CodeGen/StmtGen.cpp:496-519](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L496) |
| 4.3 | `genSpawnStmt` 分派到 `genSpawnAsThread` | [CodeGen/StmtGen.cpp](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp) |
| 4.4 | `genSpawnAsThread` 生成 `std::thread` + 异常聚合 | 同上 |
| 4.5 | sync thread 块内强制 `ioSync_ = true` | 同上 |

**验收**：编译以下 Aura 代码并运行：

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

### Phase 5: channel<T> 跨线程

| Step | 内容 |
|:---|:---|
| 5.1 | 实现 `aura_rt::Channel<T>`（mutex + cv） |
| 5.2 | Parser + Sema + CodeGen 支持 `channel<T>` 类型 |
| 5.3 | `for val in ch` 在 sync thread 中生成阻塞 receive 循环 |

**验收**：生产者-消费者模式跨线程工作。

---

## 八、待决策事项

| # | 问题 | 选项 | 建议 |
|:---:|:---|:---|:---|
| Q1 | `sync thread` 内能否调用返回 `task<T>` 的函数？ | A. 禁止 B. 自动用 `.sync()` 调用 | A（简单，避免隐式行为） |
| Q2 | `sync thread` 内的 `spawn` 是否必须显式传参？ | A. 必须 B. 允许隐式捕获 | A（与 [concurrency_lang_spec.md](file:///d:/you/Aura/plan/concurrency_lang_spec.md) 一致） |
| Q3 | `sync thread(max=N)` 的 N 运行时值还是编译期常量？ | A. 运行时 B. 编译期 | A（运行时更灵活，用 atomic+cv 实现） |
| Q4 | 多线程的 GC 暂停是否阻塞在 `sync thread` 结束？ | A. 是 B. 任意时刻 | A（sync thread 块结束前不触发 GC，简化实现） |
| Q5 | `sync thread` 是否支持 `return` / `break`？ | A. 禁止 B. 支持 | A（与 sync 块一致，sync 块内也不支持 return） |

---

## 九、风险与限制

### 9.1 主要风险

1. **GC 多线程支持**：当前 GC 单线程运行，[TODO.txt §五](file:///d:/you/Aura/TODO.txt) 中"多线程 GC 暂停"（P1）是前置条件
2. **GcRootHandle 跨线程**：需要确认 [gc.h:43-62](file:///d:/you/Aura/runtime/gc.h#L43) 的 `GcRootHandle` 是否 thread_local
3. **io._sync 版本完整性**：所有 io 方法都已有 `_sync` 版本（✅ 已确认），但 list_dir_sync 返回 `Array<Path>*` 是 GC 对象，跨线程传递需注册根
4. **数据竞争**：Aura 没有引用类型 / 可变全局变量，spawn 参数是值副本，但 GC 对象通过指针传递，多线程修改同一对象会竞争

### 9.2 明确限制

- ❌ 不支持 `sync thread` 嵌套
- ❌ 不支持 `sync thread` 内 `await`
- ❌ 不支持 `sync thread` 内调用返回 `task<T>` 的函数
- ❌ 不支持 `sync thread` 内 `return` / `break` / `continue`
- ❌ `sync thread` 块内的 GC 暂停只在块结束时触发（非并发 GC）

---

## 十、与 TODO.txt 的关系

实施本草案前，必须先完成 [TODO.txt §五](file:///d:/you/Aura/TODO.txt) 中的：
- `[~] P0  GC 实际运行 — 根集合空转问题`
- `[ ] P1  多线程 GC 暂停（stop-the-world）`

本草案完成后，[TODO.txt §一](file:///d:/you/Aura/TODO.txt) 应新增项：

```
[ ] P1  sync thread 多线程语句
      - 草案：plan/sync_thread_plan.md
      - 前置：GC 多线程暂停（P1）+ GcRootHandle thread_local 化
      - 文件：src/AST/Stmt.h, src/Parser/StmtParser.cpp,
              src/Sema/Checker/StmtChecker.cpp, src/CodeGen/StmtGen.cpp,
              runtime/sync_thread.h (新增)
```

---

## 十一、总结

`sync thread` 作为独立的并行执行语句，与 `sync` 协程模型清晰分离：

- **`sync`**：协程，单线程协作式，`co_await when_all`，适合 I/O 密集
- **`sync thread`**：OS 线程，真正并行，`std::thread::join`，适合 CPU 密集

**关键设计决策**：
1. `thread` 是软关键字，不影响现有代码
2. 复用 `SyncStmt` + `isThread` 标志，最小化 AST 改动
3. `sync thread` 内的 `spawn` 必须生成 `std::thread` 而非 `task<void>`
4. `sync thread` 内的 `io.xxx` 强制用 `_sync` 版本
5. GC 多线程暂停是硬前置条件

**ROI 评估**：
- 改动规模：~500 行（AST/Parser 50，Sema 80，CodeGen 200，运行时 150，测试 50）
- 收益：让 Aura 真正具备多核并行能力
- 前置依赖：GC 多线程暂停（P1，本身需要 ~300 行改动）
