#include "SemAnalyzer.h"
#include "BuiltinRegistry.h"
#include <algorithm>

namespace Aura {

// ============================================================
// 构造 & 主入口
// ============================================================

SemAnalyzer::SemAnalyzer(DiagnosticEngine& diag) : diag_(diag) {}

bool SemAnalyzer::analyze(const Program& program) {
    declareTopLevel(program);
    checkProgram(program);
    return !diag_.hasErrors();
}

// ============================================================
// 错误记录
// ============================================================

void SemAnalyzer::error(const ASTNode& node, const std::string& msg) {
    diag_.error(node, msg);
}

void SemAnalyzer::error(const ASTNode& node, DiagCode code, const std::string& msg, const std::string& hint) {
    diag_.error(node, code, msg, hint);
}

void SemAnalyzer::error(int line, int col, const std::string& msg) {
    diag_.error(line, col, msg);
}

void SemAnalyzer::error(int line, int col, DiagCode code, const std::string& msg, const std::string& hint) {
    diag_.error(line, col, code, msg, hint);
}

// ============================================================
// 语义类型工具
// ============================================================

namespace {
// 将 Aura 类型名映射为 C++ 类型名（未注册的类型保持原名）
std::string cppNameOf(const std::string& auraName) {
    if (auto* ti = BuiltinRegistry::get().findType(auraName))
        return ti->cppType;
    return auraName;
}

// 将 canonicalName（如 "Pair<A, B>"）模板参数列表中名为 name 的形参替换为 cppName
std::string replaceCanonicalArg(const std::string& canonicalName,
                                const std::string& name,
                                const std::string& cppName) {
    auto lt = canonicalName.find('<');
    auto rt = canonicalName.rfind('>');
    if (lt == std::string::npos || rt == std::string::npos || rt < lt)
        return canonicalName;
    std::string head = canonicalName.substr(0, lt + 1);  // 含 '<'
    std::string tail = canonicalName.substr(rt);         // 含 '>'
    std::string args = canonicalName.substr(lt + 1, rt - lt - 1);
    // 按逗号（括号深度 0）分割，支持嵌套泛型如 Pair<Stack<int>, B>
    std::vector<std::string> parts;
    std::string cur;
    int depth = 0;
    for (char c : args) {
        if (c == '<') ++depth;
        else if (c == '>') --depth;
        if (c == ',' && depth == 0) {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    parts.push_back(cur);
    std::string joined;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) joined += ", ";
        auto b = parts[i].find_first_not_of(" \t");
        auto e = parts[i].find_last_not_of(" \t");
        std::string tok = (b == std::string::npos)
            ? "" : parts[i].substr(b, e - b + 1);
        joined += (tok == name) ? cppName : parts[i];
    }
    return head + joined + tail;
}

// 将导出的类型中的所有 RecordSemType canonicalName 加上模块别名前缀
// （如 "Pair<A, B>" → "math::Pair<A, B>"），使 CodeGen mapSemType 输出完整 C++ 类型名。
// C++ 侧 CodeGen 会生成 `namespace math = aura_mod_math_utils;` 别名。
void qualifyRecordTypes(std::unique_ptr<SemType>& t, const std::string& alias) {
    if (!t || alias.empty()) return;
    if (auto* rec = dynamic_cast<RecordSemType*>(t.get())) {
        if (!rec->canonicalName.empty()
            && rec->canonicalName.find("::") == std::string::npos) {
            rec->canonicalName = alias + "::" + rec->canonicalName;
        }
        for (auto& f : rec->fields)
            if (f.type) qualifyRecordTypes(f.type, alias);
    } else if (auto* l = dynamic_cast<ListSemType*>(t.get())) {
        qualifyRecordTypes(l->elementType, alias);
    } else if (auto* u = dynamic_cast<UnionSemType*>(t.get())) {
        for (auto& v : u->variants)
            qualifyRecordTypes(v, alias);
    } else if (auto* f = dynamic_cast<FuncSemType*>(t.get())) {
        for (auto& p : f->paramTypes)
            qualifyRecordTypes(p, alias);
        qualifyRecordTypes(f->returnType, alias);
    } else if (auto* o = dynamic_cast<OptionalSemType*>(t.get())) {
        qualifyRecordTypes(o->elementType, alias);
    } else if (auto* it = dynamic_cast<IterSemType*>(t.get())) {
        qualifyRecordTypes(it->elementType, alias);
    }
}
} // namespace

std::unique_ptr<SemType> SemAnalyzer::semTypeFromAuraName(const std::string& name) {
    // 先查 BuiltinRegistry：基础类型 → Prim；其他内置类型 → GenericSemType 占位
    if (auto* ti = BuiltinRegistry::get().findType(name)) {
        switch (ti->primKind) {
            case BuiltinPrim::Int:    return intType();
            case BuiltinPrim::Float:  return floatType();
            case BuiltinPrim::Bool:   return boolType();
            case BuiltinPrim::String: return stringType();
            case BuiltinPrim::None_:  return ErrorSemType::make(); // None 不能独立使用
            case BuiltinPrim::Other: {
                auto t = std::make_unique<GenericSemType>();
                t->name = name;
                return t;
            }
        }
    }
    return ErrorSemType::make();
}

std::unique_ptr<SemType> SemAnalyzer::semTypeFromCppName(const std::string& cppName) {
    // 反向映射：C++ 类型名 → Aura 类型名 → SemType（如 "int32_t" → intType）
    if (auto* ti = BuiltinRegistry::get().findByCppType(cppName)) {
        auto t = semTypeFromAuraName(ti->name);
        if (!dynamic_cast<const ErrorSemType*>(t.get()))
            return t;
    }
    // 未注册的 C++ 类型（record 指针 / Array<T>* 等）：作为堆对象占位
    auto g = std::make_unique<GenericSemType>();
    g->name = cppName;
    g->resolvedName = cppName;
    return g;
}

// SemType → C++ 类型名（提升自匿名命名空间，供 ExprInfer 的 Iterator 桥接方法推导复用）
std::string SemAnalyzer::semTypeToCppName(const SemType& t) const {
    if (auto* p = dynamic_cast<const PrimSemType*>(&t)) {
        switch (p->kind) {
            case PrimSemType::Int:    return "int32_t";
            case PrimSemType::Float:  return "double";
            case PrimSemType::Bool:   return "bool";
            case PrimSemType::String: return "aura_rt::GcString*";
        }
    }
    if (dynamic_cast<const NoneSemType*>(&t)) return "aura_rt::NoneType";
    if (auto* l = dynamic_cast<const ListSemType*>(&t))
        return "aura_rt::Array<" + semTypeToCppName(*l->elementType) + ">*";
    if (auto* r = dynamic_cast<const RecordSemType*>(&t))
        return r->canonicalName + "*";
    if (auto* g = dynamic_cast<const GenericSemType*>(&t)) {
        if (!g->resolvedName.empty()) return g->resolvedName;
        return cppNameOf(g->name);
    }
    if (auto* o = dynamic_cast<const OptionalSemType*>(&t)) {
        std::string elem = o->elementType ? semTypeToCppName(*o->elementType) : "int32_t";
        return "aura_rt::Optional<" + elem + ">*";
    }
    if (auto* it = dynamic_cast<const IterSemType*>(&t)) {
        std::string elem = it->elementType ? semTypeToCppName(*it->elementType) : "int32_t";
        return "aura_rt::Iterator<" + elem + ">";
    }
    return "auto";
}

bool SemAnalyzer::isIteratorType(const SemType* t) const {
    if (!t) return false;
    if (auto* g = dynamic_cast<const GenericSemType*>(t))
        return g->name == "Iterator" || g->resolvedName.find("Iterator") != std::string::npos;
    if (auto* is = dynamic_cast<const InterfaceSemType*>(t))
        return is->name == "Iterator";
    return false;
}

std::unique_ptr<SemType> SemAnalyzer::elemTypeOf(const SemType* iterType) {
    if (!iterType) return ErrorSemType::make();
    if (auto* listTy = dynamic_cast<const ListSemType*>(iterType))
        return listTy->elementType ? listTy->elementType->clone() : ErrorSemType::make();
    if (auto* iterTy = dynamic_cast<const IterSemType*>(iterType))
        return iterTy->elementType ? iterTy->elementType->clone() : ErrorSemType::make();
    if (auto* os = dynamic_cast<const OptionalSemType*>(iterType))
        return os->elementType ? os->elementType->clone() : ErrorSemType::make();
    if (auto* gs = dynamic_cast<const GenericSemType*>(iterType)) {
        // sync.Channel<int32_t> / channel<int32_t> 等泛型类型：从 resolvedName 提取 <...> 内元素
        if (!gs->resolvedName.empty()) {
            auto lt = gs->resolvedName.find('<');
            auto rt = gs->resolvedName.rfind('>');
            if (lt != std::string::npos && rt != std::string::npos && rt > lt)
                return semTypeFromCppName(gs->resolvedName.substr(lt + 1, rt - lt - 1));
        }
    }
    return ErrorSemType::make();
}

std::unique_ptr<SemType> SemAnalyzer::resolveNamedType(const std::string& name) {
    // None 只能作为联合变体 / match 常量出现（E017 在 AST 层拦截独立标注）
    // semTypeFromAuraName 对 None_ 返回 ErrorSemType，此处先特判保证 `T | None` 变体正确
    if (name == "None") return NoneSemType::make();

    // 先查 BuiltinRegistry（int→intType、Io/Path→GenericSemType 占位等）
    auto builtin = semTypeFromAuraName(name);
    if (!dynamic_cast<const ErrorSemType*>(builtin.get()))
        return builtin;

    // 用户定义类型
    auto* sym = symtab_.lookup(name);
    if (sym && sym->kind == SymKind::TypeAlias) {
        // 自引用检测：该类型正在解析中（如 Tree<T> = {..., children: [Tree<T>]}）
        if (resolvingTypes_.count(name)) {
            // 返回占位符类型，打破无限递归
            auto g = std::make_unique<GenericSemType>();
            g->name = name;
            return g;
        }
        return sym->type ? sym->type->clone() : ErrorSemType::make();
    }

    // 接口类型
    if (sym && sym->kind == SymKind::Interface) {
        auto t = std::make_unique<InterfaceSemType>();
        t->name = sym->name;
        for (auto& m : sym->interfaceMethods) {
            InterfaceSemType::MethodSig ms;
            ms.name = m.name;
            for (auto& pt : m.paramTypes)
                ms.paramTypes.push_back(pt ? pt->clone() : nullptr);
            ms.returnType = m.returnType ? m.returnType->clone() : nullptr;
            ms.throws = m.throws;
            ms.hasDefault = m.hasDefault;
            ms.hasCppImpl = m.hasCppImpl;
            t->methods.push_back(std::move(ms));
        }
        return t;
    }

    // 泛型参数引用（如裸 T，由 resolveType 上下文提供）
    if (sym && sym->kind == SymKind::GenericParam) {
        auto t = std::make_unique<GenericSemType>();
        t->name = name;
        return t;
    }

    // 未找到：返回 Error 类型（后续阶段会报错）
    return ErrorSemType::make();
}

bool SemAnalyzer::matchFuncSig(
    const std::vector<std::unique_ptr<SemType>>& aParams,
    const SemType* aReturn, bool aThrows,
    const std::vector<std::unique_ptr<SemType>>& bParams,
    const SemType* bReturn, bool bThrows) const {
    if (aThrows != bThrows) return false;
    if (aParams.size() != bParams.size()) return false;
    for (size_t i = 0; i < aParams.size(); ++i)
        if (!isAssignable(*aParams[i], *bParams[i])) return false;
    if (aReturn && bReturn)
        return isAssignable(*aReturn, *bReturn);
    return !aReturn && !bReturn;
}

std::unique_ptr<SemType> SemAnalyzer::semTypeFromBuiltinReturn(
    const ReturnTypeInfo& ret, const SemType* objType) {
    switch (ret.kind) {
        case ReturnTypeInfo::Kind::None:
            return NoneSemType::make();
        case ReturnTypeInfo::Kind::Named: {
            // 基础类型 / 内置类型 → 统一映射（int→intType、Io→GenericSemType 等）
            auto named = semTypeFromAuraName(ret.typeName);
            if (!dynamic_cast<const ErrorSemType*>(named.get()))
                return named;
            // [T] 列表类型：元素类型为注册的内置类型则映射为 GenericSemType
            if (ret.typeName.size() >= 2 && ret.typeName[0] == '[' && ret.typeName.back() == ']') {
                auto lt = std::make_unique<ListSemType>();
                std::string inner = ret.typeName.substr(1, ret.typeName.size() - 2);
                if (BuiltinRegistry::get().findType(inner)) {
                    auto g = std::make_unique<GenericSemType>();
                    g->name = inner;
                    lt->elementType = std::move(g);
                } else {
                    lt->elementType = ErrorSemType::make();
                }
                typeStore_.push_back(std::move(lt));
                return typeStore_.back()->clone();
            }
            return ErrorSemType::make();
        }
        case ReturnTypeInfo::Kind::Generator:
            return IterSemType::make(semTypeFromAuraName(ret.typeName));
        case ReturnTypeInfo::Kind::Iterator: {
            // Iterator(elemType)：如 range → Iterator<int>（元素类型固定，无需 objType）
            // 用 GenericSemType{name="Iterator", resolvedName} 表达：
            //   - CodeGen 特判 name=="Iterator"（C3.3 genMethodCall / C3.4 genForStmt）
            //   - mapSemType 映射 resolvedName + "*" → aura_rt::Iterator<int32_t>*
            auto g = std::make_unique<GenericSemType>();
            g->name = "Iterator";
            g->resolvedName = "aura_rt::Iterator<" + cppNameOf(ret.typeName) + ">";
            typeStore_.push_back(std::move(g));
            return typeStore_.back()->clone();
        }
        case ReturnTypeInfo::Kind::Optional: {
            // Optional<T>: 从 objType 提取元素类型构造 OptionalSemType
            // sync.Channel<T>.receive() 时 objType 应携带元素类型信息（resolvedName）
            if (!objType) return ErrorSemType::make();
            auto elem = elemTypeOf(objType);
            if (dynamic_cast<const ErrorSemType*>(elem.get()))
                return OptionalSemType::make(intType());  // fallback: 无 resolvedName 时默认 int（保持原行为）
            return OptionalSemType::make(std::move(elem));
        }
        case ReturnTypeInfo::Kind::Generic: {
            // Generic(idx, fallback): 从 objType 提取第 idx 个类型参数
            // fallback 决定返回形状：
            //   "[T]"  → 返回与 objType 相同的列表类型（如 front/back/pop/remove/slice）
            //   "string" → 返回 string（如 concat）
            //   "channel" → 返回 channel 的元素类型（如 receive）
            if (!objType) return ErrorSemType::make();

            // fallback == "[T]" → 返回与 objType 相同的列表类型
            if (ret.typeName == "[T]") {
                if (auto* lt = dynamic_cast<const ListSemType*>(objType)) {
                    return lt->clone();
                }
                return ErrorSemType::make();
            }
            // fallback == "T" → 返回列表元素类型（front/back/pop/remove）
            if (ret.typeName == "T") {
                return elemTypeOf(objType);
            }
            // fallback == "string" → 返回 string
            if (ret.typeName == "string") {
                return stringType();
            }
            // fallback == "channel" → 返回 channel 的元素类型（D1 修复：原实现直接 return ErrorSemType）
            if (ret.typeName == "channel") {
                return elemTypeOf(objType);
            }
            return ErrorSemType::make();
        }
    }
    return ErrorSemType::make();
}

bool SemAnalyzer::isAssignable(const SemType& target, const SemType& source) const {
    // 静默传播 error 类型
    if (dynamic_cast<const ErrorSemType*>(&target) || dynamic_cast<const ErrorSemType*>(&source))
        return true;

    // 泛型形参（未解析，resolvedName 为空，如泛型函数体内的 T）接受一切（实例化时再检查）
    // 已解析的泛型类型（如 Iterator<string>）与同为泛型的 source 必须精确比较
    // （P0.5：equals 比较 resolvedName，防止 Iterator<int> ≡ Iterator<string> 混淆）；
    // source 为结构化推断类型（如 some() 的 OptionalSemType）时保持旧放行语义——
    // 类型标注 Optional<int> 物化为 GenericSemType 而工厂推断为 OptionalSemType，
    // 二者同义，直接比较会误报（历史表示不一致，不在本次修复范围）
    if (auto* gt = dynamic_cast<const GenericSemType*>(&target)) {
        if (gt->resolvedName.empty()) return true;
        if (auto* gs = dynamic_cast<const GenericSemType*>(&source))
            return gt->equals(*gs);
        return true;
    }

    // 泛型参数作为 source：查类型别名获取实际类型再做兼容检查
    // 处理递归类型引用（如 Tree<T> 内 children: [Tree<T>]，自引用产生 GenericSemType("Tree")）
    if (auto* gs = dynamic_cast<const GenericSemType*>(&source)) {
        auto* sym = symtab_.lookup(gs->name);
        if (sym && sym->kind == SymKind::TypeAlias && sym->type) {
            return isAssignable(target, *sym->type);
        }
        return false;
    }

    // 联合类型：source 匹配任一变体即为可赋值
    if (auto* u = dynamic_cast<const UnionSemType*>(&target)) {
        for (auto& v : u->variants) {
            if (v && isAssignable(*v, source))
                return true;
        }
        // none() 占位（Optional<error>，元素类型未知）→ 联合含 None 变体时视为 None 赋值放行
        if (auto* os = dynamic_cast<const OptionalSemType*>(&source)) {
            if (dynamic_cast<const ErrorSemType*>(os->elementType.get())) {
                for (auto& v : u->variants)
                    if (v && dynamic_cast<const NoneSemType*>(v.get())) return true;
            }
        }
        return false;
    }

    // 列表类型：元素类型兼容即兼容
    if (auto* lt = dynamic_cast<const ListSemType*>(&target)) {
        if (auto* ls = dynamic_cast<const ListSemType*>(&source)) {
            // 元素类型未知（null）时放行，避免空指针解引用（编译期类型未知，运行时验证）
            if (!lt->elementType || !ls->elementType) return true;
            return isAssignable(*lt->elementType, *ls->elementType);
        }
        return false;
    }

    // Optional<T>：双方元素类型需可赋值；Error 元素（none() 占位/未知类型）静默兼容
    // 非 Optional 源：None 值允许；T 值允许隐式包装为 some(T)（T | None → Optional<T> 语法糖）
    if (auto* oa = dynamic_cast<const OptionalSemType*>(&target)) {
        if (auto* ob = dynamic_cast<const OptionalSemType*>(&source)) {
            if (dynamic_cast<const ErrorSemType*>(oa->elementType.get())
                || dynamic_cast<const ErrorSemType*>(ob->elementType.get()))
                return true;
            return isAssignable(*oa->elementType, *ob->elementType);
        }
        if (dynamic_cast<const NoneSemType*>(&source)) return true;
        if (dynamic_cast<const ErrorSemType*>(oa->elementType.get())) return true;
        return isAssignable(*oa->elementType, source);
    }

    // 函数类型：逐参数检查（支持泛型参数）
    if (auto* ft = dynamic_cast<const FuncSemType*>(&target)) {
        if (auto* fs = dynamic_cast<const FuncSemType*>(&source)) {
            return matchFuncSig(ft->paramTypes, ft->returnType.get(), ft->throws,
                               fs->paramTypes, fs->returnType.get(), fs->throws);
        }
        return false;
    }

    // 接口类型：单方法接口可由函数类型（闭包）满足；具体 record 结构匹配（结构类型系统）
    if (auto* iface = dynamic_cast<const InterfaceSemType*>(&target)) {
        // 0. 接口视图 → 接口：同名接口视图值可直接透传（P1 视图按值传参，
        //    视图 { 方法Fn, self } 已由 let/参数构造，直接转发给同接口参数）
        if (auto* si = dynamic_cast<const InterfaceSemType*>(&source))
            return iface->name == si->name;
        // 1. 闭包 → 单方法接口（现有路径保留）
        if (auto* func = dynamic_cast<const FuncSemType*>(&source)) {
            if (iface->methods.size() == 1) {
                auto& m = iface->methods[0];
                return matchFuncSig(m.paramTypes, m.returnType.get(), m.throws,
                                   func->paramTypes, func->returnType.get(), func->throws);
            }
            return false;
        }
        // 2. 具体 record → 必须显式 impl 该接口（无结构匹配）
        //    （impl 方法签名一致性由 checkMethodBody 验证；完整性由 verifyImplCompleteness 验证）
        if (auto* rec = dynamic_cast<const RecordSemType*>(&source)) {
            auto it = recordImplIfaces_.find(rec->canonicalName);   // 空 = 匿名 record
            if (it == recordImplIfaces_.end()) return false;
            return it->second.count(iface->name) > 0;
        }
        return false; // 其他类型不能满足接口
    }

    // 记录类型：结构匹配，用 isAssignable 而非 equals（支持 ErrorSemType / GenericSemType 容错）
    if (auto* rt = dynamic_cast<const RecordSemType*>(&target)) {
        if (auto* rs = dynamic_cast<const RecordSemType*>(&source)) {
            if (rt->fields.size() != rs->fields.size()) return false;
            for (auto& tf : rt->fields) {
                auto it = std::find_if(rs->fields.begin(), rs->fields.end(),
                    [&](const RecordFieldSem& sf) { return sf.name == tf.name; });
                if (it == rs->fields.end()) return false;
                if (!tf.type || !it->type) return true;  // 字段类型未知：放行
                if (!isAssignable(*tf.type, *it->type)) return false;
            }
            return true;
        }
        return false;
    }

    return target.equals(source);
}

std::unique_ptr<SemType> SemAnalyzer::substitute(
    const SemType& type, const std::string& genericName, const SemType& concrete) {
    // GenericSemType(name) → concrete；否则深拷贝
    if (auto* g = dynamic_cast<const GenericSemType*>(&type)) {
        if (g->name == genericName) return concrete.clone();
        // 已物化的内置泛型（如 Optional<T> → resolvedName="aura_rt::Optional<T>"）：
        // 形参名嵌在 resolvedName 内，须同步替换（复用 RecordSemType canonicalName 的
        // replaceCanonicalArg），否则接口签名代换失效 → Optional<T> ≠ Optional<int> 误报
        if (!g->resolvedName.empty()) {
            std::string newName = replaceCanonicalArg(g->resolvedName, genericName,
                                                      semTypeToCppName(concrete));
            if (newName != g->resolvedName) {
                auto n = std::make_unique<GenericSemType>();
                n->name = g->name;
                n->resolvedName = newName;
                return n;
            }
        }
    }
    // 复合类型递归替换
    if (auto* f = dynamic_cast<const FuncSemType*>(&type)) {
        auto n = std::make_unique<FuncSemType>();
        for (auto& p : f->paramTypes)
            n->paramTypes.push_back(p ? substitute(*p, genericName, concrete) : nullptr);
        n->returnType = f->returnType ? substitute(*f->returnType, genericName, concrete) : nullptr;
        n->throws = f->throws;
        return n;
    }
    if (auto* r = dynamic_cast<const RecordSemType*>(&type)) {
        auto n = std::make_unique<RecordSemType>();
        // 泛型 record 的 canonicalName（如 "Pair<A, B>"）同步实例化：
        // 将形参名替换为绑定的具体 C++ 类型名（"Pair<int32_t, aura_rt::GcString*>"）
        n->canonicalName = replaceCanonicalArg(r->canonicalName, genericName, semTypeToCppName(concrete));
        for (auto& fld : r->fields) {
            n->fields.push_back({fld.name, fld.type ? substitute(*fld.type, genericName, concrete) : nullptr});
        }
        return n;
    }
    if (auto* u = dynamic_cast<const UnionSemType*>(&type)) {
        auto n = std::make_unique<UnionSemType>();
        for (auto& v : u->variants)
            n->variants.push_back(v ? substitute(*v, genericName, concrete) : nullptr);
        // P3c：泛型实例化二次检查——替换后无 GenericSemType 残留时，若任一变体
        // GC 不安全（如 T→string 后得 string | int）→ 报错（P0 对未实例化 Generic 放行）。
        bool hasGeneric = false;
        for (auto& v : n->variants)
            if (v && dynamic_cast<const GenericSemType*>(v.get())) { hasGeneric = true; break; }
        if (!hasGeneric) {
            for (auto& v : n->variants)
                if (v && unionVariantGcUnsafe(*v)) {
                    error(0, 0, "generic instantiation: union contains GC heap variant '" +
                          v->toString() + "' which is not GC-safe yet; "
                          "use Optional<T> for 'T | None'");
                    break;
                }
        }
        return n;
    }
    if (auto* l = dynamic_cast<const ListSemType*>(&type)) {
        auto n = std::make_unique<ListSemType>();
        n->elementType = l->elementType ? substitute(*l->elementType, genericName, concrete) : nullptr;
        return n;
    }
    return type.clone();
}

// ============================================================
// 调用参数检查辅助（inferCall / inferMethodCall 复用）
// ============================================================

void SemAnalyzer::checkThrowsContext(
    const ASTNode& callNode, const std::string& calleeName, bool calleeThrows) {
    if (!currentFunctionThrows_ && insideTry_ == 0 && calleeThrows) {
        error(callNode, DiagCode::E016_ThrowsViolation,
              "cannot call throwing function '" + calleeName + "' from non-throwing context",
              "add 'throws' to the function signature or wrap in 'try { ... } catch'");
    }
}

void SemAnalyzer::checkCallArgs(
    const ASTNode& callNode,
    const std::string& calleeName,
    const std::string& role,
    const std::vector<const SemType*>& formalTypes,
    const std::vector<std::unique_ptr<ASTNode>>& args,
    std::map<std::string, std::unique_ptr<SemType>>& genericMap,
    size_t defaultCount) {
    // 参数数量检查（支持默认参数：实参数量在 [min, total] 内合法）
    size_t total = formalTypes.size();
    size_t min   = total - defaultCount;
    if (args.size() < min || args.size() > total) {
        std::string expected = (min == total) ? std::to_string(total)
                                              : (std::to_string(min) + "~" + std::to_string(total));
        error(callNode, role + " '" + calleeName + "' expects " + expected +
              " arguments, got " + std::to_string(args.size()));
    }
    // 参数类型检查 + 泛型映射收集
    bool conflict = false;
    for (size_t i = 0; i < args.size() && i < formalTypes.size(); ++i) {
        auto argTy = inferExpr(*args[i]);
        if (formalTypes[i] && !isAssignable(*formalTypes[i], *argTy)) {
            error(*args[i], "argument type mismatch: expected '" +
                  formalTypes[i]->toString() + "', got '" + argTy->toString() + "'");
        }
        if (formalTypes[i])
            collectGenericMapping(*formalTypes[i], *argTy, genericMap, conflict);
    }
    // P2-2: 泛型绑定冲突从静默忽略改为报错
    if (conflict) {
        error(callNode, "conflicting type arguments for generic parameter(s) in call to '" + calleeName + "'");
    }
}

std::unique_ptr<SemType> SemAnalyzer::applyGenericMap(
    std::unique_ptr<SemType> result,
    const std::map<std::string, std::unique_ptr<SemType>>& genericMap) {
    for (auto& [name, concrete] : genericMap) {
        result = substitute(*result, name, *concrete);
    }
    return result;
}

// ============================================================
// collectGenericMapping — 递归匹配形参/实参，收集泛型→具体映射
// ============================================================

void SemAnalyzer::collectGenericMapping(
    const SemType& formal, const SemType& actual,
    std::map<std::string, std::unique_ptr<SemType>>& map,
    bool& conflict) const
{
    // case 1: formal 是泛型变量 <T> → actual 就是 T 的具体绑定
    if (auto* gf = dynamic_cast<const GenericSemType*>(&formal)) {
        auto it = map.find(gf->name);
        if (it != map.end()) {
            // 已绑定 → 检查一致性（同一个泛型变量被推导为不同类型则冲突）
            if (!isAssignable(*it->second, actual)) {
                conflict = true;  // 保留第一个绑定，调用方负责报错
            }
        } else {
            map[gf->name] = actual.clone();
        }
        return;
    }

    // case 2: formal 和 actual 都是 List → 递归匹配元素类型
    //         如 [T] vs [int] → T=int
    if (auto* lf = dynamic_cast<const ListSemType*>(&formal)) {
        if (auto* la = dynamic_cast<const ListSemType*>(&actual)) {
            if (lf->elementType && la->elementType)
                collectGenericMapping(*lf->elementType, *la->elementType, map, conflict);
        }
        return;
    }

    // case 3: formal 和 actual 都是函数类型 → 递归匹配参数和返回类型
    //         如 fun(T)→U vs fun(int)→int → T=int, U=int
    if (auto* ff = dynamic_cast<const FuncSemType*>(&formal)) {
        if (auto* fa = dynamic_cast<const FuncSemType*>(&actual)) {
            for (size_t i = 0; i < ff->paramTypes.size() && i < fa->paramTypes.size(); ++i) {
                if (ff->paramTypes[i] && fa->paramTypes[i])
                    collectGenericMapping(*ff->paramTypes[i], *fa->paramTypes[i], map, conflict);
            }
            if (ff->returnType && fa->returnType)
                collectGenericMapping(*ff->returnType, *fa->returnType, map, conflict);
        }
        return;
    }
}

// ============================================================
// sealSelfRefs：将 GenericSemType("Tree") → RecordSemType(canonicalName=fullName)
// ============================================================

void SemAnalyzer::sealSelfRefs(std::unique_ptr<SemType>& node,
                                const std::string& bareName,
                                const std::string& fullName) {
    // 自引用类型（如 Tree<T> 定义中的 children: [Tree<T>]）：标注 resolvedName，不展开
    if (auto* gs = dynamic_cast<GenericSemType*>(node.get())) {
        if (gs->name == bareName) {
            gs->resolvedName = fullName;
        }
        return;
    }
    // 复合类型遍历写入 resolvedName
    if (auto* r = dynamic_cast<RecordSemType*>(node.get())) {
        for (auto& f : r->fields)
            if (f.type) sealSelfRefs(f.type, bareName, fullName);
    } else if (auto* l = dynamic_cast<ListSemType*>(node.get())) {
        if (l->elementType) sealSelfRefs(l->elementType, bareName, fullName);
    } else if (auto* u = dynamic_cast<UnionSemType*>(node.get())) {
        for (auto& v : u->variants)
            if (v) sealSelfRefs(v, bareName, fullName);
    }
}

std::unique_ptr<SemType> SemAnalyzer::applyTypeArgs(
    std::unique_ptr<SemType> result,
    const Symbol& sym,
    const std::vector<std::unique_ptr<TypeExpr>>& typeArgs) {
    for (size_t i = 0; i < typeArgs.size() && i < sym.typeParams.size(); ++i) {
        auto concrete = resolveType(*typeArgs[i]);
        result = substitute(*result, sym.typeParams[i], *concrete);
    }
    return result;
}

void SemAnalyzer::materializeCanonicalName(
    std::unique_ptr<SemType>& result,
    const NamedType& n) {
    // RecordSemType 分支：用户自定义泛型类型（如 Tree<int>）
    if (auto* rec = dynamic_cast<RecordSemType*>(result.get())) {
        if (n.typeArgs.empty()) return;
        bool allConcrete = true;
        std::string fullName = rec->canonicalName + "<";
        for (size_t i = 0; i < n.typeArgs.size(); ++i) {
            if (i > 0) fullName += ", ";
            std::string auraName;
            if (auto* argNt = dynamic_cast<const NamedType*>(n.typeArgs[i].get()))
                auraName = argNt->name;
            else if (dynamic_cast<const GenericTypeRef*>(n.typeArgs[i].get())) {
                allConcrete = false; break;
            }
            fullName += cppNameOf(auraName);
        }
        fullName += ">";
        if (allConcrete) {
            rec->canonicalName = fullName;
            sealSelfRefs(result, n.name, fullName);
        }
        return;
    }

    // GenericSemType 分支：内置泛型类型（如 sync.Channel<int> / channel<int> / Iterator<T>）
    // 无 typeArgs 时无需处理；有 typeArgs 时设置 resolvedName 供后续提取元素类型
    auto* gs = dynamic_cast<GenericSemType*>(result.get());
    if (!gs || n.typeArgs.empty()) return;

    // P0.4 前缀统一：BuiltinRegistry 命中的内置泛型名（如 Iterator → "aura_rt::Iterator*"）
    // 用 cppType 去 * 作基名拼模板参数 → "aura_rt::Iterator<int32_t>"，
    // 与 range() 返回路径（semTypeFromBuiltinReturn Iterator 分支）保持一致；
    // 未命中的用户泛型名保持裸名
    std::string base = gs->name;
    if (auto* ti = BuiltinRegistry::get().findType(gs->name)) {
        base = ti->cppType;
        if (!base.empty() && base.back() == '*') base.pop_back();
    }
    std::string fullName = base + "<";
    for (size_t i = 0; i < n.typeArgs.size(); ++i) {
        if (i > 0) fullName += ", ";
        std::string auraName;
        if (auto* argNt = dynamic_cast<const NamedType*>(n.typeArgs[i].get())) {
            auraName = argNt->name;
        } else if (dynamic_cast<const GenericTypeRef*>(n.typeArgs[i].get())) {
            // 泛型形参（如 T），保留原样
            auraName = dynamic_cast<const GenericTypeRef*>(n.typeArgs[i].get())->name;
        }
        fullName += cppNameOf(auraName);
    }
    fullName += ">";
    gs->resolvedName = fullName;
}

// ============================================================
// canonicalName 传播（RecordSemType → 嵌套 RecordExpr AST）
// ============================================================

void SemAnalyzer::propagateCanonicalName(const ASTNode& expr, const SemType* type) {
    if (!type) return;

    // GenericSemType：通过符号表解析回具体类型（如 Tree → RecordSemType）
    if (auto* gs = dynamic_cast<const GenericSemType*>(type)) {
        // 已解析的自引用（如 Tree<int32_t>）：用模板体逐字段传播到嵌套 RecordExpr
        if (!gs->resolvedName.empty()) {
            if (auto* recExpr = dynamic_cast<const RecordExpr*>(&expr)) {
                auto* sym = symtab_.lookup(gs->name);
                if (sym && sym->kind == SymKind::TypeAlias && sym->type) {
                    auto resolved = sym->type->clone();
                    // 对模板副本做 sealSelfRefs，用 resolvedName 标注所有自引用
                    sealSelfRefs(resolved, gs->name, gs->resolvedName);
                    if (auto* rs = dynamic_cast<RecordSemType*>(resolved.get())) {
                        rs->canonicalName = gs->resolvedName;
                        const_cast<RecordExpr*>(recExpr)->inferredType = rs;
                        for (auto& f : recExpr->fields) {
                            for (auto& ft : rs->fields) {
                                if (f.name == ft.name && f.value && ft.type) {
                                    propagateCanonicalName(*f.value, ft.type.get());
                                    break;
                                }
                            }
                        }
                        typeStore_.push_back(std::move(resolved));
                        return;
                    }
                }
            }
            const_cast<ASTNode&>(expr).inferredType = type;
            return;
        }
        auto* sym = symtab_.lookup(gs->name);
        if (sym && sym->kind == SymKind::TypeAlias && sym->type) {
            propagateCanonicalName(expr, sym->type.get());
            return;
        }
        const_cast<ASTNode&>(expr).inferredType = type;
        return;
    }

    // UnionSemType：试每个变体，取第一个形态可匹配的（如 children: [Tree<T>] | T）
    // 只传播与表达式形态匹配的变体：RecordExpr 匹配 record/泛型变体，ListExpr 匹配 list 变体；
    // 其他表达式（int/string 字面量等）不改写 inferredType——否则 int | [int] 中 [1,2]
    // 会被错误标注为 int 变体，导致 CodeGen 隐式装箱选错变体索引
    if (auto* us = dynamic_cast<const UnionSemType*>(type)) {
        for (auto& v : us->variants) {
            if (!v) continue;
            bool shapeMatch =
                (dynamic_cast<const RecordExpr*>(&expr) &&
                 (dynamic_cast<const RecordSemType*>(v.get()) ||
                  dynamic_cast<const GenericSemType*>(v.get())))
                || (dynamic_cast<const ListExpr*>(&expr) &&
                    dynamic_cast<const ListSemType*>(v.get()));
            if (shapeMatch) { propagateCanonicalName(expr, v.get()); return; }
        }
        return;
    }

    // RecordExpr 匹配 RecordSemType ↔ 标注 + 传播到字段
    if (auto* recExpr = dynamic_cast<const RecordExpr*>(&expr)) {
        if (auto* rs = dynamic_cast<const RecordSemType*>(type)) {
            if (!rs->canonicalName.empty())
                const_cast<RecordExpr*>(recExpr)->inferredType = type;
            for (auto& f : recExpr->fields) {
                for (auto& ft : rs->fields) {
                    if (f.name == ft.name && f.value) {
                        if (ft.type)
                            propagateCanonicalName(*f.value, ft.type.get());
                        break;
                    }
                }
            }
        }
        return;
    }

    // ListExpr 匹配 ListSemType ↔ 传播到每个元素
    if (auto* listExpr = dynamic_cast<const ListExpr*>(&expr)) {
        const_cast<ListExpr*>(listExpr)->inferredType = type;
        if (auto* ls = dynamic_cast<const ListSemType*>(type)) {
            for (auto& elem : listExpr->elements) {
                if (elem) propagateCanonicalName(*elem, ls->elementType.get());
            }
        }
        return;
    }

    // 叶节点：仅标注 inferredType（Identifier 除外——其类型来自符号表，
    // 若被声明类型改写会丢失 record 具体类型；CodeGen 接口视图绑定
    // （genLetStmt §3.10）依赖 initializer 保持 RecordSemType 以识别 record 指针）
    if (dynamic_cast<const Identifier*>(&expr)) return;
    const_cast<ASTNode&>(expr).inferredType = type;
}

// ============================================================
// 调度
// ============================================================

void SemAnalyzer::checkProgram(const Program& program) {
    for (auto& d : program.decls) {
        if (d) checkDecl(*d);
    }
}

void SemAnalyzer::checkDecl(const Decl& decl) {
    if (auto* cfg = dynamic_cast<const ConfigDecl*>(&decl)) {
        if (cfg->ns == "io" && cfg->key == "sync") {
            ioSync_ = (cfg->value == "true");
        }
        return;
    }
    if (auto* f = dynamic_cast<const FunDecl*>(&decl)) {
        checkFunBody(*f);
    } else if (auto* m = dynamic_cast<const MethodDecl*>(&decl)) {
        checkMethodBody(*m);
    } else if (auto* i = dynamic_cast<const InterfaceDecl*>(&decl)) {
        // 接口默认方法体检查（v1）：self 类型 = 接口自身（InterfaceSemType），
        // 方法签名由接口方法集提供；返回类型与声明一致
        for (auto& m : i->methods) {
            if (!m.defaultBody) continue;
            loopDepth_ = 0;
            symtab_.enterScope(ScopeKind::Function);
            // 泛型参数注册（方法签名可能引用 T，如 cmp(other: T)）
            for (auto& tp : i->typeParams) {
                Symbol tpSym;
                tpSym.kind = SymKind::GenericParam;
                tpSym.name = tp;
                symtab_.define(std::move(tpSym));
            }
            {
                Symbol sym;
                sym.kind = SymKind::Parameter;
                sym.name = "self";
                sym.type = resolveNamedType(i->name);
                symtab_.define(std::move(sym));
            }
            for (auto& p : m.params) {
                Symbol sym;
                sym.kind = SymKind::Parameter;
                sym.name = p.name;
                sym.type = p.type ? resolveType(*p.type) : ErrorSemType::make();
                symtab_.define(std::move(sym));
            }
            auto retType = m.returnType ? resolveType(*m.returnType) : nullptr;
            FnCtxGuard fc(*this, retType ? retType->clone() : nullptr, m.throws);
            checkBlock(*m.defaultBody);
            symtab_.exitScope();
        }
        return;
    }
    // TypeDecl / ImportDecl 不需要体检查
}

void SemAnalyzer::checkStmt(const Stmt& stmt) {
    if (auto* b = dynamic_cast<const BlockStmt*>(&stmt))        { checkBlock(*b);       return; }
    if (auto* l = dynamic_cast<const LetDecl*>(&stmt))          { checkLetDecl(*l);     return; }
    if (auto* c = dynamic_cast<const ConstDecl*>(&stmt))        { checkConstDecl(*c);   return; }
    if (auto* r = dynamic_cast<const ReturnStmt*>(&stmt))       { checkReturnStmt(*r);  return; }
    if (auto* t = dynamic_cast<const ThrowStmt*>(&stmt))        { checkThrowStmt(*t);   return; }
    if (auto* i = dynamic_cast<const IfStmt*>(&stmt))           { checkIfStmt(*i);      return; }
    if (auto* w = dynamic_cast<const WhileStmt*>(&stmt))        { checkWhileStmt(*w);   return; }
    if (auto* f = dynamic_cast<const ForStmt*>(&stmt))          { checkForStmt(*f);     return; }
    if (auto* o = dynamic_cast<const LoopStmt*>(&stmt))         { checkLoopStmt(*o);    return; }
    if (auto* m = dynamic_cast<const MatchStmt*>(&stmt))        { checkMatchStmt(*m);   return; }
    if (auto* t = dynamic_cast<const TryCatchStmt*>(&stmt))     { checkTryCatchStmt(*t);return; }
    if (auto* s = dynamic_cast<const SyncStmt*>(&stmt))         { checkSyncStmt(*s);    return; }
    if (auto* sf = dynamic_cast<const SyncForStmt*>(&stmt))     { checkSyncForStmt(*sf);return; }
    if (auto* p = dynamic_cast<const SpawnStmt*>(&stmt))        { checkSpawnStmt(*p);   return; }
    if (auto* l = dynamic_cast<const LockStmt*>(&stmt))         { checkLockStmt(*l);    return; }
    if (auto* e = dynamic_cast<const ExprStmt*>(&stmt))         { checkExprStmt(*e);    return; }
    if (auto* br = dynamic_cast<const BreakStmt*>(&stmt)) {
        if (loopDepth_ == 0) error(*br, "'break' outside of loop");
        if (inLockBlock_) error(*br, "cannot break out of lock block");
        if (!syncBoundaryStack_.empty()
            && loopDepth_ <= syncBoundaryStack_.back().loopDepthAtEntry)
            error(*br, "cannot break out of " + syncBoundaryStack_.back().kind + " block");
        return;
    }
    if (auto* co = dynamic_cast<const ContinueStmt*>(&stmt)) {
        if (loopDepth_ == 0) error(*co, "'continue' outside of loop");
        if (inLockBlock_) error(*co, "cannot continue out of lock block");
        if (!syncBoundaryStack_.empty()
            && loopDepth_ <= syncBoundaryStack_.back().loopDepthAtEntry)
            error(*co, "cannot continue out of " + syncBoundaryStack_.back().kind + " block");
        return;
    }
}

// ============================================================
// 跨模块导入/导出（Phase A）
// ============================================================

// 导入一个导出函数/构造函数为 Function 符号（importExports 辅助）
void SemAnalyzer::importFuncSymbol(const std::string& name, const FuncExport& f,
                                    const std::string& alias) {
    Symbol sym;
    sym.kind = SymKind::Function;
    sym.name = name;
    for (auto& p : f.params) {
        SymParam sp;
        sp.name = p.name;
        sp.type = p.type ? p.type->clone() : nullptr;
        if (sp.type) qualifyRecordTypes(sp.type, alias);
        if (p.defaultExpr) { sp.defaultExpr = p.defaultExpr->clone(); sp.hasDefault = true; }
        sym.params.push_back(std::move(sp));
    }
    sym.type   = f.returnType ? f.returnType->clone() : nullptr;
    if (sym.type) qualifyRecordTypes(sym.type, alias);
    sym.throws = f.throws;
    symtab_.defineGlobal(std::move(sym));
}

void SemAnalyzer::importExports(const std::string& alias, const ModuleExports& exports) {
    for (auto& [name, type] : exports.types) {
        Symbol sym;
        sym.kind = SymKind::TypeAlias;
        sym.name = alias.empty() ? name : (alias + "." + name);
        sym.type = type->clone();
        if (sym.type) qualifyRecordTypes(sym.type, alias);
        symtab_.defineGlobal(std::move(sym));
    }
    auto qualified = [&](const std::string& name) {
        return alias.empty() ? name : (alias + "." + name);
    };
    for (auto& [name, f] : exports.ctors) importFuncSymbol(qualified(name), f, alias);
    for (auto& [name, f] : exports.funcs) importFuncSymbol(qualified(name), f, alias);
    // 注册 import 别名本身（供 inferMethodCall 检测命名空间调用）
    if (!alias.empty()) {
        Symbol aliasSym;
        aliasSym.kind = SymKind::Variable;
        aliasSym.name = alias;
        aliasSym.belongsToModule = alias;
        aliasSym.type = ErrorSemType::make();
        symtab_.defineGlobal(std::move(aliasSym));
    }
}

// 构建 FuncExport（extractExports 辅助）：params 深拷贝 + 返回类型 + throws
static FuncExport buildFuncExport(const std::vector<SymParam>& params,
                                  const SemType* returnType, bool throws) {
    FuncExport fe;
    for (auto& p : params) {
        SymParam sp;
        sp.name = p.name;
        sp.type = p.type ? p.type->clone() : nullptr;
        if (p.defaultExpr) { sp.defaultExpr = p.defaultExpr->clone(); sp.hasDefault = true; }
        fe.params.push_back(std::move(sp));
    }
    fe.returnType = returnType ? returnType->clone() : ErrorSemType::make();
    fe.throws     = throws;
    return fe;
}

ModuleExports SemAnalyzer::extractExports() const {
    ModuleExports e;
    for (auto& scope : symtab_.allScopes()) {
        if (scope->kind() != ScopeKind::Global) continue;
        scope->forEach([&](const std::string& name, const Symbol& sym) {
            if (sym.isImported) return;               // import 不透传（C6-1）
            if (hasAnyPub_ && !sym.isPublic) return;  // 模块级策略：有 pub 仅导出带 pub 的（C6-2）
            switch (sym.kind) {
                case SymKind::TypeAlias:
                    e.types[name] = sym.type ? sym.type->clone() : ErrorSemType::make();
                    // ctorDeclared：无参构造函数 ctorParams 为空，不能用 !empty() 判断
                    if (sym.ctorDeclared) {
                        const SemType* ctorRet = sym.ctorReturnType ? sym.ctorReturnType.get()
                                                 : sym.type.get();
                        e.ctors[name] = buildFuncExport(sym.ctorParams, ctorRet, sym.throws);
                    }
                    break;
                case SymKind::Function:
                    e.funcs[name] = buildFuncExport(sym.params, sym.type.get(), sym.throws);
                    break;
                default: break;
            }
        });
        break;
    }
    return e;
}

} // namespace Aura