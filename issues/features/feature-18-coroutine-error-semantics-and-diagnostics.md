---
type: feature_request
module: runtime + src/CodeGen + src/Sema
sub_module: 协程异常语义（task 异常值化）+ Error 诊断增强（协程栈解构）
status:
  - draft
  - pending_review
severity:
  - high
discover_date: 2026-09-27
discovered_by: Hermes（主人要求实测 try-catch 捕获能力时发现 bug-90；主人裁定并案为 feature）
related_issues:
  - bug-87（`try` 内 `sync` ⇒ `co_await` 落非协程 lambda）
  - bug-90（`try` 内协程调用 ⇒ `task<T>` 装不进 `variant<T, Error>`）
  - bug-88（协程 + 必然抛出体 ⇒ 补 `co_return;` 生成坏 C++；已修，§10.5 记录 task 异常机制现状）
  - change.md:252（(乙1) 语义，主人 2026-09-21 定案）
  - "[[bug-87-try-block-sync-await-noncoro-iife]]"
  - "[[bug-88-naked-throw-body-degrades-implicit-future]]"
  - "[[bug-90-try-block-coroutine-call-type-mismatch]]"
tags:
  - coroutine
  - error
  - stacktrace
  - try-catch
---

# [ ] feature-18 协程异常语义统一（Error 值化）+ Error 诊断增强（协程栈解构）

## 1. 背景与动机

### 1.1 三个症状，一个根因

| 症状 | 现状 | 出处 |
|---|---|---|
| `try { let a = coroFun(io) }` | ❌ 生成坏 C++（`task<int>` 装不进 `variant<int, Error>`）| **bug-90** |
| `try { sync { ... } }` | ❌ 生成坏 C++（`co_await` 落非协程 lambda）| **bug-87** |
| 即使能编译，`catch` 也捕不到协程异常 | ⚠️ **该症状不存在（2026-09-28 更正）**：实测 `await_resume` 抛 `Error` 值**能**被协程体 `try/catch` 捕获（`await_suspend` 抛亦可）；原「GCC 吞异常」结论源自探针缺陷（`await_suspend` 空实现 + 无驱动者 ⇒ 协程永久挂起、`await_resume` 从未执行）| `scripts/_f18/P0_report_await_resume.md`、`P0_probe5b_report.md` |

**⇒ 共同根因：Aura 的协程异常走的是 **C++ 异常**（`std::exception_ptr` + `await_resume` 里 `rethrow_exception`），
而 Aura 的 `try/catch` 捕获的是 **`aura_rt::Error` 值**（IIFE + variant 模式）—— 两条路线在 `co_await` 处交汇即断。**

**⚠️ 主人 2026-09-21 已经裁定过方向**（记录于 change.md:252 / 记忆）：
> **(乙1) 必须用 Aura 自带 `Error` 值收集 + 抛 `Error`，不得用 `std::exception_ptr`**
> 理由：C++ `exception_ptr` 重抛的形态**穿不过 Aura try-catch 边界**。

**⇒ 现状 `runtime/task.h` 恰恰用的就是 `exception_ptr` + `rethrow`，与本裁定不一致。本 feature 就是把该裁定落到实现。**

### 1.2 主人追加要求：`Error` 太单薄，缺调用栈

`runtime/types.h:254`：
```cpp
struct Error : GcObject {
    GcString* kind, * message;
    GcObject* extra;      // 无位置、无调用栈
};
```
**⇒ 目标：出错时能打印**从协程栈解构出来的调用链**（协程帧名 + 调用点位置），而不是一个孤零零的 message。**

## 2. 目标

- **(A) 协程异常值化**：task 内部不再用 C++ 异常传递；异常以 `aura_rt::Error` 值形式承载，
  由生成码在**驱动点显式检查** ⇒ `try/catch` 能真正捕获协程异常 ⇒ **bug-87 / bug-90 一并消除**。
