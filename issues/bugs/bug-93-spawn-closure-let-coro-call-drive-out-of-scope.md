---
type: bug_report
module: src/CodeGen
sub_module: StmtSync/CodeGen — `spawn` 闭包体内 `let r = <协程调用>` ⇒ sync 收集器的驱动 `try { co_await r; }` 被生成到 lambda **之外** ⇒ 跨作用域引用 ⇒ 坏 C++
status:
  - pending_fix
severity:
  - high
discover_date: 2026-09-30
discovered_by: "Hermes（feature-18 P2 批 D 的 ASAN 验证中，用对照探针发现；本鲸 2026-09-30 独立复现并定性）"
related_issues:
  - "bug-88（同类族：naked throw body 降级隐式 future）"
  - "bug-90（同族：try 块内协程调用的生成形态）"
  - "bug-87（feature-18 P2 已修复；本缺陷与 P2 **无因果关系**，见 §5.3）"
tags:
  - codegen
  - spawn
  - sync
  - coroutine
  - scope
  - scope-break
  - bad-cpp
---

# [ ] bug-93 `spawn` 闭包体内 `let` 绑定协程调用 ⇒ sync 收集器把 `co_await <var>` 生成到 lambda 外（跨作用域 ⇒ 坏 C++）

**状态**：`[ ] 待修复`（2026-09-30 登记）
**严重度**：**high** —— **合法 Aura 代码**触发，用户看到的是 `g++` 的 `'r' was not declared in this scope`（**不是干净的 Aura 报错**）；属「生成坏 C++」类，与 bug-87/bug-90 同族。

---

## 0. 一句话摘要

在 `spawn` 闭包体内用 `let` **绑定**一次协程调用的结果时：

```aura
sync {
    for i in range(2) {
        spawn (io: Io, i: int) {
            let r = worker(io, i)      // ← 变量 r 声明在 spawn 生成的 lambda **内部**
            io.println("  r=" + str(r))
        }
    }
}
```

生成码里，`r` 的声明位于 `addTask([...](...){ ... }(io, i));` 的 **lambda 内**，
但 sync 收集器的**驱动语句** `try { co_await r; } catch (const aura_rt::Error& _u5e0) { ... }`
被生成在 **lambda 之外**（`}` 闭合之后）⇒ 引用不可见变量 ⇒ `g++` 报错。

**`aurac` 自身 `rc=0`**（它只生成 `.cpp`，不做 C++ 语义检查）⇒ 缺陷在**编译生成码**时才暴露。

## 1. 现象

```bash
$ ./build/aurac.exe <探针>.aura --cpp p1.cpp -S     # rc=0，无任何报错
$ g++ -std=gnu++20 -fsyntax-only -w -I runtime p1.cpp
p1.cpp: In function 'aura_rt::task<void> aura_main(aura_rt::Io)':
p1.cpp:62:20: error: 'r' was not declared in this scope
   62 |     try { co_await r; } catch (const aura_rt::Error& _u5e0) {
      |                    ^
```

**生成码关键片段**（`p1.cpp`，行号实测，缩进原样）：

```cpp
35:    for (auto i : std::views::iota(0, 2)) {
36:    aura_rt::requireSync()->addTask([](aura_rt::Io io, int32_t i) -> aura_rt::task<void> {
37:    aura_rt::task<int32_t> r = [&]() -> auto {          // ← r 声明在 lambda 内
        ...
47:    auto _aw0_r = (co_await r);                          // ← 这里也在 lambda 内（合法）
48:    auto _aw1_r = (co_await r);
        ...
60:        co_return;
61:    }(io, i));                                           // ← lambda 结束
62:    try { co_await r; } catch (const aura_rt::Error& _u5e0) {   // ⚠️ r 已不可见
63:        if (!_u5has0) { _u5has0 = true; _u5err0 = _u5e0; }
64:        else std::fprintf(stderr, "[aura_rt] additional sync error\n");
65:    }
66:    aura_rt::gc_safepoint();
67:    }
```

## 2. 根因分析（Root Cause Analysis）

### 2.1 两条生成逻辑的**作用域假设不一致**

