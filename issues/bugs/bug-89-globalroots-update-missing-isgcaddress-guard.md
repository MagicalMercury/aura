---
type: bug_report
module: runtime/gc
sub_module: updateAllReferences 步骤 3（更新 globalRoots_）遍历时直接解引用 *rootPtr，缺 isGCAddress 防御——与步骤 1（bug-79 已加固）不对称
status:
  - fixed
severity:
  - medium
discover_date: 2026-09-27
discovered_by: Hermes（bug-86 形态② 定位时复核 compact 三条根/引用更新路径，附带发现）
related_issues:
  - bug-86（同域：compact 的根/引用更新路径；形态② 根因是 intern L1 缓存作废时机，非本路径）
  - bug-79（步骤 1 的 isGCAddress 加固即出自该缺陷）
  - "[[bug-86-sync-thread-gc-force-stw-deadlock]]"
  - "[[bug-79-coro-closure-channel-gc-dangling-root]]"
tags:
  - gc
  - hardening
  - compact
  - gc-root
  - gc_safety
---

# [x] 【`updateAllReferences` 步骤 3 更新 `globalRoots_` 时缺 `isGCAddress` 防御（与步骤 1 不对称）】

> **阻塞（2026-10-03）**：作为 `[[feature-19-io-offload-decoupling]]` 的**前置依赖**，与其同批处理（跨线程 GC 根需要该守卫）。
> **解除阻塞（2026-10-17，主人裁定 T1）**：作为 **enum v1.1 的前置依赖**先行修复（`change.md §6 Step -1`）；不再等 feature-19 同批。

**状态**：`[x] 已修复`（2026-10-17，T1 前置；修复记录见 §8）
**严重度**：**medium**
**发现场景**：bug-86 形态② 定位过程中，复核 `compact()` 的三条根/引用更新路径时附带发现

---

## 0. 一句话摘要

`GcHeap::updateAllReferences()` 的**步骤 3**（遍历 `globalRoots_` 更新全局根）直接对 `rootPtr` 解引用取出 `obj` 并调用 `obj->forwarded()`，
**没有** `isGCAddress` 校验；而同函数的**步骤 1**（遍历 thread-local 根链表）在 bug-79 的 L2 加固中**已经**加入该防御。
两者**不对称**：当 `*rootPtr` 因任何原因成为悬垂/脏值时，步骤 3 会走到**未定义行为（UB）**，而非像步骤 1 那样安全跳过。

## 1. 现场（源码对照）

**步骤 3**（`runtime/gc/compact.cpp:342-354`）——**缺防御**：
```cpp
// 3. 更新 globalRoots_（使用 memcpy 避免 strict-aliasing 问题：
//    globalRoots_ 实际指向 GcString* 等派生类型，不能通过 GcObject** 直接写入）
{
    std::lock_guard<std::mutex> lk(globalRoots_m_);
    for (auto* rootPtr : globalRoots_) {
        GcObject* obj;
        std::memcpy(&obj, rootPtr, sizeof(GcObject*));
        if (obj && obj->forwarded()) {                 // ⚠️ 直接解引用 obj；脏值即 UB
            GcObject* newPtr = obj->forwardingPtr();
            std::memcpy(rootPtr, &newPtr, sizeof(GcObject*));
        }
    }
}
```

**步骤 1**（`runtime/gc/compact.cpp:330-337`）——**已加固**：
```cpp
for (GcRootHandleBase* node = list->head; node; node = node->next_) {
    GcObject** fieldPtr = node->ptr_ref_;
    // bug-79 L2（对称加固）：页外/悬垂槽值跳过——与 scanRootsOnly 同款防御。
    // updatePtr 对 *fieldPtr 直接调用 forwarded()，脏值（0x1 等）即未定义行为。
    if (fieldPtr && *fieldPtr && isGCAddress(*fieldPtr)) {
        updatePtr(*fieldPtr);
    }
}
```

⇒ **步骤 1 有 `isGCAddress(*fieldPtr)` 三重防御；步骤 3 只有 `obj && obj->forwarded()`。**

