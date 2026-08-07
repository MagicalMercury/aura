# 联合变体含接口（Variant::isPtrActive 视图 self 子偏移支持）— 草案

> 工作流：起草 issue 实现草案（工作流 2）
> 提出时间：2026-08-06
> 状态：**待审查**（已根据三轮审查反馈修正：①补齐 CodeGen 层致命缺口、新增 ViewRoot 构造前置依赖改动 F、补 genRecordToViewIIFE 接口基名提取；②补 alloc 窗口已知限制；③修正已知限制真实性——record 变体为指针变体递归扫描天然安全（删除原限制 1）、tryAllocSlow 未 memset 为独立真实 bug（改写原限制 5）、match 分支补充 _match_val 前提）。
> 来源：[TODO.txt](file:///d:/you/Aura/TODO.txt) [二] P2「联合变体含接口」（L109-123）。
> 前置依赖：[x] P1 联合类型 GC 安全（Variant<T...> runtime 已落地）；[x] P1 迭代器 GC 安全修复（ViewRoot 机制）。

---

## 1. 元信息

| 项目 | 内容 |
| ---- | ---- |
| Plan 标题 | 联合变体含接口（Variant::isPtrActive 视图 self 子偏移支持） |
| 相关模块 | runtime: `variant.h`、`types.h`、`mark_sweep.cpp`、`compact.cpp`；CodeGen: `StmtGen.cpp`、`ExprGen.cpp`、`DeclGen.cpp`、`TypeMap.cpp`；Sema: `DeclChecker.cpp` |
| 触发场景 | 用户写 `Variant<Stringer, int>` / `Variant<Iterator<T>, NoneType>` 等含接口视图变体的联合 |
| 优先级 | P2（功能扩展，放开 Sema 拦截） |
| 前置条件 | Variant<T...> 已实现；genRecordStruct 已支持 "field+ViewType" 复合偏移；genLetStmt 已有 record→适配器 view() 预转换机制（StmtGen.cpp L319-352） |

---

## 2. Objectives

放开 `variantStorageUnsafe` 对接口变体的编译期拦截，使 `Variant<InterfaceView, ...>` 能安全地存放接口视图值。核心是让 Variant 的 per-变体 desc 正确描述"storage_ 起始 + 视图内 self 子偏移"处的有效 GC 指针，**并同步修复 CodeGen 装箱/match 分支路径的视图保护缺口**，使 mark/compact/promoteToOld 三处扫描路径与 match 分支访问都自动正确。

---

## 3. Current State Summary（分析报告）

### 3.1 Codebase Scan

| 文件 | 职责 | 关键点 |
| ---- | ---- | ---- |
| [variant.h](file:///d:/you/Aura/runtime/builtin/variant.h) | Variant<T...> runtime | L26-91：`index_` + `storage_[]`；L52-63 `descForI<I>()` 按变体类型生成 per-变体 desc（指针变体 1 字段 offset=kStorageOffset，值变体 0 字段）；L76-78 dynamicDesc 钩子 |
| [types.h:82-97](file:///d:/you/Aura/runtime/types.h#L82-L97) | TypeDescriptor | 现有钩子仅 dynamicDesc（L96） |
| [mark_sweep.cpp:182](file:///d:/you/Aura/runtime/gc/mark_sweep.cpp#L182) | markFields | 已接 dynamicDesc ✓ |
| [mark_sweep.cpp:301](file:///d:/you/Aura/runtime/gc/mark_sweep.cpp#L301) | promoteToOld | **未接 dynamicDesc**（独立 bug）→ Variant 晋升漏扫 |
| [compact.cpp:408](file:///d:/you/Aura/runtime/gc/compact.cpp#L408) | updateObjectFields | 已接 dynamicDesc ✓ |
| [DeclChecker.cpp:12](file:///d:/you/Aura/src/Sema/Checker/DeclChecker.cpp#L12) | variantStorageUnsafe | 拦截 InterfaceSemType |
| [StmtGen.cpp:102-135](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L102-L135) | genUnionBoxingImpl | **致命缺口 1**：对接口视图变体生成 `GcRootHandle<Stringer>(_bxN)`（值类型当 GC 指针），compact 覆写视图前 8 字节（函数指针）→ 视图破坏 |
| [StmtGen.cpp:319-352](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L319-L352) | genLetStmt record→适配器 view() | 已有预转换机制（IIFE 内 GcRootHandle<Record*> 保护 + XStringer::view） |
| [StmtGen.cpp:1686-1698](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L1686-L1698) | genMatchStmt 视图分支 | **致命缺口 3**：生成 `auto& s = _match_val->get<idx>();`，self 裸指针无 GC 保护 |
| [ExprGen.cpp:22](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L22) | isHeapSemType | InterfaceSemType → true（视图非指针但按堆处理，是 §3.3 缺口的根源） |
| [iterator.h:72-81](file:///d:/you/Aura/runtime/builtin/iterator.h#L72-L81) | ViewRoot | 内部 `GcRootHandle<GcObject*>` 持 `it.self`（正确的 GC 指针保护） |

### 3.2 Dependency Map

```
用户代码 Variant<Stringer, int>
  → Sema: variantStorageUnsafe 拦截（DeclChecker.cpp:12）✗ 当前报错
  → 放开拦截后：
    [CodeGen 装箱路径 genUnionBoxingImpl]
    ├─ 缺口 1: GcRootHandle<Stringer>(_bxN) → compact 覆写视图前 8 字节 → 视图破坏
    └─ 缺口 2: let x: Stringer|int = myImplRecord → idx 匹配失败（MyImpl* ≠ Stringer）→ 编译错误
    [CodeGen match 路径 genMatchStmt]
    └─ 缺口 3: auto& s = _match_val->get<idx>(); → self 裸指针 → 分支体 alloc 悬垂
  → runtime Variant descForI: 指针/值分支，无接口视图分支
  → GC mark/compact: 已接 dynamicDesc ✓；promoteToOld 未接（独立 bug）
```

### 3.3 Interface Inventory

| 接口 | 契约 | 当前状态 |
| ---- | ---- | ---- |
| `Variant<Ts...>::descForI<I>()` | 返回 per-变体 desc | 缺少接口视图变体分支 |
| `variantStorageUnsafe(SemType)` | 拦截不安全变体 | L12 拦截 InterfaceSemType |
| `genUnionBoxingImpl` | 生成装箱代码 | 对接口视图变体生成错误的 GcRootHandle<视图值> |
| `genLetStmt` record→view 预转换 | IIFE 内构造适配器 + ViewRoot | 已实现（可复用到 genUnionBoxingImpl） |
| `genMatchStmt` 视图分支 | 取出变体值 | self 裸指针无保护 |
| `promoteToOld` | 扫描字段判断 young 引用 | 未接 dynamicDesc（独立 bug） |

### 3.4 Business Logic Extraction

- Variant 的 GC 安全依赖 **dynamicDesc 钩子**：扫描时按运行时 index_ 取 per-变体 desc。
- 接口视图（如 `Stringer`）是值类型 `{方法Fn..., GcObject* self}`，self 位于视图内 8~16B 子偏移处。
- Variant storage_ 内存的是视图值，self 字段指向实际 GC 对象。
- **ViewRoot 是保护视图 self 的正确机制**：内部 `GcRootHandle<GcObject*>` 持 `it.self`（适配器指针，正确的 GC 指针），而非视图值本身。
- `GcRootHandle<视图值>` 是**错误**用法：ptr_ref_ 指向视图值起始，GC compact 覆写时破坏视图的函数指针字段。

### 3.5 State & Side Effects

- `variantStorageUnsafe` 拦截是 Sema 层编译期行为，放开后用户代码可生成 Variant 含接口变体代码。
- 放开拦截前必须先修复 CodeGen 装箱路径（缺口 1、2）和 match 路径（缺口 3），否则验收用例无法通过。
- promoteToOld 的 dynamicDesc 接入是独立 bug 修复，放开拦截后会暴露。

---

## 4. Proposed Changes

### 4.1 方案选择：dynamicDesc + 复合偏移（推荐，方案 A）

扩展 per-变体 desc 的 `ptrFieldOffsets`，直接包含复合偏移 `kStorageOffset + offsetof(T, self)`。复用现有 dynamicDesc + ptrFieldOffsets 机制，**不引入 isPtrActive 新钩子**。

理由：genRecordStruct 已用"field+ViewType"复合偏移解决 record 字段为接口视图的场景（[DeclGen.cpp:92-96](file:///d:/you/Aura/src/CodeGen/DeclGen.cpp#L92-L96) + [TypeMap.cpp:373-380](file:///d:/you/Aura/src/CodeGen/TypeMap.cpp#L373-L380)），Variant 变体为接口视图是同型问题，复用同一思路。

### 4.2 改动 A：variant.h 添加接口视图 trait + 扩展 descForI + include

**文件**：[variant.h](file:///d:/you/Aura/runtime/builtin/variant.h)

**新增 include**（文件头）：
```cpp
#include <type_traits>   // is_pointer_v, void_t, enable_if_t, is_convertible_v
```

**新增 trait**（检测 T 含 `GcObject* self` 字段，判定为接口视图）：
```cpp
template <typename T, typename = void>
struct is_iface_view : std::false_type {};
template <typename T>
struct is_iface_view<T, std::void_t<
    decltype(std::declval<T&>().self),
    std::enable_if_t<std::is_convertible_v<
        decltype(std::declval<T&>().self), GcObject*>>
>> : std::true_type {};
template <typename T>
inline constexpr bool is_iface_view_v = is_iface_view<T>::value;
```

**扩展 descForI**（L52-63 新增分支）：
```cpp
template <size_t I>
static const TypeDescriptor& descForI() {
    using T = std::tuple_element_t<I, std::tuple<Ts...>>;
    if constexpr (std::is_pointer_v<T>) {
        static const size_t offs[] = { kStorageOffset };
        static const TypeDescriptor d = { sizeof(Variant<Ts...>), 1, offs, 0, nullptr, nullptr };
        return d;
    } else if constexpr (is_iface_view_v<T>) {
        // 接口视图变体：storage_ 起始 + 视图内 self 子偏移 = GC 指针
        static const size_t offs[] = { kStorageOffset + offsetof(T, self) };
        static const TypeDescriptor d = { sizeof(Variant<Ts...>), 1, offs, 0, nullptr, nullptr };
        return d;
    } else {
        static const TypeDescriptor d = { sizeof(Variant<Ts...>), 0, nullptr, 0, nullptr, nullptr };
        return d;
    }
}
```

### 4.3 改动 B：放开 variantStorageUnsafe 对接口变体的拦截

**文件**：[DeclChecker.cpp:10-21](file:///d:/you/Aura/src/Sema/Checker/DeclChecker.cpp#L10-L21)

移除 L12 `if (dynamic_cast<const InterfaceSemType*>(&t)) return true;` 分支。

保留：L11 FuncSemType、L13 UnionSemType、L18-19 Iterator GenericSemType 拦截（后者待方案 A 验证后单独评估放开，见 §8 步骤 6）。

### 4.4 改动 C：修复 promoteToOld 接入 dynamicDesc 钩子（独立 bug）

**文件**：[mark_sweep.cpp:301](file:///d:/you/Aura/runtime/gc/mark_sweep.cpp#L301)

```cpp
const TypeDescriptor* desc = obj->desc;
if (desc && desc->dynamicDesc) desc = desc->dynamicDesc(obj);  // 新增
if (!desc) return;
```

### 4.5 改动 D：genUnionBoxingImpl 对接口视图变体改用 ViewRoot 包裹（修复缺口 1、2）

**文件**：[StmtGen.cpp:102-135](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L102-L135)

**缺口 1 修复**：对接口视图变体，不生成 `GcRootHandle<视图值>`（错误），改用 `ViewRoot<视图值>` 包裹，`make_variant` 取 `&vroot.get()`（指向视图值，memcpy 进 storage_）。

**缺口 2 修复**：当 init 是 RecordSemType 且目标变体是接口视图时，先做 record→适配器 view() 预转换（复用 genLetStmt §3.10 机制 L319-352），再装箱。

**改造逻辑**：

```cpp
std::string CodeGenerator::genUnionBoxingImpl(
    const std::vector<std::string>& cppTypes,
    const ASTNode& init, bool isCoroutine) {
    int idx = -1;
    if (init.inferredType) {
        std::string initCpp = mapSemType(*init.inferredType);
        for (size_t k = 0; k < cppTypes.size(); ++k) {
            if (cppTypes[k] == initCpp) { idx = static_cast<int>(k); break; }
        }
    }

    // 缺口 2 修复：init 是 record，目标变体是接口视图 → record→适配器 view() 预转换
    // 复用 genLetStmt §3.10 机制：IIFE 内 GcRootHandle<Record*> 保护 + XStringer::view
    bool needAdapter = false;
    std::string viewCppType;
    if (idx < 0 && init.inferredType) {
        // idx 匹配失败：检查 init 是 record 且某变体是接口视图
        if (auto* rs = dynamic_cast<const RecordSemType*>(init.inferredType)) {
            for (size_t k = 0; k < cppTypes.size(); ++k) {
                if (isIfaceViewTypeName(cppTypes[k])) {
                    // 生成 record→适配器 view() 预转换，返回视图值
                    viewCppType = cppTypes[k];
                    idx = static_cast<int>(k);
                    needAdapter = true;
                    break;
                }
            }
        }
    }
    if (idx < 0) return "";  // 仍未匹配

    std::string expr;
    bool isIfaceViewVariant = isIfaceViewTypeName(cppTypes[idx]);

    if (needAdapter) {
        // 缺口 2：record→适配器 view() 预转换（复用 genLetStmt §3.10 IIFE）
        expr = genRecordToViewIIFE(init, viewCppType, isCoroutine);
    } else {
        expr = genExpr(init, isCoroutine);
    }

    int bid = unionBoxingCounter_++;
    bool initIsHeap = isHeapSemType(init.inferredType);
    std::ostringstream oss;
    oss << "[&]() -> auto {\n";
    oss << "    auto _bx" << bid << " = (" << expr << ");\n";

    if (isIfaceViewVariant) {
        // 缺口 1 修复：接口视图变体用 ViewRoot 包裹（保护 self，不破坏视图）
        // ViewRoot 内部 GcRootHandle<GcObject*> 持 _bx.self（正确的 GC 指针）
        oss << "    aura_rt::ViewRoot<decltype(_bx" << bid << ")> _vr" << bid
            << "(_bx" << bid << ", aura_rt::GcRootScope::ThreadLocal);\n";
        oss << "    return aura_rt::make_variant<";
        for (size_t k = 0; k < cppTypes.size(); ++k) {
            if (k > 0) oss << ", ";
            oss << cppTypes[k];
        }
        oss << ">(" << idx << ", &_vr" << bid << ".get());\n";
    } else if (initIsHeap) {
        // 原路径：普通堆值用 GcRootHandle<T> 包裹
        oss << "    aura_rt::GcRootHandle<decltype(_bx" << bid << ")> _bhx"
            << bid << "(_bx" << bid << ");\n";
        oss << "    return aura_rt::make_variant<";
        for (size_t k = 0; k < cppTypes.size(); ++k) {
            if (k > 0) oss << ", ";
            oss << cppTypes[k];
        }
        oss << ">(" << idx << ", &_bhx" << bid << ".get());\n";
    } else {
        // 值/指针裸值：直接取地址
        oss << "    return aura_rt::make_variant<";
        for (size_t k = 0; k < cppTypes.size(); ++k) {
            if (k > 0) oss << ", ";
            oss << cppTypes[k];
        }
        oss << ">(" << idx << ", &_bx" << bid << ");\n";
    }
    oss << "  }()";
    return oss.str();
}
```

**新增辅助函数 genRecordToViewIIFE**（复用 genLetStmt §3.10 逻辑，提取为公共方法）：

```cpp
// record→适配器 view() 预转换 IIFE（复用 genLetStmt §3.10 机制）
// 生成：[&]() -> auto {
//   MyImpl* _ar = (expr);
//   aura_rt::GcRootHandle<MyImpl*> _ah(_ar);
//   auto* _ad = aura_rt::gcConstruct<MyImplStringer>(&MyImplStringer::desc(), _ah.get());
//   return MyImplStringer::view(_ad);
// }()
std::string CodeGenerator::genRecordToViewIIFE(const ASTNode& init,
                                                 const std::string& viewCppType,
                                                 bool isCoroutine);
```

> 实现时从 [StmtGen.cpp:319-352](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L319-L352) 提取逻辑为独立方法，供 genLetStmt 和 genUnionBoxingImpl 共用。
>
> **接口基名提取（实现要点）**：`genRecordToViewIIFE` 只接收 `viewCppType` 字符串（如 `"Stringer"` 或 `"Comparable<Point*>"`），而适配器名 = `safeName(recordName) + iface.name`（genLetStmt §3.10 直接用 NamedType 的 `nt->name`，无需提取）。提取逻辑：
> ```cpp
> // viewCppType → 接口基名（去模板参数）
> std::string ifaceBaseName = viewCppType;
> size_t lt = ifaceBaseName.find('<');
> if (lt != std::string::npos) ifaceBaseName = ifaceBaseName.substr(0, lt);
> // record 名从 init.inferredType(RecordSemType) 的 canonicalName 提取（去 '<' 模板参数）
> std::string recName = ...;  // canonicalName 去掉 "<...>"
> std::string adapterName = safeName(recName) + ifaceBaseName;  // 如 Point + Comparable
> ```

### 4.6 改动 F（前置）：ViewRoot 构造扩展

**文件**：[iterator.h:72-81](file:///d:/you/Aura/runtime/builtin/iterator.h#L72-L81)

**现状**（单参构造，scope 硬编码 ThreadLocal）：
```cpp
explicit ViewRoot(T it) : v(it), h(it.self, GcRootScope::ThreadLocal) {}
```

**改造**（构造加默认参，两参构造可供改动 D/E 使用）：
```cpp
// 栈上使用：scope 默认 ThreadLocal（无锁）；显式传 GcRootScope 供装箱/match 分支等场景
explicit ViewRoot(T it, GcRootScope scope = GcRootScope::ThreadLocal)
    : v(it), h(it.self, scope) {}
```

> ⚠️ **共享改动**：此改动与 [plan/threadRootLists节点compact重定位.md](file:///d:/you/Aura/plan/threadRootLists节点compact重定位.md) 改动 A 的构造 1 完全相同。两个 plan 合并实施时**只做一次**，改动 D（§4.5 `ViewRoot<T>(_bxN, GcRootScope::ThreadLocal)`）与改动 E（§4.7 `ViewRoot<T>(raw, GcRootScope::ThreadLocal)`）的两参用法均依赖此项。

### 4.7 改动 E：genMatchStmt 视图分支 self 保护（修复缺口 3）

**文件**：[StmtGen.cpp:1686-1698](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L1686-L1698)

**现状**：生成 `auto& s = _match_val->get<idx>();`，s 是视图值引用，self 裸指针无 GC 保护。

**修复**：对接口视图变体分支，生成 ViewRoot 包裹分支绑定值，分支体内通过 `vroot.get()` 访问。

```cpp
if (isVariantPtr && !tp->varName.empty()) {
    std::string varName = safeName(tp->varName);
    if (isIfaceViewTypeName(varCppType)) {
        // 缺口 3 修复：接口视图变体分支用 ViewRoot 包裹
        // _match_val 是 Variant*，get<idx>() 返回视图值引用
        // ViewRoot 持 self（适配器指针），分支体 alloc 触发 GC 时 self 由 GcRootHandle 更新
        binding = "auto " + varName + "_raw = _match_val->get<" + std::to_string(idx) + ">();"
                + " aura_rt::ViewRoot<" + varCppType + "> " + varName + "("
                + varName + "_raw, aura_rt::GcRootScope::ThreadLocal);";
        // 分支体内访问 varName 走 .get()（需在 viewRootVarNames_ 注册）
    } else {
        binding = "auto& " + varName + " = _match_val->get<" + std::to_string(idx) + ">();";
    }
}
```

**配套**：分支体内对该 varName 的访问需走 `viewRootVarNames_.get()` 路径（类似 genIdentifier L248-252）。实现时需在 genMatchStmt 进入分支体前临时注册 `viewRootVarNames_.insert(varName)`，分支体结束后移除。

### 4.8 不需要改动的部分

- **markFields / updateObjectFields / markInlineArrayFields**：已接 dynamicDesc ✓
- **genRecordStruct "field+ViewType"**：record 字段为接口视图已支持 ✓
- **TypeDescriptor 结构体**：方案 A 不需要新增 isPtrActive 钩子字段 ✓
- **compact.cpp**：零改动，复用 updateObjectFields 的 dynamicDesc 钩子 ✓

---

## 5. Impact Analysis

| 影响面 | 说明 |
| ---- | ---- |
| `runtime/builtin/variant.h` | 新增 is_iface_view trait + descForI 接口视图分支 + `#include <type_traits>` |
| `runtime/gc/mark_sweep.cpp` | promoteToOld 接入 dynamicDesc 钩子（1 行） |
| `src/Sema/Checker/DeclChecker.cpp` | 移除 InterfaceSemType 拦截分支（1 行） |
| `src/CodeGen/StmtGen.cpp` | genUnionBoxingImpl 改造（接口视图分支 + record→view 预转换）+ 新增 genRecordToViewIIFE + genMatchStmt 视图分支 ViewRoot 包裹 |
| `src/CodeGen/CodeGen.h` | 声明 genRecordToViewIIFE 方法 |
| ⚠️ BREAKING | 无：纯功能扩展，放开拦截后用户代码可使用新语法；既有非接口变体 Variant 行为不变 |
| 行为变化 | `Variant<Stringer, int>` 等含接口变体联合可编译运行；GC 正确扫描/更新 storage_ 内 self 指针；match 分支内 self 受保护 |
| 性能 | trait 编译期求值，零运行时开销；promoteToOld 多一次 dynamicDesc 调用（仅 Variant 对象）；装箱多一次 ViewRoot 构造（仅接口视图变体） |

### 升级/回滚兼容

- 独立可交付；回滚 = `git restore` 对应文件。
- 不影响 mark-sweep、compact 对非 Variant 对象的处理。

---

## 6. Boundary Condition Handling Strategy

| 边界条件 | 现状处理 | 计划处理 | 测试策略 |
| ---- | ---- | ---- | ---- |
| Variant 变体为普通指针（GcString*） | descForI 指针分支 ✓ | 不变 | 既有用例回归 |
| Variant 变体为接口视图（Stringer） | 拦截 ✗ | descForI 接口视图分支 + 装箱 ViewRoot + match ViewRoot | 新增用例 + ASAN |
| Variant 变体为 POD 值（int） | descForI 值分支 ✓ | 不变 | 既有用例回归 |
| let x: Stringer\|int = myImplRecord | idx 匹配失败 ✗ | record→适配器 view() 预转换 + 装箱 ViewRoot | 新增用例 |
| match 分支内视图 self | 裸指针悬垂 ✗ | ViewRoot 包裹分支绑定值 | 新增用例 + ASAN |
| trait 误判（非接口视图含 self 字段） | — | trait 要求 self 可转换为 GcObject*；Aura 类型系统中仅接口视图含此字段 | 代码审查 |
| Variant 晋升时 storage_ 含 young 引用 | promoteToOld 漏扫（bug） | 接入 dynamicDesc 钩子 | 新增晋升用例 + ASAN |
| compact 期间 Variant 被搬运 | updateObjectFields 已接 dynamicDesc ✓ | 不变 | ASAN 压力测试 |
| Variant 变体视图 self 为 nullptr | markFields 跳过 nullptr ✓ | 不变 | 空视图用例 |
| 多个接口视图变体 | per-变体 desc 各自计算 offsetof ✓ | 每个变体独立 descForI | 多变体用例 |

---

## 7. Test Plan

### 7.1 单元/集成

1. **新增用例：Variant 含用户接口（直接视图值）**
   ```
   type Stringer = { fn: (self) -> string }
   type MyImpl = { s: string }
   impl Stringer for MyImpl { fn = (self) => self.s }
   
   let x: Stringer | int = MyImpl { s = "hello" }  # record→适配器 view() 预转换
   force_gc()
   force_gc()
   match x {
     s: Stringer => assert_eq(s.fn(), "hello")
     i: int => fail()
   }
   ```

2. **新增用例：Variant 含用户接口（已构造的视图值）**
   ```
   let sv: Stringer = MyImpl { s = "world" }  # genLetStmt §3.10 生成视图值
   let x: Stringer | int = sv  # genUnionBoxingImpl 接口视图分支
   force_gc()
   match x {
     s: Stringer => assert_eq(s.fn(), "world")
     i: int => fail()
   }
   ```

3. **新增用例：match 分支内 GC 压力**
   ```
   let x: Stringer | int = MyImpl { s = "stress" }
   match x {
     s: Stringer => {
       force_gc()  # 分支体内触发 GC
       force_gc()
       assert_eq(s.fn(), "stress")  # self 仍有效
     }
     i: int => fail()
   }
   ```

4. **新增用例：Variant 含 Iterator<T>**（步骤 6 放开后）
   ```
   let it: Iterator<int> | int = range(0, 10)
   force_gc()
   match it {
     i: Iterator<int> => assert_eq(collect_all(i).length, 10)
     n: int => fail()
   }
   ```

5. **promoteToOld 回归**：构造 Variant 对象 + young 子对象，触发 minor GC 晋升，验证 rememberedSet_ 正确记录。

6. **compact 回归**：Variant 含接口变体 + 大量分配触发 compact，验证 self 指针更新正确。

7. **全量回归**：`compile.cmd` + test.aura（含既有 `T | None`、`Array<int> | string` 等用例）。

### 7.2 深度检测

按 AGENTS.md：清空 build 重建 ASAN 版 → 编译新增用例 → 运行捕获 stderr → 0 报告后切回普通模式。

### 7.3 回归风险区

- promoteToOld dynamicDesc 接入：所有含 dynamicDesc 的对象晋升路径（当前仅 Variant）。
- genUnionBoxingImpl 改造：所有联合装箱路径（接口视图变体 + 普通堆值 + POD 值）。
- genMatchStmt 视图分支：所有 match TypePattern 路径。
- genRecordToViewIIFE 提取：genLetStmt §3.10 既有路径。

---

## 8. Implementation Steps（Ordered）

0. **扩展 ViewRoot 构造**（改动 F，前置）：iterator.h 构造加默认参 `GcRootScope scope = GcRootScope::ThreadLocal`。
   → 产物：runtime 编译通过。
   > ⚠️ 与 threadRootLists plan 改动 A 构造 1 相同（共享改动，只做一次）；改动 D 的 `ViewRoot<T>(_bxN, ThreadLocal)` 与改动 E 的 `ViewRoot<T>(raw, ThreadLocal)` 两参用法依赖此项。
1. **修复 promoteToOld**（改动 C）：mark_sweep.cpp 接入 dynamicDesc 钩子。
   → 产物：runtime 编译通过 + 既有 Variant 用例回归不退步。
2. **扩展 Variant**（改动 A）：variant.h 添加 `#include <type_traits>` + is_iface_view trait + descForI 接口视图分支。
   → 产物：runtime 编译通过。
3. **提取 genRecordToViewIIFE**（改动 D 前置）：从 genLetStmt §3.10 提取逻辑为独立方法（含接口基名提取，见 §4.5）。
   → 产物：编译通过，genLetStmt 既有视图路径不退步。
4. **改造 genUnionBoxingImpl**（改动 D）：接口视图变体 ViewRoot 包裹 + record→view 预转换。
   → 产物：编译通过，`Variant<Stringer, int>` 装箱代码正确生成。
5. **放开用户接口拦截**（改动 B）：DeclChecker.cpp 移除 InterfaceSemType 分支。
   → 产物：`Variant<Stringer, int>` 可编译。
6. **改造 genMatchStmt**（改动 E）：视图分支 ViewRoot 包裹。
   → 产物：match 分支内 self 受保护。
7. **新增用例 + 回归**：步骤 1-3 用例 + 全量回归。
8. **评估放开 Iterator 拦截**：移除 DeclChecker.cpp L18-19 的 GenericSemType "Iterator" 拦截，跑步骤 4 用例。若通过则放开，否则保留拦截记入 TODO。
9. **ASAN 深度检测**。
10. **完成**：TODO.txt [二] P2 标记 `[x]`。

**回滚**：每步均 `git restore` 对应文件即可，独立可回滚。

**依赖关系**：
- 步骤 0 必须先于步骤 4、6（ViewRoot 两参构造前置）。
- 步骤 3 必须先于步骤 4（genUnionBoxingImpl 依赖 genRecordToViewIIFE）。
- 步骤 2、4 必须先于步骤 5（放开拦截前装箱路径必须正确）。
- 步骤 5 必须先于步骤 7（测试需要放开拦截才能编译用例）。
- 步骤 6 可与步骤 4 并行（独立改造点）。

---

## 9. Risks & Mitigations

| 风险 | 缓解 |
| ---- | ---- |
| trait 误判非接口视图类型为接口视图 | 要求 self 可转换为 GcObject*；Aura 类型系统中仅接口视图含此字段；static_assert 校验 offsetof 合法性 |
| offsetof(T, self) 在非标准布局类型上未定义行为 | 接口视图是 C++ 标准布局 struct（CodeGen 生成，含函数指针 + GcObject*），offsetof 合法 |
| genRecordToViewIIFE 提取引入回归 | 步骤 3 独立验证 genLetStmt 既有视图路径不退步后再进入步骤 4 |
| genMatchStmt ViewRoot 包裹改变分支体变量访问语义 | 分支体内访问需走 viewRootVarNames_.get() 路径；临时注册 + 分支体结束移除 |
| ViewRoot 两参构造依赖 | 改动 D/E 用 `ViewRoot<T>(it, GcRootScope::ThreadLocal)` 两参构造。前置步骤 0（改动 F，与 threadRootLists plan 改动 A 构造 1 共享）扩展构造加默认参；实施顺序保证步骤 0 先于步骤 4/6 |
| 放开 Iterator 拦截后暴露未知 bug | 步骤 8 单独评估，保留拦截为退路 |
| 接口视图变体的 self 指向栈上临时对象 | 适配器工厂函数保证 self 指向 GC 堆对象；make_variant memcpy 视图值时 self 已指向堆对象 |

---

## 10. 已知限制（本次不修复）

1. **Variant 变体为含多个 GC 指针的 record —— 安全，无需多 offset（第三轮审查确认）**：record 变体经 [TypeMap.cpp L302-307](file:///d:/you/Aura/src/CodeGen/TypeMap.cpp#L302-L307) 映射为 `RecordName*`（**指针变体**），descForI 走 `std::is_pointer_v` 分支（单 offset=kStorageOffset），GC 扫描 storage_ 处的 `Record*` 指针后**递归扫描 Record 对象自身**（genRecordStruct 生成的 _desc 描述所有 GC 指针字段）→ 多个 GC 指针字段天然安全；[DeclChecker.cpp L10-21](file:///d:/you/Aura/src/Sema/Checker/DeclChecker.cpp#L10-L21) 也不拦截 RecordSemType。早期草案"record 值嵌入 storage_ 需多 offset / Sema 拦截"为错误假设，**已删除**。
2. **Variant 变体为 Optional<T>**：Sema 层 `T | None` 折叠为 `Optional<T>`，不走 Variant。无问题。
3. **Variant 嵌套 Variant**：被 variantStorageUnsafe L13 拦截（UnionSemType），保持拦截。
4. **match 分支体内直接访问视图值字段（非 self）**：ViewRoot::get() 返回视图值副本，函数指针字段在 compact 后仍有效。**前提：分支体内不再访问 `_match_val`**（[StmtGen.cpp L1615](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp#L1615) `auto&& _match_val = expr;` 无 GC 保护，是独立问题，不在本 plan 范围）。此前提由改动 E 的绑定代码保证（binding 拷贝视图值到 ViewRoot，分支体仅用绑定变量）。
5. **tryAllocSlow 路径未 memset（独立真实 bug，记入 TODO）**：早期草案"make_variant alloc 窗口 index_=0 扫描 storage_ 垃圾"的描述在 TLAB 快路径下不成立——[alloc.cpp L81](file:///d:/you/Aura/runtime/gc/alloc.cpp#L81) 已 memset 清零 storage_（GC 扫 nullptr 跳过），且 [variant.h L97-103](file:///d:/you/Aura/runtime/builtin/variant.h#L97-L103) 的 make_variant 仅 alloc + 写 index_ + memcpy、不触发 safepoint，窗口不存在。**真实缺陷**：[alloc.cpp tryAllocSlow L120-174](file:///d:/you/Aura/runtime/gc/alloc.cpp#L120-L174) 未 memset，GC sweep 后旧页被复用（rebuildPageList 保留含存活对象的页，bumpOffset 后方空间 GC 前被分配过对象）时 bump 出含旧对象残留的内存，指针字段读脏数据。影响所有经 tryAllocSlow 分配的对象（首次分配 / TLAB 满 / compact 后 / gcPending），不限于 Variant。**记入 TODO 单独修复**（与 TLAB 对齐补 memset），本 plan 不涉及。
