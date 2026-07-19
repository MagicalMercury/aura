# GC Young→Old 晋升机制缺陷报告

> 评估对象：[runtime/gc.cpp](file:///d:/you/Aura/runtime/gc.cpp) + [runtime/gc.h](file:///d:/you/Aura/runtime/gc.h)
> 日期：2026-07-19
> 状态：缺陷记录（待决定是否修复）

---

## 一、晋升机制现状

### 1.1 基础设施

[gc.h:229](file:///d:/you/Aura/runtime/gc.h#L229) 声明 `promoteToOld(GcObject* obj)`，[gc.cpp:496-503](file:///d:/you/Aura/runtime/gc.cpp#L496) 实现：

```cpp
void GcHeap::promoteToOld(GcObject* obj) {
    obj->generation = 1;
    oldObjects_.push_back(obj);
    oldBytes_ += obj->desc ? obj->desc->size : 0;
    if (oldBytes_ >= kOldThreshold) {
        gcPending_ = true;
    }
}
```

### 1.2 触发时机

[gc.cpp:485-490](file:///d:/you/Aura/runtime/gc.cpp#L485) `sweepPhaseYoung` 中：minor GC 时，所有 `marked=true` 的 young 对象**无条件晋升**到 old。

### 1.3 触发链路

```
minorGc()
  → markPhase(youngOnly=true)
  → sweepPhaseYoung()
    → promoteToOld(obj)   // 遍历 youngObjects_ 中 marked=true 的对象
```

### 1.4 关键常量

[gc.h:200-202](file:///d:/you/Aura/runtime/gc.h#L200):

```cpp
static constexpr size_t kPageSize        = 4096;
static constexpr size_t kYoungThreshold  = 256 * 1024;  // 256 KB → minor GC
static constexpr size_t kOldThreshold    = 1024 * 1024; // 1 MB → major GC
```

---

## 二、缺陷清单

### 🔴 缺陷 1：`oldBytes_` 统计严重偏小（高优先级）

**位置**：[gc.cpp:499](file:///d:/you/Aura/runtime/gc.cpp#L499)

```cpp
oldBytes_ += obj->desc ? obj->desc->size : 0;
```

**问题**：
- `desc->size` 是 `TypeDescriptor` 中的**类型基础大小**
- 对变长对象严重不准确：
  - `GcString::_desc.size = sizeof(GcString)` ≈ 16 字节
  - 但实际分配大小是 `sizeof(GcString) + length + 1`（可能 KB 级别）
- 一个 1KB 的 GcString 晋升时只累加 16 字节到 `oldBytes_`

**对比**：`youngBytes_` 用的是实际分配字节（[gc.cpp:81](file:///d:/you/Aura/runtime/gc.cpp#L81): `youngBytes_ += size`），**两者单位不一致**。

**后果**：
- `kOldThreshold = 1MB` 实际很难触发
- major GC 长期不被自动触发
- 与测试现象一致：`gc_force()` 前 pages=188、live=4644 — `oldBytes_` 严重低估，未触发自动 major GC

**修复方案**：
- 让 `GcObject` 记录分配时的实际字节数（新增字段 `size_t allocSize`），或
- `TypeDescriptor` 加 `computeSize(void* obj)` 虚函数计算实际大小
- `promoteToOld` 改为 `oldBytes_ += obj->allocSize`

---

### 🔴 缺陷 2：写屏障完全未接入（严重 bug）

**位置**：[gc.h:299-301](file:///d:/you/Aura/runtime/gc.h#L299)

```cpp
inline void gc_write_barrier(GcObject* parent, void* fieldAddr, GcObject* newVal) {
    GcHeap::instance().writeBarrier(parent, fieldAddr, newVal);
}
```

**问题**：
- 提供了 `gc_write_barrier` 内联函数
- 但搜索 `src/` 全目录，**没有任何 CodeGen 代码生成 `gc_write_barrier` 调用**
- `writeBarrier` 永远不被调用
- `rememberedSet_` 永远为空

**死代码**：[gc.cpp:386-389](file:///d:/you/Aura/runtime/gc.cpp#L386)

```cpp
if (youngOnly) {
    for (auto* oldObj : rememberedSet_) {
        markFields(oldObj);   // 永远不执行（rememberedSet_ 始终为空）
        markInlineArrayFields(oldObj);
    }
}
```

**为何当前 minor GC 仍能正确工作**：
- `markPhase` 从 `roots_` / `globalRoots_` 出发递归 `markFields`
- 会标记所有从根可达的对象（包括 old 持有的 young 引用）
- 但**性能上失去了分代 GC 的核心收益** — 无法避免扫描老年代

**修复方案**：
- CodeGen 在生成对象字段写入时（`obj.field = newVal`），插入 `gc_write_barrier(obj, &obj->field, newVal)`
- 仅当 `parent->generation == 1 && newVal->generation == 0` 时记录到 `rememberedSet_`（已在 [gc.cpp:146-151](file:///d:/you/Aura/runtime/gc.cpp#L146) 实现）
- minor GC 时遍历 `rememberedSet_` 标记 old→young 引用

---

### 🟡 缺陷 3：栈根扫描在 youngOnly 时跳过老年代（潜在问题）

**位置**：[gc.cpp:366](file:///d:/you/Aura/runtime/gc.cpp#L366)

```cpp
if (youngOnly && obj->generation == 1) break;
```

**问题**：
- 栈中若持有老年代对象指针（如 `GcGlobalRoot<GcString>` 单例的引用）
- minor GC 不会从这个栈槽出发标记
- 虽然 `globalRoots_` 会单独处理全局根，缓存单例不会被错误回收
- 但若用户代码在栈上**临时持有 old 对象指针**，其引用的 young 对象可能被误回收

**修复方案**：
- 移除 `youngOnly && generation == 1` 的 break，或
- 改为只跳过 old 对象本身的标记，但仍递归标记其引用的 young 对象

---

### 🟡 缺陷 4：无年龄门槛，所有存活对象无条件晋升

**位置**：[gc.cpp:485-490](file:///d:/you/Aura/runtime/gc.cpp#L485)

```cpp
for (auto* obj : youngObjects_) {
    if (obj->marked) {
        promoteToOld(obj);  // 无条件晋升
        obj->marked = false;
    }
}
```

**问题**：
- 策略过于激进
- 短命对象如果在 minor GC 时刚好被引用，立即晋升到老年代
- 老年代会累积大量本应回收的对象
- 没有 Eden/Survivor 分区，没有 `age` 计数器

**对比工业级 GC**：
- Java：2 个 survivor 区 + age 阈值（`MaxTenuringThreshold=15`）
- V8：分代 + age 计数
- Python：分代 + age 阈值（3 代）

**修复方案**：
- `GcObject` 加 `uint8_t age` 字段
- minor GC 时仅对 `age >= kPromotionAge`（如 2）的对象晋升
- 未达阈值的存活对象留在 youngObjects_ 中（age++）
- 引入 survivor 区或直接复用 youngObjects_

---

### 🟢 缺陷 5：晋升对象与新生代对象混布 page

**问题**：
- 晋升只更新 `oldObjects_` 列表和 `generation` 标志
- **不移动对象内存**
- old 对象和后续新分配的 young 对象仍混布在同一 page 上
- page 只要有 1 个 old 存活对象就不会被回收
- dead young 对象占用的 page 空间不会被 bump allocator 重新利用（`bumpOffset` 单调递增）
- 直到 `compactAndReclaim` 或 `majorGc` 才能整理

**当前缓解**：
- [gc.cpp:571+](file:///d:/you/Aura/runtime/gc.cpp#L571) `compactAndReclaim` 能回收完全无存活对象的 page
- 但 mixed live/dead 的 page 仍无法整理

**修复方案**：
- 引入 copying GC 或 compacting GC（架构级重构，对应 [gc_features_plan.md §十三](file:///d:/you/Aura/plan/gc_features_plan.md) 已标记 `[~] 延后`）
- 或短期：分离 young/old 的 page 分配器，避免混布

---

## 三、优先级与影响评估

| 优先级 | 缺陷 | 影响范围 | 修复成本 | 风险 |
|:---:|:---|:---|:---:|:---:|
| 🔴 高 | 缺陷 1：`oldBytes_` 偏小 | major GC 触发失效 | 低（加字段 + 改一行） | 低 |
| 🔴 高 | 缺陷 2：写屏障未接入 | 分代 GC 性能收益消失 | 中（CodeGen 改造） | 中 |
| 🟡 中 | 缺陷 3：栈根扫描跳过 old | 极端场景误回收 | 低（改一行） | 中 |
| 🟡 中 | 缺陷 4：无年龄门槛 | 老年代膨胀 | 中（加 age 字段 + 逻辑） | 中 |
| 🟢 低 | 缺陷 5：old/young 混布 page | 内存碎片 | 高（架构级） | 高 |

---

## 四、推荐修复顺序

### Phase 1（短期，低风险）

1. **修复缺陷 1**：`GcObject` 加 `size_t allocSize` 字段，`alloc` 时记录，`promoteToOld` 用实际大小
   - 修复后 `oldBytes_` 准确，major GC 能正确触发
   - 影响范围小，无 API 变更

2. **修复缺陷 3**：移除 `youngOnly && generation == 1` 的 break（或改为只跳过 obj 自身的标记，仍递归标记其引用的 young）
   - 修复栈上临时 old 指针的潜在问题
   - 需谨慎评估对性能的影响（minor GC 会扫描更多对象）

### Phase 2（中期，中风险）

3. **修复缺陷 4**：引入 `age` 字段，设 `kPromotionAge = 2`
   - 短命对象不再立即晋升
   - 减少 oldObjects_ 膨胀
   - 需测试调参

4. **修复缺陷 2**：CodeGen 生成 `gc_write_barrier` 调用
   - CodeGen 在生成 `obj.field = newVal` 时插入屏障
   - `rememberedSet_` 真正生效
   - 性能大幅提升（minor GC 不再扫描整个 oldObjects_）

### Phase 3（远期，高风险）

5. **修复缺陷 5**：引入 compacting GC 或分离 young/old page 分配器
   - 架构级重构
   - 需充分 profiling 证据
   - 对应 [gc_features_plan.md §十三](file:///d:/you/Aura/plan/gc_features_plan.md) `[~] 延后` 项

---

## 五、与现有 plan 的关系

| 现有 plan 项 | 状态 | 关系 |
|:---|:---:|:---|
| [gc_features_plan.md §九 精确栈扫描](file:///d:/you/Aura/plan/gc_features_plan.md) | [-] 暂不实施 | 与缺陷 3 部分相关 |
| [gc_features_plan.md §十一 TLAB](file:///d:/you/Aura/plan/gc_features_plan.md) | [~] 延后 | 与缺陷 5 部分相关 |
| [gc_features_plan.md §十三 对象可移动性](file:///d:/you/Aura/plan/gc_features_plan.md) | [~] 延后 | 即缺陷 5 的解决方案 |
| [gcstring_optimization.md §A1~A4](file:///d:/you/Aura/plan/gcstring_optimization.md) | 待实施 | 缓存单例依赖 GC 全局根，缺陷 2 不影响（写屏障针对对象字段赋值） |

---

## 六、附录：验证证据

### 6.1 `writeBarrier` 无调用点

```
搜索 src/ 目录：
  pattern: writeBarrier|gc_write_barrier|rememberedSet
  结果: No matches found
```

确认 CodeGen 未生成任何写屏障调用。

### 6.2 `oldBytes_` 与 `youngBytes_` 单位不一致

```cpp
// youngBytes_：实际分配字节
youngBytes_ += size;  // [gc.cpp:81]

// oldBytes_：类型基础大小
oldBytes_ += obj->desc ? obj->desc->size : 0;  // [gc.cpp:499]
```

### 6.3 测试现象佐证

用户 2026-07-19 测试：
- `before force: GC: alloc=741KB young=229KB old=0KB gc=0 minor=2 live=4644 pages=188`
- `after force: GC: alloc=0KB young=0KB old=0KB gc=1 minor=2 live=4 pages=3`

**关键观察**：
- `before force` 时 `old=0KB` — 即便 `live=4644`，`oldBytes_` 仍为 0
- 说明对象可能根本未晋升，或晋升了但 `oldBytes_` 统计为 0（因 `desc->size` 计算问题）
- `pages=188` 远超 `live=4644` 实际所需 — 大量 page 含 dead 对象但未回收

**需进一步验证**：在 `promoteToOld` 加日志确认对象是否真的晋升，或 `desc->size` 是否为 0 导致 `oldBytes_` 始终不增长。
