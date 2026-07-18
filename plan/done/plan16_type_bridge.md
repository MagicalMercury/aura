# Plan 16 (v2): 根本性重构 — SemType 嵌入 ASTNode

> 状态：待实现  
> 目标：废除 CodeGen 中所有文本启发式类型推断，改为读取 SemAnalyzer 已验证的精确类型  
> 策略：`SemType*` 作为 AST 节点一级属性存储 + `RecordSemType.canonicalName` 解决类型名映射

---

## 一、动机

### 1.1 现状问题

SemAnalyzer 在分析阶段已完成全部类型推断，但推断结果被丢弃——没有写入 AST，CodeGen 无法访问。CodeGen 被迫在生成 C++ 时重新猜测类型：

| CodeGen 位置 | 当前做法 | 问题 |
|-------------|---------|------|
| `genListExpr` | 文本匹配 `first.find("make_string")`、`first.find('.')` 猜测元素类型 | `{.value=2}` 含 `.` 被误判为 `double` |
| `genLetStmt` (无标注) | `type = "auto"` | `let tree = {...}` → `auto tree = {.value=1, ...}` → `initializer_list<auto>` 冲突 |
| `genRecordExpr` | 永远生成 `{.field = val}` 指定初始化器 | 对 GC 堆对象需 `gc_alloc` 而非字面量 |

### 1.2 根本原因

`SemAnalyzer::resolveType(NamedType)` **忽略了** `typeArgs`，所有类型引用都是泛型版本（`Tree<T>` 而非 `Tree<int>`），导致 `SemType` 不携带类型名信息。

```
AST: NamedType { name="Tree", typeArgs=[NamedType("int")] }
  → resolveType() → resolveNamedType("Tree")
    → sym->type->clone() → RecordSemType {fields: [{T: GenericSemType}, {[Tree<T>]: ListSemType}]}
    // ❌ typeArgs 被丢弃！
```

---

## 二、重构设计

### 2.1 核心改动：ASTNode 内嵌 SemType 指针

```cpp
// AST/ASTNode.h
#include "../Sema/SemType.h"   // 前向声明 → 完整 include

struct ASTNode {
    virtual ~ASTNode() = default;
    virtual void print(std::ostream& os, int indent = 0) const = 0;
    [[nodiscard]] virtual std::unique_ptr<ASTNode> clone() const = 0;
    int line = 0;
    int col  = 0;

    // 类型标注：SemAnalyzer 在分析阶段设置，CodeGen 在翻译阶段读取
    // 所有权归 SemAnalyzer，CodeGen 只读访问
    // clone() 自动重置为 nullptr（新构造的对象默认初始化）
    const SemType* inferredType = nullptr;
};
```

#### 为什么用裸指针而非 unique_ptr？

- **所有权**：`SemType` 的所有者仍是 `SemAnalyzer`（栈变量，在 SemAnalyzer 析构时释放）
- **生命周期**：`main.cpp` 中 `SemAnalyzer sema` → `sema.analyze(program)` → `CodeGenerator cg` → `cg.generate(program)` → `sema` 析构。CodeGen 全程在 `sema` 存活期内运行，裸指针安全
- **clone() 行为**：`inferredType = nullptr` 作为默认值，所有 clone() 无需改动——新对象自动得到 `nullptr`

### 2.2 核心改动：RecordSemType 添加 canonicalName

```cpp
// Sema/SemType.h
struct RecordSemType : SemType {
    std::vector<RecordFieldSem> fields;
    std::string canonicalName;  // ← NEW: e.g. "Tree" (不含模板参数)
    // clone() 自动复制 canonicalName
};
```

`canonicalName` 在 `DeclChecker::declareDecl` 中设置：

```cpp
// DeclChecker.cpp
if (t->type) {
    auto resolved = resolveType(*t->type);
    if (auto* rec = dynamic_cast<RecordSemType*>(resolved.get()))
        rec->canonicalName = t->name;  // "Tree"
    auto* existing = symtab_.lookupGlobal(t->name);
    if (existing) existing->type = std::move(resolved);
}
```

### 2.3 核心改动：resolveType 使用 typeArgs

```cpp
// SemAnalyzer.cpp — resolveType
if (auto* n = dynamic_cast<const NamedType*>(&astType)) {
    auto result = resolveNamedType(n->name);
    // typeArgs 修正：如 Tree<int> → 将 RecordSemType 中 GenericSemType 替换
    if (!n->typeArgs.empty()) {
        if (auto* rec = dynamic_cast<RecordSemType*>(result.get())) {
            auto* sym = symtab_.lookup(n->name);
            if (sym && sym->kind == SymKind::TypeAlias) {
                for (size_t i = 0; i < n->typeArgs.size() && i < sym->typeParams.size(); ++i) {
                    result = substitute(*result, sym->typeParams[i],
                                        resolveType(*n->typeArgs[i]));
                }
            }
        }
    }
    return result;
}
```

