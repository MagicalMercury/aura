---
type: bug_report
module: CodeGen
sub_module: 协程闭包（CallableObj task 形态）体内含 channel receive/send → gc_force_major 时根扫描命中悬垂根（0xe）→ ASAN access-violation（runtime/gc/mark_sweep.cpp:90 → parallel_mark.cpp:63 → types.h GcObject::forwarded）
status:
  - fixed
severity:
  - high
discover_date: 2026-09-11
related_issues:
  - "[[bug-78-coro-closure-detection-incomplete-bad-cpp]]"
tags:
  - codegen
  - gc
  - coroutine
  - closure
  - asan
---

# 【协程闭包内通道挂起 + GC】协程闭包体内 `ch.receive()` 后再 `gc_force()` → 根扫描读悬垂根槽（地址 0xe）→ SIGSEGV / ASAN access-violation

[x] **主标题：协程闭包（`CallableObj<task<T>, ...>` 形态）体内含 channel `receive`/`send` 挂起点，其后触发 `gc_force_major()` 时，`scanRootsOnly` 遍历线程根链读到悬垂 `GcRootHandle` 槽（值 0xe）→ `markRootEnqueue` 解引用零页地址 → ASAN access-violation；**具名协程函数同形态不崩**（闭包路径特有）**

> **一句话摘要**：同一「通道挂起 + gc_force」语义，写在**具名协程函数**里 ASAN 干净，写在**协程闭包**里必崩——闭包路径的根生命周期（`GcRootHandle` 注册/注销与协程帧存活期）存在缺口。

## 1. 调研背景与发现

- **发现时间**：2026-09-11，bug-78 修复子 Agent 在执行 ASAN 验证（`s4_5_iodetector`）时发现。
- **发现路径**：bug-78 修复使 `s4_5_iodetector.aura`（纯挂起型协程闭包）首次可编译，ASAN 轮次即暴露该崩溃。
- **崩溃栈（关键帧）**：
  ```
  #0 aura_rt::GcObject::forwarded() const            runtime/gc/../types.h
  #1 aura_rt::GcHeap::markRootEnqueue(GcObject*)     runtime/gc/parallel_mark.cpp:63
  #2 GcHeap::scanRootsOnly(bool)::$_0                runtime/gc/mark_sweep.cpp:90   <-- memcpy(&obj, node->ptr_ref_, 8) 读到 0xe
  #14 aura_rt::gc_force_major()                      runtime/gc/gc.h:764
  #18 ...::__closure_0::__invoke(...)                <generated>  协程闭包体内 gc_force
  ```
  报错：`AddressSanitizer: access-violation on unknown address 0x00000000000e`（READ，`rax = e`）。

## 2. 根因分析（Root Cause Analysis）
> **状态：初步**（现象与隔离实验已确认；精确到哪一个 `GcRootHandle` 节点待修时定位）。

- **隔离实验（决定性）**：

  | 形态 | 用例 | ASAN |
  | :--- | :--- | :--- |
  | 协程闭包 + `io.println` + `gc_force`（**无 channel**） | `s4_2_multisuspend.aura` | ✅ 0 报警 |
  | 具名协程函数 + `ch.receive()` + `gc_force`（**无闭包**） | `_repro/f07_verify/s4b_iso_named.aura` | ✅ 0 报警 |
  | **协程闭包** + `ch.receive()` + `gc_force`（含 io.println 的旧路径形态） | `s4_6_control.aura` | ❌ 崩溃（0xe） |
  | **协程闭包** + `ch.receive()` + `gc_force`（纯挂起，bug-78 修复后可达） | `s4_5_iodetector.aura` | ❌ 崩溃（0xe） |