| 环节 | 生成者 | 假设 |
| :--- | :--- | :--- |
| `let r = <协程调用>` 的声明 | `genSpawnStmt`（spawn 闭包体就地生成）| 变量住在 **spawn lambda 内** |
| 驱动 `try { co_await <frame.names[i]>; } catch (...)` | **`genFutureDrive`**（`src/CodeGen/CodeGen.cpp:88-115`）| 变量在**当前函数体作用域**（`frame.names[i]`）|

- `genFutureDrive` 用 `frame.names[i]` 取「本帧待驱动的 future 变量名」，然后**无条件**在**当前生成位置**写字面 `co_await <name>`；
- 它**不检查**该变量是否与驱动语句处在**同一作用域**（及其是否在某个闭包/lambda 内被声明）；
- 当 `let <future>` 出现在 `spawn` 闭包体（其体是**普通 lambda**，见 `StmtSync.cpp:277` 的 `currentFunctionIsCoroutine_ = false; // 普通 lambda，禁止 co_await`）内时，声明点与驱动点**分属两层作用域** ⇒ 生成的 C++ 引用不可见。

### 2.2 为什么 `aurac` 不报错

`aurac` 的职责止于**生成 C++ 文本**；对生成码的**作用域合法性**不做检查（那是宿主编译器的活）。
⇒ 这类缺陷的暴露点是「编译生成码」，因此**单元测试里必须真的编一次生成码**才能覆盖（本缺陷正是被 ASAN 批的「编译 + 运行」链路抓到的）。

## 3. 复现（文件:行号）

**探针**（`scripts/f18_p2_BatchD_cases/`，批 D 落盘）：

| 探针 | 形态 | 结果 |
| :--- | :--- | :--- |
| `_p1_spawn_let_nosync_try.aura` | sync + for + spawn，体内 `let r = worker(io, i)`（**无 try**）| ❌ 同一报错（`:62`）|
| `_p4_spawn_let_no_for.aura` | sync + spawn（**不在 for 内**），体内 `let r = <协程调用>` | ❌ 同一报错（`:61`）|
| `_p2_spawn_nocall_let.aura` | 同上但体内**裸调用**不绑定 | ✅ rc=0 |
| `_p3_sync_let_coro.aura` | **sync 块级** `let r = <协程调用>`（= bug-87 原形态）| ✅ rc=0 |

⇒ **触发条件唯一**：`let` **绑定**协程调用出现在 **`spawn` 闭包体内**。与 `try` 无关（`p1` 无 try 也复现）。

**复现命令**（**必须两步**——只跑 `aurac` 看不到缺陷）：

```bash
cd /d/you/Aura
export PATH=/c/msys64/ucrt64/bin:$PATH
TMP="$LOCALAPPDATA/Temp/bug93"; mkdir -p "$TMP"

# ① 生成（aurac 会 rc=0，不报错）
./build/aurac.exe scripts/f18_p2_BatchD_cases/_p1_spawn_let_nosync_try.aura --cpp "$TMP/p1.cpp" -S

# ② 编译生成码（缺陷在此暴露）
g++ -std=gnu++20 -fsyntax-only -w -I runtime "$TMP/p1.cpp"     # ⇒ error: 'r' was not declared in this scope
```

**独立复现记录（本鲸，2026-09-30）**：`p1`（`:62:20`）与 `p4`（`:61:20`）**均报同一错误**；`aurac` 两步均 `rc=0`。

## 4. 影响面（Scope）

- **受影响**：任何「在 `spawn` 闭包体内用 `let`/变量绑定一个**协程调用**结果」的 Aura 程序 ⇒ **编译失败**（宿主编译器报错，错误信息与 Aura 源码无对应关系，用户难以定位）。
- **不受影响**：`spawn` 体内的**裸调用**（不绑定）✅；`sync` 块/`sync for` 体内的 `let` 绑定 ✅；非协程函数调用 ✅。
- **与 ASAN/GC 无关**：纯编译期（生成文本阶段）问题，未进入运行期。

## 5. 定性证据（**不是** feature-18 P2 引入的回归）

### 5.1 结论