这样 `Tree<int>` → `RecordSemType {canonicalName: "Tree", fields: [{int}, {[Tree<int>]}]}`，CodeGen 可直接用 `canonicalName` 获取类型名。

### 2.4 新增：SemType → C++ 映射

```cpp
// CodeGen/TypeMap.cpp
std::string CodeGenerator::mapSemType(const SemType& semType) {
    if (auto* p = dynamic_cast<const PrimSemType*>(&semType)) {
        switch (p->kind) {
            case PrimSemType::Int:    return "int32_t";
            case PrimSemType::Float:  return "double";
            case PrimSemType::Bool:   return "bool";
            case PrimSemType::String: return "aura_rt::GcString*";
        }
    }
    if (dynamic_cast<const NoneSemType*>(&semType))
        return "aura_rt::NoneType";
    if (auto* l = dynamic_cast<const ListSemType*>(&semType)) {
        return "aura_rt::Array<" + mapSemType(*l->elementType) + ">*";
    }
    if (auto* r = dynamic_cast<const RecordSemType*>(&semType)) {
        if (!r->canonicalName.empty()) {
            return r->canonicalName + "*";  // "Tree" → "Tree*"
        }
        return "aura_rt::GcObject*";
    }
    if (auto* f = dynamic_cast<const FuncSemType*>(&semType)) {
        // 保持与 mapType(FunctionType) 一致
        std::string sig = "std::function<";
        sig += f->returnType ? mapSemType(*f->returnType) : "void";
        sig += "(";
        for (size_t i = 0; i < f->paramTypes.size(); ++i) {
            if (i > 0) sig += ", ";
            sig += f->paramTypes[i] ? mapSemType(*f->paramTypes[i]) : "auto";
        }
        sig += ")>";
        return sig;
    }
    if (dynamic_cast<const GenericSemType*>(&semType))
        return "auto";
    return "/* unknown */";
}
```

### 2.5 代码生成改造：废除文本启发式

#### genLetStmt

```diff
 void CodeGenerator::genLetStmt(std::ostream& cpp, const LetDecl& decl) {
-    std::string type = decl.type ? mapType(*decl.type) : "auto";
+    std::string type;
+    if (decl.type) {
+        type = mapType(*decl.type);
+    } else if (decl.inferredType) {
+        type = mapSemType(*decl.inferredType);
+    } else {
+        type = "auto";
+    }

     // RecordExpr 作为 let 初始值
     if (auto* rec = dynamic_cast<const RecordExpr*>(decl.initializer.get())) {
-        if (decl.type) {                     // ← 旧条件：需要 decl.type
+        if (!type.empty() && type != "auto") { // ← 新条件：有类型即可
             std::string recType = mapType(*decl.type);
+            // 或者用 type 本身（已经是 mapSemType 的结果）
             ...
         }
+        // NEW: 即使没有 decl.type，decl.inferredType 也已给出类型
+        if (isHeapRecord && recType != "auto") {
+            gc_alloc + assign path (无需 decl.type)
+        }
     }
 }
```

#### genListExpr

```diff
 std::string CodeGenerator::genListExpr(const ListExpr& e, bool isCoroutine) {
     ...
-    // 文本启发式
-    std::string elemType = "int32_t";
-    if (e.elements[0] && dynamic_cast<FunExpr*>...) { ... }
+    // 优先用 SemType
+    std::string elemType;
+    if (e.inferredType) {
+        auto* listTy = dynamic_cast<const ListSemType*>(e.inferredType);
+        if (listTy && listTy->elementType)
+            elemType = mapSemType(*listTy->elementType);
+    }
+    if (elemType.empty()) {
+        // fallback：现有启发式（SemType 不可用时）
+        elemType = "int32_t"; ...
+    }
     ...
 }
```

#### genReturnStmt

```diff
 void CodeGenerator::genReturnStmt(...) {
     if (stmt.expr && dynamic_cast<const RecordExpr*>(stmt.expr.get())) {
-        std::string recType = currentReturnCppType_;
+        // 优先用 SemType
+        std::string recType;
+        if (stmt.expr->inferredType) {
+            recType = mapSemType(*stmt.expr->inferredType);
+        } else {
+            recType = currentReturnCppType_;
+        }
         ...
     }
 }
```

### 2.6 SemAnalyzer 标注 AST 节点

在 SemAnalyzer 的 `checkLetDecl`、`checkReturnStmt`、`inferExpr` 各分支中，将推断出的 `SemType` 指针设置到 AST 节点：