- **结论**：触发条件 = 「**协程闭包**」∧「体内含 channel `receive`/`send` 挂起点」∧「挂了之后调用 GC」。与 bug-78 无因果——`s4_6_control`（闭包体含 `io.println`，旧 IoDetector 亦判协程，生成代码与 bug-78 修复无关）**同样崩溃**，说明这是**既有缺口**被 bug-78 修复「解锁可运行」后暴露。
- **与具名函数路径的差异点（待修时核实）**：闭包体经 `CallableObj::__invoke` 进入，`genIdentifier` 对捕获变量走 `__c->cap_x`，通道操作数经 `co_await [&]() -> auto { ... }()` 包装 lambda 内的 `GcRootHandle _h0_0`（Ref 模式，`ptr_ref_` 指向该包装 lambda 栈上的 `_a0_0`）注册为线程根；包装 lambda 返回 awaitable 后其栈帧销毁，而根链上残留节点 / 或协程帧上 `GcRootHandle`（如 `__c_h`）在挂起-恢复窗口内被读——`scanRootsOnly` 读到已复用内存（值 `0xe`）。
- **具名函数不崩的原因（推测，待验证）**：具名函数体不经 CallableObj 帧，`ch` 是形参（`GenCoroHandler` 约束下由别的根路径保护），不存在这个「包装 lambda 栈帧根」。

## 3. 影响范围（Scope）
- **受影响**：协程闭包体内含 channel send/receive（或其它生成该包装 lambda 形态的挂起点），且该闭包体或其调用路径上有 GC 触发（`gc_force` / 分配 / `concat`）。
- **不受影响**：具名协程函数；协程闭包体内无通道挂起（s4_1/s4_2/s4_3 均 ASAN 干净）。
- **性质**：既有缺口（非 bug-78 引入）。**Step 5 删旧路径前建议一并处理**（否则「协程闭包 + 通道」组合持续存在内存不安全）。

## 4. 实测复现矩阵（Validation Matrix）

| 测试文件 | 场景 | 预期 | 当前实际 | 备注 |
| :--- | :--- | :--- | :--- | :--- |
| `_repro/f07_verify/s4_5_iodetector.aura` | 闭包 + `ch.receive()` + `gc_force` | ASAN 0 报警 | ❌ access-violation 0xe | 纯挂起形态 |
| `_repro/f07_verify/s4_6_control.aura` | 闭包 + `io.println` + `ch.receive()` + `gc_force` | ASAN 0 报警 | ❌ access-violation 0xe | **旧路径形态**（证明独立于 bug-78） |
| `_repro/f07_verify/s4_2_multisuspend.aura` | 闭包 + `io.println` + `gc_force`（无通道） | 0 报警 | ✅ | 对照 |
| `_repro/f07_verify/s4b_iso_named.aura` | **具名**协程函数 + `ch.receive()` + `gc_force` | 0 报警 | ✅ | 对照（隔离变量=闭包） |

- 生成代码：`scripts/_asan_s4_5_iodetector.cpp`、`scripts/_asan_s4_6_control.cpp`（ASAN 日志 `example/asan_err.txt`）

## 5. 修复方案（Fix Plan）
- **方向（待评估）**：
  1. **A：包装 lambda 根不落地**——通道 `receive/send` 生成点避免在临时包装 lambda 栈上建 `GcRootHandle`（改用 `ObjectRoot` 值持有 / 在协程帧上物化根）。
  2. **B：协程闭包帧根显式化**——`__invoke` 内 `__c_h`（`GcRootHandle<__closure_0*>`）与捕获根在挂起窗口的注册/注销与协程帧严格配对（参考 bug-72 的 `GcRootScope::Global` 处理思路）。
  3. **C：根链节点生命周期审计**——“包装 lambda 帧销毁 vs 根节点注销”时序，防悬垂节点残留（含并发路径 `moveRootNode`）。
- **优先**：先用 ASAN + 日志定位悬垂根节点的注册点（`_h0_0` / `__c_h` / `_a0_0`），再选最小修法。

## 6. 回归验证清单（Regression Checklist）
- [x] `s4_5_iodetector.aura` / `s4_6_control.aura` ASAN 0 报警（2026-09-12 实测，均 `(empty —— no ASAN errors)` + Exit 0 + PASSED）
- [x] `s4_1/s4_2/s4_3/_s4b_*` 不回归（ASAN 0 报警，四例全绿）
- [x] 全量单测 0 failed（1302 / 0）+ `used/1-6.aura` 全过
- [x] 非 ASAN 模式 `used/1-6.aura` 全过（1/2 结尾为 "All closure tests passed"，其余 "ALL TESTS PASSED"）

