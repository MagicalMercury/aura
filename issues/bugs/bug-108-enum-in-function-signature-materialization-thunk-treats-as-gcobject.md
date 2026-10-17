---
type: bug_report
module: CodeGen
sub_module: 物化 thunk（_aura_mat_*）对非堆用户类型无值语义分支
status:
  - fixed
severity:
  - high
discover_date: 2026-10-15
related_issues:
  - issues/features/feature-20-enum-type.md
  - issues/bugs/bug-107-bare-same-name-variant-pattern-resolved-as-type-pattern.md
tags:
  - enum
  - codegen
  - materialization-thunk
  - silent-wrong-code
---

# 【CodeGen / 物化 thunk】[x] **主标题：enum 出现在顶层函数签名（形参或返回）时物化 thunk 把 enum 当 `GcObject*` ⇒ 生成的 C++ 编不过，`aurac` 静默通过（单文件零 import 即复现）**

> **一句话摘要**：把 enum 用在**普通函数**的形参或返回类型上（如 `fun take(c: Color) -> int`、`fun give() -> Color`）时，为该函数生成的**物化 thunk**（`_aura_mat_<unit>_<n>`）在 `CallArg` 装箱/拆箱路径上把 enum 值当作**堆对象指针**（`__v[i].v.p` / `static_cast<GcObject*>`），生成坏 C++ ⇒ `g++` 报错；而 `aurac` **不报任何诊断**（`aurac_rc=0`）。enum 用在**方法**、局部 `let`、record 字段、`match`、`==` 上均正常 —— **缺口专在"顶层函数签名"这条通路**。

## 1. 调研背景与发现
- **发现时间**：`2026-10-15`（enum 实施**批 6**（跨模块 B2）遗留③ 提出；**主 Agent 用其探针独立复现并实读报错原文后登记**，非转述）。
- **触发场景**：任何把 enum 作**普通函数**形参/返回类型的代码（**不需要 import**、**不需要同名变体**）。
- **与既有批次的关系**：与批 3 报红 **B3-2**（「feature-18 方法物化 thunk 假定指针接收者」）**同族** —— 那次修的是**方法接收者**；**函数形参/返回**这条通路未被覆盖。enum 是仓内**第一个非堆用户类型**（`isValueType` 语义），凡「假定用户类型皆 `GcObject*`」的生成路径都会在 enum 上暴露。

## 2. 根因分析（Root Cause Analysis）
### 2.1 复现与报错原文（主 Agent 亲跑）
`scripts/b6_cases/_probe_local_enum_param.aura`（单文件、零 import）：
```aura
enum type Color = { Red, Green }
fun (self Color) hex() -> int { return 1 }
fun take(c: Color) -> int { return c.hex() }
fun main(io: Io) { let n = take(Color.Red) }
```
```
$ ./build/aurac.exe .../_probe_local_enum_param.aura --cpp .../_probe_local_enum_param.cpp -S
aurac_rc=0                       ← 静默
$ g++ -std=gnu++20 -fsyntax-only -w -I runtime .../_probe_local_enum_param.cpp
..._probe_local_enum_param.cpp:98:100: error: cannot convert 'aura_rt::GcObject* const' to 'Color::_Tag' in initialization
   98 |   return aura_rt::CallArg::of((int64_t)(__t->invoke(__t, static_cast<Color>(__v[0].v.p))));
      |                                                                     ~~~~~~~~~^~~~~~~
```
`scripts/b6_cases/_probe_local_enum_ret.aura`（返回侧）：
```
..._probe_local_enum_ret.cpp:99:45: error: invalid 'static_cast' from type 'Color' to type 'aura_rt::GcObject*'
   99 |   return aura_rt::CallArg::of(static_cast<aura_rt::GcObject*>(__t->invoke(__t)));
```
### 2.2 关键逻辑细节
- 报错点都在 **`_aura_mat_*`（物化 thunk）** 的 `__adapt` lambda 内：形参走 `__v[i].v.p`（**指针成员**）再 `static_cast<Color>`；返回走 `static_cast<GcObject*>(invoke(...))` ⇒ **该 thunk 假定参数/返回值皆为堆对象指针**。
- enum 侧已落地的是「**值语义**」（`enumTypeOf`/`enumTypes_` 路径：`X s = …`、`s.m()`、`_tag` 比较、入口 `const X self = *this;`），**没有接入物化 thunk 的装箱路径** ⇒ 两套口径在"函数签名"处相撞。
- **可行规避（当前）**：enum 值经 **record 字段**传递（批 6 用例即如此），或改用**方法**承载逻辑。

