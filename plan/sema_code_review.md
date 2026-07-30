# Plan：Sema 模块代码审查 — 全函数运行逻辑

> **来源**：对 `src/Sema/` 全部 11 个文件的逐函数审查
> **日期**：2026-07-30
> **状态**：审查报告

---

## 一、文件清单与职能

| # | 文件 | 行数 | 职能 |
|:---|:---|:---|:---|
| 1 | `SemType.h` | ~100 | 10 种 SemType 子类声明 + 工具函数 |
| 2 | `SemType.cpp` | ~80 | equals / toString / clone 实现 |
| 3 | `Symbol.h` | ~40 | SymKind 枚举 + Symbol 结构 |
| 4 | `SymbolTable.h` | ~50 | Scope + SymbolTable 类声明 |
| 5 | `SymbolTable.cpp` | ~60 | define / lookup / 作用域管理 |
| 6 | `BuiltinRegistry.h` | ~300 | 内置类型/方法/函数注册表 |
| 7 | `SemAnalyzer.h` | ~120 | 语义分析器类声明 |
| 8 | `SemAnalyzer.cpp` | ~350 | 主入口、类型解析、泛型代换、isAssignable、导入导出 |
| 9 | `Checker/ExprInfer.cpp` | ~400 | 表达式类型推断（20 种表达式） |
| 10 | `Checker/DeclChecker.cpp` | ~200 | 声明注册 + 函数/方法体检查 |
| 11 | `Checker/StmtChecker.cpp` | ~600 | 语句检查（15 种语句） |

---

## 二、SemType 类型体系（SemType.h + SemType.cpp）

### 2.1 类型继承树

```
SemType（抽象基类）
├─ ErrorSemType          # 类型错误占位符
├─ PrimSemType           # Int | Float | Bool | String
├─ NoneSemType           # None 值类型
├─ RecordSemType         # { field: type, ... }
├─ UnionSemType          # T1 | T2 | ...
├─ ListSemType           # [T]
├─ FuncSemType           # (params) -> returnType [throws]
├─ InterfaceSemType      # interface { methods }
├─ GenericSemType        # 泛型变量 <T>
└─ IterSemType           # 迭代器
```

### 2.2 equals 规则

| 类型 | 等价规则 |
|:---|:---|
| `ErrorSemType` | 对方也是 Error |
| `PrimSemType` | kind 相同 |
| `NoneSemType` | 对方也是 None |
| `RecordSemType` | **结构等价**：字段数相同，按名称匹配（忽略顺序），递归 equals |
| `UnionSemType` | 变体数相同，**顺序敏感**，递归 equals |
| `ListSemType` | 元素类型 equals |
| `FuncSemType` | throws 一致，参数数/类型/顺序相同，返回类型 equals |
| `InterfaceSemType` | **结构等价**：方法数相同，按名称匹配（忽略顺序），签名完全匹配 |
| `GenericSemType` | name 相同（不比较 resolvedName） |
| `IterSemType` | 元素类型 equals |

### 2.3 工具函数

| 函数 | 逻辑 |
|:---|:---|
| `intType()` / `floatType()` / `boolType()` / `stringType()` | 返回对应 PrimSemType |
| `typeEquals(a, b)` | 两个 unique_ptr，都空→true，一空→false，否则 `a->equals(*b)` |

---

## 三、Symbol + SymbolTable（Symbol.h + SymbolTable.h + SymbolTable.cpp）

### 3.1 Symbol 结构

```cpp
SymKind kind;           // Variable | Parameter | Function | Method | TypeAlias | Interface | GenericParam
std::string name;
unique_ptr<SemType> type;
bool isConst = false;
vector<SymParam> params;
bool throws = false;
vector<string> typeParams;
vector<SymParam> ctorParams;        // 构造函数参数
unique_ptr<SemType> ctorReturnType; // 构造函数返回类型
vector<MethodSig> interfaceMethods;
optional<string> belongsToModule;
bool isPublic = false;
```

### 3.2 Scope — define 覆盖规则

