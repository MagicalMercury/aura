# Aura `enum` 语法与语义（初稿 v0.1）

> 本文档独立于 `#` 元数据体系，是 enum 类型的规范草案。

---

## 1. 语法

### 1.1 基本形式

```aura
enum type <名字> [<类型绑定>] = {
    <variant>,
    <variant>,
    ...
}
```

- `enum type` 是关键字组合——固定。
- `<名字>` 是 enum 类型名。
- `[<类型绑定>]` 可选——`<具体类型>` 形式；泛型绑定（`<T>`）**暂不做**。
- `<variant>` 每个变体一行，逗号分隔。

### 1.2 三种形态

**形态 1：无绑定（纯 tag）**

```aura
enum type Color = {
    Red,
    Green,
    Blue
}
```

- variant 只有名字——无关联数据。
- 退化为整数 tag。

**形态 2：绑定具体类型**

```aura
enum type ErrorCode<string> = {
    not_found = "NOT_FOUND",
    permission = "PERMISSION"
}
```

- 每个 variant 必须写 `= <值>`——值类型必须匹配绑定的类型。
- 值在定义处给定——**固定**。

**形态 3：泛型绑定**

```aura
enum type Option<T> = {
    Some(T),
    None
}
```

- **暂不做**——v1 不实现。
- 语法占位——`<T>` 是类型参数，variant 可用 `(T)` 构造函数带 payload。

### 1.3 variant 的写法

| 形态 | variant 写法 |
| :--- | :--- |
| 无绑定 | `Red` / `Green` / `Blue` |
| 绑定具体类型 | `not_found = "NOT_FOUND"` |
| 泛型绑定（暂不做） | `Some(T)` / `None` |

**约束**：

- 同一 enum 内 variant 名**唯一**。
- 形态 1 的 variant **不能写 `= 值`**。
- 形态 2 的 variant **必须写 `= 值`**，且值类型必须匹配绑定类型。

---

## 2. 语义

### 2.1 enum 值

```aura
let c = Color.Red
let e = ErrorCode.not_found
```

- `EnumName.VariantName` 是 **enum 值的构造语法**。
- 每个 variant 是一个**单例**——不管在哪里访问，都是同一个值。

### 2.2 单例语义

```aura
let p1 = Wrapper.active
let p2 = Wrapper.active
// p1 == p2    ✅ 同一个单例
```

- variant 关联的值（形态 2）在**首次访问时构造**——之后永远是同一个。
- 单例挂在全局根上——GC 不会回收。
- 零成本：形态 1 的 variant 退化为整数，不涉及单例构造。

### 2.3 深只读

**enum 的所有 variant 关联的值都是深只读的**——这是 enum 的属性。

```aura
enum type Wrapper<Player> = {
    active = Player{ name = "A", inventory = [sword, shield] }
}

let p = Wrapper.active
p.hp = 50                          // ❌ 编译错误
p.inventory[0] = new_sword         // ❌ 编译错误（深只读——嵌套的也不可改）
```

- **编译期报错**——静态检查。
- 整个对象图不可变——不只是单例本身，嵌套的 record / 数组也不可改。
- 无需用户写 `const`——enum 定义本身赋予了只读属性。

### 2.4 类型绑定（形态 2）

```aura
enum type ErrorCode<string> = {
    not_found = "NOT_FOUND",
    permission = "PERMISSION"
}
```

- enum 类型本身有一个绑定类型（这里是 `string`）。
- 每个 variant 关联一个**该类型的值**。
- 值在定义处给定——**编译期常量**（字面量 / 枚举名 / record 构造 / …）。
- 值的「单例」性质由 2.2 保证。

**注意**：绑定类型可以是 record / string / int 等——但值必须是编译期可构造的。

### 2.5 与 `Optional<T>` 的关系

`Optional<T>` 可以重定义为：

```aura
enum type Optional<T> = {
    Some(T),
    None
}
```

但**不强制**——`Optional` 可以保留特殊地位（`T?` 语法糖 + 性能优化），语义上等价于这个 enum。

**v1 行为**：`Optional` 保留现状，不改成 enum。

---

## 3. 底层布局（实现参考）

### 3.1 形态 1：纯 tag

```cpp
enum class Color : uint8_t { Red, Green, Blue };
```

- 退化为整数。
- tag 大小按 variant 数选：≤ 256 用 `uint8_t`。
- **不是 GC 对象**。

### 3.2 形态 2：绑定具体类型

