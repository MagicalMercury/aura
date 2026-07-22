# Change Plan — Compacting GC Phase D 根因修复（GcRootHandle 方案）

> **目标**：修复 `concat_multi` 参数求值期间 compact 导致裸指针悬垂。
>
> 日期：2026-07-22
> 状态：待审查

---

## 一、分析报告

### 1.1 Codebase Scan

| 文件 | 职责 | 本计划是否改动 |
|:---|:---|:---:|
| `runtime/gc.h` | GcHeap 类声明、GcRootHandle/GcGlobalRoot 模板 | ❌（已有 GcRootHandle） |
| `runtime/gc.cpp` | GC 实现（alloc、minorGc、compact 等） | ❌（已正确实现） |
| `runtime/builtin/string.cpp` | GcString 实现、concat_multi、intern_string | ❌ |
| `runtime/builtin/string.h` | GcString 声明、concat_multi 声明 | ❌ |
| `src/CodeGen/ExprGen.cpp` | 表达式 CodeGen，concat_multi 生成在第 339-349 行 | ✅ |
| `src/CodeGen/CodeGen.h` | CodeGen 状态字段（计数器等） | ✅（新增计数器） |
| `example/test.cpp` | 测试代码（由 aurac 生成） | 自动更新 |

### 1.2 Dependency Map

```
test.aura → aurac (CodeGen/ExprGen.cpp) → test.cpp
                                            ↓
                            concat_multi({intern_string(...), from(i), ...})
                                            ↓
                    runtime/builtin/string.cpp: concat_multi()
                                            ↓
                    runtime/gc.cpp: GcHeap::alloc() → tryAlloc()
                                            ↓
                    tryAlloc: youngBytes_ >= kYoungThreshold → minorGc() → compact()
                                            ↓
                    compact: 移动对象 → 更新 GcRootHandle.ptr_ 指向的变量
                                            ↓
                    但 initializer_list 中的裸指针未被更新 → 悬垂
```

**关键数据流**：
- `intern_string("iter ")` 返回 `GcGlobalRoot::ptr_` 的**值拷贝**（裸指针）
- `from(i)` 调用 `make` → `GcHeap::alloc()` → 可能触发 `minorGc()` → `compact()`
- compact 移动对象后，`updateAllReferences` 步骤 1 会更新所有 `GcRootHandle` 指向的变量
- 但 initializer_list 中的裸指针**不是 GcRootHandle**，不会被更新

### 1.3 Interface Inventory

| 接口 | 签名 | 副作用 |
|:---|:---|:---|
| `GcRootHandle(T& ref)` | 构造函数 | 注册到 `roots_`，GC 通过它发现和更新引用 |
| `GcRootHandle::get()` | `T& get()` / `T get() const` | 返回被包装的引用/值 |
| `GcRootHandle::~GcRootHandle()` | 析构函数 | 从 `roots_` 注销 |
| `concat_multi` | `GcString* concat_multi(std::initializer_list<const GcString*>)` | 内部调用 alloc |
| `intern_string` | `GcString* intern_string(const char*, size_t)` | 返回 GcGlobalRoot::ptr_ 值拷贝 |

### 1.4 GcRootHandle 的工作原理

```cpp
// gc.h
template <typename T>
class GcRootHandle {
    T* ptr_;  // 指向实际的 GC 指针变量
};

// gc.cpp markPhase 步骤 1（标记根对象）：
for (auto* rootHandle : roots_) {
    GcObject* obj = rootHandle->get();
    if (obj) markObject(obj);  // 标记为存活
}

// gc.cpp updateAllReferences 步骤 1（compact 后自动更新）：
for (auto* rootHandle : roots_) {
    GcObject** fieldPtr = reinterpret_cast<GcObject**>(rootHandle->ptr_);
    if (fieldPtr && *fieldPtr) {
        if ((*fieldPtr)->forwarded()) {
            *fieldPtr = (*fieldPtr)->forwardingPtr();  // 自动更新！
        }
    }
}
```

**GcRootHandle 就是指针级别的 OopMap**：让 GC 知道每个引用的位置，compact 后自动更新。

### 1.5 State & Side Effects

- `argHandleCounter_`（新增）：GcRootHandle 临时变量名计数器
- 副作用：无（GcRootHandle 构造/析构自动注册/注销，作用域内有效）

---

## 二、根本原因

### 2.1 崩溃时序

```
concat_multi({intern_string("iter "), from(i), intern_string(" step "), from(i), intern_string(" done")})
```

| 步骤 | 求值 | 结果 |
|:---:|:---|:---|
| 1 | `intern_string("iter ")` | P1（旧页地址，裸指针） |
| 2 | `from(i)` (i=2159 超缓存) | `make` → `alloc` → `minorGc` → `compact` |
| 3 | compact 移动 "iter "，更新 `GcGlobalRoot::ptr_` | **但 P1 是值拷贝，未被更新** |
| 4-6 | 后续参数求值 | 新地址 |
| 7 | `concat_multi({P1(悬垂), ...})` | 访问 `P1->length` → SIGSEGV |