```
输入：Symbol sym
1. 若 sym.kind ∈ {Function, Method}：
   - 已存在同名+同种类 → 返回 false（不覆盖，支持重载）
2. 若 sym.kind == Method 且已存在同名 TypeAlias：
   - 将 Method 的 params/returnType 存入 TypeAlias.ctorParams/ctorReturnType
   - 视作构造函数注册
3. 否则：已存在任何同名符号 → 返回 false
4. 无冲突 → 插入符号表
```

### 3.3 Scope — lookup

```
输入：name
1. lookupLocal(name) → 命中返回
2. 未命中 → parent().lookup(name) 递归
3. parent() == nullptr → 返回 nullptr
```

### 3.4 SymbolTable — 作用域管理

| 操作 | 逻辑 |
|:---|:---|
| `enterScope(kind)` | 以 current_ 为父创建新 Scope，更新 current_ |
| `exitScope()` | current_ 回退到 parent；全局作用域时忽略 |
| `define(sym)` | 代理到 current_->define() |
| `defineGlobal(sym)` | 遍历 scopes_ 找全局作用域 |
| `lookup(name)` | 代理到 current_->lookup()（向上递归） |
| `lookupGlobal(name)` | 仅查全局作用域 local |

---

## 四、BuiltinRegistry（BuiltinRegistry.h）

### 4.1 init() 注册清单（每次启动硬编码初始化）

**类型（types_）**：

| 键 | isHeap | 对应 C++ 类型 |
|:---|:---|:---|
| `int` | false | `int32_t` |
| `float` | false | `double` |
| `bool` | false | `bool` |
| `string` | true | `aura_rt::GcString*` |
| `None` | false | `std::monostate` |
| `Io` | true | `aura_rt::Io` |
| `Path` | true | `aura_rt::Path` |
| `channel` | true | `aura_rt::channel<T>` |
| `Mutex` | true | `aura_rt::Mutex*` |
| `RWMutex` | true | `aura_rt::RWMutex*` |
| `Once` | true | `aura_rt::Once*` |
| `RWMutexReadView` | false | `aura_rt::RWMutex::ReadGuard` |
| `RWMutexWriteView` | false | `aura_rt::RWMutex::WriteGuard` |

**方法（methods_）**：

| 类型 | 方法 | 参数 | 返回 |
|:---|:---|:---|:---|
| `string` | `len` | 无 | int |
| `string` | `concat` | `other: string` | string |
| `string` | `append` | `other: string` | string |
| `string` | `slice` | `start: int, len: int` | string |
| `[T]` | `len/size/empty/capacity/front/back/append/pop/remove/insert/clear/reserve/slice` | 各种 | 各种 |
| `channel` | `send/receive/close` | 各种 | 各种 |
| `RWMutex` | `r/w` | 无 | RWMutexReadView / WriteView |

**全局函数（functions_）**：

| 函数 | 参数 | 返回 |
|:---|:---|:---|
| `range(end)` | int | iter |
| `range(start, end)` | int, int | iter |
| `range(start, end, step)` | int, int, int | iter |
| `channel(cap)` | int | channel\<T\> |
| `sync.Mutex()` | 无 | Mutex |
| `sync.RWMutex()` | 无 | RWMutex |
| `sync.Once()` | 无 | Once |
| `gc_force()` | 无 | None |
| `gc_stats()` | 无 | string |

### 4.2 方法查找

```
findMethod(typeName, methodName, argCount)
1. 线性遍历 methods_ 向量
2. 匹配：typeName == entry.typeName
   && methodName == entry.name
   && params.size() == argCount
3. 命中返回 &entry.returnType
4. 未中返回 nullptr
```

### 4.3 tryLoadAurai — .aurai 文件加载

```
1. loadedAurai_.count(baseName) → 已加载返回 false
2. loadedAurai_.insert(baseName)
3. doLoadAurai(ast)：
   - TypeDecl → 若 types_ 无此键，添加条目
   - MethodDecl → 提取 params/returnType，追加到 methods_
   - FunDecl → 追加到 functions_
```

---

## 五、SemAnalyzer 主入口与核心方法（SemAnalyzer.cpp）

### 5.1 analyze — 总入口

