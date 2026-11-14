# feature-21 Phase 3：编译期求值器（Plan v4）

- **Author**：Hermes（主 Agent）
- **Date**：2026-11-01；v3 2026-11-04；**v4 2026-11-08**
- **Related**：`question.txt` v0.4.1 §5.5–§5.6、§7.1–§7.4、§8.1–§8.4、§9.1–§9.3；`src/AST`、`src/Sema`、`src/CodeGen`、`src/Module/CompilationDriver.*`、`src/main.cpp`
- **前置**：Phase 1（语法骨架）、Phase 2（`#const` 展开 + 宏 import）已收官（1701/1701）。

## 0. 标记约定与 v4 裁定

### 0.1 标记

| 标记 | 含义 |
| :--- | :--- |
| **【规范】** | `question.txt` v0.4.1 明文，Phase 3 落实 |
| **【新增】** | 规范未定义，本计划主动引入的实现选择/能力 |
| **【阶段切分】** | 规范有语义，但本阶段不做、归后续 Phase（不是语言收窄） |
| **【新增收窄】** | 规范沉默，本计划主动限制（必须写 v0.4.2） |
| **【补口】** | Phase 2 收官后发现需回头补的缺口 |
| **【新语言特性】** | 主人 2026-11-08 新裁定，超出原规范/Phase 3 求值器本身 |

### 0.2 v4 裁定（2026-11-08 主人逐条确认）

| # | 事项 | v4 结论 | 性质 |
| :-: | :--- | :--- | :--- |
| 1 | `EvalPass` 遍历范围 | **1A**：只遍历非 `isComptimeDecl` 声明体 + 顶层表达式槽；跳过 `QuoteExpr`/`MetaConstructExpr`；`#fun`/`#type` 体由 `Evaluator` 调用时求值 | 【新增】实现边界 |
| 2 | `#{}` 外层名字 | **2A(a)**：进入 `#{}` 时携带外层**运行期绑定名集合**；命中 ⇒ `cannot access runtime variable 'x'`；未命中 ⇒ `undefined name 'x'`；**不读取运行期值** | 【新增】诊断机制 |
| 3 | 泛型推断 | **3A**：`Value` 带递归类型信息；`List`/`Optional`/`Union` 元素/分支类型可推；普通泛型 `fun` 经 `#g(...)` 同规则；显式类型实参优先 | 【规范】实现扩展 |
| 4 | `#fun` 内用户方法 | **4A**：支持**纯**用户方法（解释函数体；禁止项/上限/递归照常） | 【规范】§7.1 同逻辑 |
| 5–10 | v0.4.2 六项 | **全部同意**：`pub #fun`、异常禁止、阶段表、整数溢出、`#compile_error` 约束、Optional/union/slice | 见 §10.2 |
| L1 | 泛型 `<...>` 写法 | **声明/调用均可写可不写**：不写 ⇒ 编译器推断；写 ⇒ 按给定解析 | **【新语言特性】** |
| 11 | union 值回写 | 按 R7 先做最小探针；失败则停 | 【新增】风险控制 |

---

## 1. Objectives

在 Phase 2 展开后的 AST 上增加编译期求值器：对 `#(expr)`、`#{ ... }`、`#fun` 调用（以及被 `#f(...)` 翻转语境的普通函数调用）求值，把结果替换为源级 AST 值节点，使 Sema/CodeGen 不需要认识 `EvalExpr` / `EvalBlockExpr` / `MetaCallExpr`。

【规范】`#(expr)` ≡ `#{ return expr }`；失败 = 编译错误，禁止静默/回退；错误带求值栈 + 源位置；`#compile_error`（`question.txt:586-621`、`:495-509`）。

【新增】Sema 前解释 + 值回写 AST 是实现路线（A1），不是规范强制。

---

## 2. Current State