## 7. 附加资源与产物
- **复现目录**：`example/used/leakcheck/_repro/f07_verify/`
- **生成代码**：`scripts/_asan_s4_5_iodetector.cpp`、`scripts/_asan_s4_6_control.cpp`、`scripts/_asan_iso_named.cpp`
- **关联**：`[[bug-78-coro-closure-detection-incomplete-bad-cpp]]`（修复解锁了该形态）、bug-72（GC 根族）、feature-07 Step 5


## 8. 实证定位结论（2026-09-11 修复子 Agent 探针实验，未改源码）

> 状态：**已实证定位**（悬垂根节点身份 + 污染来源确定），**修复方案待定**（需架构决策）。

### 8.1 悬垂的是哪个节点（决定性证据）

在 `s4_5_iodetector` 生成代码中注入根链表全量转储（`GcHeap::dbgDumpAllRootLists()`，仅探针期，已还原），
拿到 `aura_main` 协程帧内各根句柄地址与 `__invoke` 执行前后的对照：

```
IDENT &ch=...0060  &tag=...0088  &c=...00B0  &t=...00D8
RAW   &ch_raw=...0148  tag_raw 正常，c_raw/t_raw 正常
```

GC 崩溃前根链（单一线程链表，4 节点）：

| 链表节点地址 | 身份 | ptr_ref_ | 读到值 | 状态 |
| :--- | :--- | :--- | :--- | :--- |
| `...00D8` | `t`（GcString*） | `...0160` | 有效 GcString* | ✅ |
| `...00B0` | `c`（CallableObj*） | `...0158` | 有效对象 | ✅ |
| `...0088` | `tag`（GcString*） | `...0150` | 有效 GcString* | ✅ |
| **`...0060`** | **`ch`（channel 根）** | **`...0148`** | **`0x1` → 随后 `0xe`** | ❌ **悬垂/被污染** |

`&ch_raw == ...0148`（与 `ch.ptr_ref_` 完全一致 → 句柄接线本身正确）。
**崩溃根 = `ch` 句柄本身仍在线程根链上，但其 `ptr_ref_` 指向的 `ch_raw` 槽已被覆写为非指针小整数 `1`**
（`1` 正是 `channel(1)` 的容量实参），GC 保守解引用 `GcObject*` → `forwarded()` 读 0x1/0xe → access-violation。

### 8.2 污染时序（watch 实验）

```
WATCH before-invoke  ch_raw = 000011B1FBBA0040   (valid channel*)
WATCH after-invoke   ch_raw = 0000000000000001   ← 被 __invoke 执行期间覆写
```

- `aura_main` 帧内 `&ch_raw` 与 `__invoke` 帧内同偏移成员的低 32 位布局一致（`...FBBA0060` / `...FBBA0068`）。
- **`ch` 在生成代码中的最后一次使用是 `send` 包装 lambda（`ch.get()`）**，此后 `let ch` 的 `ch_raw` 槽
  不再被程序语义引用，但 `GcRootHandle<Channel<int32_t>*> ch(ch_raw)` 的**析构仍在作用域末尾**；
  Ref 模式根把 `ptr_ref_ = &ch_raw` 长期挂在线程根链上 → **根句柄寿命 ≫ 其引用目标（编译器临时槽）寿命**。
- **具名协程函数不崩的原因（已对比确认）**：`worker` 形参 `ch_raw` 在函数体内**后续仍被使用**（`ch.get()` 出现在
  多个后续 lambda），槽保持活跃；`s4b_iso_named` 的 `aura_main` 亦然。闭包路径中 `ch` 在 `__invoke` 调用后再无使用，
  槽被编译器合法复用 → 读到垃圾。
- 另注：`ch_raw` 指向的是 `new Channel<int32_t>(1)`（**非 GC 对象**），却被包成 `GcRootHandle` 注册为根——
  这是一条「非 GC 指针进根链」的既有隐患，`0x1` 恰是小整数才被 `markRootEnqueue` 解引用而爆。

### 8.3 结论对修复方向的指向

