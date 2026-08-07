# compact 全局根地址重定位修复（堆内 ValueGlobal rootPtr 悬垂）

> 工作流：起草 issue 实现草案（工作流 2）
> 提出时间：2026-08-06
> 状态：**已实现并验证（2026-08-06）**。小页 + 中页 + 大页（sweepLargePages）三条搬运路径全部修复（大页为实施期补充，见 §4.2 改动 E / §11 记录）；iter_gc_test 全 5 用例 + ASAN 通过，test.aura 全量回归通过。
> 来源：迭代器 GC 修复（plan/迭代器GC安全修复.md）P0 测试 `iter_gc_test [2] test_closure_capture` 暴露的 **compact GC 缺陷**（ASAN access-violation @ markPhase）。
> 关联 issue：[TODO.txt](file:///d:/you/Aura/TODO.txt) [五] P0「compact 不重定位堆内 globalRoots rootPtr 地址」。

---

## 1. 元信息

| 项目 | 内容 |
| ---- | ---- |
| Plan 标题 | compact 全局根地址重定位修复（堆内 ValueGlobal rootPtr 悬垂） |
| 相关模块 | `runtime/gc/compact.cpp`、`runtime/gc/gc.h` |
| 触发场景 | B+W 迭代器视图化（闭包捕获 GcRootHandle 首次进入 GC 堆） |
| 优先级 | P0（实际崩溃，阻塞迭代器 GC 修复验收） |

---

## 2. Objectives

修复 compact GC 的一个悬垂根缺陷：**闭包捕获的 `GcRootHandle`（ValueGlobal 模式）被存进 GC 堆对象后，compact 搬运对象只更新根的值、不重定位 `rootPtr`（`&val_`）地址**，导致旧地址悬垂、下轮 GC 读到被复用内存而崩溃。修复后 compact 利用已有的 oldAddr→newAddr 映射把小页/中页搬运对象的内部根指针重写为新地址，使"GC 堆内的 GcRootHandle"与"栈/静态区的 GcRootHandle"一样安全。

---

## 3. Current State Summary（分析报告）

### 3.1 Bug 形态与复现

`iter_gc_test [2] test_closure_capture`：`make_map` 的 lambda 值捕获 `GcRootHandle<GcString*>`（GcRootScope::Global → ValueGlobal 模式）。lambda 对象存在 GC 堆内 `MapIter::fn_` 中；按 [handles.h:36-38](file:///d:/you/Aura/runtime/gc/handles.h#L36-L38)，ValueGlobal 模式构造时 `registerGlobalRoot(ptr_ref_)`，`ptr_ref_ = &val_` **指向 GC 堆对象内部**。

ASAN：`access-violation ... markPhase`（读 `0x1f354d601e0`）。
gdb 铁证：`globalRoots_` 中 `root[4] rootPtr=0x1801e0 val=0x900000008`，`0x1801e0` 处内存已是 `Array<int32_t>` 内联数据——即 rootPtr 落在**已被回收并复用**的旧对象内部。

### 3.2 崩溃链路逐步推演（T0 → T4）

先明确本 bug 的三个易混淆概念：

| 符号 | 含义 | 类型 |
| ---- | ---- | ---- |
| `val_` | GcRootHandle 内部的指针**存储槽位**，存的是被保护对象的地址 | `GcString*`（8B） |
| `rootPtr`（= `&val_`） | 指向槽位的**地址**（globalRoots_ 里存的就是它） | `GcObject**` |
| `*rootPtr` | 从槽位读出的值 = 被保护对象的地址 | `GcObject*` |

> 一句话总结：**compact 更新了"槽位里存的值"（`*rootPtr`），却没有更新"槽位自己住在哪"（`rootPtr`）。槽位随对象搬走后，globalRoots_ 记的地址就指向了别人的内存。**

**T0 构造：handle 进入 GC 堆**

```cpp
GcRootHandle<GcString*> h(s, GcRootScope::Global);  // ValueGlobal 模式
it.map([h](int v) { ... });                          // lambda 值捕获 h 的副本
```

- lambda 副本 `h'` 按 [handles.h:83-97](file:///d:/you/Aura/runtime/gc/handles.h#L83-L97) 拷贝构造：`ptr_ref_ = &h'.val_`，并 `registerGlobalRoot(&h'.val_)` → `globalRoots_` 里存入 **`&h'.val_` 这个地址**。
- `h'` 是 `MapIter::fn_` 的成员，MapIter 是 GC 堆对象（`gcConstruct` 分配）→ **`&h'.val_` 落在 GC 堆内**（gdb 实测 offset 约 24）。

状态：`globalRoots_ = [ ..., &h'.val_(=MapIter旧地址+24), ... ]`

**T1 第一次 GC（纯 mark-sweep）：一切正常**

markPhase 遍历 globalRoots_：读 `*rootPtr`（从堆内槽位读出被捕获字符串地址）→ 对象存活。此时无对象移动，槽位地址有效。

**T2 compact：对象搬走，槽位地址被遗弃**（缺陷点）

按 [compact.cpp:47-54](file:///d:/you/Aura/runtime/gc/compact.cpp#L47-L54) 四步：

1. `computeForwardingAddresses`：MapIter 选中搬运，`compactEntries_` 记录 `{oldAddr=旧地址, newAddr=新地址, size, ...}`；`setForwardingPtr` 把对象头部 desc 覆盖为转发指针。
2. `updateAllReferences` 步骤 3（[L317-329](file:///d:/you/Aura/runtime/gc/compact.cpp#L317-L329)）：对每个 rootPtr 做 `memcpy`：
   - 读 `*rootPtr` → 得到被捕获字符串的旧地址；
   - 若字符串也 forwarding → **写回新地址**（值更新成功）；
   - **rootPtr 本身（`&h'.val_` 旧地址）没有任何改动** ← 就是这里漏了
3. `copyObjectsToNewLocations`：MapIter 整体 memcpy 到新地址（`h'.val_` 槽位从此住在 `新地址+24`）。
4. `rebuildPageList`：**释放 MapIter 的旧页**。

此刻：`globalRoots_` 里的 rootPtr 仍是 `MapIter旧地址+24`，但那个地址已经不是 `h'.val_` 了。

**T3 旧页复用：槽位原址变成别人的数据**

后续 `Array<int32_t>` 分配 bump 到旧页，往旧地址写入数据。gdb 实测 `0x1801e0` 处看到 `0x0000000900000008`（Array 内联元素）——正是 globalRoots_ root[4] 记的悬垂地址。

**T4 第二次 GC（markPhase）：读到垃圾 → 崩溃**

markPhase 再次遍历 globalRoots_：`memcpy(&obj, rootPtr)` 从旧地址读出 `0x900000008`（数组数据，碰巧像地址）→ 当作 `GcObject*`，继续访问 `obj->marked()` / `obj->desc` → deref 非法内存 → **ASAN access-violation**（崩溃点 [mark_sweep.cpp](file:///d:/you/Aura/runtime/gc/mark_sweep.cpp) markPhase）。

内存视角图示：

```
T2 前：                    T2 后（对象已搬走）：          T3 后（旧页被复用）：
┌─ globalRoots_ ─┐        ┌─ globalRoots_ ─┐            ┌─ globalRoots_ ─┐
│ rootPtr ───────┼──►     │ rootPtr ───────┼──(未改)──► │ rootPtr ───────┼──► 旧地址(悬垂)
└────────────────┘        └────────────────┘            └────────────────┘
旧堆对象：                 新堆对象：                     旧地址内存已被 Array 占用：
┌────────────────┐        ┌────────────────┐            ┌────────────────┐
│ MapIter        │        │ MapIter        │            │ 0x0000000900000008 │ ← 垃圾
│ offset24: val_ │◄─      │ offset24: val_ │◄─(值已更新) │ 0x900000008 ... │
└────────────────┘        └────────────────┘            └────────────────┘
```

**为什么"字符串值"不崩而"槽位地址"会崩**：被捕获的 GcString 是**值**，compact 通过解引用 rootPtr 更新了它；而 rootPtr 是**地址**，没有任何代码改写它。值跟地址是两个不同的量——修复方案正是补上"地址的更新"。

### 3.3 根因（代码路径）

compact 主流程 [compact.cpp:47-54](file:///d:/you/Aura/runtime/gc/compact.cpp#L47-L54)：

```cpp
computeForwardingAddresses(scope);
updateAllReferences(scope);        // 先更新引用（对象仍在旧地址，desc 已覆盖为 forwardingPtr）
copyObjectsToNewLocations(scope);  // memcpy 到新地址 + 恢复 desc/flags + clear compactEntries_
rebuildPageList(scope);            // 释放旧页
```

- `updateAllReferences` 步骤 3（[compact.cpp:317-329](file:///d:/you/Aura/runtime/gc/compact.cpp#L317-L329)）遍历 `globalRoots_`，对每个 `rootPtr` 仅 `memcpy` 更新 `*rootPtr`（值），**不改写 rootPtr 本身**（地址）。
- 同理 `updateMediumPageReferences`（[compact.cpp:652-663](file:///d:/you/Aura/runtime/gc/compact.cpp#L652-L663)）。
- 对象被搬到新地址后，旧 `&val_` 地址悬垂；`rebuildPageList` 释放旧页后内存被复用 → 下轮 GC 遍历 `globalRoots_` 时 deref 垃圾指针 → 崩溃。

### 3.4 为什么旧代码不崩

此前 GcRootHandle 值持有（ValueGlobal）只出现在**栈 / 静态区**（如 [string.cpp:69](file:///d:/you/Aura/runtime/builtin/string.cpp#L69) `_cache[2048]`、[string.cpp:103-104](file:///d:/you/Aura/runtime/builtin/string.cpp#L103-L104) 静态 `_t/_f`），`&val_` 地址不受 compact 移动影响。B+W 方案首次把值持有 handle **放进 GC 堆对象**（lambda 闭包），缺陷暴露。

### 3.5 可用的映射数据（修复基础）

| 数据 | 位置 | 内容 | 生命周期 |
| ---- | ---- | ---- | ---- |
| `compactEntries_` | [gc.h:158-161](file:///d:/you/Aura/runtime/gc/gc.h#L158-L161) `CompactEntry{oldAddr,newAddr,size,...}` | 小页待搬运对象；**按 oldAddr 升序**（[compact.cpp:112-113](file:///d:/you/Aura/runtime/gc/compact.cpp#L112-L113) 先 `std::sort(toCompact)`，L142 按排序后顺序 push → 严格升序） | `compact()` 主流程在 `relocateGlobalRootPtrs` 之后统一 **clear**（改动 A 移出 `copyObjectsToNewLocations`） |
| `forwardMap` | [compact.cpp:623](file:///d:/you/Aura/runtime/gc/compact.cpp#L623)（中页）/ [compact.cpp:840](file:///d:/you/Aura/runtime/gc/compact.cpp#L840)（大页）`tuple<GcObject*,GcObject*,size_t>` 三元组 | 中页/大页待搬运对象；push 时带 size（区间判定用，旧页释放后不能回读头部） | 函数局部，至 `compactMediumPages` / `sweepLargePages` 结束 |

修复只需地址运算（不 deref 旧地址），因此旧页是否已释放不影响正确性，但**映射数据必须在重定位时仍可用**。

---

## 4. Proposed Changes

### 4.1 小页 compact（改动 A + B）

**改动 A**：把 `copyObjectsToNewLocations` 末尾的 clear（[compact.cpp:196-198](file:///d:/you/Aura/runtime/gc/compact.cpp#L196-L198)）移出，改到 `compact()` 主流程重定位之后清理：

```cpp
void GcHeap::compact(CompactScope scope) {
    computeForwardingAddresses(scope);
    if (compactEntries_.empty()) return;

    updateAllReferences(scope);
    copyObjectsToNewLocations(scope);   // 内部不再 clear
    relocateGlobalRootPtrs();           // 新增：重定位堆内 rootPtr
    savedDescs_.clear();
    compactEntries_.clear();
    rebuildPageList(scope);
}
```

`copyObjectsToNewLocations` 本体删除末尾两行 clear（函数其余不变）。

**改动 B**：gc.h 声明（[gc.h:382](file:///d:/you/Aura/runtime/gc/gc.h#L382) 后）新增成员函数 + compact.cpp 实现。**除重定位 `globalRoots_` 元素外，必须同步改写对象内 `GcRootHandle::ptr_ref_` 槽位值**（审查 N1/N2）：`unregisterGlobalRoot` 用 `std::find` 精确匹配地址（[roots.cpp:101-107](file:///d:/you/Aura/runtime/gc/roots.cpp#L101-L107)），若对象内 `ptr_ref_` 成员仍持旧地址，对象回收时 finalizer 析构注销会 find 失败 → globalRoots_ 残留悬垂 → 崩溃。

布局依据（[gc.h:66-82](file:///d:/you/Aura/runtime/gc/gc.h#L66-L82)，注释"内存布局 40B：基类 24B + union{ptr_,val_} 8B + mode_ 1B"）：`GcRootHandleBase{next_(0) prev_(8) ptr_ref_(16)}`，`GcRootHandle<T>{union{ptr_,val_}(24) mode_(32)}` → **`ptr_ref_` 槽位恒在 `val_` 槽位前 `sizeof(void*)` 字节**。该偏移对所有 `GcRootHandle<T>`（T 恒为指针类型）一致，且 `ptr_ref_` 槽位不被任何 desc 扫描（`fn_` 不归 GC 扫描，见 iterator.h 注释），无副作用。

```cpp
// 布局耦合常量：GcRootHandle 的 ptr_ref_ 槽位恒在 val_ 槽位前 sizeof(void*) 字节
// （gc.h GcRootHandleBase + GcRootHandle<T> 布局，见 §4.1 说明；改布局需同步更新）
static constexpr size_t kGcHandlePtrRefValDelta = sizeof(void*);

void GcHeap::relocateGlobalRootPtrs() {
    if (compactEntries_.empty()) return;
    std::lock_guard<std::mutex> lk(globalRoots_m_);
    for (auto*& rootPtr : globalRoots_) {   // 引用形式：直接改写容器元素（元素类型 GcObject**）
        const char* p = reinterpret_cast<const char*>(rootPtr);
        // compactEntries_ 按 oldAddr 升序（computeForwardingAddresses 已 sort），二分定位
        auto it = std::upper_bound(
            compactEntries_.begin(), compactEntries_.end(), p,
            [](const char* addr, const CompactEntry& e) {
                return addr < reinterpret_cast<const char*>(e.oldAddr);
            });
        if (it == compactEntries_.begin()) continue;
        --it;
        const char* old = reinterpret_cast<const char*>(it->oldAddr);
        if (p >= old && p < old + it->size) {
            const size_t off = static_cast<size_t>(p - old);       // val_ 槽位在对象内偏移
            GcObject** newValPtr = reinterpret_cast<GcObject**>(
                reinterpret_cast<char*>(it->newAddr) + off);       // 新 val_ 槽位地址
            rootPtr = newValPtr;                                    // 1) 重定位 globalRoots_ 元素
            if (off >= kGcHandlePtrRefValDelta) {
                // 2) 同步对象内 ptr_ref_ 槽位（memcpy 后新对象内 off-8 处仍为旧 &val_，
                //    改写为 newValPtr，保证 ~GcRootHandle 注销时 std::find 命中）
                //    槽位内容类型为 GcObject**（GcRootHandleBase::ptr_ref_），故用 GcObject***
                *reinterpret_cast<GcObject***>(
                    reinterpret_cast<char*>(it->newAddr) + off - kGcHandlePtrRefValDelta) =
                    newValPtr;
            }
        }
    }
}
```

### 4.2 中页 compact（改动 C + D）

**改动 C**：`forwardMap` 元素补 size（重定位需区间判定；旧页已释放不能回读 `oldAddr->allocSize()`）：

- [compact.cpp:623](file:///d:/you/Aura/runtime/gc/compact.cpp#L623)（中页）/ [compact.cpp:840](file:///d:/you/Aura/runtime/gc/compact.cpp#L840)（大页）：`std::vector<std::pair<GcObject*, GcObject*>> forwardMap;` → `std::vector<std::tuple<GcObject*, GcObject*, size_t>> forwardMap;`
- [compact.cpp:668](file:///d:/you/Aura/runtime/gc/compact.cpp#L668)（中页）/ [compact.cpp:866](file:///d:/you/Aura/runtime/gc/compact.cpp#L866)（大页）：`forwardMap.push_back({obj, newAddr});` → `forwardMap.push_back({obj, newAddr, size});`（size 为该 obj 的 allocSize，push 时对象未覆盖，可读）
- 中页拷贝循环 [compact.cpp:672-676](file:///d:/you/Aura/runtime/gc/compact.cpp#L672-L676)、young/oldObjects_ 更新循环 [compact.cpp:692](file:///d:/you/Aura/runtime/gc/compact.cpp#L692)、大页拷贝循环 [compact.cpp:870-874](file:///d:/you/Aura/runtime/gc/compact.cpp#L870-L874) 及 young/old 更新循环 [compact.cpp:884](file:///d:/you/Aura/runtime/gc/compact.cpp#L884)：统一改为三元解构（`for (auto& [oldAddr, newAddr, size] : forwardMap)`），删除局部 `size` 声明避免遮蔽。

**改动 D**：中页重定位。**实施时将内联重定位段提取为公共函数 `relocateRootsInForwardMap`**（[compact.cpp:449-473](file:///d:/you/Aura/runtime/gc/compact.cpp#L449-L473)，大页复用，见改动 E），中页在 [compact.cpp:702](file:///d:/you/Aura/runtime/gc/compact.cpp#L702) `updateMediumPageReferences()` 之后调用。**与改动 B 相同，命中区间时同步改写对象内 `ptr_ref_` 槽位**（审查 N1/N2 同样适用于中页）：

```cpp
// 线性版重定位：用于中页/大页（对象数量少，遍历 forwardMap 足够）。
// 逻辑与 relocateGlobalRootPtrs 相同（含对象内 ptr_ref_ 槽位同步）。
void GcHeap::relocateRootsInForwardMap(
    const std::vector<std::tuple<GcObject*, GcObject*, size_t>>& forwardMap) {
    std::lock_guard<std::mutex> lk(globalRoots_m_);
    for (auto*& rootPtr : globalRoots_) {
        const char* p = reinterpret_cast<const char*>(rootPtr);
        for (const auto& entry : forwardMap) {
            const auto& [oldAddr, newAddr, size] = entry;
            const char* old = reinterpret_cast<const char*>(oldAddr);
            if (p >= old && p < old + size) {
                const size_t off = static_cast<size_t>(p - old);       // val_ 槽位在对象内偏移
                GcObject** newValPtr = reinterpret_cast<GcObject**>(
                    reinterpret_cast<char*>(newAddr) + off);           // 新 val_ 槽位地址
                rootPtr = newValPtr;                                    // 1) 重定位 globalRoots_ 元素
                if (off >= kGcHandlePtrRefValDelta) {
                    // 2) 同步对象内 ptr_ref_ 槽位（见 relocateGlobalRootPtrs 说明；
                    //    槽位内容类型为 GcObject**（GcRootHandleBase::ptr_ref_））
                    *reinterpret_cast<GcObject***>(
                        reinterpret_cast<char*>(newAddr) + off - kGcHandlePtrRefValDelta) =
                        newValPtr;
                }
                break;
            }
        }
    }
}
```

**改动 E（实施期补充）**：**大页 sweepLargePages 同路径一并修复**。大页 sweep 同样搬运对象（[compact.cpp:838-892](file:///d:/you/Aura/runtime/gc/compact.cpp#L838-L892)），闭包捕获的 ValueGlobal handle 进大页对象时存在同一缺陷（原 plan 遗漏此路径，实施时发现）。改动：`forwardMap` 同样改三元组（L840）+ 拷贝循环改 3 元素解构（L870/L884），在 [compact.cpp:881](file:///d:/you/Aura/runtime/gc/compact.cpp#L881) `updateMediumPageReferences()` 之后调用 `relocateRootsInForwardMap(forwardMap)`。

> 去重说明：小页用二分（`compactEntries_` 升序，量大）；中页/大页对象数量少、线性扫足够。中页/大页两段逻辑完全相同（仅调用时机不同），故提取公共函数 `relocateRootsInForwardMap` 复用，避免两处重复代码。

### 4.3 为什么不需要改动值更新逻辑

`updateAllReferences` 步骤 3 / `updateMediumPageReferences` 的 globalRoots_ 段已正确更新 `*rootPtr` 值（对象 forwarding → 新地址）。本次修复只补**地址本身**的重定位，两者互补、顺序无关（值更新在拷贝前、地址重定位在拷贝后，互不冲突）。

---

## 5. Impact Analysis

| 影响面 | 说明 |
| ---- | ---- |
| `runtime/gc/compact.cpp` | `compact()` 主流程 + `copyObjectsToNewLocations`（移出 clear）+ `compactMediumPages`（forwardMap 三元组 + 重定位调用）+ `sweepLargePages`（forwardMap 三元组 + 重定位调用，改动 E）+ 新增 `relocateGlobalRootPtrs`（小页二分）/ `relocateRootsInForwardMap`（中页/大页线性） |
| `runtime/gc/gc.h` | 新增两个成员函数声明（L383-385） |
| ⚠️ BREAKING（内部） | 无：不涉及任何公开 API、类型布局、ABI；纯 compact 内部缺陷修复 |
| 行为变化 | compact 后 `globalRoots_` 中指向被搬运堆对象的 rootPtr 从旧地址变为新地址（此前是悬垂垃圾地址）；**对象内 `GcRootHandle::ptr_ref_` 槽位值同步改写**（方案 P，保证析构注销 `std::find` 命中） |
| 性能 | `relocateGlobalRootPtrs` 对每个 rootPtr 一次二分（O(log n)）；中页线性扫（对象数少）；仅在 compact 时执行 |
| 并发 | 与既有 globalRoots_ 段同一把 `globalRoots_m_` 锁，无嵌套锁、无死锁风险 |

**性能量级评估**：

| 参数 | 构成 | 量级 |
| ---- | ---- | ---- |
| `\|globalRoots_\|` | 静态缓存（[string.cpp:69](file:///d:/you/Aura/runtime/builtin/string.cpp#L69) `_cache[2048]` lazy init、`_t/_f/_e`、intern 池）+ 用户闭包捕获 ValueGlobal 副本 | 几十 ~ 几千 |
| `\|compactEntries_\|` | 一次 compact 搬运的存活对象数 | 几十 ~ 几万 |
| `\|forwardMap\|` | 中页存活对象数 | 通常 < 几百 |

- 小页：`O(\|globalRoots_\| × log\|compactEntries_\|)` ≈ 几千 × ~15 次比较 → **微秒级**
- 中页：`O(\|globalRoots_\| × \|forwardMap\|)` ≈ 几十万次比较 → **微秒级**
- 仅在 compact 时执行（碎片率超阈值才触发）；对照 compact 自身开销（memcpy 全部存活对象 + 遍历 young/old 更新字段，MB 级）占比 < 1%，**可忽略**
- 额外开销：一次 `globalRoots_m_` 加锁（STW 下无竞争）、无内存分配、无 deref 旧地址
- 优化空间：二分已最优；中页对象少时线性扫足够，无必要改二分（属过度优化）

### 升级/回滚兼容

- 独立可交付；回滚 = `git restore` compact.cpp / gc.h 两个文件。
- 不影响 mark-sweep、大页、LOS 路径。

---

## 6. Boundary Condition Handling Strategy

| 边界条件 | 现状处理 | 计划处理 | 测试策略 |
| ---- | ---- | ---- | ---- |
| rootPtr 指向栈/静态区（绝大多数） | 值更新正确，地址固定无需重定位 | 区间判断不命中 → 跳过 | `[2]` 及全部既有用例回归 |
| rootPtr 指向被 compact 的堆对象（本 bug） | 悬垂 → 崩溃 | 二分/线性命中区间 → 重写到新地址 | `[2]` 修复后通过 + ASAN |
| rootPtr 指向**未参与 compact** 的对象（Young 模式 mixed 页/mark-sweep 未移动） | 地址未变，安全 | 不在映射中 → 跳过 | Young compact 用例回归 |
| rootPtr 恰在对象边界 / padding 间隙 | 恒为字段地址，不会恰好落在边界 | 半开区间 `[oldAddr, oldAddr+size)` 判定 | 代码审查 + gdb 抽查 |
| 空 `compactEntries_` / `forwardMap` | — | 提前 return / 循环空转 | — |
| 中页/大页对象 push 后、拷贝前对象头部被 setForwardingPtr 覆盖 | push 时 size 已先读（中页 L668 / 大页 L866 处 allocSize 有效） | 三元组携带 size，拷贝循环不再读头部 | 中页/大页压力用例 |
| 对象被搬运后又被回收（后续 GC finalizer unregister） | — | 注销用的 rootPtr 是重定位后的新地址，与 globalRoots_ 存储一致 | `[2]` + ASAN |

---

## 7. Test Plan

### 7.1 单元/集成

1. **iter_gc_test.cpp**（触发源）：`[1]-[5]` 全用例通过，其中 `[2] test_closure_capture` 修复前崩溃、修复后通过。
2. **compact 定向压力**：`[2]` 循环多轮 forceGc（增加 compact 命中率）；链式 map 闭包捕获多个 GC 对象场景。
3. **中页 compact 覆盖**：构造足够大的 record/Array 触发 `shouldCompactMedium` 路径，闭包捕获 handle 场景下重跑（`relocateRootsInForwardMap` 中页段验证）。
4. **大页 sweep 覆盖**：构造 > 中页上限的大对象 + 闭包捕获 handle 进大页对象，触发 `sweepLargePages` 重定位（改动 E，实施期补充路径）。
5. **编译器全量回归**：`compile.cmd`（非 ASAN）+ `example/test.aura` 通过（compact 改动影响所有 GC 程序）。

### 7.2 深度检测

按 AGENTS.md：清空 build 重建 ASAN 版 → 编译 iter_gc_test → 运行捕获 stderr → 0 报告后切回普通模式。

### 7.3 正向验证（重定位正确性）

gdb 断点 `copyObjectsToNewLocations` 之后：检查 `globalRoots_` 中原 `rootPtr=0x1801e0` 的项已变为 `新对象地址 + 24`（`&val_` 在 MapIter 内 lambda 捕获槽位偏移），且新地址指向拷贝后对象内部。

### 7.4 回归风险区

- `copyObjectsToNewLocations` 移出 clear 后，`compact()` 的所有调用路径（All/Young scope）都必须走新增清理点（代码审查确认无其他调用方）。
- `compactMediumPages` forwardMap 三元组改造的 3 处解构点（拷贝循环 / young-old 更新循环 / 新增重定位段）。

---

## 8. Implementation Steps（Ordered）

1. **小页修复**：改动 A（移出 clear）+ 改动 B（gc.h 声明 + `relocateGlobalRootPtrs` 实现）。
   → 产物：runtime 编译通过（`cmake --build runtime/build`）。
2. **中页修复**：改动 C（forwardMap 三元组）+ 改动 D（`relocateRootsInForwardMap` 公共函数 + 中页调用点）。
   → 产物：runtime 编译通过。
3. **大页修复（实施期补充）**：改动 E（`sweepLargePages` forwardMap 三元组 + 调用 `relocateRootsInForwardMap`）。
   → 产物：runtime 编译通过。
4. **回归测试**：iter_gc_test `[1]-[5]` 全过（重点 `[2]`）。
5. **ASAN 深度检测**：重建 ASAN 版跑 iter_gc_test，0 报告后切回普通模式。
6. **全量回归**：`compile.cmd` + test.aura。
7. **完成**：TODO.txt 该 issue 标记 `[x]`。

**回滚**：每步均 `git restore runtime/gc/compact.cpp runtime/gc/gc.h` 即可，独立可回滚。

---

## 9. Risks & Mitigations

| 风险 | 缓解 |
| ---- | ---- |
| 二分比较器方向写反导致误定位 | `std::upper_bound` 谓词 `addr < oldAddr`（严格小于）；单测/代码审查覆盖 |
| `copyObjectsToNewLocations` 移出 clear 遗漏其他调用方 | grep 确认仅 `compact()` 一处调用（改动前核验） |
| 迭代 `globalRoots_` 时改写元素 | 引用遍历，只改值不增删，与既有 `for (auto* rootPtr : ...)` 不同但安全 |
| forwardMap 三元组改造遗漏解构点 | 编译期结构化绑定不匹配即报错；步骤 2/3 各自编译验证 |
| 中页对象 push 的 size 记录时机错误 | push 在 `setForwardingPtr` 之后、memcpy 之前，allocSize 字段未被覆盖，值有效 |
| 方案 P 依赖 GcRootHandle 布局（`ptr_ref_` 在 `val_` 前 8B） | 布局耦合常量 `kGcHandlePtrRefValDelta` + 注释；改 GcRootHandle 布局（[gc.h:66-82](file:///d:/you/Aura/runtime/gc/gc.h#L66-L82)）须同步更新；审查时复核 |
| 对象内 `ptr_ref_` 槽位误改（命中非 handle 地址） | `globalRoots_` 元素唯一来源是 ValueGlobal handle 的 `registerGlobalRoot(&val_)`，命中区间即 handle；`off >= delta` 保护（val_ 实际 offset 24，恒成立） |

---

## 10. 已知限制（本次不修复，后续独立 issue）

> 审查 N1/N2（ValueGlobal 的 `ptr_ref_` 槽位未同步）已由 **方案 P** 解决（§4.1 改动 B / §4.2 改动 D），**不再属于本限制**。以下仅剩与 ThreadLocal/Ref 模式相关的场景。

1. **`threadRootLists_` 链表 node（this）指针同类缺陷**：若闭包捕获 **ThreadLocal 模式**的 `GcRootHandle` 进 GC 堆，`registerRootThreadLocal(this)`（[handles.h:40](file:///d:/you/Aura/runtime/gc/handles.h#L40)）注册的 `this` 地址随 compact 移动而悬垂；且 `node->ptr_ref_` 若指向堆内 `&val_` 也需重定位。当前代码路径只产生 ValueGlobal（本 plan 覆盖），ThreadLocal 进堆场景不存在，故不修。
   - 触发条件（未来）：用户代码/CodeGen 把 ThreadLocal scope 的 GcRootHandle 值捕获进 lambda 并存入 GC 堆对象。
   - 复杂度：node 重定位与 ptr_ref_ 重定位存在顺序耦合（须在 deref 前完成），需单独设计。
2. **Ref 模式句柄指向 GC 堆内变量**：`GcRootHandle(T& ref)` 的 `ptr_` 指向外部变量；若外部变量恰为 GC 堆对象字段（无实际用例），Ref 模式本身不注册地址，仅线程链表 node 在栈上，安全；若未来出现则随限制 1 一并评估。
3. **GcRootHandle 在 GC 堆内被 `set()` 重建指针的场景**：值更新已在 compact 前完成，重定位后保持一致；无需额外处理（记录以明确语义边界）。

> 以上限制已同步写入 [TODO.txt](file:///d:/you/Aura/TODO.txt) [五] P0 issue 的"已知限制"说明。

---

## 11. 审查修正记录（2026-08-06 审查报告 out.txt）

| # | 审查发现 | 核实结论 | 处理 |
| ---- | ---- | ---- | ---- |
| N3 | `compactEntries_` 未排序 → 二分前提不成立 → 误定位 | **误报**：[compact.cpp:112-113](file:///d:/you/Aura/runtime/gc/compact.cpp#L112-L113) 有 `std::sort(toCompact)`，L142 按排序后顺序 push，`compactEntries_` 严格升序 | 无改动，§3.5 补充源码引用 |
| N1 | 对象回收时 `~GcRootHandle` 用旧 `ptr_ref_` 调 `unregisterGlobalRoot` → `std::find`（[roots.cpp:101-107](file:///d:/you/Aura/runtime/gc/roots.cpp#L101-L107)）失败 → globalRoots_ 残留悬垂 → 崩溃 | **真实（P0）**：GcRootHandle 布局固定（[gc.h:66-82](file:///d:/you/Aura/runtime/gc/gc.h#L66-L82)），`ptr_ref_` 槽位值未随对象移动更新 | **方案 P**：重定位命中区间时同步改写对象内 `ptr_ref_` 槽位（§4.1 改动 B、§4.2 改动 D），`kGcHandlePtrRefValDelta` 常量 + 注释约束布局耦合 |
| N2 | 移动构造 `unregisterGlobalRoot(other.ptr_ref_)`（[handles.h:69](file:///d:/you/Aura/runtime/gc/handles.h#L69)）同类 find 失败 | **真实（P1）**：与 N1 同源 | 方案 P 一并解决（`other.ptr_ref_` 与 globalRoots_ 同步为新地址） |
| N4 | ThreadLocal 模式进堆 | 已知 | §10 限制 1 保持（方案 P 不覆盖） |
| — | 审查建议替代方案 3「禁止 ValueGlobal 进堆」 | 方案 P 改动面更小且完整解决 N1/N2；方案 3 需联动 plan/迭代器GC安全修复.md 的闭包捕获机制改造，且 ThreadLocal 进堆仍悬垂（§10 限制 1） | 不采纳，记录为备选 |

## 12. 实施修正记录（2026-08-06 实施期）

| # | 发现 | 处理 |
| ---- | ---- | ---- |
| R1 | **大页 sweepLargePages 同样搬运对象，存在同一 rootPtr 悬垂缺陷**（原 plan 仅覆盖小页/中页） | **改动 E**（§4.2）：`forwardMap` 三元组 + 拷贝循环 3 元素解构 + `updateMediumPageReferences()` 后调用 `relocateRootsInForwardMap`（[compact.cpp:838-892](file:///d:/you/Aura/runtime/gc/compact.cpp#L838-L892)），三路径全覆盖 |
| R2 | 中页重定位段与新增大页段逻辑完全相同 → 原 plan"抽象收益低、各写一处"的决策不再成立 | 提取公共函数 `relocateRootsInForwardMap`（[compact.cpp:449-473](file:///d:/you/Aura/runtime/gc/compact.cpp#L449-L473)），中页（L702）/大页（L881）两处调用；§4.2 去重说明同步更新 |
| R3 | `ptr_ref_` 槽位赋值类型笔误：槽位内容类型为 `GcObject**`（`GcRootHandleBase::ptr_ref_`），改写需解引用 `GcObject***` | 两处（小页 L439 / 公共函数 L465）修正为 `*reinterpret_cast<GcObject***>(...) = newValPtr`，编译错误（`cannot convert 'GcObject**' to 'GcObject*'`）暴露并修复 |
