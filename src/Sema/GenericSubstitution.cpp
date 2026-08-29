#include "SemAnalyzer.h"

namespace Aura {

namespace {
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
    // joined 约等于 args 的字符总量，预留容量避免循环内多次重分配
    joined.reserve(args.size());
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

// 实参推断是否"需要期望类型"才能得到精确类型：
//   FunExpr  —— 无标注参数需从形参函数类型反推（缺口 1）
//   ListExpr —— 空列表元素类型需从形参列表类型反推（缺口 3）；非空也可透传期望
//   none()   —— 无期望 → Optional<error>（元素推不出）；传参形态 take(none())
//               （形参 Optional<T>）需期望反推元素，CodeGen 才能生成 make_none<T>
static bool needsExpectedType(const ASTNode& arg) {
    if (dynamic_cast<const FunExpr*>(&arg)) return true;
    if (dynamic_cast<const ListExpr*>(&arg)) return true;
    if (auto* ce = dynamic_cast<const CallExpr*>(&arg)) {
        if (auto* id = dynamic_cast<const Identifier*>(ce->callee.get()))
            return id->name == "none" && ce->args.empty();
    }
    return false;
}
} // namespace

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
        n->isTuple = r->isTuple;   // 元组标志随泛型实例化保留（canonicalName 为空，replaceCanonicalArg 空操作）
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
              "add 'throws' to the function signature or wrap in 'try { ... }' catch");
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
    // 两阶段参数检查 + 泛型映射收集：
    //   阶段一：非"需期望"实参 —— 纯自底向上收集泛型映射（普通表达式 / 非空列表）
    //   阶段二：需期望实参（FunExpr / ListExpr）—— 先代换已绑定泛型，再带期望推断，
    //           使泛型闭包实参（apply(fun(n){...}, 5)）能在 T 绑定后反推参数类型
    bool conflict = false;
    for (int phase = 1; phase <= 2; ++phase) {
        for (size_t i = 0; i < args.size() && i < formalTypes.size(); ++i) {
            // 尽力模式防御：`f(match 5)` 等实参 parseExpr 失败 → 实参为 null
            // （parseCall 已跳过部分，此处兜底）→ 干净跳过而非 *args[i] 空指针崩溃
            if (!args[i]) continue;
            bool needs = needsExpectedType(*args[i]);
            if (phase == 1 && needs) continue;   // 推迟到阶段二
            if (phase == 2 && !needs) continue;  // 阶段一已处理
            std::unique_ptr<SemType> argTy;
            if (phase == 2 && formalTypes[i]) {
                // 用已绑定的泛型映射代换形参，得到精确期望类型
                std::unique_ptr<SemType> substituted = formalTypes[i]->clone();
                for (auto& [name, concrete] : genericMap)
                    substituted = substitute(*substituted, name, *concrete);
                argTy = inferExpr(*args[i], substituted.get());
            } else if (formalTypes[i] && isRecordLiteralArg(*args[i])) {
                // #1：record 字面量实参（{..} / some({..})）——匿名 record 需期望类型
                // 才能解析（决策 A）。阶段一也带期望推断（直接用形参类型；不做
                // genericMap 代换——collectGenericMapping 会把已物化的内置泛型
                // （Optional<Point>）误绑为泛型变量，代换后期望类型被匿名 record
                // 覆盖）。否则 inferRecordExpr 无期望报错、CodeGen 退化为 designated init。
                argTy = inferExpr(*args[i], formalTypes[i]);
            } else {
                argTy = inferExpr(*args[i]);
            }
            if (formalTypes[i] && !isAssignable(*formalTypes[i], *argTy)) {
                error(*args[i], "argument type mismatch: expected '" +
                      formalTypes[i]->toString() + "', got '" + argTy->toString() + "'");
            }
            if (formalTypes[i])
                collectGenericMapping(*formalTypes[i], *argTy, genericMap, conflict);
        }
    }
    // P2-2: 泛型绑定冲突从静默忽略改为报错
    if (conflict) {
        error(callNode, "conflicting type arguments for generic parameter(s) in call to '" + calleeName + "'");
    }
    // #1：record 字面量实参 canonicalName 传播——inferRecordExpr 只按期望反推字段、
    // 不写 canonicalName；必须由 propagateCanonicalName 写入（含 GenericSemType{Optional}
    // / OptionalSemType / UnionSemType 分支对 some(record) 实参的下钻），否则
    // genRecordExpr 读不到具体类型退化为 designated init（函数/方法/构造函数实参）。
    // 直接用形参类型传播（不做 genericMap 代换——collectGenericMapping 会把已物化的
    // 内置泛型（Optional<Point>）误绑为泛型变量，代换后期望类型被匿名 record 覆盖，
    // canonicalName 无法写入）；泛型变量形参（resolvedName 空）本就无法解析（决策 A 报错）。
    for (size_t i = 0; i < args.size() && i < formalTypes.size(); ++i) {
        if (!args[i] || !formalTypes[i] || !isRecordLiteralArg(*args[i])) continue;
        typeStore_.push_back(formalTypes[i]->clone());
        propagateCanonicalName(*args[i], typeStore_.back().get());
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
        // G2-A：已物化的内置泛型（resolvedName 非空，如 Optional<Point> →
        // "aura_rt::Optional<Point*>"）不是泛型变量 <T>，不得当泛型变量绑定——
        // 否则显式 `Optional<X>`/`Iterator<X>`/`channel<X>` 注解被误绑，多实参同族
        // 泛型元素类型不同（Optional<Point> + Optional<int>）时误报 conflicting
        // type arguments。真正的泛型变量 T（GenericTypeRef / 泛型参数符号经
        // resolveType / resolveNamedType 产生）resolvedName 恒为空；用户泛型 type
        // 实例化（Pair<int>）是 RecordSemType / InterfaceSemType，不落入本分支。
        // 故 resolvedName 非空即跳过（不绑、不查冲突）。
        if (!gf->resolvedName.empty()) return;
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

} // namespace Aura