- **(B) Error 诊断增强**：`Error` 增加**逻辑调用栈**（协程链解构），输出形如：
  ```
  Error: [config_missing] no config file
    at loadConfig     (example/app.aura:12)
    at main           (example/app.aura:30)
  ```
- **(C) 语义一致性**：`sync` 块尾驱动的 (乙1)（驱动完全部 + 记录首个 + 末重抛）改为基于 `Error` 值。

## 3. 代码地图（现状）

| 位置 | 现状 |
|---|---|
| `runtime/task.h:54` | `std::exception_ptr exception_;` |
| `runtime/task.h:73` | `unhandled_exception()` ⇒ `exception_ = std::current_exception();` |
| `runtime/task.h:129` / `:189` | `await_resume()` ⇒ `if (exception_) std::rethrow_exception(...)` — ⚠️ **2026-09-28 更正**：此处**不是**「异常被丢弃」的地方（实测 `await_resume` 抛 `Error` 值可被捕获）；真正的断点是 **`try` 体被塞进非协程 IIFE**（`co_await` 无 promise type / `task<T>` 装不进 `variant`）|
| `runtime/task.h:255` | ThreadPool workerLoop 同样 rethrow |
| `runtime/types.h:254` | `Error{kind, message, extra}` |
| `src/CodeGen/StmtTry.cpp:72-119` | 协程安全模式：IIFE + `variant<resultType, Error>`（只支持同步表达式）|
| `src/CodeGen/StmtTry.cpp:126-` | 无 setupLet 版：IIFE + `variant<monostate, Error>` |
| `src/CodeGen` sync 块尾驱动 | (乙1) 实现处（记录首个 Error + 末重抛）|

## 4. 设计草案

### 4.1 (A) task 异常值化

**核心转变**：promise 不再存 `exception_ptr`，而是存 **`aura_rt::Error` 值**（或"是否有错误 + Error"）。

- `task_promise_base::unhandled_exception()` ⇒ 不能再用 `std::current_exception()` 存裸指针；
  **改为 catch 住 `aura_rt::Error` 并存入 `error_`**（Aura 的 throw 生成的就是 `throw Error{...}` ⇒ 可捕获）。
- **`await_resume()` 不再 `rethrow`** ⇒ 而是**把错误暴露给调用点**：
  - 形式一（推荐）：awaiter 暴露 `bool has_error()` + `const Error& error()`，生成码在 `co_await` 后**显式检查**；
  - 形式二：`await_resume` 返回一个 `Result<T>`（值 或 Error），由生成码解包。
  - ⚠️ **绝不能保留"重抛 C++ 异常"**（那是 GCC 丢异常的原因）。

### 4.2 (B) 协程栈解构（本 feature 的技术亮点）

**⚠️ 为什么不能用常规 backtrace**：
1. **协程帧在堆上**（`operator new` 分配），C++ 的栈回溯看不到协程调用链；
2. 协程 `co_await` 挂起后**控制流已经切走**，原生栈帧不再对应逻辑调用关系；
3. MinGW/Windows 下 `execinfo.h`/`backtrace()` 不可用（UCRT64 无该接口）。

**⇒ 方案：用协程自己的链接关系重建"逻辑调用栈"**

- C++20 协程通过 `awaiter::await_suspend(continuation)` 建立"等待者"关系，
  `task.h:60` 的 `final_awaiter` 已持有 `continuation` 句柄 ⇒ **已有一条现成的链**。
- **补足调用点信息**：在 `task_promise_base` 增加编译期常量字段：
  ```cpp
  const char* frame_fn_;    // 协程函数名
  const char* frame_file_;  // 源文件
  int         frame_line_;  // 定义/调用点行号
  ```
  ⇒ **由 codegen 在协程函数体开头注入**（值都是字符串字面量 ⇒ **零运行时开销、无 GC 压力**）。
