# change.md — Interface 改造 + 根引用统一 + 内置接口（实现代码）

> 来源 plan：`plan/interface_rework_plan.md`（已审查）
> 分 7 个变更块：C0 根引用统一 → C1 typeMethods_ 映射 → C2 isAssignable 结构匹配 → C3 适配器 CodeGen → C4 接口默认方法 → C5 内置接口+泛型接口 → C5b Comparable 运算符重载
> 实施顺序：C0 → C1 → C2 → C3 → C4 → C5 → C5b（每步可独立编译验证）

---

## C0. runtime 根引用统一（GcRootHandle 三模式）

### C0.1 修改 `runtime/gc/gc.h`

**新增枚举**（`GcRootHandleBase` 之前）：

```cpp
// 根持有模式：引用外部变量（栈变量）/ 值持有（适配器）/ 值持有+全局（闭包捕获）
enum class GcRootMode : uint8_t { Ref, ValueThreadLocal, ValueGlobal };
// 值持有模式的根作用域（编译期静态决定，非运行时判断）
enum class GcRootScope { ThreadLocal, Global };
```

**修改 GcRootHandle 类定义**（替换 L73-100）：

```cpp
template <typename T>
class GcRootHandle : public GcRootHandleBase {
public:
    // 模式 A：引用外部变量（线程局部）← 现有 CodeGen 栈变量，语义零变化
    GcRootHandle(T& ref);
    // 模式 B/C：值持有。scope 必须显式（无默认值），避免与 T& 重载歧义
    //   ThreadLocal ← 接口适配器；Global ← 闭包捕获/全局缓存
    GcRootHandle(T val, GcRootScope scope);
    ~GcRootHandle();

    // 拷贝：按 other.mode_ 分支（Ref→引用同一变量；Value→深拷贝值+独立注册）
    GcRootHandle(const GcRootHandle& other);
    GcRootHandle& operator=(const GcRootHandle&) = delete;

    // 更新被包装的引用目标（仅 Ref 模式）
    void rebind(T& ref) {
        ptr_ = &ref;
        ptr_ref_ = reinterpret_cast<GcObject**>(ptr_);
    }

    T& operator*()  const { return *ptr_; }
    T* operator->() const { return ptr_; }
    T& get()              { return *ptr_; }  // 非 const：返回引用，可作赋值左侧
    T  get()        const { return *ptr_; }  // const：返回值

    void set(T v);                            // Ref 写外部变量；Value 写内部 val_

private:
    union { T* ptr_; T val_; };   // Ref 用 ptr_（&外部变量）；Value 用 val_（内部持值）——共享 8B 槽
    GcRootMode mode_;             // 1B：拷贝构造与析构据此分支（Ref/ValueTL/ValueGlobal）
    friend class GcHeap;
};
```

**删除** L137-150 `GcGlobalRoot<T>` 类定义。

### C0.2 修改 `runtime/gc/handles.h`

**替换 GcRootHandle 模板实现**（L20-40）：

```cpp
template <typename T>
GcRootHandle<T>::GcRootHandle(T& ref)
    : GcRootHandleBase(), ptr_(&ref), mode_(GcRootMode::Ref) {
    ptr_ref_ = reinterpret_cast<GcObject**>(ptr_);
    GcHeap::instance().registerRootThreadLocal(this);
}

template <typename T>
GcRootHandle<T>::GcRootHandle(T val, GcRootScope scope)
    : GcRootHandleBase(), val_(val), ptr_(&val_), mode_(scope == GcRootScope::Global
        ? GcRootMode::ValueGlobal : GcRootMode::ValueThreadLocal) {
    ptr_ref_ = reinterpret_cast<GcObject**>(ptr_);
    if (mode_ == GcRootMode::ValueGlobal)
        GcHeap::instance().registerGlobalRoot(ptr_ref_);   // 全局根容器
    else
        GcHeap::instance().registerRootThreadLocal(this);  // 线程局部链表
}

template <typename T>
GcRootHandle<T>::~GcRootHandle() {
    if (!ptr_) return;
    if (mode_ == GcRootMode::ValueGlobal)
        GcHeap::instance().unregisterGlobalRoot(ptr_ref_);
    else
        GcHeap::instance().unregisterRootThreadLocal(this);
}

// 拷贝：Ref → 引用同一外部变量（注册独立线程局部根，指针相同，现语义）；
//       Value → 深拷贝值 + 独立注册（闭包捕获语义，各副本独立持有根）
template <typename T>
GcRootHandle<T>::GcRootHandle(const GcRootHandle& other)
    : GcRootHandleBase(), mode_(other.mode_) {
    if (other.mode_ == GcRootMode::Ref) {
        ptr_ = other.ptr_;                       // 引用同一外部变量
        ptr_ref_ = reinterpret_cast<GcObject**>(ptr_);
        GcHeap::instance().registerRootThreadLocal(this);
    } else {
        val_ = other.get();                      // 深拷贝值
        ptr_ = &val_;
        ptr_ref_ = reinterpret_cast<GcObject**>(ptr_);
        if (other.mode_ == GcRootMode::ValueGlobal)
            GcHeap::instance().registerGlobalRoot(ptr_ref_);
        else
            GcHeap::instance().registerRootThreadLocal(this);
    }
}

template <typename T>
void GcRootHandle<T>::set(T v) {
    if (mode_ == GcRootMode::Ref) {
        *ptr_ = v;                               // 写外部变量
        GcHeap::instance().writeBarrier(nullptr, ptr_, static_cast<GcObject*>(v));
    } else {
        val_ = v;                                // 写内部 val_
    }
}
```