| # | 断言 | 证据 |
| :-: | :--- | :--- |
| C1 | `#(expr)` ⇒ `EvalExpr` | `src/AST/Expr.h:141-149`；`src/Parser/ExprParser.cpp:482` |
| C2 | `#{...}` ⇒ `EvalBlockExpr` | `src/AST/Stmt.h:111-119`；`src/Parser/ExprParser.cpp:491` |
| C3 | `#fun` ⇒ `FunDecl{isComptime}`；入口 `parseMetaFunDecl` | `src/AST/Stmt.h:395-408`；`src/Parser/DeclParser.cpp:316` |
| C4 | `isComptimeDecl` 过滤 | `src/AST/Stmt.h:680` |
| C5 | 普通调用 `CallExpr{callee,args,typeArgs}`；成员 `MemberAccessExpr` | `src/AST/Expr.h:230-235`、`:263-269` |
| C6 | `#f(...)` ⇒ `MetaCallExpr{callee=MetaValueRefExpr}` | `src/Parser/DeclParser.cpp:189-205` |
| C7 | `#CFG.priority` ⇒ `MetaMemberExpr` | `src/Parser/ExprParser.cpp:520` |
| C8 | `RecordExpr{typeName,fields,typeArgs}` | `src/AST/Expr.h:93-99` |
| C9 | enum 变体由 Sema 置 `enumVariantAccess` | `src/AST/Expr.h:266-272`；`ExprInferMisc.cpp:26` |
| C10 | Sema 拒绝编译期表达式 | `src/Sema/Checker/ExprInfer.cpp:35-41` |
| C11 | CodeGen 占位 | `src/CodeGen/ExprGen.cpp:270-275` |
| C12 | Phase 2 收敛轮 commit + refresh | `src/Module/CompilationDriver.cpp:416-417` |
| C13 | 单/多文件入口在 Sema 前 | `src/main.cpp:157`、`:321` |
| C14 | `ExpandConst.cpp` 对 `EvalBlockExpr` 零命中 | `grep -c EvalBlockExpr` = 0 |
| C15 | `#const` 绑 AST，Phase 2 删除 | `question.txt:811-820`；Phase 2 §3.4.4 |
| C16 | Optional 构造 `some(v)` / `none()`；`slice(start,len)` 是内建 | `READMEs/03-types.md:55`；`src/Sema/BuiltinRegistry.h:309/325` |
| **C17** | **调用位 `id<int>(1)` 普通泛型函数已支持**；**声明位 `fun id<T>` 与 `#g<int>` 尚不支持** | 2026-11-08 实测：`fun id(x:<T>)` + `id<int>(1)` rc=0；`fun id<T>` 报 `expected '(' after function name`；`#g<int>(1)` 报 `'#const' reference '#g' is not defined` |

> **L1 的现状**：`CallExpr` 已有 `typeArgs`（`Expr.h:235`），Sema 的 `CallInfer.cpp:202-205` 已按 `sym->typeParams` 消费显式实参；缺的是**声明位 `<...>`** 和 **`MetaCallExpr` 的 `typeArgs`**。

---

## 3. Scope

### 3.1 做

- 【规范】`#(expr)` / `#{...}`；`#(expr)` ≡ `#{ return expr }`。
- 【规范】`#fun` 调用；`#f(...)` 语境下的普通函数调用（动态检查）。
- 【规范】**值域**：`int/float/bool/string/None/数组/record/enum/Optional/union`；字符串切片。
- 【规范】**纯用户方法**（4A）：`#fun` 内调用 record 方法，解释其函数体。
- 【规范】**泛型调用**：显式 `#g<int>(...)` 或省略推断（3A/L1）。
- 【规范】禁止 IO / 并发 / 时钟随机环境 / 全局可变 / C++ 桥接 / 堆对象逃逸。
- 【规范】步数 + 内存上限、求值栈 + 源位置、`#compile_error`。
- 【新增】`maxCallDepth`；首错即止；2A 名字集合诊断。
- 【补口】Phase 2 的 `ConstTable`/pass/删除进入 `EvalBlockExpr::block`。
- **【新语言特性 L1】** 泛型 `<...>` 可写可不写（声明位与调用位）。

### 3.2 不做（阶段切分/收窄）

