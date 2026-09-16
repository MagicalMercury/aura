---
type: bug_report
module: CodeGen
sub_module: "接口默认方法体内 spawn/sync 整体不可用（ioInScope_ 从未设置 / sync 静默产出坏 C++）"
status:
  - pending_fix
severity:
  - medium
discover_date: 2026-09-16
related_issues:
  - "[[feature-12-callable-reserved-domains-migration]]"
  - "[[bug-83-old-path-f0-forwarding-param-residue]]"
tags:
  - feature-12
  - codegen
  - interface-default-method
  - spawn-sync
  - bad-cpp
  - silent-corruption
---

# 【接口默认方法体内 spawn/sync 不可用】`spawn` 报 "requires an 'io' variable"；`sync` 无诊断直接产出坏 C++（`co_await` 出现在非协程函数）

**状态：部分修复（2026-09-16）**
- ✅ **`sync` 子项已修复** —— 见文末「## 8. 修复记录（sync 侧）」（feature-12 批次 3 · 5.1b）
- ⏳ **`spawn` 子项待处理** —— 属「spawn 语义改动」独立课题（词法约束 → 动态上下文约束）


> **一句话摘要**：接口默认方法体（`DeclGen.cpp` 的 `m.defaultBody` 分支）**从不设置
> `ioInScope_`**（该状态的全部 4 处设置点都在 `DeclFun.cpp`）→ 体内 `spawn` 直接报
> 「spawn requires an 'io' variable」；而体内 `sync` **绕过了 `refsIo` 守卫**，
> CodeGen 无诊断通关，产物发射 `co_await` 而所在函数**不是协程** → g++ 报
> `unable to find the promise type for this coroutine`。

## 1. 调研背景与发现
- **发现时间**：2026-09-16，feature-12 批次 3 · 5.1（接口 receiver 迁移）实施期的 Phase 0 探针。
- **发现方式**：子 Agent 按简报 §三「Phase 0 前置探针」实测三态（探针文件 `example/_probe_p1.aura` ~ `p3.aura`）。
- **原始动机**：5.1 要评估「接口默认方法内 spawn/sync 捕获 receiver」的可行性与处置方式
  （GLM 交叉裁决时提出的 Q3）。探测中发现：**根本走不到 receiver 捕获那一步**——
  spawn/sync 本身在该上下文就不可用。

## 2. 复现（两个独立形态）

### 2.1 `spawn`（P1 / P2）
```aura
interface Named {
    name() -> string
    work(io: Io) -> int {
        sync thread {
            spawn (io: Io) {
                io.println("task")
            }
        }
        return 1
    }
}
```
→ `error: codegen: spawn requires an 'io' variable in the enclosing scope;
   add an 'io: Io' parameter to the enclosing function`

**两个变体（`sync thread{...}` 与 `sync{...}`）报同一错误** → 与 `sync` 写法无关。

### 2.2 `sync`（P3）—— **静默坏码，更危险**
```aura
interface Named {
    name() -> string
    work() -> int {
        sync {
            let x = 1
        }
        return 1
    }
}
```
→ `aurac` **rc=0（无诊断！）**，但生成的 C++ 里：
```cpp
co_await aura_rt::when_all(_tasks);
co_await [&]() -> auto { ... };
```
而所在函数**不是协程**（接口视图成员函数，无 `task<>` 返回类型）→ g++：
```
error: unable to find the promise type for this coroutine
```

## 3. 根因

### 3.1 `spawn` 侧：`ioInScope_` 从未在接口默认方法分支设置

`ioInScope_` 的全部设置点（实测 grep）：

| 位置 | 场景 |
|---|---|
| `DeclFun.cpp:167-169` | 普通函数形参含 `io` |
| `DeclFun.cpp:589-591` | 普通方法形参含 `io` |
| `DeclFun.cpp:732` | 方法级复位 |
| `DeclFun.cpp:796-817` | ctor 形参含 `io` |
| `StmtSpawn.cpp:96-98` / `StmtSync.cpp:300-302` | spawn/sync 内部 save/restore |