```cpp
enum class ErrorCode_tag : uint8_t { not_found, permission };

// 值查静态表（编译期静态数据）
const char* ErrorCode_value(ErrorCode_tag t) { ... }
```

- enum 值 = tag（1 字节）。
- 值不内联——查静态表。
- 如果值是 record 实例 ⇒ **懒构造 + 全局根**（首次访问构造，之后同一）。

### 3.3 形态 3：泛型 tagged union（暂不做）

```cpp
template<typename T>
struct Option {
    uint8_t tag;
    union { T Some_value; };
};
```

---

## 4. GC 集成

| 形态 | 是否 GC 对象 |
| :--- | :--- |
| 形态 1（纯 tag） | ❌ 不是 |
| 形态 2（值查表） | 取决于值类型——值含 GC 指针则单例是 GC 对象 |
| 形态 3（泛型 tagged union） | 暂不做 |

- **单例的 GC 安全**：单例挂全局根——GC 不回收。
- **只读**：GC 不涉及「只读」——只读是编译期概念。

---

## 5. 与 reflect 的集成

### 5.1 `reflect.TypeKind` 加 `enum`

```aura
enum reflect.TypeKind {
    prim, string, array, optional, union, iterator,
    record, interface, generic_instance, fun,
    enum                                // ← 新增
}
```

### 5.2 `reflect.Type` 加 `variants()` 方法

```aura
fun (self reflect.Type) variants() -> [reflect.VariantSpec] ...

type reflect.VariantSpec = {
    name: string,
    value: Optional[reflect.Any],       // 形态 1 为 None；形态 2 为关联值
}
```

### 5.3 示例

```aura
#const T = #reflect.type("Color")
for v in T.variants() {
    io.println(v.name)                  // "Red" / "Green" / "Blue"
}
```

---

## 6. 与 `#` 体系的关系

enum 是**运行期类型**——不是 `#type`。

- `#type` 是**编译期元数据 schema**——不分配 GC、不进 desc。
- `enum` 是**运行期类型**——有 GC 布局、进 desc、参与运行期类型系统。

**两者是独立的**。

但 enum **可以被 `#type` 引用**：

```aura
#type EventConfig = {
    level: LogLevel,                    // LogLevel 是 enum
    priority: int
}
```

- `LogLevel` 作为字段类型——合法。
- 字段值必须是编译期常量——enum variant 名是编译期常量。

---

## 7. 示例集

```aura
// ── 形态 1：纯 tag ────────────────────────────────
enum type Color = { Red, Green, Blue }

fun (self Color) to_hex() -> string {
    match self {
        Red   => "#FF0000",
        Green => "#00FF00",
        Blue  => "#0000FF",
    }
}

// ── 形态 2：绑定具体类型 ──────────────────────────
enum type ErrorCode<string> = {
    not_found  = "NOT_FOUND",
    permission = "PERMISSION",
    timeout    = "TIMEOUT"
}

let e = ErrorCode.not_found
// e 的值是 "NOT_FOUND"——通过某种访问方式（待定）

// ── 作为 #type 字段类型 ───────────────────────────
#type LogEntry = {
    level: LogLevel = LogLevel.Info,
    message: string
}
```

---

## 8. 待裁定

| # | 项 | 状态 |
| :-: | :--- | :--- |
| 1 | **形态 2 的值访问语法**——`e.value()` / `e.to_string()` / 隐式转换？ | **待定** |
| 2 | **enum 的方法定义**——`fun (self Color) m(...)` 是否允许？ | **待定**（示例假设允许） |
| 3 | **`match` 语法**——示例用了 `match`，但 match 本身没定义 | **待定** |
| 4 | **泛型绑定**——`enum type Option<T> = { Some(T), None }` | **v1 不做** |
| 5 | **variant 名与类型名冲突**——`enum type Foo = { Foo }` 合法吗？ | **待定** |
| 6 | **空 enum**——`enum type E = { }` 合法吗？ | **待定** |
| 7 | **enum 的 `==` / hash**——自动生成还是用户定义？ | **待定** |
| 8 | **与 `Optional<T>` 的统一**——v1 保留 Optional 特殊地位 | **已定**（v1 不改） |
| 9 | **`reflect.VariantSpec.value` 的类型**——`Optional[reflect.Any]` 是否合适？ | **待定** |

---

**当前状态**：`2026-10-13` enum 初稿 v0.1。待主人审后定稿。