| 项 | 归属/处理 | 标记 |
| :--- | :--- | :--- |
| `Code` / `Aura{}` / `target.func` / 织入 | Phase 6 | 【规范】 |
| `#reflect` / 元数据实例值 | Phase 4/5；Phase 3 遇报未支持 | 【阶段切分】 |
| `#type` / `#where` 语义校验 | Phase 4/7 | 【规范】 |
| 类型槽求值（`[0; #n]`、`#fixed(int,16)`） | 本阶段不做；AST 无 sized-array | 【阶段切分】 |
| 注解字段实参求值 | Phase 4 | 【阶段切分】 |
| **编译期方法** `fun (self #X)` | Phase 4/6；与 4A 的**运行期方法**分开 | 【阶段切分】 |
| 异常 `TryCatch`/`Throw` | 本阶段禁止 | 【新增收窄】 |
| `#fun` 的 `by #fun` / `where` 谓词体 | Phase 7；Phase 3 只提供求值器 API | 【阶段切分】 |
| interface 默认体（当前产物不可编译，bug-117） | 本阶段遇则报未支持 | 【阶段切分】 |

---

## 4. Design

### 4.1 模块

| 文件 | 职责 | 标记 |
| :--- | :--- | :--- |
| `src/CompileTime/Value.h` | `Value` + 递归 `TypeInfo` + 相等/打印 | 【新增】 |
| `src/CompileTime/DeclIndex.h/.cpp` | `#fun`/`fun`/record/enum/方法索引；泛型形参表 | 【新增】 |
| `src/CompileTime/Evaluator.h/.cpp` | 解释器：表达式/语句/调用/控制流/泛型实例化/方法/上限/栈 | 【新增】 |
| `src/CompileTime/EvalPass.h/.cpp` | 替换值位节点；跳过 `#fun`/`#type` 体；2A 名字集合 | 【新增】 |
| `src/Parser` / `src/AST` | L1：`FunDecl::typeParams`、`MetaCallExpr::typeArgs`、可选 `<...>` | 【新语言特性】 |

### 4.2 `Value` 与递归类型信息（3A）

```cpp
struct TypeInfo {
    enum class Kind { Unknown, Int, Float, Bool, String, None,
                      List, Record, Enum, Optional, Union };
    Kind kind = Kind::Unknown;
    std::string name;                    // Record/Enum/Union 类型名
    std::vector<std::string> typeArgs;   // 泛型实参
    std::vector<std::shared_ptr<TypeInfo>> params; // List 元素 / Optional 元素 / Union 分支
};

struct Value {
    enum class Kind { Int, Float, Bool, String, None, List, Record, Enum,
                      Optional, Union };
    Kind kind;
    int64_t i = 0; double f = 0; bool b = false;
    std::string s;                      // String；Record/Enum/Union 类型名
    std::string variant;                // Enum/Union 活动变体
    std::vector<Value> elems;           // List / Record 字段 / Optional(payload) / Union(payload)
    std::vector<std::string> names;     // Record 字段名
    std::shared_ptr<TypeInfo> type;     // 递归类型（3A；空 list / none 可 Unknown）
    bool hasValue = false;              // Optional: present?
};
```

**推断规则（3A）**：

| 值 | 推出的类型 |
| :--- | :--- |
| Int/Float/Bool/String/None | 标量 |
| List | `List<unify(元素)>`；空 list 且无期望类型 ⇒ `Unknown` |
| Record | `Record{typeName, typeArgs}` |
| Enum | `Enum{typeName}` |
| Optional | `Optional< payload 类型 >`；`none()` ⇒ `Optional<Unknown>` |
| Union | `Union{typeName, 活动分支类型}` |

**显式实参优先**：`#g<int>(...)` / `id<int>(...)` 时直接用给定类型建 `TypeEnv`；数量不符、未知类型 ⇒ 编译错误；不再推断该参数。

### 4.3 求值矩阵