- **不是** 包装 lambda 的 `_h0_0` 生命周期问题（探针证实包装 lambda 返回时已正确注销，链上无残留）。
- **不是** `__c_h`（协程闭包自根）问题（`__invoke` 存活期内其值始终有效）。
- 真正缺口 = **Ref 模式根句柄的 `ptr_ref_` 指向的编译器临时槽（`*_raw`）寿命短于根句柄寿命**——
  该 `*_raw` 是 codegen 产物的实现细节，而非 Aura 语义变量本身。
- 由此，笔记 §5 的**方向 A（包装 lambda 根不落地）对不上根因**；方向 B/C 需要重新表述为
  「缩小根句柄作用域至其 `*_raw` 的活跃期」或「让 `*_raw` 与根句柄同寿命（提升到与句柄同作用域且不可复用）」。
- **待定**：改动落在 codegen（作用域/生命周期重排）还是 runtime（根句柄自持值）需主人裁决，故本轮未落地代码。

---

## 9. 修复方案定案（2026-09-11：3 路 SearchAgent 实证 + 主 Agent 亲读 8 处承重代码）

> 结论先行：**双层修复**——L1（CodeGen，根治）：单次绑定类根化点 **Ref→Value 单点追加**（一行追加第二实参，`*_raw`/decltype/`.get()` 形态全保留）；L2（runtime，防御纵深）：根扫描入口加**页内判定**（与栈扫描路径 `scanStackCandidate` 的既有防御对称）。原 §5 方向 A/B/C 均不采纳（见 9.2）。

### 9.1 缺陷三层定性（实证补充）

| 层 | 缺陷 | 实证依据 |
| :--- | :--- | :--- |
| **D1 主因** | Ref 模式根句柄 `ptr_ref_` 指向编译器临时槽 `*_raw`（trivial 指针变量），其「最后一次使用后」存储可被 g++ 依 as-if 合法复用；句柄寿命（作用域末/协程帧销毁）≫ 槽活跃期 → 根链读到垃圾（`0x1`） | §8.2 watch 实验；StmtLet.cpp:573-577 生成形态 |
| **D2 放大器** | 根链扫描路径**零防御**：`scanRootsOnly`（mark_sweep.cpp:86-91）`memcpy` 读值后仅判非空即 `markRootEnqueue`；`markRootEnqueue`（parallel_mark.cpp:62-68）直接 `obj->forwarded()` 解引用——任何垃圾值/非 GC 指针即炸（`0x1` → 读 `0xe`）。**对照**：同库栈扫描路径 `scanStackCandidate`（mark_sweep.cpp:160-204）对每个候选指针有完整「页内 + `registeredDescs_` desc 合法」双校验——防御不对称 | 崩溃栈与三处源码亲读 |
| **D3 伴生隐患** | `isGcPointerType`（TypeMap.cpp:32-39）按「`*` 结尾即 GC」粗判 → `Channel<T>*`（channel.h:11-24，普通 C++ struct，`::new` 分配，**非 GcObject**）等非 GC 指针被包进根链；具名协程形态「不崩」是布局巧合，且伴随 `tryMark()` CAS **写坏 Channel 头部**的未定义行为 | §8.2 另注；BuiltinRegistry.h:258-265 |

### 9.2 方案选型（否决项与理由）

| 候选 | 裁决 | 理由 |
| :--- | :--- | :--- |
| §5-A 包装 lambda 根不落地 | ❌ | §8.3 已实证推翻（`_h0_0` 注销正确，非根因） |
| §8.3-① 缩小根句柄作用域至 `*_raw` 活跃期 | ❌ | 依赖编译器活跃性分析结论，优化器行为不可约束、不可移植 |
| §8.3-② 让 `*_raw` 与句柄同寿命（防复用） | ✅ 落地为 **L1 Value 迁移** | 不去"防复用"（不可能），而是让句柄**自持值**——`val_` 是句柄自身成员，句柄 non-trivial 析构 → lifetime 内存储受语言规则保护，编译器**不可**复用（as-if 语义保证，非优化器巧合） |
| channel 类型从 `isGcPointerType` 排除 | ❌ 不做 | 治标不治本（ThreadChannel 等同类面仍在）、波及全部 channel 用例 `.get()` 生成（回归面大）；D3 经 L2 页判定即无害化。登记为可选后续项 |
| 仅 runtime 页判定（不动 CodeGen） | ❌ 不足 | 防崩不防悬垂：悬垂槽若读到「恰好落在 GC 页内的旧值」→ 误标/转发表错乱（无崩溃的内存不安全）；D1 必须根治 |

