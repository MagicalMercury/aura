---
type: bug_report
module: runtime/gc
sub_module: markPhase 步骤 3 扫描 globalRoots_ 时只判非空即 markRootEnqueue(*rootPtr)，缺 isGCAddress 防御——与 bug-89（compact 步骤 3）同源不同路径
status:
  - fixed
severity:
  - medium
discover_date: 2026-10-17
discovered_by: Hermes（bug-89 修复批的承重测试构造过程实测暴露；主 Agent 复核确认）
related_issues:
  - bug-89（同源：globalRoots_ 槽值脏值 ⇒ UB；已修 compact 步骤 3，mark 路径未修）
  - bug-79（步骤 1 的 isGCAddress 加固出自该缺陷）
  - "[[bug-89-globalroots-update-missing-isgcaddress-guard]]"
  - "[[bug-79-coro-closure-channel-gc-dangling-root]]"
tags:
  - gc
  - hardening
  - mark
  - gc-root
  - gc_safety
---

# [x] 【`markPhase` 扫描 `globalRoots_` 时缺 `isGCAddress` 防御（与 bug-89 同源）】

**状态**：`[x] 已修复`（2026-10-17，见 §8）
**严重度**：**medium**
**发现场景**：bug-89 修复批构造「脏值 Global 根 + compact」用例时，脏值若在 `gc_force_major()` **之前**注入 ⇒ 崩溃帧不是 `compact()` 而是 `markPhase()` 的 `globalRoots_` 扫描（子 Agent 实测；主 Agent 复核源码确认）。

---

## 0. 一句话摘要

`GcHeap::markPhase()` 的**步骤 3**（从全局根出发标记，`runtime/gc/mark_sweep.cpp:125-134`）对 `globalRoots_` 的每一项**只判非空**就调 `markRootEnqueue(*rootPtr)`，而该函数内部对 `obj` 调用 `forwarded()`（读 `obj+12`）——**没有** `isGCAddress` 校验。⇒ 全局根槽是脏值（`0x1` 等页外地址）时，**标记阶段**即未定义行为（UB）。

这与 **bug-89**（`compact()` 的 `updateAllReferences()` 步骤 3）是**同一缺陷族**：同一条 `globalRoots_` 集合、同一个"脏值即 UB"的机理，只是**消费路径不同**（mark vs compact）。bug-89 修复只加固了 compact 侧，**mark 侧仍敞开**。

## 1. 现场（源码对照）

**mark 侧**（`runtime/gc/mark_sweep.cpp:125-134`）——**缺防御**：
```cpp
    // 3. 从全局根出发标记（运行时缓存 / interned 字符串）
    {
        std::lock_guard<std::mutex> lk(globalRoots_m_);
        for (auto* rootPtr : globalRoots_) {
            if (rootPtr && *rootPtr) {          // ⚠️ 只判非空；脏值直入
                markRootEnqueue(*rootPtr);      //    内部对 obj 调 forwarded() ⇒ UB
            }
        }
    }
```

**已验证加固的对照点**：
- `compact()` 步骤 1（线程局部根，`compact.cpp:341`）：`if (fieldPtr && *fieldPtr && isGCAddress(*fieldPtr))` ✔
- `compact()` 步骤 3（全局根，`compact.cpp:359`，bug-89 本次修复）：`if (obj && isGCAddress(obj) && obj->forwarded())` ✔
- `markPhase()` 步骤 3（全局根，`mark_sweep.cpp:130`）：**只有 `rootPtr && *rootPtr`** ✖

## 2. 影响与触发条件