| 节点 | 支持 | 说明 |
| :--- | :--- | :--- |
| 字面量 / `Identifier` | ✅ | 局部 env 查找；未命中再看 2A 运行期名字集合 |
| `BinaryExpr`/`UnaryExpr`/`ConditionalExpr` | ✅ | 除零/溢出显式报错（A18） |
| `ListExpr`/`RecordExpr` | ✅ | record 按 `typeName` + 泛型实参 |
| `MemberAccessExpr` | ✅ | record 字段；`Color.Red` |
| `MetaMemberExpr` | ✅ | base 求值后读字段；`#reflect.*` ⇒ Phase 5 未支持 |
| `IndexExpr` | ✅ | list/string 越界报错 |
| `CallExpr`（无 `#`） | ⚠️ | 普通 `fun` / 白名单内建；命中 `#fun` ⇒ `'#fun' must be called with '#'`（D1） |
| `MethodCallExpr` | ✅ | 内建白名单 + **纯用户方法（4A）**；编译期方法 `#X` 接收者 ⇒ Phase 4/6 |
| `AssignExpr` | ✅ | 局部/字段/元素 |
| `EvalExpr`/`EvalBlockExpr` | ✅ | 嵌套求值 |
| `MetaCallExpr` | ✅ | `#fun`/普通 fun/`compile_error` 特判（D2）；支持 `typeArgs`（L1） |
| `MetaConstructExpr`/`QuoteExpr`/`MetaValueRefExpr` | ❌ | Phase 4/5/6；显式报未支持 |
| `FunExpr`/`PipeExpr`/`ErrorPropagationExpr` | ❌ | 【新增收窄 A17】显式报未支持 |

**语句**：`BlockStmt`/`LetDecl`/`ConstDecl`/`ExprStmt`/`IfStmt`/`WhileStmt`/`ForStmt`/`ReturnStmt`/`BreakStmt`/`ContinueStmt`。
**`HashConstDecl`**：由【补口 A15】Phase 2 在 `#{}` 内展开/删除；Phase 3 不应见到。

**参数绑定**：位置实参 → 形参；尾部缺省用 `Param::defaultExpr` 在**被调函数环境**求值；**命名实参不存在**（`CallExpr` 无名字段），不实现。

### 4.4 泛型：显式与推断（3A + L1）

**L1 语法（新语言特性）**：

```aura
// 声明位：两种写法等价
fun id<T>(x: T) -> T { return x }        // 新：<T> 显式声明
fun id(x: <T>) -> T { return x }         // 旧：由参数里的 <T> 推导

// 调用位：写或不写
id<int>(1)     // 显式：按给定解析
id(1)          // 省略：编译器推断
```

规则：

1. 声明位 `<...>` 可省略；省略时从签名中的 `<T>` 占位符收集泛型名（现有行为）。
2. 写了 `<...>` 时，签名里的 `T` 指该泛型参数；重复声明 / 未使用 / 数量不符 ⇒ 编译错误。
3. 调用位 `<...>` 可省略；省略 ⇒ 用 3A 递归推断；写了 ⇒ 按给定类型建 `TypeEnv`，不再推断该参数。
4. `#fun` 同样支持：`#fun g<T>(x: T) -> T`、`#g<int>(1)`；当前 `#g<int>` 不支持，由 L1 补 `MetaCallExpr::typeArgs`。
5. 普通泛型函数经 `#g(...)` 调用（§7.2 规则 5）走同一套绑定；`where` 检查本阶段不做（A10 扩展为 `#fun` 与普通 fun 均延后）。

**求值器绑定**：

- `DeclIndex` 保存每个 `#fun`/`fun` 的 `typeParams`（L1 显式或旧式收集）；
- 调用点：有显式 `typeArgs` ⇒ 直接绑定；无 ⇒ 从 `Value.type` 递归推断；
- 无法推断 ⇒ `cannot infer type parameter 'T' at compile time`；
- 泛型 record 值回写保留 `typeArgs`。

### 4.5 `#compile_error`（D2）

```cpp
if (auto* r = dynamic_cast<MetaValueRefExpr*>(call->callee.get());
    r && r->name == "compile_error") {
    // 只接受 1 个 string 字面/编译期字符串（A20）
    return evalCompileError(call, r);
}
```

`CallExpr + Identifier("compile_error")` 不是合法形态（规范只定义 `#compile_error`）。

### 4.6 值回写