```
输入：Program& ast
1. declareTopLevel(program)    # 第 1 遍：注册所有顶层符号
2. checkProgram(program)        # 第 2 遍：检查函数/方法体
3. 返回 !diag_.hasErrors()
```

### 5.2 resolveNamedType — 名称→类型解析

```
输入：name
1. BuiltinRegistry::findType(name) → 命中：
   - Int/Float/Bool/String → PrimSemType
   - None → ErrorSemType
   - Other → GenericSemType(name)
2. symtab_.lookup(name) → 命中：
   - TypeAlias：
     a. resolvingTypes_.count(name) → 自引用 → GenericSemType 占位
     b. 否则 sym->type.clone()
   - Interface → InterfaceSemType（克隆所有方法签名）
   - GenericParam → GenericSemType(name)
3. 未命中 → ErrorSemType
```

### 5.3 isAssignable — 类型兼容性

```
输入：target, source
优先级从高到低：
1. target 或 source 是 ErrorSemType → true（静默传播）
2. target 是 GenericSemType → true（实例化时才检查）
3. source 是 GenericSemType → 查 TypeAlias 展开后递归
4. target 是 UnionSemType → source 与任意变体兼容即 true
5. 双方是 ListSemType → 递归元素
6. 双方是 FuncSemType → matchFuncSig
7. target 是 InterfaceSemType → source 是 FuncSemType 且接口仅1方法 → 闭包适配
8. target 是 RecordSemType → 结构兼容（按名匹配，递归 isAssignable）
9. target.equals(source) 严格相等
```

### 5.4 substitute — 泛型代换

```
输入：type, genericName, concrete
1. type 是 GenericSemType(genericName) → concrete.clone()
2. 复合类型 → 递归遍历子类型
3. 其他 → clone()
```

### 5.5 collectGenericMapping — 泛型映射收集

```
输入：formal（形参类型）, actual（实参类型）, map
1. formal 是 GenericSemType → map[name] = actual（冲突时 isAssignable 校验）
2. 双方是 ListSemType → 递归元素
3. 双方是 FuncSemType → 递归参数+返回
```

### 5.6 importExports / extractExports

```
importExports(alias, exports):
1. exports.types → 注册为 TypeAlias
2. exports.ctors → 注册为 Function
3. exports.funcs → 注册为 Function
4. 有 alias → 额外注册命名空间变量符号

extractExports():
1. 遍历全局作用域符号
2. isPublic → 收集 TypeAlias（含 ctorParams）和 Function
```

---

## 六、声明注册与体检查（Checker/DeclChecker.cpp）

### 6.1 declareTopLevel → declareDecl

```
遍历所有顶层声明，按类型分发：

TypeDecl：
1. 注册泛型参数
2. 前向声明 ErrorSemType 占位（解决自引用）
3. collectGenericRefs 检查未定义泛型参数
4. resolveType 解析类型体
5. sealSelfRefs 处理自引用
6. 用完整类型更新占位符

InterfaceDecl：
1. 解析所有方法签名
2. 注册为 SymKind::Interface

FunDecl：
1. 临时作用域注册泛型参数
2. 解析参数类型+返回类型
3. 退出临时作用域，注册到全局

MethodDecl：
1. 直接解析参数+返回类型
2. 注册到全局
```

### 6.2 resolveType — AST TypeExpr → SemType

```
动态分派：
- NamedType → resolveNamedType + applyTypeArgs + materializeCanonicalName
- ListType → ListSemType(elementType)
- RecordType → RecordSemType(fields)
- UnionType → UnionSemType(variants)
- FunctionType → FuncSemType(params, return, throws)
- GenericTypeRef → GenericSemType(name)
```

### 6.3 checkFunBody / checkMethodBody

```
checkFunBody：
1. 推入 Function 作用域
2. 注册泛型参数 / 形参
3. 设置 currentReturnType_ / currentFunctionThrows_
4. checkBlock(body)

checkMethodBody：
1. 同上 + 注册 self 参数
2. 若有 impl 接口 → 逐方法比较签名
```

---

## 七、表达式类型推断（Checker/ExprInfer.cpp）

### 7.1 inferExpr — 调度入口

