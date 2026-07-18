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

关键字：`fun, let, const, throws, throw, try, catch, match, if, else, for, while, loop, break, continue, spawn, sync, return, import, type, interface, true, false, None, impl, as`

```aura
// --- 合法标识符 ---
let myVar = 42
let _private = "hidden"
let snake_case_123 = 3.14

// --- 关键字不能用作标识符 ---
// let fun = 1      // 错误：fun 是关键字
// let let = 2      // 错误：let 是关键字
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

1. 一元 `-` `not`
2. `*` `/` `%`
3. `+` `-`
4. `<` `<=` `>` `>=`
5. `==` `!=`
6. `and`
7. `or`
8. 错误传播后缀：`!`

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
```
