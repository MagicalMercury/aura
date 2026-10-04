---
type: bug_report
module: CodeGen
sub_module: StmtSpawn.cpp 线程版块形态（:706-728）
status:
  - blocked
severity:
  - medium
discover_date: 2026-10-03
related_issues:
  - "[[bug-98-spawn-call-form-silently-does-nothing]]"
  - "[[bug-99-sync-block-bare-coroutine-call-statement-dropped]]"
tags:
  - sync-thread
  - spawn
  - block-form
---

# 【sync thread 块形态 spawn 体内 let 绑定协程调用】生成码把 `task<T>` 绑进 `T` ⇒ 坏 C++（bug-99 族；与 bug-98 未覆盖的第三条入口同源）

> **登记来源**：2026-10-03 bug-93/94/97 实施批**线程块形态探针**实测转红（`scripts/fix_b939497_impl_report.md` §2.3），主 Agent 复核后登记。
> **一句话摘要**：`sync thread { spawn (io) { let r = worker(io, i) } }`（`worker` 含 io ⇒ 判为协程）中，线程版 spawn 的 body 被生成为**非协程** lambda（`genStmt(cpp, *s, false)`），`let r = <协程调用>` 于是被生成成 `int32_t r = <task<int>>` ⇒ **g++ 编译失败**（`cannot convert 'aura_rt::task<int>' to 'int32_t'`）。`aurac` 本身 `rc=0`（只跑一步看不出来）。

## 1. 调研背景与发现（Background）

- **发现时间**：2026-10-03，bug-93 关单判定复核时按 `change.md §1.2` 边界注记补跑线程块形态探针。
- **触发场景**：
  ```aura
  fun worker(io: Io, id: int) -> int {
      io.println("  worker " + str(id))
      return id
  }
  fun main(io: Io) throws {
      sync thread(max = 2) {
          for i in range(2) {
              spawn (io: Io, i: int) {
                  let r = worker(io, i)      // ← 缺陷点：非协程体内绑定协程调用
                  io.println("  r=" + str(r))
              }
          }
      }
      io.println("tb ok")
  }
  ```
- **影响范围**：`sync thread` 内**块形态** `spawn (…) { … }` 的 body **含 `let` 绑定协程调用**（且该协程被判为协程）；同类「非协程体里绑定/使用协程返回值」的形态均受影响。

## 2. 根因分析（Root Cause Analysis）

> **关键链条**：`spawn` 有**四个形态**（协程块 / 协程调用 / 线程调用 / **线程块**）。`bug-98` 的 `insideSyncBlock_` save/置 `false`/restore 只覆盖**前两个**；**线程块形态**（`src/CodeGen/StmtSpawn.cpp:706-728`）入口段**没有**同款处理（该区间 `insideSyncBlock_` 命中 = **0**），且其 body 以 `genStmt(cpp, *s, false)`（注释：**非协程！**）生成 ⇒ 协程调用返回的 `task<T>` 被直接绑进 `T`。

### 2.1 代码路径追踪

- **Parser 端**：不涉及（`spawn (args) { body }` 块形态语法已就位）。
- **Sema 主根因**：不涉及（Sema 不产生调用边；`io` 含 io ⇒ 判协程的判据在 CodeGen `CoroDecide.cpp:189 return !ioSync_;`）。
- **CodeGen 相关路径（主战场）**：
  - `src/CodeGen/StmtSpawn.cpp:706-728` —— 线程版块形态 body 生成段；`:708 insideSpawn_ = true;`、`:725-727 for (auto& s : stmt.body) if (s) genStmt(cpp, *s, false);  // 非协程！`
  - 对照（已修的协程两形态）：`src/CodeGen/StmtSpawn.cpp:105` / `:283` —— bug-98 加的 `insideSyncBlock_ = false;`（入口）+ `:155` / `:308`（出口 restore）。
  - `src/CodeGen/CodeGen.h:1316` —— `isDeferredCoroutineCall() { return insideSyncBlock_; }`（判据入口）。
- **其他端**：不涉及。

### 2.2 关键逻辑细节

- **时机问题**：线程版 body = **非协程 lambda**（`genStmt(..., false)`）⇒ lambda 内**不能**出现 `co_await`；但 `let r = <协程调用>` 需要的正是「驱动协程并取回 `T`」⇒ 现状既没驱动也没解包。
- **设计意图**：`sync thread` 是**真线程**执行环境（`runtime/thread_pool.cpp` worker；worker **无线程事件循环**）⇒ 该形态的正确语义应与 `sync` 块内的「调用点立即 `co_await`」对齐，或由该形态的语义裁定给出（归属 `[[bug-99]]` 族）。

## 3. 影响范围（Scope）

