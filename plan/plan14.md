# Plan 14: 分析器已知限制与后续改进

> 状态：待实现  
> 关联：`example/output.txt` 中剩余的 Semantic errors

---

## 一、当前 output.txt 剩余错误

```
  [line 88:12]  return type mismatch: expected '{ value: <T>, children: [error] }'
  [line 145:32] argument type mismatch: expected 'interface StringProcessor', got '(string) -> string throws'
  [line 152:13] list element type mismatch: expected '{ value: int, children: [error] }'
  [line 158:30] argument type mismatch: expected '{ value: <T>, children: [error] }'
```

均源于两个分析器未实现特性。

---

## 二、待实现特性

### 2.1 递归泛型类型推断（🔴 阻塞 tree 测试）

**问题**

```aura
type Tree<T> = { value: T, children: [Tree<T>] }
```

`Tree<T>` 的自引用导致 `inferExpr` 在推断 `children: [Tree<U>]` 时递归，元素类型退回 `{ value: <T>, children: [error] }`，使后续所有 Tree 操作都报类型不匹配。

**涉及错误**：L88, L152, L158

**原因**

当前类型推断在解析记录体字段时，遇到引用自身的 `ListType` 没有设置递归锚点。分析器需在 `SemType` 中为 `RecordSemType` / `ListSemType` 增加自引用 ID，遇到同一 ID 时直接返回类型变量而非重新推断。

**方案**

1. 在 `SemAnalyzer` 中添加 `std::set<std::string> resolvingTypes_`（正在解析中的类型名集合）
2. `inferRecordExpr` 遇到 `Tree<T>` 时，先向正在解析集合注册 `"Tree"`，再推断字段
3. `inferTypeExpr(ListType(Tree<T>))` 遇到 `Tree<T>` 且在 `resolvingTypes_` 中 → 返回类型变量而非重新推断
4. 推断结束后从集合中移除

**影响面**

| 文件 | 改动 |
|------|------|
| `Sema/SemAnalyzer.h` | 新增 `resolvingTypes_` 成员 |
| `Sema/Checker/ExprInfer.cpp` | `inferListExpr` / `inferRecordExpr` 加入递归检测 |
| `Sema/SemType.h` | 可选：给 `RecordSemType` 加 `canonicalName` 字段用于递归引用 |

**优先级**：高 — 阻塞任何递归记录类型（树、图、链表）

---

### 2.2 闭包到接口的结构匹配（🔴 阻塞 processor 测试）

**问题**

```aura
interface StringProcessor {
    process(s: string) throws -> string
}

let my_processor = fun(s: string) throws -> string { ... }
process_with_interface(io, my_processor, "Hello")
// 错误：expected 'interface StringProcessor', got '(string) -> string throws'
```

**涉及错误**：L145

**原因**

Aura 使用结构类型系统，`fun(string) throws -> string` 和 `{ process(s: string) throws -> string }` 应该在调用时自动匹配（只要有同名同签名方法即可）。但分析器当前不做此桥接，直接将闭包的 `FunctionType` 当作整体与接口名比较。

**方案**

在 `isAssignable` 或 `inferCall` 中加一条规则：当目标类型是 `InterfaceSemType` 且实参类型是 `FunctionSemType`，检查该闭包是否能"扮演"接口的单方法：

```
如果 Interface 只有一个方法 m(arg: A) -> R，
且闭包的签名是 fun(A) -> R（或兼容），
则视为可赋值。
```

**影响面**

| 文件 | 改动 |
|------|------|
| `Sema/SemAnalyzer.cpp` | `isAssignable` 加 `FunctionSemType` → `InterfaceSemType` 分支 |
| `Sema/SemType.h` | `InterfaceSemType` 加 `int methodCount()` 方法 |

**优先级**：高 — 阻塞闭包作为接口参数的所有场景

---

## 三、已修复问题回顾

上一轮修复后，以下错误已消除：

| 原错误 | 修复 |
|--------|------|
| L39 `string + int` | `inferBinaryExpr` 中字符串拼接检查提到算术之前 |
| L135 `string + int` | 同上 |
| L123 `compose` 闭包缺 `throws` | test.aura 中闭包加 `throws` |
| L173 `square`/`negate` 缺 `throws` | test.aura 中闭包加 `throws` |

---

## 四、讨论

- **2.1 递归泛型类型**：是否要全面支持 `type A<T> = { ..., children: [A<T>] }` 这种模式？如果是，`SemAnalyzer` 中 `type alias` 的解析也需要配合——解析别名时先注册占位符，再递归推断。这会涉及 `DeclChecker.cpp` 中 `genTypeAlias` 逻辑。
- **2.2 闭包→接口**：是否需要支持多方法接口的闭包适配？如果需要，那 `fun(string, string) throws -> string` 不可能同时满足 `process1` 和 `process2` 两个签名，只能在单方法接口处做桥接。
- 其他已知未实现：泛型闭包（plan12）、泛型函数作为一等值（partial specialization）——这些暂不在 plan14 范围内。