**删除 GcGlobalRoot 模板实现**（L50-61）。

**删除 GcSharedRoot<T> 类**（L63-131）——闭包捕获迁移到 `GcRootHandle<T>(val, GcRootScope::Global)`。

### C0.3 迁移调用点

**`runtime/builtin/string.cpp`**（7 处 `GcGlobalRoot<GcString>`）：逐处替换为值持有模式，如：

```cpp
// 修改前（示例 1 处）
static GcGlobalRoot<GcString> emptyRoot(GcString::from(""));
// 修改后
static GcRootHandle<GcString*> emptyRoot(nullptr, aura_rt::GcRootScope::Global);
// 使用处 emptyRoot.get() 接口不变
```

（其余 6 处同式替换；`GcString::empty()/from(bool)/from(int)` 等全局单例缓存。）

**`src/CodeGen/ExprGen.cpp`** 闭包捕获（约 L1224，生成 `[name = aura_rt::GcSharedRoot<T>(name.get())]`）：

```cpp
// 修改前
h << "[name = aura_rt::GcSharedRoot<T>(" << name << ".get())]";
// 修改后
h << "[name = aura_rt::GcRootHandle<T>(" << name << ".get(), "
  << "aura_rt::GcRootScope::Global)]";
```

### C0.4 验证

- 编译 runtime + aurac；回归 intern 字符串、闭包捕获、栈变量包装（example/used + test.aura 全量）
- `GcRootHandle` 内存 40B（基类 24 + union 8 + mode_ 1 + padding），assert 或注释核对

---

## C1. Sema "类型→方法"映射（typeMethods_）

### C1.1 修改 `src/Sema/SemAnalyzer.h`

private 区新增（`symtab_` 附近）：

```cpp
// ============ 接口结构匹配（Interface 改造）============
// receiverType 规范名 → 该 record 类型拥有的方法签名（buildTypeMethods 构建）
std::map<std::string, std::vector<InterfaceSemType::MethodSig>> typeMethods_;
// 第 1 遍末尾统一构建（resolveType 安全时刻）
void buildTypeMethods(const Program& program);
// receiverType 规范名（查符号表 RecordSemType.canonicalName）
[[nodiscard]] std::string recordTypeKey(const std::string& receiverType) const;
```

### C1.2 实现（`src/Sema/Checker/DeclChecker.cpp` 或 SemAnalyzer.cpp）

**注意：`MethodDecl.receiverType` 是 `std::string`（非 TypeExpr）**，recordTypeKey 直接查符号表：