### 9.3 L1 定案：Ref→Value 单点追加（CodeGen，根治 D1）

**改法（一行追加，最小改动面）**：

```cpp
// 现状（Ref 模式：ptr_ref_ 指向 var_raw 槽 → 槽复用即悬垂）
T var_raw = init;
aura_rt::GcRootHandle<T> var(var_raw);
// 改后（Value 模式：val_ 自持，ptr_ref_ = &val_ → 句柄析构前恒有效；
//       var_raw 保留供 decltype / 后续引用，槽复用无害——GC 只读 val_）
T var_raw = init;
aura_rt::GcRootHandle<T> var(var_raw, aura_rt::GcRootScope::ThreadLocal);
```

**语义等价性论证**（为何 let 场景 Ref 的"追踪外部变量"能力可弃）：
- `let` 不可重赋值 → `var_raw` 构造后值不变 → Ref 的 `*ptr_ref_` 间接层无语义收益；
- `.get()`：Value 模式返回 `val_`，genIdentifier 的 `.get()` 生成**零改动**；
- compact 重写：Value 模式 GC 原位重写 `val_` → 挂起恢复后 `.get()` 恒最新（与 Ref 重写 `var_raw` 等价）；
- **唯一差异**：`var_raw` 不再被 GC 更新（保持旧值）——grep 实证生成代码无裸读 `var_raw` 的后续点（string 重赋值走 `s.get()->append(...)`，ExprAccess.cpp:262-280），实施步骤 1 复核封堵。

**库内正典佐证**（Value 两参构造已是 runtime 新近代码的事实标准，本修复 = 用户代码侧对齐）：
`ViewRoot` 内嵌句柄（iterator.h:86-87）、`collect_all` 的 guard/arrGuard（iterator.h:400-402）、`MapFnIter/FilterFnIter` 的 selfH（feature-06-D 修复）——全部 Value 形态。

**改点清单**（全部为 Ref 单参构造生成点；帧类型标注危险度）：

| # | 位置 | 场景 | 帧 | 危险度 |
| :-: | :--- | :--- | :--- | :--- |
| 1 | StmtLet.cpp:573-577 | 普通 let GC 指针根化 | 协程帧/栈 | **高**（bug-79 直击点） |
| 2 | StmtLet.cpp:562-572 | 泛型列表/函数值 let（decltype 形态） | 同上 | **高** |
| 3 | StmtLet.cpp:47-68 | record 解构字段根化 | 同上 | **高** |
| 4 | StmtLet.cpp:211-220 | record 字面量 let 根化 | 同上 | 中 |
| 5 | StmtLet.cpp:713-716 | const 根化（genConstStmt） | 同上 | **高** |
| 6 | DeclFun.cpp:200-215 | 函数 GC 指针形参根化 | 协程帧/栈 | **高**（具名协程函数同款风险，目前靠巧合不炸） |
| 7 | DeclFun.cpp:623-638, 721 | 方法 receiver + 形参根化 | 同上 | **高** |
| 8 | ExprClosure.cpp:935-936 | 旧路径闭包参数根化 | 协程帧 | **高** |
| 9 | ExprClosure.cpp:1237-1238 | 新路径 `__invoke` 参数包裹 | **协程帧** | **高**（feature-07 Step 4 协程闭包的前置加固） |
| 10 | StmtControl.cpp:386-388 | for-in channel `_ch` 根化 | **协程帧** | **高**（channel for-in 挂起同款窗口） |
| 11 | StmtControl.cpp:127-129 | for-in record 项绑定 | 协程帧/栈 | 中 |
| 12 | StmtControl.cpp:325-327 | Iterator 适配器 `_ah` | IIFE（同步） | 低（顺带） |
| 13 | StmtMatch.cpp:169, 214 | match 分支 Optional 绑定 | 协程帧/栈 | 中 |
| 14 | ExprGen.cpp:514, 588 附近 | 列表/record 字面量 `_raw` | IIFE（同步） | 低（顺带） |
| 15 | ExprClosure.cpp:93-107 | genGcRootedArgs IIFE 内 `_h` | IIFE（同步） | 低（顺带） |

