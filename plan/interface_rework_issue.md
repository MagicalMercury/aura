# Interface 改造 issue：C++ 虚函数层限制 + 内置常见接口

> 状态：issue 草稿（待审查）
> 提出日期：2026-08-02
> 来源：newIssue.txt §1
> 关联 TODO：TODO.txt §二（Iterator<T> 高级类型、Callable<T> 概念类型）

## 一、issue 目标（要做什么）

1. **让接口真正在 C++ 层以虚函数（或等价机制）生效**：实现接口的具体类型（record）在 CodeGen 生成时继承对应抽象基类并 override 虚方法，使接口参数获得运行时多态（"在 C++ 层面做出限制"）。
2. **补齐"具体类型 → 接口"的结构类型匹配**：当前 Sema 的 isAssignable 对具体类型直接 `return false`，与 README 文档承诺（"任何类型拥有同名同签名方法即自动实现接口"）矛盾，必须修。
3. **内置常见接口**：
   - `Printable`（更名建议 `Stringer`，取自 READMEs/15-example.md 已有雏形，语义"对象可以变成 string"）
   - `Comparable`（比较接口）
   - `Iterator`（迭代接口，与 TODO §二 Iterator<T> 高级类型整合）

## 二、详细现状（文档 vs 源码对照）

### 2.1 文档承诺

READMEs/07-methods-interfaces.md §7.3：
- `interface Greetable { greet() -> string }` 声明语法
- **"任何类型拥有同名同签名方法即自动实现接口（结构类型）"** ← 文档承诺
- `fun (self User impl Greetable) greet() -> string`：`impl` 可选验证
- 接口可作为类型使用：`fun welcome(g: Greetable, io: Io)`

READMEs/15-example.md L8-19：`Stringer { to_string() -> string }` 接口雏形 + `fun (self User impl Stringer) to_string()` —— 但**从未作为类型参数使用过**，接口作为类型的路径无实测用例。

### 2.2 Parser / AST 层（已实现）

- `InterfaceDecl`（AST/Stmt.h）：`name` + `methods` 方法签名列表
- `parseInterfaceDecl`（Parser/DeclParser.cpp:108-124）：解析 `interface Name { ... }`
- `MethodDecl.implInterface`（Parser/DeclParser.cpp:146-149）：`fun (self X impl Greetable)` 可选标注
- **不支持**：泛型接口、接口方法默认实现、接口继承

### 2.3 Sema 层（部分实现 + 一处硬伤）

- 接口符号注册：declareDecl（DeclChecker.cpp:122-138），方法签名存入 `Symbol::interfaceMethods`
- `InterfaceSemType{ name, methods }`（SemType.h:87-97）；resolveNamedType 构建（SemAnalyzer.cpp:213-225）
- **硬伤**：`isAssignable` 接口分支（SemAnalyzer.cpp:366-377）：

  ```cpp
  if (auto* iface = dynamic_cast<const InterfaceSemType*>(&target)) {
      if (auto* func = dynamic_cast<const FuncSemType*>(&source)) {
          if (iface->methods.size() == 1) {
              // 单方法接口 ← 闭包（函数类型）：结构匹配通过
              return matchFuncSig(...);
          }
          return false;
      }
      return false; // 非函数类型不能满足接口  ← 具体 record 类型直接被拒
  }
  ```

  → 具体类型对象传给接口参数（`welcome(u, io)` where `u: User`）在 checkCallArgs 阶段报类型不匹配，**结构类型自动实现未落地**。
- `impl` 验证（DeclChecker.cpp:456-503）：逐方法比对参数数量/类型/返回类型/throws —— 只做**一致性校验**，不建立"类型满足接口"的能力，也不注册任何运行时关系。

### 2.4 CodeGen 层（抽象基类已生成但无人继承）

- `genInterfaceDecl`（DeclGen.cpp:124-170）：
  - 抽象基类：`struct Greetable { virtual ~Greetable() = default; virtual string greet() const = 0; };`
  - **仅单方法接口**额外生成适配器 `GreetableFunc : Greetable { std::function<...> func; ... }`（闭包专用）
