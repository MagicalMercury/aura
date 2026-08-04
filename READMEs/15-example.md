# 15. 完整示例

```aura
import path

type User = { id: int, name: string }

// Stringer 是内置接口（builtins/interfaces.aurai，始终加载），无需重新声明
// 直接显式 impl 即可：str(user) 自动调用 to_string()

fun (self User) User(id: int, name: string) {
    self.id = id
    self.name = name
}

fun (self User impl Stringer) to_string() -> string {
    return "User(" + self.id + ", " + self.name + ")"
}

// 泛型记录 Pair
type Pair<A, B> = { first: A, second: B }

fun zip(a: <A>, b: <B>) -> Pair<A, B> {
    return { first = a, second = b }
}

// 泛型容器 Stack
type Stack<T> = { items: [T], top: int }

fun (self Stack<T>) Stack() {
    self.items = []
    self.top = -1
}

fun push(s: Stack<T>, value: T) {
    s.items[s.top + 1] = value
    s.top = s.top + 1
}

fun pop(s: Stack<T>) throws -> T {
    if s.top < 0 {
        throw { kind = "empty_stack", message = "cannot pop from empty stack" }
    }
    let val = s.items[s.top]
    s.top = s.top - 1
    return val
}

// 泛型闭包工厂
type Mapper<T, U> = fun([T], fun(T) -> U) -> [U]

fun make_mapper() -> Mapper<T, U> {
    return fun(items: [T], transform: fun(T) -> U) -> [U] {
        let result: [U] = []
        for item in items {
            result.append(transform(item))
        }
        return result
    }
}

// 闭包示例
fun make_greeter(prefix: string) -> fun(string) -> None {
    return fun(name: string) -> None {
        io.println(prefix + " " + name + "!")
    }
}

// Iterator 示例：record 实现 next() 即可 for-in
type Countdown = { n: int }

fun (self Countdown impl Iterator<int>) next() -> Optional<int> {
    if self.n <= 0 { return none() }
    self.n = self.n - 1
    return some(self.n)
}

fun main(io: Io) throws {
    // 路径演示
    let p = path.join(path.new("config"), "app.toml")
    io.println("Config path: " + p.to_string())

    // 泛型示例：Pair
    let pair = zip(42, "hello")
    io.println("Pair: (" + pair.first + ", " + pair.second + ")")

    // 泛型示例：Stack
    let stack = Stack()
    push(stack, 10)
    push(stack, 20)
    io.println("Stack top: " + pop(stack)!)   // 20

    // 闭包使用
    let greeter = make_greeter("Hello")
    greeter("Aura")   // 输出 "Hello Aura!"

    // 泛型闭包使用
    let mapper = make_mapper()
    let nums = [1, 2, 3]
    let doubled = mapper(nums, fun(x: int) -> int { return x * 2 })
    io.println("Doubled: " + doubled[0] + ", " + doubled[1] + ", " + doubled[2])

    // Iterator 惰性链：map 返回新迭代器，collect 收集为数组
    let squares = range(5).map(fun(x: int) -> int { return x * x }).collect()
    io.println("Squares: " + squares[0] + "..." + squares[4])   // 0...16

    // Iterator for-in：record 实现 next() 后直接遍历
    let cd: Countdown = { n = 3 }
    for v in cd {
        io.println("cd " + str(v))    // cd 2 / cd 1 / cd 0
    }

    io.println("Done.")
}
```