**保持现状（不迁移）**：闭包捕获 init-capture（Global Value，本就 Value）；`ViewRoot`/`collect_all`/迭代器新类内嵌句柄（已 Value）。

### 9.4 L2 定案：根扫描页内判定（runtime，防御纵深，治 D2/D3）

**改动点**：`scanRootsOnly`（mark_sweep.cpp:88-91）`memcpy` 读值后、`markRootEnqueue` 前追加页判定：

```cpp
GcObject* obj;
std::memcpy(&obj, node->ptr_ref_, sizeof(GcObject*));
// bug-79 L2：根链值页内判定（与栈扫描 scanStackCandidate 防御对称）——
// 悬垂槽垃圾值（0x1 等）与非 GC 堆指针（Channel* 等）一律跳过，
// 消灭 markRootEnqueue 直接 forwarded() 解引用的未定义行为
if (obj && isGCAddress(obj)) markRootEnqueue(obj);
```

- **API 复用 + 缺口修补**：`GcHeap::isGCAddress`（compact.cpp:642-651，gc.h:509 声明）现查小页/中页/大页三链，**漏 LOS**（对照 mark_sweep.cpp:98 注释，栈扫描走 `los_.contains`）——实施时补 `los_.contains(p)` 分支（或抽公共 `isGcAddressFast` 供两路共用）。
- **可选加固（推荐但非必须）**：页内值再仿 `scanStackCandidate` 加 `registeredDescs_` desc 校验（防页内 mid-object 误标）；根链值理论恒为对象起点，最低配页判定即可，推荐配与栈扫描完全对称。
- **性能**：根链规模 = 活跃句柄数（数量级小），每根一次页链线性查找；`scanStackCandidate` 对**每个栈槽候选**做同类判定已是先例，停顿内占比可忽略。
- **生效面**：串行 `markPhase` 与并发标记 `startConcurrentGc`（复用 `scanRootsOnly`）同点覆盖。
- **对 D3 的效果**：`Channel*` 等非 GC 指针被页判定跳过 → 不再被 `tryMark()` 写坏头部、不再依赖布局巧合——「具名形态碰巧不炸」变「确定不炸」。

### 9.5 不改动清单

- `isGcPointerType` 的 Channel/ThreadChannel 排除（L2 已无害化，回归面不划算，登记后续可选）；
- 闭包捕获 init-capture / ViewRoot 内嵌句柄 / 迭代器新类内嵌句柄（已 Value 或 Global Value）；
- `relocateGlobalRootPtrs` 等 compact 既有机制（与本缺陷无因果）；
- GcRootHandle 的 Ref 模式实现本体（保留——for-in 循环项等「块作用域每轮重建」形态与运行时内部仍有合法用途；若实施步骤 1 grep 出 var 重赋值裸写 `_raw` 的生成点，该分支保持 Ref 并单独登记）。

### 9.6 实施步骤（建议单修复子 Agent 串行 4 步）

1. **步骤 0（复核封堵）**：grep 生成代码模板中全部 `*_raw` 直接引用（重点：var 重赋值路径、泛型上下文 `decltype(x_raw)` 之外的裸用）；确认无「Value 化后丢更新」的点，若有则该点保持 Ref 并登记。
2. **步骤 1（L1 低危顺带组）**：改点 \#12/\#14/#15（IIFE 同步窗口）→ 全量回归（基线 1274）+ used/1-6 → 确认 `.get()` 语义无漂移。
3. **步骤 2（L1 高危组）**：改点 \#1-#11、#13（协程帧/栈全部单次绑定点）→ 全量回归 + **ASAN 矩阵 4 例**（§4 全表）→ 非 ASAN 模式 used/1-6 + test.aura。
4. **步骤 3（L2 runtime）**：`isGCAddress` 补 LOS + `scanRootsOnly` 追加判定 → 专项负例：临时构造「根链含非 GC 指针（Channel*）+ 页外垃圾值」场景（可用 s4_5/s4_6 直接验证 0x1 不再崩，且 ASAN 全绿）→ 全量回归。
5. **步骤 4（收尾）**：回归全绿后更新本笔记（勾选 §6、追加「## 10. 修复记录」）；bug-79 状态 pending_fix → fixed。