- **现状不可复现为独立故障**：正常流程下 `globalRoots_` 槽值有效（注册/注销配对）。
- **但它是"同一张安全网上的洞"**：bug-89 把 compact 侧从"脏值即 UB"改成"安全跳过"，mark 侧仍是 UB ⇒ 任何导致 `*rootPtr` 悬垂的缺陷（并发注册/注销竞态、值未更新/未作废、注销路径异常）在 **mark 阶段**会以 UB 形式表现（SIGSEGV 或读到垃圾后误标记）。
- **对 enum v1.1（L-1）直接影响**：L-1 会新增 `GcRootHandle<T>{…, GcRootScope::Global}` 形态（`change.md` 改动 7/8），即**扩大 `globalRoots_` 的写入面**。若 v1.1 的生成期守卫/运行期逻辑出现任何脏值写入，mark 侧将**先于** compact 侧崩溃 ⇒ 必须在 L-1 落地前闭合。
- **实测证据（bug-89 批）**：脏值在 `gc_force_major()` 之前注入 ⇒ 用例崩溃帧为 `markPhase` 的 `globalRoots_` 扫描，而非 `compact()` 步骤 3 ⇒ 证明该路径真实可达且优先命中。

## 3. 修法建议

与 bug-89 对称（`isGCAddress` 只做页区间比较 + `los_.contains`，**不解引用**，对任意值安全；声明 `runtime/gc/gc.h:509`）：
```cpp
for (auto* rootPtr : globalRoots_) {
    if (rootPtr && *rootPtr && isGCAddress(*rootPtr)) {   // ← 补 isGCAddress
        markRootEnqueue(*rootPtr);
    }
}
```
- 只改这一处判据；`markRootEnqueue` 本体、`globalRoots_` 结构与注册/注销逻辑**不动**。
- 同类检查（顺带核对、不作为本缺陷范围）：`mark_sweep.cpp` 里其它"从槽值直接取对象"的根扫描点（若有）应一并确认是否需要同款守卫——**先列表，再逐点判**，禁止顺手扩大改动面。

## 4. 验证建议

- **单测**（承重）：`GcRootHandle<GcString*>{nullptr, GcRootScope::Global}` → `set(0x1)` → `gc_force_major()` ⇒ 修前 SIGSEGV（崩溃帧 = mark 的 `globalRoots_` 扫描）、修后安全跳过且后续 GC 正常；三步对照（去守卫必须转红）。
- **回归**：全量单测 0 failed（当前基线 **1585/1585**）+ `--filter=Examples` **17/17** + `example/test.aura` md5 `5f1760a5a360f4139d176434775abddd`。
- 双目标重建：`cmake --build runtime/build` **和** `cmake --build test/build`。

## 5. 关联

- **bug-89**（已修，2026-10-17）：`compact()` 侧同族缺口已加固；本项是 mark 侧余留。
- **bug-79**：`isGCAddress` 防御体系的起点（步骤 1 / `scanRootsOnly`）。
- **enum v1.1（`change.md` L-1）**：新增 `Global` 根形态 ⇒ 本项应作为其前置闭合（与 bug-89 同批处理最经济）。
- **feature-19（io offload）**：跨线程 GC 根同样依赖该守卫体系。

## 6. 调研确认（2026-10-17，只读调研批 + 主 Agent 复核）

**裁决：确认（机制级）**。主 Agent 独立复跑：脏值 `0x1` 进 `Global` 根 → `gc_force_major()` ⇒ **rc=139**，
gdb 帧逐字一致：

```
#0  GcObject::forwarded (this=0x1)                 runtime/types.h:190
#1  GcHeap::markRootEnqueue (obj=0x1)              runtime/gc/parallel_mark.cpp:63
#2  GcHeap::scanRootsOnly (youngOnly=false)        runtime/gc/mark_sweep.cpp:131
#3  GcHeap::markPhase                              runtime/gc/mark_sweep.cpp:65
```

对照：同一脏值**只**调 `compact(All)`（bug-89 已修）⇒ **rc=0**、槽值仍为 `0x1`（守卫确实跳过，非"碰巧读 false"）。

### 6.1 性质订正：「放大器」而非独立故障
- 正常注册/注销路径**自身造不出**脏值（`roots.cpp:145-158` 配对，`globalRoots_m_` 两侧持锁，无竞态窗口）；
- 但**已实测**它作为上游缺陷的下游表现真实出现：bug-111 的机制把 `oomError_.message` 留在已 `VirtualFree` 的页上
  （gdb：`oomError_.message = 0x180000`）⇒ 下一轮 GC 正是崩在 `:131`。⇒ §2「现状不可复现」应理解为
  「**没有独立可复现路径，但不是纯理论**」。

