# `range()` — 迭代器设计

> 日期：2026-07-18
> 状态：基本完成
> 替代文档：`plan/range_implementation.md`（已废弃）

---

## 1. 设计

```aura
for i in range(5) {          // Iter<int>: i = 0, 1, 2, 3, 4
    io.println(i)
}

for i in range(1, 10) {      // Iter<int>: i = 1, 2, ..., 9
    io.println(i)
}

for i in range(1, 10, 2) {   // Iter<int>: i = 1, 3, 5, 7, 9
    io.println(i)
}
```

**类型模型**：

```cpp
// SemType.h — 替代旧 RangeSemType
struct IterSemType : SemType {
    std::unique_ptr<SemType> elementType;  // 迭代器产出的元素类型
};
```

- `range()` 返回 `IterSemType(elementType = int)`
- 未来 `items.iter()` 返回 `IterSemType(elementType = T)`
- 元素类型是泛型，不是硬编码 `int`

**类型名称**：`Iter<T>` 而非 `Range<T>`。`range` 是函数名，`Iter` 是类型名，职责清晰。

---

## 2. 运行时：零新增

- `range(n)` / `range(s, e)` → C++ 已有 `std::views::iota(start, end)`
- `range(s, e, step)` → 手写 `for (auto i = s; i < e; i += step)`
- 头文件：`runtime/aura_rt.h` 中已有 `<ranges>`（或按需 `#include`）

---

## 3. 实施步骤

### Step 1: `SemType.h` — 新增 `IterSemType`

```cpp
struct IterSemType : SemType {
    std::unique_ptr<SemType> elementType;
    [[nodiscard]] bool equals(const SemType& other) const override;
    [[nodiscard]] std::string toString() const override {
        return "Iter<" + (elementType ? elementType->toString() : "?") + ">";
    }
    [[nodiscard]] std::unique_ptr<SemType> clone() const override;
    static std::unique_ptr<IterSemType> make(std::unique_ptr<SemType> el);
};
```

注意：需在相关虚函数（`sameBaseKind`、`clone` 等分发链）中加 `IterSemType` 分支。

### Step 2: `ExprInfer.cpp` — `inferCall` 处理 `range`

在 `BuiltinRegistry::get().findFunction("range", n)` 查到 range 后，返回 `IterSemType`：

```cpp
case ReturnTypeInfo::Kind::Generator:
    return IterSemType::make(intType());  // range 元素恒为 int
```

删除旧注释 "Phase 3 实现 RangeSemType 返回"。

### Step 3: `StmtChecker.cpp` — `checkForStmt` 处理 `IterSemType`

```cpp
if (auto* iter = dynamic_cast<const IterSemType*>(iterableType.get())) {
    // 循环变量类型 = 迭代器元素类型
    auto loopVarType = iter->elementType ? iter->elementType->clone() : ErrorSemType::make();
    Symbol sym;
    sym.kind = SymKind::Variable;
    sym.name = stmt.varName;
    sym.type = std::move(loopVarType);
    symtab_.define(std::move(sym));
}
```

### Step 4: `StmtGen.cpp` — `genForStmt` 展开 range

检测 `IterSemType` 的 iterable → 展开为 C++ iota 循环：

```cpp
if (auto* iter = dynamic_cast<const IterSemType*>(inferred.get())) {
    if (auto* call = dynamic_cast<const CallExpr*>(stmt.iterable.get())) {
        auto* id = dynamic_cast<const Identifier*>(call->callee.get());
        if (id && id->name == "range") {
            // range(...) → iota / step loop
            std::string start = call->args.size() >= 2 ? genExpr(*call->args[0]) : "0";
            std::string end   = genExpr(*call->args[call->args.size() >= 2 ? 1 : 0]);
            std::string var   = safeName(stmt.varName);
            if (call->args.size() == 3) {
                std::string step = genExpr(*call->args[2]);
                cpp << "for (auto " << var << " = " << start
                    << "; " << var << " < " << end
                    << "; " << var << " += " << step << ") ";
            } else {
                cpp << "for (auto " << var << " : std::views::iota("
                    << start << ", " << end << ")) ";
            }
            genBlock(cpp, *stmt.body);
            return;
        }
    }
}
```

### Step 5: `runtime/aura_rt.h` — 加 `<ranges>` include

```cpp
#include <ranges>
```

---

## 4. 改动清单

| 文件 | 改动 | 量 |
|------|------|:---:|
| `src/Sema/SemType.h` | 新增 `IterSemType` 结构体；相关分发函数加分支 | ~15 行 |
| `src/Sema/Checker/ExprInfer.cpp` | `inferCall` 中 range 返回 `IterSemType` | ~2 行 |
| `src/Sema/Checker/StmtChecker.cpp` | `checkForStmt` 加 `IterSemType` 分支 | ~8 行 |
| `src/CodeGen/StmtGen.cpp` | `genForStmt` 加 range → iota 展开 | ~25 行 |
| `runtime/aura_rt.h` | `#include <ranges>` | 1 行 |

---

## 5. 不做的

- ❌ 数组 `.iter()` — 后续单独做
- ❌ 链式 `map` / `filter` 迭代器 — 后续单独做
- ❌ `Iter<T>` 的 C++ 运行时类型 — 用 `std::views::iota` 即可，不包装
- ❌ range 中元素类型推导（如 `range(1.0, 5.0)` → `Iter<float>`）— 短期只支持 int