```cpp
std::string SemAnalyzer::recordTypeKey(const std::string& receiverType) const {
    auto* sym = symtab_.lookup(receiverType);
    if (sym && sym->kind == SymKind::TypeAlias && sym->type) {
        if (auto* rec = dynamic_cast<const RecordSemType*>(sym->type.get()))
            return rec->canonicalName.empty() ? rec->name : rec->canonicalName;
    }
    return "";   // 非 record 接收者（v1 接口仅支持 record 实现）
}

void SemAnalyzer::buildTypeMethods(const Program& program) {
    typeMethods_.clear();
    for (auto& d : program.decls) {
        if (auto* m = dynamic_cast<const MethodDecl*>(d.get())) {
            if (m->isConstructor || m->receiverType.empty()) continue;
            std::string key = recordTypeKey(m->receiverType);
            if (key.empty()) continue;
            InterfaceSemType::MethodSig sig;
            sig.name = m->name;
            // 签名直接 resolveType 解析（第 1 遍末尾所有类型已声明，resolveType 安全）
            // 注：不从符号表 lookup(m->name) 取——不同 record 的同名方法会取错符号
            for (auto& p : m->params) {
                if (p.type) sig.paramTypes.push_back(resolveType(*p.type));
                else        sig.paramTypes.push_back(ErrorSemType::make());
            }
            if (m->returnType) sig.returnType = resolveType(*m->returnType);
            sig.throws = m->throws;
            typeMethods_[key].push_back(std::move(sig));
        }
    }
}
```

### C1.3 调用时机（`src/Sema/Checker/DeclChecker.cpp` declareTopLevel 末尾）

```cpp
void SemAnalyzer::declareTopLevel(const Program& program) {
    for (auto& d : program.decls) {
        if (d) declareDecl(*d);
    }
    // 第 1 遍末尾统一构建：此时所有 record/interface/方法符号已注册，resolveType 安全
    buildTypeMethods(program);
}
```

> 说明：`implInterface` 一致性验证仍走第 2 遍 checkMethodBody（现有 L455-503），结构匹配不依赖 impl 声明。

---

## C2. Sema isAssignable 结构匹配（record → 接口）

修改 `src/Sema/SemAnalyzer.cpp` L366-377 接口分支：

```cpp
    // 接口类型：单方法接口可由闭包满足；具体 record 结构匹配（方法名+签名全满足）
    if (auto* iface = dynamic_cast<const InterfaceSemType*>(&target)) {
        // 1. 闭包 → 单方法接口（现有路径保留）
        if (auto* func = dynamic_cast<const FuncSemType*>(&source)) {
            if (iface->methods.size() == 1) {
                auto& m = iface->methods[0];
                return matchFuncSig(m.paramTypes, m.returnType.get(), m.throws,
                                   func->paramTypes, func->returnType.get(), func->throws);
            }
            return false;
        }
        // 2. 具体 record → 结构匹配（README"结构类型自动实现"）
        if (auto* rec = dynamic_cast<const RecordSemType*>(&source)) {
            std::string key = rec->canonicalName.empty() ? rec->name : rec->canonicalName;
            auto it = typeMethods_.find(key);
            if (it == typeMethods_.end()) return false;   // 无方法 → 不满足
            for (auto& im : iface->methods) {
                // 接口每个方法必须在 record 方法集中存在同名同签名
                bool found = false;
                for (auto& rm : it->second) {
                    if (rm.name != im.name) continue;
                    found = matchFuncSig(im.paramTypes, im.returnType.get(), im.throws,
                                         rm.paramTypes, rm.returnType.get(), rm.throws);
                    break;
                }
                if (!found) return false;
            }
            return true;
        }
        return false; // 其他类型不能满足接口
    }
```

**缺方法报错**（调用点可给出更友好提示）：在 checkCallArgs / inferCall 中，实参类型是 RecordSemType 而形参是 InterfaceSemType 且不满足时，现有报错已是"expected 'Iface', got 'Record'"。本阶段补充：当 record 差一个方法时提示方法名（可选增强，v1 用现有通用报错）。

验证：`welcome(u: User)` 编译通过；缺 `greet` 方法时编译失败。

---

## C3. CodeGen 接口适配器（方案 B）

### C3.1 修改 `src/CodeGen/CodeGen.h`

public 区新增（`genInterfaceDecl` 附近）：

```cpp
    // 生成"类型 × 接口"适配器（方案 B：record 保持不动，适配器持值持有根）
    void genIfaceAdapter(std::ostream& h, const std::string& recordName,
                         const InterfaceDecl& iface);
    // 已生成适配器组合名缓存（record 名 + 接口名）
    std::set<std::string> ifaceAdapterCache_;
```

### C3.2 组合收集 + 生成入口（`src/CodeGen/CodeGen.cpp`）

在 generate 的第一遍扫描（interfaceNames_ 收集处 L90-91 附近）同步收集组合：

