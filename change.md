# GC Young→Old 晋升机制缺陷修复 Plan

> 来源：[plan/gc_promotion_issues.md](file:///d:/you/Aura/plan/gc_promotion_issues.md)
> 日期：2026-07-19
> 状态：草案（待批准）
> 范围：5 个缺陷分 3 个 Phase 实施

---

## 一、Summary

修复 [plan/gc_promotion_issues.md](file:///d:/you/Aura/plan/gc_promotion_issues.md) 记录的 5 个 young→old 晋升机制缺陷，分 3 个 Phase：

| Phase | 缺陷 | 风险 | 改动量 |
|:---:|:---|:---:|:---:|
| Phase 1 | 缺陷 1 (`oldBytes_` 偏小) + 缺陷 3 (栈根扫描跳过 old) | 低 | +25 行 |
| Phase 2 | 缺陷 4 (无年龄门槛) + 缺陷 2 (写屏障未接入) | 中 | +60 行 |
| Phase 3 | 缺陷 5 (old/young 混布 page) | 高 | 推迟 |

**本 plan 详细描述 Phase 1 + Phase 2 的实施方案，Phase 3 仅作远期展望**。

---

## 二、Current State Analysis

### 2.1 晋升机制现状

[gc.cpp:497-503](file:///d:/you/Aura/runtime/gc.cpp#L497):

```cpp
void GcHeap::promoteToOld(GcObject* obj) {
    obj->generation = 1;
    oldObjects_.push_back(obj);
    oldBytes_ += obj->desc ? obj->desc->size : 0;   // ← 缺陷 1：用类型基础大小，非实际分配大小
    if (oldBytes_ >= kOldThreshold) {
        gcPending_ = true;
    }
}
```

### 2.2 关键数据结构

[types.h:81-92](file:///d:/you/Aura/runtime/types.h#L81) `TypeDescriptor`：

```cpp
struct TypeDescriptor {
    size_t        size;               // 对象总大小（字节），含内联数据
    size_t        ptrFieldCount;
    const size_t* ptrFieldOffsets;
    size_t              inlineArrayFieldCount = 0;
    const InlineArrayField* inlineArrayFields = nullptr;
    void (*finalizer)(GcObject* self) = nullptr;
};
```

[types.h:105-121](file:///d:/you/Aura/runtime/types.h#L105) `GcObject`：

```cpp
struct GcObject {
    const TypeDescriptor* desc = nullptr;
    bool     marked = false;
    GcObject* next  = nullptr;
    uint8_t  generation = 0;       // 0=young, 1=old
    bool     finalized = false;
    virtual ~GcObject() = default;
};
```

### 2.3 关键常量

[gc.h:201-203](file:///d:/you/Aura/runtime/gc.h#L201):

```cpp
static constexpr size_t kPageSize        = 4096;
static constexpr size_t kYoungThreshold  = 256 * 1024;   // 256 KB → minor GC
static constexpr size_t kOldThreshold    = 1024 * 1024; // 1 MB → major GC
```

### 2.4 测试现象佐证

2026-07-19 测试结果：
- `before force: GC: alloc=741KB young=229KB old=0KB gc=0 minor=2 live=4644 pages=188`
- `after force: GC: alloc=0KB young=0KB old=0KB gc=1 minor=2 live=4 pages=3`

**关键观察**：`before force` 时 `old=0KB` — 即便 `live=4644`，`oldBytes_` 仍为 0，证明 `desc->size` 统计严重偏小。

---

## 三、Proposed Changes

### Phase 1：低风险修复（缺陷 1 + 缺陷 3）

#### Step 1.1：缺陷 1 — 修复 `oldBytes_` 统计

**改动文件**：[runtime/types.h](file:///d:/you/Aura/runtime/types.h) + [runtime/gc.h](file:///d:/you/Aura/runtime/gc.h) + [runtime/gc.cpp](file:///d:/you/Aura/runtime/gc.cpp)

**3.1.1** `GcObject` 新增 `allocSize` 字段（[types.h:105-121](file:///d:/you/Aura/runtime/types.h#L105)）

```cpp
struct GcObject {
    const TypeDescriptor* desc = nullptr;
    bool     marked = false;
    GcObject* next  = nullptr;
    uint8_t  generation = 0;
    bool     finalized = false;

    // 新增：实际分配字节数（含对象头 + 内联数据 + 对齐填充）
    // GC 分配时记录，promoteToOld 用于准确累加 oldBytes_
    size_t   allocSize = 0;

    virtual ~GcObject() = default;
};
```

**3.1.2** `tryAlloc` 记录 `allocSize`（[gc.cpp:46-90](file:///d:/you/Aura/runtime/gc.cpp#L46)）

在 [gc.cpp:74-82](file:///d:/you/Aura/runtime/gc.cpp#L74) 之后修改：

```cpp
GcObject* obj = static_cast<GcObject*>(mem);
obj->desc       = desc;
obj->marked     = false;
obj->next       = nullptr;
obj->generation = 0;
obj->allocSize  = size;          // ← 新增：记录对齐后的实际分配大小
obj->finalized  = false;

youngObjects_.push_back(obj);
youngBytes_ += size;
allocatedBytes_ += size;
```

**3.1.3** `promoteToOld` 用 `allocSize`（[gc.cpp:497-503](file:///d:/you/Aura/runtime/gc.cpp#L497)）

```cpp
void GcHeap::promoteToOld(GcObject* obj) {
    obj->generation = 1;
    oldObjects_.push_back(obj);
    oldBytes_ += obj->allocSize;   // ← 改：用实际分配大小
    if (oldBytes_ >= kOldThreshold) {
        gcPending_ = true;
    }
}
```

**3.1.4** `sweepPhaseAll` 统计同步修正（[gc.cpp:509-560](file:///d:/you/Aura/runtime/gc.cpp#L509)）

```cpp
// 当前（错误）：
liveOldBytes += obj->desc ? obj->desc->size : 0;

// 改为：
liveOldBytes += obj->allocSize;
```

**3.1.5** 行数估计

| 文件 | 改动 | 行数 |
|:---|:---|:---:|
| [runtime/types.h](file:///d:/you/Aura/runtime/types.h) | + `allocSize` 字段 + 注释 | +3 |
| [runtime/gc.cpp](file:///d:/you/Aura/runtime/gc.cpp) | `tryAlloc` 记录 + `promoteToOld` 改用 + `sweepPhaseAll` 改用 | +1 / -2 |
| **合计** | | **+2 净增** |

---

#### Step 1.2：缺陷 3 — 修复栈根扫描跳过 old

**改动文件**：[runtime/gc.cpp](file:///d:/you/Aura/runtime/gc.cpp)

**3.2.1** 修改 [gc.cpp:353-372](file:///d:/you/Aura/runtime/gc.cpp#L353) `markPhase` 中栈根扫描逻辑

当前：

```cpp
for (auto& [begin, end] : stackRoots_) {
    char* start2 = static_cast<char*>(begin);
    char* stop2  = static_cast<char*>(end);
    for (char* p = start2; p + sizeof(void*) <= stop2; p += sizeof(void*)) {
        void* candidate = *reinterpret_cast<void**>(p);
        if (!candidate) continue;
        for (Page* page = headPage_; page; page = page->next) {
            if (candidate >= static_cast<void*>(page->data) &&
                candidate < static_cast<void*>(page->data + kPageSize)) {
                GcObject* obj = static_cast<GcObject*>(candidate);
                if (!obj->desc || obj->desc->size == 0) break;
                if (youngOnly && obj->generation == 1) break;   // ← 缺陷：跳过 old 但也跳过其引用的 young
                markObject(obj);
                break;
            }
        }
    }
}
```

改为：

```cpp
for (auto& [begin, end] : stackRoots_) {
    char* start2 = static_cast<char*>(begin);
    char* stop2  = static_cast<char*>(end);
    for (char* p = start2; p + sizeof(void*) <= stop2; p += sizeof(void*)) {
        void* candidate = *reinterpret_cast<void**>(p);
        if (!candidate) continue;
        for (Page* page = headPage_; page; page = page->next) {
            if (candidate >= static_cast<void*>(page->data) &&
                candidate < static_cast<void*>(page->data + kPageSize)) {
                GcObject* obj = static_cast<GcObject*>(candidate);
                if (!obj->desc || obj->desc->size == 0) break;
                // 修复缺陷 3：youngOnly 模式下，
                // - old 对象本身不重复标记（已由 globalRoots_ 处理或上次 major GC 标记）
                // - 但仍需递归标记其引用的 young 对象（这是 minor GC 的关键）
                // markObject 内部会判断 marked 标志，避免重复扫描 old
                markObject(obj);
                break;
            }
        }
    }
}
```

**关键设计**：
- 移除 `if (youngOnly && obj->generation == 1) break;`
- 改为始终调用 `markObject(obj)`
- `markObject` 内部已有 `if (obj->marked) return;` 守卫（[gc.cpp:407-409](file:///d:/you/Aura/runtime/gc.cpp#L407)），重复调用安全
- old 对象的 `markFields` 递归会标记其引用的 young 对象 — 这是 minor GC 正确性的关键

**风险评估**：
- ⚠️ 性能影响：minor GC 现在可能扫描更多 old 对象（栈上临时持有的 old 指针）
- ✅ 但 `markObject` 的 `marked` 守卫避免重复扫描
- ✅ old 对象的 `markFields` 仅遍历其指针字段，不递归到其他 old（已被标记）

**3.2.2** 行数估计

| 文件 | 改动 | 行数 |
|:---|:---|:---:|
| [runtime/gc.cpp](file:///d:/you/Aura/runtime/gc.cpp) | 移除 1 行 break + 加注释 | -1 / +3 |
| **合计** | | **+2 净增** |

---

### Phase 2：中风险修复（缺陷 4 + 缺陷 2）

#### Step 2.1：缺陷 4 — 引入年龄门槛

**改动文件**：[runtime/types.h](file:///d:/you/Aura/runtime/types.h) + [runtime/gc.h](file:///d:/you/Aura/runtime/gc.h) + [runtime/gc.cpp](file:///d:/you/Aura/runtime/gc.cpp)

**3.3.1** `GcObject` 加 `age` 字段（[types.h:105-121](file:///d:/you/Aura/runtime/types.h#L105)）

```cpp
struct GcObject {
    const TypeDescriptor* desc = nullptr;
    bool     marked = false;
    GcObject* next  = nullptr;
    uint8_t  generation = 0;
    bool     finalized = false;
    size_t   allocSize = 0;          // Phase 1 已加

    // 新增：对象存活年龄（经历 minor GC 的次数）
    uint8_t  age = 0;

    virtual ~GcObject() = default;
};
```

**3.3.2** `gc.h` 加晋升年龄阈值常量（[gc.h:201-203](file:///d:/you/Aura/runtime/gc.h#L201)）

```cpp
static constexpr size_t kPageSize        = 4096;
static constexpr size_t kYoungThreshold  = 256 * 1024;
static constexpr size_t kOldThreshold    = 1024 * 1024;
static constexpr uint8_t kPromotionAge   = 2;   // 新增：经历 2 次 minor GC 后晋升
```

**3.3.3** `sweepPhaseYoung` 改造为按年龄晋升（[gc.cpp:483-494](file:///d:/you/Aura/runtime/gc.cpp#L483)）

当前：

```cpp
// 3. 晋升所有存活对象到老年代，清空新生代列表。
for (auto* obj : youngObjects_) {
    if (obj->marked) {
        promoteToOld(obj);
        obj->marked = false;
    }
}

youngBytes_ = 0;
youngObjects_.clear();
```

改为：

```cpp
// 3. 按年龄门槛晋升：age >= kPromotionAge 的存活对象晋升到老年代，
// 其余存活对象 age++ 并留在新生代；未存活对象将被回收（youngObjects_.clear 后丢弃）。
std::vector<GcObject*> survivors;
for (auto* obj : youngObjects_) {
    if (obj->marked) {
        obj->age++;
        if (obj->age >= kPromotionAge) {
            promoteToOld(obj);
            obj->marked = false;
        } else {
            survivors.push_back(obj);
            obj->marked = false;
        }
    }
    // 未 marked 的对象不进入 survivors，将被回收（内存由 compactAndReclaim 整理）
}

// 更新 youngBytes_ 为存活对象的总大小
youngBytes_ = 0;
for (auto* obj : survivors) {
    youngBytes_ += obj->allocSize;
}
youngObjects_ = std::move(survivors);
```

**关键设计**：
- `kPromotionAge = 2` — 经历 2 次 minor GC 仍存活的对象晋升
- 短命对象（如临时字符串）在 1-2 次 minor GC 中即被回收，不进入老年代
- 长期存活对象（如缓存单例）经过 2 次 minor GC 后晋升，稳定存在于老年代
- `youngBytes_` 现在只统计存活对象（之前是 `youngBytes_ = 0`，不准确）

**3.3.4** 行数估计

| 文件 | 改动 | 行数 |
|:---|:---|:---:|
| [runtime/types.h](file:///d:/you/Aura/runtime/types.h) | + `age` 字段 | +1 |
| [runtime/gc.h](file:///d:/you/Aura/runtime/gc.h) | + `kPromotionAge` 常量 | +1 |
| [runtime/gc.cpp](file:///d:/you/Aura/runtime/gc.cpp) | `sweepPhaseYoung` 改造 | +15 / -5 |
| **合计** | | **+12 净增** |

---

#### Step 2.2：缺陷 2 — CodeGen 接入写屏障

**改动文件**：[src/CodeGen/ExprGen.cpp](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp) + [runtime/gc.h](file:///d:/you/Aura/runtime/gc.h)

**3.4.1** 改造 `genAssignExpr`（[ExprGen.cpp:566-585](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L566)）

当前：

```cpp
std::string CodeGenerator::genAssignExpr(const AssignExpr& e, bool isCoroutine) {
    std::string target = genExpr(*e.target, isCoroutine);
    std::string value  = genExpr(*e.value, isCoroutine);
    // ... stringVarNames_ 追踪 ...
    return target + " = " + value;
}
```

改为：

```cpp
std::string CodeGenerator::genAssignExpr(const AssignExpr& e, bool isCoroutine) {
    std::string target = genExpr(*e.target, isCoroutine);
    std::string value  = genExpr(*e.value, isCoroutine);
    // ... stringVarNames_ 追踪 ...

    // 写屏障：若 target 是 GC 对象字段（如 obj.field = newVal），插入 gc_write_barrier
    // 仅对 obj.field = newVal 形式生效，不针对局部变量赋值（局部变量不涉及跨代引用）
    if (isGcFieldAssignment(target)) {
        // 生成：
        //   target = value;
        //   gc_write_barrier(parentObj, &target, value);
        // target 形如 "obj.get()->field" 或 "obj->field"
        auto [parentObj, fieldAddr] = decomposeFieldAccess(target);
        return target + " = " + value + ";\n  aura_rt::gc_write_barrier(" +
               parentObj + ", " + fieldAddr + ", " +
               "static_cast<aura_rt::GcObject*>(" + value + "))";
    }
    return target + " = " + value;
}
```

**3.4.2** 新增辅助方法 `isGcFieldAssignment` + `decomposeFieldAccess`

在 [CodeGen.h](file:///d:/you/Aura/src/CodeGen/CodeGen.h) 加私有方法声明：

```cpp
// 判断 target 是否为 obj.field 形式（而非局部变量）
[[nodiscard]] bool isGcFieldAssignment(const std::string& target) const;

// 将 obj.get()->field 分解为 (parentObj, fieldAddr)
// parentObj: "obj.get()" — 用于 gc_write_barrier 的第一个参数
// fieldAddr: "obj.get()->field" 的完整表达式 — 用于第二个参数（字段地址）
[[nodiscard]] std::pair<std::string, std::string>
decomposeFieldAccess(const std::string& target) const;
```

实现：

```cpp
bool CodeGenerator::isGcFieldAssignment(const std::string& target) const {
    // 形如 "xxx->field" 或 "xxx.field"
    // 排除局部变量（如 "s" 或 "s.get()"）
    return target.find("->") != std::string::npos
        || (target.find('.') != std::string::npos
            && target.find(".get()") == std::string::npos);  // 排除 .get() 调用本身
}

std::pair<std::string, std::string>
CodeGenerator::decomposeFieldAccess(const std::string& target) const {
    // 查找最后一个 -> 或 .
    auto arrowPos = target.rfind("->");
    auto dotPos   = target.rfind('.');

    size_t splitPos = std::string::npos;
    size_t fieldNameStart = std::string::npos;

    if (arrowPos != std::string::npos) {
        splitPos = arrowPos;
        fieldNameStart = arrowPos + 2;
    } else if (dotPos != std::string::npos) {
        splitPos = dotPos;
        fieldNameStart = dotPos + 1;
    }

    if (splitPos == std::string::npos) {
        return {target, target};  // 无法分解，保守返回
    }

    std::string parentObj = target.substr(0, splitPos);
    // fieldAddr 用 & 取地址
    std::string fieldAddr = "&(" + target + ")";
    return {parentObj, fieldAddr};
}
```

**3.4.3** `gc_write_barrier` 已存在（[gc.h:299-301](file:///d:/you/Aura/runtime/gc.h#L299)），无需改动

```cpp
inline void gc_write_barrier(GcObject* parent, void* fieldAddr, GcObject* newVal) {
    GcHeap::instance().writeBarrier(parent, fieldAddr, newVal);
}
```

[gc.cpp:146-151](file:///d:/you/Aura/runtime/gc.cpp#L146) `writeBarrier` 已正确实现：

```cpp
void GcHeap::writeBarrier(GcObject* parent, void* /*fieldAddr*/, GcObject* newVal) {
    if (parent && parent->generation == 1 && newVal && newVal->generation == 0) {
        rememberedSet_.insert(parent);
    }
}
```

**关键设计**：
- 仅对 `obj.field = newVal` 形式生成写屏障
- 局部变量赋值（`s = value`）不生成屏障（栈变量不涉及跨代引用）
- `gc_write_barrier` 内部判断 `parent->generation == 1 && newVal->generation == 0`，仅在跨代引用时记录
- `rememberedSet_` 用 `std::set<GcObject*>` 去重
- minor GC 的 [gc.cpp:386-389](file:///d:/you/Aura/runtime/gc.cpp#L386) 死代码将真正生效

**3.4.4** 行数估计

| 文件 | 改动 | 行数 |
|:---|:---|:---:|
| [src/CodeGen/CodeGen.h](file:///d:/you/Aura/src/CodeGen/CodeGen.h) | + 2 个私有方法声明 | +6 |
| [src/CodeGen/ExprGen.cpp](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp) | `genAssignExpr` 改造 + 2 个方法实现 | +40 |
| **合计** | | **+46 净增** |

---

### Phase 3：远期展望（缺陷 5）

**缺陷 5：old/young 混布 page** — 不在本 plan 范围内

对应 [plan/gc_features_plan.md §十三](file:///d:/you/Aura/plan/gc_features_plan.md) `[~] 延后` 项，需架构级重构（compacting GC 或分离 page 分配器），风险过高，留待 profiling 证据充足后再做。

---

## 四、Assumptions & Decisions

### 4.1 关键假设

1. **`allocSize` 可准确记录**：`tryAlloc` 中 `size = (size + 7) & ~size_t(7)` 对齐后赋给 `obj->allocSize`，与 `youngBytes_ += size` 一致
2. **`markObject` 的 `marked` 守卫足够**：[gc.cpp:407-409](file:///d:/you/Aura/runtime/gc.cpp#L407) `if (!obj || obj->marked) return;` 保证重复调用安全
3. **`kPromotionAge = 2` 是合理初始值**：Java 默认 15，但 Aura 的 minor GC 频率较高（256KB 阈值），2 次已足够区分短命与长期对象
4. **写屏障仅对字段赋值生效**：局部变量赋值不涉及跨代引用，无需屏障
5. **`rememberedSet_` 用 `std::set` 去重**：避免同一 old 对象多次记录

### 4.2 关键决策

| 决策 | 选择 | 理由 |
|:---|:---|:---|
| 缺陷 1 修复方式 | `GcObject` 加 `allocSize` 字段 | 比 `TypeDescriptor::computeSize` 虚函数更简单，无虚调用开销 |
| 缺陷 3 修复方式 | 移除 break，始终调用 `markObject` | `markObject` 的 `marked` 守卫已足够，避免重复扫描 |
| 缺陷 4 晋升阈值 | `kPromotionAge = 2` | Aura minor GC 频率高，2 次已足够 |
| 缺陷 4 survivor 区 | 不引入，直接复用 `youngObjects_` | 避免数据结构复杂化，`age` 字段已能区分 |
| 缺陷 2 屏障范围 | 仅 `obj.field = newVal` | 局部变量赋值不涉及跨代引用 |
| 缺陷 2 屏障实现 | 复用现有 `gc_write_barrier` | API 已就绪，只需 CodeGen 接入 |

### 4.3 不破坏现有功能的验证

- ✅ `allocSize` 是新增字段，默认 0，旧代码不依赖
- ✅ `age` 是新增字段，默认 0，旧代码不依赖
- ✅ `kPromotionAge` 是新增常量，不影响现有逻辑
- ✅ `markObject` 移除 break 后，old 对象的 `marked` 守卫避免无限递归
- ✅ `genAssignExpr` 对局部变量赋值路径不变（`isGcFieldAssignment` 返回 false）
- ✅ `gc_write_barrier` 已存在，无新 API

---

## 五、Verification Steps

### 5.1 Phase 1 验证

#### 5.1.1 缺陷 1 验证：`oldBytes_` 准确

```aura
fun main(io: Io) {
    // 分配大量对象触发 minor GC
    for i in range(0, 5000) {
        let s = "iter " + i
    }
    let info = gc_stats()
    io.println(info)  // 期望 old > 0KB（缓存单例已晋升）
}
```

**预期**：
- `old` 不再是 0KB（缓存单例 + 长期存活对象正确累加）
- `gc` 可能仍为 0（如果 `oldBytes_` 未达 1MB），但 `old` 数值合理

#### 5.1.2 缺陷 3 验证：栈持有 old 指针不出错

```aura
fun main(io: Io) {
    let s = "long lived string"  // 会被 GcRootHandle 包装
    for i in range(0, 100) {
        let tmp = "temp " + i
    }
    io.println(s)  // 期望 "long lived string" 不被错误回收
    let info = gc_stats()
    io.println(info)
}
```

**预期**：
- `s` 在 minor GC 后仍能正确访问
- 无 crash / 无 use-after-free

### 5.2 Phase 2 验证

#### 5.2.1 缺陷 4 验证：年龄门槛生效

```aura
fun main(io: Io) {
    let tmp1 = "temp1"  // 应在 1-2 次 minor GC 后被回收
    for i in range(0, 10000) {
        let tmp = "iter " + i
    }
    let info = gc_stats()
    io.println(info)  // 期望 live 数量比无年龄门槛时少
}
```

**预期**：
- `tmp1` 经历 2 次 minor GC 后晋升到 old（如果在第 3 次 minor GC 时仍被引用）
- 若 `tmp1` 在第 1 次 minor GC 后已无引用，应在 `sweepPhaseYoung` 中被回收，不晋升

#### 5.2.2 缺陷 2 验证：写屏障记录跨代引用

```aura
type Holder {
    ref: string
}

fun main(io: Io) {
    let h = Holder { ref: "initial" }  // h 晋升到 old
    for i in range(0, 5000) {
        h.ref = "iter " + i  // 写屏障应记录 h 到 rememberedSet_
    }
    let info = gc_stats()
    io.println(info)
    io.println(h.ref)
}
```

**预期**：
- `h` 晋升到 old 后，`h.ref = "iter " + i` 触发写屏障
- `rememberedSet_` 包含 `h`
- minor GC 时 `h.ref` 指向的 young 对象被正确标记，不被回收
- 最后 `io.println(h.ref)` 输出最后一个 `"iter 4999"`

### 5.3 综合回归测试

```aura
fun main(io: Io) {
    let s = ""
    for i in range(0, 5000) {
        s = s + "x"
    }
    io.println(s.len())
    gc_force()
    let info = gc_stats()
    io.println(info)
    io.println(s.len())  // 期望 5000，未被错误回收
}
```

**预期**：
- 程序正常退出
- `s` 在 `gc_force()` 后仍存活
- `s.len()` 输出 5000

### 5.4 性能验证

对比修复前后的 GC 统计：

| 指标 | 修复前 | 修复后预期 |
|:---|:---:|:---:|
| `old` 字节数 | 0 KB（错误） | 准确反映晋升对象大小 |
| `gc` (major GC 次数) | 0（即使 live=4644） | 在 `old` 达 1MB 时自动触发 |
| `live` 数量 | 包含短命对象 | 更少（年龄门槛过滤） |
| minor GC 耗时 | 较高（无 rememberedSet_） | 降低（写屏障减少 old 扫描） |

---

## 六、可能的风险与应对方案

### 6.1 风险一：`allocSize` 字段增加内存开销

**问题**：每个 `GcObject` 多 8 字节（`size_t allocSize`）。

**应对**：
- ✅ Aura 对象通常较大（含字段/数据），8 字节占比可忽略
- ✅ 收益（`oldBytes_` 准确）远大于成本

### 6.2 风险二：缺陷 3 修复后 minor GC 扫描更多对象

**问题**：移除 `youngOnly && generation == 1` break 后，栈上 old 指针会触发 `markObject` + `markFields`。

**应对**：
- ✅ `markObject` 的 `marked` 守卫避免重复扫描
- ✅ old 对象的 `markFields` 仅遍历其指针字段，递归深度有限
- ⚠️ 若 profiling 显示明显性能下降，可优化为：仅当 old 对象未被标记时才 `markFields`

### 6.3 风险三：`kPromotionAge = 2` 可能不合适

**问题**：阈值过低 → 短命对象过早晋升；阈值过高 → survivor 区膨胀。

**应对**：
- ✅ 初始值 2 是保守选择，可在测试后调整
- ⚠️ 若 oldObjects_ 膨胀过快，提高到 3-4
- ⚠️ 若 youngObjects_ 积压过多，降低到 1

### 6.4 风险四：写屏障对性能影响

**问题**：每次 `obj.field = newVal` 都插入 `gc_write_barrier`，增加运行时开销。

**应对**：
- ✅ `writeBarrier` 内部仅做 2 次比较（generation 标志），极轻量
- ✅ 仅 `parent->generation == 1 && newVal->generation == 0` 时才 `rememberedSet_.insert`
- ⚠️ 热路径中频繁字段赋值可能影响性能 — 可后续优化为编译期判断（若字段类型非 GC 指针则跳过）

### 6.5 风险五：写屏障 CodeGen 漏接

**问题**：`genAssignExpr` 仅覆盖 `AssignExpr` 形式，可能漏接其他字段写入场景。

**应对**：
- ⚠️ 需检查：构造函数字段初始化、`obj.field += value`、`obj.field.field2 = value` 等
- ✅ Phase 2 实施时需全面排查 CodeGen 中所有字段写入点
- 🟡 可后续在 `genFieldAccess` 或 `genMemberExpr` 层面统一拦截

---

## 七、实施顺序与提交粒度

| 顺序 | Step | 提交点 | 依赖 | 行数估计 |
|:---:|:---|:---|:---|:---:|
| 1 | Step 1.1 | commit: "gc: fix oldBytes_ underestimation with allocSize field" | 无 | +2 |
| 2 | Step 1.2 | commit: "gc: mark old objects referenced from stack in minor GC" | Step 1.1 | +2 |
| 3 | Step 2.1 | commit: "gc: add age-based promotion threshold (kPromotionAge=2)" | Step 1.1 | +12 |
| 4 | Step 2.2 | commit: "codegen: emit gc_write_barrier for field assignments" | Step 1.1 | +46 |

**每个 Step 独立编译 + 测试，失败可回滚。**

---

## 八、改动规模总览

| 文件 | 改动 | Phase | 净增行数 |
|:---|:---|:---:|:---:|
| [runtime/types.h](file:///d:/you/Aura/runtime/types.h) | + `allocSize` + `age` 字段 | 1+2 | +4 |
| [runtime/gc.h](file:///d:/you/Aura/runtime/gc.h) | + `kPromotionAge` 常量 | 2 | +1 |
| [runtime/gc.cpp](file:///d:/you/Aura/runtime/gc.cpp) | `tryAlloc` 记录 + `promoteToOld` 改用 + `sweepPhaseAll` 改用 + `sweepPhaseYoung` 改造 + `markPhase` 栈扫描修复 | 1+2 | +14 |
| [src/CodeGen/CodeGen.h](file:///d:/you/Aura/src/CodeGen/CodeGen.h) | + 2 个私有方法声明 | 2 | +6 |
| [src/CodeGen/ExprGen.cpp](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp) | `genAssignExpr` 改造 + 2 个方法实现 | 2 | +40 |
| **合计** | | | **+65 净增** |

---

## 九、与现有 plan 的关系

| 现有 plan 项 | 状态 | 本 plan 处理 |
|:---|:---:|:---|
| [gc_features_plan.md §九 精确栈扫描](file:///d:/you/Aura/plan/gc_features_plan.md) | [-] 暂不实施 | 不实施，缺陷 3 修复不依赖精确栈扫描 |
| [gc_features_plan.md §十一 TLAB](file:///d:/you/Aura/plan/gc_features_plan.md) | [~] 延后 | 不实施，等待 sync_thread |
| [gc_features_plan.md §十三 对象可移动性](file:///d:/you/Aura/plan/gc_features_plan.md) | [~] 延后 | 不实施，对应缺陷 5（Phase 3 远期） |
| [TODO.txt §五 P3 分代年龄记录](file:///d:/you/Aura/TODO.txt) | `[ ]` | ✅ 本 plan Phase 2 Step 2.1 实施 |
| [plan/gc_promotion_issues.md](file:///d:/you/Aura/plan/gc_promotion_issues.md) | 缺陷记录 | ✅ 本 plan 细化为实施 plan |

---

## 十、后续

完成本 plan 后：
- [TODO.txt](file:///d:/you/Aura/TODO.txt) §五新增"GC 晋升机制修复"条目，标记 `[x]`
- [plan/gc_promotion_issues.md](file:///d:/you/Aura/plan/gc_promotion_issues.md) 标注缺陷 1-4 已修复，缺陷 5 延后
- [plan/gc_features_plan.md](file:///d:/you/Aura/plan/gc_features_plan.md) §十三状态保持 `[~] 延后`

**Phase 3（缺陷 5）**留待 profiling 证据充足后再启动，对应 [gc_features_plan.md §十三](file:///d:/you/Aura/plan/gc_features_plan.md) `[~] 延后` 项。