```cpp
// StmtChecker.cpp — checkLetDecl
void SemAnalyzer::checkLetDecl(const LetDecl& decl) {
    auto inferredType = decl.initializer ? inferExpr(*decl.initializer) : ErrorSemType::make();
    // ...
    // 标注 AST 节点（const_cast：逻辑上我们在分析阶段写入只读 AST）
    const_cast<LetDecl&>(decl).inferredType = sym->type.get();
}

// StmtChecker.cpp — checkReturnStmt
void SemAnalyzer::checkReturnStmt(const ReturnStmt& stmt) {
    if (stmt.expr) {
        auto retType = inferExpr(*stmt.expr);
        const_cast<ReturnStmt&>(stmt).inferredType = retType.get();  // 只在 expr 有类型时标注
        // ...
    }
}

// ExprInfer.cpp — inferListExpr
std::unique_ptr<SemType> SemAnalyzer::inferListExpr(const ListExpr& e) {
    auto t = std::make_unique<ListSemType>();
    // ...
    const_cast<ListExpr&>(e).inferredType = t.get();  // 暂存到列表临时，最终存到 owning type
    return t;
}
```

**注意**：`inferExpr` 返回 `unique_ptr<SemType>`，其所有权在调用方（如 `checkLetDecl` 中的 `inferredType` 局部变量）。AST 节点上的 `inferredType` 始终指向调用方的 `unique_ptr` 所管理的对象。这形成一条所有权链：

```
SemAnalyzer::checkLetDecl:
  auto inferredType = inferExpr(...);  // owns the SemType
  const_cast<LetDecl&>(decl).inferredType = sym->type.get();
                                         // ↑ points into symtab_ (stable)
  // inferredType 析构时：如果被 symtab_ 持有，则无需额外管理
```

**更好的所有权模型**：所有 `inferredType` 指针都指向 `symtab_` 中的 `Symbol::type`，因为分析结束后类型已存入符号表。`Expr` 节点的 `inferredType` 直接指向 `Symbol::type`：

```cpp
// ExprInfer.cpp — 存储 inferredType 到 AST 节点的正确方式
// 不在 inferExpr 中存储（类型引用不稳定）
// 在 checkLetDecl / checkConstDecl 中存储（类型已存入符号表）
auto* sym = symtab_.lookup(decl.name);
if (sym) const_cast<LetDecl&>(decl).inferredType = sym->type.get();
```

对于无符号表的表达式（如列表元素、Return 表达式），采用一个 `SemAnalyzer` 内部的 `typeStore_` 来持有 `SemType`：

```cpp
// SemAnalyzer.h
std::vector<std::unique_ptr<SemType>> typeStore_;  // 持有表达式推断结果的临时类型

// ExprInfer.cpp
auto t = std::make_unique<ListSemType>();
t->elementType = std::move(elemType);
const_cast<ListExpr&>(e).inferredType = t.get();
typeStore_.push_back(std::move(t));  // 转移所有权给 SemAnalyzer
return /* typeStore_ 中最后一项的引用 */ ...;
```

---

## 三、改动清单

| # | 文件 | 改动 | 行数 |
|---|------|------|:---:|
| 1 | `AST/ASTNode.h` | + `#include "../Sema/SemType.h"` + `const SemType* inferredType = nullptr` | +3 |
| 2 | `Sema/SemType.h` | `RecordSemType` + `std::string canonicalName` | +1 |
| 3 | `Sema/SemType.cpp` | `RecordSemType::clone()` 复制 `canonicalName` | +1 |
| 4 | `Sema/SemAnalyzer.h` | + `typeStore_` 成员 | +1 |
| 5 | `Sema/Checker/DeclChecker.cpp` | `declareDecl` + `rec->canonicalName = t->name` | +2 |
| 6 | `Sema/SemAnalyzer.cpp` | `resolveType(NamedType)` 使用 `n->typeArgs` 做代换 | +10 |
| 7 | `Sema/Checker/StmtChecker.cpp` | `checkLetDecl` / `checkConstDecl` / `checkReturnStmt` 设置 `inferredType` | +8 |
| 8 | `Sema/Checker/ExprInfer.cpp` | `inferListExpr` / `inferRecordExpr` / `inferFunExpr` 存储类型到 `typeStore_` + 标注 AST | +12 |
| 9 | `CodeGen/CodeGen.h` | —（无需改动，通过 `node.inferredType` 访问） | 0 |
| 10 | `CodeGen/TypeMap.cpp` | + `mapSemType()` 方法 | +35 |
| 11 | `CodeGen/CodeGen.h` | + `mapSemType()` 声明 | +1 |
| 12 | `CodeGen/StmtGen.cpp` | `genLetStmt` 从 `decl.inferredType` 取类型 | +8 |
| 13 | `CodeGen/ExprGen.cpp` | `genListExpr` 从 `e.inferredType` 取元素类型 | +8 |
| 14 | `CodeGen/ExprGen.cpp` | `genReturnStmt` 从 `stmt.expr->inferredType` 取返回类型 | +5 |
| 15 | `main.cpp` | —（无需改动，新旧通过 AST 自动传递） | 0 |

