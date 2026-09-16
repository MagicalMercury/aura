#include "CodeGen.h"
#include "../Sema/BuiltinRegistry.h"
#include "../Sema/SemType.h"
#include <sstream>

namespace Aura {

std::string CodeGenerator::genMemberAccess(const MemberAccessExpr& e) {
    // feature-06（阶段 C）：方法值（p.next——record 方法在取值位置）→ __mv_N 包装
    //（CallableObj 值，cap_recv 槽承载 receiver）。判定：member 非字段而是方法
    //（recordMethods_ 命中）且 Sema 推断为 FuncSemType（方法值）。字段访问（含闭包
    // 字段值）保持 obj->field 原路径。字段/方法同名时字段优先（recordMethods_ 只含
    // 方法名，字段闭包不注册）。
    if (e.object && e.object->inferredType && e.inferredType
        && dynamic_cast<const FuncSemType*>(e.inferredType)) {
        if (auto* ro = dynamic_cast<const RecordSemType*>(e.object->inferredType)) {
            std::string recKey = ro->canonicalName;
            size_t lt = recKey.find('<');
            if (lt != std::string::npos) recKey = recKey.substr(0, lt);
            auto mIt = recordMethods_.find(recKey);
            if (mIt != recordMethods_.end() && mIt->second.count(e.member)) {
                std::string w = genFnRefCallableObjValue(e);
                if (!w.empty()) return w;
            }
        }
    }
    // bug-68：Union receiver 字段直访（h.v.x，v: int | Point）→ 逐变体 get-if 分派
    //（镜像 ExprMethodCall.cpp genUnionDispatch：按 active index 取具体变体字段；
    // 含该字段的变体 → 具体类型 .field；其余 → default 抛 type_error）。
    // 表示形态：有堆变体 → aura_rt::Variant<...>*（->index()/->get<I>()）；
    // 全值 → aura_rt::ValueVariant<...>（.index()/.get<I>()），与 StmtMatch 一致。
    // Sema inferMemberAccess 已按「任一 record 变体含该字段」前置校验。
    if (e.object && e.object->inferredType) {
        if (auto* u = dynamic_cast<const UnionSemType*>(e.object->inferredType)) {
            std::vector<size_t> hit;
            std::vector<std::string> varCpp;
            const SemType* fieldType = nullptr;
            bool hasHeap = false;
            for (size_t k = 0; k < u->variants.size(); ++k) {
                auto& v = u->variants[k];
                varCpp.push_back(v ? mapSemType(*v) : "void");
                if (v && isUnionHeapVariant(v.get())) hasHeap = true;
                auto* rec = v ? dynamic_cast<const RecordSemType*>(v.get()) : nullptr;
                if (!rec) continue;
                for (auto& f : rec->fields) {
                    if (f.name == e.member) {
                        hit.push_back(k);
                        if (!fieldType) fieldType = f.type.get();
                        break;
                    }
                }
            }
            std::string retType = fieldType ? mapSemType(*fieldType) : "void";
            std::ostringstream uout;
            uout << "[&]() -> " << retType << " {\n";
            indentLevel_++;
            std::string objExpr = genExpr(*e.object, false);
            uout << indentStr() << "auto _fa_v = (" << objExpr << ");\n";
            if (hasHeap)
                uout << indentStr()
                     << "aura_rt::GcRootHandle<decltype(_fa_v)> _fa_rh(_fa_v);\n";
            std::string acc = hasHeap ? "->" : ".";
            if (hit.empty()) {
                // 防御：正常编译产物不可达（Sema 已拦截无字段联合）
                uout << indentStr() << "throw aura_rt::make_type_error(\"TypeError: variant has no field '"
                     << e.member << "'\");\n";
            } else if (hit.size() == 1) {
                size_t I = hit[0];
                uout << indentStr() << "if (_fa_v" << acc << "index() != " << I
                     << ") throw aura_rt::make_type_error(\"TypeError: union ("
                     << u->toString() << ") active variant has no field '" << e.member << "'\");\n";
                uout << indentStr() << "return _fa_v" << acc << "get<" << I << ">()"
                     << (varCpp[I].size() && varCpp[I][varCpp[I].size()-1] == '*' ? "->" : ".")
                     << safeName(e.member) << ";\n";
            } else {
                uout << indentStr() << "switch (_fa_v" << acc << "index()) {\n";
                indentLevel_++;
                for (size_t I : hit) {
                    uout << indentStr() << "case " << I << ": return _fa_v" << acc
                         << "get<" << I << ">()"
                         << (varCpp[I].size() && varCpp[I][varCpp[I].size()-1] == '*' ? "->" : ".")
                         << safeName(e.member) << ";\n";
                }
                uout << indentStr() << "default: throw aura_rt::make_type_error(\"TypeError: union ("
                     << u->toString() << ") active variant has no field '" << e.member << "'\");\n";
                indentLevel_--;
                uout << indentStr() << "}\n";
            }
            indentLevel_--;
            uout << indentStr() << "}()";
            return uout.str();
        }
    }
    std::string obj = genExpr(*e.object, false);
    bool isPointer = true;
    if (auto* id = dynamic_cast<const Identifier*>(e.object.get())) {
        if (valueTypeVarNames_.count(id->name)) {
            isPointer = false;
        }
    }
    std::string access = isPointer ? "->" : ".";
    return obj + access + safeName(e.member);
}

// ============================================================
// P4：联合接收者动态分派（方法调用）
// 生成运行时类型判定：单支持变体 → 检查 index 后直调；多变体 → switch 分派。
// 激活变体不支持该调用时抛 TypeError（make_type_error）。
// 注：IIFE 内不能含 co_await（C++ 限制），参数含 co_await 的联合分派 v1 不支持。
// ============================================================
std::string CodeGenerator::genUnionDispatch(const MethodCallExpr& e,
                                            const UnionSemType& u,
                                            bool isCoroutine) {
    // 支持变体判定（镜像 Sema inferMethodCallOnVariant）：
    //   - 接口变体：方法在接口方法集中
    //   - 内置类型变体（string / [T] / 内置泛型）：BuiltinRegistry 有该方法
    //   - record / None / Optional / 嵌套联合：不支持（无法静态判定，参数兼容过滤语义）
    std::vector<size_t> sups;
    for (size_t k = 0; k < u.variants.size(); ++k) {
        auto& v = u.variants[k];
        if (!v) continue;
        if (dynamic_cast<const NoneSemType*>(v.get())) continue;
        if (auto* iface = dynamic_cast<const InterfaceSemType*>(v.get())) {
            for (auto& m : iface->methods)
                if (m.name == e.method) { sups.push_back(k); break; }
            continue;
        }
        std::string typeKey;
        if (auto* p = dynamic_cast<const PrimSemType*>(v.get()))
            if (p->kind == PrimSemType::String) typeKey = "string";
        if (dynamic_cast<const ListSemType*>(v.get())) typeKey = "[T]";
        if (auto* g = dynamic_cast<const GenericSemType*>(v.get()))
            if (BuiltinRegistry::get().findType(g->name)) typeKey = g->name;
        if (typeKey.empty()) continue;
        if (BuiltinRegistry::get().findMethod(typeKey, e.method, (int)e.args.size()))
            sups.push_back(k);
    }
    if (sups.empty()) {
        // Sema 已静态拦截；防御：运行时 TypeError（正常编译产物不可达）
        return "([]{ throw aura_rt::make_type_error(\"TypeError: variant has no method '"
               + e.method + "'\"); })()";
    }

    std::string obj = genExpr(*e.object, isCoroutine);
    std::vector<std::string> argExprs;
    for (size_t i = 0; i < e.args.size(); ++i)
        argExprs.push_back(e.args[i] ? genExpr(*e.args[i], isCoroutine) : "???");

    // 返回类型：Sema 合并结果（合并为联合时 v1 用 auto 推导，用户需保证各变体返回一致）
    std::string retType = "auto";
    if (e.inferredType && !dynamic_cast<const UnionSemType*>(e.inferredType))
        retType = mapSemType(*e.inferredType);
    // NoneType 返回（如 append）→ IIFE 返回 void，调用点不 return 值
    bool retIsVoid = retType == "aura_rt::NoneType";
    if (retIsVoid) retType = "void";

    std::ostringstream out;
    // bug-77: method-call receiver evaluation window (union dispatch path).
    // C++17 [expr.call]/8: the postfix-expression (the receiver) is evaluated
    // BEFORE the arguments. The old form bound a raw receiver reference first
    // and then evaluated the arguments; that reference is a PLAIN pointer with
    // no root, so an argument triggering GC (intern_string / concat / gc_force)
    // could compact the object and leave it dangling -> UAF.
    // Hardening (same shape as G6, see ExprCall.cpp:721-779): pass arguments as
    // a parameter pack, so they are evaluated AT THE CALL SITE (before the
    // receiver); bind the receiver inside the lambda body and root it via
    // GcRootHandle so compact rewrites it. All access goes through the handle
    // (one pack expansion only, matching G6 fixed form). co_await arguments
    // stay at the call site: C++20 forbids co_await in a deduced-return lambda.
    const int dspHid = unionDispatchCounter_++;
    out << "[&](auto&&... _as) -> " << retType << " {\n";
    indentLevel_++;
    out << indentStr() << "auto&& _dsp_v" << dspHid << " = (" << obj << ");\n";
    out << indentStr() << "aura_rt::GcRootHandle<std::remove_reference_t<decltype(_dsp_v"
        << dspHid << ")>> _dsp_h" << dspHid << "(_dsp_v" << dspHid
        << ", aura_rt::GcRootScope::ThreadLocal);\n";
    // 单包展开的实参传递（经 _as 包，与 G6 一致：只有一个 \...\）
    // arguments are injected through the _as pack (single ... expansion, same as G6)
    std::string argCallArgs;   // args injected via the _as pack (single ... expansion, same as G6)
    if (!argExprs.empty())
        argCallArgs = "static_cast<decltype(_as)>(_as)...";
    if (sups.size() == 1) {
        // 单支持变体：运行时类型检查 + 直调
        size_t I = sups[0];
        out << indentStr() << "if (_dsp_h" << dspHid << ".get()->index() != " << I
            << ") throw aura_rt::make_type_error(\"TypeError: variant active variant has no method '"
            << e.method << "'\");\n";
        out << indentStr();
        if (!retIsVoid) out << "return ";
        // #3：接口视图变体是值类型（Stringer），用 . 访问；其余变体用 ->
        out << "_dsp_h" << dspHid << ".get()->get<" << I << ">()"
            << (isIfaceView(u.variants[I].get()) ? "." : "->")
            << safeName(e.method) << "(" << argCallArgs << ")";
        if (retIsVoid)
            out << "; return;\n";
        else
            out << ";\n";
    } else {
        // 多变体：switch 分派，default 抛 TypeError
        out << indentStr() << "switch (_dsp_h" << dspHid << ".get()->index()) {\n";
        indentLevel_++;
        for (size_t k : sups) {
            out << indentStr() << "case " << k << ": ";
            if (!retIsVoid) out << "return ";
            out << "_dsp_h" << dspHid << ".get()->get<" << k << ">()"
                << (isIfaceView(u.variants[k].get()) ? "." : "->")
                << safeName(e.method) << "(" << argCallArgs << ")";
            if (retIsVoid)
                out << "; return;\n";
            else
                out << ";\n";
        }
        out << indentStr() << "default: throw aura_rt::make_type_error(\"TypeError: variant ("
            << u.toString() << ") active variant has no method '" << e.method << "'\");\n";
        indentLevel_--;
        out << indentStr() << "}\n";
    }
    indentLevel_--;
    out << indentStr() << "}(";
    for (size_t i = 0; i < argExprs.size(); ++i) {
        if (i > 0) out << ", ";
        out << argExprs[i];
    }
    out << ")";
    return out.str();
}

// ============================================================
// P4：联合索引分派（v[i]）
// 支持变体判定：仅数组变体（ListSemType）。生成方式同 genUnionDispatch。
// ============================================================
std::string CodeGenerator::genUnionIndexDispatch(const IndexExpr& e,
                                                 const UnionSemType& u,
                                                 bool isCoroutine) {
    std::string obj = genExpr(*e.object, isCoroutine);
    std::string idx = genExpr(*e.index, isCoroutine);

    std::vector<size_t> sups;
    for (size_t k = 0; k < u.variants.size(); ++k) {
        auto& v = u.variants[k];
        if (v && dynamic_cast<const ListSemType*>(v.get())) sups.push_back(k);
    }
    if (sups.empty()) {
        return "([]{ throw aura_rt::make_type_error(\"TypeError: variant has no index operator\"); })()";
    }

    std::string retType = "auto";
    if (e.inferredType && !dynamic_cast<const UnionSemType*>(e.inferredType))
        retType = mapSemType(*e.inferredType);

    std::ostringstream out;
    out << "[&]() -> " << retType << " {\n";
    indentLevel_++;
    out << indentStr() << "auto&& _dsp_v = (" << obj << ");\n";
    if (sups.size() == 1) {
        size_t I = sups[0];
        out << indentStr() << "if (_dsp_v->index() != " << I
            << ") throw aura_rt::make_type_error(\"TypeError: variant active variant has no index operator\");\n";
        out << indentStr() << "return (*_dsp_v->get<" << I << ">())["
            << idx << "];\n";
    } else {
        out << indentStr() << "switch (_dsp_v->index()) {\n";
        indentLevel_++;
        for (size_t k : sups) {
            out << indentStr() << "case " << k << ": return (*_dsp_v->get<" << k << ">())["
                << idx << "];\n";
        }
        out << indentStr() << "default: throw aura_rt::make_type_error(\"TypeError: variant active variant has no index operator\");\n";
        indentLevel_--;
        out << indentStr() << "}\n";
    }
    indentLevel_--;
    out << indentStr() << "}()";
    return out.str();
}

std::string CodeGenerator::genIndexExpr(const IndexExpr& e, bool isCoroutine) {
    // P4：联合索引分派
    if (e.object && e.object->inferredType) {
        if (auto* u = dynamic_cast<const UnionSemType*>(e.object->inferredType))
            return genUnionIndexDispatch(e, *u, isCoroutine);
    }
    std::string obj   = genExpr(*e.object, false);
    std::string idx   = genExpr(*e.index, isCoroutine);
    return "(*" + obj + ")[" + idx + "]";
}

// ============================================================
// 赋值
// ============================================================

std::string CodeGenerator::genAssignExpr(const AssignExpr& e, bool isCoroutine) {
    std::string target = genExpr(*e.target, isCoroutine);
    // feature-06（阶段 C）：Callable 目标（CallableSemType 变量/字段/元素）重赋值 →
    // 值包装为 CallableErased（函数形态包装；Callable 值拷贝透传）。目标签名取
    // target->inferredType 的 origins（Sema 赋值传播点 3 已 join 本次签名）。
    const FuncSemType* erasedSig = nullptr;
    if (e.target && e.target->inferredType) {
        if (auto* ctgt = dynamic_cast<const CallableSemType*>(e.target->inferredType))
            if (ctgt->origins.size() == 1) erasedSig = ctgt->origins[0].get();
    }
    // P3b：目标为"含 None 变体的联合"且赋 none() 时，生成 NoneType 值而非 Optional 指针
    std::string value;
    if (isNoneCallExpr(*e.value)) {
        bool unionHasNone = false;
        if (auto* u = dynamic_cast<const UnionSemType*>(e.inferredType)) {
            for (auto& v : u->variants)
                if (v && dynamic_cast<const NoneSemType*>(v.get())) { unionHasNone = true; break; }
        }
        value = unionHasNone ? "aura_rt::None" : genExpr(*e.value, isCoroutine);
    } else {
        // P3b 隐式装箱：目标为含堆联合（Variant 指针）且赋非联合值 → make_variant<I>
        // G1 延伸：目标为 Optional（OptionalSemType / GenericSemType{name=="Optional"}）
        // 且赋裸值（record/值/列表，如 o = {..} / arr[0] = {..}）→ make_optional 装箱
        std::string boxed;
        if (e.target && e.target->inferredType) {
            if (dynamic_cast<const CallableSemType*>(e.target->inferredType)) {
                // feature-06（阶段 C）：Callable 目标 → 函数形态值包装为 CallableErased
                boxed = genErasedInitValue(*e.value, erasedSig);
            } else if (auto* u = dynamic_cast<const UnionSemType*>(e.target->inferredType))
                boxed = genUnionBoxing(*u, *e.value, isCoroutine);
            else if (auto* os = dynamic_cast<const OptionalSemType*>(e.target->inferredType)) {
                std::string elem = os->elementType ? mapSemType(*os->elementType) : "";
                if (!elem.empty())
                    boxed = genOptionalTargetInit(*e.value, elem, isCoroutine);
            } else if (auto* gs = dynamic_cast<const GenericSemType*>(e.target->inferredType);
                       gs && gs->name == "Optional") {
                std::string elem = optionalElemCppName(gs);
                if (!elem.empty())
                    boxed = genOptionalTargetInit(*e.value, elem, isCoroutine);
            }
        }
        if (!boxed.empty())
            value = boxed;
        else
            value = genExpr(*e.value, isCoroutine);
    }

    // stripGet 辅助：去掉 GcRootHandle 变量的 ".get()" 后缀，返回裸变量名
    auto stripGet = [](const std::string& s) -> std::string {
        if (s.size() > 6 && s.substr(s.size() - 6) == ".get()")
            return s.substr(0, s.size() - 6);
        return s;
    };

    // s = s + x 优化：若变量是 string 且赋值为自身 + 单元素，改写为 append
    // 如 s = s + "x" → s.get()->append(make_string("x"))
    // append 在容量足够时原地修改，避免 concat 每次创建新对象的开销
    if (auto* targetId = dynamic_cast<const Identifier*>(e.target.get())) {
        if (auto* binExpr = dynamic_cast<const BinaryExpr*>(e.value.get())) {
            if (binExpr->op == "+") {
                if (auto* leftId = dynamic_cast<const Identifier*>(binExpr->left.get())) {
                    std::string targetBase = stripGet(targetId->name);
                    std::string leftBase   = stripGet(leftId->name);
                    if (targetBase == leftBase && stringVarNames_.count(targetBase)) {
                        std::string rightExpr = genExpr(*binExpr->right, isCoroutine);
                        // 用 GcRootHandle 保护 rightExpr 求值期间 targetBase.get() 的裸指针
                        std::vector<std::pair<std::string, const SemType*>> gcArgs;
                        gcArgs.emplace_back(rightExpr, binExpr->right->inferredType);
                        // 修复 append 返回值丢弃 bug：
                        // append 容量不足时返回新分配的 GcString*，必须赋回 targetBase.get()
                        // 否则 s 永远不增长且每次迭代都从同一小基址 realloc
                        // GcRootHandle::get() 非 const 版本返回 T&（GcString*&），可作赋值左侧
                        return targetBase + ".get() = " + genGcRootedArgs(gcArgs,
                            targetBase + ".get()->append({0})", isCoroutine);
                    }
                }
            }
        }
    }

    // 如果目标变量是字符串类型且值使用了 concat，更新追踪
    // Bug 修复：stripGet 后再查/插，保持 stringVarNames_ 的 key 一致（裸变量名）
    std::string targetBase = stripGet(target);
    if (stringVarNames_.count(targetBase)) {
        if (value.find("aura_rt::concat") == std::string::npos &&
            value.find("aura_rt::make_string") == std::string::npos &&
            value.find("aura_rt::intern_string") == std::string::npos) {
            // 不再从 make_string/concat/intern_string 赋值 — 移除字符串追踪
            // (但保守起见保留 — 可能是 string + int 产生的 concat 还没替换)
        }
    }
    // 如果值包含 concat/make_string/intern_string，标记目标为字符串变量
    // Bug 修复（同 StmtGen genLetStmt）：排除 IIFE 顶层——如 `x = float("1")!`
    // 生成的 [&]() -> auto { ...intern_string... }() 内部含 intern_string 但结果是 float；
    // 结果类型由下方 Sema inferredType 判定覆盖
    if (!value.empty() && !(value.size() > 4 && value.compare(0, 4, "[&](") == 0) &&
        (value.find("aura_rt::concat") != std::string::npos ||
         value.find("aura_rt::make_string") != std::string::npos ||
         value.find("aura_rt::intern_string") != std::string::npos ||
         value.find("aura_rt::string_of") != std::string::npos)) {
        stringVarNames_.insert(targetBase);
    }
    if (e.value->inferredType) {
        if (auto* p = dynamic_cast<const PrimSemType*>(e.value->inferredType)) {
            if (p->kind == PrimSemType::String)
                stringVarNames_.insert(targetBase);
        }
    }

    // 写屏障：GC 对象字段赋值（如 obj.field = newVal）时，
    // 记录 old→young 跨代引用到记忆集
    if (isGcFieldAssignment(target) && isHeapSemType(e.value->inferredType)) {
        auto [parentObj, fieldAddr] = decomposeFieldAccess(target);
        // 泛型上下文：值类型是未实例化的模板参数（GenericSemType）时，
        // 编译期无法判断实例化后是标量还是 GC 指针，改用模板辅助函数
        // （实例化为标量时跳过写屏障，static_cast<GcObject*>(int) 非法）
        if (auto* gs = dynamic_cast<const GenericSemType*>(e.value->inferredType)) {
            if (gs->resolvedName.empty()) {
                return target + " = " + value + ";\n" + indentStr()
                     + "aura_rt::gc_write_barrier_generic(" + parentObj
                     + ", " + fieldAddr + ", " + value + ")";
            }
        }
        // G3：接口视图值（InterfaceSemType，isIfaceView）是值类型 {方法Fn, self}
        // 非指针 → static_cast<GcObject*>(value) 编译失败（方法体/构造体
        // `self.s = s`，s: Stringer/Greetable 视图）。视图的 GC 引用在 self 成员
        // （desc 已按 offsetof(Box,s)+offsetof(Stringer,self) 注册 self 子偏移）→
        // 写屏障取 fieldAddr.self / value.self。值本身是指针（如
        // Optional<Stringer>*）时 isIfaceView=false 走下方 static_cast 不变。
        // 仅当目标字段未被装箱成指针（union/Optional 目标已把视图 make_variant/
        // make_optional 成 Variant/Optional 指针，value 非视图，.self 对指针非法）
        // 时取 .self；目标类型缺失时视图值必非指针，.self 安全。
        if (isIfaceView(e.value->inferredType)) {
            bool boxedToPointer = false;
            if (e.target && e.target->inferredType) {
                if (dynamic_cast<const UnionSemType*>(e.target->inferredType))
                    boxedToPointer = true;
                else if (dynamic_cast<const OptionalSemType*>(e.target->inferredType))
                    boxedToPointer = true;
                else if (auto* gs = dynamic_cast<const GenericSemType*>(e.target->inferredType);
                         gs && gs->name == "Optional")
                    boxedToPointer = true;
            }
            if (!boxedToPointer) {
                return target + " = " + value + ";\n" + indentStr()
                     + "aura_rt::gc_write_barrier(" + parentObj
                     + ", &(" + target + ".self)"
                     + ", (" + value + ").self)";
            }
        }
        return target + " = " + value + ";\n" + indentStr()
             + "aura_rt::gc_write_barrier(" + parentObj
             + ", " + fieldAddr
             + ", static_cast<aura_rt::GcObject*>(" + value + "))";
    }

    return target + " = " + value;
}

// ============================================================
// 写屏障辅助方法
// ============================================================

bool CodeGenerator::isGcFieldAssignment(const std::string& target) const {
    // GC 对象字段访问使用 ->（如 obj.get()->field 或 this->field）
    // 局部变量和值类型访问使用 . 或不含 ->，不需要写屏障
    return target.find("->") != std::string::npos;
}

std::pair<std::string, std::string>
CodeGenerator::decomposeFieldAccess(const std::string& target) const {
    auto arrowPos = target.rfind("->");
    if (arrowPos == std::string::npos) {
        return {target, "&(" + target + ")"};
    }
    std::string parentObj = target.substr(0, arrowPos);
    std::string fieldAddr = "&(" + target + ")";
    return {parentObj, fieldAddr};
}

// ============================================================
// 错误传播 !  / 管道 |>
// ============================================================

std::string CodeGenerator::genErrorPropagation(const ErrorPropagationExpr& e,
                                                bool isCoroutine) {
    // plan §4.6: ! 操作符不生成额外代码
    // 异常自然传播，C++ 异常机制自动处理
    return genExpr(*e.expr, isCoroutine);
}

std::string CodeGenerator::genPipeExpr(const PipeExpr& e, bool isCoroutine) {
    // x |> f(y) → f(x, y) 或 f(x)
    // 简化：暂不支持，直接展开
    (void)e; (void)isCoroutine;
    return "/* pipe_expr */";
}

std::string CodeGenerator::genConditionalExpr(const ConditionalExpr& e, bool isCoroutine) {
    // 直接映射 C++ 三元：只求值选中的分支，整体为单表达式右值。
    // GC 安全：分配发生在选中分支内，外层由既有 let/实参/赋值上下文成根保护。
    //
    // P1-1 回归：三元内单分支 none() 的 inferredType 为 Optional<error>（无期望，
    // 元素推不出），其元素由另一分支统一（如 `true ? some(7) : none()` → Optional<int>，
    // Sema 经 isAssignable 兼容放行、不报错）。生成时用另一分支的 Optional 元素
    // 填充 currentReturnElem_，供 none() 分支回退取用，避免防御性报错误触发。
    std::string savedReturnElem = currentReturnElem_;
    const ASTNode* other = nullptr;
    if (isNoneCallExpr(*e.thenBranch)) other = e.elseBranch.get();
    else if (isNoneCallExpr(*e.elseBranch)) other = e.thenBranch.get();
    if (other && other->inferredType) {
        std::string elem = optionalElemCppName(other->inferredType);
        if (!elem.empty()) currentReturnElem_ = elem;
    }
    std::string result = "(" + genExpr(*e.cond, isCoroutine) + " ? "
               + genExpr(*e.thenBranch, isCoroutine) + " : "
               + genExpr(*e.elseBranch, isCoroutine) + ")";
    currentReturnElem_ = savedReturnElem;
    return result;
}

} // namespace Aura
