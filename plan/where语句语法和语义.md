# `where` / `#where` 规范（草案 v0.4）

> 本草案整合本轮全部决策。取代 v0.3.2 §5 与 v0.3.3。

---

## 1. 总则：两套约束，两个关键字

| 关键字 | 约束什么 | 出现位置 | 检查时机 |
| :--- | :--- | :--- | :--- |
| `where` | 声明的**类型参数** | 有类型参数的声明 | 实例化时 |
| `#where` | 声明的**挂载点** | `#type` / 返回 `Code` 的 `#fun` | 挂载时 |

**为什么要分两套**：`T`（类型参数）和 `fun`（挂载类别）**本质不同**——前者是「值」，后者是「分类器」。塞进同一个 `:` 语法，怎么解释都别扭。分开后，两边各自的谓词集合完全不重叠，混乱消失。

**位置规律**：两者都在「**体之前**」——`where` 在 `#type` 上先于 `#where`；两者都在 `=` / `{` 之前。

```aura
#type Container<T> where { T impl Comparable } #where { method impl Tickable } = { value: T }
```

---

## 2. `where`（泛型侧）

### 2.1 能挂在哪

所有**有类型参数**的声明：`fun` / 方法 / `#fun` / `type` / `interface` / `#type`。

### 2.2 目标

| 目标 | 含义 |
| :--- | :--- |
| `T` / `U` / … | 声明引入的**类型参数** |
| `Self` | 方法 / 接口方法的**所属类型** |
| 具体类型名（`Point`） | ❌ 不合法 |

`where { T }` 合法——语义「T 已被引入」，说了等于没说，不报错。

### 2.3 界

只有三种：

| 界 | 形态 |
| :--- | :--- |
| `impl I` | 接口约束（`I` 是接口名，可带实参 `impl Iterator<int>`） |
| `by #f` | 自定义谓词（`f` 是 `#fun` 名） |
| `{ ... }` | 多界包 |

```aura
where { T impl Comparable }
where { T by #is_hashable }
where { T : {impl Comparable, by #is_hashable} }
```

### 2.4 连接词与逗号

- **接口界必须写 `impl`**——`T : Comparable` 非法。
- **介词出现时 `:` 可省略**：`T impl I` ≡ `T : impl I`。
- **顶层逗号 = AND**（Rust 风格）——多个类型参数各写一条，都要满足。
- **同一类型参数多约束**：用 `{ ... }` 包（包内逗号 = AND），不能重复写 `T`。

```aura
where {
    T impl Comparable,
    U : {impl Stringer, by #is_hashable},
}
```

### 2.5 `by #f` 的签名

```aura
#fun is_hashable(t: #reflect.Type) -> bool { ... }
```

收 `#reflect.Type`（类型反射视图），返回 `bool`。

### 2.6 检查时机

**实例化时**——参数位引入的：调用点推断出实参后检查；隐式引入的：返回的闭包被调用时检查。**与调用语法无关**：调用方仍不写尖括号。

---

## 3. `#where`（挂载侧）

### 3.1 能挂在哪

| 声明 | 能挂？ |
| :--- | :--- |
| `#type` | ✅——定义元数据类型的挂载约束 |
| 返回 `Code` 的 `#fun` | ✅——注解器能被应用在哪 |
| 非 `Code` 的 `#fun` | ❌——不被挂载，`#where` 无意义 |
| 其它（`fun` / `type` / `interface`） | ❌——不是可挂载的元数据实体 |

### 3.2 目标（挂载类别）

只有四个：

```aura
#where { fun, method, type, interface }
```

**`#fun` / `#type` 不在其中**。

### 3.3 界

只有五种：

| 界 | 形态 | 说明 |
| :--- | :--- | :--- |
| `throws` | 裸关键字 | 目标函数必须 throws |
| `impl I` | 介词 + 接口名 | 目标加载接口 I |
| `Self impl I` | 复合界 | 目标的**所属类型**加载 I（仅 `method` 适用） |
| `by #f` | 介词 + 函数引用 | 目标满足自定义谓词 |
| `{ ... }` | 多界包 | 包内逗号 = AND，不嵌套 |

**已移除的界**（改由 `by #fun` + 反射手动实现）：`params <= N`、`methods <= N`、`has_method(...)`、`fields { ... }`、`field(...)`、`coroutine`、`public`。

