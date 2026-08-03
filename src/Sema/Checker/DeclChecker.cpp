#include "Sema/SemAnalyzer.h"

#include <functional>

namespace Aura {

// ============================================================
// 辅助：遍历 TypeExpr 树，对每个泛型类型引用回调 fn(name)
// （统一 collectGenericRefs / registerGenericParams 的 6 分支遍历）
// ============================================================
static void forEachGenericRef(const TypeExpr& type,
                              const std::function<void(const std::string&)>& fn) {
    if (auto* g = dynamic_cast<const GenericTypeRef*>(&type)) { fn(g->name); return; }
    if (auto* n = dynamic_cast<const NamedType*>(&type)) {
        for (auto& arg : n->typeArgs)
            if (arg) forEachGenericRef(*arg, fn);
        return;
    }
    if (auto* l = dynamic_cast<const ListType*>(&type)) {
        if (l->elementType) forEachGenericRef(*l->elementType, fn);
        return;
    }
    if (auto* r = dynamic_cast<const RecordType*>(&type)) {
        for (auto& f : r->fields)
            if (f.type) forEachGenericRef(*f.type, fn);
        return;
    }
    if (auto* u = dynamic_cast<const UnionType*>(&type)) {
        for (auto& v : u->types)
            if (v) forEachGenericRef(*v, fn);
        return;
    }
    if (auto* fnT = dynamic_cast<const FunctionType*>(&type)) {
        for (auto& p : fnT->paramTypes)
            if (p) forEachGenericRef(*p, fn);
        if (fnT->returnType) forEachGenericRef(*fnT->returnType, fn);
    }
}

// 注册 TypeExpr 中所有泛型引用为 GenericParam 符号（checkFunBody/checkMethodBody 复用）
static void registerTypeGenerics(SymbolTable& symtab, const TypeExpr& type) {
    forEachGenericRef(type, [&](const std::string& g) {
        Symbol sym;
        sym.kind = SymKind::GenericParam;
        sym.name = g;
        symtab.define(std::move(sym));
    });
}

// ============================================================
// 第 1 遍：声明顶层符号
// ============================================================

// receiverType 规范名：查符号表 RecordSemType.canonicalName（与 isAssignable 查询一致）
std::string SemAnalyzer::recordTypeKey(const std::string& receiverType) const {
    auto* sym = symtab_.lookup(receiverType);
    if (sym && sym->kind == SymKind::TypeAlias && sym->type) {
        if (auto* rec = dynamic_cast<const RecordSemType*>(sym->type.get()))
            return rec->canonicalName;   // 空 = 匿名 record（无法实现接口）
    }
    return "";   // 非 record 接收者（v1 接口仅支持 record 实现）
}

// 构建 receiverType → 方法签名映射（接口结构匹配数据源）
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

void SemAnalyzer::declareTopLevel(const Program& program) {
    // 内置接口先注册（interfaces.aurai：Stringer/Comparable/Iterator），
    // 用户 decls 可引用内置接口（如 impl Comparable<Point>）
    for (auto& i : BuiltinRegistry::get().auraiInterfaces())
        declareInterface(*i);
    for (auto& d : program.decls) {
        if (d) declareDecl(*d);
    }
    // 第 1 遍末尾统一构建 typeMethods_：此时所有 record/interface/方法符号已注册，
    // recordTypeKey 的 resolveType 安全（declareDecl 阶段前向/自引用类型可能未注册）
    buildTypeMethods(program);
    // 显式 impl 收集（第 1 遍末尾，record 类型已全部注册，recordTypeKey 安全）
    recordImplIfaces_.clear();
    for (auto& d : program.decls) {
        if (auto* m = dynamic_cast<const MethodDecl*>(d.get())) {
            if (m->implInterface.empty() || m->receiverType.empty()) continue;
            std::string key = recordTypeKey(m->receiverType);
            if (!key.empty())
                recordImplIfaces_[key].insert(m->implInterface);
        }
    }
    // 显式 impl 完整性验证：record 声明 impl 接口 → 接口所有非默认方法必须已实现
    verifyImplCompleteness(program);
}

// ============================================================
// 显式 impl 完整性验证（第 1 遍末尾）
// ============================================================
void SemAnalyzer::verifyImplCompleteness(const Program& program) {
    for (auto& [recKey, ifaces] : recordImplIfaces_) {
        for (auto& ifaceName : ifaces) {
            auto* sym = symtab_.lookup(ifaceName);
            if (!sym || sym->kind != SymKind::Interface) continue;  // 未找到接口 → 已报错
            auto tmIt = typeMethods_.find(recKey);
            for (auto& m : sym->interfaceMethods) {
                if (m.hasDefault) continue;  // 默认方法豁免
                bool found = false;
                if (tmIt != typeMethods_.end()) {
                    for (auto& rm : tmIt->second) {
                        if (rm.name != m.name) continue;
                        found = matchFuncSig(m.paramTypes, m.returnType.get(), m.throws,
                                             rm.paramTypes, rm.returnType.get(), rm.throws);
                        break;
                    }
                }
                if (!found) {
                    // 定位报错节点：该 record 任意一个带 impl 的方法声明
                    const MethodDecl* anchor = nullptr;
                    for (auto& d : program.decls) {
                        if (auto* md = dynamic_cast<const MethodDecl*>(d.get())) {
                            if (md->implInterface == ifaceName && !md->receiverType.empty()
                                && recordTypeKey(md->receiverType) == recKey) {
                                anchor = md;
                                break;
                            }
                        }
                    }
                    if (anchor) {
                        error(*anchor,
                              "type '" + recKey + "' implements interface '" + ifaceName +
                              "' but does not implement required method '" + m.name + "'");
                    } else {
                        error(0, 0, "type '" + recKey + "' implements interface '" + ifaceName +
                              "' but does not implement required method '" + m.name + "'");
                    }
                }
            }
        }
    }
}

void SemAnalyzer::declareDecl(const Decl& decl) {
    // 模块级 pub 策略：pub 仅可修饰声明（type/fun/方法/构造函数）
    // pub import 为错误（C6-3）；config 语法待定，不参与策略（C6-4）
    if (decl.isPublic && dynamic_cast<const ImportDecl*>(&decl)) {
        error(decl, "pub cannot be applied to import declarations");
    } else if (decl.isPublic && !dynamic_cast<const ConfigDecl*>(&decl)) {
        hasAnyPub_ = true;
    }
    if (auto* t = dynamic_cast<const TypeDecl*>(&decl)) {
        // 注册类型泛型参数（如 type Stack<T> 中的 T）
        for (auto& tp : t->typeParams) {
            Symbol tpSym;
            tpSym.kind = SymKind::GenericParam;
            tpSym.name = tp;
            symtab_.defineGlobal(std::move(tpSym));
        }

        // 先注册前向声明（解决自引用类型如 Tree<T> = {..., children: [Tree<T>]}）
        Symbol fwd;
        fwd.kind = SymKind::TypeAlias;
        fwd.name = t->name;
        fwd.isPublic = t->isPublic;  // Phase B
        fwd.typeParams = t->typeParams;    // 泛型参数名列表
        fwd.type = ErrorSemType::make();  // 占位符，resolveType 完成后覆盖
        symtab_.defineGlobal(std::move(fwd));
        resolvingTypes_.insert(t->name);

        if (t->type) {
            // 检查类型表达式中使用的泛型参数是否都已声明
            std::set<std::string> usedGenerics;
            forEachGenericRef(*t->type, [&](const std::string& g) { usedGenerics.insert(g); });
            for (const std::string& g : usedGenerics) {
                bool declared = false;
                for (const std::string& tp : t->typeParams) {
                    if (g == tp) { declared = true; break; }
                }
                if (!declared) {
                    error(*t->type, "undefined type parameter '" + g + "' in type '" + t->name + "'; declare it with type " + t->name + "<" + g + ">");
                }
            }

            auto resolved = resolveType(*t->type);
            // 记录类型的规范名（如 "Tree"），供 CodeGen 映射 C++ 类型
            if (auto* rec = dynamic_cast<RecordSemType*>(resolved.get())) {
                rec->canonicalName = t->name;
                // 非泛型自引用类型（如 type IntTree = { children: [IntTree] }）：
                // resolveType 在解析自引用字段时返回 GenericSemType(name="IntTree")
                // 但 resolvedName 为空。sealSelfRefs 将 resolvedName 设为类型名，
                // 使 CodeGen 的 mapSemType 能正确映射为 C++ 类型名（而非 "auto"）
                // 泛型类型的 sealSelfRefs 在 materializeCanonicalName 中调用
                if (t->typeParams.empty()) {
                    sealSelfRefs(resolved, t->name, t->name);
                }
            }
            // 用完整类型更新占位符
            auto* existing = symtab_.lookupGlobal(t->name);
            if (existing) existing->type = std::move(resolved);
        }

        resolvingTypes_.erase(t->name);
        return;
    }
    if (auto* i = dynamic_cast<const InterfaceDecl*>(&decl)) {
        declareInterface(*i);
        return;
    }
    if (auto* f = dynamic_cast<const FunDecl*>(&decl)) {
        Symbol sym;
        sym.kind   = SymKind::Function;
        sym.name   = f->name;
        sym.throws = f->throws;
        sym.isPublic = f->isPublic;  // Phase B

        // 先注册泛型参数（从参数类型中提取 <T>/<U> 等），
        // 再解析参数类型和返回类型，这样 Tree<U> 中的 U 才能正确解析。
        symtab_.enterScope(ScopeKind::Function);
        for (auto& p : f->params) {
            if (p.type) registerTypeGenerics(symtab_, *p.type);
        }
        if (f->returnType) registerTypeGenerics(symtab_, *f->returnType);

        for (auto& p : f->params) {
            SymParam sp;
            sp.name = p.name;
            sp.type = p.type ? resolveType(*p.type) : ErrorSemType::make();
            if (p.defaultExpr) { sp.defaultExpr = p.defaultExpr->clone(); sp.hasDefault = true; }
            sym.params.push_back(std::move(sp));
        }
        sym.type = f->returnType ? resolveType(*f->returnType) : nullptr;
        symtab_.exitScope();

        symtab_.defineGlobal(std::move(sym));
        return;
    }
    if (auto* m = dynamic_cast<const MethodDecl*>(&decl)) {
        Symbol sym;
        sym.kind   = SymKind::Method;
        sym.name   = m->name;
        sym.throws = m->throws;
        sym.isPublic = m->isPublic;  // Phase B
        for (auto& p : m->params) {
            SymParam sp;
            sp.name = p.name;
            sp.type = p.type ? resolveType(*p.type) : ErrorSemType::make();
            if (p.defaultExpr) { sp.defaultExpr = p.defaultExpr->clone(); sp.hasDefault = true; }
            sym.params.push_back(std::move(sp));
        }
        sym.type = m->returnType ? resolveType(*m->returnType) : nullptr;
        symtab_.defineGlobal(std::move(sym));
        return;
    }
}

// ============================================================
// AST 类型 → 语义类型
// ============================================================

std::unique_ptr<SemType> SemAnalyzer::resolveType(const TypeExpr& astType) {
    if (auto* n = dynamic_cast<const NamedType*>(&astType)) {
        std::string fullName = n->name;
        if (!n->namespacePrefix.empty())
            fullName = n->namespacePrefix[0] + "." + n->name;
        auto result = resolveNamedType(fullName);
        // sync 命名空间限定的内置类型（如 sync.Mutex）：
        // BuiltinRegistry 中只注册 "Mutex"，需用 n->name 再查一次
        if (dynamic_cast<const ErrorSemType*>(result.get()) &&
            n->namespacePrefix.size() == 1 && n->namespacePrefix[0] == "sync") {
            result = resolveNamedType(n->name);
        }
        // 未找到类型 → 报错（Io/Path 为内置能力类型，由 CodeGen 注册）
        if (dynamic_cast<const ErrorSemType*>(result.get()) &&
            fullName != "None" && fullName != "int" && fullName != "float" &&
            fullName != "bool" && fullName != "string" && fullName != "Io" && fullName != "Path") {
            error(n->line, n->col, "undefined type '" + fullName + "'");
        }
        // 若有名称类型且有泛型实参（如 Tree<int>），用 substitute 将泛型形参替换为实参
        if (!n->typeArgs.empty()) {
            auto* sym = symtab_.lookup(fullName);
            if (sym && sym->kind == SymKind::TypeAlias && !sym->typeParams.empty()) {
                // 泛型实参数必须与声明一致（缺省/多余均报错，D4）
                if (n->typeArgs.size() != sym->typeParams.size()) {
                    error(*n, "type '" + n->name + "' expects "
                          + std::to_string(sym->typeParams.size())
                          + " type argument(s), got " + std::to_string(n->typeArgs.size()));
                }
                // 用户自定义泛型：applyTypeArgs 替换形参为实参 + materializeCanonicalName
                result = applyTypeArgs(std::move(result), *sym, n->typeArgs);
                materializeCanonicalName(result, *n);
            } else {
                // 内置泛型（如 sync.Channel<int> / channel<int>）：result 为 GenericSemType
                // 无 typeParams 可替换，仅设置 resolvedName 供 CodeGen / for-in 提取元素类型
                materializeCanonicalName(result, *n);
            }
        }
        return result;
    }
    if (auto* l = dynamic_cast<const ListType*>(&astType)) {
        auto t = std::make_unique<ListSemType>();
        t->elementType = l->elementType ? resolveType(*l->elementType) : ErrorSemType::make();
        return t;
    }
    if (auto* r = dynamic_cast<const RecordType*>(&astType)) {
        auto t = std::make_unique<RecordSemType>();
        for (auto& f : r->fields) {
            t->fields.push_back({f.name, f.type ? resolveType(*f.type) : ErrorSemType::make()});
        }
        return t;
    }
    if (auto* u = dynamic_cast<const UnionType*>(&astType)) {
        auto t = std::make_unique<UnionSemType>();
        for (auto& v : u->types) {
            t->variants.push_back(v ? resolveType(*v) : ErrorSemType::make());
        }
        return t;
    }
    if (auto* fn = dynamic_cast<const FunctionType*>(&astType)) {
        auto t = std::make_unique<FuncSemType>();
        for (auto& p : fn->paramTypes) {
            t->paramTypes.push_back(p ? resolveType(*p) : ErrorSemType::make());
        }
        t->returnType = fn->returnType ? resolveType(*fn->returnType) : nullptr;
        t->throws = fn->throws;
        return t;
    }
    if (auto* g = dynamic_cast<const GenericTypeRef*>(&astType)) {
        auto t = std::make_unique<GenericSemType>();
        t->name = g->name;
        return t;
    }
    return ErrorSemType::make();
}

// ============================================================
// 接口符号注册（用户接口 + 内置 .aurai 接口共用）
// ============================================================
void SemAnalyzer::declareInterface(const InterfaceDecl& i) {
    Symbol sym;
    sym.kind = SymKind::Interface;
    sym.name = i.name;
    sym.isPublic = i.isPublic;  // Phase B
    sym.typeParams = i.typeParams;  // 泛型参数名（checkMethodBody substitute 用）
    // 泛型参数先注册（方法签名可能引用 T，如 cmp(other: T)）
    symtab_.enterScope(ScopeKind::Function);
    for (auto& tp : i.typeParams) {
        Symbol tpSym;
        tpSym.kind = SymKind::GenericParam;
        tpSym.name = tp;
        symtab_.define(std::move(tpSym));
    }
    for (auto& m : i.methods) {
        InterfaceSemType::MethodSig sig;
        sig.name   = m.name;
        for (auto& p : m.params)
            sig.paramTypes.push_back(p.type ? resolveType(*p.type) : ErrorSemType::make());
        sig.returnType = m.returnType ? resolveType(*m.returnType) : nullptr;
        sig.throws = m.throws;
        sig.hasDefault = m.defaultBody != nullptr;   // 默认方法豁免结构匹配
        sym.interfaceMethods.push_back(std::move(sig));
    }
    symtab_.exitScope();
    symtab_.defineGlobal(std::move(sym));
}

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
        // 默认值表达式声明处求值检查（inferExpr 写入 defaultExpr->inferredType，C5 复用）
        auto dt = inferExpr(*p.defaultExpr);
        if (dynamic_cast<const ErrorSemType*>(dt.get())) {
            error(*p.defaultExpr, "invalid default argument for parameter '" + p.name + "'");
            continue;
        }
        if (p.type) {
            auto pt = resolveType(*p.type);
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
static bool stmtAllPathsReturn(const Stmt& stmt);

static bool blockAllPathsReturn(const BlockStmt& block) {
    for (auto& s : block.stmts) {
        if (!s) continue;
        // 一旦遇到终结语句（return/throw），其后的语句不可达
        if (stmtAllPathsReturn(*s)) return true;
    }
    return false;
}

static bool stmtAllPathsReturn(const Stmt& stmt) {
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
    }
    if (decl.returnType) registerTypeGenerics(symtab_, *decl.returnType);

    // 2. 注册参数（此时泛型已可解析）
    for (auto& p : decl.params) {
        Symbol sym;
        sym.kind = SymKind::Parameter;
        sym.name = p.name;
        sym.type = p.type ? resolveType(*p.type) : ErrorSemType::make();
        symtab_.define(std::move(sym));
    }

    // 3. 解析返回类型（泛型已注册，T 可正确解析为 GenericSemType）
    //    用 FnCtxGuard 保存/恢复外层上下文（支持闭包体嵌套检查）
    auto retType = decl.returnType ? resolveType(*decl.returnType) : nullptr;
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
    }
    if (decl.returnType) registerTypeGenerics(symtab_, *decl.returnType);

    // 3. 注册接收者 self
    {
        Symbol sym;
        sym.kind = SymKind::Parameter;
        sym.name = decl.receiverName;
        sym.type = resolveNamedType(decl.receiverType);
        symtab_.define(std::move(sym));
    }

    // 4. 注册参数（泛型已就绪）
    for (auto& p : decl.params) {
        Symbol sym;
        sym.kind = SymKind::Parameter;
        sym.name = p.name;
        sym.type = p.type ? resolveType(*p.type) : ErrorSemType::make();
        symtab_.define(std::move(sym));
    }

    // 5. 解析返回类型（用 FnCtxGuard 保存/恢复外层上下文）
    auto retType = decl.returnType ? resolveType(*decl.returnType) : nullptr;
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
    symtab_.exitScope();

    // impl 接口一致性验证
    if (!decl.implInterface.empty()) {
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
    }
}

} // namespace Aura