### 6.2 同族清点新发现（本批已穷举，依据）
**外部槽**类共 6 处，其中**缺守卫 = 3 处**：

| # | 位置 | 现状 |
| :-: | :--- | :--- |
| 1 | `mark_sweep.cpp:129-132`（本缺陷） | `if (rootPtr && *rootPtr)` ✖ |
| 2 | **`mark_sweep.cpp:156-160`（步骤 5，oomError 四指针）** | `if (oomError_.kind) markRootEnqueue(...)` ✖ **本次新发现** |
| 3 | **`compact.cpp:833-840`（中页路径·全局根）** | `if (obj && obj->forwarded())` ✖ **bug-89 未覆盖的同族副本**（bug-89 §4 提醒过、本次确认仍缺） |
| 4-6 | `mark_sweep.cpp:88-93` / `compact.cpp:341` / `compact.cpp:359` | ✔ 已有守卫 |

⚠️ **注意 `oomError_.stack`**（**本节原判已作废，见 §8.1**）：原判「它是裸栈指针，直接套 `isGCAddress`
会把它恒跳过（语义变化）⇒ 该处需单独判断」**不成立**——`stack` 的静态类型是 `Array<uint64_t>*`，
而 `template <typename T> struct Array : GcObject`（`runtime/builtin/array.h:97`）⇒ 它是**真正的
GC 对象指针**。故该处已与 `kind/message/file` **同等加守卫**（标记语义不变）。

**不建议扩面**：「内部集合」类（`rememberedSet_`/`youngObjects_`/`oldObjects_`/`satbQueue_`）由 GC 自己在 STW 内维护，
属另一层不变式；「堆内字段」类属 bug-29/30 一族。

### 6.3 修复范围（P1，与 bug-89 对称）
1. `mark_sweep.cpp:130` — `if (rootPtr && *rootPtr && isGCAddress(*rootPtr))`
2. `mark_sweep.cpp:157-160` — 四个 `oomError_.*` 入口加 `isGCAddress(...)`（`stack` 单独裁定）
3. `compact.cpp:836` — `if (obj && isGCAddress(obj) && obj->forwarded())`

**开销提示**：`isGCAddress` 是 O(页数) 区间扫描（`compact.cpp:663-673`）；`:130` 在 `globalRoots_m_` 锁内调用。
enum v1.1 的 L-1 若显著增加全局根数量，需要在修复后复测该路径开销（不阻断，先记录）。

---

## 8. 修复记录（2026-10-17）

### 8.1 `oomError_.stack` 的性质核定（**§6.2 的 ⚠️ 作废**）

brief 要求「先读 `struct OomError`（`gc.h`）定性质」。实测该结构**不存在**：

- `runtime/gc/gc.h:621` 的成员声明是 `Error oomError_;`（不是 `OomError`）；
- `struct Error : GcObject`（`runtime/types.h:257`），其 `stack` 字段的静态类型是
  `Array<uint64_t>*`（`runtime/types.h:265`，注释「逻辑栈快照·紧凑帧」）；
- `template <typename T> struct Array : GcObject`（`runtime/builtin/array.h:97`）
  ⇒ **`stack` 就是真正的 GC 对象指针**（由 `captureLogicalStack()` 经 GC 分配）。

⇒ §6.2「直接套 `isGCAddress` 会把它恒跳过（语义变化）」**不成立**：合法值恒落在
GC 页区间内，守卫只过滤脏值/悬垂值。故 `stack` 与 `kind/message/file` **同等加守卫**，
标记语义不变（这一点是本批最关键的裁定反转，已写进源码注释）。

### 8.2 改动（3 处，全部只改判据，不动 `markRootEnqueue` / 注册注销逻辑）

| # | 位置 | 改后 |
| :-: | :--- | :--- |
| A1 | `runtime/gc/mark_sweep.cpp:130` | `if (rootPtr && *rootPtr && isGCAddress(*rootPtr))` |
| A2 | `runtime/gc/mark_sweep.cpp:157-160` | 四个 `oomError_.*` 各自 `&& isGCAddress(...)`（含 `stack`，依据见 §8.1） |
| A3 | `runtime/gc/compact.cpp:836` | `if (obj && isGCAddress(obj) && obj->forwarded())` |