### 3.4 连接词与逗号

- **介词出现时 `:` 可省略**：`method impl I` ≡ `method : impl I`；`method by #f` ≡ `method : by #f`。
- **`Self` 不是介词**——`Self impl I` 是复合界，`:` **不可省**：`method : Self impl I`。
- **顶层逗号 = OR**——挂载类别互斥，白名单条目之间选一个。
- **同一目标多约束**：用 `{ ... }` 包，不能重复写；同名目标出现两次报错。
- **方括号 `[]` 不再需要**——顶层逗号已是 OR。

```aura
#where { fun, method : throws }                       // 允许 fun，或（method 且 throws）
#where { method : {throws, impl Tickable} }           // 只允许 method，且满足两条界
#where { method : Self impl Tickable }                // 允许 method，且所属类型加载 Tickable
#where { fun, method : Self impl Tickable }           // ❌ 若 fun 不需要 Self 约束——但同一条 where 里 fun 无附加条件
```

### 3.5 `by #f` 的签名

```aura
#fun small_params(t: #reflect.Target) -> bool { ... }
```

收 `#reflect.Target`（挂载点的反射视图），返回 `bool`。

**与泛型侧签名不同**——泛型侧收 `#reflect.Type`，挂载侧收 `#reflect.Target`。

### 3.6 检查时机

**挂载时**——元数据被贴到声明上时检查。

---

## 4. 两个关键字的并存

`#type` 上可以同时有两者：

```aura
#type Container<T>
where  { T impl Comparable }
#where { method impl Tickable }
= { value: T }
```

**顺序固定**：`where` 在前，`#where` 在后。

---

## 5. `fun` / `method` / `Self` / `T` 的建模

**统一建模为「编译期反射值」**——但这个建模和 reflect 库的实现深度耦合，**细节待 reflect 库设计好后再定**。

当前仅确认：

- `fun` / `method` / `type` / `interface` / `Self`：**编译器预定义、始终加载**（不需要 `import`）。
- `T` / `U`：**声明引入**。
- 挂载检查时，它们「变成对应的东西」——`fun` 变成「当前挂载点是 fun」，`Self` 变成「当前方法的所属类型」。

**实现顺序**：初步 `#` 语法 → reflect 库 → 再决定 `where` 相关的最终建模。

---

## 6. 完整例集

```aura
// ── 泛型侧 ─────────────────────────────────────────
fun max(a: <T>, b: <T>) -> T
where { T impl Comparable }
{ if a.cmp(b) > 0 { a } else { b } }

fun report(a: <A>, b: <B>) -> string
where {
    A : {impl Stringer, by #is_short},
    B impl Stringer,
}
{ ... }

// ── 挂载侧 ─────────────────────────────────────────
#type TickHook
#where { method : Self impl Tickable }
= { priority: int = 0 }

fun (self Player impl Tickable) tick() -> None { ... }    // ✅
#TickHook{ priority = 1 }
fun (self Player) ping() -> None { ... }                  // ❌ Self 未加载 Tickable

#type Guarded
#where { method : {throws, by #small_params} }
= { }

#type EventRegister
#where { [改为] fun, method }                              // 顶层逗号 = OR
= { event: string, priority: int = 0 }

// ── 并存 ──────────────────────────────────────────
#type Container<T>
where  { T impl Comparable }
#where { method impl Tickable }
= { value: T }
```

---

## 7. 待裁定

| # | 项 | 状态 |
| :-: | :--- | :--- |
| 1 | `fun` / `method` / `Self` 在 reflect 库中的**预定义建模**（同一类型？各自独立？`#Reflect` 单例成员？） | **待 reflect 库设计** |
| 2 | `by #f` 的 `#f` 在泛型侧 / 挂载侧的完整签名（是否只有 `#reflect.Type` / `#reflect.Target` 一个参数？返回值只能是 `bool`？） | **待 reflect 库设计** |
| 3 | `#type` 能否接入接口 | **待裁定**（v0.3.2 §12 #5） |
| 4 | `#interface` 定位 | **待裁定**（v0.3.2 §12 #6） |

---

**当前状态**：`2026-10-12` —— where 重设计草案。待主人审后并入 v0.4。