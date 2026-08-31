#include "Sema/SemAnalyzer.h"

namespace Aura {

// ============================================================
// 函数体/方法体检查入口
// ============================================================

// 默认参数声明规则：尾部连续、类型可赋值、泛型参数拒绝（C3.1）
void SemAnalyzer::checkDefaultArgRules(const ASTNode& declNode,
                                       const std::vector<Param>& params) {
    bool seenDefault = false;
    for (auto& p : params) {
        if (!p.defaultExpr) {
            if (seenDefault)
                error(declNode, "parameter '" + p.name
                      + "': default argument must be trailing");
            continue;
        }
        seenDefault = true;
        // v1 限制：泛型参数不支持默认值（isAssignable 对未绑定 T 语义未定义）
        if (p.type && dynamic_cast<const GenericTypeRef*>(p.type.get())) {
            error(declNode, "parameter '" + p.name
                  + "': default argument not supported on generic parameter");
        }
        // 默认值表达式声明处求值检查（inferExpr 写入 defaultExpr->inferredType，C5 复用）。
        // 带参数类型作为期望（P1-1）：`x: Optional<float> = none()` 的默认 none() 需形参
        // Optional<float> 反推元素，否则推断为 Optional<error> → CodeGen none() 分支报错
        std::unique_ptr<SemType> pt;
        if (p.type) pt = resolveType(*p.type);
        auto dt = inferExpr(*p.defaultExpr, pt ? pt.get() : nullptr);
        if (dynamic_cast<const ErrorSemType*>(dt.get())) {
            error(*p.defaultExpr, "invalid default argument for parameter '" + p.name + "'");
            continue;
        }
        if (p.type) {
            if (!isAssignable(*pt, *dt))
                error(*p.defaultExpr, "default argument type mismatch for parameter '"
                      + p.name + "': expected '" + pt->toString() + "', got '"
                      + dt->toString() + "'");
        }
    }
}

// ============================================================
// 辅助：漏 return 检查（非 None 返回类型函数必须所有路径显式 return）
// 递归判断语句是否在所有路径上以 return/throw 终结（终结后语句不可达）
// ============================================================
bool SemAnalyzer::blockAllPathsReturn(const BlockStmt& block) {
    for (auto& s : block.stmts) {
        if (!s) continue;
        // 一旦遇到终结语句（return/throw），其后的语句不可达
        if (stmtAllPathsReturn(*s)) return true;
    }
    return false;
}

bool SemAnalyzer::stmtAllPathsReturn(const Stmt& stmt) {
    // return / throw 均为终结语句
    if (dynamic_cast<const ReturnStmt*>(&stmt)) return true;
    if (dynamic_cast<const ThrowStmt*>(&stmt)) return true;
    if (auto* blk = dynamic_cast<const BlockStmt*>(&stmt))
        return blockAllPathsReturn(*blk);
    if (auto* iff = dynamic_cast<const IfStmt*>(&stmt)) {
        if (!iff->elseBranch) return false;   // 无 else：条件为假时落入函数尾
        if (!blockAllPathsReturn(*iff->thenBranch)) return false;
        for (auto& ei : iff->elseIfs) {
            if (!blockAllPathsReturn(*ei.body)) return false;
        }
        return blockAllPathsReturn(*iff->elseBranch);
    }
    if (auto* tc = dynamic_cast<const TryCatchStmt*>(&stmt)) {
        // try 全路径 return → 安全；否则 try 落入函数尾的路径必须被 catch 兜住
        return tc->tryBody && tc->catchBody
            && blockAllPathsReturn(*tc->tryBody)
            && blockAllPathsReturn(*tc->catchBody);
    }
    if (auto* m = dynamic_cast<const MatchStmt*>(&stmt)) {
        if (m->cases.empty()) return false;
        for (auto& c : m->cases) {
            if (!c.body) return false;
            // case body 可为 BlockStmt 或表达式
            if (auto* blk = dynamic_cast<const BlockStmt*>(c.body.get())) {
                if (!blockAllPathsReturn(*blk)) return false;
            } else {
                return false;  // 表达式体不可能含 return
            }
        }
        return true;
    }
    // 循环（while/loop/for/sync for）可能执行 0 次 → 视为可落入函数尾
    // lock/sync/spawn/表达式/声明等 → 保守不返回
    return false;
}

void SemAnalyzer::checkFunBody(const FunDecl& decl) {
    loopDepth_ = 0;
    syncBoundaryStack_.clear();

    symtab_.enterScope(ScopeKind::Function);

    // 1. 先注册泛型参数（后续类型解析需要能查到 T）
    for (auto& p : decl.params) {
        if (p.type) registerTypeGenerics(symtab_, *p.type);
        // bug-07：参数直接写泛型函数类型（`f: fun(U) -> U`）时 U 是裸 NamedType，
        // registerTypeGenerics 不收集；仿返回侧提前注册（仅顶层 FunctionType）
        if (p.type) registerFuncTypeGenerics(symtab_, *p.type);
    }
    if (decl.returnType) registerTypeGenerics(symtab_, *decl.returnType);
    // M5：返回类型直接写泛型函数类型（`-> fun(U) -> U`）时，U 是裸 NamedType，
    // registerTypeGenerics 不收集；此处仿其提前注册，使返回类型解析不报 undefined
    if (decl.returnType) registerFuncTypeGenerics(symtab_, *decl.returnType);

    // 2. 注册参数（此时泛型已可解析）
    // G4：同时从解析后的参数类型收集"裸泛型变量"名（含类型别名实例化泄漏的 T，
    // 如 Transform<T>），用于构建当前函数的泛型参数集合
    std::vector<std::string> fnGenerics;
    for (auto& p : decl.params) {
        auto resolved = p.type ? resolveType(*p.type) : ErrorSemType::make();
        collectGenericNames(resolved.get(), fnGenerics);
        Symbol sym;
        sym.kind = SymKind::Parameter;
        sym.name = p.name;
        sym.type = std::move(resolved);
        symtab_.define(std::move(sym));
    }

    // 3. 解析返回类型（泛型已注册，T 可正确解析为 GenericSemType）
    //    用 FnCtxGuard 保存/恢复外层上下文（支持闭包体嵌套检查）
    auto retType = decl.returnType ? resolveType(*decl.returnType) : nullptr;
    // bug-28：main 入口禁止声明非 None 返回类型。genMainEntry（DeclFun.cpp）异步分支恒走
    // `auto t = ::aura_main(io); run_event_loop(t);`，而 run_event_loop 只接受 task<void>&
    // （task.h/event_loop.h/task.cpp 非模板）→ int/task<int> 均无法绑定 → g++ 坏 C++。
    // 无标注（retType=null）与 `-> None`（NoneSemType）放行；ErrorSemType 跳过防级联。
    if (decl.name == "main" && retType
        && !dynamic_cast<const NoneSemType*>(retType.get())
        && !dynamic_cast<const ErrorSemType*>(retType.get())) {
        error(decl, "entry function 'main' must not declare a return type (expected None)");
    }
    // P1-2：保存解析后的返回类型到 returnType->inferredType（typeStore_ 保活），
    // 供 CodeGen funSignature 读取（currentReturnVariantCppTypes_ / HasNoneVariant_ 装箱）
    if (retType && decl.returnType) {
        typeStore_.push_back(retType->clone());
        const_cast<TypeExpr*>(decl.returnType.get())->inferredType = typeStore_.back().get();
    }
    collectGenericNames(retType.get(), fnGenerics);
    // G4：压入泛型函数栈（函数体/闭包体检查期间 containsUnresolvedGeneric 据此
    // 判定签名引用的泛型参数可引用），退出函数体时恢复
    fnGenericStack_.push_back(std::move(fnGenerics));
    FnCtxGuard fc(*this, retType ? retType->clone() : nullptr, decl.throws);

    // 默认参数声明规则检查（尾部连续、类型可赋值、泛型参数拒绝）
    checkDefaultArgRules(decl, decl.params);

    if (decl.body) {
        checkBlock(*decl.body);
        // 漏 return 检查：非 None 返回类型必须所有路径显式 return
        // 避免 CodeGen 生成缺 return 的 C++ 函数导致 g++ 编译错误
        if (retType && !dynamic_cast<const NoneSemType*>(retType.get())
            && !dynamic_cast<const ErrorSemType*>(retType.get())
            && !blockAllPathsReturn(*decl.body)) {
            error(decl, "function '" + decl.name
                  + "' must return a value on all paths (missing explicit return)");
        }
    }
    fnGenericStack_.pop_back();
    symtab_.exitScope();
}

void SemAnalyzer::checkMethodBody(const MethodDecl& decl) {
    loopDepth_ = 0;
    syncBoundaryStack_.clear();

    symtab_.enterScope(ScopeKind::Function);

    // 1. 先注册接收者泛型参数
    for (auto& ta : decl.receiverTypeArgs) {
        Symbol tpSym;
        tpSym.kind = SymKind::GenericParam;
        tpSym.name = ta;
        symtab_.define(std::move(tpSym));
    }

    // 2. 注册参数泛型 + 返回类型泛型
    for (auto& p : decl.params) {
        if (p.type) registerTypeGenerics(symtab_, *p.type);
        // bug-07：方法参数直接写泛型函数类型（`f: fun(U) -> U`）时 U 是裸 NamedType，
        // registerTypeGenerics 不收集；仿返回侧提前注册（仅顶层 FunctionType）
        if (p.type) registerFuncTypeGenerics(symtab_, *p.type);
    }
    if (decl.returnType) registerTypeGenerics(symtab_, *decl.returnType);
    // M5：返回类型直接写泛型函数类型（`-> fun(U,T) throws -> U`）时，U/T 是裸
    // NamedType，registerTypeGenerics 不收集；此处仿其提前注册，使返回类型解析不报 undefined
    if (decl.returnType) registerFuncTypeGenerics(symtab_, *decl.returnType);

    // 3. 注册接收者 self
    {
        Symbol sym;
        sym.kind = SymKind::Parameter;
        sym.name = decl.receiverName;
        sym.type = resolveNamedType(decl.receiverType);
        symtab_.define(std::move(sym));
    }

    // 4. 注册参数（泛型已就绪）
    // G4：同时从解析后的参数类型收集"裸泛型变量"名（含类型别名实例化泄漏的 T）
    std::vector<std::string> fnGenerics;
    for (auto& ta : decl.receiverTypeArgs) fnGenerics.push_back(ta);
    for (auto& p : decl.params) {
        auto resolved = p.type ? resolveType(*p.type) : ErrorSemType::make();
        collectGenericNames(resolved.get(), fnGenerics);
        Symbol sym;
        sym.kind = SymKind::Parameter;
        sym.name = p.name;
        sym.type = std::move(resolved);
        symtab_.define(std::move(sym));
    }

    // 5. 解析返回类型（用 FnCtxGuard 保存/恢复外层上下文）
    auto retType = decl.returnType ? resolveType(*decl.returnType) : nullptr;
    // P1-2：保存解析后的返回类型到 returnType->inferredType（typeStore_ 保活），
    // 供 CodeGen methodSignature 读取（currentReturnVariantCppTypes_ / HasNoneVariant_ 装箱）
    if (retType && decl.returnType) {
        typeStore_.push_back(retType->clone());
        const_cast<TypeExpr*>(decl.returnType.get())->inferredType = typeStore_.back().get();
    }
    collectGenericNames(retType.get(), fnGenerics);
    // G4：压入泛型函数栈（方法体/闭包体检查期间 containsUnresolvedGeneric 据此
    // 判定签名引用的泛型参数可引用），退出方法体时恢复
    fnGenericStack_.push_back(std::move(fnGenerics));
    FnCtxGuard fc(*this, retType ? retType->clone() : nullptr, decl.throws);

    // 默认参数声明规则检查（尾部连续、类型可赋值、泛型参数拒绝）
    checkDefaultArgRules(decl, decl.params);

    if (decl.body) {
        checkBlock(*decl.body);
        // 漏 return 检查：非 None 返回类型方法必须所有路径显式 return
        if (retType && !dynamic_cast<const NoneSemType*>(retType.get())
            && !dynamic_cast<const ErrorSemType*>(retType.get())
            && !blockAllPathsReturn(*decl.body)) {
            error(decl, "method '" + decl.name
                  + "' must return a value on all paths (missing explicit return)");
        }
    }
    fnGenericStack_.pop_back();
    symtab_.exitScope();

    // impl 接口一致性验证
    if (!decl.implInterface.empty()) {
        // bug-07：impl 一致性校验下方 resolveType(参数/返回类型) 需要方法自身裸泛型 U
        // 在作用域（方法体 scope 已在 L241 退出）。临时 Function scope + 注册（仅顶层
        // FunctionType 内部裸泛型），使 `fun (self Box impl Getter) getU() -> fun(U)->U`
        // 的签名解析不报 undefined type 'U'。
        symtab_.enterScope(ScopeKind::Function);
        for (auto& p : decl.params)
            if (p.type) registerFuncTypeGenerics(symtab_, *p.type);
        if (decl.returnType) registerFuncTypeGenerics(symtab_, *decl.returnType);
        // 泛型 record 实现接口 → v1 报错（适配器类型名无法对应，见 C3.2）
        if (!decl.receiverTypeArgs.empty()) {
            error(decl, "generic type '" + decl.receiverType
                  + "' cannot implement interface in v1 (adapter generation unsupported)");
        }
        auto* ifaceSym = symtab_.lookup(decl.implInterface);
        if (!ifaceSym || ifaceSym->kind != SymKind::Interface) {
            error(decl, "interface '" + decl.implInterface + "' not found");
        } else {
            // 泛型接口：implTypeArgs ↔ typeParams 数量校验（substitute 前置条件）
            if (!ifaceSym->typeParams.empty() && decl.implTypeArgs.size() != ifaceSym->typeParams.size()) {
                error(decl, "interface '" + decl.implInterface + "' expects " +
                      std::to_string(ifaceSym->typeParams.size()) + " type argument(s), got " +
                      std::to_string(decl.implTypeArgs.size()));
            } else if (ifaceSym->typeParams.empty() && !decl.implTypeArgs.empty()) {
                error(decl, "interface '" + decl.implInterface + "' is not generic");
            }
            // 接口签名代换：将接口方法签名中的泛型形参（T/U...）替换为 impl 类型实参
            // （如 Comparable<T> 的 cmp(other: T) 在 impl Comparable<Point> 下为 cmp(other: Point)）
            auto substIface = [&](const SemType& t) -> std::unique_ptr<SemType> {
                std::unique_ptr<SemType> cur = t.clone();
                for (size_t k = 0; k < ifaceSym->typeParams.size()
                                  && k < decl.implTypeArgs.size(); ++k) {
                    auto concrete = resolveType(*decl.implTypeArgs[k]);
                    cur = substitute(*cur, ifaceSym->typeParams[k], *concrete);
                }
                return cur;
            };
            bool found = false;
            for (auto& m : ifaceSym->interfaceMethods) {
                if (m.name == decl.name) {
                    found = true;
                    if (decl.params.size() != m.paramTypes.size()) {
                        error(decl, "impl method '" + decl.name + "' expects " +
                              std::to_string(m.paramTypes.size()) + " parameter(s), got " +
                              std::to_string(decl.params.size()));
                    }
                    for (size_t i = 0; i < decl.params.size() && i < m.paramTypes.size(); ++i) {
                        if (decl.params[i].type && m.paramTypes[i]) {
                            auto ifaceParamTy = substIface(*m.paramTypes[i]);
                            auto implParamTy = resolveType(*decl.params[i].type);
                            if (!isAssignable(*ifaceParamTy, *implParamTy)) {
                                error(*decl.params[i].type,
                                      "impl method '" + decl.name + "' parameter " +
                                      std::to_string(i + 1) + " type mismatch: expected '" +
                                      ifaceParamTy->toString() + "', got '" +
                                      implParamTy->toString() + "'");
                            }
                        }
                    }
                    if (decl.returnType && m.returnType) {
                        auto ifaceRetTy = substIface(*m.returnType);
                        auto implRetTy = resolveType(*decl.returnType);
                        if (!isAssignable(*ifaceRetTy, *implRetTy)) {
                            error(*decl.returnType,
                                  "impl method '" + decl.name + "' return type mismatch: expected '" +
                                  ifaceRetTy->toString() + "', got '" +
                                  implRetTy->toString() + "'");
                        }
                    }
                    if (decl.throws != m.throws) {
                        error(decl, "impl method '" + decl.name + "' throws mismatch: interface " +
                              (m.throws ? "requires" : "does not require") + " 'throws'");
                    }
                    break;
                }
            }
            if (!found) {
                error(decl, "interface '" + decl.implInterface +
                      "' has no method '" + decl.name + "'");
            }
        }
        symtab_.exitScope();
    }
}

} // namespace Aura
