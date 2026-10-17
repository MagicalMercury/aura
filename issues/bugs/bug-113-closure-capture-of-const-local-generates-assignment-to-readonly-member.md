---
type: bug_report
module: src/CodeGen
sub_module: 闭包捕获 const 局部（含 enum 值接收者的 `self`）时生成 `cap_x = x`，而捕获槽类型 = `decltype(x)` = const 限定 ⇒ 产物对只读成员赋值，C++ 编译失败（aurac 报的是 C++ 错误而非 Aura 诊断）
status:
  - fixed
severity:
  - high
discover_date: 2026-10-17
discovered_by: Hermes（enum v1.1 L-3 探针批 P1 的副产品；主 Agent 自建三情形复现）
related_issues:
  - "[[bug-89-globalroots-update-missing-isgcaddress-guard]]"（同日发现，无关）
  - enum v1.1 / change.md §4.10（P1 探针场景）
tags:
  - codegen
  - closure
  - capture
  - const
  - bad-cpp
  - enum
---

# [x] 【闭包捕获 `const` 局部（enum 值接收者的 `self` / `const n`）⇒ 产物对只读成员赋值 ⇒ 编译失败】

**状态**：`[x] 已修复（2026-10-17）`
**严重度**：**high**（enum 方法里任何捕获 `self` 的内层闭包都**用不了**；且 `aurac` 抛出的是**生 C++** 错误，不是 Aura 诊断）
**发现场景**：enum v1.1 · L-3 探针批 P1（`cap_recv` 可达性探针）的副产品；主 Agent 用三个最小情形独立复现。

---

## 0. 一句话摘要

闭包捕获一个 `const` 限定局部变量时，生成器把捕获槽声明为 `decltype(<变量>)` —— 对 `const T x` 即 **`const T`**；
随后生成 `__o_h.get()->cap_x = x;` ⇒ **对只读成员赋值** ⇒ C++ 报错。enum 值接收者的方法体里 `self` 被声明为
`const <壳> self = *this;`（`DeclFun.cpp:762-765` 的值接收者分支），因此 **enum 值接收者 + 内层闭包捕获 `self`** 必然踩中。

## 1. 复现（主 Agent 自建三情形，`scripts/_v11_l3_verify_bug113.sh`）

| 情形 | 源 | 实测 |
| :-: | :--- | :--- |
| **A** | `enum type Color = { Red, Green }` + `fun (self Color) pick() -> bool { let g = fun() -> bool { return self == Color.Red } return g() }` | **`aurac_rc=1`**；错误来自**产物**：`a.cpp:77:29: error: passing 'const Color' as 'this' argument discards qualifiers`（`__o_h.get()->cap_self = self;`） |
| **B** | `fun main(io: Io) { const n = 5  let g = fun() -> int { return n } … }` | **`aurac_rc=1`**；`b.cpp:61:24: error: assignment of read-only member '…::__closure_0::cap_n'`（`__o_h.get()->cap_n = n;`） |
| **C**（对照） | 普通 record 值接收者 + 闭包捕获 `self`（`fun (self Point) get() -> int { … self.x … }`） | **`aurac_rc=0` / `g++_rc=0`** ⇒ 不是"值接收者"通病 |

**产物证据（A）**：
```
:59   decltype(self) cap_self;
:77   __o_h.get()->cap_self = self;
```
`decltype(const Color self)` = **`const Color`** ⇒ 捕获槽是只读成员；赋值即错。

## 2. 影响

- **enum 值接收者方法内的一切 `self` 捕获**（含内层闭包、`spawn`/`sync` 里的闭包）：**语言级不可用**（今天之前没人测过）。
- **普通函数里的 `const` 局部捕获**同样不可用 —— 根因与 enum 无关，enum 只是把 `const` 带进了 `self`。
- **诊断面**：`aurac` 退出码 1，但错误文本是 **g++ 的**（`error: passing 'const Color' as 'this' argument…`），用户看到的是生成码文件名与行号 ⇒ 属「生坏 C++」而非「Aura 报错」。
- **测试覆盖**：`test/` 无此形态用例（探针批 `grep` 未命中同形态）。

## 3. 修法方向（待实施批裁定，不在本笔记改码）

候选（按"准确性 > 可维护性 > 复杂性"）：
1. **捕获槽去掉 const 限定**：生成 `std::remove_const_t<decltype(x)> cap_x;`（值语义捕获，**拷贝**一份即可，`const` 只是源变量的限定，不影响拷贝后可变性）—— 最小、语义正确；
2. 或捕获时按值显式 `auto`（`cap_x` 用 `std::decay_t`）；
3. **引用捕获**路径若存在，检查 `[&]` 捕获是否应改用引用（需评估生命周期/GC 根）。
> ⚠️ 必须同时检查 `GcViewSlot` / `desc` 复合偏移那一套（`ExprClosureCallableObj.cpp:226` 附近）对 `const` 类型的判定是否也受影响（本次未展开）。

## 4. 验证建议

- 单测（承重）：上文 A、B 两情形 `aurac --cpp` ⇒ **rc=0** 且产物 `g++ -fsyntax-only` **rc=0**；运行期读到正确值（A：`pick()` 返回 true；B：闭包返回 5）。
- 对照：C 情形不回归；现有闭包全套（`CodeGenEnum` / `SemaEnum` / `Examples` 17/17）不回归。
- 全量 0 failed（基线 **1606 / 1606**）+ `example/test.aura` md5 `5f1760a5a360f4139d176434775abddd`。

