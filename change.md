# change.md — ArrayView<T> 零拷贝视图（ViewSemType + 隐式深拷贝退化）

## 概述

实施 [plan/arrayview_design_conflict_issue.md](file:///d:/you/Aura/plan/arrayview_design_conflict_issue.md) 的详细方案：
- 新增 ViewSemType 绑定 owner 类型（与 owner 等价，Aura 层不暴露 View 类型）
- slice 返回 ViewSemType，view 调用修改方法时隐式深拷贝并永久退化为 owner 类型
- view 支持只读方法（len/[]/front/back/slice）和 for-in 迭代
- view[i] = val 触发深拷贝退化

---

## 变更 1：新增 ViewSemType

### 文件：src/Sema/SemType.h

**修改 1a：在 IterSemType 之后（L124 附近）新增 ViewSemType 声明**

```cpp
// 原（L124 附近）：
//      return "Iter<" + (elementType ? elementType->toString() : "?") + ">";
//  }
//  [[nodiscard]] std::unique_ptr<SemType> clone() const override;
//  static std::unique_ptr<IterSemType> make(std::unique_ptr<SemType> el) {
//      ...
//  }
//};
//
// ============================================================
// 工具函数
// ============================================================

// 改为（在 IterSemType 之后、工具函数之前新增）：
//      return "Iter<" + (elementType ? elementType->toString() : "?") + ">";
//  }
//  [[nodiscard]] std::unique_ptr<SemType> clone() const override;
//  static std::unique_ptr<IterSemType> make(std::unique_ptr<SemType> el) {
//      ...
//  }
//};

// 视图类型 — slice 返回值，绑定 owner 类型（与 owner 在 Aura 层等价）
// ViewSemType<ListSemType(int)> 在 Aura 层视为 [int]
// 调用修改方法时隐式深拷贝退化为 owner 类型
struct ViewSemType : SemType {
    std::unique_ptr<SemType> ownerType;  // 绑定的 owner 类型（ListSemType / PrimSemType(String) 等）
    [[nodiscard]] bool equals(const SemType& other) const override;
    [[nodiscard]] std::string toString() const override {
        return "View<" + (ownerType ? ownerType->toString() : "?") + ">";
    }
    [[nodiscard]] std::unique_ptr<SemType> clone() const override;
    static std::unique_ptr<ViewSemType> make(std::unique_ptr<SemType> owner) {
        auto v = std::make_unique<ViewSemType>();
        v->ownerType = std::move(owner);
        return v;
    }
};

// ============================================================
// 工具函数
// ============================================================
```

### 文件：src/Sema/SemType.cpp

**修改 1b：在 IterSemType 实现之后新增 ViewSemType 实现（文件末尾 `} // namespace Aura` 之前）**

```cpp
// ============================================================
// ViewSemType
// ============================================================
bool ViewSemType::equals(const SemType& other) const {
    // View<View<X>> == View<X>：比较 ownerType
    if (auto* v = dynamic_cast<const ViewSemType*>(&other)) {
        if (!ownerType || !v->ownerType) return !ownerType && !v->ownerType;
        return ownerType->equals(*v->ownerType);
    }
    // View<X> == X：与 owner 等价（Aura 层透明）
    if (ownerType) return ownerType->equals(other);
    return false;
}
std::unique_ptr<SemType> ViewSemType::clone() const {
    return make(ownerType ? ownerType->clone() : nullptr);
}
```

---

## 变更 2：ReturnTypeInfo 新增 View kind

### 文件：src/Sema/BuiltinRegistry.h

**修改 2a：ReturnTypeInfo 枚举新增 View（L41）**

```cpp
// 原（L41）：
enum class Kind { Named, Generic, None, Generator };

// 改为：
enum class Kind { Named, Generic, None, Generator, View };
```

**修改 2b：ReturnTypeInfo 新增 View 工厂（L49 附近）**

```cpp
// 原（L49）：
static ReturnTypeInfo Generator(const std::string& el){ return {Kind::Generator, el, 0}; }
};

// 改为：
static ReturnTypeInfo Generator(const std::string& el){ return {Kind::Generator, el, 0}; }
// View: 返回 ownerTypeName 的视图（slice 专用）
// typeName 存储 owner 类型名（如 "[T]"、"string"），Sema 根据调用方 objType 构造 ViewSemType
static ReturnTypeInfo View(const std::string& ownerTypeName) {
    return {Kind::View, ownerTypeName, 0};
}
};
```

---

## 变更 3：slice 方法注册改为 View 返回

### 文件：src/Sema/BuiltinRegistry.h

**修改 3a：string.slice 改为 View 返回（L239）**

```cpp
// 原（L239）：
{"string", "slice",  {{"start", "int"}, {"len", "int"}}, ReturnTypeInfo::Named("string")},

// 改为：
{"string", "slice",  {{"start", "int"}, {"len", "int"}}, ReturnTypeInfo::View("string")},
```

**修改 3b：[T].slice 改为 View 返回（L255）**

```cpp
// 原（L255）：
{"[T]", "slice",     {{"start", "int"}, {"len", "int"}}, ReturnTypeInfo::Generic(0, "[T]")},

// 改为：
{"[T]", "slice",     {{"start", "int"}, {"len", "int"}}, ReturnTypeInfo::View("[T]")},
```

---

## 变更 4：semTypeFromBuiltinReturn 处理 View kind

### 文件：src/Sema/SemAnalyzer.h

**修改 4a：semTypeFromBuiltinReturn 签名扩展（L67）**

```cpp
// 原（L67）：
[[nodiscard]] std::unique_ptr<SemType> semTypeFromBuiltinReturn(const ReturnTypeInfo& ret);

// 改为：
// objType: 调用方对象类型（Kind::View 时构造 ViewSemType(ownerType=objType)）
//          默认 nullptr 保证向后兼容
[[nodiscard]] std::unique_ptr<SemType> semTypeFromBuiltinReturn(
    const ReturnTypeInfo& ret, const SemType* objType = nullptr);
```

### 文件：src/Sema/SemAnalyzer.cpp

**修改 4b：semTypeFromBuiltinReturn 实现（L115-150）**

```cpp
// 原（L115-150）：
std::unique_ptr<SemType> SemAnalyzer::semTypeFromBuiltinReturn(const ReturnTypeInfo& ret) {
    switch (ret.kind) {
        case ReturnTypeInfo::Kind::None:
            return NoneSemType::make();
        case ReturnTypeInfo::Kind::Named: {
            // ... 现有 Named 处理 ...
        }
        case ReturnTypeInfo::Kind::Generator:
            return IterSemType::make(intType());
        case ReturnTypeInfo::Kind::Generic:
            return ErrorSemType::make();
    }
    return ErrorSemType::make();
}

// 改为：
std::unique_ptr<SemType> SemAnalyzer::semTypeFromBuiltinReturn(
    const ReturnTypeInfo& ret, const SemType* objType) {
    switch (ret.kind) {
        case ReturnTypeInfo::Kind::None:
            return NoneSemType::make();
        case ReturnTypeInfo::Kind::Named: {
            // ... 现有 Named 处理保持不变 ...
        }
        case ReturnTypeInfo::Kind::Generator:
            return IterSemType::make(intType());
        case ReturnTypeInfo::Kind::Generic:
            return ErrorSemType::make();
        case ReturnTypeInfo::Kind::View: {
            // View 返回类型：用调用方 objType 构造 ViewSemType
            // objType 为空时退化为 ErrorSemType（容错）
            if (!objType) return ErrorSemType::make();
            auto v = ViewSemType::make(objType->clone());
            typeStore_.push_back(v->clone());
            return typeStore_.back()->clone();
        }
    }
    return ErrorSemType::make();
}
```

**注意**：`case ReturnTypeInfo::Kind::Named:` 内的完整逻辑保持不变（包括 [T] 类型展开等），仅在 switch 末尾新增 `Kind::View` case。

---

## 变更 5：inferMethodCall 传递 objType

### 文件：src/Sema/Checker/ExprInfer.cpp

**修改 5a：内置模块函数调用处（L316）**

```cpp
// 原（L309-318）：
        auto& ret = fn->returns;
        switch (ret.kind) {
            case ReturnTypeInfo::Kind::None:
                return NoneSemType::make();
            case ReturnTypeInfo::Kind::Named:
            case ReturnTypeInfo::Kind::Generator:
            case ReturnTypeInfo::Kind::Generic:
                return semTypeFromBuiltinReturn(ret);
        }
    }
}

// 改为：
        auto& ret = fn->returns;
        switch (ret.kind) {
            case ReturnTypeInfo::Kind::None:
                return NoneSemType::make();
            case ReturnTypeInfo::Kind::Named:
            case ReturnTypeInfo::Kind::Generator:
            case ReturnTypeInfo::Kind::Generic:
                return semTypeFromBuiltinReturn(ret);
            // View 返回类型在模块函数路径无意义，退化为 Error
            case ReturnTypeInfo::Kind::View:
                return ErrorSemType::make();
        }
    }
}
```

**修改 5b：内置类型方法调用处（L342-351）**

```cpp
// 原（L342-352）：
        if (auto* entry = BuiltinRegistry::get().findMethod(typeKey, e.method, (int)e.args.size())) {
            auto& ret = entry->returns;
            switch (ret.kind) {
                case ReturnTypeInfo::Kind::None:
                    return NoneSemType::make();
                case ReturnTypeInfo::Kind::Named:
                case ReturnTypeInfo::Kind::Generator:
                case ReturnTypeInfo::Kind::Generic:
                    return semTypeFromBuiltinReturn(ret);
            }
        }

// 改为：
        if (auto* entry = BuiltinRegistry::get().findMethod(typeKey, e.method, (int)e.args.size())) {
            auto& ret = entry->returns;
            switch (ret.kind) {
                case ReturnTypeInfo::Kind::None:
                    return NoneSemType::make();
                case ReturnTypeInfo::Kind::Named:
                case ReturnTypeInfo::Kind::Generator:
                case ReturnTypeInfo::Kind::Generic:
                    return semTypeFromBuiltinReturn(ret);
                case ReturnTypeInfo::Kind::View: {
                    // View 返回类型：用 objType 构造 ViewSemType
                    // 注意：ViewSemType + 修改方法会在变更 6 中处理退化
                    auto viewType = semTypeFromBuiltinReturn(ret, objType.get());
                    // 修改方法触发类型退化
                    if (dynamic_cast<const ViewSemType*>(viewType.get())
                        && isViewMutatingMethod(e.method)) {
                        return handleViewMutation(e, viewType);
                    }
                    return viewType;
                }
            }
        }
```

**修改 5c：新增辅助函数 isViewMutatingMethod（ExprInfer.cpp 文件顶部，inferMethodCall 之前）**

```cpp
// 判断方法是否是视图修改方法（触发深拷贝退化）
// 修改方法：append/pop/insert/remove/clear/reserve
// 注意：view[i] = val 不走 inferMethodCall，在 inferAssign 中处理
static bool isViewMutatingMethod(const std::string& method) {
    return method == "append" || method == "pop" || method == "insert"
        || method == "remove" || method == "clear" || method == "reserve";
}
```

---

## 变更 6：ViewSemType 修改方法触发类型退化

### 文件：src/Sema/Checker/ExprInfer.cpp

**修改 6a：新增 handleViewMutation 方法声明（SemAnalyzer.h 私有方法区）**

```cpp
// 在 SemAnalyzer.h 私有方法区（L113 inferExpr 附近）新增：
private:
    // View 修改方法处理：深拷贝退化，返回 ownerType 并更新 symtab 中变量类型
    [[nodiscard]] std::unique_ptr<SemType> handleViewMutation(
        const MethodCallExpr& e, std::unique_ptr<SemType> viewType);
```

**修改 6b：handleViewMutation 实现（ExprInfer.cpp，inferMethodCall 之后）**

```cpp
std::unique_ptr<SemType> SemAnalyzer::handleViewMutation(
    const MethodCallExpr& e, std::unique_ptr<SemType> viewType) {
    auto* v = dynamic_cast<const ViewSemType*>(viewType.get());
    if (!v || !v->ownerType) return ErrorSemType::make();

    // 返回 ownerType（ListSemType / PrimSemType(String) 等）
    auto ownerType = v->ownerType->clone();

    // 更新 symtab 中变量类型：view 变量永久退化为 owner 类型
    if (auto* id = dynamic_cast<const Identifier*>(e.object.get())) {
        if (auto* sym = symtab_.lookup(id->name)) {
            sym->updateType(ownerType->clone());
        }
    }

    return ownerType;
}
```

---

## 变更 7：Symbol 新增 updateType 方法

### 文件：src/Sema/Symbol.h

**修改 7a：Symbol 新增 updateType（L52 之前，结构体末尾）**

```cpp
// 原（L29-53）：
struct Symbol {
    SymKind  kind;
    std::string name;
    std::unique_ptr<SemType> type; // 符号的类型
    // ... 其他字段 ...
    std::string belongsToModule;
    bool isPublic = true;
};

// 改为（在 isPublic 之后新增 updateType 方法）：
struct Symbol {
    SymKind  kind;
    std::string name;
    std::unique_ptr<SemType> type; // 符号的类型
    bool isConst = false;

    std::vector<SymParam> params;
    bool throws = false;

    std::vector<std::string> typeParams;

    std::vector<SymParam> ctorParams;
    std::unique_ptr<SemType> ctorReturnType;

    std::vector<InterfaceSemType::MethodSig> interfaceMethods;

    std::string belongsToModule;
    bool isPublic = true;

    // View 类型退化：替换符号的类型（unique_ptr 不能直接赋值）
    void updateType(std::unique_ptr<SemType> t) { type = std::move(t); }
};
```

---

## 变更 8：mapSemType 处理 ViewSemType

### 文件：src/CodeGen/TypeMap.cpp

**修改 8a：mapSemType 新增 ViewSemType 分支（L182 ListSemType 之后）**

```cpp
// 原（L182-184）：
    if (auto* l = dynamic_cast<const ListSemType*>(&semType)) {
        return "aura_rt::Array<" + mapSemType(*l->elementType) + ">*";
    }

// 改为：
    if (auto* l = dynamic_cast<const ListSemType*>(&semType)) {
        return "aura_rt::Array<" + mapSemType(*l->elementType) + ">*";
    }
    // ViewSemType：映射为 ArrayView<T>*（owner 是 ListSemType 时）
    if (auto* v = dynamic_cast<const ViewSemType*>(&semType)) {
        if (!v->ownerType) return "auto";
        if (auto* l = dynamic_cast<const ListSemType*>(v->ownerType.get())) {
            return "aura_rt::ArrayView<" + mapSemType(*l->elementType) + ">*";
        }
        // 未来：string → GcStringView*（暂不支持，退化为 auto）
        return "auto";
    }
```

---

## 变更 9：isGcPointerType 识别 ArrayView*

### 文件：src/CodeGen/TypeMap.cpp

**修改 9a：isGcPointerType 保持现状即可（L32-39）**

```cpp
// 现有实现（无需修改）：
bool CodeGenerator::isGcPointerType(const std::string& cppType) const {
    if (cppType.empty() || cppType.back() != '*') return false;
    if (cppType == "int32_t*" || cppType == "double*" || cppType == "bool*")
        return false;
    if (cppType == "const char*") return false;
    if (cppType == "auto") return false;
    return true;  // ArrayView<T>* 自动识别为 GC 指针
}
```

**说明**：`ArrayView<T>*` 以 `*` 结尾且不在排除列表中，自动被识别为 GC 指针，会被 GcRootHandle 包装。

---

## 变更 10：genLetStmt 识别 ViewSemType

### 文件：src/CodeGen/StmtGen.cpp

**修改 10a：genLetStmt 推断类型分支新增 ViewSemType（L90 附近）**

```cpp
// 原（L73-94）：
    if (auto* rs = dynamic_cast<const RecordSemType*>(decl.inferredType)) {
        // ... Record 处理 ...
    } else if (auto* gs = dynamic_cast<const GenericSemType*>(decl.inferredType)) {
        // ... Generic 处理 ...
    } else if (auto* ls = dynamic_cast<const ListSemType*>(decl.inferredType)) {
        type = mapSemType(*ls);
    } else if (auto* ps = dynamic_cast<const PrimSemType*>(decl.inferredType)) {
        type = mapSemType(*ps);
    }

// 改为：
    if (auto* rs = dynamic_cast<const RecordSemType*>(decl.inferredType)) {
        // ... Record 处理不变 ...
    } else if (auto* gs = dynamic_cast<const GenericSemType*>(decl.inferredType)) {
        // ... Generic 处理不变 ...
    } else if (auto* ls = dynamic_cast<const ListSemType*>(decl.inferredType)) {
        type = mapSemType(*ls);
    } else if (auto* ps = dynamic_cast<const PrimSemType*>(decl.inferredType)) {
        type = mapSemType(*ps);
    } else if (auto* vs = dynamic_cast<const ViewSemType*>(decl.inferredType)) {
        // View 类型：映射为 ArrayView<T>*
        type = mapSemType(*vs);
    }
```

**说明**：通过 mapSemType 得到 `aura_rt::ArrayView<T>*`，isGcPointerType 自动包装为 GcRootHandle。

---

## 变更 11：genMethodCall 修改方法生成重新绑定

### 文件：src/CodeGen/ExprGen.cpp

**修改 11a：genMethodCall 中检测 ViewSemType + 修改方法（L750 oss << ")" 之后）**

```cpp
// 原（L750-760）：
    oss << ")";
    std::string callExpr = oss.str();

    // Io 调用、值类型对象...

// 改为：
    oss << ")";
    std::string callExpr = oss.str();

    // View 修改方法：生成 view = view->method(args) 重新绑定
    // Sema 已将 view 变量类型退化为 owner，但 CodeGen 时 view 底层仍是 ArrayView<T>*
    // 通过 rebind 将 view 指针指向深拷贝返回的新 Array<T>*
    if (auto* id = dynamic_cast<const Identifier*>(e.object.get())) {
        if (e.object->inferredType
            && dynamic_cast<const ViewSemType*>(e.object->inferredType)) {
            // 此时 view 变量已通过 GcRootHandle 包装，使用 .get() 赋值
            std::string varName = safeName(id->name);
            if (gcRootVarNames_.count(varName)) {
                // view.get() = view.get()->method(args)
                // 通过 genGcRootedArgs 保护参数
                std::vector<std::pair<std::string, const SemType*>> gcArgs;
                gcArgs.emplace_back(obj, e.object->inferredType);  // {0} = obj
                for (size_t i = 0; i < mArgExprs.size(); ++i)
                    gcArgs.emplace_back(mArgExprs[i], e.args[i]->inferredType);
                std::ostringstream gcCall;
                gcCall << "{0}" << access << safeName(e.method) << "(";
                for (size_t i = 0; i < mArgExprs.size(); ++i) {
                    if (i > 0) gcCall << ", ";
                    gcCall << "{" << (i + 1) << "}";
                }
                gcCall << ")";
                return varName + ".get() = " + genGcRootedArgs(gcArgs, gcCall.str(), isCoroutine);
            }
        }
    }

    // Io 调用、值类型对象...
```

**注意**：`e.object->inferredType` 在 Sema 阶段已被设置为 ViewSemType（slice 返回值推断）。修改方法调用后 Sema 将变量类型更新为 owner，但 `e.object->inferredType` 仍是调用时的类型（ViewSemType）。

**修正**：实际上 Sema 在 handleViewMutation 中更新的是 symtab 中 Symbol::type，而 ASTNode::inferredType 保持为 ViewSemType。但 CodeGen 的 genMethodCall 通过 `e.object->inferredType` 判断的是调用时类型，**仍是 ViewSemType**。因此此处的判断正确。

---

## 变更 12：genAssignExpr view[i]=val 触发深拷贝

### 文件：src/CodeGen/ExprGen.cpp

**修改 12a：genAssignExpr 新增 view 索引赋值处理（L829 附近，s = s + x 优化之后）**

```cpp
// 原（L829-864）：
std::string CodeGenerator::genAssignExpr(const AssignExpr& e, bool isCoroutine) {
    std::string target = genExpr(*e.target, isCoroutine);
    std::string value  = genExpr(*e.value, isCoroutine);

    // stripGet 辅助
    auto stripGet = ...

    // s = s + x 优化
    if (auto* targetId = dynamic_cast<const Identifier*>(e.target.get())) {
        if (auto* binExpr = dynamic_cast<const BinaryExpr*>(e.value.get())) {
            // ... s = s + x 优化 ...
        }
    }
    // ... 后续处理 ...

// 改为（在 s = s + x 优化之后、string 追踪之前新增 view[i] = val 处理）：
std::string CodeGenerator::genAssignExpr(const AssignExpr& e, bool isCoroutine) {
    std::string target = genExpr(*e.target, isCoroutine);
    std::string value  = genExpr(*e.value, isCoroutine);

    // stripGet 辅助
    auto stripGet = ...

    // s = s + x 优化（保持不变）
    if (auto* targetId = dynamic_cast<const Identifier*>(e.target.get())) {
        if (auto* binExpr = dynamic_cast<const BinaryExpr*>(e.value.get())) {
            // ... s = s + x 优化不变 ...
        }
    }

    // view[i] = val 深拷贝退化
    // 检测：target 是 IndexExpr，object 是 view 变量
    if (auto* idxExpr = dynamic_cast<const IndexExpr*>(e.target.get())) {
        if (auto* viewId = dynamic_cast<const Identifier*>(idxExpr->object.get())) {
            // 通过 symtab 推断的 objType 是否是 ViewSemType
            // 注意：此时 view 变量类型已可能退化（如果之前调用了修改方法）
            // 但 Sema 推断 IndexExpr 时 objType 仍是 ViewSemType（未退化）
            if (idxExpr->object->inferredType
                && dynamic_cast<const ViewSemType*>(idxExpr->object->inferredType)) {
                std::string viewVar = safeName(viewId->name);
                if (gcRootVarNames_.count(viewVar)) {
                    std::string idxExprStr = genExpr(*idxExpr->index, false);
                    // view.get() = view.get()->set(idx, val)
                    // ArrayView::set 深拷贝后赋值并返回新 Array*
                    std::vector<std::pair<std::string, const SemType*>> gcArgs;
                    gcArgs.emplace_back(viewVar, idxExpr->object->inferredType);  // {0} = view
                    gcArgs.emplace_back(idxExprStr, idxExpr->index->inferredType);  // {1} = idx
                    gcArgs.emplace_back(value, e.value->inferredType);  // {2} = val
                    return viewVar + ".get() = " + genGcRootedArgs(gcArgs,
                        "{0}->set({1}, {2})", isCoroutine);
                }
            }
        }
    }

    // ... 后续 string 追踪等保持不变 ...
```

**说明**：ArrayView 需新增 `set(int32_t idx, T value)` 方法（见变更 13）。

---

## 变更 13：ArrayView 新增修改方法 + ViewIterator

### 文件：runtime/builtin/array.h

**修改 13a：ArrayView 类新增修改方法声明（L993 之前，工厂方法之前）**

```cpp
// 原（L988-1003）：
    // 嵌套 slice
    ArrayView<T>* slice(int32_t relStart, int32_t subLen) const;

    // GC 类型描述符
    static const TypeDescriptor& desc();

    // 工厂方法
    static ArrayView<T>* make(Array<T>* owner, int32_t start, int32_t len) {
        // ...
    }
};

// 改为：
    // 嵌套 slice
    ArrayView<T>* slice(int32_t relStart, int32_t subLen) const;

    // ===== 修改方法：深拷贝退化为 owner（返回新 Array<T>*) =====
    // CodeGen 生成：view = view->method(args)
    // Sema 同步将 view 变量类型退化为 ListSemType
    Array<T>* append(T value) const;
    Array<T>* pop() const;
    Array<T>* pop(int32_t idx) const;
    Array<T>* insert(int32_t idx, T value) const;
    Array<T>* remove(int32_t idx) const;
    Array<T>* clear() const;
    Array<T>* reserve(int32_t cap) const;
    // view[i] = val 深拷贝退化
    Array<T>* set(int32_t relIdx, T value) const;

    // ===== for-in 迭代 =====
    using Iterator = ArrayViewIterator<T>;
    Iterator begin();
    Iterator end();

    // GC 类型描述符
    static const TypeDescriptor& desc();

    // 工厂方法
    static ArrayView<T>* make(Array<T>* owner, int32_t start, int32_t len) {
        // ... 保持不变 ...
    }
};
```

**修改 13b：ArrayView 修改方法 + ViewIterator 实现（ArrayView::slice 实现之后）**

```cpp
// ============================================================
// P2-D: ArrayView 修改方法 — 深拷贝退化为 Array<T>
// ============================================================
template<typename T>
Array<T>* ArrayView<T>::append(T value) const {
    GcCompactSuspendGuard guard;
    Array<T>* arr = Array<T>::make(len_);
    for (int32_t i = 0; i < len_; ++i) {
        arr->append((*owner_)[start_ + i]);
    }
    arr->append(value);
    return arr;
}

template<typename T>
Array<T>* ArrayView<T>::pop() const {
    GcCompactSuspendGuard guard;
    Array<T>* arr = Array<T>::make(len_ > 0 ? len_ - 1 : 0);
    for (int32_t i = 0; i < len_; ++i) {
        arr->append((*owner_)[start_ + i]);
    }
    if (arr->len() > 0) arr->pop();
    return arr;
}

template<typename T>
Array<T>* ArrayView<T>::pop(int32_t idx) const {
    GcCompactSuspendGuard guard;
    Array<T>* arr = Array<T>::make(len_ > 0 ? len_ - 1 : 0);
    for (int32_t i = 0; i < len_; ++i) {
        if (i != idx) arr->append((*owner_)[start_ + i]);
    }
    return arr;
}

template<typename T>
Array<T>* ArrayView<T>::insert(int32_t idx, T value) const {
    GcCompactSuspendGuard guard;
    Array<T>* arr = Array<T>::make(len_ + 1);
    for (int32_t i = 0; i < idx; ++i) {
        arr->append((*owner_)[start_ + i]);
    }
    arr->append(value);
    for (int32_t i = idx; i < len_; ++i) {
        arr->append((*owner_)[start_ + i]);
    }
    return arr;
}

template<typename T>
Array<T>* ArrayView<T>::remove(int32_t idx) const {
    GcCompactSuspendGuard guard;
    Array<T>* arr = Array<T>::make(len_ > 0 ? len_ - 1 : 0);
    for (int32_t i = 0; i < len_; ++i) {
        if (i != idx) arr->append((*owner_)[start_ + i]);
    }
    return arr;
}

template<typename T>
Array<T>* ArrayView<T>::clear() const {
    return Array<T>::make(0);
}

template<typename T>
Array<T>* ArrayView<T>::reserve(int32_t cap) const {
    GcCompactSuspendGuard guard;
    Array<T>* arr = Array<T>::make(cap > len_ ? cap : len_);
    for (int32_t i = 0; i < len_; ++i) {
        arr->append((*owner_)[start_ + i]);
    }
    return arr;
}

template<typename T>
Array<T>* ArrayView<T>::set(int32_t relIdx, T value) const {
    GcCompactSuspendGuard guard;
    Array<T>* arr = Array<T>::make(len_);
    for (int32_t i = 0; i < len_; ++i) {
        if (i == relIdx) {
            arr->append(value);
        } else {
            arr->append((*owner_)[start_ + i]);
        }
    }
    return arr;
}

// ============================================================
// P2-D: ArrayViewIterator — for-in 迭代器
// 限制范围 [start_, start_+len_)
// ============================================================
template<typename T>
class ArrayViewIterator {
    Array<T>* owner_;
    int32_t cur_;   // 当前绝对索引
    int32_t end_;   // 终止绝对索引（start_ + len_）

public:
    ArrayViewIterator(Array<T>* owner, int32_t cur, int32_t end)
        : owner_(owner), cur_(cur), end_(end) {}

    T& operator*() { return (*owner_)[cur_]; }
    T* operator->() { return &(*owner_)[cur_]; }

    ArrayViewIterator& operator++() {
        ++cur_;
        return *this;
    }

    bool operator!=(const ArrayViewIterator& o) const {
        return cur_ != o.cur_;
    }

    // 默认拷贝/移动（Iterator 需要可拷贝）
    ArrayViewIterator(const ArrayViewIterator&) = default;
    ArrayViewIterator& operator=(const ArrayViewIterator&) = default;
    ArrayViewIterator(ArrayViewIterator&&) = default;
    ArrayViewIterator& operator=(ArrayViewIterator&&) = default;
};

template<typename T>
typename ArrayView<T>::Iterator ArrayView<T>::begin() {
    return {owner_, start_, start_ + len_};
}

template<typename T>
typename ArrayView<T>::Iterator ArrayView<T>::end() {
    return {owner_, start_ + len_, start_ + len_};
}
```

**注意**：ArrayViewIterator 需要在 ArrayView 类定义之前前向声明，或放在 ArrayView 之前。

**修改 13c：ArrayViewIterator 前向声明（ArrayView 之前）**

在 `class ArrayView` 定义之前新增：

```cpp
template<typename T>
class ArrayViewIterator;
```

---

## 变更 14：inferIndexExpr + genIndexExpr 识别 ViewSemType

### 文件：src/Sema/Checker/ExprInfer.cpp

**修改 14a：inferIndexExpr 支持 ViewSemType（L401-408）**

```cpp
// 原（L401-408）：
std::unique_ptr<SemType> SemAnalyzer::inferIndexExpr(const IndexExpr& e) {
    auto objType = inferExpr(*e.object);
    if (auto* list = dynamic_cast<const ListSemType*>(objType.get())) {
        return list->elementType ? list->elementType->clone() : ErrorSemType::make();
    }
    return ErrorSemType::make();
}

// 改为：
std::unique_ptr<SemType> SemAnalyzer::inferIndexExpr(const IndexExpr& e) {
    auto objType = inferExpr(*e.object);
    if (auto* list = dynamic_cast<const ListSemType*>(objType.get())) {
        return list->elementType ? list->elementType->clone() : ErrorSemType::make();
    }
    // ViewSemType：索引返回 owner 的元素类型
    if (auto* view = dynamic_cast<const ViewSemType*>(objType.get())) {
        if (auto* list = dynamic_cast<const ListSemType*>(view->ownerType.get())) {
            return list->elementType ? list->elementType->clone() : ErrorSemType::make();
        }
    }
    return ErrorSemType::make();
}
```

### 文件：src/CodeGen/ExprGen.cpp

**修改 14b：genIndexExpr 保持现状（L819-823）**

```cpp
// 现有实现（无需修改）：
std::string CodeGenerator::genIndexExpr(const IndexExpr& e, bool isCoroutine) {
    std::string obj   = genExpr(*e.object, false);
    std::string idx   = genExpr(*e.index, isCoroutine);
    return "(*" + obj + ")[" + idx + "]";
}
```

**说明**：`(*view)[i]` 调用 ArrayView::operator[]，已实现。

---

## 变更 15：for-in 迭代识别 ViewSemType

### 文件：src/Sema/Checker/StmtChecker.cpp

**修改 15a：checkForStmt 支持 ViewSemType（L150-170）**

```cpp
// 原（L150-170）：
void SemAnalyzer::checkForStmt(const ForStmt& stmt) {
    auto iterType = inferExpr(*stmt.iterable);
    bool prev = insideLoop_; insideLoop_ = true;
    symtab_.enterScope();
    Symbol sym;
    sym.kind = SymKind::Variable;
    sym.name = stmt.itemName;
    if (auto* listTy = dynamic_cast<ListSemType*>(iterType.get())) {
        sym.type = listTy->elementType ? listTy->elementType->clone() : ErrorSemType::make();
    } else if (auto* iterTy = dynamic_cast<IterSemType*>(iterType.get())) {
        sym.type = iterTy->elementType ? iterTy->elementType->clone() : ErrorSemType::make();
    } else {
        sym.type = ErrorSemType::make();
    }
    symtab_.define(std::move(sym));
    if (stmt.body) checkBlock(*stmt.body);
    symtab_.exitScope();
    insideLoop_ = prev;
}

// 改为：
void SemAnalyzer::checkForStmt(const ForStmt& stmt) {
    auto iterType = inferExpr(*stmt.iterable);
    bool prev = insideLoop_; insideLoop_ = true;
    symtab_.enterScope();
    Symbol sym;
    sym.kind = SymKind::Variable;
    sym.name = stmt.itemName;
    if (auto* listTy = dynamic_cast<ListSemType*>(iterType.get())) {
        sym.type = listTy->elementType ? listTy->elementType->clone() : ErrorSemType::make();
    } else if (auto* iterTy = dynamic_cast<IterSemType*>(iterType.get())) {
        sym.type = iterTy->elementType ? iterTy->elementType->clone() : ErrorSemType::make();
    } else if (auto* viewTy = dynamic_cast<ViewSemType*>(iterType.get())) {
        // ViewSemType：元素类型 = owner 的元素类型
        if (auto* list = dynamic_cast<const ListSemType*>(viewTy->ownerType.get())) {
            sym.type = list->elementType ? list->elementType->clone() : ErrorSemType::make();
        } else {
            sym.type = ErrorSemType::make();
        }
    } else {
        sym.type = ErrorSemType::make();
    }
    symtab_.define(std::move(sym));
    if (stmt.body) checkBlock(*stmt.body);
    symtab_.exitScope();
    insideLoop_ = prev;
}
```

### 文件：src/CodeGen/StmtGen.cpp

**修改 15b：genForStmt 默认数组遍历支持 view（L443-450）**

```cpp
// 原（L443-450）：
    // 默认：数组/列表遍历
    std::string iter = genExpr(*stmt.iterable, isCoroutine);
    cpp << indentStr() << "for (auto " << safeName(stmt.itemName)
        << " : *" << iter << ") {\n";
    if (stmt.body) genBlock(cpp, *stmt.body, isCoroutine);
    writeLine(cpp, "aura_rt::gc_safepoint();");  // L2 safepoint
    cpp << indentStr() << "}\n";
}

// 改为（无需修改，view 的 begin/end 已实现）：
    // 默认：数组/列表遍历
    // view 遍历同样用 *view 解引用（ArrayView 实现了 begin/end）
    std::string iter = genExpr(*stmt.iterable, isCoroutine);
    cpp << indentStr() << "for (auto " << safeName(stmt.itemName)
        << " : *" << iter << ") {\n";
    if (stmt.body) genBlock(cpp, *stmt.body, isCoroutine);
    writeLine(cpp, "aura_rt::gc_safepoint();");  // L2 safepoint
    cpp << indentStr() << "}\n";
}
```

**说明**：`for (auto x : *view)` 会调用 ArrayView::begin/end，返回 ArrayViewIterator，遍历范围 `[start_, start_+len_)`。

---

## 变更 16：inferAssign 中 view 类型退化后处理

### 文件：src/Sema/Checker/ExprInfer.cpp

**修改 16a：inferAssign 中处理 view[i] = val（L410-427）**

```cpp
// 原（L410-427）：
std::unique_ptr<SemType> SemAnalyzer::inferAssign(const AssignExpr& e) {
    // const 绑定不可重新赋值
    if (auto* id = dynamic_cast<const Identifier*>(e.target.get())) {
        if (auto* sym = symtab_.lookup(id->name)) {
            if (sym->isConst) {
                error(e, DiagCode::E015_ConstReassign,
                      "cannot reassign to const binding '" + id->name + "'",
                      "use 'let' instead of 'const' if you need to reassign");
            }
        }
    }
    auto targetTy = inferExpr(*e.target);
    auto valueTy  = inferExpr(*e.value);
    if (!isAssignable(*targetTy, *valueTy)) {
        error(e, "assignment type mismatch: ...");
    }
    return valueTy->clone();
}

// 改为（新增 view[i] = val 类型退化处理）：
std::unique_ptr<SemType> SemAnalyzer::inferAssign(const AssignExpr& e) {
    // const 绑定不可重新赋值
    if (auto* id = dynamic_cast<const Identifier*>(e.target.get())) {
        if (auto* sym = symtab_.lookup(id->name)) {
            if (sym->isConst) {
                error(e, DiagCode::E015_ConstReassign,
                      "cannot reassign to const binding '" + id->name + "'",
                      "use 'let' instead of 'const' if you need to reassign");
            }
        }
    }
    auto targetTy = inferExpr(*e.target);
    auto valueTy  = inferExpr(*e.value);
    if (!isAssignable(*targetTy, *valueTy)) {
        error(e, "assignment type mismatch: ...");
    }

    // view[i] = val 触发深拷贝退化
    // 检测：target 是 IndexExpr，object 是 view 变量（ViewSemType）
    if (auto* idxExpr = dynamic_cast<const IndexExpr*>(e.target.get())) {
        if (idxExpr->object->inferredType
            && dynamic_cast<const ViewSemType*>(idxExpr->object->inferredType)) {
            if (auto* viewId = dynamic_cast<const Identifier*>(idxExpr->object.get())) {
                if (auto* sym = symtab_.lookup(viewId->name)) {
                    // 退化为 owner 类型（ListSemType）
                    auto* viewType = dynamic_cast<const ViewSemType*>(
                        idxExpr->object->inferredType);
                    if (viewType && viewType->ownerType) {
                        sym->updateType(viewType->ownerType->clone());
                    }
                }
            }
        }
    }

    return valueTy->clone();
}
```

---

## 变更 17：isHeapSemType 识别 ViewSemType

### 文件：src/CodeGen/ExprGen.cpp

**修改 17a：isHeapSemType 识别 ViewSemType（L12-30）**

```cpp
// 原（L12-30）：
bool CodeGenerator::isHeapSemType(const SemType* type) const {
    if (!type) return false;
    if (auto* p = dynamic_cast<const PrimSemType*>(type))
        return p->kind == PrimSemType::String;
    if (dynamic_cast<const NoneSemType*>(type)) return false;
    if (dynamic_cast<const ErrorSemType*>(type)) return false;
    if (dynamic_cast<const InterfaceSemType*>(type)) return false;
    if (dynamic_cast<const FuncSemType*>(type)) return false;
    if (auto* u = dynamic_cast<const UnionSemType*>(type)) {
        for (auto& v : u->variants)
            if (isHeapSemType(v.get())) return true;
        return false;
    }
    return true;
}

// 改为：
bool CodeGenerator::isHeapSemType(const SemType* type) const {
    if (!type) return false;
    if (auto* p = dynamic_cast<const PrimSemType*>(type))
        return p->kind == PrimSemType::String;
    if (dynamic_cast<const NoneSemType*>(type)) return false;
    if (dynamic_cast<const ErrorSemType*>(type)) return false;
    if (dynamic_cast<const InterfaceSemType*>(type)) return false;
    if (dynamic_cast<const FuncSemType*>(type)) return false;
    if (auto* u = dynamic_cast<const UnionSemType*>(type)) {
        for (auto& v : u->variants)
            if (isHeapSemType(v.get())) return true;
        return false;
    }
    // ViewSemType：堆类型（ArrayView 是 GcObject）
    if (dynamic_cast<const ViewSemType*>(type)) return true;
    return true;
}
```

**说明**：实际上 ViewSemType 会落入最后的 `return true`，但显式添加分支更清晰。

---

## 测试方案

新建独立测试文件 `example/test.aura`（覆盖原文件）：

```aura
// ArrayView<T> 零拷贝视图测试

fun main(io: Io) {
    // K1: slice 基本读访问
    io.println("=== K1: slice read ===")
    let arr: [int] = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]
    let view = arr.slice(2, 5)
    io.println("view len: " + view.len())
    io.println("view[0]: " + view[0])
    io.println("view[4]: " + view[4])

    // K2: 嵌套 slice
    io.println("=== K2: nested slice ===")
    let view2 = view.slice(1, 3)
    io.println("nested view len: " + view2.len())
    io.println("nested view[0]: " + view2[0])
    io.println("nested view[2]: " + view2[2])

    // K3: GC 后 view 引用有效
    io.println("=== K3: GC safety ===")
    gc_force()
    io.println("after GC, view[0]: " + view[0])
    io.println("after GC, view2[2]: " + view2[2])

    // K4: 空 slice
    io.println("=== K4: empty slice ===")
    let emptyView = arr.slice(0, 0)
    io.println("empty view len: " + emptyView.len())
    io.println("empty view empty: " + emptyView.empty())

    // K5: front/back
    io.println("=== K5: front/back ===")
    io.println("view front: " + view.front())
    io.println("view back: " + view.back())

    // K6: for-in 迭代
    io.println("=== K6: for-in iteration ===")
    let sum: int = 0
    for x in view {
        sum = sum + x
    }
    io.println("sum of view: " + sum)

    // K7: view.append 触发深拷贝退化
    io.println("=== K7: view.append degradation ===")
    let view3 = arr.slice(1, 3)
    io.println("before append, view3 len: " + view3.len())
    view3.append(99)
    io.println("after append, view3 len: " + view3.len())
    io.println("view3[3]: " + view3[3])
    view3.append(100)
    io.println("view3[4]: " + view3[4])
    io.println("arr len unchanged: " + arr.len())

    // K8: view[i] = val 触发深拷贝
    io.println("=== K8: view[i] = val ===")
    let view4 = arr.slice(0, 3)
    view4[0] = 999
    io.println("view4[0]: " + view4[0])
    io.println("arr[0] unchanged: " + arr[0])

    // K9: 函数参数传递（view 与 [int] 等价）
    io.println("=== K9: function param ===")
    processView(view)

    // K10: 大数组 slice + 多次 GC
    io.println("=== K10: large slice + GC ===")
    let big: [int] = []
    let i: int = 0
    while i < 100 {
        big.append(i)
        i = i + 1
    }
    let bigView = big.slice(10, 50)
    gc_force()
    gc_force()
    io.println("bigView len: " + bigView.len())
    io.println("bigView[0]: " + bigView[0])
    io.println("bigView[49]: " + bigView[49])

    // K11: view 修改方法后 GC
    io.println("=== K11: post-mutation GC ===")
    let view5 = arr.slice(2, 4)
    view5.append(77)
    gc_force()
    io.println("view5[4]: " + view5[4])

    io.println("=== All tests passed ===")
}

fun processView(items: [int]) {
    io.println("processView len: " + items.len())
    io.println("processView first: " + items[0])
}
```

---

## 实施顺序

1. 变更 1 — ViewSemType（SemType.h + .cpp）
2. 变更 2 — ReturnTypeInfo View kind（BuiltinRegistry.h）
3. 变更 3 — slice 注册改 View（BuiltinRegistry.h）
4. 变更 7 — Symbol::updateType（Symbol.h）
5. 变更 4 — semTypeFromBuiltinReturn 扩展（SemAnalyzer.h + .cpp）
6. 变更 5 — inferMethodCall 传递 objType（ExprInfer.cpp）
7. 变更 6 — handleViewMutation（SemAnalyzer.h + ExprInfer.cpp）
8. 变更 16 — inferAssign view 退化（ExprInfer.cpp）
9. 变更 14 — inferIndexExpr ViewSemType（ExprInfer.cpp）
10. 变更 15 — checkForStmt ViewSemType（StmtChecker.cpp）
11. 变更 8 — mapSemType ViewSemType（TypeMap.cpp）
12. 变更 17 — isHeapSemType ViewSemType（ExprGen.cpp）
13. 变更 10 — genLetStmt ViewSemType（StmtGen.cpp）
14. 变更 11 — genMethodCall 重新绑定（ExprGen.cpp）
15. 变更 12 — genAssignExpr view[i]=val（ExprGen.cpp）
16. 变更 13 — ArrayView 修改方法 + ViewIterator（array.h）
17. 编译 + 测试