### 2.2 GDB 验证

用户确认：**"parts 列表中的地址后四个都被更改，第一个反而没变"** —— 完全吻合时序分析。

### 2.3 根因本质

GC compact 移动对象后，通过 `updateAllReferences` 更新所有已注册的 `GcRootHandle` 指向的变量。但 `concat_multi` 的参数是 initializer_list 中的**裸指针值拷贝**，不在 GC 跟踪范围内。

**Java 的 OopMap + Safepoint 机制**通过在 safepoint 记录所有引用位置解决此问题。**GcRootHandle 是 Aura 的 OopMap 等价物**——让 GC 跟踪每个引用的位置，compact 后自动更新。

---

## 三、Proposed Changes

### 改动 1：新增 argHandleCounter_ 字段（src/CodeGen/CodeGen.h）

**What**：在 CodeGen 类中添加 `argHandleCounter_` 计数器，用于生成唯一的 GcRootHandle 变量名。

**Where**：`src/CodeGen/CodeGen.h`，`listCounter_` 附近（第 411 行）

**Why**：生成唯一变量名避免命名冲突。

```cpp
int listCounter_ = 0;
int recordAllocCounter_ = 0;
int argHandleCounter_ = 0;  // 新增：concat_multi 参数 GcRootHandle 变量名计数器
```

### 改动 2：CodeGen 为 concat_multi 参数生成 GcRootHandle（src/CodeGen/ExprGen.cpp）

**What**：将 concat_multi 调用包裹在 IIFE 中，为每个参数生成临时变量 + GcRootHandle 保护。

**Where**：`src/CodeGen/ExprGen.cpp` 第 339-349 行

**Why**：让 GC 跟踪每个参数指针，compact 后自动更新，防止悬垂。

```cpp
// 修改前（第 339-349 行）：
std::string result = "aura_rt::concat_multi({";
for (size_t i = 0; i < chain.size(); ++i) {
    if (i) result += ", ";
    if (isStringExprInChain(chain[i])) {
        result += chain[i];
    } else {
        result += "aura_rt::GcString::from(" + chain[i] + ")";
    }
}
result += "})";

// 修改后：
int hid = argHandleCounter_++;
std::string result = "[&](){";
for (size_t i = 0; i < chain.size(); ++i) {
    std::string expr = isStringExprInChain(chain[i])
                       ? chain[i]
                       : "aura_rt::GcString::from(" + chain[i] + ")";
    result += "auto _a" + std::to_string(hid) + "_" + std::to_string(i)
            + " = " + expr + ";";
    result += "aura_rt::GcRootHandle<aura_rt::GcString*> _h"
            + std::to_string(hid) + "_" + std::to_string(i)
            + "(_a" + std::to_string(hid) + "_" + std::to_string(i) + ");";
}
result += "return aura_rt::concat_multi({";
for (size_t i = 0; i < chain.size(); ++i) {
    if (i) result += ", ";
    result += "_a" + std::to_string(hid) + "_" + std::to_string(i);
}
result += "}); }()";
```

**生成的代码变化**：

```cpp
// 修改前：
aura_rt::concat_multi({aura_rt::intern_string("iter "), aura_rt::GcString::from(i), ...})

// 修改后：
[&](){
    auto _a0_0 = aura_rt::intern_string("iter ");
    aura_rt::GcRootHandle<aura_rt::GcString*> _h0_0(_a0_0);
    auto _a0_1 = aura_rt::GcString::from(i);
    aura_rt::GcRootHandle<aura_rt::GcString*> _h0_1(_a0_1);
    ...
    return aura_rt::concat_multi({_a0_0, _a0_1, ...});
}()
```

### 工作原理

1. 每个参数求值后存入临时变量 `_a0_i`
2. `GcRootHandle` 包装该变量，注册到 GC `roots_`
3. 若后续参数求值触发 compact，`updateAllReferences` 步骤 1 自动更新 `_a0_i` 为新地址
4. `concat_multi` 收到的所有指针都是最新的
5. IIFE 结束时，所有 `GcRootHandle` 析构，从 `roots_` 注销

---

## 四、Impact Analysis

| 组件 | 影响 | 说明 |
|:---|:---|:---|
| GcHeap | 无变更 | GcRootHandle 机制已存在 |
| compact | 无变更 | 正常移动对象，自动更新 GcRootHandle |
| minorGc | 无变更 | 正常执行 mark-sweep + compact |
| concat_multi | 无 API 变更 | 生成的调用代码变更 |
| CodeGen | 新增 argHandleCounter_ + 修改 concat_multi 生成 | 唯一变量名生成 |
| 其他 concat 路径 | 不受影响 | `concat(a, b)` 双元素路径不走 concat_multi |