- **收集**：抛错时（或 Error 构造时）沿 `continuation_` 链向上遍历（**限制深度，如 32 层**，防环/防爆），
  拼成栈帧列表存入 `Error` 的新字段。
- ✅ **已由探针 1 验证并修正（2026-09-28）**：`continuation_` 在 lazy 协程上确实为空，但**逻辑栈机制不依赖它**——
  实测（`scripts/_f18/P0_probe1_report.md`）跨协程帧**全部来自「lazy 创建点的整条栈快照」**（且创建点**不能截断**）；
  continuation 链**仅在逻辑栈为空/深度 0 时**作为辅段启用（无脑拼接会导致帧顺序错乱）。

### 4.3 (B) `Error` 结构扩展

```cpp
struct Error : GcObject {
    GcString* kind;      // 既有
    GcString* message;   // 既有
    GcObject* extra;     // 既有（自定义附加字段）
    // 新增（feature-18）：
    GcString* file;      // 抛出点源文件
    int32_t   line;      // 抛出点行号
    Array<GcString*>* stack;   // 逻辑调用栈（"func (file:line)" 列表），无则空
};
```
（具体字段形态待定稿；`stack` 需考虑 GC 根注册 —— 对齐既有 `GcRootHandle` 保护模式）

### 4.4 (A) `try` 生成重构（`StmtTry.cpp`）

- 目标：`try` 体**允许包含协程语义**（协程调用、`sync` 块）。
- 方向：**放弃"非协程 IIFE + variant"**（它天然只能装同步值），改为
  **「协程体内原地生成 + 每个可失败点后显式检查 Error」**，或
  **「协程 IIFE（`task<...>`） + 显式错误检查」**。
- ⚠️ **关键约束（2026-09-28 更正）**：
  - ~~协程内 `co_await` 期间抛出的 C++ 异常捕不到~~ ❌ **该结论已被推翻**（见 §1.1 更正）：`await_resume`/`await_suspend` 抛 `Error` 均可被协程体捕获；
  - ✅ **真实约束（仍生效）**：① **catch handler 内禁止 `co_await`**（标准要求，GCC 实测拒绝）⇒ `catchBody` 必须外移到正常流程；② **`await_suspend` 已排程恢复者之后不得再抛**（⇒ 陈旧 resume，实测可 segfault）；③ **`noexcept` 的 `await_suspend` 不得抛**（⇒ `terminate`）。
  ⇒ 若"原地 try/catch"仍依赖 C++ 异常，则**本轮改造必须确保异常不跨 `co_await` 边界**（即：值传递，非异常传递）。
- **新增 Sema 兜底（可选，与 bug-87 方案 C 合流）**：若某形态短期仍不支持 ⇒ 编译期干净报错。

## 5. 优点与代价

### 优点
1. **一次改造消除三个症状**（bug-87 / bug-90 / "catch 不到协程异常"），且**落实主人 09-21 的既有裁定**；
2. **不依赖「C++ 异常跨挂起点传播」**（2026-09-28 更正口径）：值化后异常只在**同一 C++ 栈内**传播（`throw` → `unhandled_exception` 值化 → 父协程 `await_resume` 抛 → 就地捕获），
   ⇒ 不依赖编译器对「跨 `co_await` 异常传播」的实现细节（对齐 change.md §9.3「新机制语义可移植」的自举约束）。**注**：原表述「GCC 实测有丢异常行为」已证否（探针缺陷所致），但「值化 = 不采用 C++ 异常承载跨协程错误」这一设计取向不变；
3. **诊断能力质变**：从"一行 message"到"逻辑调用栈"，异步错误定位不再靠猜；
4. **字符串字面量注入 ⇒ 零运行时开销**（栈信息是编译期常量）。