## 5. 关联

- **enum v1.1 L-3 P1**：本缺陷即在其 `cap_recv` 可达性探针中被顺带暴露（`cap_recv` 本身确认**不可达**，L-3② 可结案）。
- `DeclFun.cpp:762-765`（值接收者 `const <壳> self = *this;`）、`ExprClosureCallableObj.cpp:226`（捕获槽类型生成）——**修法落点需实施批再核实行号**。

---

## 8. 修复记录（2026-10-17）

**修法**：值捕获槽类型去 const（候选 1，最小面）。

- 落点：`src/CodeGen/ExprClosureCallableObj.cpp:232`（`genFunExprCallableObj` 捕获槽「普通变量（值捕获）」分支）
  - before：`addSlot(sn, "cap_" + sn, "decltype(" + src + ")", src);`
  - after ：`addSlot(sn, "cap_" + sn, "std::remove_const_t<decltype(" + src + ")>", src);`
- 理由：值捕获是**拷贝**语义，源变量的 `const` 只是源变量的限定，不应带到槽上；`remove_const_t` 只去顶阶 const，**不**做数组/函数退化（故不用 `decay_t`，避免改动槽类型进而扰动 `GcViewSlot`/`desc` 判据）。嵌套捕获形态（`src` = `__c_h.get()->cap_x`）同样适用且无害（外层槽已非 const）。

**未动槽源（核查结论：不碰 GC 视图槽）**

| 槽源 | 位置 | 类型来源 | 是否受影响 |
| :--- | :--- | :--- | :--- |
| `cap_recv`（receiver/handle） | `:173-177` | `currentReceiverCppType_` / `+ "*"` | 否（无 `decltype`、无 cv）；C 情形产物仍为 `Point* cap_recv;` |
| 递归自引用 `cap_self` | `:190` | `base + "*"` | 否 |
| 视图值槽 | `:199-209` | `viewRootTypes_[cn]` | 否（本次未改；`ClosureNestedCaptureOuterViewSlotScopeExpr` 等视图槽用例全绿） |
| GC 根槽 | `:211-220` | `gcRootTypes_[cn]` | 否（值取 `.get()`，源非 const 句柄） |
| `desc`/`GcViewSlot` 复合偏移 | `:353-404` | `decltype(cls::槽名)` | 否（判据仍是「槽成员的实际类型」；视图槽类型未变，值捕获槽原本就非 GC/视图槽） |

**新增用例**（`test/codegen/test_codegen_enum.cpp`）

- `:893 CodeGenEnum.ClosureCaptureConstReceiverSelf`（A：enum 值接收者 + 闭包捕 `self`）——断言槽声明行不含 `const`、含 `std::remove_const_t<`、`__o_h.get()->cap_self = self;` 仍在（未改引用/放弃捕获）。
- `:921 CodeGenEnum.ClosureCaptureConstLocal`（B：`const n = 5` 被捕）。
- `:942 CodeGenEnum.ClosureCaptureValueReceiverControl`（C：record 值接收者对照）——`cap_recv` 仍为 `Point*` 且不含 `remove_const_t`。

**两步铁律**（脚本 `scripts/bug113_two_step.sh`）

| 情形 | `aurac --cpp` | `g++ -std=gnu++20 -fsyntax-only -w -I runtime` | 修前 |
| :-: | :-: | :-: | :-: |
| A | 0 | 0 | 1 / n-a（`passing 'const Color' as 'this'`） |
| B | 0 | 0 | 1 / n-a（`assignment of read-only member …::cap_n`） |
| C | 0 | 0 | 0 / 0 |

A 产物另做链接 + 运行：输出 `true`（`Color.Red.pick()`）。

**三步对照（承重）**

1. 修复版：`1609 tests, 1609 passed, 0 failed`。
2. 临时撤销（`addSlot` 回到 `decltype(...)`）重建 ⇒ `1609 tests, 1603 passed, 6 failed`，新增用例原文转红：
   - `EXPECT_CONTAINS failed: "std::remove_const_t<" not found in: "        decltype(self) cap_self;"`
   - `EXPECT_CONTAINS failed: "std::remove_const_t<" not found in: "        decltype(n) cap_n;"`
   - 另有 4 个闭包形态断言用例同时转红（见下「偏差」），C 对照仍绿。
3. 恢复修复并重建 ⇒ `1609/1609, 0 failed`，`example/test.aura` md5 回原值。

**全量/回归**

- 全量：基线 1606/1606 → 修复后 **1609/1609**（+3 新用例，0 failed）。
- `--filter=Examples`：**17/17**。
- `example/test.aura` md5：`5f1760a5a360f4139d176434775abddd`（不变）。
- 双目标重建：`cmake --build build` + `cmake --build test/build`。

**偏差（如实登记）**

- `test/codegen/test_codegen_closure.cpp` 5 处**形态断言**（`:795 / :842 / :1284 / :1287 / :1309`）由 `decltype(x) cap_x;` 改为 `std::remove_const_t<decltype(x)> cap_x;`：这类断言锁的是生成文本形态，形态因本修复而变；语义断言（init 表达式 `__o_h.get()->cap_x = …`、嵌套槽访问、负例）逐字未动，断言强度不变。
- 修法未采用候选 2（`decay_t`）：数组/函数退化会改动槽类型，可能扰动 `GcViewSlot`/`desc` 判定（无实测必要，取最小面）。