## 3. 影响范围（Scope）
- **结论**：**所有**「enum 作普通函数形参 / 返回类型」的代码生成坏 C++，且 `aurac` 静默（无诊断、无 rc 变化）⇒ 用户要到 `g++` 阶段才看到错误，且错误信息指向编译器内部 thunk（可读性极差）。
- **不受影响**：enum 作**方法**形参/返回（`fun (self Color) …`）、局部 `let`、record 字段、`match`、`==`、单例访问 —— 主 Agent 实测 10 例单文件两步铁律全 `0/0`（`scripts/_verify_b6.sh` §4）。
- **影响面评级**：这是 enum **可用性**层面的缺口（"把 enum 传给函数"是最常见用法之一），非边角。

## 4. 实测复现矩阵（Validation Matrix）
> 全部为主 Agent 亲跑；产物在 `%LOCALAPPDATA%\Temp\b6verify\`。

| 探针 / 用例 | 场景 | 预期（修复后） | 当前实际 | 状态 |
| :--- | :--- | :--- | :--- | :--- |
| `_probe_local_enum_param.aura` | enum 作**形参**（单文件零 import） | `aurac=0` 且 `g++=0` | ❌ `aurac=0` / `g++=1`（`cannot convert 'GcObject* const' to 'Color::_Tag'`） | 根因直击 |
| `_probe_local_enum_ret.aura` | enum 作**返回** | `aurac=0` 且 `g++=0` | ❌ `aurac=0` / `g++=1`（`invalid static_cast` 到 `GcObject*`） | 同源 |
| `scripts/b3_cases/c1..c9` + `_v1_probe/p09` | 方法 / let / match / == / 同名变体 | 全 `0/0` | ✅ 全 `0/0` | **不误伤**（说明缺口专在函数签名） |
| 批 6 跨模块用例（enum 经 **record 字段**传递） | 变通路径 | `0/0` | ✅ `0/0` | 规避写法有效 |

## 5. 修复方案（Fix Plan）
- **根因落点（主 Agent 本轮已定位到行号，下一轮直接从这里查）**：
  - **装箱/拆箱助手（主战场）**：`src/CodeGen/ExprClosureArgs.cpp`
    - `:35` `return "static_cast<" + cppTy + ">(" + slot + ".v.p)";`（**形参拆箱走指针成员**）
    - `:45` `return "return aura_rt::CallArg::of(" + cast + ");";`
    - `:52` `return "static_cast<" + retCpp + ">(" + var + ".v.p)";`（**返回拆箱走指针**）
    - **`:464` vs `:467` 是「指针 / 值」两条分支的选择点** ⇒ enum 在此**选错分支**（应为值分支）
  - **同族（同判据，须一并核）**：`src/CodeGen/ExprClosureCallableObj.cpp:35`（同形助手）、`src/CodeGen/CodeGen.cpp:614`/`:661`（接收者侧 `static_cast<GcObject*>(recv->v.p)`）
  - **thunk 命名/收集**：`src/CodeGen/MetaCollect.cpp:19–26`（`"_aura_mat_" + nsStem_ + "_" + seqSymbol_`，一次定型）、`MetaCollect.h:27/:98/:121`
- **方向**：把 `:464`/`:467` 的**分支判据**改为按「是否堆对象」判定（复用既有 `isValueType` / `enumTypeOf` / `enumTypes_`；enum = **非堆值类型**），使 enum 形参/返回走**值语义**分支（不套 `GcObject*`）；同时核对同族三处判据一致。
- **一致性要求**：与批 3 落地的「值接收者 / `enumTypes_` / 非堆口径」、批 6 的「导入 enum 装填」使用**同一判据**，不得分叉。
- **测试**：enum 作形参/返回各一例（含泛型函数、协程函数、方法内调用）⇒ 两步铁律 `0/0`；并补单测断言产物不含 `static_cast<GcObject*>` 形式的 enum 装箱。

## 6. 回归验证清单（Regression Checklist）
- [ ] `_probe_local_enum_param.aura` / `_probe_local_enum_ret.aura` 两步铁律 `0/0`
- [ ] enum 作泛型函数形参 / 协程函数形参-返回 ⇒ `0/0`
- [ ] 方法路径、`let`、record 字段、`match`、`==` 行为与修复前**一致**（不误伤）
- [ ] 全量单测保持基线（批 6 后 **1569 / 1569**）不回归
- [ ] `--filter=Examples`（17 例）通过
- [ ] 同步 `change.md`：§4.9.4 新增已知限制 + 附十一登记引用本 bug 号

## 7. 附加资源与产物
- **探针**：`scripts/b6_cases/_probe_local_enum_param.aura`、`scripts/b6_cases/_probe_local_enum_ret.aura`（批 6 遗留物，超其红线未修）
- **复现命令**：`bash scripts/_verify_b6.sh`（§3 段）
- **产物**：`%LOCALAPPDATA%\Temp\b6verify\_probe_local_enum_param.cpp:98`、`_probe_local_enum_ret.cpp:99`
- **同族历史**：批 3 报红 B3-2（方法物化 thunk 假定指针接收者，已修）；`CodeGen/DeclEnum.cpp`（值语义落地）、`CodeGen/TypeMap.cpp`（`isHeapSemType` 曾误 `return true`）
- **来源批次**：`scripts/enum_impl_b6_report.md` §报红/遗留 ③

---
**当前状态**：`2026-10-15` 调研完成；**`2026-10-16` 已修复**（见 §8）。

## 8. 修复记录（2026-10-16）

### 8.1 根因（实读确认，修正 §5 的推测）
- 装箱/拆箱助手 `src/CodeGen/ExprClosureArgs.cpp` 的 **`erasedIsPtrCpp(cppTy)`** 是**纯 C++ 名字**判据：
  `{int32_t, double, bool, void, aura_rt::NoneType}` 之外**一律**判 GC 指针（末行 `return true;`）。
  enum 壳的 C++ 名是**裸壳名**（无 `*`，`mapSemType(EnumSemType)` → `Color`）⇒ 被误判为指针。
- §5 推测的「`:464` vs `:467` 分支选择点」**不是** enum 的落点（那是 `genErasedInvoke` 的
  Ptr 实参句柄保护分支，判据同为 `erasedIsPtrCpp`）；真正落点是**该助手组的每一个函数**：
  `erasedKindName` / `erasedArgToCallArg` / `erasedSlotToParam` / `erasedRetToCallArg` /
  `erasedResultUnpack`（同一份判据的 5 个消费点，旧形态都无法表达 enum 值语义）。
- thunk 名（`MetaCollect.cpp:19–26` `_aura_mat_<nsStem>_<seq>`）与 `CodeGen.cpp:614/:661` 的
  接收者侧**无需改动**（enum 接收者的方法物化 thunk 已在批 3 报红 B3-2 处显式跳过）。

### 8.2 修法（判据同源 + 窄守卫）
1. 新增**唯一**判据入口 `CodeGenerator::isEnumValueSemType(const SemType*)`
   （声明 `CodeGen.h`，实现 `ExprGen.cpp`，紧邻 `isHeapSemType`）——
   判据同源：认的就是 `enumTypes_` 装填的 `EnumSemType*`（批 3 值语义链同一 hop），
   结论与 `isHeapSemType` 的显式 enum 分支（`EnumSemType ⇒ false`）逐字一致，
   词法与仓内既有 8 处 `dynamic_cast<const EnumSemType*>` 同款；**零新增语义**。
   窄守卫（只认 enum）：与 `DeclFun.cpp:635` / `DeclGen.cpp:146` / `CodeGen.cpp:620`
   的既有纪律一致，**未**放宽成 `isValueType`，**未**用 `!isHeapSemType`
   （后者对 Iterator 值视图亦 false ⇒ 会把值视图误判为标量）。
2. 5 个助手各加 `bool isEnum` 形参，调用点用 `isEnumValueSemType(sig.paramTypes[i])` /
   `(sig.returnType)` 计算 —— 与生成 `paramCpp`/`retCpp` 的 `mapSemType` **同一个 SemType**，
   判据与类型串**不可能漂移**。
3. enum 值表示：`CallArg` 只有 `I64/F64/Ptr/Nonev` 四槽 ⇒ 值经 **I64 槽**承载壳的身份字段 `_tag`
   （**零 runtime 改动**）：
   - 装箱 `(int64_t)((EXPR)._tag)`
   - 拆箱 `Color{static_cast<Color::_Tag>(slot.v.i)}`（聚合初始化，与 `DeclEnum.cpp:362`
     的单例定义 `const Color Color::Red{Color::_Tag::Red};` 逐字同款；壳恒为单非静态成员 `_tag`
     的聚合，形态 1/2 同款）
4. 同批修掉**同源面**：`genErasedInvoke`（erased 调用点）——
   实参打包与返回拆箱同样走值语义（修复前会产 `static_cast<aura_rt::GcObject*>(Color.Red)`）。
   为此 `ErasedCallArg` 载荷新增 `sem` 字段（`CodeGen.h`），调用点 `ExprCall.cpp:303` 填
   `arg->inferredType`。
5. 镜像文件 `ExprClosureCallableObj.cpp` 的同名助手**逐字同步**（本 TU 当前未调用它们；
   登记为「同形助手的判据分叉风险」已封，清理留待单独批）。

### 8.3 落点（`文件:行号`，修复后）
- `src/CodeGen/CodeGen.h`：`isEnumValueSemType` 声明（`isHeapSemType` 之后）、`ErasedCallArg`、`genErasedInvoke` 签名
- `src/CodeGen/ExprGen.cpp`：`isEnumValueSemType` 实现
- `src/CodeGen/ExprClosureArgs.cpp`：助手组（`erasedKindName`/`erasedArgToCallArg`/`erasedSlotToParam`/`erasedRetToCallArg`/`erasedResultUnpack`）、`genErasedWrap`（paramIsEnum/retIsEnum）、`genErasedInvoke`
- `src/CodeGen/ExprClosureCallableObj.cpp`：助手组镜像同步
- `src/CodeGen/ExprCall.cpp`：`genErasedInvoke` 调用点填 `sem`

### 8.4 验证（两步铁律原文 + 统计）
| 项 | 结果 |
| :--- | :--- |
| `_probe_local_enum_param.aura` | `aurac_rc=0` / `g++_rc=0` ✅（修复前 `0/1`） |
| `_probe_local_enum_ret.aura` | `aurac_rc=0` / `g++_rc=0` ✅（修复前 `0/1`） |
| `scripts/b8_cases/p01_enum_param_ret_erased.aura`（形参+返回+**erased 调用点**） | `0/0` ✅ |
| `scripts/b8_cases/g01_enum_generic_fn.aura`（**泛型函数**） | `0/0` ✅；**不发 thunk**（签名不可静态构造 ⇒ `materialize=nullptr`，安全降级、无坏 C++） |
| `scripts/b8_cases/k01_enum_coro_fn.aura`（**协程函数**） | `0/0` ✅；**不发 thunk**（归因对照：协程必需的 `io: Io` 形参本身即令签名不可构造——仅含 `io: Io` 的普通函数同样不发 thunk） |
| 不误伤：`b3_cases/c1..c9`、`_v1_probe/p01..p12`、`_b5_probe/c01..c05` | 全 `0/0` ✅ |
| 不误伤：`b6_cases/c01/c02`（跨模块） | `0/0/0` ✅ |
| 负例回归：`_v1_probe/n01..n16`、`_b5_probe/n01` | 仍 `aurac_rc=1` + 干净报错 + 零产物 ✅ |
| 全量单测 | 基线 **1579/1579** → **1583/1583**（+4 新用例，见 §8.5） |
| `--filter=CodeGenEnum` | 28 → **32 passed, 0 failed** |
| `--filter=Examples` | **17 passed, 0 failed**（`example/used/1-6.aura` **不存在**，实为 `1.aura`…`6.aura`） |
| `example/test.aura` md5 | `5f1760a5a360f4139d176434775abddd`（未变） |
| `git status --short -- runtime/` | 空 ✅（零 runtime 改动） |

### 8.5 新增测试
`test/codegen/test_codegen_enum.cpp` 追加 4 例（同文件自带 `enumThunkGen`：
本目录的 `compileSource` **不注入 collector ⇒ 不发射 thunk**，故 thunk 面断言须自有注入点）：
- `ThunkEnumParamValueSemantics`（enum 形参 ⇒ `Kind::I64` + `Color{static_cast<Color::_Tag>(__v[0].v.i)}`；断言**不含** `__v[0].v.p`）
- `ThunkEnumReturnValueSemantics`（enum 返回 ⇒ `of((int64_t)((__t->invoke(__t))._tag))`；断言**不含** `static_cast<aura_rt::GcObject*>(__t->invoke(__t))`）
- `ErasedCallSiteEnumValueSemantics`（erased 调用点两向；断言不含 `static_cast<aura_rt::GcObject*>(Color::` / `GcRootHandle<Color>`）
- `ThunkNonEnumReturnsKeepBranch`（不误伤：`-> string` 仍走 `GcObject*` 装箱、`-> int` 仍走 `(int64_t)`）

### 8.6 遗留 / 未做（**不属本缺陷**）
- 本批**未改** `change.md`（批 8 红线②「禁改 change.md」）⇒ §6 清单里「同步 change.md §4.9.4 + 附十一登记」
  一条**未执行**，与批 8 简报红线冲突，**需主人裁定**后另行处理。
- 批 6 遗留②「跨模块类型进函数签名 ⇒ header 缺 namespace 别名（**record 同病**）」**未修**（另一族既有缺陷，红线③）。
- `ExprClosureCallableObj.cpp` 中同名的助手组是**死代码**（本 TU 未调用），本批仅同步判据、未清理。