### 代价与风险
1. **改动面大**：`runtime/task.h`（promise/awaiter）+ `src/CodeGen/StmtTry.cpp` + sync 块尾驱动 + `Error` 结构 + 可能的 Sema 规则；
2. **`Error` 结构扩展涉及 GC**：新增的 `GcString*`/`Array` 字段需要根注册与转移更新（历史上有 `GcRootHandle`/ViewRoot 的坑）；
3. **协程链完整性待验证**：`continuation_` 链在哪些时刻可用需探针确认（§4.2 待确认项）；
4. **兼容性**：生成的 C++ 形态变化会影响既有单测（1366 项）中的生成码断言 ⇒ 需同步；
5. **`Error` 值语义的传播成本**：值传递（拷贝）比 `exception_ptr` 重，需评估热路径（`sync` 块内大量调用）开销。

## 6. 影响面

- **runtime**：`task.h`（核心）、`types.h`（Error）、可能的 `error.h`/`task.cpp`（驱动/重抛逻辑）；
- **codegen**：`StmtTry.cpp`、sync 块尾驱动、每个协程函数的入口（注入帧信息）；
- **sema**（可选）：不支持形态的干净报错；
- **测试**：`test/rt/`（task 异常、栈解构）、`test/codegen/`（生成码断言）、`example/used/leakcheck/_repro/bug-87`+`bug-90`；
- **文档**：`READMEs/11-concurrency.md`（§11.4 隐式 future 的异常语义、新 Error 输出格式）。

## 7. 开放问题（需主人裁定 / 需探针）

> ✅ **状态（2026-09-28）：1–5 已全部裁定**（`out.md` 2026-09-27 + plan §12 的 D/E 系列 2026-09-28）；探针 5b 另补 **C-1/C-2/C-3** 三条新约束（见 §4.4 与 plan §4.7）。

1. **`await_resume` 的形态**：暴露 `has_error()/error()`（最小改动）还是返回 `Result<T>`（语义更清晰）？
2. **栈解构的触发时机**：构造 `Error` 时立即解构（成本在抛出路径，可接受）还是打印时才解构（需保留链）？
3. **无 `continuation` 的 lazy 协程**栈如何补全（是否记录"创建点"）？
4. **是否保留 C++ 异常作为兜底**（非 Aura 异常，如 `std::bad_alloc`）？
5. **bug-87 的 `try { sync { } }`** 是否在本 feature 内一并支持（`sync` 块的 `co_await` 在协程 IIFE 内是否可行）？

## 7.5 与 feature-10 / feature-13 的接口契约（2026-09-27 主人裁定：**同源两表**）

**背景**：本 feature 的逻辑栈需要「帧描述」（函数名 + 文件 + 行号），而 `feature-10`（反射元数据表）正在建**编译期符号元数据表** ⇒ 主人裁定**两者对接，同源两表**。

### 契约内容

```
feature-13 两遍扫描 → 全局符号表（编译期唯一真相源）
        ├──► [全量] FrameDesc[]        ← 本 feature（feature-18）消费
        └──► [按需裁剪] SymbolInfo[]   ← feature-10 消费（其 §4 裁剪规则不变）
```

| 项 | 归属 | 裁剪 | 说明 |
|---|---|---|---|
| `FrameDesc`（`name`/`file`/`line`）| **feature-18** | ✅ **全量** | 任何函数都可能出现在栈里，编译期不可预知 ⇒ 不可裁剪；只是字面量 + 行号，**无代码膨胀** |
| `SymbolInfo` 等反射元数据 | feature-10 | ✅ **按需**（零成本原则保持）| 含 `materialize` 生成函数 ⇒ 全量会导致**代码膨胀** |

**⚠️ 边界红线**：**不得**为了本 feature 的"全量帧元数据"而取消 feature-10 的裁剪规则 —— 两者性质不同（纯静态数据 vs 生成代码）。

### 帧表示（本 feature 侧）

```cpp
struct Frame {
    uint32_t symbolIdx;   // 指向 FrameDesc[]（或 feature-10 的同源表项）
    uint32_t callLine;    // 调用点行号（codegen 在每个调用点注入，编译期常量）
};                        // = 8B（对比原设计 {ptr,ptr,int32} = 24B ⇒ 缩小 3 倍）
```