| 值 | AST |
| :--- | :--- |
| Int/Float/Bool/String/None | 对应字面量 |
| List | `ListExpr` |
| Record | `RecordExpr{typeName, fields, typeArgs}` |
| Enum | `MemberAccessExpr{Identifier(type), variant}`；Sema 置 `enumVariantAccess` |
| Optional | `CallExpr(Identifier("some"/"none"), …)` |
| Union | 活动变体 payload 表达式（标签仅保留在 `Value`，见 R7） |

### 4.7 类型位（Q6 类型位，阶段切分）

`EvalPass` **只遍历表达式/语句槽，不遍历 `TypeExpr` 槽**。因此：

- `[0; #n]`（当前 AST 无 sized-array）与 `#fixed(int, 16)`（类型位置）**Phase 3 不处理**；
- 类型槽里的 `#(...)`/`#f(...)` 不在本阶段范围；由未来的 sized-array/类型位置特性统一设计；
- 边界表显式登记，避免实施者“顺手”实现出半套。

### 4.8 集成、上限、诊断

- 单文件：`main.cpp:157` 后、Sema 前；多文件：`:321` 后、Sema 前。
- 【新增】`EvalLimits{1e6, 64MB, 256}`；错误带栈；首错即止。
- **1A**：`EvalPass` 跳过 `isComptimeDecl` 声明体、`QuoteExpr`、`MetaConstructExpr` 子树；`#fun`/`#type` 体内的 `#(...)` 由 `Evaluator` 在调用时求值。

### 4.9 2A（a）：运行期名字集合

`EvalPass` 维护 `RuntimeScopeStack`：

- 进入运行期函数压入参数名；
- 遇到 `let`/`const`（非 `#const`）声明、`for` 循环变量时加入当前作用域；
- 进入 `EvalBlockExpr` 时把**当前可见的运行期名字集合**快照传给 `Evaluator`。

`Evaluator` 查找顺序：

1. 编译期局部环境；
2. 未命中且名字在运行期集合 ⇒ `cannot access runtime variable 'x' in compile-time block`；
3. 都不在 ⇒ `undefined name 'x'`。

```aura
fun main(io: Io) {
    let hp = 10
    hp = hp + 1
    let a = #{ return hp + 1 }    // ❌ cannot access runtime variable 'hp'
    let b = #{ return hpp + 1 }   // ❌ undefined name 'hpp'
}
```

**不读取运行期值**：不绑定 10/11；`hp` 是运行期变量，编译期块只能“知道名字”，不能取值。

### 4.10 4A：纯用户方法

- `MethodCallExpr` 解析顺序：内建白名单 → 用户方法；
- 用户方法：按接收者 `TypeInfo` 查 record 方法，绑定 `self`，解释函数体；
- 纯检查：方法体内出现 IO/并发/时钟随机/全局可变/C++ 桥接/堆逃逸 ⇒ 在该节点报错；
- 递归/上限沿用 `maxCallDepth` + `maxSteps`；
- 编译期方法（接收者 `#X`）仍归 Phase 4/6，不在此路径。

```aura
type Point { x: int; y: int }

fun (self Point) sum() -> int { return self.x + self.y }

#fun total(p: Point) -> int {
    return p.sum()          // ✅ 运行期纯方法，解释其函数体
}
```

---

## 5. 特意新增清单（审查重点）

