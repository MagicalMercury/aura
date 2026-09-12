#include "SemAnalyzer.h"
#include "BuiltinRegistry.h"

namespace Aura {

namespace {
// 将 Aura 类型名映射为 C++ 类型名（未注册的类型保持原名）
std::string cppNameOf(const std::string& auraName) {
    if (auto* ti = BuiltinRegistry::get().findType(auraName))
        return ti->cppType;
    return auraName;
}

// G2-C：以下三个辅助供 cppNameOfTypeExpr 的 UnionType 分支使用，与 CodeGen
// TypeMap::mapType UnionType 分支（TypeMap.cpp L142-221）的判定/形态对齐。
//（cppNameOfTypeExpr 定义在下方，先前置声明供 unionVariantCppName 递归调用）
std::string cppNameOfTypeExpr(const TypeExpr* te);

// 折叠判定：`T|None`（恰 2 变体、另一为堆类型）是否折叠为 Optional。
// 与 Sema resolveType UnionType 折叠（unionVariantGcUnsafe，DeclChecker.cpp:29-47）
// 及 TypeMap::mapType L168-176 一致：record/string/list 折叠；内置泛型
// （BuiltinPrim::Other）/接口视图/值类型（int/float/bool/None）不折叠。
bool unionVariantFoldable(const TypeExpr* t) {
    if (!t) return false;
    if (auto* n = dynamic_cast<const NamedType*>(t)) {
        if (auto* ti = BuiltinRegistry::get().findType(n->name)) {
            switch (ti->primKind) {
                case BuiltinPrim::Int:
                case BuiltinPrim::Float:
                case BuiltinPrim::Bool:
                case BuiltinPrim::None_:
                    return false;             // 值类型不折叠
                case BuiltinPrim::String:
                    return true;              // GcString* 堆 → 折叠
                case BuiltinPrim::Other:
                    return false;             // 内置泛型（Optional<int>/Iterator<T>）不折叠
            }
        }
        // 内置接口视图不折叠（与 Sema InterfaceSemType=false 一致）；用户类型
        // 无法区分 record/接口 → 按 record 折叠（接口进 Variant 的形态属边界，非本次目标）
        for (auto& ai : BuiltinRegistry::get().auraiInterfaces())
            if (ai->name == n->name) return false;
        return true;
    }
    if (dynamic_cast<const ListType*>(t)) return true;    // Array<...>* 堆
    if (dynamic_cast<const UnionType*>(t)) return true;   // 递归联合视为堆
    if (dynamic_cast<const RecordType*>(t)) return true;  // 内联 record 堆
    return false;  // GenericTypeRef / FunctionType 等保守不折叠
}

// Variant 堆判定：任一变体含 GC 引用（堆指针 / 含 self 的接口·Iterator 视图）→
// aura_rt::Variant<...>*（GC 封装），否则 std::variant<...>（全值 POD）。
// 与 TypeMap::mapType L181-213 的 hasHeap 判定对齐（接口/Iterator 视图按堆）。
bool unionVariantHasHeap(const TypeExpr* t) {
    if (!t) return false;
    if (auto* n = dynamic_cast<const NamedType*>(t)) {
        if (auto* ti = BuiltinRegistry::get().findType(n->name)) {
            switch (ti->primKind) {
                case BuiltinPrim::Int:
                case BuiltinPrim::Float:
                case BuiltinPrim::Bool:
                case BuiltinPrim::None_:
                    return false;
                default:
                    return true;  // String / Other（Optional<...>* / Iterator 视图等）
            }
        }
        return true;  // 用户 record（堆）/ 接口视图（含 self GC 指针）→ 堆封装
    }
    if (dynamic_cast<const ListType*>(t)) return true;        // Array<...>* 堆
    if (dynamic_cast<const UnionType*>(t)) return true;       // 递归联合 → 堆
    if (dynamic_cast<const FunctionType*>(t)) return false;   // std::function 值
    if (dynamic_cast<const RecordType*>(t)) return true;      // 内联 record 堆
    return false;
}

// 联合变体 → 完整 C++ 类型名（与 TypeMap::mapType 的变体形态一致：堆 record 叶补
// '*'、容器递归、接口/Iterator 视图无 '*'）。与基础 cppNameOfTypeExpr 的差异仅在
// record 叶的 '*'（mapType registeredTypes_ 语义），否则 Optional<[Point|None]>
// 的 Optional<Point> 元素缺 * 与声明侧 mapType 不匹配。
std::string unionVariantCppName(const TypeExpr* v) {
    if (!v) return "";
    if (auto* n = dynamic_cast<const NamedType*>(v)) {
        if (n->namespacePrefix.empty() && n->typeArgs.empty()) {
            if (auto* ti = BuiltinRegistry::get().findType(n->name))
                return ti->cppType;    // int/float/string/None 等
            // 内置接口视图（Stringer/Comparable/Iterator 基名）：值视图无 '*'
            for (auto& ai : BuiltinRegistry::get().auraiInterfaces())
                if (ai->name == n->name) return n->name;
            // 用户类型：record（堆指针补 '*'）或接口（无法区分，保守按 record 处理）
            return n->name + "*";
        }
        return cppNameOfTypeExpr(v);   // 泛型实例 / 命名空间限定
    }
    if (auto* l = dynamic_cast<const ListType*>(v)) {
        std::string elem = unionVariantCppName(l->elementType.get());
        if (elem.empty()) return "";
        return "aura_rt::Array<" + elem + ">*";
    }
    return cppNameOfTypeExpr(v);       // Union/Function/GenericRef 等
}

// 递归拼接 TypeExpr 的完整 C++ 模板参数形态（materializeCanonicalName 用），
// 修复嵌套泛型参数丢失（原 cppNameOf 只取顶层名，如 Optional<Iterator<int>>
// 物化为 Optional<Iterator*>——丢 <int> 且多出旧指针形态 * 后缀）：
//   - NamedType：registry 查表（namespacePrefix 拼全限定名优先）；有 typeArgs 时
//     逐层递归展开。registry 命中的 cppType * 后缀按"嵌套元素"语义剥除
//     （aura_rt::Iterator* → aura_rt::Iterator<int>）；未命中的用户泛型实例
//     拼完补 *（用户泛型 record/接口实例均为堆指针）
//   - ListType：aura_rt::Array<E>*（与 mapType L135-138 一致）
//   - GenericTypeRef：泛型形参保留裸名
//   - UnionType：与 TypeMap::mapType UnionType 分支同形态（T|None 折叠 Optional /
//     多成员 Variant），此前落空串导致 Optional<[Point|None]> resolvedName 损坏（G2-C）
//   - Tuple/Record 内联形态及未注册全限定名：返回空串，调用方拼接空参数，
//     维持旧路径行为（这些形态本应被上游拦截）
std::string cppNameOfTypeExpr(const TypeExpr* te) {
    if (!te) return "";
    if (auto* nt = dynamic_cast<const NamedType*>(te)) {
        std::string name;
        if (!nt->namespacePrefix.empty()) {
            std::string fq;
            // 预估容量：各命名空间段 + 分隔点 + 末段名，减少循环内重分配
            fq.reserve(nt->name.size() + nt->namespacePrefix.size() * 16);
            for (const auto& ns : nt->namespacePrefix) fq += ns + ".";
            fq += nt->name;
            name = cppNameOf(fq);
            if (name == fq) return "";  // 未注册的全限定名（跨模块用户类型），无法可靠映射
        } else {
            name = cppNameOf(nt->name);
        }
        if (nt->typeArgs.empty()) return name;
        bool userGeneric = (name == nt->name);  // registry 未命中 → 用户泛型实例
        if (!name.empty() && name.back() == '*') name.pop_back();
        name += "<";
        // 预估模板参数拼接容量，减少循环内多次重分配
        name.reserve(name.size() + nt->typeArgs.size() * 16);
        for (size_t j = 0; j < nt->typeArgs.size(); ++j) {
            if (j > 0) name += ", ";
            name += cppNameOfTypeExpr(nt->typeArgs[j].get());
        }
        name += ">";
        if (userGeneric) name += "*";  // 用户泛型实例是堆指针
        return name;
    }
    if (auto* lt = dynamic_cast<const ListType*>(te)) {
        std::string elem = cppNameOfTypeExpr(lt->elementType.get());
        if (elem.empty()) return "";
        return "aura_rt::Array<" + elem + ">*";
    }
    if (auto* ft = dynamic_cast<const FunctionType*>(te)) {
        // 函数类型 → std::function<...>（与 TypeMap::mapType FunctionType 分支一致，
        // 忽略 throws）。返回/参数任一无法映射则放弃（保持旧空串行为，避免拼坏 C++ 名）
        std::string ret = ft->returnType ? cppNameOfTypeExpr(ft->returnType.get()) : "void";
        if (ret.empty()) return "";
        std::string sig = "std::function<" + ret + "(";
        bool first = true;
        for (auto& p : ft->paramTypes) {
            std::string pt = p ? cppNameOfTypeExpr(p.get()) : "";
            if (pt.empty()) return "";
            if (!first) sig += ", ";
            sig += pt;
            first = false;
        }
        sig += ")>";
        return sig;
    }
    if (auto* gtr = dynamic_cast<const GenericTypeRef*>(te))
        return cppNameOf(gtr->name);
    if (auto* u = dynamic_cast<const UnionType*>(te)) {
        // G2-C：UnionType（此前落空串 → Optional<[Point|None]> 等内置泛型 typeArg
        // 含 union 时 resolvedName="aura_rt::Optional<>" 损坏，elemTypeOf 提空串 →
        // 元素期望断裂「cannot infer type of record literal」）。与 TypeMap::mapType
        // UnionType 分支（TypeMap.cpp L142-221）同形态：
        //   - `T|None`（恰 2 变体、另一为堆类型）→ 折叠 "aura_rt::Optional<elem>*"
        //     （与 Sema resolveType / mapType 折叠一致：record/string/list 折叠）
        //   - 其余 → 含堆变体 ? "aura_rt::Variant<...>*" : "std::variant<...>"
        if (u->types.size() == 2) {
            auto* na = dynamic_cast<const NamedType*>(u->types[0].get());
            auto* nb = dynamic_cast<const NamedType*>(u->types[1].get());
            auto isNoneNamed = [](const NamedType* n) {
                return n && n->name == "None" && n->typeArgs.empty()
                    && n->namespacePrefix.empty();
            };
            bool aIsNone = isNoneNamed(na);
            bool bIsNone = isNoneNamed(nb);
            if (aIsNone != bIsNone) {  // 恰一个为 None
                const auto& other = aIsNone ? u->types[1] : u->types[0];
                if (other && unionVariantFoldable(other.get()))
                    return "aura_rt::Optional<" + unionVariantCppName(other.get()) + ">*";
            }
        }
        bool hasHeap = false;
        for (auto& v : u->types)
            if (v && unionVariantHasHeap(v.get())) { hasHeap = true; break; }
        // feature-05：全值联合弃用 std::variant → aura_rt::ValueVariant 值语义
        // （与 TypeMap::mapType / mapSemType 生成形态一致，消除 resolvedName 分裂）
        std::string result = hasHeap ? "aura_rt::Variant<" : "aura_rt::ValueVariant<";
        for (size_t i = 0; i < u->types.size(); ++i) {
            if (i > 0) result += ", ";
            result += u->types[i] ? unionVariantCppName(u->types[i].get()) : "void";
        }
        result += hasHeap ? ">*" : ">";
        return result;
    }
    return "";
}
} // namespace

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

// #4：声明完成后折叠自引用 `Node | None` 字段为 Optional<Node>（见头文件注释）。
// 与 mapType 声明侧折叠（TypeMap.cpp UnionType 分支）对齐：仅在「恰 2 变体、
// 其一 None、另一为已 sealed（resolvedName 非空）且非内置泛型」时折叠——内置
// 泛型（channel/Iterator/Optional 等）Sema 与 mapType 均保持 Variant 路径，不折叠。
void SemAnalyzer::foldSelfRefOptionalUnions(std::unique_ptr<SemType>& node) {
    if (auto* u = dynamic_cast<UnionSemType*>(node.get())) {
        if (u->variants.size() == 2) {
            GenericSemType* genV = nullptr;
            const SemType* noneV = nullptr;
            for (auto& v : u->variants) {
                if (!v) continue;
                if (dynamic_cast<const NoneSemType*>(v.get())) noneV = v.get();
                else if (auto* g = dynamic_cast<GenericSemType*>(v.get())) genV = g;
            }
            if (noneV && genV && !genV->resolvedName.empty()) {
                auto* ti = BuiltinRegistry::get().findType(genV->name);
                bool isBuiltinGeneric = ti && ti->primKind == BuiltinPrim::Other;
                if (!isBuiltinGeneric) {
                    node = OptionalSemType::make(genV->clone());
                    return;
                }
            }
        }
        for (auto& v : u->variants)
            if (v) foldSelfRefOptionalUnions(v);
        return;
    }
    if (auto* r = dynamic_cast<RecordSemType*>(node.get())) {
        for (auto& f : r->fields)
            if (f.type) foldSelfRefOptionalUnions(f.type);
    } else if (auto* l = dynamic_cast<ListSemType*>(node.get())) {
        if (l->elementType) foldSelfRefOptionalUnions(l->elementType);
    } else if (auto* o = dynamic_cast<OptionalSemType*>(node.get())) {
        if (o->elementType) foldSelfRefOptionalUnions(o->elementType);
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
        fullName.reserve(fullName.size() + n.typeArgs.size() * 16);
        for (size_t i = 0; i < n.typeArgs.size(); ++i) {
            if (i > 0) fullName += ", ";
            if (dynamic_cast<const GenericTypeRef*>(n.typeArgs[i].get())) {
                allConcrete = false; break;
            }
            // bug-17：实参统一走 semTypeToCppName(resolveType(...))——与 substitute
            // 实例化路径（GenericSubstitution.cpp:95 semTypeToCppName 补 *）及 CodeGen
            // mapType（递归补 *）三方对齐。cppNameOfTypeExpr 对 record 实参（Rec5）不补
            // *、对内置堆泛型实参（Optional<int>）剥 * 不补回 → 物化 canonicalName
            // 缺 * 与声明侧 Box2<Rec5*>* / Box2<Optional<int>*>* 不一致 → 坏 C++。
            // resolveType 递归解析嵌套泛型（Tree<Tree<int>> 内层也完整拼接），逐层正确。
            fullName += semTypeToCppName(*resolveType(*n.typeArgs[i]));
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
    // 模板参数统一走 semTypeToCppName(resolveType(...)) 递归展开（bug-17，同
    // RecordSemType 分支）：实参为 record/内置堆泛型时补 *，与声明侧 mapType 一致
    //   - 嵌套泛型（Optional<Iterator<int>>）逐层拼接，修复丢内层参数 + 旧 * 后缀
    //   - ListType（Optional<[int]>）→ aura_rt::Array<E>*
    //   - 泛型形参（T）保留裸名
    std::string fullName = base + "<";
    fullName.reserve(fullName.size() + n.typeArgs.size() * 16);
    for (size_t i = 0; i < n.typeArgs.size(); ++i) {
        if (i > 0) fullName += ", ";
        fullName += semTypeToCppName(*resolveType(*n.typeArgs[i]));
    }
    fullName += ">";
    gs->resolvedName = fullName;
}

} // namespace Aura
