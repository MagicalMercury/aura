#include "SemAnalyzer.h"
#include "BuiltinRegistry.h"
#include <algorithm>

namespace Aura {

namespace {
// 将 Aura 类型名映射为 C++ 类型名（未注册的类型保持原名）
std::string cppNameOf(const std::string& auraName) {
    if (auto* ti = BuiltinRegistry::get().findType(auraName))
        return ti->cppType;
    return auraName;
}

// 提取 "Prefix<Inner...>" 的 Inner（嵌套尖括号用末个 '>' 定位；尾部 '*' 不影响）。
// 如 "aura_rt::Array<aura_rt::Array<int32_t>*>*" → "aura_rt::Array<int32_t>*"
std::string templateInner(const std::string& s) {
    auto lt = s.find('<');
    auto rt = s.rfind('>');
    if (lt == std::string::npos || rt == std::string::npos || rt < lt)
        return "";
    return s.substr(lt + 1, rt - lt - 1);
}

// 按顶层逗号分割（尖括号/圆括号深度 0）并去空白；解析 std::function<R(A1, A2, ...)>
// 的参数列表时使用（参数可含嵌套模板如 aura_rt::Array<int32_t>*）
std::vector<std::string> splitTopLevelArgs(const std::string& s) {
    std::vector<std::string> parts;
    std::string cur;
    int depth = 0;
    for (char c : s) {
        if (c == '<' || c == '(') ++depth;
        else if (c == '>' || c == ')') --depth;
        else if (c == ',' && depth == 0) {
            parts.push_back(cur);
            cur.clear();
            continue;
        }
        cur += c;
    }
    parts.push_back(cur);
    for (auto& p : parts) {
        auto b = p.find_first_not_of(" \t");
        auto e = p.find_last_not_of(" \t");
        p = (b == std::string::npos) ? "" : p.substr(b, e - b + 1);
    }
    return parts;
}

// 在类型树中把名为 name 的未解析 GenericSemType（泛型形参）替换为 concrete。
// 供 instantiateUserRecordFromCppName 展开用户泛型 record 定义时替换形参（仿
// substitute 的结构递归，但不触发 Union 变体 GC 检查 / 不改写 canonicalName——
// canonicalName 由调用方统一设为实例化后的完整名）。仅结构替换，无副作用。
std::unique_ptr<SemType> replaceGenericRef(const SemType& type,
                                           const std::string& name,
                                           const SemType& concrete) {
    if (auto* g = dynamic_cast<const GenericSemType*>(&type)) {
        if (g->name == name) return concrete.clone();
        return type.clone();
    }
    if (auto* f = dynamic_cast<const FuncSemType*>(&type)) {
        auto n = std::make_unique<FuncSemType>();
        for (auto& p : f->paramTypes)
            n->paramTypes.push_back(p ? replaceGenericRef(*p, name, concrete) : nullptr);
        n->returnType = f->returnType ? replaceGenericRef(*f->returnType, name, concrete) : nullptr;
        n->throws = f->throws;
        return n;
    }
    if (auto* r = dynamic_cast<const RecordSemType*>(&type)) {
        auto n = std::make_unique<RecordSemType>();
        n->isTuple = r->isTuple;
        n->canonicalName = r->canonicalName;
        for (auto& fld : r->fields)
            n->fields.push_back({fld.name,
                fld.type ? replaceGenericRef(*fld.type, name, concrete) : nullptr});
        return n;
    }
    if (auto* u = dynamic_cast<const UnionSemType*>(&type)) {
        auto n = std::make_unique<UnionSemType>();
        for (auto& v : u->variants)
            n->variants.push_back(v ? replaceGenericRef(*v, name, concrete) : nullptr);
        return n;
    }
    if (auto* l = dynamic_cast<const ListSemType*>(&type)) {
        auto n = std::make_unique<ListSemType>();
        n->elementType = l->elementType ? replaceGenericRef(*l->elementType, name, concrete) : nullptr;
        return n;
    }
    if (auto* o = dynamic_cast<const OptionalSemType*>(&type)) {
        return OptionalSemType::make(o->elementType
            ? replaceGenericRef(*o->elementType, name, concrete) : nullptr);
    }
    return type.clone();
}
} // namespace

std::unique_ptr<SemType> SemAnalyzer::semTypeFromAuraName(const std::string& name) const {
    // feature-06（阶段 C）：裸 Callable 标注 → CallableSemType（erased 契约——标注即
    // 擦除，origins 由赋值/传播点维护）。先于 BuiltinRegistry 通用 GenericSemType
    // 占位路径（registry 条目仍用于 CodeGen mapNamedType 输出 C++ 名 CallableErased*）。
    if (name == "Callable") return std::make_unique<CallableSemType>();
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

std::unique_ptr<SemType> SemAnalyzer::semTypeFromCppName(const std::string& cppName) const {
    // 反向映射：C++ 类型名 → Aura 类型名 → SemType（如 "int32_t" → intType）
    if (auto* ti = BuiltinRegistry::get().findByCppType(cppName)) {
        auto t = semTypeFromAuraName(ti->name);
        if (!dynamic_cast<const ErrorSemType*>(t.get()))
            return t;
    }

    // 语义化还原（unwrap 消费方向元素推断，2026-08-26）：
    // 原实现仅基础类型可还原，其余一律 GenericSemType{name=原始 C++ 名}（有损），
    // 使显式 Optional<接口/list/显式 Iterator/std::function/record> 的 unwrap 元素
    // 类型错误（CodeGen 多补 '*' / Sema isAssignable、typeKey 不命中）。逐形态还原：
    // 1) aura_rt::Array<X>* → ListSemType{ X }（元素递归还原，如 Optional<[int]> 的 unwrap）
    if (cppName.rfind("aura_rt::Array<", 0) == 0
        && !cppName.empty() && cppName.back() == '*') {
        auto lt = std::make_unique<ListSemType>();
        lt->elementType = semTypeFromCppName(templateInner(cppName));
        return lt;
    }
    // 2) aura_rt::Iterator<X> → GenericSemType{name=="Iterator"}（触发既有 Iterator 特判）
    if (cppName.rfind("aura_rt::Iterator<", 0) == 0
        && !cppName.empty() && cppName.back() == '>') {
        auto g = std::make_unique<GenericSemType>();
        g->name = "Iterator";
        g->resolvedName = cppName;
        return g;
    }
    // 3) aura_rt::Optional<X> / aura_rt::Optional<X>* → GenericSemType{name=="Optional"}
    //    （修嵌套 unwrap：元素仍是 Optional 时再 .unwrap() 可命中 Optional 方法表）
    if (cppName.rfind("aura_rt::Optional<", 0) == 0) {
        auto g = std::make_unique<GenericSemType>();
        g->name = "Optional";
        g->resolvedName = cppName;
        return g;
    }
    // 4) std::function<R(A1, A2, ...)> → FuncSemType（函数值是值类型，非指针）
    if (cppName.rfind("std::function<", 0) == 0
        && !cppName.empty() && cppName.back() == '>') {
        std::string inner = templateInner(cppName);   // "R(A1, A2, ...)"
        auto lp = inner.find('(');
        auto rp = inner.rfind(')');
        if (lp != std::string::npos && rp != std::string::npos && rp > lp) {
            auto f = std::make_unique<FuncSemType>();
            std::string ret = inner.substr(0, lp);
            auto rb = ret.find_first_not_of(" \t");
            auto re = ret.find_last_not_of(" \t");
            ret = (rb == std::string::npos) ? "" : ret.substr(rb, re - rb + 1);
            if (!ret.empty() && ret != "void")
                f->returnType = semTypeFromCppName(ret);
            std::string params = inner.substr(lp + 1, rp - lp - 1);
            if (!params.empty()) {
                for (auto& p : splitTopLevelArgs(params)) {
                    if (p.empty() || p == "void") continue;   // void() = 无参数
                    f->paramTypes.push_back(semTypeFromCppName(p));
                }
            }
            return f;
        }
        // 解析失败：落入下方 GenericSemType 占位（保持原行为）
    }

    // 5) 裸 C++ 名（可能带 record 指针 '*' 后缀）：经符号表解析用户类型
    //    （接口→InterfaceSemType、类型别名/record→类型 clone、泛型参数→GenericSemType）
    std::string bare = cppName;
    if (!bare.empty() && bare.back() == '*') bare.pop_back();
    if (!bare.empty()) {
        // 6a) 内置接口 C++ 名反解（bug-85 B3 配套，2026-09-18）：
        //     B3 将 Stringer/Comparable 的 C++ 形态从产物裸名迁入 runtime
        //     公共头（带 aura_rt:: 命名空间）。反解方向若不同步，elemTypeOf 会将
        //     "aura_rt::Stringer" 误判为「未知 record 占位」GenericSemType →
        //     Assignability 的 tIsIface 为 false → 「匿名 record → Optional<视图>」
        //     本应报 type mismatch 的形态被放行（SemaOptional.*AnonRecord
        //     ToOptionalViewCleanError 群）。Comparable<T> 带实参：基名拿到 Interface
        //     SemType 后再递归反解 <...> 内实参（与 CodeGen mapSemType 的
        //     InterfaceSemType 分支同源）。内置 Iterator 走上方方案 2
        //     （GenericSemType{name=="Iterator"}），不在此列。
        {
            static const char* kBuiltinIfaces[] = { "Stringer", "Comparable" };
            for (const char* ifn : kBuiltinIfaces) {
                std::string pfx = std::string("aura_rt::") + ifn;
                if (bare.rfind(pfx, 0) != 0) continue;
                if (bare.size() != pfx.size() && bare[pfx.size()] != '<') continue;
                auto* isym = symtab_.lookup(ifn);
                if (!isym || isym->kind != SymKind::Interface) continue;
                auto t = std::make_unique<InterfaceSemType>();
                t->name = ifn;
                // ⚠️ bug-85 修复（2026-09-18）：必须填 methods（同本文件 L421-430
                // 的接口分支）。只设 name 会让 InterfaceSemType::methods 为空 →
                // 接口方法查找失败 → 误报 "interface 'Stringer' has no method
                // 'to_string'"（实测：DoubleBoxNestedOptionalView /
                // NoAnnotLetOptionalValueElemsStillOk 两例编译报错）。
                for (auto& m : isym->interfaceMethods) {
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
                auto lt = bare.find('<');
                auto rt = bare.rfind('>');
                if (lt != std::string::npos && rt != std::string::npos && rt > lt)
                    for (auto& a : splitTopLevelArgs(bare.substr(lt + 1, rt - lt - 1)))
                        t->typeArgs.push_back(semTypeFromCppName(a));
                return t;
            }
        }
        auto named = resolveNamedType(bare);
        if (!dynamic_cast<const ErrorSemType*>(named.get()))
            return named;
        // 6) 用户泛型 record 的 C++ 名（如 "Tree<int32_t>*"）：符号表键是基名 "Tree"，
        //    resolveNamedType("Tree<int32_t>") 查不到 → 反解基名 + 提取 <...> 实参 +
        //    替换形参实例化 → 返回实例化 RecordSemType（而非 GenericSemType 占位）。
        //    Optional<Tree<int>> = some({..}) 的 children 期望传播依赖此反解
        //    （problem.txt「Optional<用户泛型 record> = some({..})」条目）。
        if (auto inst = instantiateUserRecordFromCppName(bare))
            return inst;
    }

    // 未注册的 C++ 类型（record 指针 / Array<T>* 等）：作为堆对象占位
    auto g = std::make_unique<GenericSemType>();
    g->name = cppName;
    g->resolvedName = cppName;
    return g;
}

std::unique_ptr<RecordSemType> SemAnalyzer::instantiateUserRecordFromCppName(
    const std::string& rawName) const {
    std::string name = rawName;
    if (!name.empty() && name.back() == '*') name.pop_back();   // 剥 C++ 指针后缀
    if (name.empty()) return nullptr;
    auto lt = name.find('<');
    bool hasArgs = lt != std::string::npos;
    std::string base = hasArgs ? name.substr(0, lt) : name;      // 基名（符号表键）
    // 仅用户 TypeAlias→RecordSemType 可反解（内置泛型如 Optional/Iterator/channel 的
    // resolvedName 走 isAssignable 既有分支；接口符号返回 nullptr → 保持旧放行）
    auto* sym = symtab_.lookup(base);
    if (!sym || sym->kind != SymKind::TypeAlias || !sym->type) return nullptr;
    auto* rec = dynamic_cast<const RecordSemType*>(sym->type.get());
    if (!rec) return nullptr;
    // clone 原始定义后按 typeParams 位置替换形参（仿 applyTypeArgs；不触发 substitute
    // 的 Union GC 检查——record 定义内 `T | None` 已在 resolveType 折叠为 Optional<T>）
    auto inst = sym->type->clone();
    if (hasArgs && !sym->typeParams.empty()) {
        auto args = splitTopLevelArgs(templateInner(name));
        for (size_t i = 0; i < args.size() && i < sym->typeParams.size(); ++i) {
            if (args[i].empty()) continue;
            auto concrete = semTypeFromCppName(args[i]);   // 嵌套用户 record 递归反解
            if (dynamic_cast<const ErrorSemType*>(concrete.get())) continue;
            inst = replaceGenericRef(*inst, sym->typeParams[i], *concrete);
        }
    }
    auto* rinst = dynamic_cast<RecordSemType*>(inst.get());
    if (!rinst) return nullptr;
    // 实例化后的完整名（如 "Tree<int32_t>" / "Node"）——CodeGen / canonicalName 传播依据
    rinst->canonicalName = name;
    // sealSelfRefs：展开副本的 children 元素也标 resolvedName（如 "Tree<int32_t>"），
    // 深层递归 isAssignable 再走 L720 展开——否则 resolvedName 空落回 L662 多层漏检
    sealSelfRefs(inst, base, name);
    return std::unique_ptr<RecordSemType>(
        dynamic_cast<RecordSemType*>(inst.release()));
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
    // feature-06（阶段 C）：CallableSemType → aura_rt::CallableErased*（与 CodeGen
    // mapSemType 分支一致；供嵌套容器 C++ 名拼接（如 [Callable] 列表元素/实参名））
    if (dynamic_cast<const CallableSemType*>(&t))
        return "aura_rt::CallableErased*";
    if (auto* l = dynamic_cast<const ListSemType*>(&t))
        return "aura_rt::Array<" + semTypeToCppName(*l->elementType) + ">*";
    // 函数值（std::function 值类型，无尾 *）：与 CodeGen mapSemType 的 FuncSemType
    // 分支一致。bug-17 引入 semTypeToCppName 作为 materializeCanonicalName 的实参
    // 拼接函数后，函数实参（Optional<fun(int) -> int> 的 fun(int) -> int）必须返回
    // std::function 名而非兜底 "auto"。
    if (auto* f = dynamic_cast<const FuncSemType*>(&t)) {
        std::string sig = "std::function<";
        sig += f->returnType ? semTypeToCppName(*f->returnType) : "void";
        sig += "(";
        for (size_t i = 0; i < f->paramTypes.size(); ++i) {
            if (i > 0) sig += ", ";
            sig += f->paramTypes[i] ? semTypeToCppName(*f->paramTypes[i]) : "auto";
        }
        sig += ")>";
        return sig;
    }
    if (auto* r = dynamic_cast<const RecordSemType*>(&t))
        return r->canonicalName + "*";
    // 接口视图（值类型，无尾 *）：与 CodeGen mapSemType 的 InterfaceSemType 分支一致。
    // bug-17 引入 semTypeToCppName 作为 materializeCanonicalName 的实参拼接函数后，
    // 接口实参（Optional<Stringer> 的 Stringer）必须返回视图名而非兜底 "auto"。
    if (auto* is = dynamic_cast<const InterfaceSemType*>(&t)) {
        // bug-85（方案 B3）：内置接口 Stringer/Comparable 的 C++ 形态移入
        // runtime 公共头（builtin/interfaces.h）→ 带 aura_rt:: 命名空间，与
        // CodeGen mapNamedType / mapSemType 的映射保持一致（不一致会让适配器
        // 模板实参与视图类型名失配）。
        // 注：Iterator 不在本处特判（其 C++ 名由调用方直接拼 "aura_rt::Iterator<...>"，
        // 见 CallInfer.cpp；本函数对 Iterator 维持原裸名行为，避免波及既有路径）。
        std::string baseName = is->name;
        if (baseName == "Stringer" || baseName == "Comparable")
            baseName = "aura_rt::" + baseName;
        if (is->typeArgs.empty()) return baseName;
        std::string result = baseName + "<";
        for (size_t i = 0; i < is->typeArgs.size(); ++i) {
            if (i > 0) result += ", ";
            result += is->typeArgs[i] ? semTypeToCppName(*is->typeArgs[i]) : "void";
        }
        return result + ">";
    }
    if (auto* g = dynamic_cast<const GenericSemType*>(&t)) {
        if (!g->resolvedName.empty()) {
            // bug-17：resolvedName（如 "aura_rt::Optional<int32_t>"）是堆泛型去尾 * 的
            // 基型。作为类型实参嵌入 canonicalName（Box2<Optional<int>> → 完整 C++
            // 名须为 Box2<aura_rt::Optional<int32_t>*>）时须补回堆指针 *，与声明侧
            // mapType（aura_rt::Optional<int32_t>*）及 CodeGen mapSemType
            // （TypeMap.cpp:527-531 finalizeCppElem + 尾 * 补全）一致。值视图
            // （Iterator，aura_rt::Iterator<...> 无尾 *）与已带 * 的 resolvedName 不追加。
            if (g->resolvedName.rfind("aura_rt::Iterator<", 0) == 0
                || (!g->resolvedName.empty() && g->resolvedName.back() == '*'))
                return g->resolvedName;
            return g->resolvedName + "*";
        }
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

std::unique_ptr<SemType> SemAnalyzer::elemTypeOf(const SemType* iterType) const {
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

std::vector<std::unique_ptr<SemType>>
SemAnalyzer::extractTypeArgsFromCanonicalName(const std::string& canonicalName) const {
    std::vector<std::unique_ptr<SemType>> result;
    auto lt = canonicalName.find('<');
    auto rt = canonicalName.rfind('>');
    if (lt == std::string::npos || rt == std::string::npos || rt < lt)
        return result;
    // 按顶层逗号分割（splitTopLevelArgs 支持嵌套如 "Pair<Stack<int>, B>"），
    // 每个实参经 semTypeFromCppName 还原为 SemType（"int32_t"→int、record 指针→RecordSemType）
    for (auto& arg : splitTopLevelArgs(canonicalName.substr(lt + 1, rt - lt - 1)))
        result.push_back(semTypeFromCppName(arg));
    return result;
}

std::unique_ptr<SemType> SemAnalyzer::resolveNamedType(const std::string& name) const {
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

// ============================================================
// feature-06（阶段 C）：origins 溯源签名集辅助
// ============================================================
std::vector<std::shared_ptr<const FuncSemType>>
SemAnalyzer::originsOf(const SemType& t) {
    std::vector<std::shared_ptr<const FuncSemType>> r;
    if (auto* f = dynamic_cast<const FuncSemType*>(&t)) {
        // 函数签名值 → 单元素 origins（clone 自持——签名可能是临时推断类型）
        r.push_back(std::shared_ptr<const FuncSemType>(
            static_cast<const FuncSemType*>(f->clone().release())));
        return r;
    }
    if (auto* c = dynamic_cast<const CallableSemType*>(&t)) {
        r = c->origins;   // shared_ptr 拷贝（共享不可变签名）
        return r;
    }
    return r;   // 其余类型 → 空（非可调用值）
}

void SemAnalyzer::joinOrigins(
    std::vector<std::shared_ptr<const FuncSemType>>& dst,
    const std::vector<std::shared_ptr<const FuncSemType>>& src) {
    for (auto& o : src) {
        if (!o) continue;
        bool dup = false;
        for (auto& d : dst)
            if (d && d->equals(*o)) { dup = true; break; }
        if (!dup) dst.push_back(o);
    }
}

const InterfaceSemType::MethodSig* SemAnalyzer::findRecordMethod(
    const std::string& canonicalName, const std::string& methodName) const {
    auto findIn = [&](const std::map<std::string,
                     std::vector<InterfaceSemType::MethodSig>>& tbl,
                     const std::string& key)
        -> const std::vector<InterfaceSemType::MethodSig>* {
        auto it = tbl.find(key);
        return it != tbl.end() ? &it->second : nullptr;
    };
    // canonicalName 先全名（typeMethods_ 本模块 / importedMethods_ 跨模块导入），
    // 泛型 record 物化实例名（如 "Stack<int32_t>"）按基名回退查找
    const std::vector<InterfaceSemType::MethodSig>* methods =
        findIn(typeMethods_, canonicalName);
    if (!methods) methods = findIn(importedMethods_, canonicalName);
    if (!methods) {
        auto lt = canonicalName.find('<');
        if (lt != std::string::npos) {
            std::string base = canonicalName.substr(0, lt);
            methods = findIn(typeMethods_, base);
            if (!methods) methods = findIn(importedMethods_, base);
        }
    }
    if (!methods) return nullptr;
    for (auto& m : *methods)
        if (m.name == methodName) return &m;
    return nullptr;
}

std::vector<std::shared_ptr<const FuncSemType>>
SemAnalyzer::callableOriginsFromType(const SemType& initTy) const {
    // 函数签名值 → 单签名（未绑定泛型无法静态生成 C++ 包装 → 剔除落 erased）
    if (auto* f = dynamic_cast<const FuncSemType*>(&initTy)) {
        if (containsUnboundGenericParam(f)) return {};
        return originsOf(initTy);
    }
    // Callable 值拷贝 → 沿用其 origins（erased 保持 erased）
    if (auto* c = dynamic_cast<const CallableSemType*>(&initTy)) {
        if (c->erased()) return {};
        std::vector<std::shared_ptr<const FuncSemType>> r;
        for (auto& o : c->origins)
            if (o && !containsUnboundGenericParam(o.get())) r.push_back(o);
        return r;
    }
    // functor record → invoke 方法签名（Assignability (d) 放行的可赋形态）
    if (auto* rs = dynamic_cast<const RecordSemType*>(&initTy)) {
        if (auto* m = findRecordMethod(rs->canonicalName, "invoke")) {
            auto fs = std::make_shared<FuncSemType>();
            for (auto& pt : m->paramTypes)
                fs->paramTypes.push_back(pt ? pt->clone() : nullptr);
            fs->returnType = m->returnType ? m->returnType->clone() : NoneSemType::make();
            fs->throws = m->throws;
            if (containsUnboundGenericParam(fs.get())) return {};   // 泛型 functor v1 不支持
            std::vector<std::shared_ptr<const FuncSemType>> r;
            r.push_back(std::move(fs));
            return r;
        }
        return {};
    }
    return {};
}

} // namespace Aura