| # | 内容 | 类型 | 需要 v0.4.2？ |
| :-: | :--- | :--- | :-: |
| A1 | Sema 前解释 + 值回写源级 AST | 【新增】实现路线 | 否 |
| A2 | `Value` + `Value→AST` | 【新增】实现选择 | 否 |
| A3 | `DeclIndex`（不依赖 Sema） | 【新增】实现选择 | 否 |
| A4 | `maxCallDepth=256` | 【新增】 | 可选 |
| A5 | 首错即止 | 【新增】策略 | 可选 |
| A6 | 跨模块 `#fun` 必须 `pub`（Q1） | **【新增收窄】** | **是** |
| A7 | 编译期方法 Phase 4/6 | **【阶段切分】** | 是（阶段表） |
| A8 | 元数据值 Phase 4/5 | **【阶段切分】** | 是（阶段表） |
| A9 | 异常禁止（Q11） | **【新增收窄】** | **是** |
| A10 | `where` 检查延后（`#fun` + 普通 fun，Q12/3A） | **【阶段切分】** | 是（阶段表） |
| A11 | 类型位求值不做（含 `#fixed`/数组长度） | **【阶段切分】** | 是（阶段表） |
| A12 | 注解字段实参 Phase 4 | **【阶段切分】** | 是（阶段表） |
| A13 | 内建白名单 `str/len/slice/append/…` | 【新增】细节 | 是（附清单） |
| A14 | `#compile_error` AST 特判 | 【新增】实现细节 | 否 |
| A15 | **Phase 2 补 `EvalBlockExpr` 收集** | **【补口】跨期缺陷修补** | 否 |
| A16 | 泛型 record 保留 `typeArgs`（Q10） | 【规范】实现手段 | 是（补细则） |
| A17 | `FunExpr/Pipe/ErrorPropagation` 未支持 | 【新增收窄】 | 是 |
| A18 | 整数溢出显式报错 | 【新增】语义选择 | **是** |
| A19 | 2A(a) 运行期名字集合 + 专用诊断 | 【新增】诊断机制 | 是（诊断文案） |
| A20 | `#compile_error` 只收 1 个 string | 【新增】约束 | 是（可选） |
| A21 | 默认值绑定规则（尾部默认） | 【规范】实现细节 | 否 |
| **A22** | **4A 纯用户方法支持** | **【规范】§7.1 同逻辑** | 是（方法表） |
| **A23** | **L1 泛型 `<...>` 声明/调用可写可不写** | **【新语言特性】** | **是** |
| **A24** | **3A 递归 `TypeInfo` 推断** | **【规范】实现手段** | 是（推断规则） |

> **A15 单列**：Phase 2 的实证缺口（C14），不是 Phase 3 新增能力。
> **命名实参**：当前语言不存在（`CallExpr` 无名字段；Sema 无 named-arg），不在 A 表、也不实现。

---

## 6. Impact Analysis

| 组件 | 影响 |
| :--- | :--- |
| `src/main.cpp` | 两处调用（`:157` 后、`:321` 后），Sema 前 |
| `src/Module/ExpandConst.*` | 【补口】扩 `EvalBlockExpr`；不改其它展开语义 |
| Sema/CodeGen | 普通调用路径零改动；L1 需 Sema 消费声明位 `typeParams`（已有调用位 `typeArgs` 支持） |
| AST | L1 新增 `FunDecl::typeParams`、`MetaCallExpr::typeArgs`；不改现有节点语义 |
| Parser | L1：函数声明可选 `<...>`；`#fun` 声明/调用可选 `<...>` |
| 构建 | `CMakeLists.txt` 增 `src/CompileTime/*.cpp` |
| 兼容性 | 无 `#(...)`/`#{}`/`#fun` 的程序产物逐字不变；旧式 `fun f(x:<T>)` 继续可用 |

⚠️ **BREAKING（语言面）**：求值失败从“Sema not supported”变为“编译期求值错误”；L1 新增语法，v0.4.2 留档。

---

## 7. Boundary Conditions