## 2. 影响与触发条件

- **不是当前可复现缺陷**：正常流程下 `globalRoots_` 中的值应始终有效（根注册/注销配对 + 各更新路径正确）。
- **但缺失防御的代价不对称**：
  - 步骤 1 遇到脏值 ⇒ **安全跳过**（并保有后续机会修正）；
  - 步骤 3 遇到脏值 ⇒ `obj->forwarded()` **读任意地址** ⇒ UB。后果分两类：
    - 直接 SIGSEGV；或
    - 读到垃圾后 `forwarded()` 恰好为 false ⇒ **静默跳过更新** ⇒ 该全局根**永久悬垂** ⇒ 在更晚的时点以更难定位的形式崩溃。
- **现实风险来源**：任何使 `*rootPtr` 悬垂的缺陷（例如 bug-86 形态② 那类"值未更新/未作废"缺陷、注销路径异常、并发注册/注销竞态）都会把这条路径从"干净失败"退化为 UB。

## 3. 修法建议

给步骤 3 补上与步骤 1 同款的防御（保持对称）：
```cpp
GcObject* obj;
std::memcpy(&obj, rootPtr, sizeof(GcObject*));
if (obj && isGCAddress(obj) && obj->forwarded()) {     // ← 补 isGCAddress
    GcObject* newPtr = obj->forwardingPtr();
    std::memcpy(rootPtr, &newPtr, sizeof(GcObject*));
}
```
⚠️ 需确认 `isGCAddress` 的开销（它在步骤 1 的热路径上已在用 ⇒ 预期可接受）。

**同类检查（本次未做）**：`compact.cpp` 中另有中页/大页路径的 `globalRoots_` 更新（约 `:810-820`、`:970-990` 区域）——
**需一并核对是否同样缺防御**；本笔记只登记了普通路径。

## 4. 验证建议

- **单测**：构造 `globalRoots_` 含脏值（例如 `registerGlobalRoot` 一个随后被写坏的槽）⇒ 修前应 UB/崩溃、修后应安全跳过。
- **回归**：修完跑全量单测（1364+）与 `example/used/1-6`。

## 5. 关联

- **bug-86**：同域（`compact()` 的根/引用更新路径）。形态② 的根因是 **intern L1 缓存作废时机**，**不是**本路径；本项是复核该路径时的附带发现。
- **bug-79**：步骤 1 的「页外/悬垂槽值跳过」加固即出自该缺陷（其 L2 条目写明"脏值（0x1 等）即未定义行为"）。

## 8. 修复记录（2026-10-17，T1 前置）

**根因**：`GcHeap::updateAllReferences()` **步骤 3**（`runtime/gc/compact.cpp:349-365`）对 `globalRoots_`
槽值只做 `obj &&` 非空判定即调 `obj->forwarded()`（读 `obj+12` 的 flags）；而同函数**步骤 1**
（`compact.cpp:341`）已按 bug-79 L2 加固为 `fieldPtr && *fieldPtr && isGCAddress(*fieldPtr)`。
两者不对称 ⇒ 槽值为脏值（`0x1` 等页外地址）时步骤 3 是未定义行为（读任意地址）。

**修法（唯一改动 = 1 行判据，`compact.cpp:359`）**：
```cpp
if (obj && isGCAddress(obj) && obj->forwarded()) {   // [FIX-BUG89] 与步骤 1 对称
```
- `isGCAddress(const void*)`（声明 `runtime/gc/gc.h:509`，实现 `compact.cpp:660-670`）只做
  小页/中页/大页区间比较 + `los_.contains`，**不解引用** ⇒ 对任意值安全；
- 保留 `memcpy` 读法（`globalRoots_` 元素可能指向 `GcString*` 等派生类型，不能直接 `GcObject**` 写入）；
- `compact.cpp` 其余 14 处 `forwarded()` 调用点**未动**；步骤 1、`isGCAddress` 本体、`globalRoots_`
  结构与注册逻辑**未动**。

