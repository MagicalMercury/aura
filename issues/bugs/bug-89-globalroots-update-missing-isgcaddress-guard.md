---
type: bug_report
module: runtime/gc
sub_module: updateAllReferences 步骤 3（更新 globalRoots_）遍历时直接解引用 *rootPtr，缺 isGCAddress 防御——与步骤 1（bug-79 已加固）不对称
status:
  - pending_fix
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

# 【`updateAllReferences` 步骤 3 更新 `globalRoots_` 时缺 `isGCAddress` 防御（与步骤 1 不对称）】

**状态**：`[ ] 未修复`（**加固项** —— 非当前可复现缺陷，属防御性收口）
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