- 接口参数映射：`const Greetable&`（TypeMap.cpp mapParamType:181-184；mapType:156 接口名保留原名）
- 调用点自动包装：`GreetableFunc(实参)`（ExprGen.cpp:628-637）—— 仅闭包可构造 std::function
- 接口方法调用：`g.greet()` 直接虚调用（valueTypeVarNames_ 注册接口参数 → `.` 访问）
- 接口按**非 GC 堆对象**处理（ExprGen.cpp isHeapSemType:18-22）
- `interfaceNames_` 收集（CodeGen.cpp:91）、genInterfaceDecl 生成（CodeGen.cpp:188）
- **核心缺陷**：具体类型（record）生成时**不继承接口抽象基类**——没有 `struct User : Greetable { greet() override }` 的产出路径。虚函数机制生成出来但闲置，唯一可用路径是"闭包 → 单方法接口"的 std::function 适配。

### 2.5 现状小结

| 方面 | 现状 | 问题 |
|---|---|---|
| 接口声明 / 抽象基类生成 | ✅ | — |
| 单方法接口 ← 闭包 | ✅ Sema+CodeGen 通 | 仅闭包，std::function 包装，非虚函数多态 |
| 具体类型 → 接口参数 | ❌ Sema 拒绝 | 与文档承诺矛盾 |
| record 继承抽象基类 + override | ❌ 无产出路径 | 虚函数机制闲置 |
| 多方法接口实例化 | ❌ 无 | 仅单方法接口生成 Func 适配器 |
| 接口作 record 字段 / Array 元素 | ⚠️ 未验证 | mapType 返回接口名，值类型存抽象类有风险 |
| 内置接口 | ❌ 无 | 仅文档示例 Stringer 雏形；Comparable/Iterator 无 |

## 三、接口 C++ 生成方案对比（关键设计决策）

> 问题：一个 record 接入多个接口（如 `User : Greetable, Comparable, Iterator<T>`），
> 若用 C++ 多重继承生成，结构难看且 GC 布局被污染。以下对比可行方案。

### 3.1 方案 A：多重继承 + 虚函数 override（现状生成方式扩展，不推荐）

```cpp
// record 直接继承所有接口抽象基类
struct User : aura_rt::GcObject, Greetable, Comparable {
    aura_rt::GcString* name;
    // 接口方法 override
    aura_rt::GcString* greet() const override { return name; }
    int32_t compare_to(const User& other) const override { ... }
};
```

**难看之处 / 代价：**
1. **GC compact 指针调整灾难**：GC 移动对象更新的是 `User*`，但接口引用是 `Greetable*`（位于 User 对象 +8/+16 偏移的子对象），`static_cast<Greetable*>` 需做 this 指针偏移调整——compact 后必须对每个基类子对象指针重算，GC 层要感知接口继承关系。
2. **GC 扫描字段偏移全乱**：record 字段按 GcObject 基类布局计算偏移，接口基类插入后 `offsetof(name)` 后移，且 GC 不知道接口基类内部的虚表指针/成员，扫描器需跳过。
3. **菱形继承**：两个接口间接继承同一接口 → 必须虚继承，进一步复杂化。
4. **record 定义与接口集合强耦合**：每接入一个接口就改 record 生成代码。
5. **跨模块接口实现**：模块 B 给模块 A 的类型接接口时，无法修改 A 的 record 定义。

### 3.2 方案 B：类型专属适配器（trait object 风格，**推荐**）

record 保持 `struct User : GcObject` 不变（GC 布局零影响），为每个 "类型 × 接口" 组合生成一个薄适配器：

