# 15. 完整示例

```aura
import path

type User = { id: int, name: string }

interface Stringer {
    to_string() -> string
}

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

    io.println("Done.")
}
```
