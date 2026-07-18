## 闭包（Lambda / FunExpr）实现方案

> 状态：讨论中，未实装
> 参考：[README.md §2.3](file:///d:/you/Aura/README.md)（字面量语法）、[README.md §5.4](file:///d:/you/Aura/README.md)（闭包章节）

### 1. 目标语法（与 README 一致）

闭包值（表达式）：`fun (params) throws? -> ReturnType? { body }`
函数类型（类型标注）：`fun(params) -> Ret`

**关键区分**：闭包值的 `fun` 后有**空格** + `(`，函数类型无空格。

```aura
// 基础闭包 — fun 后有空格
let add = fun (x: int, y: int) -> int { return x + y }

// 无参闭包，返回类型省略 → 推断为 None
let greet = fun () { io.println("Hello") }

// 捕获外部变量
let prefix = ">>"
let printer = fun (msg: string) -> None {
    io.println(prefix + msg)   // 自动捕获 prefix
}

// 作为返回值（闭包生产函数）
fun make_handler(prefix: string) -> fun(string) -> None {
    return fun (msg: string) -> None {
        io.println(prefix + msg)
    }
}

// 作为参数传递（高阶函数）
fun apply(f: fun(int) -> int, x: int) -> int {
    return f(x)
}

// throws 闭包
fun make_lambda(io: Io) throws -> fun() throws -> None {
    return fun () throws {
        io.println("Inside lambda")
    }
}

// 类型标注中有类型 → 参数类型可从上下文推断（Phase 4）
let doubler: fun(int) -> int = fun (x) { return x * 2 }
```

### 2. 语法解析关键：上下文决定 `fun` 的含义

这是实现闭包的**第一个关键决策点**。`fun` 关键字在两种语法上下文出现：

| 上下文 | 当前处理 | 看到 `fun` + | 含义 |
|--------|---------|-------------|------|
| 声明级（`parseDecl`） | ✅ 已有 | `fun` + `(` | 方法声明 `parseMethodDecl()` |
| 声明级（`parseDecl`） | ✅ 已有 | `fun` + 标识符 | 函数声明 `parseFunDecl()` |
| 表达式级（`parsePrimary`） | ❌ 需新增 | `fun` + `(` | 闭包表达式 `parseFunExpr()` |

**关键点**：

- `fun` + `(` 在**声明级**表示方法 `fun (self T) methodName(...)`（现有 `parseDecl` 逻辑，见 [DeclParser.cpp L9](file:///d:/you/Aura/src/Parser/DeclParser.cpp#L9)）。
- `fun` + `(` 在**表达式级**表示闭包 `fun (x: int) -> int { ... }`（需新增的 `parsePrimary` 路由）。
- 这两个上下文**永不重叠**——表达式不能出现在顶层裸声明位置，方法的 `fun (self ...)` 不会出现在表达式位置。
- 因此 `parsePrimary()` 中看到 `fun` 后直接检查 `peek()` 是否为 `(` 即可，无需更深的 lookahead（不需要区分 `self`，因为表达式上下文不涉及方法声明）。

```cpp
// parsePrimary() 中新增：
if (check(TokType::Fun)) {
    return parseFunExpr();
}
```

`parseFunExpr()` 自己消费 `fun` token 后检查 `(` 并继续解析参数列表。

### 3. 现有基础设施（可直接复用）

| 组件 | 位置 | 说明 |
|------|------|------|
| `FuncSemType` | [SemType.h](file:///d:/you/Aura/src/Sema/SemType.h) L82-L91 | 语义层函数类型已存在，含 `paramTypes`/`returnType`/`throws` |
| `FunctionType` | [Type.h](file:///d:/you/Aura/src/AST/Type.h) L80-L97 | AST 层函数类型语法节点（用于标注，如 `fun(int) -> int`） |
| `mapType(FunctionType)` | [TypeMap.cpp](file:///d:/you/Aura/src/CodeGen/TypeMap.cpp) L77-L88 | 已将 `FunctionType` 映射为 `std::function<Ret(Args...)>` |
| `IdRefCollector` | [StmtGen.cpp](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp) L307-L482 | 收集 Stmt/Expr 子树中所有 Identifier 引用 |
| `DeclaredCollector` | [StmtGen.cpp](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp) L487-L525 | 收集局部变量声明 |
| `genSpawnStmt` | [StmtGen.cpp](file:///d:/you/Aura/src/CodeGen/StmtGen.cpp) L318-L388 | 已有 lambda 生成 + 捕获分析模式 |
| `ASTWalker` | [ASTWalker.h](file:///d:/you/Aura/src/ASTWalker.h) | 统一 AST 遍历框架 |

### 4. 实现步骤

#### 步骤 1：AST 节点 — `FunExpr`

**文件**: `src/AST/Expr.h`

在 `PipeExpr` 之后新增：

```cpp
// ============================================================
// FunExpr ─ 闭包表达式 (fun (params) throws? -> Ret? { body })
//
// README §2.3, §5.4: 闭包字面量，作为表达式使用。
// 与 FunDecl 的区别：FunExpr 是表达式（可出现在任何表达式位置），
// 没有名称，返回类型可省略（由推断决定），参数类型在特定条件下可省略。
// ============================================================
struct FunExpr : ASTNode {
    std::vector<Param> params;               // 参数列表（可为空；类型标注在特定条件下可选）
    bool throws = false;                      // 是否可能抛出
    std::unique_ptr<TypeExpr> returnType;     // 返回类型（可为空，由推断决定；无返回 → 推断为 None）
    std::unique_ptr<BlockStmt> body;          // 函数体
    void print(std::ostream& os, int indent) const override;
    [[nodiscard]] std::unique_ptr<ASTNode> clone() const override;
};
```

**注意**: `FunExpr` 继承 `ASTNode`（非 `Stmt`/`Decl`），因为它是一个表达式，可以出现在任何表达式位置。`Param` 复用现有的 `Stmt.h` 中定义。

同步更新 `ASTWalker.h` 中的 `ExprWalker`，添加 `FunExpr` 分支。

---

#### 步骤 2：Parser — 在表达式上下文中解析 `fun`

**关键改动**: `fun` 后的 token lookahead 决定解析路径。

**文件**: `src/Parser.h`

新增声明：
```cpp
std::unique_ptr<ASTNode> parseFunExpr();
```

**文件**: `src/Parser/ExprParser.cpp`

修改 `parsePrimary()`，在 `Identifier` 分支**之前**添加：
```cpp
// 闭包表达式: fun (params) -> Ret { body }
if (check(TokType::Fun)) {
    return parseFunExpr();
}
```

**注意位置**：放在 `identifier` 检查之前，因为 `fun` 是关键字，Lexer 不会生成 Identifier token。

**文件**: `src/Parser/ExprParser.cpp` — `parseFunExpr()` 完整实现：

```
fun                         → advance()
(                           → consume
[params] (parseParams)      → 复用 DeclParser 的参数解析（可选）
)                           → consume
[throws]                    → 可选
[-> returnType]             → 可选，调用 parseType()
{ ... }                     → parseBlock()
→ 构造 FunExpr{params, throws, returnType, body} 返回
```

**参数解析的特殊处理**：
闭包的参数类型标注是**可选的**。当前 `parseParams()` 要求每个参数有 `: Type`。需要新增一个 `parseFunExprParams()` 或在 `parseFunExpr` 内自己解析参数列表，允许 `name: Type` 或仅 `name`。

简化的 Phase 1 策略：先要求参数类型必须显式标注（与普通函数一致），Phase 4 再放开。

---

#### 步骤 3：Sema — 类型推断

**文件**: `src/Sema/SemAnalyzer.h`

新增声明：
```cpp
std::unique_ptr<SemType> inferFunExpr(const FunExpr& e);
```

**文件**: `src/Sema/Checker/ExprInfer.cpp`

在 `inferExpr()` 的 `dynamic_cast` 链中添加：
```cpp
if (auto* e = dynamic_cast<const FunExpr*>(&expr))
    return inferFunExpr(*e);
```

`inferFunExpr()` 逻辑：

```
1. 构建参数类型列表
   ├── 有显式标注 → 使用标注类型
   └── 无标注 → Phase 1 报错 "parameter type annotation required for closure"

2. 获取返回类型
   ├── 有显式 -> Ret 标注 → 使用标注类型
   ├── 无标注 → 从函数体推断
   │   ├── 分析所有 return 语句的类型
   │   ├── 无 return 语句 → FuncSemType 返回类型为 None
   │   └── 多个 return 有不同类型 → 联合类型或报错
   └── returnType = None → 推断为 NoneSemType

3. 检查函数体内部类型（递归 StmtChecker）
   ├── 推入新作用域（含参数符号）
   ├── 遍历 BlockStmt 逐条检查
   └── 弹出作用域

4. 构造并返回 FuncSemType{paramTypes, returnType, throws}
```

**类型标注策略（分阶段）**：

| 阶段 | 参数类型 | 返回类型 | 上下文推断 |
|------|---------|---------|-----------|
| Phase 1 | 必须显式标注 | 可由 body 推断，无 return → None | 不支持 |
| Phase 4 | 可省略（从标注推断） | 同 Phase 1 | 支持 `let f: fun(int)->int = fun (x) { ... }` |

---

#### 步骤 4：CodeGen — C++20 lambda 生成

这是工作量最大的一步。

**文件**: `src/CodeGen/CodeGen.h`

新增声明：
```cpp
[[nodiscard]] std::string genFunExpr(const FunExpr& e, bool isCoroutine);
```

**文件**: `src/CodeGen/ExprGen.cpp`

在 `genExpr()` 的 `dynamic_cast` 链中添加：
```cpp
if (auto* e = dynamic_cast<const FunExpr*>(&expr))
    return genFunExpr(*e, isCoroutine);
```

**`genFunExpr()` 核心逻辑**：

```
1. 捕获分析（复用 IdRefCollector + DeclaredCollector + 现有 genSpawnStmt 模式）
   ├── 收集闭包体内所有 Identifier 引用 → allRefs
   ├── 收集闭包体内局部声明 → declared
   ├── 收集闭包参数名 → params
   ├── 过滤：自由变量 = allRefs - declared - params - 内置(io, _tasks)
   └── 对每个自由变量判断类型
       ├── isValueType / isHeapType
       ├── 值类型（int/float/bool/Io）→ 按值捕获 [var]
       └── 堆类型（string/User）→ Phase 1 报错

2. 处理泛型类型变量（README §5.5）
   ├── 闭包自身不声明泛型参数（FunExpr 无 typeParams 字段）
   ├── 扫描闭包体内使用的泛型类型变量（GenericTypeRef name）
   ├── 从当前 currentTParams_ / 外层作用域中查找该类型变量
   ├── 若类型变量来自外层泛型函数 → 生成 C++ 模板 lambda：
   │   auto lambda = [cap...](auto&&... params) -> auto { ... }
   │   或显式 [cap...]<typename T>(T x, ...) { ... }（C++20 lambda 模板）
   └── 若类型变量无法解析 → 报错 "unresolved type variable T in closure"

3. 生成 C++ lambda：
   ├── 无泛型：  [cap1, cap2, ...](param1_type param1, ...) -> ret { body... }
   └── 有泛型：  [cap1, cap2, ...]<typename T>(T param1, ...) -> ret { body... }
                 （C++20 泛型 lambda，T 由外层模板函数实例化）

4. 返回类型：
   ├── 有标注 → 用 mapType(标注)
   ├── 无标注且无 return 语句 → auto
   └── 无标注但有 return → auto（让 C++ 编译器推导）
```

**C++ 代码示例**：

Aura 源码（README §13 闭包部分）：
```aura
fun make_greeter(prefix: string) -> fun(string) -> None {
    return fun (name: string) -> None {
        io.println(prefix + " " + name + "!")
    }
}
```

生成的 C++：
```cpp
std::function<void(aura_rt::GcString*)> make_greeter(aura_rt::GcString* prefix) {
    return [prefix](aura_rt::GcString* name) -> void {
        aura_rt::GcString* _t1 = aura_rt::concat(prefix, aura_rt::make_string(" "));
        aura_rt::GcString* _t2 = aura_rt::concat(_t1, name);
        aura_rt::GcString* _t3 = aura_rt::concat(_t2, aura_rt::make_string("!"));
        io.println(_t3);
    };
```

**泛型闭包示例**（README §5.5）：

Aura 源码：
```aura
fun make_adder(inc: <T>) -> fun(T) -> T {
    return fun(x: T) -> T {
        return x + inc
    }
}
let add_five = make_adder(5)    // T = int
```

生成的 C++：
```cpp
template<typename T>
std::function<T(T)> make_adder(T inc) {
    return [inc]<typename T>(T x) -> T {
        return x + inc;
    };
}
auto add_five = make_adder<int32_t>(5);
```

**注意**：C++20 的泛型 lambda（`[]<typename T>(T x) { ... }`）允许在 lambda 中声明模板参数 `T`。`make_adder` 本身是模板函数，其中的 `T` 被闭包引用，生成的 lambda 需要同样使用 `T`。

---

#### 步骤 4.5：闭包捕获 GC 堆对象时的运行时支持

**问题**：如果闭包捕获了 `GcObject*`（堆对象），C++ lambda 的默认按值捕获只拷贝指针，不会增加 GC 引用计数，导致悬垂指针。

**解决方案（分两阶段）**：

**Phase 1（本次实现）**：
- 限制：闭包只能捕获值类型变量（`int`/`float`/`bool`/`Io`）和函数参数
- 捕获 `GcString*` 或用户记录类型时**报编译错误**："cannot capture heap-allocated variable in closure (not yet supported)"
- 这个限制对于 `io.println(...)` 等最常见场景已足够

**Phase 2（后续）**：
- 生成 GC 可追踪的闭包对象（继承 `GcObject`）
- 在 `TypeDescriptor` 中注册捕获的堆指针字段
- 闭包生命周期由 GC 管理

判断是否可捕获的逻辑：
```cpp
bool canCapture(const std::string& varName) {
    // 值类型 → 安全
    if (isValueType(varName)) return true;
    // 函数参数 → 安全（参数在栈上，生命周期覆盖闭包调用）
    if (isParam(varName)) return true;
    // 堆类型 → Phase 1 不允许
    return false;
}
```

---

#### 步骤 5：函数类型作为变量类型

当前 `let` 声明只支持 `NamedType`、`ListType` 等。需要让 `FunctionType` 可以作为变量类型标注：

```aura
let f: fun(int) -> int = fun (x: int) -> int { return x * 2 }
```

**注意区分**：
- 类型标注位置：`fun(int) -> int`（无空格，紧凑语法，是 `FunctionType` AST 节点）
- 表达式位置：`fun (x: int) -> int { ... }`（有空格，是 `FunExpr` AST 节点）

这在 Parser 层需要确保 `parseType()` 能解析 `fun(...) -> T` 语法。需要检查现有的 [TypeParser.cpp](file:///d:/you/Aura/src/Parser/TypeParser.cpp) 是否已支持——`FunctionType` 在 `Type.h` 中已有 AST 节点定义，但 Parser 可能尚未处理。

---

### 5. 各文件修改清单

| 文件 | 修改内容 | 工作量 |
|------|----------|--------|
| `src/AST/Expr.h` | 新增 `FunExpr` 节点 + `clone()` | 小 |
| `src/ASTWalker.h` | `ExprWalker` 添加 `FunExpr` 分支 | 小 |
| `src/Parser.h` | 声明 `parseFunExpr()` | 小 |
| `src/Parser/ExprParser.cpp` | 实现 `parseFunExpr()`，在 `parsePrimary` 中路由 | 中 |
| `src/Parser/DeclParser.cpp` | 可能需要 `parseParams` 的可选类型版本 | 小 |
| `src/Parser/TypeParser.cpp` | 检查/实现 `fun(params) -> Ret` 类型语法 | 中 |
| `src/Sema/SemAnalyzer.h` | 声明 `inferFunExpr()` | 小 |
| `src/Sema/Checker/ExprInfer.cpp` | 实现 `inferFunExpr()` | 中 |
| `src/Sema/Checker/StmtChecker.cpp` | `ReturnStmt` 返回值类型与闭包标注对齐 | 小 |
| `src/CodeGen/CodeGen.h` | 声明 `genFunExpr()` + 捕获辅助方法 | 小 |
| `src/CodeGen/ExprGen.cpp` | 实现 `genFunExpr()` + 捕获分析 | 大 |
| 测试用例 | `example/` 下新增 `.aura` 测试 | 小 |

### 6. 优先级 & 分阶段计划

| 阶段 | 内容 | 说明 |
|------|------|------|
| **Phase 1** | AST + Parser + Sema + CodeGen（值类型捕获，参数类型必需标注） | 核心功能 |
| **Phase 2** | `fun(params) -> Ret` 函数类型语法支持（TypeParser） | 完善语法 |
| **Phase 3** | GC 堆对象捕获支持 | 需运行时改动 |
| **Phase 4** | 上下文类型推断（省略参数类型如 `let f: fun(int)->int = fun (x) { ... }`） | 复杂度高 |

### 7. 与现有 FunDecl 的代码复用

闭包体（`BlockStmt`）的检查/代码生成完全可以复用现有的函数体逻辑：

| 能力 | 复用来源 | 说明 |
|------|---------|------|
| `parseParams()` | `DeclParser.cpp` | 参数列表解析（可能需要放宽类型可选） |
| `parseBlock()` | `StmtParser.cpp` | 函数体 `{ ... }` 解析 — 完全复用 |
| 函数体类型检查 | `StmtChecker.cpp` | 推入新作用域 → 逐条检查 → 弹出 — 完全复用 |
| 函数体代码生成 | `StmtGen.cpp` | `genBlock` / `genStmt` / `genReturnStmt` 等 — 完全复用 |
| 捕获分析 | `StmtGen.cpp` (genSpawnStmt) | `IdRefCollector` + `DeclaredCollector` — 模式完全复用 |
| 协程判定 | `CoroDecide.cpp` | `CoroScanner` 扫描体 — 完全复用 |

这意味着 `genFunExpr()` 的核心工作只是：**捕获分析 → 生成 `[...](...) { ... }` 包装**，内部语句生成全部委托给现有的 `genStmt`/`genBlock`。

### 8. 风险点

1. **`std::function` 开销**：每次闭包创建都涉及 `std::function` 的堆分配 + 类型擦除。对于高频调用的闭包这可能成为性能瓶颈。Phase 1 接受此开销，后续可优化为自定义闭包类型（直接用 `auto` lambda 类型）。

2. **递归闭包**：闭包调用自身需要 Y 组合子或显式自引用参数。Aura 暂不支持（普通函数的递归也有限制）。

3. **`throws` 闭包与 C++ 异常对齐**：Aura 的 `throws` 闭包需要生成的 lambda 也能抛异常。C++20 lambda 天然支持 `throw()`，映射应该直接。

4. **`Io` 的捕获语义**：`Io` 是能力令牌（值类型，`registeredTypes_["Io"] = false`），C++ lambda 按值捕获即可。但 `io` 变量本身是一个 C++ 对象，其拷贝构造函数是否安全需确认。

5. **命名冲突**：闭包参数名可能与外部变量名冲突。需要确认符号表的作用域正确隔离（当前 `SemAnalyzer` 已有作用域栈，应该自动处理）。

### 9. 闭包与泛型规则（参考 README §5.5）

闭包在 Aura 中是一等公民，但其泛型行为与普通函数略有不同。核心规则：

> **闭包字面量本身不能声明泛型参数**（不能在 `fun` 关键字后接 `<T>`）。  
> 闭包的泛型能力必须通过**外层泛型函数**或**泛型函数类型**引入。

#### 为什么需要这样设计？

闭包是运行时创建的值，它的类型在创建时即确定。如果一个闭包自身声明了泛型参数，那它实际上是一个"泛型值"，需要在不同调用点被单态化，这与闭包作为值的语义冲突。  
因此 Aura 采用**外层引入类型变量**的方式：当闭包位于泛型函数内部时，它可以引用外层函数引入的类型变量，从而在不同实例化中得到不同的具体类型。  
如果闭包的类型本身是泛型的（例如 `fun([<T>], fun(<T>) -> <U>) -> [<U>]`），编译器会根据调用时传入的参数推断 `T` 和 `U`。

#### 对实现的影响（补充步骤 3/4）

**SemAnalyzer（inferFunExpr）**：
- 检查 `FunExpr.params` 和 `FunExpr.body` 中使用的类型名
- 对 `GenericTypeRef`（如 `T`），在当前作用域 → 外层作用域链中查找
- 若找到：记录该类型变量来自哪个外层函数
- 若未找到：报错 "unknown type T in closure"

**CodeGen（genFunExpr）**：
- 若闭包使用了外层泛型类型变量 → 生成 C++20 泛型 lambda `[]<typename T>(T x) { ... }`
- 若闭包类型是泛型函数类型（基于类型标注）→ 同上

#### 规则总结

| 场景 | AST 层面 | CodeGen 层面 | 说明 |
|------|---------|-------------|------|
| 闭包字面量直接写 `fun<T>(x: T) -> T` | Parser 禁止（`fun` 后只能接 `(`） | — | 语法层禁止 |
| 闭包在外层泛型函数内使用 `T` | `FunExpr.body` 含 `Identifier("T")` | 生成 `[]<typename T>(...)` | 核心用例 |
| 闭包的类型是泛型函数类型 | `FunExpr` 无 `typeParams`，类型从标注推导 | 同上 | 类型别名场景 |

#### 实现优先级

| 阶段 | 内容 |
|------|------|
| Phase 1 | 支持非泛型闭包（参数类型显式标注、无类型变量） |
| Phase 5 | 支持外层泛型函数的类型变量传递到闭包 |
