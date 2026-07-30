# ArrayView<T> 零拷贝视图 — 详细实施方案

## 4.1 标题与元数据

- **Plan Title**: ArrayView<T> 零拷贝视图 — 统一 ViewSemType + 隐式深拷贝退化
- **Author/Agent**: Aura Agent
- **Date**: 2026-07-30
- **Related modules/packages**:
  - `src/Sema/SemType.h`, `src/Sema/SemType.cpp`
  - `src/Sema/BuiltinRegistry.h`
  - `src/Sema/SemAnalyzer.h`, `src/Sema/SemAnalyzer.cpp`
  - `src/Sema/Checker/ExprInfer.cpp`
  - `src/Sema/Symbol.h`, `src/Sema/SymbolTable.h`
  - `src/CodeGen/CodeGen.h`, `src/CodeGen/TypeMap.cpp`
  - `src/CodeGen/StmtGen.cpp`, `src/CodeGen/ExprGen.cpp`
  - `runtime/builtin/array.h`
  - `runtime/gc/gc.h`, `runtime/gc/handles.h`

## 4.2 目标

为 Aura 增加 Array<T> 的零拷贝 slice 视图机制：通过新增 ViewSemType 绑定 owner 类型实现类型系统统一处理，view 调用修改方法时隐式深拷贝并永久退化为 owner 类型，view 与原类型在 Aura 层完全等价（不暴露视图类型）。

## 4.3 当前状态摘要（分析报告）

### 代码扫描