```cpp
// ① 接口声明 → 抽象基类（虚函数表）
struct Greetable {
    virtual ~Greetable() = default;
    virtual aura_rt::GcString* greet() const = 0;
};
struct Comparable {
    virtual ~Comparable() = default;
    virtual int32_t compare_to(const Comparable& other) const = 0;
};

// ② record 保持纯粹，不感知接口（GC 扫描/compact 零改动）
struct User : aura_rt::GcObject {
    aura_rt::GcString* name;
    aura_rt::GcString* greet() const;      // 普通方法，与接口无关
    int32_t compare_to(const User& other) const;
};

// ③ 使用点自动生成适配器（User × Greetable）
struct UserGreetable final : Greetable {
    User* obj;
    explicit UserGreetable(User* o) : obj(o) {}
    aura_rt::GcString* greet() const override { return obj->greet(); }
};
struct UserComparable final : Comparable {
    User* obj;
    explicit UserComparable(User* o) : obj(o) {}
    int32_t compare_to(const Comparable& other) const override {
        return obj->compare_to(*static_cast<const UserComparable*>(&other)->obj);
    }
};

// ④ 传参点：welcome(u) → welcome(UserGreetable(&u))（u 为 GcRootHandle，取 .get()）
void welcome(const Greetable& g, aura_rt::Io& io);
```

**优点：**
- record 与接口完全解耦；GC 零改动；无菱形继承问题
- 接口引用即单一 `const Greetable&`，虚函数多态成立
- 适配器为栈上临时对象，生命周期覆盖整个调用，传参场景 GC 安全（对象指针来自当前帧的 GcRootHandle）
- 与现有 `GreetableFunc`（std::function 闭包适配器，DeclGen.cpp:124-170）思路同源，可统一改造为虚函数适配器，**同时支持闭包与具体类型**两个来源

**代价 / 待定：**
- 接口对象**长期持有**（存 record 字段 / Array 元素 / 返回接口类型）时：适配器持 `User*` 会被 GC compact 失效——v1 需限制接口类型仅作参数/局部使用，或引入 `GcRootHandle<Iface*>` 式 GC 感知包装（后续 P 级扩展）
- 每 "类型 × 接口" 组合一个适配器类 → 惰性按使用点生成即可（未被使用的组合不生成）

### 3.3 方案 C：std::function 字段化接口（Go 鸭子类型，备选）

```cpp
struct Greetable {
    std::function<aura_rt::GcString*()> greet;   // 接口 = 一组 std::function 字段
};
// 调用点：welcome(Greetable{ [o = u.get()] { return o->greet(); } });
```
- 优点：可拷贝、可存字段、无继承、接口就是数据（最灵活）
- 缺点：std::function 堆分配 + 类型擦除调用开销；lambda 捕获 GC 指针 compact 后悬垂（同 B 的持有问题）；不如 B 直观

### 3.4 方案 D：Go 风格 interface 值（类型指针 + 数据指针）

```cpp
struct GreetableValue { void* data; const GreetableVtbl* vtbl; };
// 调用点：GreetableValue{ &u, &kUserGreetableVtbl }
```
- 与方案 B 本质等价（B 是该思想的直接虚函数实现），B 更简单、避免手工维护 vtbl

### 3.5 接口默认方法（模式 1，**已定稿**）

> 决策：一个接口多个函数，其中若干派生态有默认实现、基于用户必须实现的核心方法时，
> 采用 **接口默认方法**（Java `default` / Rust trait 默认实现 / C++ 非纯虚函数）。
> 典型场景：`Iterator<T>` 用户只实现 `next()`，即可免费获得 `map` / `filter` / `collect`。

**Aura 语法扩展**：接口方法带函数体 = 默认实现；无函数体 = 用户必须实现。

```aura
interface Iterator<T> {
    next() -> Optional<T>                             // 无体：必须实现（纯虚）
    map(f: fun(T) -> U) -> Iterator<U> { ... }        // 有体：默认实现，内部 this->next()
    filter(p: fun(T) -> bool) -> Iterator<T> { ... }
    collect() -> [T] { ... }
}
```

**生成 C++**（与方案 B 适配器天然契合——默认实现写在抽象基类，适配器继承即得）：