**承重测试**：新增 `test/rt/test_gc_bug89.cpp`（套件 `GcBug89`，2 例），登记 `test/CMakeLists.txt`：
1. `DirtyGlobalRootSlotSurvivesCompactAll` —— `GcRootHandle<GcString*>{nullptr, Global}`，先
   `gc_force_major()`，再把槽值置 `0x1`，再 `compact(CompactScope::All)`；断言【进程存活】+
   【槽值未被改写（= 步骤 3 真正跳过的证据）】+ 【随后堆仍可分配/读取】。
2. `ValidGlobalRootSlotUpdatedByCompactAll` —— 同形态换有效对象，`compact(All)` 后内容仍正确
   且**地址确已改变**（实测 `before=0x…00e0 → after=0x…00e0`，差一页 0x1000）⇒ 守卫没把正常
   转发路径挡掉。

⚠️ **调用次序是承重的**（两轮实测得出，已写进测试文件头注释；理由见下方「范围外观察」）：
必须 **先 `gc_force_major()` 再 `compact(All)`**，且脏值必须在 `gc_force_major()` **之后**注入。
否则：① 提前注入 ⇒ 先崩在标记阶段的 `globalRoots_` 扫描（`mark_sweep.cpp:130`，缺同款守卫）；
② 裸调 `compact(All)`（不经标记/清扫）⇒ 本轮用例自身能过，但**后续 GC** 崩在
`updateObjectAllFields`（全量跑必现 rc=139）。

**三步对照（真跑；md5 为 `runtime/gc/compact.cpp` 整文件，重建均覆盖 `runtime/build` + `test/build`）**

| 步 | 动作 | 结果 | 退出码 / md5 |
|---|---|---|---|
| ① | 修复版 | `--filter=GcBug89` **2 passed, 0 failed**；全量 **1585/1585** | rc=0；`80d4456781188797d0e59edb124ef8cc` |
| ② | **临时去掉** `isGCAddress(obj) &&`（保留注释）→ 两处重建 | `--filter=GcBug89` **SIGSEGV**（stdout 0 字节，崩溃前无输出）。gdb 栈：`GcObject::forwarded()` ← `GcHeap::updateAllReferences()` ← `GcHeap::compact()` ← `compactAll()` ← `GcBug89_DirtyGlobalRootSlotSurvivesCompactAll()` | **rc=139**；`b61f1ce898bd28049aa9008ea0bc0563` |
| ③ | 恢复守卫 → 两处重建 | `--filter=GcBug89` **2 passed, 0 failed**；全量 **1585/1585** | rc=0；md5 回到 `80d4456781188797d0e59edb124ef8cc`（=①） |

⇒ ② **确定性转红**（2/2 复现 SIGSEGV，不是静默跳过），且崩溃帧正是步骤 3 的 `forwarded()`
⇒ 用例对本次修复**有判别力**（非装饰品）。

**回归统计**
- 全量单测：基线 **1583 / 1583** → 现 **1585 / 1585，0 failed**（+2 = 本笔记新增用例）
- `--filter=Examples`：**17 / 17 passed，0 failed**（`example/used/1.aura`…`6.aura` 逐文件登记）
- `example/test.aura` md5：`5f1760a5a360f4139d176434775abddd`（未变）

**范围外观察（本批未修，仅记录，待主人裁定/另行登记）**
1. **标记阶段的同款缺失**：`mark_sweep.cpp:126-134` 的 `globalRoots_` 扫描只判非空就
   `markRootEnqueue(*rootPtr)`（内部首行即 `obj->forwarded()`）⇒ 脏值在 **mark 处**即 UB，
   与本缺陷同源不同路径。
2. **裸调 `compact(All)` 会破坏后续 GC**：不经 `markPhase` 直接 `compact(CompactScope::All)`
   （即使对象列表随后被正确更新）会让**下一轮** GC 崩在 `updateObjectAllFields`（E7 实测：
   `--filter=GcBug89` 单跑绿、全量跑到中间 rc=139；加 `gc_force_major()` 前置后全量绿 ——
   E8）⇒ `compact()` 缺少「必须紧跟标记/清扫」的契约断言/自检。
