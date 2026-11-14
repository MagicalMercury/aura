// ============================================================
// feature-21 Phase 3 · S2：DeclIndex 实现
//
// 权威：change.md §4.5（`change.md:525-691`）
// 简报：scripts/f21_p3_s2_brief.md
//
// S1 ⚠️7 修正：文档版 `collectTypeParamsFromType` 只覆盖 4 种 TypeExpr，
// 会漏掉出现在函数签名 `(int)->int` / 匿名 record / 元组 / enum 绑定类型里的泛型名。
// 此处按 `src/AST/Type.h` 真实字段补齐 4 个分支（不修改 AST）：
//   FunctionType  { paramTypes, returnType }
//   RecordType    { fields[].type }
//   TupleTypeExpr { elementTypes }
//   EnumTypeExpr  { bindingTypes }
// 与 Sema 侧 `forEachGenericRef`（DeclChecker.cpp:59-78）的覆盖面保持一致。
// ============================================================

#include "DeclIndex.h"

#include <algorithm>

namespace Aura::ct {

namespace {

std::string baseTypeName(const std::string& t) {
    auto lt = t.find('<');
    return lt == std::string::npos ? t : t.substr(0, lt);
}

void collectTypeParamsFromType(const TypeExpr* t, std::vector<std::string>& out) {
    if (!t) return;
    if (auto* g = dynamic_cast<const GenericTypeRef*>(t)) {
        if (std::find(out.begin(), out.end(), g->name) == out.end())
            out.push_back(g->name);
        return;
    }
    if (auto* n = dynamic_cast<const NamedType*>(t)) {
        for (const auto& a : n->typeArgs) collectTypeParamsFromType(a.get(), out);
        return;
    }
    if (auto* l = dynamic_cast<const ListType*>(t)) {
        collectTypeParamsFromType(l->elementType.get(), out);
        return;
    }
    if (auto* u = dynamic_cast<const UnionType*>(t)) {
        for (const auto& a : u->types) collectTypeParamsFromType(a.get(), out);
        return;
    }
    // ——— S1 ⚠️7 补充分支 ———
    if (auto* ft = dynamic_cast<const FunctionType*>(t)) {
        for (const auto& p : ft->paramTypes) collectTypeParamsFromType(p.get(), out);
        collectTypeParamsFromType(ft->returnType.get(), out);
        return;
    }
    if (auto* rt = dynamic_cast<const RecordType*>(t)) {
        for (const auto& f : rt->fields) collectTypeParamsFromType(f.type.get(), out);
        return;
    }
    if (auto* tt = dynamic_cast<const TupleTypeExpr*>(t)) {
        for (const auto& e : tt->elementTypes) collectTypeParamsFromType(e.get(), out);
        return;
    }
    if (auto* et = dynamic_cast<const EnumTypeExpr*>(t)) {
        for (const auto& b : et->bindingTypes) collectTypeParamsFromType(b.get(), out);
        return;
    }
}

std::vector<std::string> typeParamsOf(const FunDecl& f) {
    if (!f.typeParams.empty()) return f.typeParams;
    std::vector<std::string> out;
    for (const auto& p : f.params) collectTypeParamsFromType(p.type.get(), out);
    collectTypeParamsFromType(f.returnType.get(), out);
    return out;
}

std::vector<std::string> typeParamsOf(const MethodDecl& m) {
    std::vector<std::string> out = m.receiverTypeArgs;
    for (const auto& p : m.params) collectTypeParamsFromType(p.type.get(), out);
    collectTypeParamsFromType(m.returnType.get(), out);
    return out;
}

} // namespace

void DeclIndex::addModule(const std::string& modulePath, const Program& program) {
    for (const auto& d : program.decls) {
        if (!d) continue;
        if (auto* f = dynamic_cast<const FunDecl*>(d.get())) {
            funs_.push_back(FunInfo{f, typeParamsOf(*f), f->isComptime,
                                    modulePath, f->isPublic});
            continue;
        }
        if (auto* m = dynamic_cast<const MethodDecl*>(d.get())) {
            if (m->isConstructor || isComptimeMethod(*m)) continue;
            methods_.push_back(MethodInfo{m, typeParamsOf(*m),
                                          modulePath, m->isPublic});
            continue;
        }
    }
}

const FunInfo* DeclIndex::fun(const std::string& name,
                              const std::string& currentModule,
                              bool* ambiguous) const {
    if (ambiguous) *ambiguous = false;
    const FunInfo* pub = nullptr;
    int pubCount = 0;
    for (const auto& f : funs_) {
        if (!f.decl || f.decl->name != name) continue;
        if (f.modulePath == currentModule) return &f;   // 同模块优先（含私有）
        if (f.isPublic) {
            if (!pub) pub = &f;
            ++pubCount;
        }
    }
    if (pubCount > 1) {
        if (ambiguous) *ambiguous = true;
        return nullptr;
    }
    return pub;
}

const MethodInfo* DeclIndex::method(const std::string& receiverType,
                                    const std::string& name,
                                    const std::string& currentModule,
                                    bool* ambiguous) const {
    if (ambiguous) *ambiguous = false;
    const MethodInfo* pub = nullptr;
    int pubCount = 0;
    const std::string base = baseTypeName(receiverType);
    for (const auto& m : methods_) {
        if (!m.decl || m.decl->name != name) continue;
        if (baseTypeName(m.decl->receiverType) != base) continue;
        if (m.modulePath == currentModule) return &m;
        if (m.isPublic) {
            if (!pub) pub = &m;
            ++pubCount;
        }
    }
    if (pubCount > 1) {
        if (ambiguous) *ambiguous = true;
        return nullptr;
    }
    return pub;
}

} // namespace Aura::ct