- **`feature-10` 的 `SymbolInfo` 需补 `file` / `defLine`**（已记入 `feature-10` 笔记 **§9 增强记录**）；
- ⚠️ **注意**：栈帧行号是**动态**的（同一函数在不同调用点进入下一层），**不能只存 `defLine`** ⇒ `callLine` 必须由 codegen 在**每个调用点**注入；
- **收益**：帧缩小 3 倍 ⇒ 协程栈快照成本降 3-4 倍（实测 3.54 ns → ~1 ns 量级）；函数名在产物里只存一份。

### 7.6 多文件产物契约（2026-09-27 主人裁定：集中元数据单元）

**背景**：多文件模式下，编译器**每个模块**生成一对 `<modStem>.aura.h` / `<modStem>.aura.cpp`（`src/main.cpp:420/427`）⇒ **多 TU**；
而编译期只有**一份全局符号表**（`compileMultiFile` 的汇总段）⇒ 这两张表放哪里成为必须定契约的问题。

**裁定：生成一个独立的元数据单元（集中表）**

| 产物 | 内容 |
|---|---|
| **`aura.meta.h`**（编译器生成）| `extern const FrameDesc kFrameTable[];` / `extern const uint32_t kFrameCount;`（+ feature-10 的 `SymbolInfo[]` 等反射表）|
| **`aura.meta.cpp`**（编译器生成）| 这两张表的**唯一定义**（全程序帧描述 + 按需反射表）|
| 各模块 `<mod>.aura.cpp` | `#include "aura.meta.h"` ⇒ 共享访问 |

**命名（主人 2026-09-27 定）**：`aura.meta.h` / `aura.meta.cpp`
- 模块产物形态是 `<modStem>.aura.h`，而 **`aura.meta` 不是任何合法模块名**（模块名不含 `.`）⇒ **永不与模块产物重名**；
- 避免 `aura_meta` 被同名模块撞上的隐患。

**理由（三选一的依据）**：
1. **全局符号索引仍成立** ⇒ 汇总段统一分配编号 ⇒ 帧保持 `{symbolIdx, callLine}` = **8B**；
2. **ODR 安全** ⇒ 表只在**一个** TU 定义；
3. **跨模块栈帧可查** ⇒ 栈可能跨模块（A→B→C），所有模块都需能解析他人帧。

**⚠️ 两条红线**：
1. **表定义必须落在独立 TU，不得放头文件** —— MinGW 上 `inline` 变量多 TU 包含会 `multiple definition`
   （实测事故：`g_syncStack` 的 TLS init function 冲突，见 bug-86 排查记录）；
2. **索引在汇总段统一分配** —— 各模块自行编号会导致跨模块栈帧解析错位。

**未来扩展点（迁移信号）**：若将来做**分离编译 / 增量编译**（feature-13 明确「增量编译：明确不做」），
符号索引不再全局唯一 ⇒ 帧需改为 `{const FrameDesc* table, uint32_t idx, uint32_t callLine}`（12-16B）
或全局哈希 ID。

---

## 8. 建议的实施步骤（待审查后细化）

1. **探针阶段**：验证 `continuation_` 链的可用时刻与完整性；验证"Error 值传递"下 `try` 能捕获（不含 C++ 异常）；
2. **runtime 阶段**：`Error` 扩展 + `task_promise_base` 值化 + 帧信息字段；
3. **codegen 阶段**：协程函数入口注入帧信息；`StmtTry.cpp` 重构；sync 尾驱动改造；
4. **Sema 兜底**（可选）；
5. **验证**：全量单测 + `used/1-6` + bug-87/90 探针转正 + 新增单测（含栈解构断言）；
6. **文档**：`READMEs/11` + `change.md` 相应章节。

---
*登记：2026-09-27（主人裁定：选 B + Error 增强，合并为一个 feature）*