- **结论**：`sync thread` 内**块形态** spawn 的 body **含 `let` 绑定协程调用** ⇒ 编译期失败（干净报错，**非静默**）：`error: cannot convert 'aura_rt::task<int>' to 'int32_t' in initialization`。
- **不受影响**：① 线程版 spawn body 内**裸调用**不绑定（不产生 `task<T>` 到 `T` 的转换）；② 协程块/协程调用两形态（bug-98 已修）；③ 非协程调用（无 `task<T>` 包装）。
- **严重度 medium**：编译期暴露、不静默；但形态正当（`worker` 只是含 io 的普通函数），用户无 workaround（只能改成裸调用或换 `sync` 块）。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 测试场景描述 | 预期结果（修复后） | 当前实际结果（修复前） | 状态/备注 |
| :--- | :--- | :--- | :--- | :--- |
| `$TMP/b93_tb.aura`（探针，仓库外） | `sync thread(max=2)` + `for` + `spawn (io,i)` 体内 `let r = worker(io,i)` | `aurac rc=0` + `g++ rc=0`，运行输出含 `r=0`/`r=1`/`tb ok` | `aurac rc=0`（不查 C++ 语义）；**`g++ rc=1`**：`b93_tb.cpp:73:4: error: cannot convert 'aura_rt::task<int>' to 'int32_t'` | ❌ 未修（本笔记登记） |
| 对照：`scripts/f18_p2_BatchD_cases/_p1/_p4`（协程两形态） | `sync` 块 spawn 体内 `let r = <协程调用>` | `g++ rc=0` | `g++ rc=0`（bug-98 已修） | ✅ |
| 对照：`scripts/_b939497/b94*`（多文件） | 与本缺陷无关 | — | — | 不适用 |

**复现命令**（**必须两步**，只跑 `aurac` 看不出）：
```bash
cd /d/you/Aura && export PATH=/c/msys64/ucrt64/bin:$PATH
./build/aurac.exe <探针>.aura --cpp $TMP/b93_tb.cpp -S     # rc=0
g++ -std=gnu++20 -fsyntax-only -w -I runtime $TMP/b93_tb.cpp   # ⇒ rc=1（缺陷在此暴露）
```

## 5. 修复方案（Fix Plan）

- **修复位置（待裁定，两条路线）**：
  - **路线 A（本形态就地对齐）**：`src/CodeGen/StmtSpawn.cpp:706-728` 线程版块形态入口/出口补 `insideSyncBlock_` save/置 `false`/restore（与 bug-98 两形态同款）+ 为 body 内「绑定协程调用」生成**解包形态**（该形态无 `co_await` 上下文 ⇒ 需要按「就地驱动 + 取值」生成，参照 `run_to_completion` 或 `_sync` 通路）。
  - **路线 B（随 feature-19 消解）**：`[[feature-19-io-offload-decoupling]]` 让**含写型 io 的函数退回普通函数** ⇒ 探针里 `worker` 不再是协程 ⇒ 该形态自然合法。**纯计算协程**在该形态下仍需路线 A。
- **修复逻辑**：**未定**（本笔记只登记，不含实现代码——待主人裁定路线后由实施批补 §5 细节）。
- **配套修复**：`change.md §1.2` 的边界注记已记录本形态不在 bug-93 关单范围内。

## 6. 回归验证清单（Regression Checklist）

- [ ] 探针 `b93_tb.aura` 两步编译：`aurac rc=0` **且** `g++ rc=0`
- [ ] 端到端运行：输出含 `r=0` / `r=1` / `tb ok`，`rc=0`
- [ ] 对照不被误伤：`sync thread` 内 body **裸调用**（不绑定）仍 `rc=0`
- [ ] 对照不被误伤：协程块 / 协程调用两形态（bug-98 用例）保持绿
- [ ] 全量单测 `0 failed`（基线以实施时实测为准）
- [ ] 新增回归用例入 `test/codegen/`（真编译 + 真链接 + 真运行）

## 7. 附加资源与产物

- **复现目录**：探针落 `$TMP/b93_tb.aura`（仓库外临时；建议实施批移入 `scripts/_b939497/b93/`）
- **留存产物**：`$TMP/b93_tb.cpp`（生成码）、`$TMP/b93_tb.err`（g++ 报错原文）
- **关联报告**：`scripts/fix_b939497_impl_report.md` §2.3（发现过程 + 实测原文）、§9.2 ①（未闭合项）
- **关联笔记**：`[[bug-98-spawn-call-form-silently-does-nothing]]`（同族两形态已修）、`[[bug-99-sync-block-bare-coroutine-call-statement-dropped]]`（族主）、`[[feature-19-io-offload-decoupling]]`（路线 B）
- **代码位置**：`src/CodeGen/StmtSpawn.cpp:706-728`（线程版块形态）、`:105/:155/:283/:308`（bug-98 已修两形态）

---

**当前状态**：`2026-10-03` 已登记（探针实测转红，未修；路线待主人裁定）