```
dynamic_cast 顺序：
字面量(Int/Float/String/Bool/None) → Identifier → ListExpr → RecordExpr
→ BinaryExpr → UnaryExpr → CallExpr → MethodCallExpr → MemberAccessExpr
→ IndexExpr → AssignExpr → FunExpr → ErrorPropagationExpr → PipeExpr → MatchExpr
→ 其他报 "internal error"
```

### 7.2 inferBinaryExpr — 二元运算

```
操作符分派：
- +：有一方是 string → string（隐式转换），否则算术
- -/*/%：检查兼容，返回左操作数类型
- < <= > >= == !=：返回 bool
- and / or：双方 bool → bool

兼容性检查：isAssignable(*lt, *rt) || isAssignable(*rt, *lt)
```

### 7.3 inferCall — 函数调用

```
1. callee 非标识符 → 推断为 FuncSemType → 返回 returnType
2. BuiltinRegistry::findFunction → 返回注册的 ReturnType
3. 符号表 Function/Method：
   a. throws 兼容性（E016_ThrowsViolation）
   b. 参数数量检查
   c. 逐参数 isAssignable
   d. 泛型：collectGenericMapping + substitute 返回类型
4. TypeAlias 有 ctorParams → 构造函数调用
5. Variable 但类型是 FuncSemType → 函数值调用
```

### 7.4 inferMethodCall — 方法调用

```
1. import 命名空间：alias.method → 查全局符号
2. 内置模块：path.new 等 → 拼接名查 BuiltinRegistry
3. 内置方法：推断对象类型 → 匹配类型键 → findMethod
4. 未知 → ErrorSemType + 错误提示（方法列表）
```

### 7.5 inferFunExpr — 闭包

```
1. 解析参数类型（必须显式标注，否则报错）
2. 解析返回类型
3. 推入 Function 作用域，注册参数
4. 保存/恢复 currentReturnType_ / currentFunctionThrows_
5. checkBlock(body)
6. 返回 FuncSemType
```

### 7.6 inferAssign — 赋值

```
1. const 绑定 → E015_ConstReassign 报错
2. isAssignable(target, value)
```

### 7.7 inferErrorPropagation — `!` 运算符

```
1. 非 throwing 函数且不在 try → 报错
2. insideTry_ > 0 → 放行
3. 委托 inferExpr 推断内部表达式
```

---

## 八、语句检查（Checker/StmtChecker.cpp）

### 8.1 控制流检查

| 函数 | 关键逻辑 |
|:---|:---|
| `checkBlock` | 推入作用域 → 遍历子语句 → 退出作用域 |
| `checkIfStmt` | 条件 bool → thenBranch → elseIfs → elseBranch |
| `checkWhileStmt` | 条件 bool → insideLoop_=true → body → 恢复 |
| `checkForStmt` | 迭代器类型推断 → 推导元素类型 → 注册变量 → insideLoop_ |
| `checkLoopStmt` | insideLoop_=true → body → 恢复 |
| `checkReturnStmt` | 返回值 isAssignable(currentReturnType_)；inLockBlock_ 禁止 |

### 8.2 声明检查

| 函数 | 关键逻辑 |
|:---|:---|
| `checkLetDecl` | 先注册占位符 → 推断初始值 → isAssignable 类型标注 → 更新类型 → propagateCanonicalName |
| `checkConstDecl` | 同上但 isConst=true；None 独立类型报错 E017 |
| `checkThrowStmt` | 非 throws 且不在 try → 报错 |

### 8.3 并发语句检查

| 函数 | 规则 |
|:---|:---|
| `checkSyncStmt` | R1: 禁嵌套 sync thread；R4: maxExpr 必须 int |
| `checkSyncForStmt` | maxExpr 可选 int；推断迭代器元素；注册 itemName |
| `checkSpawnStmt` | E018: 必须在 sync 块内；L6: lock 块内禁止；R3: sync thread 内需有参 |

### 8.4 lock 块检查（checkLockStmt）— L1/L8/L9