```cpp
        if (auto* i = dynamic_cast<const InterfaceDecl*>(d.get()))
            interfaceNames_.insert(i->name);
        // 接口适配器组合收集：record 方法声明 → (receiverType, 该接口名)
        // ⚠️ Bug 1 修复：只收集非泛型 record（receiverTypeArgs 为空）——
        //   泛型 record 的 C++ 类型名是 "Stack<T>"（DeclGen.cpp:541-549 拼接模板参数），
        //   receiverType 仅 "Stack"，适配器类型名无法对应。v1 泛型 record 接接口 → Sema 报错。
        if (auto* m = dynamic_cast<const MethodDecl*>(d.get())) {
            if (!m->implInterface.empty() && m->receiverTypeArgs.empty())
                interfaceImplementations_[m->receiverType].insert(m->implInterface);
        }
```

CodeGen.h 增加成员：

```cpp
    // receiverType → 其 impl 的接口名集合（组合收集；键 = AST 接收者名，非泛型下与 C++ 类型名一致）
    std::map<std::string, std::set<std::string>> interfaceImplementations_;
```

> **泛型 record 实现接口 → Sema 报错**（checkMethodBody 的 impl 验证处追加）：
> ```cpp
> if (!decl.receiverTypeArgs.empty()) {
>     error(decl, "generic type '" + decl.receiverType
>           + "' cannot implement interface in v1 (adapter generation unsupported)");
> }
> ```

在 `genInterfaceDecl` 生成入口（CodeGen.cpp L185-189，declarationsOnly 分支）之后，为每个实现组合生成适配器：

```cpp
    if (auto* i = dynamic_cast<const InterfaceDecl*>(&decl)) {
        if (declarationsOnly) {
            genInterfaceDecl(h, *i);
            // 为所有实现该接口的 record 生成适配器（在接口基类之后、record 之前）
            for (auto& [rec, ifaces] : interfaceImplementations_) {
                if (ifaces.count(i->name) == 0) continue;
                std::string key = rec + i->name;
                if (ifaceAdapterCache_.count(key)) continue;
                ifaceAdapterCache_.insert(key);
                genIfaceAdapter(h, rec, *i);
            }
        }
        return;
    }
```

### C3.3 适配器生成（`src/CodeGen/DeclGen.cpp` 新增）

```cpp
// record × 接口适配器：持值持有根，转发到 record 具体方法
void CodeGenerator::genIfaceAdapter(std::ostream& h,
                                    const std::string& recordName,
                                    const InterfaceDecl& iface) {
    std::string adapterName = safeName(recordName) + iface.name;
    h << "struct " << adapterName << " final : " << iface.name << " {\n";
    h << "  aura_rt::GcRootHandle<" << recordName << "*> obj;\n";   // 模式 B：值持有+线程局部根
    h << "  explicit " << adapterName << "(" << recordName << "* o)\n";
    h << "      : obj(o, aura_rt::GcRootScope::ThreadLocal) {}\n";  // 显式 scope（避免匹配模式 A）
    for (auto& m : iface.methods) {
        std::string retType = m.returnType ? mapType(*m.returnType) : "void";
        h << "  " << retType << " " << m.name << "(";
        for (size_t i = 0; i < m.params.size(); ++i) {
            if (i > 0) h << ", ";
            h << (m.params[i].type ? mapType(*m.params[i].type) : "auto")
              << " " << safeName(m.params[i].name);
        }
        h << ") const override {\n";
        h << "    return obj.get()->" << m.name << "(";
        for (size_t i = 0; i < m.params.size(); ++i) {
            if (i > 0) h << ", ";
            h << safeName(m.params[i].name);
        }
        h << ");\n";
        h << "  }\n";
    }
    h << "};\n\n";
}
```

> 注意：`obj.get()->greet()` 要求 record 的具体方法为 C++ 成员函数（PendingMethod 机制已嵌入 struct），直接虚调用。

### C3.4 调用点双源包装（`src/CodeGen/ExprGen.cpp` L621-648）