**既有缺陷**（与 feature-18 P2 **无因果关系**）。

### 5.2 证据：P2 的 diff 中，位置决策行是**上下文行**

`git diff -U6 -- src/CodeGen/CodeGen.cpp` 显示（`genFutureDrive`）：

```diff
      writeLine(cpp, "try { co_await " + frame.names[0] + "; } catch (const aura_rt::Error& _u5e" +   ← 上下文行（未改动）
                     sfx + ") {");
      indentLevel_++;
-     writeLine(cpp, "if (!_u5has" + sfx + ") { _u5has" + sfx + " = true; _u5msg" + sfx + ...);
+     writeLine(cpp, "if (!_u5has" + sfx + ") { _u5has" + sfx + " = true; _u5err" + sfx + " = _u5e" + sfx + "; }");
```

- **P2 只改了 catch 体内部的赋值目标**（`_u5msg/_u5kind` 双标量 → `_u5err` 完整值）；
- **`co_await frame.names[i]` 这一行 P2 未触碰**（diff 中的上下文行）⇒ 其行为与 P2 之前一致；
- `r` 的**声明位置**由 `genSpawnStmt` / `let` 语句生成逻辑决定，P2 同样未触及（P2 的改动面 = `StmtTry.cpp`（重写）、`CodeGen.h/.cpp`、`StmtControl.cpp`、`StmtSync.cpp`、`main.cpp`、`ExprGen.cpp`；其中 `genSpawnStmt` 的**函数体**未在 diff 出现）。

### 5.3 铁证：P2 前后这 4 行**逐字相同**（零风险方法，取代 stash 实验）

**方法**：直接取 P2 之前的源码对比（不需要 `git stash`、不需要重新编译）：

```bash
cd /d/you/Aura
git show HEAD:src/CodeGen/CodeGen.cpp > "$LOCALAPPDATA/Temp/base_cg.cpp"
grep -n co_await "$LOCALAPPDATA/Temp/base_cg.cpp"
```

**结果**（P2 前 = HEAD `1b286fd`，该文件 **585 行**）：

```
90:        writeLine(cpp, "try { co_await " + frame.names[0] + "; } catch (const aura_rt::Error& _u5e" +
99:        writeLine(cpp, "co_await " + frame.names[0] + ";");
103:            writeLine(cpp, "try { co_await " + frame.names[i] + "; } catch (const aura_rt::Error& _u5e" +
112:            writeLine(cpp, "co_await " + frame.names[i] + ";");
```

**对比当前（P2 后，该文件 588 行）**：同样这 4 行、**行号与文本逐字相同** ⇒ P2 的 3 行净增全在别处。

⇒ **`co_await <frame.names[i]>` 的生成逻辑 P2 前后完全一致** ⇒ 本缺陷在 P2 之前**必然同样存在**。**定性：既有缺陷**（与 feature-18 P2 无因果关系）。

**（过程如实记录）** 本鲸先前另用 `git stash` 跑过 baseline 对照（`scripts/_f18p2_baseline_probe.sh`：stash `src/` → 编 baseline `aurac` → 编探针 → `stash pop` → 编回 P2），脚本**执行成功并正确恢复**（`src/` 改动 8 个文件回位），但输出经 `| tail -70` **截断**，`BASELINE_EXIT` 未留存（管道截断不可恢复）⇒ 改用上述 `git show` 对比法复核，结论一致。
**教训**：长脚本的产物要 `> 文件` 落盘再读，**不要接 `tail`**（截断后无法恢复）。

## 6. 修复方向（待调研）

- **必须**让「驱动语句」与「future 变量」**作用域一致**：驱动应由**声明该变量的那一层**生成（即 `spawn` 闭包体内生成自己的驱动），或
- 让 `genFutureDrive` 感知变量所在作用域（若变量被闭包捕获/未跨出闭包，则不在外层写 `co_await`），或
- 在 `let` 绑定协程调用时**把该 future 提升到与驱动同层**（改变生成形态，需评估对既有 943 处生成码断言的影响）。
- ⚠️ 本缺陷**未修**（发现于验证批，按「验证批只测量不修复」纪律保留最小复现）。
