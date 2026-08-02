# 4. 变量与常量

- `let` 声明可变变量，类型通常可省略，由初始化表达式推断。
- `const` 声明不可变绑定（绑定本身不可改，但引用类型的字段可修改）。

```aura
let x = 10               // 推断为 int
const pi = 3.14          // 推断为 float
let name: string = "Aura"
x = 20                   // 允许
// pi = 3.0              // 错误：const 绑定不可重新赋值
```

**空列表必须标注元素类型**（编译器无法从 `[]` 推断）：

```aura
let a: [int] = []        // 必须标注
// let a = []            // 编译错误：无法推断元素类型
let b = [1, 2, 3]        // 合法：从元素推断
```

嵌套同理：`let m: [[int]] = [[]]`。

对于记录：

```aura
const p = Point{x=1, y=2}
p.x = 5                  // 允许：修改字段
// p = Point{x=3, y=4}   // 错误：绑定不可重新赋值
```

对于泛型闭包（其类型参数来自隐式引入，见 §6.2.5），`let` 绑定的类型参数**推迟到闭包被调用时推断**——同一闭包可在不同调用点实例化为不同具体类型：

```aura
let mapper = make_mapper()               // mapper: Mapper<T, U>（多态闭包）
let d = mapper([1,2], fun(x) { x*2 })    // 此处 T=int, U=int
let l = mapper(["a"], fun(s) { s.len() }) // 此处 T=string, U=int
```