```cpp
    // 接口参数自动包装（双源：闭包 → IfaceFunc；具体 record → 适配器；接口变量 → 透传）
    // ⚠️ Bug 4 修复：透传判定完全基于 inferredType（InterfaceSemType），
    //    删除现有 arg.find(ifaceName) 字符串判断——record 名含接口名子串（如 GreetableUser）
    //    或 inferredType 缺失时都会误判
    auto ipIt = fnInterfaceParams_.find(calleeName);
    ...
    for (size_t i = 0; i < e.args.size(); ++i) {
        std::string arg = genExpr(*e.args[i], isCoroutine);
        if (ipIt != fnInterfaceParams_.end()) {
            for (auto& [idx, ifaceName] : ipIt->second) {
                if (idx == i) {
                    const SemType* argTy = e.args[i]->inferredType;
                    if (argTy && dynamic_cast<const InterfaceSemType*>(argTy)) {
                        // 接口参数透传（已在传参处构造适配器）：不包装
                    } else if (auto* rt = dynamic_cast<const RecordSemType*>(argTy)) {
                        // 具体 record → 适配器构造（v1 仅非泛型 record，见 C3.2）
                        std::string recName = rt->canonicalName.empty() ? rt->name : rt->canonicalName;
                        arg = safeName(recName) + ifaceName + "(" + arg + ")";
                    } else {
                        // 闭包/其他路径：直接包装为 IfaceFunc（不再用 find 判断）
                        arg = ifaceName + "Func(" + arg + ")";
                    }
                    break;
                }
            }
        }
        ...
    }
```

验证：`g.greet()` 运行时正确；welcome 内 io.println 触发 GC 后仍正确（ASAN 辅助）；多接口各适配器独立。

---

## C4. 接口默认方法（模式 1）

### C4.1 修改 `src/AST/Stmt.h` InterfaceMethodSig（L397-402）

```cpp
struct InterfaceMethodSig {
    std::string name;
    std::vector<Param> params;
    bool throws = false;
    std::unique_ptr<TypeExpr> returnType;
    std::unique_ptr<BlockStmt> defaultBody;   // 默认实现（nullptr = 必须实现）
    // clone() 同步复制 defaultBody（与 InterfaceDecl::clone 同改）
};
```

InterfaceDecl::clone（L408-422）中同步：

```cpp
            sig.throws = m.throws;
            if (m.returnType) sig.returnType.reset(...);
            if (m.defaultBody) sig.defaultBody.reset(
                static_cast<BlockStmt*>(m.defaultBody->clone().release()));
```

### C4.2 修改 `src/Parser/TypeParser.cpp` parseInterfaceMethodSig（L231-249）

