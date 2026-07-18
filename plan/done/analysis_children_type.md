# 为什么 `children` 的类型是 `Array<int32_t>` 而不是 `Array<Tree<int32_t>>`

## 问题代码

```cpp
// test.cpp 第 151-162 行
Tree<int32_t>* tree = aura_rt::gc_alloc<Tree<int32_t>>(&Tree<int32_t>::_desc);
tree->value = 1;
tree->children = [&]() -> aura_rt::Array<int32_t>* {   // ← 类型错误！
    auto* _list_3 = aura_rt::Array<int32_t>::make(2);
    _list_3->append({.value = 2, .children = nullptr});
    _list_3->append({.value = 3, .children = [&]() -> aura_rt::Array<int32_t>* {
        auto* _list_2 = aura_rt::Array<int32_t>::make(1);
        _list_2->append({.value = 4, .children = nullptr});
        return _list_2;
    }()});
    return _list_3;
}();
```

预期应该是 `aura_rt::Array<Tree<int32_t>*>*`，实际生成的是 `aura_rt::Array<int32_t>*`。

---

## 根因分析

### 1. 类型推断的层级结构

Aura → C++ 的类型推断有两个阶段：

```
SemAnalyzer（语义分析）        CodeGenerator（代码生成）
      │                              │
      ▼                              │
设置 inferredType ───────────────►  读取 inferredType
（每个表达式节点上挂 SemType）      （用 mapSemType / mapType 映射）
```

`Tree<T>` 的 `children` 字段定义：

```cpp
// test.cpp 第 27 行
template<typename T>
struct Tree : aura_rt::GcObject {
    T value;
    aura_rt::Array<Tree<T>*>* children;   // 期望：Array<Tree<T>*>*
};
```

所以 `children` 的 **Aura 类型** 是 `Array<Tree<int32_t>>`，语义类型是 `ListSemType`（元素类型是 `RecordSemType`）。

### 2. `genListExpr` 的类型推断逻辑

问题出在 [ExprGen.cpp 第 86-167 行](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L86-L167)。

关键代码段：

```cpp
// 第 101 行：默认值
std::string elemType = "int32_t";

// 第 104-113 行：优先用 inferredType
if (e.inferredType) {
    auto* listTy = dynamic_cast<const ListSemType*>(e.inferredType);
    if (listTy && listTy->elementType) {
        std::string semElemType = mapSemType(*listTy->elementType);
        // 仅当推断出有意义类型且非 GcObject* 时使用
        if (semElemType.find("GcObject") == std::string::npos
            && semElemType.find("/*") == std::string::npos
            && semElemType != "auto")
            elemType = semElemType;
    }
}

// 第 117-151 行：文本启发式回退（inferredType 无效时）
if (elemType == "int32_t") {
    // 检测 RecordExpr ...
    else if (auto* rec = dynamic_cast<const RecordExpr*>(e.elements[0].get())) {
        // 只检查第一个字段的值类型
        if (firstFieldType.find("aura_rt::make_string") != std::string::npos)
            elemType = "aura_rt::GcString*";
        else if (firstFieldType == "true" || firstFieldType == "false")
            elemType = "bool";
        // ⚠ 没有处理 RecordExpr 是 Tree 的情况！
    }
}
```

### 3. 问题的触发路径

```
SemAnalyzer 推断 children 赋值表达式的类型时
    │
    ▼
对于右边的列表表达式 [...Tree..., ...Tree...]：
    │
    ├── 期望：ListSemType { elementType = RecordSemType { canonicalName = "Tree" } }
    │
    ▼
但实际：inferredType 为空 或 elementType 被推断为 GcObject*
    │
    ▼
genListExpr 进入 elemType == "int32_t" 的文本启发式分支
    │
    ▼
检查第一个 RecordExpr 的第一个字段：
    { .value = 2, .children = nullptr }
              ↑
              这个值是 IntLiteral(2)，所以 elemType = "int32_t"
    │
    ▼
生成：aura_rt::Array<int32_t>*
```

### 4. 为什么 `RecordExpr` 的启发式没有命中

文本启发式检查的是：

```cpp
std::string firstFieldType = rec->fields[0].value ? genExpr(...) : "";
if (firstFieldType.find("aura_rt::make_string") != std::string::npos)
    elemType = "aura_rt::GcString*";
else if (firstFieldType == "true" || firstFieldType == "false")
    elemType = "bool";
```

对于 `Tree<int32_t>` 的记录 `{ .value = 2, .children = ... }`：

- `fields[0]` 是 `.value = 2`
- `genExpr` 生成 `"2"`
- `"2".find(...)` 都不匹配，没有回退

而且，这个启发式根本没有检查 **注册类型** 信息，所以不知道 `{value, children}` 结构的 RecordExpr 实际上代表 `Tree<T>`。

---

## 核心 Bug 总结

| 层面 | 问题 | 代码位置 |
|------|------|---------|
| SemAnalyzer | 没有正确为嵌套列表表达式设置 `inferredType = ListSemType<RecordSemType>` | SemAnalyzer |
| CodeGenerator | `genListExpr` 的 inferredType 路径在 `elementType` 是 RecordSemType 时返回 `GcObject*`，导致回退到文本启发式 | [ExprGen.cpp:109-113](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L109-L113) |
| CodeGenerator | 文本启发式没有检查 `registeredTypes_` 中是否存在与 RecordExpr 结构匹配的注册类型（如 Tree） | [ExprGen.cpp:132-140](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L132-L140) |

---

## 修复方向

### 方向 1：修复 SemAnalyzer（根本解决）

确保 `genListExpr` 调用的上下文中，列表表达式的 `inferredType` 正确设置为 `ListSemType`，其 `elementType` 指向具有正确 `canonicalName` 的 `RecordSemType`。

### 方向 2：增强 CodeGenerator 的类型回退逻辑

在 `genListExpr` 的文本启发式分支中，增加对注册类型的检查：

```cpp
// 检测 RecordExpr → 检查是否是注册的类型别名
else if (auto* rec = dynamic_cast<const RecordExpr*>(e.elements[0].get())) {
    // 尝试从 SemAnalyzer 设置的 inferredType 获取 RecordSemType 的 canonicalName
    if (auto* listTy = dynamic_cast<const ListSemType*>(e.inferredType.get())) {
        if (auto* recTy = dynamic_cast<const RecordSemType*>(listTy->elementType.get())) {
            if (!recTy->canonicalName.empty()) {
                elemType = recTy->canonicalName + "*";  // Tree<int32_t>*
                // 但还需要处理泛型参数...
            }
        }
    }
}
```

### 方向 3：让 SemAnalyzer 在 RecordExpr 上直接设置 inferredType

确保每个 `RecordExpr` 节点上的 `inferredType` 直接就是对应的 `RecordSemType`（含 `canonicalName`），这样 `genListExpr` 可以直接用它来推断列表元素类型。