| 文件 | 责任 |
|------|------|
| [src/Sema/SemType.h](file:///d:/you/Aura/src/Sema/SemType.h) | SemType 抽象基类 + 所有具体类型（Error/Prim/None/Record/Union/List/Func/Interface/Generic/Iter） |
| [src/Sema/SemType.cpp](file:///d:/you/Aura/src/Sema/SemType.cpp) | SemType 各子类的 equals/clone 实现 |
| [src/Sema/BuiltinRegistry.h](file:///d:/you/Aura/src/Sema/BuiltinRegistry.h) | 内置类型/方法注册表，单例；ReturnTypeInfo 含 Kind::Named/Generic/None/Generator |
| [src/Sema/SemAnalyzer.cpp](file:///d:/you/Aura/src/Sema/SemAnalyzer.cpp#L114-L149) | `semTypeFromBuiltinReturn`（L114）将 ReturnTypeInfo 转换为 SemType；`isAssignable`（L151）类型兼容检查 |
| [src/Sema/Checker/ExprInfer.cpp](file:///d:/you/Aura/src/Sema/Checker/ExprInfer.cpp#L263-L380) | `inferMethodCall`（L263）方法调用类型推断；L321 推断 objType，L325-334 设置 typeKey |
| [src/Sema/Symbol.h](file:///d:/you/Aura/src/Sema/Symbol.h#L29-L53) | `Symbol` 结构，含 `std::unique_ptr<SemType> type`（不可变 unique_ptr） |
| [src/Sema/SymbolTable.h](file:///d:/you/Aura/src/Sema/SymbolTable.h#L56-L88) | `SymbolTable` 类，`lookup` 返回 `Symbol*`（可变指针，可修改字段） |
| [src/CodeGen/TypeMap.cpp](file:///d:/you/Aura/src/CodeGen/TypeMap.cpp#L170-L208) | `mapSemType`（L170）SemType → C++ 类型字符串 |
| [src/CodeGen/StmtGen.cpp](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L66-L242) | `genLetStmt`（L66）变量声明生成；L188-192 GcRootHandle 包装；L73-94 SemType → C++ 类型 |
| [src/CodeGen/ExprGen.cpp](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L648-L805) | `genMethodCall`（L648）方法调用生成；L792-804 genGcRootedArgs 包装 |
| [runtime/builtin/array.h](file:///d:/you/Aura/runtime/builtin/array.h#L941-L1004) | 已存在 ArrayView 类（owner_/start_/len_，operator[]/front/back/slice） |
| [runtime/gc/gc.h](file:///d:/you/Aura/runtime/gc/gc.h#L73-L100) | `GcRootHandle<T>` 模板；含 `rebind(T& ref)` 方法（L87-90）可更新指针 |
| [runtime/gc/handles.h](file:///d:/you/Aura/runtime/gc/handles.h#L20-L40) | GcRootHandle 构造/析构/拷贝实现 |

### 依赖映射

```
inferMethodCall (ExprInfer.cpp:263)
    ├─ inferExpr (object) → objType
    ├─ BuiltinRegistry::findMethod(typeKey, method, argc) → entry
    └─ semTypeFromBuiltinReturn(ret) (SemAnalyzer.cpp:114)
            └─ Kind::Generic → ErrorSemType (L146, bug)

genLetStmt (StmtGen.cpp:66)
    ├─ dynamic_cast<RecordSemType/GenericSemType/ListSemType/PrimSemType>
    └─ isGcPointerType(type) → GcRootHandle 包装

genMethodCall (ExprGen.cpp:648)
    └─ genGcRootedArgs (L36) → IIFE + GcRootHandle

mapSemType (TypeMap.cpp:170)
    └─ dynamic_cast 各 SemType 子类 → C++ 类型字符串
```

### 接口清单

**SemType 体系**（SemType.h）：
- `equals(const SemType&) const → bool`：结构等价
- `toString() const → string`：可读名
- `clone() const → unique_ptr<SemType>`：深拷贝

**Symbol**（Symbol.h:29-53）：
- `type`：`std::unique_ptr<SemType>`，**不可变**（unique_ptr 不支持重新赋值）

**SymbolTable**（SymbolTable.h:73）：
- `lookup(name) → Symbol*`：返回可变指针，可直接修改字段（但 `type` 是 unique_ptr，无法整体替换）

**GcRootHandle**（gc.h:73-100）：
- `rebind(T& ref)`（L87）：更新被包装的引用目标（同步更新 ptr_ref_）
- 模板参数 T 静态绑定，无法运行时改变

### 业务逻辑提取

当前 slice 返回 `Kind::Generic` → `semTypeFromBuiltinReturn` 返回 ErrorSemType → CodeGen 推断 `type = "auto"` → `isGcPointerType("auto") = false` → view 裸指针 → GC 悬垂。

### 状态与副作用

- `SymbolTable` 的 `Symbol::type` 是 unique_ptr，**整体替换需要 reset()**
- `GcRootHandle<T>` 的 T 静态绑定，退化时类型变化需特殊处理
- `typeStore_`（SemAnalyzer.h）延长 SemType 生命周期，避免悬垂

## 4.4 拟议变更

### 变更 1：新增 ViewSemType

**What**：在 SemType 体系中新增 ViewSemType，持 ownerType 引用，与 owner 等价。
**Where**：[src/Sema/SemType.h](file:///d:/you/Aura/src/Sema/SemType.h) 新增（L124 IterSemType 之后），[src/Sema/SemType.cpp](file:///d:/you/Aura/src/Sema/SemType.cpp) 新增实现。
**Why**：slice 返回类型需绑定 owner，供 Sema/CodeGen 统一处理；与 owner 等价实现 Aura 层透明。

### 变更 2：ReturnTypeInfo 新增 View kind

**What**：ReturnTypeInfo 新增 `Kind::View` 和 `View(ownerTypeName)` 工厂。
**Where**：[src/Sema/BuiltinRegistry.h](file:///d:/you/Aura/src/Sema/BuiltinRegistry.h#L40-L49)。
**Why**：区分 slice 返回类型为视图，传入 ownerTypeName 供 Sema 构造 ViewSemType。

### 变更 3：slice 方法注册改为 View 返回

**What**：[T] 和 string 的 slice 方法返回类型改为 `ReturnTypeInfo::View`。
**Where**：[src/Sema/BuiltinRegistry.h](file:///d:/you/Aura/src/Sema/BuiltinRegistry.h#L239)（string.slice）和 L255（[T].slice）。
**Why**：触发 ViewSemType 推断路径。

### 变更 4：semTypeFromBuiltinReturn 处理 View kind

**What**：新增 `Kind::View` 分支，接收 objType 参数构造 ViewSemType。
**Where**：[src/Sema/SemAnalyzer.cpp](file:///d:/you/Aura/src/Sema/SemAnalyzer.cpp#L114-L149) 签名扩展 + 新增 case。
**Why**：ViewSemType 需要绑定调用方 objType。

### 变更 5：inferMethodCall 传递 objType

**What**：`Kind::View` 分支调用 `semTypeFromBuiltinReturn(ret, objType)`。
**Where**：[src/Sema/Checker/ExprInfer.cpp](file:///d:/you/Aura/src/Sema/Checker/ExprInfer.cpp#L342-L351)。
**Why**：为 View kind 提供 ownerType 来源。

### 变更 6：ViewSemType + 修改方法触发类型退化

**What**：inferMethodCall 检测 ViewSemType + 修改方法（append/pop/insert/remove/clear/reserve），返回 ownerType，并更新 symtab 中变量类型。
**Where**：[src/Sema/Checker/ExprInfer.cpp](file:///d:/you/Aura/src/Sema/Checker/ExprInfer.cpp#L336-L380) 新增逻辑。
**Why**：实现隐式深拷贝退化，后续操作按 owner 类型处理。

### 变更 7：Symbol 类型重新绑定接口

**What**：Symbol 新增 `updateType(unique_ptr<SemType>)` 方法（reset 旧 type）。
**Where**：[src/Sema/Symbol.h](file:///d:/you/Aura/src/Sema/Symbol.h#L29-L53)。
**Why**：unique_ptr 不能直接赋值，需 reset() 替换。

### 变更 8：mapSemType 处理 ViewSemType

**What**：ViewSemType → `aura_rt::ArrayView<T>*`（根据 ownerType 映射）。
**Where**：[src/CodeGen/TypeMap.cpp](file:///d:/you/Aura/src/CodeGen/TypeMap.cpp#L170-L208)。
**Why**：CodeGen 生成正确的 C++ 类型。

### 变更 9：isGcPointerType 识别 ArrayView*

**What**：`ArrayView<T>*` 自动识别为 GC 指针（字符串匹配 `aura_rt::Array<` → 改为也匹配 `aura_rt::ArrayView<`）。
**Where**：[src/CodeGen/TypeMap.cpp](file:///d:/you/Aura/src/CodeGen/TypeMap.cpp#L32-L39)。
**Why**：确保 view 变量被 GcRootHandle 包装。

### 变更 10：genLetStmt 识别 ViewSemType

**What**：dynamic_cast<ViewSemType> 生成 `aura_rt::ArrayView<T>*` 类型。
**Where**：[src/CodeGen/StmtGen.cpp](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L73-L94)。
**Why**：让 `let view = arr.slice(...)` 生成正确类型。

### 变更 11：genMethodCall 修改方法生成重新绑定

**What**：检测 ViewSemType + 修改方法，生成 `view.get() = view.get()->method(args)`。
**Where**：[src/CodeGen/ExprGen.cpp](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L648-L805)。
**Why**：实现隐式深拷贝退化，view 指针重新绑定到新 Array*。

### 变更 12：genAssignExpr view[i]=val 触发深拷贝

**What**：检测 view[i] = val，生成 `view.get() = view.get()->set(idx, val)`（ArrayView::set 深拷贝后赋值并返回新 Array*）。
**Where**：[src/CodeGen/ExprGen.cpp](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L829-L880)。
**Why**：view[i] = val 与修改方法语义一致，触发深拷贝退化。

### 变更 13：ArrayView 新增修改方法 + ViewIterator

**What**：ArrayView 新增 append/pop/insert/remove/clear/reserve/set 方法（深拷贝返回 Array*），新增 begin/end + ViewIterator。
**Where**：[runtime/builtin/array.h](file:///d:/you/Aura/runtime/builtin/array.h#L941-L1004)。
**Why**：支持退化机制和 for-in 迭代。

### 变更 14：inferIndexExpr + genIndexExpr 识别 ViewSemType

**What**：inferIndexExpr 中 ViewSemType 的索引返回 owner 的元素类型；genIndexExpr 生成 `(*view)[i]`。
**Where**：[src/Sema/Checker/ExprInfer.cpp](file:///d:/you/Aura/src/Sema/Checker/ExprInfer.cpp) inferIndexExpr，[src/CodeGen/ExprGen.cpp](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L819-L823) genIndexExpr。
**Why**：支持 view[i] 只读访问。

### 变更 15：for-in 迭代识别 ViewSemType

**What**：StmtChecker 中 for-in 识别 ViewSemType，委托 owner 迭代；CodeGen 生成 `for (auto& x : *view)`。
**Where**：src/Sema/Checker/StmtChecker.cpp（for-in 处理），src/CodeGen/StmtGen.cpp（for-in 生成）。
**Why**：支持 `for x in view { ... }`。

## 4.5 影响分析

### 受影响组件

| 组件 | 影响 | 破坏性 |
|------|------|--------|
| SemType 体系 | 新增 ViewSemType | 无（向后兼容） |
| BuiltinRegistry | ReturnTypeInfo 新增 Kind::View | 无（新增枚举值） |
| semTypeFromBuiltinReturn | 签名扩展（新增 objType 参数） | ⚠️ 所有调用点需更新 |
| inferMethodCall | 新增 View/退化逻辑 | 无 |
| Symbol | 新增 updateType 方法 | 无 |
| mapSemType | 新增 ViewSemType 分支 | 无 |
| isGcPointerType | 识别 ArrayView* | 无 |
| genLetStmt | 新增 ViewSemType 处理 | 无 |
| genMethodCall | 新增修改方法重新绑定 | 无 |
| genAssignExpr | 新增 view[i]=val 处理 | 无 |
| ArrayView 类 | 新增修改方法/迭代器 | 无（纯新增） |
| for-in 迭代 | 新增 ViewSemType 识别 | 无 |

### ⚠️ BREAKING

- `semTypeFromBuiltinReturn` 签名变更：新增 `const SemType* objType = nullptr` 参数（默认值保证向后兼容）

### 兼容性

- 现有 [T] 代码不受影响（ListSemType 路径不变）
- 现有 string.slice 返回 string 路径暂时保留（直到 GcStringView 实现）

## 4.6 边界条件处理策略

| Boundary Condition | Current Handling | Planned Handling | Test Strategy |
| --- | --- | --- | --- |
| view GC 悬垂（slice 返回裸指针） | ❌ 崩溃（ASAN 报错） | ViewSemType → ArrayView* + GcRootHandle 包装 | K5: gc_force() 后访问 view |
| view 调用 append 后类型变化 | 无（返回 ErrorSemType） | 隐式深拷贝退化，symtab 更新类型 | K8: view.append 后再 append |
| view[i] = val 触发深拷贝 | 无（ErrorSemType） | genAssignExpr 生成 view.get() = view.get()->set(i, val) | K9: view[0]=99 后验证 arr 不变 |
| for x in view 迭代 | 无（ErrorSemType） | ViewIterator 限制范围 [start_, start_+len_) | K10: for-in 迭代 view |
| 空数组 slice(0, 0) | 返回 view，len_=0 | 检查 start/len 合法性，返回空 view | K7: emptyView.len() == 0 |
| slice 越界（start+len > length） | 抛 IndexError | 保持抛 IndexError | （已有测试） |
| view 嵌套 slice | 返回新 view | view.slice 返回新 ViewSemType | K4: view2 = view.slice(1, 3) |
| view 传给函数参数（func f(arr: [int])） | 无（ErrorSemType） | ViewSemType.equals(ListSemType) = true，直接传递 | K11: process(view) |
| 修改方法深拷贝后 OOM | 无 | 抛 OutOfMemoryError | （GC 已有处理） |
| view 修改方法后原 view 操作（如 view2 = view.slice 后 view.append） | 无 | view 类型退化，view2 仍是 view（指向新 Array），原 arr 不变 | K12: view.append 后 view2 访问 |
| view 修改方法后 GC | 无 | view 退化为 Array*，被 GcRootHandle 保护 | K13: view.append 后 gc_force |
| view.operator[] 越界 | 抛 IndexError | 保持抛 IndexError | （已有测试） |
| view.front/back 空视图 | 抛 IndexError | 保持抛 IndexError | （已有测试） |
| view + view 拼接（如果未来支持） | 无 | 未在本次实现范围 | N/A |

## 4.7 测试方案

### 单元测试（在 example/test.aura 中）

- **K1-K2**：front() / capacity()（已实现，保留）
- **K3**：slice 基本读写（view[i] = val 触发深拷贝，验证 arr 不变）
- **K4**：嵌套 slice（view2 = view.slice）
- **K5**：GC 后 view 引用有效（gc_force 后访问 view）
- **K6**：大数组 slice + 多次 GC
- **K7**：空 slice（emptyView.len() == 0）
- **K8**：view.append 后类型退化（后续 append 无深拷贝）
- **K9**：view[i] = val 触发深拷贝（验证 arr 不变，view 退化）
- **K10**：for x in view 迭代（验证遍历范围正确）
- **K11**：函数参数传递（process(view) 视为 [int]）
- **K12**：view.append 后 view2 仍是 view（指向新 Array）
- **K13**：view.append 后 gc_force（view 退化为 Array*，被 GcRootHandle 保护）

### 集成测试

- 编译通过（非 ASAN 模式）
- 运行 test.exe 无崩溃，输出正确
- ASAN 模式验证无内存错误（可选，需 CLANG64）

### 边界测试

- 空 view 操作（front/back 抛 IndexError）
- view 嵌套 slice 越界抛 IndexError
- view.append OOM（手动构造大数组）

### 回归风险

- 现有 [T] 代码：ListSemType 路径不变，无回归
- 现有 string.slice：返回 string 路径暂时保留，无回归
- GcRootHandle 模板：新增 ArrayView* 特化，不影响现有 Array*/GcString*

## 4.8 实施步骤（有序）

### 阶段 1：Sema 层（类型系统）

**步骤 1.1**：新增 ViewSemType（SemType.h + SemType.cpp）
- 实现 equals（与 owner 等价）、toString、clone
- 期望：编译通过，无类型检查错误

**步骤 1.2**：ReturnTypeInfo 新增 View kind（BuiltinRegistry.h）
- 新增 `Kind::View` 枚举值和 `View(ownerTypeName)` 工厂
- slice 方法注册改为 View 返回
- 期望：编译通过

**步骤 1.3**：semTypeFromBuiltinReturn 签名扩展（SemAnalyzer.h + .cpp）
- 新增 `const SemType* objType = nullptr` 参数
- 新增 `Kind::View` 分支：返回 `ViewSemType::make(objType->clone())`
- 期望：编译通过，slice 返回 ViewSemType

**步骤 1.4**：inferMethodCall 传递 objType（ExprInfer.cpp）
- `Kind::View` 分支调用 `semTypeFromBuiltinReturn(ret, objType.get())`
- 期望：slice 调用推断为 ViewSemType

**步骤 1.5**：Symbol 新增 updateType（Symbol.h）
- 新增 `void updateType(std::unique_ptr<SemType> t) { type = std::move(t); }`
- 期望：编译通过

**步骤 1.6**：inferMethodCall 类型退化（ExprInfer.cpp）
- 检测 ViewSemType + 修改方法（硬编码列表）
- 返回 ownerType，调用 `sym->updateType(ownerType->clone())`
- 期望：view.append 后 view 类型退化为 ListSemType

**步骤 1.7**：isAssignable 兼容 ViewSemType（SemAnalyzer.cpp）
- ViewSemType 与 owner 互相兼容（已通过 equals 实现）
- 期望：函数参数传递 view 视为 [int]

### 阶段 2：CodeGen 层

**步骤 2.1**：mapSemType 处理 ViewSemType（TypeMap.cpp）
- ViewSemType → `aura_rt::ArrayView<T>*`（根据 ownerType 映射）
- 期望：let view = arr.slice(...) 生成正确类型

**步骤 2.2**：isGcPointerType 识别 ArrayView*（TypeMap.cpp）
- 字符串匹配 `aura_rt::ArrayView<` 开头 + `*` 结尾
- 期望：view 变量被 GcRootHandle 包装

**步骤 2.3**：genLetStmt 识别 ViewSemType（StmtGen.cpp）
- dynamic_cast<ViewSemType> → 调用 mapSemType
- 期望：view 变量生成 `GcRootHandle<ArrayView<T>*>`

**步骤 2.4**：genMethodCall 修改方法重新绑定（ExprGen.cpp）
- 检测 ViewSemType + 修改方法
- 生成 `view.get() = view.get()->method(args)`
- 期望：view.append 生成重新绑定代码

**步骤 2.5**：genAssignExpr view[i]=val（ExprGen.cpp）
- 检测 view 索引赋值
- 生成 `view.get() = view.get()->set(idx, val)`
- 期望：view[0]=99 触发深拷贝

**步骤 2.6**：inferIndexExpr + genIndexExpr 识别 ViewSemType
- inferIndexExpr：ViewSemType 索引返回 owner 元素类型
- genIndexExpr：生成 `(*view)[i]`
- 期望：view[i] 只读访问正确

**步骤 2.7**：for-in 迭代识别 ViewSemType（StmtChecker + StmtGen）
- StmtChecker：for-in ViewSemType 合法
- StmtGen：生成 `for (auto& x : *view)`
- 期望：for x in view 正确迭代

### 阶段 3：Runtime 层

**步骤 3.1**：ArrayView 新增修改方法（array.h）
- append/pop/insert/remove/clear/reserve/set（深拷贝返回 Array*）
- 期望：编译通过

**步骤 3.2**：ArrayView 新增 ViewIterator + begin/end（array.h）
- ViewIterator 限制范围 [start_, start_+len_)
- 期望：for-in 迭代正确

### 阶段 4：测试

**步骤 4.1**：编译（非 ASAN）
- `cmake --build build && cmake --build runtime/build`
- `aurac example/test.aura --cpp example/test.cpp -o example/test.exe`
- 期望：编译通过

**步骤 4.2**：运行测试
- `example/test.exe`
- 期望：所有 K1-K13 测试通过

**步骤 4.3**：ASAN 验证（可选）
- CLANG64 编译 test.cpp
- 期望：无内存错误

## 4.9 风险与缓解

| 风险 | 缓解 |
| --- | --- |
| semTypeFromBuiltinReturn 签名变更影响调用点 | 使用默认参数 `objType = nullptr`，向后兼容 |
| ViewSemType 类型退化在 symtab 中更新失败 | 所有路径调用 `sym->updateType` 后验证 `sym->type` 非 null |
| GcRootHandle 模板参数静态绑定，退化时类型变化 | view 变量统一用 `GcRootHandle<ArrayView<T>*>`，退化时通过 `rebind` 更新指针，但模板参数不变（ArrayView* 和 Array* 都是 GcObject* 子类，reinterpret_cast 安全） |
| view[i]=val 深拷贝性能 O(n) | 文档警告，用户需显式 `let arr = view.to_array()` 后修改（未来实现 to_array） |
| for-in ViewIterator 边界错误 | 单元测试覆盖空 view、单元素 view、满范围 view |
| 函数参数传递 view 时 CodeGen 类型不匹配 | ViewSemType.equals(ListSemType)=true，但 CodeGen 生成 `ArrayView<T>*` 传给 `Array<T>*` 参数需特殊处理（选项 C：函数参数直接接收 ArrayView*） |
| 修改方法硬编码列表遗漏 | 单元测试覆盖所有修改方法 |

### 假设

- Aura 的 `let` 变量底层指针可通过 `GcRootHandle::rebind` 更新（已验证 gc.h:87）
- ArrayView 和 Array 都是 GcObject 子类，指针可 reinterpret_cast（已验证 array.h:34, 949）
- ViewSemType 与 owner equals 返回 true 不影响现有 ListSemType 处理（equals 是对称的）

### 未知

- 函数参数传递 view 时 CodeGen 具体生成方式（选项 C 实现细节需在步骤 2.4 中确定）
- for-in 生成 `for (auto& x : *view)` 是否需要 ViewIterator 的 `operator*` 返回引用（已实现）