```
L1：遍历 lockExprs，类型必须是 Mutex/RWMutexReadView/RWMutexWriteView/Once
L8：记录 Once 位置；多锁+Once 不兼容
L9：编译期重复锁检测：
  - Identifier 同名 → duplicate lock
  - MemberAccessExpr 同字段链 → duplicate lock
  - 其他 → 放行（依赖运行时 L5）
进入 lock 块：inLockBlock_=true
```

### 8.5 match 穷尽性（isMatchExhaustive）

```
UnionSemType → 遍历变体：
  TypePattern：pattern 名 equals 变体名 → 覆盖
  WildcardPattern → 覆盖所有
  ConstantPattern(None) → 覆盖 NoneSemType
有未覆盖变体 → E014_MatchNotExhaustive
```

---

## 九、全局状态标志位语义

| 标志 | 设置时机 | 含义 | 影响 |
|:---|:---|:---|:---|
| `currentReturnType_` | checkFunBody / checkMethodBody | 当前函数返回类型 | checkReturnStmt 做 isAssignable |
| `currentFunctionThrows_` | 同上 | 当前函数是否 throws | checkThrow / inferCall 的 throws 检查 |
| `insideLoop_` | while/for/loop 块内 | break/continue 是否合法 | BreakStmt / ContinueStmt 检查 |
| `insideSync_` | sync 块内 | spawn 是否合法 | E018_SpawnOutsideSync |
| `inSyncThreadBlock_` | sync thread 块内 | 多线程标记 | R3: spawn 需有参 |
| `inLockBlock_` | lock 块内 | 禁止 return/break/continue/spawn 跨出 | L3/L4/L6 |
| `insideTry_` | try/catch 块内 | throws 相关检查放行 | inferCall / inferErrorPropagation 的 throws 检查 |
| `resolvingTypes_` | TypeDecl 解析中 | 自引用检测 | resolveNamedType 打破递归 |
| `ioSync_` | ConfigDecl `#io.sync` | 禁用异步 IO 方法 | — |

---

## 十、错误码清单

| 码 | 名称 | 触发条件 |
|:---|:---|:---|
| E013 | MethodNotFound | 方法未注册 |
| E014 | MatchNotExhaustive | match 未覆盖所有联合变体 |
| E015 | ConstReassign | const 绑定被重新赋值 |
| E016 | ThrowsViolation | 非 throws 上下文调用 throws 函数 |
| E017 | NoneStandalone | None 作为独立类型标注 |
| E018 | SpawnOutsideSync | sync 块外调用 spawn |

---

## 十一、潜在问题与待审项

### 11.1 RecordSemType::equals 忽略字段顺序

按名称匹配是正确的（结构等价），但可能有用户期望顺序敏感的场景。

### 11.2 UnionSemType::equals 顺序敏感

与 Record 不同，Union 变体顺序敏感。如果两个 union 变体列表排序不同但集合相同，视为不等价。需确认这是否符合语义预期。

### 11.3 BuiltinRegistry::findMethod 线性搜索

methods_ 是 `vector`，每次 `findMethod` 是 O(n)。在方法很少（~30 个）时无影响，但如果 `tryLoadAurai` 加载大量外部方法可能成为瓶颈。

### 11.4 inferCall 泛型推断仅支持一层

`collectGenericMapping` 从形参-实参对照推导泛型映射，但仅支持函数级别的泛型参数（`typeParams` 列表），不支持嵌套泛型传递。

### 11.5 Statement 遗漏分支

`checkStmt` 的 dynamic_cast 链中，不认识的类型不报错（"不处理"）。如果有新增语句类型但忘记在此注册，会静默跳过。

### 11.6 RecordExpr canonicalName 在非具体情况时未生成

`materializeCanonicalName` 在 typeArgs 中存在 GenericTypeRef 时 `allConcrete = false` 不设置 `canonicalName`。CodeGen 依赖此名称生成 C++ record 类型名，缺失时会导致编译错误。

---

**审查结论**：类型系统完整（10 种 SemType 正交覆盖），符号表正确处理自引用和重载，表达式推断覆盖 20 种表达式类型，语句检查覆盖 15 种语句含 9 条锁/并发规则。主要风险点：CanonicalName 未生成时的 CodeGen 影响、match 穷尽性仅检查 Union（不检查其他枚举）、泛型深层传递不支持。