```cpp
// 接口声明 → 抽象基类：核心方法纯虚，派生态非纯虚（默认实现）
struct Iterator {
    virtual ~Iterator() = default;
    virtual Optional<T>* next() = 0;                  // 纯虚：用户/适配器必须实现
    virtual Iterator<U>* map(f)  { ... this->next() ... }   // 默认实现
    virtual Iterator<T>* filter(p) { ... this->next() ... }
    virtual Array<T>*    collect() { ... this->next() ... }
};

// 用户类型适配器只需 override 核心方法
struct UserIterator final : Iterator<int> {
    Optional<int>* next() override { return user->next(); }
    // map / filter / collect 继承默认实现，开箱即用
};
```

**关键设计细节：**
1. **默认实现内部访问核心方法**：C++ 直接 `this->next()` 虚调用
2. **默认实现返回 `Iterator<U>`**：需生成内部实现类（如 `MapIterator`，由默认实现体生成）；
   v1 限定默认实现的返回类型须可表达（接口类型 / 基础类型）
3. **默认实现可被 override**：适配器按需优化（通常不需要）
4. **解析/CodeGen 改动面**：Parser 接口方法体解析；Sema 默认实现体内 `self` 虚调用校验 +
   必须实现方法（纯虚）集合计算；CodeGen 非纯虚生成 + 内部实现类生成

### 3.6 待审设计问题

1. **内置接口方法签名不能依赖具体类型**（关键）：
   - `Comparable.compare_to(other: ???)` 参数不能写 `User`——应设计为 `compare_to(other: Comparable) -> int`（用接口自身类型，适配器内 downcast 回具体类型），或泛型方法
   - 同理 `Iterator<T>.next() -> Optional<T>` 的 T 如何绑定
2. **GC 持有策略**：接口对象长期存储 v1 限制 vs v2 引入 GC 感知接口包装
3. **str() 衔接**：内置 Stringer 与现有 `str()`/`string_of` 的关系（`str(obj)` 是否自动走 Stringer 接口）

## 四、初步方向（框架性，供审查）

1. **Sema 补齐结构匹配**：isAssignable 增加"具体 record 类型 → 接口"分支——检查 source 类型是否拥有接口全部同名同签名方法（含继承关系、方法表查找）。
2. **CodeGen 生成类型专属适配器**（方案 B，非 record 继承）：为使用点生成 `UserGreetable final : Greetable { User* obj; ... }`，传参时自动包装；统一改造现有 `GreetableFunc` 闭包适配器为虚函数适配器，同时支持闭包与具体类型两个来源。
3. **GC 影响评估**：适配器为栈上临时对象（传参/局部场景 GC 安全）；接口类型长期持有（字段/Array/返回值）在 v1 限制或引入 GC 感知接口包装——这是本 issue 最大风险点。
4. **内置接口**：
   - `Stringer { to_string() -> string }`（Printable 更名；与现有 GcString 体系、str() 转换衔接）
   - `Comparable { compare_to(other) -> int }` 或 `lt/eq`
   - `Iterator<T> { next() -> Optional<T> }`（与 TODO §二 Iterator<T> 整合，支持 .map/.filter/.collect 链式）
5. **文档同步**：READMEs/07-methods-interfaces.md §7.3 现状描述需重写（结构类型承诺 → 实际机制）。

## 五、关联 TODO 条目

- TODO §二 `[ ] P2 Iterator<T> 高级类型` —— 与内置 Iterator 接口整合
- TODO §二 `[ ] P2 Callable<T> 概念类型` —— "建议先做接口扩展"，本 issue 即该扩展
- TODO §三 `[ ] P2 用户自定义 .aurai 接口文件` —— 接口机制完善后的导出机制（可顺带）
- TODO §四 `[ ] P2 闭包赋值给接口类型参数` —— 本 issue 单方法接口→闭包已有基础，泛型参数化场景需验证

> 实现草案已移至：plan/interface_rework_plan.md