**`src/CodeGen/DeclGen.cpp` 的接口默认方法分支（`if (m.defaultBody)`，L252-266）
完全没有设置** → 体内 `ioInScope_` 恒为 `false` → `StmtSpawn.cpp:75` 的
`if ((hasIo || bodyRefsIo) && !ioInScope_)` 命中 → 干净报错。

**⚠️ 连带影响**：该分支也未设置 `currentReceiverCppType_`（2026-09-15 由批次 3 · 5.1
修复）、未设置 `currentMethodThisHandle_` —— 说明**接口默认方法体生成分支整体缺少
"方法体氛围"初始化**，`ioInScope_` 只是其中一例。

### 3.2 `sync` 侧：守卫条件不覆盖该形态

`StmtSync.cpp:145` / `:267` 有 `if (ioUsed && !ioInScope_)` 之类的守卫，但
**纯 `sync { }`（不引用 io）不触发** → 直接生成 `co_await`。
而接口视图成员函数**不是 C++ 协程**（`co_await` 无 promise type）→ 坏码。

**这是"静默坏码"**（CodeGen 无诊断，直到 g++ 才炸）——比 `spawn` 的干净报错更危险，
因为它逃过了 CodeGen 层所有守卫。

## 4. 影响面
- **存量用例**：`used/1-6`、`f07_verify` 68 个、`builtins/*.aurai` 中
  **未发现**接口默认方法体内使用 spawn/sync 的用法（已实测：这些用例全过）→ **存量影响 ≈ 0**。
- **潜在影响**：任何**新写**的接口默认方法若使用并发语句，会撞上：
  - `spawn` → 困惑的报错（明明形参有 `io`，却提示"add an io: Io parameter"）；
  - `sync` → **编译期才炸的坏码**（如果用户不编译到 C++ 层就发现不了）。
- **与 5.1 的关系**：5.1 的 S4（9 处 `_sp_this` 拼接点加诊断）**因本缺陷而降级** ——
  接口 receiver 根本到不了那些拼接点（被 `ioInScope_` 先拦住）。
  实测结论：**`currentReceiverCppType_` 对 spawn/sync 的 9 处消费点，在接口 receiver 下全部不可达**。

## 5. 修复方案（建议，未实施）

### 方案 A（推荐）：补全接口默认方法分支的「方法体氛围」
在 `DeclGen.cpp:252-266` 的 `if (m.defaultBody)` 分支，参照 `DeclFun.cpp:589-591` 补：
- `ioInScope_`：遍历 `m.params` 找名为 `io` 的形参（与普通方法同款逻辑）；
- **但需先确认**：接口视图成员函数**能否承载协程**（`sync` 需要函数是 `task<>`）。
  → 若不能，则 `sync` 应改为**干净报错**（而非支持），`spawn` 同理。

### 方案 B（最小改动）：把两个形态都变成**干净诊断**
- `spawn`：现有报错已干净，但**文案误导**（提示加 io 形参，而加了也没用）→ 改文案为
  「接口默认方法内暂不支持 spawn」；
- `sync`：在 `StmtSync.cpp` 补守卫 —— 检测「当前函数非协程」（如 `currentFunctionIsCoroutine_` 为假）
  → `error()` 干净报错。**这是本缺陷最危险的子项（静默坏码），建议优先修**。

**推荐**：先做 **B**（消除静默坏码 + 修正误导文案），A 待「接口默认方法是否支持并发」这个
语言层决策明确后再做。

## 6. 关联
- `issues/bugs/bug-83-*.md`（同属「方法体氛围/分流判据遗漏」族；bug-83 已 fixed）
- `change.md` 批次 3 · 5.1（§5.1 接口 receiver 迁移）—— 本缺陷是其 Phase 0 探针的副产品
- 探针文件：`example/_probe_p1.aura` ~ `_probe_p4.aura`（**中文注释已 mojibake，仅作字节级证据**）
- 5.1 实施中 S4 的降级依据：`scripts/f12_batch3_5.1_impl_brief.md` §三

## 7. 备注
- ⚠️ 本缺陷**不是** feature-12 引入：`DeclGen.cpp` 的接口默认方法分支与
  `ioInScope_` 机制均为 feature-07 之前既有（`DeclFun.cpp:167` 的 `#46` 注释为证）。