### 9.7 回归验证（对 §6 的扩充）

- [ ] §6 原四项全过（s4_5/s4_6 ASAN 0 报警；s4_1/s4_2/s4_3/s4b 不回归；全量单测 + used/1-6）
- [ ] **Value 专项**：多页压实后 `.get()` 恒最新值断言（r 系列负例 20 轮——compact 重写 `val_` 路径）
- [ ] **L2 专项**：s4_5 生成代码中 `ch_raw` 槽复现读 `0x1`（可加临时日志确认）但 GC 不再崩、结果正确
- [ ] 非预期生成变化审计：全量单测中既有 codegen 断言（test_codegen_* 系列）零改动通过（`.get()`/`_raw` 形态未变）

### 9.8 风险与开放问题

|  #  | 风险/问题                                         | 缓解                                                                                                |
| :-: | :-------------------------------------------- | :------------------------------------------------------------------------------------------------ |
|  1  | Value 化后 `var_raw` 不再被 compact 更新（保持旧值），潜在裸读点 | 步骤 0 grep 封堵；现状实证无裸读（string 重赋值走 append）                                                          |
|  2  | var 可重赋值 GC 指针若有「裸写 `_raw`」的重赋值生成 → Value 丢根  | 步骤 0 复核；若有则该分支保持 Ref + 登记（重赋值即活跃，槽复用窗口天然小）                                                        |
|  3  | `isGCAddress` 线性页链遍历性能                        | 根链规模小 + `scanStackCandidate` 同款先例；如需可后续优化为页表哈希（不阻塞本修复）                                            |
|  4  | LOS 判定缺失是否已构成既有 bug（`isGCAddress` 现有调用点）      | 实施时排查 `isGCAddress` 现有调用方语义，补 LOS 后回归                                                             |
|  5  | 与 feature-07 的关系                              | L1 改点 \#9（`__invoke` 参数包裹）正是 feature-07 Step 4 协程闭包的前置加固——本修复先行落地，change.md §5.2/§5.4 相关描述实施时同步引用 |

---
**当前状态**：`2026-09-12` **已修复**（L1 Ref→Value 已在上轮落地；本轮完成 `__c_h` 设计冲突消解——A 方案 `__c_h` Value 化 + 捕获映射改走 `__c_h.get()`；L2 根扫描页判定已在位）。ASAN 6/6 用例 0 报警、单测 1302/0、used 1-6 全过。详见 **§10 修复记录**。

---

## 10. 修复记录（2026-09-12，A 方案续：`__c_h` 设计冲突消解）

### 10.1 本轮问题（上轮卡住点）

L1 主体 Ref→Value 单点追加已在上一子 Agent 落地（StmtLet.cpp 5 处、DeclFun.cpp:724、ExprClosure.cpp:103 等 15 处改点 + L2 mark_sweep.cpp:93 页判定），单测 1301/0。但 `ExprClosure.cpp:1324` 的 `__c_h` 陷入**双向皆崩**的设计冲突：

| 形态 | 结果 | 机理 |
| :--- | :--- | :--- |
| 保持 Ref（单参） | ❌ GC 崩 | `ptr_ref_` 指向协程帧局部 `__c`，帧销毁/槽复用后根链悬垂（即 §8 实证的 D1 同款） |
| 直接改 Value（A 方案原稿） | ❌ 崩 | compact 只重写 `__c_h.val_`，**裸 `__c` 永不更新** `→` body 内 `__c->cap_x` stale |

### 10.2 A 方案落地（实际改动 4 处）

核心洞察：`__c` 在 body 中只经**一条路径**进入生成代码——`currentClosureCaptures_` 映射串（`ExprClosure.cpp:1369`），body 内全部捕获访问（`genIdentifier`）都查这张表。故使「可更新的值」与「body 用的值」合一即可。

