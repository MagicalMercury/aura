# 9. 模式匹配

`match` 用于联合类型，强制穷尽所有分支。

```aura
match expr {
    TypePattern => body,
    Constant    => body,
    _           => body
}
```

> `=>` 后可跟表达式或 `{ }` 块体。使用表达式时末尾需加逗号（`,`），使用块体时末尾逗号可选。

## 匹配联合类型

```aura
fun describe(val: int | string | None) -> string {
    match val {
        int n    => return "integer: " + n,
        string s => return "string: " + s,
        None     => return "none"
    }
}
```

## `T | None` 示例

```aura
fun safeDivide(a: int, b: int) -> int | None {
    if b == 0 { return None }
    return a / b
}

match safeDivide(10, 2) {
    int result => io.println("Result: " + result),
    None       => io.println("Division by zero")
}
```
