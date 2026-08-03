# Interface 改造——详细实施方案（工作流程 3，待审查）

> 状态：详细实施方案（待审查）
> 提出日期：2026-08-02
> 来源 issue：plan/interface_rework_issue.md（现状/方案对比/默认方法定稿见该文件 §一~§五）
> 本文件为 plan_rule 模板的完整实施方案（上一版草案已废弃，内容全部替换）

## 一、目标与范围

1. 具体 record 类型可传接口参数：Sema 结构匹配 + CodeGen 类型专属适配器（方案 B），获得 C++ 虚函数多态。
2. **GC 适配**：接口适配器持值持有根（GcRootHandle 模式 B），解决"被调函数体内 GC compact 移动对象 → 适配器裸指针悬垂"问题。
3. 接口默认方法（模式 1）：接口方法带函数体即默认实现。
4. 内置 `Stringer` / `Comparable` / `Iterator<T>`。

**v1 范围裁剪**（需审查确认）：
- 接口类型仅作参数/局部变量；作 record 字段/Array 元素/函数返回值 → Sema 报错
- 接口参数传**协程函数**（co_await 调用）→ 报错（临时适配器生命周期问题，见 §4.4）
- 默认方法返回接口类型（如 `Iterator<U>`）→ 暂不支持（内部实现类生命周期黑洞，见 §八）；内置 Iterator 先提供 `next()` + `collect()`（返回 Array，基础类型）

## 二、现状关键结论（详见 issue 文件 §二）