内部集合（`rememberedSet_` / `youngObjects_` / `oldObjects_` / `satbQueue_`）与堆内字段类
**未触碰**（按 §6.2 的边界）。

### 8.3 承重测试 `test/rt/test_gc_bug110.cpp`（4 用例，已登记 `test/CMakeLists.txt`）

| 用例 | 压的点 | 做法 |
| :--- | :--- | :--- |
| `DirtyGlobalRootSurvivesMarkPhase` | A1 | `GcRootHandle<GcString*>{nullptr, Global}` → `set(0x1)` → `gc_force_major()`；断言不崩、槽值仍为 `0x1`、随后 intern 正常 |
| `DirtyOomErrorPointersSurviveMarkPhase` | A2 | 保存/恢复 `oomError_` 四指针（脏值 `0x1..0x4`）+ `incCompactSuspend()`（把压力**只**压在 mark 步骤 5，避开范围外的 compact 侧 `updatePtr`）→ `gc_force_major()` |
| `DirtyGlobalRootSurvivesMediumPageReferenceUpdate` | A3 | 脏全局根 + 直接调 `updateMediumPageReferences()`（`:836` 所在函数） |
| `ValidGlobalRootStillProcessed` | 反向回归 | 有效根走 A1/A2/A3 与 `:359` 四条路径，仍能读回内容 |

### 8.4 三步对照（真跑，rc 与帧）

| 步 | 操作 | 结果 |
| :-: | :--- | :--- |
| A1-红 | 去掉 `:130` 的 `&& isGCAddress(*rootPtr)`（仅此一处） | `--filter=GcBug110` ⇒ **rc=139**；帧 `GcObject::forwarded` ← `markRootEnqueue` ← `scanRootsOnly` ← `markPhase` ← `majorGc` ← `forceGc` |
| A1-绿 | 恢复（md5 回 `6c6d375d…`） | 4/4 PASSED，rc=0 |
| A2-红 | 去掉四指针的 `&& isGCAddress(...)` | **rc=139**；同一形状帧（该用例只脏化 `oomError_`，根槽干净 ⇒ `scanRootsOnly` 内唯一可崩入口即步骤 5） |
| A2-绿 | 恢复（md5 回 `6c6d375d…`） | 4/4 PASSED，rc=0 |
| A3-红 | 去掉 `:836` 的 `isGCAddress(obj) &&` | **rc=139**；帧 `GcObject::forwarded` ← `updateMediumPageReferences` ← `GcBug110_DirtyGlobalRootSurvivesMediumPageReferenceUpdate` |
| A3-绿 | 恢复（md5 回 `9efff1a4…`） | 4/4 PASSED，rc=0 |

### 8.5 回归

- 双目标重建：`cmake --build runtime/build` + `cmake --build test/build` 均 0 error；
- 全量 **1591/1591 passed, 0 failed**（连跑 2 次，rc=0）；
- `--filter=Examples` **17/17**，rc=0；`example/test.aura` md5 `5f1760a5a360f4139d176434775abddd`（未变）；
- 未改 `src/`、`example/`、`change.md`、`READMEs/`、`plan/`；无 git 写操作。

### 8.6 遗留（不属本缺陷，仅登记）

- `compact.cpp` 中页/小页路径的**弱引用 / `oomError_` 更新**（`:388`、`:393-412`、`:825`、`:861`、`:865-883`）仍是
  `updatePtr` 的裸 `forwarded()`（调研 §2 Q4 的 P2 清单）。本批按 brief **未扩面**：
  它们的脏值来源与本缺陷同族，但属「另一层防线」，需单独裁定。
- `isGCAddress` 为 O(页数) 线性扫描；`:130` 在 `globalRoots_m_` 锁内调用。enum v1.1（L-1）若显著增加全局根
  数量，需复测该路径开销（与本批无关的既有提示，仍然有效）。
