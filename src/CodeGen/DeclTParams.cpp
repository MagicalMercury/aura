#include "CodeGen.h"
#include "../Sema/BuiltinRegistry.h"
#include <set>
#include <sstream>

namespace Aura {

// ============================================================
// 泛型模板参数收集
// ============================================================

void CodeGenerator::collectTParams(const TypeExpr& type, std::set<std::string>& out) const {
    if (auto* g = dynamic_cast<const GenericTypeRef*>(&type)) {
        out.insert(g->name);
        return;
    }
    if (auto* n = dynamic_cast<const NamedType*>(&type)) {
        // 先递归收集 typeArgs 中的泛型变量（这些变量会原样出现在 C++ 形参类型中，
        // 需作为函数/方法模板参数；如 Transform<T> 的 T、Pair<A,B> 的 A/B）。先收集
        // 到独立集合 argGen 供下方「typeArgs 是否含泛型」判定复用（含嵌套，如
        // Transform<[T]> 的 argGen={T}）。
        std::set<std::string> argGen;
        for (auto& a : n->typeArgs)
            if (a) collectTParams(*a, argGen);
        for (auto& g : argGen) out.insert(g);
        // 若该名称是模板类型别名，添加其模板参数——但仅当 typeArgs 含泛型变量且该
        // 类型非已实例化 record（恒堆）时：
        // - Transform<int>（typeArgs 全具体）→ C++ 形参 std::function<int(int)> 不含
        //   T，收集 T 使函数/方法模板化但 T 不在形参 → 调用点无法推导（g++ no
        //   matching / couldn't deduce template parameter）；
        // - Transform<T>（typeArgs 含泛型 T）→ C++ 形参 std::function<T(T)> 含 T，
        //   需收集（根因 A 修复后 t3/t4 由根因 B 的 std::function 包装推导）；
        // - Pair<T,int>（已实例化 record）→ 其声明形参 A/B 不直接出现在 C++ 形参
        //   （Pair<T,int>*），收集 A/B 造成方法模板参数冗余 Runner<A,B,T>（t13）。
        auto aliasIt = typeAliasTemplateParams_.find(n->name);
        if (aliasIt != typeAliasTemplateParams_.end()) {
            auto rit = registeredTypes_.find(n->name);
            bool isRecord = rit != registeredTypes_.end() && rit->second;
            if (!isRecord && !argGen.empty()) {
                for (auto& tp : aliasIt->second) out.insert(tp);
            }
            return;
        }
        // 排除内置类型（int/float/bool/string/None 不是泛型参数，无需 template<...>）
        if (n->name == "int" || n->name == "float" || n->name == "bool"
            || n->name == "string" || n->name == "None")
            return;

        // 无 typeArgs + 非注册类型 → 是泛型参数（如 Pair<A,B> 中的 A/B）
        // P1-2：内置接口（interfaces.aurai 的 Stringer/Comparable 等）不在
        // interfaceNames_（仅 program.decls 收集用户接口）且 findType 查不到
        // （接口不在 BuiltinRegistry types_）→ 此前被误判为泛型形参，使
        // `-> Stringer | None` 生成 template 函数。此处显式排除内置接口名。
        bool isBuiltinIface = false;
        for (auto& ai : BuiltinRegistry::get().auraiInterfaces())
            if (ai->name == n->name) { isBuiltinIface = true; break; }
        if (n->typeArgs.empty() && !registeredTypes_.count(n->name)
            && !interfaceNames_.count(n->name)
            && !isBuiltinIface
            && !BuiltinRegistry::get().findType(n->name))
            out.insert(n->name);
        return;
    }
    if (auto* l = dynamic_cast<const ListType*>(&type)) {
        if (l->elementType) collectTParams(*l->elementType, out);
        return;
    }
    if (auto* r = dynamic_cast<const RecordType*>(&type)) {
        for (auto& f : r->fields)
            if (f.type) collectTParams(*f.type, out);
        return;
    }
    if (auto* u = dynamic_cast<const UnionType*>(&type)) {
        for (auto& v : u->types)
            if (v) collectTParams(*v, out);
        return;
    }
    if (auto* fn = dynamic_cast<const FunctionType*>(&type)) {
        for (auto& p : fn->paramTypes)
            if (p) collectTParams(*p, out);
        if (fn->returnType) collectTParams(*fn->returnType, out);
        return;
    }
}


// feature-12 bug-82: 仅收集「形参类型本身是 FunctionType 时其内部 <> 引入的函数级泛型名」。
// 与 collectTParams 的区别：
//   make_adder(inc: <T>)                 -> 形参类型是【裸 GenericTypeRef】-> 不收集（闭包自身泛型）
//   make_tree_mapper(f: fun(<T>) -> <U>) -> 形参类型是【FunctionType】，其内部泛型 -> 收集
// 实现：仅当形参类型动态类型是 FunctionType 时，对其 paramTypes/returnType 跑 collectTParams。
// 注意：collectTParams 对 FunctionType 本身就会递归（L78-83），故此判定落在形参类型的
//       【动态类型】上，而不是 collectTParams 的递归里。
void CodeGenerator::collectFnTypeNestedGenerics(
        const TypeExpr& type, std::set<std::string>& out) const {
    if (auto* fn = dynamic_cast<const FunctionType*>(&type)) {
        for (auto& p : fn->paramTypes)
            if (p) collectTParams(*p, out);
        if (fn->returnType) collectTParams(*fn->returnType, out);
    }
}

std::vector<std::string> CodeGenerator::collectFunTParams(const FunDecl& decl) const {
    std::set<std::string> names;

    // plan12 统一方案：若返回泛型闭包，外层函数不模板化，泛型由闭包自身声明
    if (decl.returnType) {
        std::set<std::string> retGen;
        bool retIsGenericFunc = false;
        if (auto* ft = dynamic_cast<const FunctionType*>(decl.returnType.get())) {
            collectTParams(*ft, retGen);
            retIsGenericFunc = !retGen.empty();
        } else if (auto* nt = dynamic_cast<const NamedType*>(decl.returnType.get())) {
            // 仅函数式类型别名（如 Pipeline<T> = fun(T)->T，非堆类型）跳过模板化
            // 堆类型（如 Tree<T>）保持模板参数
            auto it = registeredTypes_.find(nt->name);
            if (typeAliasTemplateParams_.count(nt->name)
                && it != registeredTypes_.end() && !it->second)  // registered as non-heap
                return {};
        }
        // bug-82：返回泛型函数类型时，若这些泛型名由形参的【函数类型】内部 <> 引入
        // （函数级泛型）-> 必须模板化，不可早退。反之（闭包自身泛型，如 makeU /
        // make_adder 的形参顶层 <T>）-> 保持原早退行为。
        if (retIsGenericFunc) {
            std::set<std::string> fnNestedGen;
            for (auto& p : decl.params)
                if (p.type) collectFnTypeNestedGenerics(*p.type, fnNestedGen);
            bool needTemplatize = false;
            for (auto& g : retGen)
                if (fnNestedGen.count(g)) { needTemplatize = true; break; }
            if (!needTemplatize) return {};   // 闭包自身泛型 -> 原行为
        }
    }

    for (auto& p : decl.params)
        if (p.type) collectTParams(*p.type, names);
    if (decl.returnType) collectTParams(*decl.returnType, names);
    return {names.begin(), names.end()};
}

bool CodeGenerator::isFuncAliasRet(const TypeExpr* retType) const {
    if (!retType) return false;
    auto* nt = dynamic_cast<const NamedType*>(retType);
    if (!nt) return false;
    auto it = registeredTypes_.find(nt->name);
    return typeAliasTemplateParams_.count(nt->name) > 0
        && it != registeredTypes_.end() && !it->second; // 函数式别名 registered non-heap
}

bool CodeGenerator::isFunctionTypedParam(const Param& p) const {
    const TypeExpr* pty = p.type.get();
    if (!pty) return false;
    if (dynamic_cast<const FunctionType*>(pty)) return true;
    if (pty->inferredType
        && dynamic_cast<const FuncSemType*>(pty->inferredType))
        return true;
    // NamedType 非堆注册类型别名（Transform<T>/Transform = 函数式别名；含非模板别名
    // type Transform = fun(int)->int）：Sema 对形参类型标注的 inferredType 可能为空
    //（未走表达式推断），按别名注册表兜底。非堆注册类型仅函数/联合值别名——联合
    // 形参不会作为 callee 直呼（Sema 拦截），注册无副作用（对照 fnCallbackParams_
    // 的 NamedType 注册先例放开 typeAliasTemplateParams_ 限定，覆盖非模板别名）
    if (auto* nt = dynamic_cast<const NamedType*>(pty)) {
        auto rit = registeredTypes_.find(nt->name);
        if (rit != registeredTypes_.end() && !rit->second) return true;
    }
    return false;
}

std::vector<std::string> CodeGenerator::collectMethodTParams(const MethodDecl& decl) const {
    std::set<std::string> names;
    // 优先从 receiverTypeArgs（如 Stack<T> 中的 T）
    for (auto& ta : decl.receiverTypeArgs)
        names.insert(ta);
    // 从参数类型中收集
    for (auto& p : decl.params)
        if (p.type) collectTParams(*p.type, names);
    // 返回类型收集：函数式别名（Mapper<A,U>）不收集（闭包自身泛型 U 由闭包声明/调用点
    // 推断，对照 collectFunTParams 的 FunctionType 分支，M1）；直接写泛型函数类型
    // （fun(U,T)->U）仅在返回 FunctionType 含"未在 receiver/参数中出现的闭包自身新泛型"
    // 时不收集返回类型泛型——否则外层方法被迫模板化 + struct 内声明 'U' was not declared
    // （M5）；若返回泛型均在 receiver/参数中（如 Box<T>::identity() -> fun(T)->T 的 T），
    // 保持收集（方法模板化 + 显式 std::function<T(T)>，ClosureRefsOuterMethodTParamNoShadow）。
    if (decl.returnType && !isFuncAliasRet(decl.returnType.get())) {
        bool skipRetGen = false;
        if (auto* ft = dynamic_cast<const FunctionType*>(decl.returnType.get())) {
            std::set<std::string> retGen;
            collectTParams(*ft, retGen);
            for (auto& g : retGen)
                if (!names.count(g)) { skipRetGen = true; break; }
        }
        if (!skipRetGen)
            collectTParams(*decl.returnType, names);
    }
    return {names.begin(), names.end()};
}

} // namespace Aura