- `isAssignable` 接口分支 [SemAnalyzer.cpp:366-377](file:///d:/you/Aura/src/Sema/SemAnalyzer.cpp#L366-L377)：仅单方法接口←闭包；具体类型 `return false`
- 方法以**全局名**注册（[DeclChecker.cpp:167-182](file:///d:/you/Aura/src/Sema/Checker/DeclChecker.cpp#L167-L182)），**无"类型→方法"映射**——结构匹配前置依赖
- CodeGen：`genInterfaceDecl`（[DeclGen.cpp:124-170](file:///d:/you/Aura/src/CodeGen/DeclGen.cpp#L124-L170)）抽象基类 + `IfaceFunc`（std::function 闭包适配器）；`fnInterfaceParams_`（[DeclGen.cpp:197-206](file:///d:/you/Aura/src/CodeGen/DeclGen.cpp#L197-L206)）；调用点包装 `IfaceFunc(arg)`（[ExprGen.cpp:621-637](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L621-L637)）；接口参数→`const&`（[TypeMap.cpp:178-187](file:///d:/you/Aura/src/CodeGen/TypeMap.cpp#L178-L187)）
- 接口方法调用 `g.greet()` 走 `valueTypeVarNames_`（`. ` 访问，[ExprGen.cpp:1303-1305](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L1303-L1305)）

## 三、GC 适配设计（核心）

### 3.1 问题分析

方案 B 适配器若持裸指针：

```cpp
struct UserGreetable final : Greetable {
    User* obj;                              // 裸指针：GC compact 移动 User 后悬垂
    GcString* greet() const override { return obj->greet(); }
};
// 调用点：welcome(UserGreetable(&u), io)
```

- **调用点表达式内**：`&u` → 构造临时 → welcome 调用，同一完整表达式内无 safepoint（GC 只在分配/显式点触发）→ 安全
- **welcome 函数体内**：`g: const Greetable&` 引用该临时适配器；若体内调用 io.println()/分配等触发 GC compact，User 被移动 → `UserGreetable.obj` 仍是旧地址 → `g.greet()` 悬垂崩溃

**根因**：适配器内部对象指针不是 GC 根，GC 期间不被更新。

### 3.2 方案：根引用类统一——`GcRootHandle<T>` 三模式（替代 GcStackRoot 提案）

**决策**：不新增 `GcStackRoot`，而是将现有根引用类族统一为**一个 `GcRootHandle<T>`**（模式 A/B/C 由构造重载决定）。理由：
- 现有 3 个类本质差异只有 2 个维度：**槽位位置**（引用外部变量 vs 内部持值）、**根作用域**（线程局部 vs 全局）；拷贝性只是闭包场景的派生物
- 接口适配器的"值持有+线程局部"需求 = 现有语义的一个新组合，统一类用构造参数表达即可，避免类族膨胀

**runtime 改造**（`runtime/gc/gc.h` 声明 + `runtime/gc/handles.h` 实现）：

```cpp
enum class GcRootScope { ThreadLocal, Global };
enum class GcRootMode : uint8_t { Ref, ValueThreadLocal, ValueGlobal };

template <typename T>
class GcRootHandle : public GcRootHandleBase {
public:
    GcRootHandle(T& ref);                        // 模式A：引用外部变量（线程局部）← CodeGen 栈变量（现有语义零变化）
    GcRootHandle(T val, GcRootScope scope);      // 模式B/C：值持有，scope 显式指定（⚠️ 无默认值，避免与 T& 重载歧义）
                                                 //   ThreadLocal ← 接口适配器；Global ← 闭包捕获/全局缓存
    GcRootHandle(const GcRootHandle& other);     // 拷贝：按 other.mode_ 分支（Ref→引用同一变量；Value→深拷贝值+独立注册）
    GcRootHandle& operator=(const GcRootHandle&) = delete;
    void rebind(T& ref);                         // 仅 Ref 模式
    T& get() { return *ptr_; }                   // Ref：外部变量；Value：内部 val_
    T  get() const { return *ptr_; }
    void set(T v);                               // Ref 写外部变量；Value 写内部 val_
private:
    union { T* ptr_; T val_; };  // Ref 用 ptr_（&外部变量）；Value 用 val_（内部持值）——共享 8B 槽（内存优化，见下）
    GcRootMode mode_;            // 1B：Ref/ValueTL/ValueGlobal——拷贝构造与析构据此分支（Bug 2 修复）
};
```

**构造歧义（Bug 1 修复）**：值持有构造**必须带 scope 参数**（2 参 `(T val, GcRootScope)`），与模式 A（1 参 `T&`）签名不同、无歧义。**删除单参数 `GcRootHandle(T val)` 重载**——否则左值实参永远优先匹配 `T&`（模式 A），值持有无法表达。适配器生成必须显式写 `obj(o, GcRootScope::ThreadLocal)`。

**拷贝语义（Bug 2 修复）**：`mode_` 为 3 值枚举，拷贝构造读 `other.mode_` 分支：
- `Ref` → `ptr_ = other.ptr_`（引用同一外部变量）+ 注册线程局部根（现语义）
- `Value*` → `val_ = other.get()` + `ptr_(&val_)` + 按 mode 注册（深拷贝值，闭包捕获语义）

**内存布局**（T=指针类型，64 位）：

| 成员 | 大小 | 偏移 |
|---|---|---|
| GcRootHandleBase（next_/prev_/ptr_ref_） | 24B | 0 |
| union { ptr_; val_ } | 8B | 24 |
| mode_（uint8_t）+ padding | 8B | 32 |
| **合计** | **40B** | |

对比：现有 GcRootHandle 32B（基类 24 + ptr_ 8）。统一后 **+8B（+25%）**；朴素双字段方案 48B（+50%）被 union 省去。模式 A（栈变量，绝大多数场景）只用 union 的 ptr_ 半槽，无浪费。40B×10 个变量 ≈ 400B 栈，相对协程帧（KB 级）可忽略。
进一步压回 32B 需 tagged pointer（mode 编码进指针低 bit），读写全部掩码、风险高，v1 不做。

**实现要点**：
- 构造 A：`ptr_(&ref), mode_(Ref)` → `registerRootThreadLocal(this)`（现行为）
- 构造 B：`val_(val), ptr_(&val_), mode_(ValueThreadLocal)` → `registerRootThreadLocal(this)`
- 构造 C：`val_(val), ptr_(&val_), mode_(ValueGlobal)` → `registerGlobalRoot(ptr_ref_)`（GcHeap 独立容器，不经线程链表）
- 析构：按 `mode_` 分支注销（Ref/ValueTL 走线程链表；ValueGlobal 走 GcHeap 容器）
- 为什么省不掉 mode_：析构必须知道走哪条通道注销；拷贝构造必须知道 union 里是 ptr_ 还是 val_。next_/prev_ 不能作判据（单节点链表的 head 节点两者皆 nullptr）

**迁移**：
| 旧类 | 新写法 | 迁移点 |
|---|---|---|
| `GcSharedRoot<T>(val)`（闭包捕获） | `GcRootHandle<T>(val, GcRootScope::Global)` | ExprGen.cpp:1224（1 处） |
| `GcGlobalRoot<Obj>(ptr)`（全局缓存） | `GcRootHandle<Obj*>(ptr, GcRootScope::Global)` | string.cpp:68/74/101/102/160/661/753（7 处；**类型参数对象→指针**） |
| `GcStackRoot<T>(val)`（提案） | `GcRootHandle<T>(val, GcRootScope::ThreadLocal)` | 本 plan 阶段 3 |

删除 `GcGlobalRoot`/`GcSharedRoot` 类声明（`GcWeakHandle` 弱引用语义独立，保留）。

**适配器生成形态**（阶段 3 产物）：

```cpp
struct UserGreetable final : Greetable {
    aura_rt::GcRootHandle<User*> obj;                     // 模式B（值持有+线程局部）：GC 期间自动更新
    explicit UserGreetable(User* o)
        : obj(o, aura_rt::GcRootScope::ThreadLocal) {}    // ⚠️ 必须显式 scope（Bug 1：否则左值 o 匹配模式 A，引用悬垂）
    aura_rt::GcString* greet() const override { return obj.get()->greet(); }
};
```

**调用点**（genCallExpr 生成）：

```cpp
// u 为 GcRootHandle<User*>（Aura 堆对象变量），obj.get() 取当前指针
welcome(UserGreetable(u.get()), io);
```

**GC 生命周期推演**：
1. 构造临时 `UserGreetable(u.get())`：`obj` 注册线程局部根，GC 可发现并更新 `UserGreetable.obj` 指向对象
2. `welcome` 体内任意 GC compact：`UserGreetable.obj` 被更新为新地址；同时调用者 `u`（GcRootHandle）也被更新——两处一致
3. `g.greet()` 虚调用：`obj.get()` 返回最新指针 → 安全
4. 完整表达式结束，临时适配器析构：`obj` 注销根

### 3.3 为什么不用旧类 / 原提案

| 方案 | 问题 |
|---|---|
| 裸 `User*` | 函数体内 GC compact 悬垂（3.1） |
| 原 `GcSharedRoot`（现统一为模式 C） | 全局根 + 堆分配，适配器每调用构造 new + 全局锁，性能差；语义偏长期持有 |
| 原 `GcRootHandle<T&>` 引用模式（现模式 A） | 构造需 `T&`（用户栈变量），临时适配器无法绑定右值；且会额外更新外部变量，语义不符 |
| 原提案 `GcStackRoot`（已废弃，现为模式 B） | 与统一后的 GcRootHandle 模式 B 完全等价，无需独立类 |
| 抽象基类 virtual 返回对象指针 + 函数体入口根化 | 适配器内部 obj 仍 stale（根化在适配器外无效）——根化必须在适配器内部 |

### 3.4 生命周期边界与限制

- **C++ 临时对象生命周期**：`welcome(UserGreetable(u.get()), io)` 中临时适配器析构于完整表达式结束 → 覆盖 welcome 全程 ✓
- **协程边界**：`co_await welcome(...)` 中临时对象在 co_await 挂起点可能提前析构（C++20 已知坑）→ **v1 禁止接口参数传给协程函数**（Sema 在 checkCallArgs 检测：函数在 coroutineFunctions_ 且参数含接口类型 → 报错）
- **多线程（sync）**：模式 B 为线程局部根，worker 线程 GC 时只扫本线程根——适配器构造/使用在同一 worker 线程内（传参调用同线程）✓
- **嵌套接口调用**：`welcome(g.greet2(...))` 多层适配器各自独立根化 ✓

## 四、详细变更（分阶段）

### 阶段 1：Sema "类型→方法"映射

- **文件**：`src/Sema/SemAnalyzer.h`（新增成员）、`src/Sema/Checker/DeclChecker.cpp`（declareDecl 登记）、`src/Sema/SemType.h`（复用 MethodSig）
- **接口契约**：
  ```cpp
  // SemAnalyzer 新增成员（声明后初始化，方法已全局注册完）
  std::map<std::string, std::vector<InterfaceSemType::MethodSig>> typeMethods_;
  ```
- **构建时机（Bug 9 修复）**：在 **`declareTopLevel` 完成之后（第 1 遍末尾）统一构建**，而非 declareDecl 内逐条登记——理由：`recordTypeKey` 依赖 `resolveType(receiverType)` 得到 RecordSemType.canonicalName，而 declareDecl 阶段 record 类型声明顺序不保证（前向/自引用类型可能尚未注册，resolveType 返回 ErrorSemType）。第 1 遍末尾所有类型已声明，resolveType 安全：
  ```cpp
  void SemAnalyzer::buildTypeMethods(const Program& program) {
      for (auto& d : program.decls) {
          if (auto* m = dynamic_cast<const MethodDecl*>(d.get())) {
              if (!m->receiverType) continue;
              std::string key = recordTypeKey(*m->receiverType);   // resolveType 此刻安全
              if (key.empty()) continue;
              InterfaceSemType::MethodSig sig;                      // 数据源：第 1 遍已解析的 sym
              sig.name = m->name;
              // 从符号表查已注册 Method 符号，复制 sym.params/sym.type
              typeMethods_[key].push_back(std::move(sig));
          }
      }
  }
  ```
  （也可直接 resolveType 各 param/returnType——第 1 遍末尾同样安全；取符号表 sym 更省）
- **implInterface 与 typeMethods_ 无关**：`decl.implInterface` 的一致性验证仍走第 2 遍 checkMethodBody（现有 L455-503），结构匹配不依赖用户是否写 impl 声明——只要 record 有同名同签名方法即视为实现（README 结构类型语义）。
- **键名统一（Bug 3 修复）**：登记与查询**共用同一规范名来源**——新增 helper：
  ```cpp
  // receiverType (TypeExpr) → 规范名；buildTypeMethods 与 isAssignable 查询都走这里
  std::string SemAnalyzer::recordTypeKey(const TypeExpr& receiverType) {
      auto resolved = resolveType(receiverType);
      if (auto* rec = dynamic_cast<const RecordSemType*>(resolved.get()))
          return rec->canonicalName;   // 与 RecordSemType.canonicalName 完全一致
      return "";                        // 非 record 接收者（v1 接口仅支持 record 实现）
  }
  ```
  禁止登记侧用 AST 字符串（"User"）、查询侧用 canonicalName（可能含命名空间/实例化差异）造成键不一致。

### 阶段 2：Sema isAssignable 结构匹配

- **文件**：`src/Sema/SemAnalyzer.cpp:366-377`（接口分支）
- **逻辑**（伪码）：
  ```
  if (target 是 InterfaceSemType iface) {
      if (source 是 FuncSemType) { 保持现有单方法闭包路径 }
      if (source 是 RecordSemType rec) {
          for each iface.method m:
              typeMethods_[rec.canonicalName] 中查找同名方法（含继承链——v1 无类型继承，仅查自身）
              比对参数/返回/throws（复用 matchFuncSig 逻辑）
              缺任一 → return false
          return true
      }
      return false
  }
  ```
- **错误信息**：checkCallArgs 报"type 'X' does not satisfy interface 'Y': missing method 'Z'"（列出缺失方法名）

### 阶段 3：CodeGen 类型专属适配器（方案 B）+ GcRootHandle 模式 B

- **文件**：
  - `runtime/gc/gc.h` / `runtime/gc/handles.h`：GcRootHandle 三模式统一（§3.2）；删除 GcGlobalRoot/GcSharedRoot
  - `runtime/builtin/string.cpp`：7 处 GcGlobalRoot → `GcRootHandle<GcString*>(..., GcRootScope::Global)`（迁移）
  - `src/CodeGen/ExprGen.cpp:1224`：闭包捕获 GcSharedRoot → `GcRootHandle<T>(..., GcRootScope::Global)`（迁移）
  - `src/CodeGen/CodeGen.h`：新增 `ifaceAdapterCache_`（`std::set<std::string>` 记录已生成组合名）
  - `src/CodeGen/CodeGen.cpp`：第一遍扫描收集组合（见下）；`genIfaceAdapter` 新函数
  - `src/CodeGen/DeclGen.cpp`：`genInterfaceDecl` 保持抽象基类输出；追加适配器生成入口
  - `src/CodeGen/ExprGen.cpp:621-637`：调用点双源包装
- **组合收集（第一遍）**（Bug 4 修复：覆盖全部 record→接口 转换点）：
  1. 所有 FunDecl/MethodDecl 参数（复用 fnInterfaceParams_ 构建逻辑，但需要参数类型名→具体 record 名）
  2. 函数体调用表达式（预扫描：接口类型实参）——**genCallExpr 与 genMethodCall 两处调用点都覆盖**
  3. **接口透传放行**：调用点实参静态类型已是接口（`welcome` 参数 g 透传给 `foo(g)`）→ 不生成新适配器（已在传参处构造）
  4. **泛型 record**：组合按实例化名收集（`User<int>` × Greetable），适配器名随实例化生成
  - 存 `std::set<std::pair<std::string,std::string>>`（record 名, 接口名）
- **生成形态**（header，record 定义之后）：
  ```cpp
  struct UserGreetable final : Greetable {
      aura_rt::GcRootHandle<User*> obj;                     // 模式B：值持有+线程局部根
      explicit UserGreetable(User* o)
          : obj(o, aura_rt::GcRootScope::ThreadLocal) {}    // ⚠️ 显式 scope（与 §3.2 一致；单参数会匹配模式 A）
      aura_rt::GcString* greet() const override { return obj.get()->greet(); }
  };
  ```
- **scope 的确定是编译期静态的，非运行时判断**：CodeGen 在生成每个 GcRootHandle 构造时**写死** scope 值——适配器成员恒为 `ThreadLocal`（栈上临时对象，同线程使用），闭包捕获恒为 `Global`（跨线程 lambda）。运行时不存在"判断当前在哪个区域"：对象构造时 mode_ 已写入，析构按 mode_ 走对应通道即可。
  - 每个接口方法生成转发；参数/返回类型 mapType 映射
- **调用点包装**（ExprGen）：
  ```
  对 fnInterfaceParams_ 命中的参数 idx：
    if 实参 inferredType 是 FuncSemType（闭包）→ IfaceFunc(arg)（保留现有路径）
    else if 实参 inferredType 是 RecordSemType → 适配器名(arg.get() 或 arg 裸指针取地址)
    else → 报错（v1 仅支持闭包与具体 record）
  ```
- **valueTypeVarNames_**：接口参数仍注册（`. ` 访问不变）；局部 `let g: Greetable = obj` 场景 v1 报错

### 阶段 4：接口默认方法（模式 1）

- **文件**：`src/Parser/TypeParser.cpp:231-249`（parseInterfaceMethodSig）、`src/AST/Stmt.h:397-402`（InterfaceMethodSig 加字段）、`src/Sema/Checker/DeclChecker.cpp`、`src/CodeGen/DeclGen.cpp:124-170`
- **AST 契约**：`InterfaceMethodSig` 加 `std::unique_ptr<BlockStmt> defaultBody;`（clone 同步）
- **Parser**：方法签名后 `match(TokType::LBrace)` → `defaultBody = parseBlock()`（注意接口内 `{` 与闭包语法区分——接口方法签名后直接 `{`）
- **Sema**：
  - 接口声明后：有 defaultBody 的方法体内校验（参数/`self` 引用、返回类型 inferExpr 比对）
  - 计算"必须实现集合"：无 defaultBody 的方法
  - 默认方法体内 `this->otherMethod()` 虚调用——`self` 语义（默认方法体用 `self` 指代接口自身，CodeGen 映射 this）
- **CodeGen**（genInterfaceDecl）：
  ```
  无 defaultBody → virtual T m(...) const = 0;
  有 defaultBody → virtual T m(...) const { <body 生成，self→this> }
  ```
  - 默认方法体生成复用 genBlock（函数体生成器），`self` 映射 this（复用 currentReceiverName_ 机制）
  - **v1 限制**：默认方法返回接口类型 → Sema 报错（裁剪，见 §一）

### 阶段 5：内置接口 + 泛型接口支持

- **前置**：Parser 接口支持泛型参数（`interface Iterator<T>`）——`parseInterfaceDecl` 复用类型参数解析（参照 TypeDecl 的 typeParams），InterfaceDecl 加 `typeParams` 字段，**clone() 同步复制 typeParams**（Bug 6：字段与 clone 必须同改，否则深拷贝丢泛型参数）；Sema/CodeGen 相应模板化（interfaceNames_ 仍按名注册，泛型参数在方法签名内解析）
- **文件**：`builtins/interfaces.aurai`（新建，独立于 builtin.aurai）、`src/Module/ModuleManager.cpp`（loadBuiltinAurai 追加）、`src/Sema/BuiltinRegistry.h`
- **内容**：
  ```aura
  interface Stringer {
      to_string() -> string
  }
  interface Comparable<T> {
      cmp(other: T) -> int               // 三路比较核心（<0/=0/>0）：实现它 → 全部符号自动
      equal(other: T) -> bool { return cmp(other) == 0 }    // ==
      ne(other: T) -> bool { return !equal(other) }         // !=
      less(other: T) -> bool { return cmp(other) < 0 }      // <
      greater(other: T) -> bool { return cmp(other) > 0 }   // >
      le(other: T) -> bool { return !greater(other) }       // <=
      ge(other: T) -> bool { return !less(other) }          // >=
  }
  ```
  - **Comparable 泛型化（Bug 5 修复）**：`cmp(other: T)` 参数是**具体类型**而非接口自身，消除 downcast UB：
    ```aura
    fun (self Point impl Comparable<Point>) cmp(other: Point) -> int { ... }  // other 直接是 Point
    ```
    适配器 `struct PointComparable : Comparable<Point> { Point* obj; int cmp(Point* o) override { return obj->cmp(o); } }`——无 `static_cast`。非泛型版本 `cmp(other: Comparable)` 必须在接口方法内 downcast（`static_cast<const PointComparable*>(&other)`），混类型传入即 UB。
  - **MethodDecl AST/Parser 契约（Bug 8 补充）**：`impl Comparable<Point>` 需存储类型实参——
    - [Stmt.h:462](file:///d:/you/Aura/src/AST/Stmt.h#L462) `MethodDecl.implInterface` 从 `std::string` 扩展为复合结构：
      ```cpp
      std::string implInterface;                                    // 接口名（如 "Comparable"）
      std::vector<std::unique_ptr<TypeExpr>> implTypeArgs;          // 类型实参（如 <Point>），可空
      ```
    - Parser（parseMethodDecl）：`impl` 后解析接口名 + 可选 `<` 类型实参列表 `>`（复用 NamedType 的类型实参解析逻辑）
    - **clone() 同步复制 implTypeArgs**（与 implInterface 同改）
    - Sema：checkMethodBody 的接口一致性验证（DeclChecker.cpp:455-503）适配——用 `implTypeArgs` 对接口泛型参数做 substitute 后再比对方法签名；类型实参数与接口泛型参数数不一致报错
    - 非泛型接口（Stringer）沿用 implInterface 单名，implTypeArgs 为空
  - **接口泛型实例化**：`Comparable<Point>` 生成独立抽象基类（模板特化 `ComparablePoint` 或模板 `Comparable<Point>`），适配器继承对应实例
  ```aura
  interface Iterator<T> {
      next() -> Optional<T>
      collect() -> [T] { ... }   // 默认方法示范（返回基础类型，v1 允许）
  }
  ```
- **str() 衔接**：`str(obj)` 若实参静态类型实现 Stringer → CodeGen 生成 `aura_rt::string_of(StringerAdapter(obj))` 或直接 `obj->to_string()`（v1 用直接方法调用，绕过接口）

### 阶段 5b：Comparable 运算符重载（最后实现）

**语言级语义**（本阶段核心目标）：任何实现 Comparable 的 record 类型，Aura 源码中**可直接对其实例使用比较符号** `<` `<=` `>` `>=` `==` `!=`，编译器自动映射到接口方法，用户无需手动调用：

```aura
let p1: Point = { x = 1, y = 2 }
let p2: Point = { x = 3, y = 4 }
if p1 < p2 { io.println("p1 < p2") }        // → PointComparable(&p1).less(PointComparable(&p2))
if p1 == p2 { ... }                          // → PointComparable(&p1).equal(PointComparable(&p2))
io.println(str(p1 <= p2))                    // 表达式内同样生效
```

**符号 ↔ 函数一一映射**：`==→equal`、`!=→ne`、`<→less`、`>→greater`、`<=→le`、`>=→ge`；核心三路比较 `cmp`。每个符号方法有默认实现（基于 cmp），用户可 override 任意符号。

**推导规则**（Sema 判定，三种路径）：

| 用户实现了 | 自动获得 | 原理 |
|---|---|---|
| 仅 `cmp` | 全部 6 符号 | 默认实现推导（推荐） |
| `equal` + `less` | 全部 6 符号 | `ne=!equal`、`greater=less(b,a)`、`le=!greater`、`ge=!less` |
| 任意符号（如只 `less`） | 部分（`ge=!less`、`greater=less(b,a)`、`le=!greater`）；`equal/ne` 需另实现 | 符号间推导 |

推导依赖：`equal↔ne` 互补；`less↔greater` 反向（swap 参数）；`le=!greater`、`ge=!less`。有序比较（< <= > >=）⇐ 至少 less/greater/cmp 之一；相等（== !=）⇐ 至少 equal/ne/cmp 之一。

**运算符分发**（CodeGen）：`a < b`（a、b 为实现 Comparable 的 record）→ `PointComparable(&a).less(PointComparable(&b))`（接口虚调用，尊重 override；两适配器复用阶段 3 机制）。**T 一致性由 Sema 保证**（`Comparable<T>` 泛型实参匹配，`Point < Rect` 在类型检查即报错，无 downcast UB）。`==` 特例：record 实现 Comparable → 走接口；未实现 → 维持现状（string→string_eq、基础类型内置、record 无 == 语义）。

**错误处理**：
- 用 `<` 但类型未实现 Comparable → "type 'X' does not implement Comparable, cannot use operator '<'"
- 有序比较缺 less/greater/cmp → "missing '<' implementation: implement cmp() or less() for ordered comparison"
- 混合类型比较（a < b 的 T 不一致）→ 类型检查报错（泛型实参不匹配）

**v1 边界**：内置类型（int/float/string）自动实现 Comparable 不纳入本 issue（后续）；比较只读不改 GC 语义。

**Comparable 适配器**（泛型化后，无 downcast）：`PointComparable : Comparable<Point>` 的 `cmp(Point* other)` 直接转发 `obj->cmp(other)`——比较方法签名天然是具体类型，GcRootHandle 模式 B 的 get() 读取自身 obj 即可。

## 五、影响分析

| 组件 | 影响 | 兼容性 |
|---|---|---|
| runtime（gc.h/handles.h） | GcRootHandle 三模式统一（模式 A 现语义零变化） | ⚠️ 影响面集中在根类；迁移点 8 处 |
| Sema（isAssignable） | 具体类型→接口放宽 | ⚠️ 非破坏性放宽；typeMethods_ 只读新增 |
| Sema（方法调用） | 不变（仍放行） | — |
| CodeGen（genInterfaceDecl） | 抽象基类保留 + 默认方法支持 | 向后兼容（无体仍纯虚） |
| CodeGen（调用点包装） | IfaceFunc 闭包路径保留；新增适配器路径 | 向后兼容 |
| Parser/AST | 接口泛型 + 方法体 | 向后兼容 |
| 闭包→单方法接口（现有功能） | 不变 | — |

## 六、边界条件处理策略

| 边界条件 | 现状 | 计划处理 | 测试策略 |
|---|---|---|---|
| 适配器函数体内 GC compact | 悬垂 | GcRootHandle 模式 B（值持有根，§3.2） | GC 压力测试：welcome 内 io.println 触发 GC |
| 接口参数传协程函数 | 无此路径 | Sema 报错（co_await 临时对象生命周期） | 负向用例 |
| record 缺接口方法 | 无 | Sema 报缺失方法名 | 负向用例 |
| 同方法名多接口 | 无 | 各适配器独立转发 | 双接口用例 |
| Comparable\<T\> 泛型参数 | 无 | cmp 参数即具体类型（T 实参），无 downcast | 用例 |
| 接口作字段/Array 元素/返回值 | 未验证 | v1 Sema 报错（Bug 7 检查点：① record 字段类型解析处 ② List/Array 元素类型解析处 ③ 函数/方法返回类型解析处，报 "interface type cannot be used as ..."）| 负向用例 |
| 闭包→多方法接口 | return false | 保持拒绝 | 负向用例 |
| 默认方法返回接口类型 | 无 | v1 报错（裁剪） | 负向用例 |
| 泛型接口 Iterator\<T\> | 无 | 接口泛型参数 + 适配器模板化 | 泛型用例 |
| 多线程传接口参数 | 无 | 模式 B 线程局部根，同线程使用 | sync 用例 |

## 七、测试计划

- **阶段 1+2**：`welcome(u: User)` 编译通过；缺方法报错
- **阶段 3**：`g.greet()` 运行时正确；**GC 用例**（循环中 io.println 触发 GC 后仍正确）；多接口
- **阶段 4**：默认方法继承；override；默认方法返回基础类型
- **阶段 5**：`Iterator<int>` 用户实现 next() + collect() 默认方法；`str(user)` 走 Stringer
- **阶段 5b**：Point 实现 cmp（`impl Comparable<Point>`）→ 全部六符号直接比较（`p1 < p2`、`p1 == p2`、`p1 <= p2` 等）；override 某符号生效；只实现 equal+less 也能用全部；缺实现报错；`Point < Rect` 类型不匹配报错
- **回归**：example/used 全量 + 现有闭包→接口用例
- 测试环境：`compile.cmd` + test.exe（非 ASAN）；GC 压力用例用 ASAN 辅助验证（CLANG64 手动编译）

## 八、实施步骤（有序，每步独立可验证）

1. **runtime 根引用统一**：GcRootHandle 扩展模式 A/B/C（gc.h + handles.h）→ 迁移 string.cpp 7 处 GcGlobalRoot + ExprGen.cpp:1224 闭包捕获 → 删除 GcGlobalRoot/GcSharedRoot → 编译 runtime + 回归（intern/闭包捕获/栈变量）
2. **Sema 阶段 1**：`typeMethods_` 映射（第 1 遍末尾 `buildTypeMethods` 统一构建）→ 编译 aurac
3. **Sema 阶段 2**：isAssignable 结构匹配 + 缺失方法报错 → test.aura 正向/负向
4. **CodeGen 阶段 3a**：组合收集 + `genIfaceAdapter` + 调用点双源包装 → 编译生成验证
5. **CodeGen 阶段 3b**：GC 压力测试（welcome 内 GC）→ ASAN 验证
6. **阶段 4**：默认方法四层改造 → 默认方法用例
7. **阶段 5**：接口泛型 + interfaces.aurai + str() 衔接 → 内置接口用例
8. **全量回归** + READMEs/07-methods-interfaces.md §7.3 重写
9. 移除 TODO §二 该 issue 条目（测试通过后）

**回滚策略**：每阶段独立 commit；runtime 根统一改动独立可回滚（保留 GcGlobalRoot/GcSharedRoot 兼容层至迁移完成）；Sema/CodeGen 改动集中在接口分支与接口参数路径，不影响非接口代码。

## 九、风险与缓解

| 风险 | 缓解 |
|---|---|
| 根引用统一影响面（构造重载歧义/拷贝语义分叉） | 模式 A 现语义零变化 + 迁移点仅 8 处；统一先行完成并通过全量回归，再启用模式 B |
| 适配器根注册/注销开销（每调用点） | 传参场景稀疏；复用 thread-local 无锁链表；后续可优化为"适配器作整体根" |
| 适配器组合收集（第一遍扫描）漏收集 | 收集覆盖函数参数 + 调用点两处；漏则编译报"undefined type"暴露 |
| 默认方法体生成复用 genBlock 的上下文（self/throws） | 独立 genDefaultMethod 封装，显式传接口上下文 |
| 内置接口签名冻结问题 | 实现前先冻结签名（issue §3.6）；Comparable 用接口类型 |
| 协程接口参数限制过严 | v1 报错提示清晰；后续用"协程帧注册"（registerStackRoots）解禁 |