| 边界 | 处理 | 测试 |
| :--- | :--- | :--- |
| 除零 / 溢出 | 显式报错（A18） | `DivByZero`、`IntOverflow` |
| 索引越界 | 报错 | `IndexOOB` |
| 空 list/string | 合法；空 list 推断需上下文/显式实参 | `EmptyValues`、`EmptyListInfer` |
| `None` 误用 | 类型错误 | `NoneMisuse` |
| Optional `none` 解包 | 报错 | `NoneUnwrap` |
| union 活动变体 | 按 payload 求值 | `UnionValue` |
| 递归/死循环 | 步数 + 深度 | `InfiniteRecursion`、`StepLimit` |
| 内存 | 字节上限 | `MemoryLimit` |
| 未支持节点 | 明确报「Phase 3 未支持」 | `UnsupportedNode` |
| IO/并发/时钟随机/全局/C++ 桥接 | 执行到即报 | 5 组负例 |
| `#{}` 无 return | 报错 | `NoReturn` |
| `#{}` 访问块外运行期变量 | 2A(a) 专用诊断 | `RuntimeVarInBlock` |
| `#{}` 拼写错误 | `undefined name` | `UndefinedInBlock` |
| 语句位 `#(...)`/`#{}` | 普通值 ⇒ `value cannot be used as a statement` | `EvalInStmtPosition` |
| `#fun` 漏 `#` | `'#fun' must be called with '#'` | `FunWithoutHash` |
| 默认值 | 尾部缺省按 `defaultExpr` | `DefaultParam` |
| 泛型无法推断 | 报 `cannot infer type parameter` | `GenericInferFail` |
| 显式类型实参数量不符 | 报错 | `GenericArityMismatch` |
| 泛型值回写 | 保留 `typeArgs` | `GenericRecordValue` |
| L1 声明位 `<...>` 与旧式 `<T>` 混用 | 重复声明报错；引用一致则允许 | `GenericDeclForms` |
| `#g<int>` / `#fun g<T>` | L1 后支持 | `MetaGenericArgs` |
| 用户方法不纯 | 在禁止节点报错 | `UserMethodImpure` |
| 用户方法递归 | 上限 | `UserMethodRecursion` |
| `#reflect.Type` 形参 | Phase 5 未支持 | `ReflectParamUnsupported` |
| 类型位 `#fixed`/数组长度 | 本阶段不遍历类型槽 | `TypePositionUntouched`（文档级） |
| 跨模块 `#fun` | 必须 `pub #fun`；否则未定义 | `CrossModuleFun` |

---

## 8. Test Plan

### 8.1 单元 `test/compile_time/test_eval.cpp`

字面量/运算（`IntArith`、`FloatArith`、`BoolLogic`、`StringConcat`、`Compare`、`Slice`）；控制流（`IfExpr`、`WhileSum`、`ForSum`、`EarlyReturn`、`BreakContinue`）；值（`ListBuild`、`RecordBuild`、`RecordField`、`EnumVariant`、`NoneValue`、`OptionalValue`、`OptionalNone`、`UnionValue`）；函数（`FunCall`、`RecursiveFact`、`OrdinaryFunCall`、`NestedFunCall`、`DefaultParam`、`GenericFun`、`GenericRecordValue`、`GenericExplicitArgs`、`GenericInferList`、`GenericInferOptional`）；方法（`UserMethodCall`、`UserMethodImpure`、`UserMethodRecursion`）；块（`EvalExprEquivBlock`、`NoReturn`、`RuntimeVarInBlock`、`UndefinedInBlock`）；错误（`DivByZero`、`IndexOOB`、`NoneMisuse`、`UnsupportedNode`、`ForbiddenIO`、`ForbiddenSpawn`、`ForbiddenBridge`、`CompileErrorBuiltin`、`FunWithoutHash`、`ReflectParamUnsupported`、`GenericArityMismatch`）；上限（`StepLimit`、`CallDepth`、`MemoryLimit`）；注入（`ExprValueToLiteral`、`RecordValueToRecordExpr`、`EnumValueToMemberAccess`、`OptionalValueToSomeCall`）。

### 8.2 集成

`ConstEvalExpr`、`ConstEvalBlock`、`ConstWithoutHash`、`ConstWithHash`、`FunInConst`、`MultiModuleFun`（`pub #fun`）、`NoSilentFallback`、`NoConstRegression`、`D3BlockConst`（`#{ #const n=2; return n*4 }`）、`D6MetaMember`（`#CFG.priority`）、`L1DeclForm`（`fun id<T>`）、`L1CallForm`（`id<int>`）、`L1MetaCall`（`#g<int>`）、`UserMethodInFun`。

### 8.3 回归

全量 ≥1701 零减少；`used/1–6` 6/6；`test.aura` md5 不变；正例 `aurac + g++ -fsyntax-only`；独立验证批。

---

## 9. Implementation Steps