| # | 文件:行 | 改动 |
| :-: | :--- | :--- |
| 1 | `ExprClosure.cpp:1327` | `GcRootHandle<__closure_N*> __c_h(__c);` → `__c_h(__c, aura_rt::GcRootScope::ThreadLocal);`（Value 模式：`val_` 自持，compact 原位重写，析构 non-trivial 使存储不可被复用） |
| 2 | `ExprClosure.cpp:1373` | 捕获映射 `currentClosureCaptures_[k] = "__c->" + slot` → `"__c_h.get()->" + slot`（compact 后 `.get()` 恒为最新地址，与旧 Ref 原位重写语义等价） |
| 3 | `ExprCall.cpp:643-646` | `calleeIsClosureSlot` 判定**兼容新旧两前缀**（`__c_h.get()->cap_` 与 `__c->cap_`）——旧 lambda 路径 / 未迁移生成源可能仍产旧前缀，必须两收 |
| 4 | `ExprClosure.cpp:1320-1325`、`1366-1370` | 注释同步（说明 Value + `.get()` 新契约） |

**同类点排查结论（brief 改动 3 要求）**：`ExprClosure.cpp:1669`、`1828` 的 `auto* __c = static_cast<...>(__self); __c->cap_recv->member(args)` 为 `__mv_N::__invoke` 内**单表达式**——`__c` 赋值后立即用于一次成员调用，中间无 GC 点/alloc；且 receiver 槽 `cap_recv` 已由 desc 追踪。与 §8.2「形参槽后续仍被使用故不炸」同型，**非本缺陷形态，不做改动**。

**改动 4（裸读点排查）结论**：全仓 grep 15 处已 Value 化点的 `_raw` 引用，全部只有四种用途——声明、句柄构造实参、`decltype(_raw)`、`const_cast`/`ViewRoot` 构造实参；**无「Value 化后裸读旧变量」的形态**。

### 10.3 验证证据

| 项 | 结果 |
| :--- | :--- |
| **ASAN 主验证** | `s4_5_iodetector` ✅ `(empty —— no ASAN errors)` + Exit 0 + PASSED（原崩溃点）；`s4_6_control` ✅ 同 |
| **ASAN 对照** | `s4_1_escape` / `s4_2_multisuspend` / `s4_3_nested` / `s4b_iso_named` 四例全 ✅ 0 报警 |
| **body 捕获值正确性（非仅不崩）** | `s4_5` 输出 `S4_5 r=io-less|42`——压实后捕获槽 `tag`/`got` 读值正确；**新增 `r5_bug79_capture_after_compact`**（协程闭包 + 通道挂起 + 3 次 gc_force + 嵌套捕获）ASAN 输出 `R5 r=T79-5-9-7/T79579` ✅（`tag`=T79、`k`=5、`got`=9、`n`=7 全部读回正确，且同一批捕获槽压实后二次读取仍正确） |
| **全量单测** | 基线 1301/0 → **1302/0**（新增 `CodeGen.Bug79ClosureHandleValueModeNotRef`） |
| **codegen 断言同步** | 11 个用例断言期望串 `__c->cap_*` → `__c_h.get()->cap_*`（语义一一对应，纯前缀同步）；另主动同步 2 处未失败的陈旧断言（`test_codegen_generic.cpp:962`、`test_codegen_iface_view.cpp:206`），防虚假通过 |
| **used/1-6** | 6/6 compile=0、run 语义正确 |
| **不回归轮次** | `r1`/`r2`/`r3`/`r4`/`t3e_shallow`/`t3i_thread_norec_churn` 各 20 轮 = 120 轮零失败 |
| **build 模式** | 已清空 `build` + `runtime/build` 重配重建（常规 UCRT64，非 ASAN），aurac / libaura_rt.a / aura_tests 三者均已重建 |

### 10.4 遗留 / 风险

1. `ExprCall.cpp:646` 保留旧前缀兼容分支——Step 5 删旧路径后应复核是否可收敛为单前缀（当前故意两收，避免漏判）。
2. D3（`isGcPointerType` 按 `*` 结尾粗判导致 `Channel<T>*` 等非 GC 指针进根链）由 L2 页判定无害化，**未根治**，仍登记为可选后续项。
3. 承载证据的复现件：`_repro/f07_verify/r5_bug79_capture_after_compact.aura`（新增，ASAN 捕获值正确性）、`scripts/_asan_*.cpp`（ASAN 生成代码）。