**总计：14 个文件，~95 行有效改动。**

---

## 四、风险分析

| 风险 | 可能性 | 影响 | 应对 |
|------|--------|------|------|
| ASTNode.h 循环依赖 `SemType.h` | 低 | 编译失败 | SemType.h 不依赖任何 AST 头文件，引用安全 |
| `typeStore_` 中指针因 vector reallocation 失效 | 低 | 悬挂指针 | `typeStore_` 预分配足够容量，或改用 `std::list` |
| `n->typeArgs` 代换后 clone 丢失字段 | 中 | 类型不准确 | 仔细实现 `substitute`，确保 `canonicalName` 传播 |
| `inferredType` 在 Parser 阶段被访问 | 无 | — | Parser 不访问此字段 |
| 多文件模式下 SemAnalyzer 生命周期 | 低 | 悬挂指针 | `ModuleManager` 中 `parseModule` 后 AST 常驻，每个模块独立 SemAnalyzer 栈变量 |

---

## 五、实施阶段

### Phase 1：铺设类型通道（无行为变更）

**文件**：1, 2, 3, 4, 5

1. `ASTNode.h`：添加 `const SemType* inferredType = nullptr`
2. `SemType.h`：`RecordSemType` 添加 `std::string canonicalName`
3. `SemType.cpp`：`clone()` 复制 `canonicalName`
4. `SemAnalyzer.h`：添加 `typeStore_`
5. `DeclChecker.cpp`：设置 `canonicalName`

**验证**：编译通过，运行时行为不变

### Phase 2：SemAnalyzer 标注 AST

**文件**：6, 7, 8

1. `SemAnalyzer.cpp`：`resolveType(NamedType)` 使用 typeArgs 代换
2. `StmtChecker.cpp`：`checkLetDecl` / `checkConstDecl` / `checkReturnStmt` 设置 `inferredType`
3. `ExprInfer.cpp`：存储推断类型到 `typeStore_` + 标注 AST 节点

**验证**：编译通过，SemType 标注到位（可通过 ASTPrinter 验证）

### Phase 3：CodeGen 读取 SemType

**文件**：10, 11, 12, 13, 14

1. `TypeMap.cpp`：实现 `mapSemType()`
2. `StmtGen.cpp`：改造 `genLetStmt`
3. `ExprGen.cpp`：改造 `genListExpr`、`genReturnStmt`

**验证**：翻译 `test.aura`，生成 C++ 无编译错误，程序运行正确

---

## 六、验证方案

1. `cmake --build build` — 编译器自身编译通过
2. `.\build\aurac.exe example\test.aura --cpp example\test_out.cpp`
3. 检查 `test_out.cpp` 关键片段：
   - `let tree = { value = 1, ... }` → 不再 `auto tree = {.value = 1, ...}`
   - List 元素类型 → `Array<Tree<int32_t>*>` 而非 `Array<int32_t>`
   - `return {value = ..., children = ...}` → gc_alloc 而非 `{.value = ..., .children = ...}`
4. g++ 编译 `test_out.cpp` + 链接 runtime
5. 运行 exe：全部测试通过

---

## 七、讨论

### 与 Plan15 的关系

Plan15（Bug 6 修复）解决了泛型高阶函数模板参数推导，操作在 `genFunExpr` 闭包生成端。Plan16 解决的是 CodeGen 类型推断的底层问题。两者正交：

- **Plan15**：SemType 未参与，纯粹靠闭包模板参数和 `invoke_result_t` 生成正确的 C++ lambda
- **Plan16**：为 CodeGen 提供 SemType 信息来源，让 CodeGen 可以废弃文本启发式

完成后 Bug 5 剩余问题全部自然解决。

### 为什么 `n->typeArgs` 代换可行？

`resolveType(NamedType("Tree", [int]))` → 先 clone Tree 的泛型 RecordSemType → 用 `substitute()` 将 `T` 替换为 `PrimSemType(Int)` → 得到具体化的 RecordSemType。`substitute` 已存在（用于泛型函数实例化），复用即可。

### `typeStore_` 的内存效率

`typeStore_` 在 SemAnalyzer 析构时一次性释放。即使遍历过程中连续 push_back，`unique_ptr<SemType>` 的移动语义确保不会有额外的深拷贝。vector 扩容时旧的 unique_ptr 被移动到新 buffer，不影响指针稳定性——但指针地址确实会变。替代方案：在 `checkLetDecl` 之前先把表达式类型存入 `typeStore_`，再设置 `inferredType` 指向 `typeStore_.back().get()`。