| 批 | 内容 | 出口 | 回滚 |
| :-: | :--- | :--- | :--- |
| **S0** | 【补口 A15】Phase 2 扩 `EvalBlockExpr` 收集/展开/删除 + 测试 | D3 用例过；1701 不降 | 撤销补口 |
| **S0.5** | **L1 前置**：`FunDecl::typeParams`、`MetaCallExpr::typeArgs`、声明/调用可选 `<...>`、`#fun g<T>`；旧式 `<T>` 保持 | 新旧写法均编译；无回归 | 关语法分支 |
| **S1** | 只读核查（AST/Sema/Driver/索引/泛型形参） | 报告 | 无写盘 |
| **S2** | `Value` + `TypeInfo`（递归）+ `DeclIndex` + 泛型表 | 构建；索引用例 | 删目录 |
| **S3** | 表达式层（D1/D2/D6 + Optional/Union + 切片 + 方法） | 单元 1–5 组 | 关调用 |
| **S4** | 语句层 + 泛型推断/显式实参 + 栈/上限 + `#compile_error` | 单元 6–8 组 | 回退表达式 only |
| **S5** | `EvalPass`（1A 跳过 + 2A 名字集合）+ 值回写 + main 两处 | 集成 8.2 | 去 main 调用 |
| **S6** | 全量 + 独立验证 + 文档（v0.4.2：5–10 + L1） | 全绿 | 文档备份 |

> **S0.5 提醒**：L1 是**独立语言特性**，会影响 Parser/AST/Sema。若主人希望拆出单独 issue/plan，可将 S0.5 移出 Phase 3；Phase 3 先只支持旧式 `<T>` 声明与已存在的调用位 `typeArgs`。默认按本计划并入。

---

## 10. Risks & Remaining

### 10.1 风险

| # | 风险 | 缓解 |
| :-: | :--- | :--- |
| R1 | 解释语义与运行期漂移 | 白名单语义 + 对照测试；溢出/除零显式化（A18） |
| R2 | 普通函数解释把运行期语义抄进编译器 | 5 类禁止项负例；未支持节点显式报错 |
| R3 | 递归/大循环挂死 | 三重上限 + 注入 |
| R4 | 值回写 record/enum/Optional/union 后 Sema 推断失败 | 集成用例先红后绿；保留 `typeName/typeArgs` |
| R5 | 泛型推断失败（空 list / none / union） | `TypeInfo{Unknown}` + 显式实参兜底；明确报错 |
| R6 | 与 Phase 4/5/6/7 边界混淆 | 遇未支持即报；阶段表写 v0.4.2 |
| R7 | union 值回写丢标签 | `Value` 保标签；回写 payload；S3 前最小探针；失败即停 |
| R8 | L1 语法改动面大（Parser/AST/Sema） | 旧式 `<T>` 必须零回归；S0.5 独立提交；必要时拆出 Phase 3 |
| R9 | 用户方法动态派发/接口默认体 | 先支持 record 方法；interface 默认体遇则报未支持（bug-117） |

### 10.2 v0.4.2 待落文本（主人已同意，5–10）

1. `pub #fun`（Q1/A6）；
2. 异常禁止（Q11/A9）；
3. 阶段表：编译期方法、元数据值、类型位、注解字段、where（A7/A8/A10–A12）；
4. 整数溢出语义（A18）；
5. `#compile_error` 参数约束（A20）；
6. Optional/union/slice 纳入值域与 `slice` 白名单（A13/A16）。

**新增（v4）**：

7. 2A(a) 运行期名字集合 + 专用诊断文案（A19）；
8. 4A 纯用户方法（A22）；
9. L1 泛型 `<...>` 声明/调用可写可不写（A23）；
10. 3A 递归类型推断规则（A24）。

---

## 11. 结论

v4 已落实主人 2026-11-08 的逐条裁定：1A、2A(a)、3A、4A、5–10 全同意；L1 作为【新语言特性】并入 S0.5，并明确“旧式 `<T>` 零回归”。

**剩余需你确认的只有两件**：

1. **S0.5 是否留在 Phase 3**：L1 会动 Parser/AST/Sema，默认并入；若你想单独立项，我把它移出。
2. **R7 union 探针**：S3 前跑；若 Sema 不接受 payload 回写，停下来裁定，不硬写。