```cpp
    // 接口方法签名后可选函数体（默认实现）：签名后直接 `{`
    if (check(TokType::LBrace)) {
        sig->defaultBody = parseBlock();
    }
```

（与闭包语法区分：接口方法签名后直接 `{` 即默认实现体。）

### C4.3 修改 `src/CodeGen/DeclGen.cpp` genInterfaceDecl（L131-141）

```cpp
    for (auto& m : decl.methods) {
        std::string retType = m.returnType ? mapType(*m.returnType) : "void";
        if (m.defaultBody) {
            // 有默认实现 → 非纯虚；体内 self 映射 this
            h << "  virtual " << retType << " " << m.name << "(";
            ... 参数列表同现有 ...
            h << ") const {\n";
            // 生成默认方法体（复用 genBlock，StmtGen.cpp:11 签名 genBlock(ostream&, const BlockStmt&, bool)）
            currentReceiverName_ = "self";
            genBlock(h, *m.defaultBody, /*isCoroutine=*/false);
            currentReceiverName_.clear();
            h << "  }\n";
        } else {
            h << "  virtual " << retType << " " << m.name << "(" ... << ") const = 0;\n";
        }
    }
```

> 实现细节：genInterfaceDecl 内直接内联生成体，或抽取 `genInterfaceDefaultBody`；self 复用 `currentReceiverName_` 映射 this 机制。v1 限制：默认方法返回接口类型 → Sema 报错。

### C4.4 Sema 校验（checkMethodBody 附近）

接口声明后校验默认方法体（inferExpr 检查返回类型与声明一致）；"必须实现集合" = 无 defaultBody 的方法。

验证：默认方法继承、override、返回基础类型。

---

## C5. 内置接口 + 泛型接口支持

### C5.1 `src/AST/Stmt.h` InterfaceDecl 加 typeParams + clone 同步（Bug 6）

```cpp
struct InterfaceDecl : Decl {
    std::string name;
    std::vector<std::string> typeParams;          // 泛型参数（如 Iterator<T> 的 T）
    std::vector<InterfaceMethodSig> methods;
    // clone() 中：n->typeParams = typeParams;
};
```

### C5.2 `src/Parser/DeclParser.cpp` parseInterfaceDecl 解析泛型参数（L108-124）

```cpp
    decl->name = nameTok.lexeme;
    // 泛型参数：interface Iterator<T>
    if (match(TokType::Less)) {
        do {
            auto& tp = consume(TokType::Identifier, "expected type parameter in interface");
            decl->typeParams.push_back(tp.lexeme);
        } while (match(TokType::Comma));
        consume(TokType::Greater, "expected '>' after interface type parameters");
    }
    consume(TokType::LBrace, "expected '{' after interface name");
```

### C5.3 MethodDecl impl 复合结构（Bug 8）

**`src/AST/Stmt.h`** L461：

```cpp
    std::string implInterface;                              // 接口名（如 "Comparable"）
    std::vector<std::unique_ptr<TypeExpr>> implTypeArgs;    // 类型实参（如 <Point>），可空
    // clone() 同步复制 implTypeArgs
```

**`src/Parser/DeclParser.cpp`** L146-149：

```cpp
    if (match(TokType::Impl)) {
        auto& implTok = consume(TokType::Identifier, "expected interface name after 'impl'");
        decl->implInterface = implTok.lexeme;
        // 泛型接口实参：impl Comparable<Point>
        if (match(TokType::Less)) {
            do { decl->implTypeArgs.push_back(parseType()); } while (match(TokType::Comma));
            consume(TokType::Greater, "expected '>' after interface type arguments");
        }
    }
```

**`src/Sema/Checker/DeclChecker.cpp`** checkMethodBody 的接口验证（L456-503）适配：

```cpp
        if (!ifaceSym || ifaceSym->kind != SymKind::Interface) { ...现有报错... }
        else {
            // 泛型接口：类型实参 substitute（implTypeArgs ↔ typeParams 数量校验）
            if (!decl.implTypeArgs.empty() && ifaceSym->interfaceMethods.empty())
                ; // 无方法接口不参与签名验证
            // 比对时：若接口有泛型参数，将 m.paramTypes/returnType 中的 GenericSemType
            // 按 substitute 替换为 implTypeArgs 后，再与实现方法签名 isAssignable 比对
            for (auto& m : ifaceSym->interfaceMethods) {
                if (m.name == decl.name) { ...现有数量/参数/返回/throws 比对，签名用替换后类型... }
            }
        }
```

### C5.4 新建 `builtins/interfaces.aurai`

```aura
// 内置常见接口（builtin 模块自动加载）
interface Stringer {
    to_string() -> string
}

interface Comparable<T> {
    cmp(other: T) -> int                // 三路比较核心（<0/=0/>0）：实现它 → 全部符号自动
    equal(other: T) -> bool { return cmp(other) == 0 }   // ==
    ne(other: T) -> bool { return !equal(other) }        // !=
    less(other: T) -> bool { return cmp(other) < 0 }     // <
    greater(other: T) -> bool { return cmp(other) > 0 }  // >
    le(other: T) -> bool { return !greater(other) }      // <=
    ge(other: T) -> bool { return !less(other) }         // >=
}

interface Iterator<T> {
    next() -> Optional<T>
    collect() -> [T] { ... }   // 默认方法示范（返回基础类型，v1 允许）
}
```

### C5.5 `src/Module/ModuleManager.cpp` loadBuiltinAurai 追加（L226-230）

```cpp
void ModuleManager::loadBuiltinAurai() {
    loadAuraiFile("io.aurai");
    loadAuraiFile("builtin.aurai");      // 基础内置全局函数（int/float/str/gc_*）
    loadAuraiFile("interfaces.aurai");   // 内置接口（Stringer/Comparable/Iterator）
    // path.aurai 不在此加载——由 import path 时按需加载
}
```

### C5.6 str() 衔接

`str(obj)` 若实参静态类型实现 Stringer → 直接方法调用 `obj->to_string()`（v1 绕过接口）。ExprGen.cpp C5.2 全局映射分支中：当实参 inferredType 是 RecordSemType 且该 record 方法集含 to_string 时，将 `{0}` 替换为 `arg.to_string()`（生成 `.to_string()` 成员调用）。

验证：`Iterator<int>` 用户实现 next() + collect() 默认方法；`str(user)` 走 Stringer。

---

## C5b. Comparable 运算符重载（最后实现）

### C5b.1 Sema（`src/Sema/Checker/ExprInfer.cpp` inferBinaryExpr）

比较符号 `<` `<=` `>` `>=` `==` `!=` 处理：左右 inferredType 均为相同 RecordSemType 时，查该 record 是否实现 `Comparable<T>`（通过符号表查接口符号 + recordTypeKey + typeMethods_ 中同名方法）：

```cpp
// 伪码：inferBinaryExpr 的 op 是 Ls/Le/Gt/Ge/Eq/Ne 分支前
if (isComparisonOp(e.op)) {
    auto* lt = dynamic_cast<const RecordSemType*>(e.lhs->inferredType.get());
    auto* rt = dynamic_cast<const RecordSemType*>(e.rhs->inferredType.get());
    if (lt && rt && lt->canonicalName == rt->canonicalName) {
        std::string key = ...canonicalName...;
        // 校验实现了 Comparable<T>（cmp 或对应符号存在），否则报错：
        //   "type 'X' does not implement Comparable, cannot use operator '<'"
        // 缺符号推导源时：
        //   "missing '<' implementation: implement cmp() or less() for ordered comparison"
        return boolType();   // 比较结果恒 bool（inferredType 仅表达类型，不携带标记）
    }
    // 否则维持现有行为
}
```

### C5b.2 CodeGen（`src/CodeGen/ExprGen.cpp` genBinaryExpr）

比较符号分发：**判定"record 实现 Comparable"不依赖 inferredType 标记**（Bug 3 修复：inferredType 是 `const SemType*` 且比较符号推断为 `boolType()`，无法携带标记）——改为查询 C3.2 的组合收集集合 `interfaceImplementations_`（键 = record 名，含 "Comparable" 即视为实现）：

```cpp
// 判定：lhs 的静态类型是 RecordSemType，且其名在 interfaceImplementations_ 中关联 Comparable
auto* lt = dynamic_cast<const RecordSemType*>(e.lhs->inferredType);
if (lt) {
    std::string recName = lt->canonicalName.empty() ? lt->name : lt->canonicalName;
    auto it = interfaceImplementations_.find(recName);
    // 兜底：适配器名按 receiverType 生成（C3.3），此处用同一 recName 匹配
    if (it != interfaceImplementations_.end() && it->second.count("Comparable") > 0) {
        // 生成：<recName>Comparable(a.get()).less(<recName>Comparable(b.get()))
        // a、b 是 GcRootHandle 变量 → genExpr 生成 "a.get()"（已是 User*），直接传适配器
        std::string lhs = genExpr(*e.lhs, isCoroutine);   // 如 "p1.get()"
        std::string rhs = genExpr(*e.rhs, isCoroutine);
        // op → 接口方法名映射：< → less, <= → le, > → greater, >= → ge, == → equal, != → ne
        return adapterName + "(" + lhs + ")." + methodName
             + "(" + adapterName + "(" + rhs + "))";
    }
}
// 否则维持现有比较代码生成（string→string_eq、基础类型内置等）
```

> 注意：C5b.1 Sema 校验与 C5b.2 CodeGen 判定共用同一数据源（Sema 用 typeMethods_、CodeGen 用 interfaceImplementations_，两者同源于 receiverType 名，非泛型下一致），避免两套逻辑漂移。

验证：Point 实现 `impl Comparable<Point>` 的 cmp → `p1 < p2`、`p1 == p2`、`p1 <= p2` 直接可用；override 某符号生效；只实现 equal+less 也能用全部；缺实现报错；`Point < Rect` 类型不匹配报错。

---

## 删除项汇总

| 文件 | 删除内容 |
|---|---|
| runtime/gc/gc.h | `GcGlobalRoot<T>` 类定义 |
| runtime/gc/handles.h | `GcGlobalRoot` 实现、`GcSharedRoot<T>` 整个类 |
| runtime/builtin/string.cpp | 7 处 `GcGlobalRoot` 使用（改为 GcRootHandle 值持有+Global） |

## 测试计划（写入 example/test.aura 分阶段验证）

1. C0：回归（intern/闭包捕获/栈变量）
2. C1+C2：`welcome(u: User)` 编译通过；缺方法报错
3. C3：`g.greet()` 正确；GC 压力（welcome 内 io.println 循环触发 GC）；多接口
4. C4：默认方法继承；override；默认方法返回基础类型
5. C5：`Iterator<int>` next+collect；`str(user)` 走 Stringer
6. C5b：六符号直接比较；override；equal+less 组合；缺实现/混类型报错
7. 回归：example/used 全量
