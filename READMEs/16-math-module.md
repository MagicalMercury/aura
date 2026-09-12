# 16. `math` 内置模块

`math` 是内置模块，提供纯函数数学运算，无副作用、无 throws。导入方式为 `import math`（无引号）。

```aura
import math
```

## 16.1 绝对值

```aura
// --- math.abs(x: float) -> float ---
// 返回 x 的绝对值；int 实参自动提升为 float
let a = math.abs(-3.5)    // 3.5
let b = math.abs(-7)      // 7.0
```

## 16.2 平方根

```aura
// --- math.sqrt(x: float) -> float ---
// 返回 x 的平方根；x < 0 时返回 NaN
let s = math.sqrt(16.0)   // 4.0
```

## 16.3 取整

```aura
// --- math.floor(x: float) -> float ---
// 向下取整（向 -∞）
let f = math.floor(2.7)   // 2.0

// --- math.ceil(x: float) -> float ---
// 向上取整（向 +∞）
let c = math.ceil(2.1)    // 3.0

// --- math.round(x: float) -> float ---
// 四舍五入（.5 远离零）
let r = math.round(2.5)   // 3.0
```

## 16.4 幂与指数

```aura
// --- math.pow(x: float, y: float) -> float ---
// 返回 x 的 y 次幂
let p = math.pow(2.0, 10.0)   // 1024.0

// --- math.exp(x: float) -> float ---
// 返回 e 的 x 次幂
let e = math.exp(0.0)     // 1.0
```

## 16.5 对数

```aura
// --- math.log(x: float) -> float ---
// 返回 x 的自然对数；x <= 0 时返回 -inf / NaN
let l = math.log(1.0)     // 0.0
```

> 注：所有函数参数均为 `float`；传 `int` 实参时自动提升（与语言级数值提升规则一致）。
