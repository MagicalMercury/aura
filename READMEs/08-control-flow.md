# 8. 控制流

## 8.1 条件

```aura
if score >= 90 {
    io.println("A")
} else if score >= 60 {
    io.println("B")
} else {
    io.println("C")
}
```

## 8.2 循环

| 形式 | 语法 | 用途 |
|------|------|------|
| 条件循环 | `while cond { ... }` | 条件为真时重复执行 |
| 无限循环 | `loop { ... }` | 无限循环，用 `break` 退出 |
| 遍历迭代 | `for item in iterable { ... }` | 遍历列表、字符串等可迭代对象 |

```aura
// while — 条件循环
let i = 0
while i < 5 {
    io.println(i)
    i = i + 1
}

// loop — 无限循环
loop {
    if done { break }
}

// for-in — 列表遍历
let nums = [1, 2, 3]
for n in nums {
    io.println(n)
}

// for-in — range 整数迭代
for i in range(5) {
    io.println(i)   // 0 1 2 3 4
}

for i in range(2, 6) {
    io.println(i)   // 2 3 4 5
}

for i in range(0, 10, 2) {
    io.println(i)   // 0 2 4 6 8
}

// for-in — 通道遍历
for val in ch {
    io.println(val)
}

// for-in — Iterator 接口（含 record impl）
for v in fib {                 // fib: Fib impl Iterator<int>
    io.println(v)
}
for v in range(5).map(process) {   // 惰性链式迭代器
    io.println(v)
}

// for-in — 字符串遍历（字符级）
let s = "Aura"
for ch in s {
    io.println(ch)
}
```

`range()` 返回 `Iterator<int>` 泛型迭代器类型，支持 1/2/3 参数形式。`channel<T>` 支持 `for val in ch` 迭代接收。任何显式 `impl Iterator<T>` 的 record（实现 `next()`）及 `map`/`filter`/`from` 产生的迭代器都支持 `for-in` 遍历。