**无 Breaking Change**：所有变更新增，不修改现有 API 签名。

**GC 统计正常**：`gc>0 minor>0`，compact 正常触发，内存正常回收。

---

## 五、Boundary Condition Handling

| Boundary Condition | Current Handling | Planned Handling | Test Strategy |
|:---|:---|:---|:---|
| 参数求值期间触发 compact | 崩溃 | GcRootHandle 自动更新指针 | 集成测试 |
| GcRootHandle 变量名冲突 | 不存在 | argHandleCounter_ 生成唯一名 | 代码审查 |
| 嵌套 concat_multi | 不存在 | argHandleCounter_ 每次递增 | 单元测试 |
| GcRootHandle 构造/析构开销 | 不存在 | 每参数 2 次 register/unregister | 性能可接受 |
| 空参数列表 | 不存在 | chain.size() >= 3 才走此路径 | 代码审查 |
| 参数为 nullptr | 不存在 | GcRootHandle 支持 nullptr | 代码审查 |

---

## 六、Test Plan

### 6.1 核心测试

```powershell
cd d:\you\Aura
cmake --build build          # 重新编译 aurac
cmake --build runtime/build   # 重新编译 runtime
.\example\compile.cmd         # 重新生成 test.cpp 并编译
.\example\test.exe            # 运行测试
```

**预期**：
```
GC: alloc=... gc>=0 minor>=0 pages=<50
Run exit: 0
```

### 6.2 验证点

- [ ] test.exe 退出码 0
- [ ] 无 SIGSEGV/ACCESS_VIOLATION
- [ ] **GC 统计正常（minor GC 次数 > 0）**
- [ ] **compact 生效（pages 数量减少）**
- [ ] 生成的 test.cpp 包含 GcRootHandle 包装

### 6.3 回归测试

- 双元素 concat 路径不受影响（`"a" + b` 不走 concat_multi）
- 禁用 compact 触发点后测试仍正常

---

## 七、Implementation Steps

### Step 1: 新增 argHandleCounter_ 字段（src/CodeGen/CodeGen.h）
- 在 `listCounter_` 附近添加 `int argHandleCounter_ = 0;`
- **验证**：编译通过

### Step 2: 修改 concat_multi 生成代码（src/CodeGen/ExprGen.cpp）
- 修改第 339-349 行，生成 IIFE + GcRootHandle 包装
- **验证**：`cmake --build build` 编译通过

### Step 3: 重新生成 test.cpp 并运行测试
- 执行 `example/compile.cmd`
- 运行 `example/test.exe`
- **验证**：退出码 0，GC 统计 minor > 0，pages 减少

### Rollback
- 若测试失败，回退 Step 2 的 CodeGen 改动
- 禁用 compact 触发点验证非 compact 路径

---

## 八、Risks & Mitigations

| 风险 | 可能性 | 影响 | 缓解 |
|:---|:---|:---|:---|
| GcRootHandle 构造/析构开销 | 低 | 每参数 2 次 register/unregister | vector push_back/erase，O(1) |
| 生成的代码膨胀 | 低 | 每参数多 2 行 | 可读性略降，但正确性优先 |
| 嵌套 IIFE 命名冲突 | 极低 | argHandleCounter_ 保证唯一 | 计数器递增 |
| GcRootHandle 析构异常 | 极低 | roots_ 未清理 | unregisterRoot 无异常操作 |

---

## 九、方案对比

| 方案 | GC 统计 | compact | 复杂度 | 根本性 | 问题 |
|:---|:---:|:---:|:---:|:---:|:---|
| 禁用整个 GC（GcSuspendGuard） | gc=0 ❌ | 禁用 | 低 | 否 | GC 完全禁用 |
| 禁用 compact（GcCompactSuspendGuard） | gc>0 | 禁用 | 低 | 否 | compact 可能永不执行 |
| Pin 不移动 | gc>0 | 部分 | 中 | 否 | CodeGen 改动 = GcRootHandle |
| **GcRootHandle** ✅ | **gc>0** | **正常** | **中** | **是** | **无副作用** |

---

## 十、后续扩展（Phase D-2）

当前 Phase D-1 仅修复 concat_multi。其他 11 类场景（string_eq、concat 二元、记录字面量、列表字面量、用户函数调用等）存在同类问题，后续可系统性修复：

1. 提取公共辅助函数 `genSafeArgs`，为 GC 指针参数生成 GcRootHandle
2. 在 `genCallExpr`、`genMethodCall`、`genRecordExpr`、`genListExpr` 中调用
3. 利用 `isGcPointerType` 判断参数类型，仅为 GC 指针参数生成 GcRootHandle

这需要更大的 CodeGen 重构，作为后续工作。
