# 2. 词法基础

## 2.1 注释

```aura
// 单行注释

/*
  多行注释
  可以跨行
*/
```

## 2.2 标识符与关键字

标识符以字母或下划线开头，后可跟字母、数字、下划线，区分大小写。

关键字：`fun, let, const, throws, throw, try, catch, match, if, else, for, while, loop, break, continue, spawn, sync, return, import, type, interface, true, false, None, impl`

软关键字（上下文关键字）：`module, as, lock, thread, enum, where`。它们**不是**关键字——词法上仍是普通标识符，
仅在特定位置由 Parser 按上下文识别（`module <name>` 只能在文件首；`enum` 只有在后随硬关键字 `type`
时才构成 `enum type Name = { Variant, ... }` 声明；由于 Aura **换行不参与语法**（无 `Newline` token），
该组合**分处两行**亦同样成立）。因此它们**可以**继续用作变量名、参数名、字段名与方法名：
`let enum = 1`、`fun f(enum: int)`、`x.enum` 均合法。

例如 `enum` 与 `type Color = { Red, Green }` **分开写在两行**时，其 token 序列与写成同一行
`enum type Color = { Red, Green }` 完全相同 ⇒ 解析为**同一个**声明（**不必**挤在同一行）；
反之 `enum` 后面不是 `type` 时（如 `let enum = 1`、`x.enum`）**不**构成 enum 声明，`enum` 仍是普通标识符。

```aura
// --- 合法标识符 ---
let myVar = 42
let _private = "hidden"
let snake_case_123 = 3.14

// --- 关键字不能用作标识符 ---
// let fun = 1      // 错误：fun 是关键字
// let let = 2      // 错误：let 是关键字
```

### 编译期元数据前缀 `#`

- `#type` / `#fun` / `#const`：声明编译期类型、编译期函数与编译期常量。
- `#X{...}`：元数据构造，紧跟一条声明即挂载到该声明上（出现在表达式或 `#const` 初值处则为独立值）。
- `#(表达式)` / `#{ ... }`：编译期求值——`#(...)` 等价于 `#{ return 表达式 }`；`#{...}` 是多语句块，**必须显式 `return`**（唯一例外是 `#(...)` 这个语法糖）。
- `Aura { ... }`：引述，记录块内代码的 **AST**（可含多行语句与声明）供注解器使用；**不执行、不触发 import 解析**，引述内的 `import` 只是 AST 数据。

旧的 `#ns.key = value` 指令已移除；原 `#io.sync = true` 的迁移写法是 `#io{ sync = true }`（运行期接线在后续 Phase）。
权威规范：`issues/features/feature-21-compile-time-metadata-system.md`（附录 A 为 v0.4.1 手稿）；
使用手册见 [17. 编译期与元数据](17-compile-time.md)。

```aura
#type Point = { x: int, y: int }
let n = #(1 + 2)              // 编译期求值 → 3
```

## 2.3 字面量

```aura
42                  // 整数
0xFF                // 十六进制
3.14                // 浮点数
"hello"             // 字符串（转义：\n \t \\ \"）
true, false         // 布尔
None                // 空值（用于联合类型，也表示无返回值）
[1, 2, 3]           // 列表
{ x = 1, y = 2 }    // 记录（无构造函数时可用）
fun (a: int, b: int) -> int { return a + b }   // 闭包字面量
```

## 2.4 运算符优先级（由高到低）

1. 后缀：`!`（错误传播）；一元 `-` `not`
2. `*` `/` `%`
3. `+` `-`
4. `<` `<=` `>` `>=`
5. `==` `!=`
6. `and`
7. `or`
8. 管道：`|>`
9. 三元条件：`?:`（右结合；then 分支可含完整表达式）
10. 赋值：`=` `+=` `-=` `*=` `/=` `%=`（右结合；复合赋值等价脱糖为 `x = x op v`）

```aura
// --- 演示：not 优先级高于 and ---
let a = true
let b = false
let r1 = not a and b     // 解析为 (not a) and b → false and false → false
let r2 = not (a and b)   // 括号改变优先级 → not false → true

// --- 演示：算术优先级 ---
let x = 2 + 3 * 4        // 3 * 4 先算 → 2 + 12 → 14
let y = (2 + 3) * 4      // 括号先算 → 5 * 4 → 20

// --- 演示：比较 + 逻辑 ---
let score = 85
let good = score >= 60 and score <= 100   // (score >= 60) and (score <= 100)

// --- 演示：三元条件 + 复合赋值 ---
let sign = x < 0 ? -1 : 1  // 优先级低于 or：解析为 (x < 0) ? (-1) : 1
let acc = 10
acc += 3                   // acc = acc + 3 → 13
```