- 登记依据：主 Agent 复核子 Agent 探针（`_probe_p1/p3`）+ `ioInScope_` 全仓 grep 实测。

---

## 8. 修复记录（sync 侧，2026-09-16，feature-12 批次 3 · 5.1b）

> **语义裁定（主人）**：「方法里面确实应该允许 `sync` 块出现」。
> 故 `sync` 子项**不是**改为"干净报错"（原 §5 方案 B 的一部分），而是**让它能跑**。

### 8.1 根因回顾（sync 侧）

接口默认方法在视图 struct 内生成**普通成员函数**（非协程），
但 `sync` 的产物含 `co_await _sync.wait_all()`（`StmtSync.cpp:29`）
→ 非协程函数里 `co_await` 非法 → g++ `unable to find the promise type for this coroutine`。

**关键**：`decideCoro` 的固定点迭代（`CodeGen.cpp:137-159`）**只扫 `FunDecl`/`MethodDecl`**，
**不扫 `InterfaceDecl` 的默认方法** → 接口默认方法**从未被标为协程**。

### 8.2 修复（5 文件）

| # | 文件 | 内容 |
|---|---|---|
| S1 | `CoroDecide.cpp` + `CodeGen.h` | 新增 `bool decideCoro(const BlockStmt&)`（复用 `CoroScanner`，与具名函数/方法**同源判据**）|
| S2 | `DeclGen.cpp` | 签名按 `decideCoro(*m.defaultBody)` 协程化（`task<R>`）；`genBlock(..., isCoroutine=ifaceCoro)`（原硬编码 `false`）|
| S3 | `CodeGen.cpp` | 固定点迭代**新增接口默认方法登记**（键 `IfaceName.methodName`，与 `methodDefaultArgs_` 同格式）→ 使协程**传染**生效 |
| S4 | `ExprMethodCall.cpp` | 调用点 `recvKey` 推导**新增 `InterfaceSemType` 分支**（`is->name`，与同文件 L463 同源）→ `co_await` 解包 |
| S5 | `test_codegen_closure.cpp` | 新增 2 条单测（见 §8.3）|

### 8.3 验证证据（主 Agent 独立复跑）

| 项 | 结果 |
|---|---|
| **`example/p_sync2.aura`**（接口默认方法内 sync）| ✅ **compile=0 + 输出 `1`** |
| **产物三重检查** | ✅ `aura_rt::task<int32_t> work()`（签名协程化）<br>✅ `co_return t;`（非 `return t;`）<br>✅ `co_await n.get().work()`（调用点解包）|
| **单测** | ✅ **1322 tests / 1322 passed / 0 failed**（+2 新增）|
| 新增单测 ① `InterfaceDefaultMethodWithSyncIsCoroutineized` | 含 sync → 断言 `task<int32_t> work()` + `co_return` + `co_await` |
| 新增单测 ② `InterfaceDefaultMethodWithoutSyncStaysPlain` | **反向断言**：不含 sync → **不得**协程化（防判据过宽）|
| `used/1-6.aura` | ✅ 全过 |
| 5.1 成果（`_repro/f12_batch3/iface_local_closure` / `iface_escape_closure`）| ✅ 未回归（exit=0）|
| `bug81_defcb.aura` | ✅ exit=0 |
| `f07_verify` | ✅ 68 通过 / 4 失败（4 个 Sema 类既有，与基线一致）|
| **`spawn` 未受影响** | ✅ `ioInScope_` 未在 `DeclGen.cpp` 出现（未碰 spawn 路径）|

### 8.4 剩余（spawn 侧）

`spawn` 子项**仍待处理**，且**不是**简单的"补 `ioInScope_`" ——
主人的语义裁定是：**`spawn` 可写在任意位置，约束改为「必须被 sync 块动态调用」**
（编译期调用图分析 + 运行时上下文检查）。这是**语言语义级改动**，需独立立项设计。

→ 详见主 Agent 将另写的《spawn 同步上下文约束设计草案》。
