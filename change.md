# Array 旧 API → 新 API 替换指南

> 生成日期：2026-06-18  
> 背景：`runtime/builtin/array.h` 从连续数组重构为块链表实现，API 发生变化。  
> 以下为 `src/CodeGen/` 和 `src/Sema/` 中需要对应的修改。

---

## 一、新旧 API 对照表

| 旧 API（连续数组） | 新 API（块链表） | 说明 |
|---|---|---|
| `Array<T>::make(n)` | `Array<T>::make(n)` | 兼容，语义从"预分配 n 个连续元素"变为"预分配能容纳 n 个元素的 chunks" |
| `->push(value)` | `->append(value)` | **方法名变更** |
| `->pop()` | `->pop()` | 兼容 |
| `->pop(idx)` | `->pop(idx)` | 兼容（`std::optional<int32_t>`） |
| `->len()` | `->len()` | 兼容 |
| `->elements[i]` | `(*arr)[i]` | 旧 Array 通过 `elements` 字段直接访问，新 Array 通过 `operator[]` |
| `->length` | `->len()` | 旧 Array 直接读 `length` 字段，新 Array 通过 `len()` 方法 |
| `arr->elements`（迭代） | `arr->begin()` / `arr->end()` | 旧 Array 用裸指针迭代，新 Array 用 `ArrayIterator` |
| 无 | `->append(value)` | **新增** |
| 无 | `->insert(idx, value)` | 新增 |
| 无 | `->remove(idx)` | 新增 |
| 无 | `->front()` / `->back()` | 新增 |
| 无 | `->empty()` / `->size()` / `->capacity()` | 新增 |
| 无 | `->clear()` / `->reserve(n)` | 新增 |

---

## 二、需要修改的文件

### 2.1 `src/Sema/BuiltinMethods.h` — 内置方法表

**当前代码**（第 44–46 行）：
```cpp
// === [T] (→ Array<T>*) ===
{"[T]",    "push",          1, false, true,  0},
{"[T]",    "pop",           0, true,  false, 0},
{"[T]",    "len",           0, false, false, (int)Int},
```

**修改**：`"push"` → `"append"`，并新增方法：
```cpp
// === [T] (→ Array<T>*) ===
{"[T]",    "append",        1, false, true,  0},
{"[T]",    "pop",           0, true,  false, 0},
{"[T]",    "pop",           1, true,  false, 0},     // pop(idx) — 带索引的重载
{"[T]",    "len",           0, false, false, (int)Int},
{"[T]",    "size",          0, false, false, (int)Int},
{"[T]",    "empty",         0, false, false, (int)Bool},
{"[T]",    "remove",        1, true,  false, 0},
{"[T]",    "insert",        2, false, true,  0},
```

> **注意**：Aura 语言层面，用户调用 `list.append(x)` 或 `list.pop(idx)` 分别映射到上表中的方法。`genMethodCall`（`ExprGen.cpp:324`）直接使用 `e.method` 作为 C++ 方法名，因此 Aura 方法名与 C++ 运行时方法名必须一致。

---

### 2.2 `src/CodeGen/ExprGen.cpp` — 列表字面量生成

**位置**：`genListExpr` 函数，第 121–128 行

**当前代码**：
```cpp
oss << "[&]() -> aura_rt::Array<" << elemType << ">* {\n";
oss << "    auto* " << var << " = aura_rt::Array<" << elemType
    << ">::make(" << e.elements.size() << ");\n";
for (auto& expr : elemExprs)
    oss << "    " << var << "->push(" << expr << ");\n";   // ← 旧方法名
oss << "    return " << var << ";\n";
oss << "  }()";
```

**修改**：第 125 行 `push` → `append`：
```cpp
for (auto& expr : elemExprs)
    oss << "    " << var << "->append(" << expr << ");\n";
```

> **说明**：`make(n)` 仍然存在（语义兼容），无需修改。

---

### 2.3 `src/CodeGen/StmtGen.cpp` — for-in 循环生成

**位置**：`genForStmt` 函数，第 183–190 行

**当前代码**：
```cpp
void CodeGenerator::genForStmt(std::ostream& cpp, const ForStmt& stmt,
                                bool isCoroutine) {
    std::string iter = genExpr(*stmt.iterable, isCoroutine);
    cpp << indentStr() << "for (auto " << safeName(stmt.itemName)
        << " : *" << iter << ") {\n";
    if (stmt.body) genBlock(cpp, *stmt.body, isCoroutine);
    cpp << indentStr() << "}\n";
}
```

**修改**：**无需修改**。新 Array 支持 `begin()`/`end()` 迭代器，`for (auto item : *arr)` 语法在 C++ 层自动展开为基于迭代器的遍历，与旧 Array 兼容。

---

### 2.4 `src/CodeGen/TypeMap.cpp` — 类型映射

**位置**：`mapType` 函数，第 61–63 行

**当前代码**：
```cpp
if (auto* l = dynamic_cast<const ListType*>(&type)) {
    auto elem = l->elementType ? mapType(*l->elementType) : "???";
    return "aura_rt::Array<" + elem + ">*";
}
```

**修改**：**无需修改**。`aura_rt::Array<T>*` 类型字符串对块链表 Array 同样有效。

---

## 三、不需要修改的文件

| 文件 | 原因 |
|------|------|
| `CodeGen.cpp` — `push_back` 调用 | 使用的是 `std::vector::push_back`（C++ 标准库），与 `aura_rt::Array` 无关 |
| `CodeGen.h` — 注释中的 `ListType(Int) → Array<int32_t>*` | 仅文档注释，写的仍然是正确的类型映射 |
| `DeclGen.cpp` — 注释 `ListType（如 Array<T>*）` | 仅注释，无需修改 |
| `CoroDecide.cpp` — `ListExpr` 遍历 | 遍历 AST 节点的 `elements` 字段（`std::vector`），与运行时 Array 无关 |
| `ExprGen.cpp` line 90 — `Array<T>::make(0)` | 空列表生成使用 `make(0)`，新 Array 兼容 |
| `ExprGen.cpp` line 123 — `Array<T>::make(n)` | 同上，兼容 |

---

## 四、修改顺序建议

1. **先改 `BuiltinMethods.h`**：将 `"push"` → `"append"`，确保语义分析和代码生成对齐
2. **再改 `ExprGen.cpp:125`**：`->push(` → `->append(`
3. **更新 Aura 测试用例**（`example/` 和 `plan/` 中的 `.aura` 文件）：
   - 搜索 `push(` → 替换为 `append(`
4. **重新编译验证**：`cmake --build build`

---

## 五、Aura 用户侧影响

| 旧语法 | 新语法 | 兼容性 |
|--------|--------|:---:|
| `list.push(x)` | `list.append(x)` | ❌ 破坏性变更 |
| `list.pop()` | `list.pop()` | ✅ |
| `list.pop(i)` | `list.pop(i)` | ✅ |
| `list.len()` | `list.len()` | ✅ |
| `for x in list` | `for x in list` | ✅ |

> 注：如果希望向后兼容，可在 `BuiltinMethods.h` 中同时保留 `push` 和 `append`，并在 `genMethodCall` 中添加方法名映射。当前建议使用单一名称 `append` 以保持简洁。
