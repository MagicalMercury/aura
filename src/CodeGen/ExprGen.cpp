#include "CodeGen.h"
#include "../Sema/BuiltinRegistry.h"
#include "../Sema/SemType.h"
#include <cctype>
#include <sstream>

namespace Aura {

// ============================================================
// G1：形参 C++ 类型名辅助——Optional 元素 / Variant 变体列表提取。
// 形参 C++ 类型由 mapType 生成（与 funSignature/mapParamType 同源），已含 record '*'，
// 前缀判定只认 aura_rt::Optional< / aura_rt::Variant<，其余形参类型不命中、不装箱。
// ============================================================

// "aura_rt::Optional<X>*" → "X"（末个 '>' 定位，兼容嵌套尖括号与尾 '*'）
static std::string optionalElemFromParamCpp(const std::string& paramCpp) {
    static const std::string prefix = "aura_rt::Optional<";
    if (paramCpp.rfind(prefix, 0) != 0 || paramCpp.size() < prefix.size() + 2) return "";
    size_t rt = paramCpp.rfind('>');
    if (rt == std::string::npos || rt <= prefix.size()) return "";
    return paramCpp.substr(prefix.size(), rt - prefix.size());
}

// 按顶层逗号分割 C++ 模板参数列表（"aura_rt::Variant<A, B>*" → {A, B}，含嵌套尖括号），
// 并 trim 空白（形参 C++ 名中 "int32_t, aura_rt::GcString*" 的变体间有空格）
static std::vector<std::string> splitCppTemplateArgs(const std::string& s) {
    std::vector<std::string> parts;
    std::string cur;
    int depth = 0;
    for (char c : s) {
        if (c == '<' || c == '(') ++depth;
        else if (c == '>' || c == ')') --depth;
        else if (c == ',' && depth == 0) { parts.push_back(cur); cur.clear(); continue; }
        cur += c;
    }
    if (!cur.empty()) parts.push_back(cur);
    for (auto& p : parts) {
        auto b = p.find_first_not_of(" \t");
        auto e = p.find_last_not_of(" \t");
        p = (b == std::string::npos) ? "" : p.substr(b, e - b + 1);
    }
    return parts;
}

// G1：生成 Optional/Union 形参的实参装箱表达式。paramCpp 为形参 C++ 类型名：
//   - "aura_rt::Optional<X>*" → genOptionalTargetInit（some/none/已 Optional 直通，
//     裸值/record/列表 → make_optional<X>；条件分支逐分支装箱）
//   - "aura_rt::Variant<...>*" → genUnionBoxingImpl（已 Union 值 idx 匹配失败返回空，
//     自然防二次装箱）
// 非 Optional/Variant 形参或提取失败 → 返回 ""（不装箱）。
std::string CodeGenerator::genParamBoxing(const std::string& paramCpp,
                                          const ASTNode& arg,
                                          bool isCoroutine) {
    if (paramCpp.rfind("aura_rt::Optional<", 0) == 0
        && !paramCpp.empty() && paramCpp.back() == '*') {
        std::string elem = optionalElemFromParamCpp(paramCpp);
        if (!elem.empty())
            return genOptionalTargetInit(arg, elem, isCoroutine);
        return "";
    }
    if (paramCpp.rfind("aura_rt::Variant<", 0) == 0
        && !paramCpp.empty() && paramCpp.back() == '*') {
        const std::string prefix = "aura_rt::Variant<";
        // 去尾 ">*" 两字符后剩余变体列表（如 "int32_t, aura_rt::GcString*"）
        std::string inner = paramCpp.substr(prefix.size(),
                                            paramCpp.size() - prefix.size() - 2);
        std::vector<std::string> cppTypes = splitCppTemplateArgs(inner);
        if (!cppTypes.empty() && !cppTypes[0].empty())
            return genUnionBoxingImpl(cppTypes, arg, isCoroutine);
    }
    return "";
}

// G3：构造接口视图类型标记（record→view 转换后的实参类型）。仅供 genGcRootedArgs
// 的 isIfaceView/isHeapSemType 判定：视图是值类型（{Fn, self}），若按原 record 堆
// 指针类型传递，genGcRootedArgs 会生成 GcRootHandle<视图>——视图非指针，GcRootHandle
// 的 ptr_ref_ 指向视图首 8B（方法 Fn 指针）被 GC 当 GcObject* 扫描 → 坏根 → GC 扫描
// 崩溃。标记为视图后走值拷贝（非协程）/ ViewRoot（协程）分支。name 仅占位不做 mapType。
static std::unique_ptr<InterfaceSemType> makeIfaceViewMarker(const std::string& ifaceName) {
    auto v = std::make_unique<InterfaceSemType>();
    v->name = ifaceName;
    return v;
}

// ============================================================
// escapeStringLiteral — 转义字符串字面量内容，使其可安全嵌入生成的 C++ 源码
// 不转义则源串中的 "、\、\n、\t、\r 会破坏生成的 C++ 字符串字面量或被解释为控制字符
// ============================================================
static std::string escapeStringLiteral(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default:   out += c;
        }
    }
    return out;
}

// ============================================================
// isHeapSemType — 成员方法实现（原 file-static，提升为成员供 StmtGen 使用）
// ============================================================
bool CodeGenerator::isHeapSemType(const SemType* type) const {
    if (!type) return false;
    if (auto* p = dynamic_cast<const PrimSemType*>(type))
        return p->kind == PrimSemType::String;
    if (dynamic_cast<const NoneSemType*>(type)) return false;
    if (dynamic_cast<const ErrorSemType*>(type)) return false;
    // P1：接口语义类型按堆处理——接口值是"值视图 + 堆适配器"复合形态，视图含
    // GC 指针 self。接收者保护/字段赋值等场景需要 GC 参与决策；
    // 具体传参包装由 genGcRootedArgs 的视图排他判定（isIfaceView）另行排除。
    // std::function 是 C++ RT 值，不在 GC 堆中，恒非堆。
    if (dynamic_cast<const InterfaceSemType*>(type)) return true;
    if (dynamic_cast<const FuncSemType*>(type)) return false;
    // 内置 Iterator：值视图（{nextFn, self}，16B），非 GC 堆对象。
    // 视图不能被 GcRootHandle<View> 包裹（视图非指针，模板参数不成立），
    // self 由保守栈扫描 / 视图字段 desc 子偏移（genTypeDescriptor）保护。
    // 误判为 heap 会在 record 字段赋值/传参处生成 GcRootHandle<Iterator<T>> → 编译失败
    if (auto* g = dynamic_cast<const GenericSemType*>(type))
        if (g->name == "Iterator") return false;
    if (auto* u = dynamic_cast<const UnionSemType*>(type)) {
        for (auto& v : u->variants)
            if (isHeapSemType(v.get())) return true;
        return false;
    }
    return true;
}

// P1：视图类型判定（值视图 { 函数指针, self }，非 GC 堆对象）
//   - 内置 Iterator<T>（GenericSemType "Iterator"）
//   - 接口视图（InterfaceSemType：Stringer/Comparable/用户接口）
// 视图不能被 GcRootHandle<View> 包裹（视图非指针，模板参数不成立），
// 传参/包装时按非堆值处理，self 由保守栈扫描 / 视图字段 desc 子偏移保护。
bool CodeGenerator::isIfaceView(const SemType* t) const {
    if (!t) return false;
    if (auto* g = dynamic_cast<const GenericSemType*>(t))
        return g->name == "Iterator";
    if (dynamic_cast<const InterfaceSemType*>(t))
        return true;
    return false;
}

// 联合变体堆封装判定：isHeapSemType（堆对象）|| isIfaceView（视图含 self GC 指针）。
// 视图变体必须进 aura_rt::Variant<T...>*（descForI 子偏移扫描），
// 否则错误生成 std::variant → self 对 GC 不可见 → 悬垂崩溃。
bool CodeGenerator::isUnionHeapVariant(const SemType* t) const {
    return isHeapSemType(t) || isIfaceView(t);
}

// 回调包装辅助：SemType 是否"完全具体"（可安全映射为 C++ 类型，无未解析泛型/错误）。
// 泛型函数调用点（fnCallbackParams_ 仅对模板函数注册）的 ftStr 形如 std::function<T(T)>，
// 含未绑定泛型变量，在非泛型调用点无 T 作用域 → 生成代码编译失败；
// 实参推断的 FuncSemType（双向推断已将 T 代换为具体类型）可给出 std::function<int(int)>。
static bool semTypeIsConcrete(const SemType* t) {
    if (!t) return false;
    if (dynamic_cast<const ErrorSemType*>(t)) return false;
    if (auto* g = dynamic_cast<const GenericSemType*>(t))
        return !g->resolvedName.empty();   // 已实例化泛型（如 Iterator<int32_t>）视为具体
    if (auto* l = dynamic_cast<const ListSemType*>(t))
        return semTypeIsConcrete(l->elementType.get());
    if (auto* o = dynamic_cast<const OptionalSemType*>(t))
        return semTypeIsConcrete(o->elementType.get());
    if (auto* f = dynamic_cast<const FuncSemType*>(t)) {
        if (!semTypeIsConcrete(f->returnType.get())) return false;
        for (auto& p : f->paramTypes)
            if (!semTypeIsConcrete(p.get())) return false;
        return true;
    }
    return true;   // Prim/None/Record/Union/Interface → 具体
}

// ============================================================
// genGcRootedArgs — 为 GC 堆类型参数生成 IIFE + GcRootHandle 包装
// 所有参数都不是堆类型时，直接返回 callExpr（避免无意义 IIFE）
// ============================================================
std::string CodeGenerator::genGcRootedArgs(
    const std::vector<std::pair<std::string, const SemType*>>& args,
    const std::string& callExpr, bool /*isCoroutine*/)
{
    // co_await 不能放在 auto 返回类型 lambda 内部（C++20 限制）
    // 将 co_await 前缀移到 IIFE 外部：co_await [&]()->auto{ ... return expr; }()
    std::string awaitPrefix;
    std::string expr = callExpr;
    const std::string coAwaitKw = "co_await ";
    if (expr.compare(0, coAwaitKw.size(), coAwaitKw) == 0) {
        awaitPrefix = "co_await ";
        expr = expr.substr(coAwaitKw.size());
    }

    bool hasHeap = false;
    bool hasArgAwait = false;
    for (auto& [e, type] : args) {
        if (isHeapSemType(type) && !isIfaceView(type)) hasHeap = true;
        // Bug 2-C: 参数表达式顶层带 co_await（协程闭包调用作为实参）
        if (e.compare(0, coAwaitKw.size(), coAwaitKw) == 0) hasArgAwait = true;
    }
    if (!hasHeap && !hasArgAwait) {
        // 无堆类型参数且无 co_await 参数：直接替换占位符返回
        std::string result = expr;
        for (size_t i = 0; i < args.size(); ++i) {
            std::string placeholder = "{" + std::to_string(i) + "}";
            size_t pos = 0;
            while ((pos = result.find(placeholder, pos)) != std::string::npos) {
                result.replace(pos, placeholder.size(), args[i].first);
                pos += args[i].first.size();
            }
        }
        return awaitPrefix + result;
    }

    // 协程调用（awaitPrefix 非空）时，IIFE 返回 task 后局部变量立即析构，
    // 但协程是懒启动（initial_suspend=suspend_always），恢复执行时引用已析构的临时对象 → 悬垂。
    // 因此非堆参数必须在 IIFE 外声明（auto 值拷贝），生命周期跨越 co_await。
    // 堆参数仍留在 IIFE 内（配 GcRootHandle，GC compact 后自动更新指针）。
    int hid = argHandleCounter_++;
    std::ostringstream outer;  // 协程调用时，非堆参数声明到 IIFE 外
    std::ostringstream inner;  // IIFE 内：堆参数 + 非协程非堆参数
    inner << "[&]() -> auto {\n";
    for (size_t i = 0; i < args.size(); ++i) {
        auto& [argExpr, type] = args[i];
        std::string vi = "_a" + std::to_string(hid) + "_" + std::to_string(i);
        bool isHeap = isHeapSemType(type) && !isIfaceView(type);
        // Bug 2-C: 参数顶层 co_await 不能出现在推导返回类型 lambda（IIFE）内，
        // 绑定移到 IIFE 外（外层是协程函数体，co_await 合法）
        bool argIsAwait = argExpr.compare(0, coAwaitKw.size(), coAwaitKw) == 0;
        if (isHeap) {
            if (argIsAwait) {
                // 堆 + 协程参数：IIFE 外 auto 值拷贝 + GcRootHandle，生命周期跨越 co_await
                outer << "auto " << vi << " = (" << argExpr << ");\n";
                outer << "aura_rt::GcRootHandle<decltype(" << vi << ")> _h"
                      << hid << "_" << i << "(" << vi << ");\n";
            } else {
                // 堆类型：IIFE 内 auto + GcRootHandle
                inner << "    auto " << vi << " = (" << argExpr << ");\n";
                inner << "    aura_rt::GcRootHandle<decltype(" << vi << ")> _h"
                      << hid << "_" << i << "(" << vi << ");\n";
            }
        } else if (!awaitPrefix.empty() || argIsAwait) {
            if (isIfaceView(type)) {
                // 接口视图 + 协程调用：IIFE 外裸值拷贝会跨 co_await 挂起，
                // 挂起期间其他协程 GC compact 不重写协程帧内裸 self → 悬垂。
                // ViewRoot 包裹注册 self 为 GcRootHandle（ThreadLocal 根，compact 时重写），
                // 恢复后 .get() 重建视图取最新 self（与 P1 ViewRoot 机制一致）
                outer << "auto " << vi << "_raw = (" << argExpr << ");\n";
                outer << "aura_rt::ViewRoot<decltype(" << vi << "_raw)> " << vi
                      << "(" << vi << "_raw);\n";
            } else {
                // 非堆 + 协程调用（或参数含 co_await）：IIFE 外 auto 值拷贝，生命周期跨越 co_await
                outer << "auto " << vi << " = (" << argExpr << ");\n";
            }
        } else if (isIfaceView(type)) {
            // 接口视图（含 self 的值类型）：视图成员函数均非 const（默认方法体内
            // 调用非 const 转发成员），const auto& 绑定无法调用 → 值拷贝（~16-24B 可接受）
            inner << "    auto " << vi << " = (" << argExpr << ");\n";
        } else {
            // 非堆 + 非协程：IIFE 内 const auto& 引用绑定
            inner << "    const auto& " << vi << " = (" << argExpr << ");\n";
        }
    }
    inner << "    return " << expr << ";\n";
    inner << "  }()";
    // 替换 {0}, {1}, ... 为实际变量名
    // 堆类型参数：使用 _h{hid}_{i}.get() 读取 GcRootHandle 中可能被 GC 更新的指针
    // 非堆类型参数：使用原始变量 _a{hid}_{i}
    std::string result = outer.str() + awaitPrefix + inner.str();
    for (size_t i = 0; i < args.size(); ++i) {
        std::string placeholder = "{" + std::to_string(i) + "}";
        std::string repl;
        if (isHeapSemType(args[i].second) && !isIfaceView(args[i].second)) {
            repl = "_h" + std::to_string(hid) + "_" + std::to_string(i) + ".get()";
        } else if (isIfaceView(args[i].second)
                   && (!awaitPrefix.empty()
                       || args[i].first.compare(0, coAwaitKw.size(), coAwaitKw) == 0)) {
            // 视图 + 协程调用：outer 分支 ViewRoot 包裹 → .get() 重建视图取最新 self
            repl = "_a" + std::to_string(hid) + "_" + std::to_string(i) + ".get()";
        } else {
            repl = "_a" + std::to_string(hid) + "_" + std::to_string(i);
        }
        size_t pos = 0;
        while ((pos = result.find(placeholder, pos)) != std::string::npos) {
            result.replace(pos, placeholder.size(), repl);
            pos += repl.size();
        }
    }
    return result;
}

// ============================================================
// 表达式总调度
// ============================================================

std::string CodeGenerator::genExpr(const ASTNode& expr, bool isCoroutine) {
    if (auto* e = dynamic_cast<const IntLiteral*>(&expr))
        return genIntLiteral(*e);
    if (auto* e = dynamic_cast<const FloatLiteral*>(&expr))
        return genFloatLiteral(*e);
    if (auto* e = dynamic_cast<const StringLiteral*>(&expr))
        return genStringLiteral(*e);
    if (auto* e = dynamic_cast<const BoolLiteral*>(&expr))
        return genBoolLiteral(*e);
    if (dynamic_cast<const NoneLiteral*>(&expr))
        return genNoneLiteral();
    if (auto* e = dynamic_cast<const Identifier*>(&expr))
        return genIdentifier(*e);
    if (auto* e = dynamic_cast<const ListExpr*>(&expr))
        return genListExpr(*e, isCoroutine);
    if (auto* e = dynamic_cast<const RecordExpr*>(&expr))
        return genRecordExpr(*e, isCoroutine);
    if (auto* e = dynamic_cast<const BinaryExpr*>(&expr))
        return genBinaryExpr(*e, isCoroutine);
    if (auto* e = dynamic_cast<const UnaryExpr*>(&expr))
        return genUnaryExpr(*e, isCoroutine);
    if (auto* e = dynamic_cast<const CallExpr*>(&expr))
        return genCallExpr(*e, isCoroutine);
    if (auto* e = dynamic_cast<const MethodCallExpr*>(&expr))
        return genMethodCall(*e, isCoroutine);
    if (auto* e = dynamic_cast<const MemberAccessExpr*>(&expr))
        return genMemberAccess(*e);
    if (auto* e = dynamic_cast<const IndexExpr*>(&expr))
        return genIndexExpr(*e, isCoroutine);
    if (auto* e = dynamic_cast<const AssignExpr*>(&expr))
        return genAssignExpr(*e, isCoroutine);
    if (auto* e = dynamic_cast<const ErrorPropagationExpr*>(&expr))
        return genErrorPropagation(*e, isCoroutine);
    if (auto* e = dynamic_cast<const PipeExpr*>(&expr))
        return genPipeExpr(*e, isCoroutine);
    if (auto* e = dynamic_cast<const ConditionalExpr*>(&expr))
        return genConditionalExpr(*e, isCoroutine);
    if (auto* e = dynamic_cast<const FunExpr*>(&expr))
        return genFunExpr(*e, isCoroutine);
    return "/* ??? */";
}

// ============================================================
// 字面量
// ============================================================

std::string CodeGenerator::genIntLiteral(const IntLiteral& e) {
    return std::to_string(e.value);
}

std::string CodeGenerator::genFloatLiteral(const FloatLiteral& e) {
    return std::to_string(e.value);
}

std::string CodeGenerator::genStringLiteral(const StringLiteral& e) {
    return "aura_rt::intern_string(\"" + escapeStringLiteral(e.value) + "\")";
}

std::string CodeGenerator::genBoolLiteral(const BoolLiteral& e) {
    return e.value ? "true" : "false";
}

std::string CodeGenerator::genNoneLiteral() {
    return "aura_rt::None";
}

std::string CodeGenerator::genIdentifier(const Identifier& e) {
    // 方法/构造函数体内的接收者名（如 self, p）映射为 C++ 的 this
    if (!currentReceiverName_.empty() && e.name == currentReceiverName_)
        return "this";

    std::string name = safeName(e.name);

    // 已注册为 GcRootHandle 的变量 → 生成 .get() 解引用
    if (gcRootVarNames_.count(name)) {
        return name + ".get()";
    }

    // ViewRoot 包裹的视图变量（接口视图/迭代器视图）→ .get() 重建视图
    // （GC compact 后 self 由 ViewRoot 内 GcRootHandle 更新为新地址）
    if (viewRootVarNames_.count(name)) {
        return name + ".get()";
    }

    return name;
}

// ============================================================
// 列表 & 记录
// ============================================================

std::string CodeGenerator::genListExpr(const ListExpr& e, bool isCoroutine) {
    // #13：识别 Optional 元素列表（折叠 union `T|None` → OptionalSemType / 显式
    // `Optional<T>` 注解 → GenericSemType{name=="Optional"}）。元素 C++ 名经
    // optionalElemCppName 提取（record 元素统一补 *），列表元素类型为
    // aura_rt::Optional<elemCpp>*；listElemCpp 为空 = 非 Optional 元素（维持原逻辑）。
    std::string listElemCpp;
    if (e.inferredType) {
        if (auto* lt = dynamic_cast<const ListSemType*>(e.inferredType)) {
            if (lt->elementType) {
                const SemType* et = lt->elementType.get();
                bool isOpt = dynamic_cast<const OptionalSemType*>(et)
                    || (dynamic_cast<const GenericSemType*>(et)
                        && static_cast<const GenericSemType*>(et)->name == "Optional");
                if (isOpt) listElemCpp = optionalElemCppName(et);
            }
        }
    }

    // #7：接口视图元素列表（[Stringer]/[Comparable<Point>]/[Iterator<int>]）——
    // 元素是值视图 { 方法Fn..., GcObject* self }（非 GC 指针）：① 元素为 record 时
    // 需 record→view 转换（genRecordToViewIIFE），否则 Array<Stringer>::append 直传
    // record 指针类型不匹配；② append 的是视图值，不能 GcRootHandle（视图非指针）。
    // runtime 侧 ArrayChunk<Stringer>::desc() 已注册 self 子偏移（#7 方案 A）支撑 GC 追踪。
    bool elemIsIfaceView = false;
    std::string elemViewCpp;
    if (listElemCpp.empty() && e.inferredType) {
        if (auto* lt = dynamic_cast<const ListSemType*>(e.inferredType)) {
            if (lt->elementType) {
                std::string etCpp = mapSemType(*lt->elementType);
                if (isIfaceViewTypeName(etCpp)) {
                    elemIsIfaceView = true;
                    elemViewCpp = etCpp;
                }
            }
        }
    }

    if (e.elements.empty()) {
        // 优先用 inferredType 推断空列表元素类型
        if (e.inferredType) {
            auto* listTy = dynamic_cast<const ListSemType*>(e.inferredType);
            if (listTy && listTy->elementType) {
                // #13：Optional 元素列表 → aura_rt::Optional<elemCpp>*（optionalElemCppName
                // 已补 record 元素 *，顺带修 B/L 显式 Optional 元素缺 *）
                std::string semElem = !listElemCpp.empty()
                    ? "aura_rt::Optional<" + listElemCpp + ">*"
                    : mapSemType(*listTy->elementType);
                if (semElem.find("GcObject") == std::string::npos
                    && semElem != "auto"
                    && semElem.find("auto") == std::string::npos)   // G4：拦截 std::function<auto(...)>
                    return "aura_rt::Array<" + semElem + ">::make(0)";
            }
        }
        // 泛型上下文中的空列表：用第一个模板参数
        if (!currentTParams_.empty()) {
            return "aura_rt::Array<" + currentTParams_[0] + ">::make(0)";
        }
        return "/* empty list - element type unknown */ nullptr";
    }

    // 生成所有元素表达式
    std::vector<std::string> elemExprs;
    for (auto& elem : e.elements) {
        // #13：Optional 元素列表——元素值经 genOptionalTargetInit 按目标元素 C++ 类型
        // 装箱：some(arg)/none()/已 Optional 值（Optional 变量、返回 Optional 的调用）直通，
        // 裸值（record 字面量等）make_optional<elemCpp> 装箱；非 Optional 元素保持 genExpr。
        // #7：接口视图元素且值为 record → record→view 转换（否则 Array<视图>::append
        // 直传 record 指针类型不匹配）；元素已是视图值（range 产 Iterator 等）直用。
        if (elem) {
            if (!listElemCpp.empty()) {
                elemExprs.push_back(genOptionalTargetInit(*elem, listElemCpp, isCoroutine));
            } else if (elemIsIfaceView
                       && dynamic_cast<const RecordSemType*>(elem->inferredType)
                       && !static_cast<const RecordSemType*>(elem->inferredType)->canonicalName.empty()) {
                auto* rt = static_cast<const RecordSemType*>(elem->inferredType);
                elemExprs.push_back(genRecordToViewIIFE(genExpr(*elem, isCoroutine),
                                                        rt->canonicalName, elemViewCpp));
            } else {
                elemExprs.push_back(genExpr(*elem, isCoroutine));
            }
        } else {
            elemExprs.push_back("???");
        }
    }

    // 从第一个元素推断列表元素类型
    std::string elemType = "int32_t";  // 默认 int

    // 优先用 SemAnalyzer 推断的类型
    if (e.inferredType) {
        auto* listTy = dynamic_cast<const ListSemType*>(e.inferredType);
        if (listTy && listTy->elementType) {
            // #13：Optional 元素列表 → aura_rt::Optional<elemCpp>*（optionalElemCppName
            // 已补 record 元素 *，顺带修 B/L 显式 Optional 元素缺 *）；非 Optional 走 mapSemType
            std::string semElemType = !listElemCpp.empty()
                ? "aura_rt::Optional<" + listElemCpp + ">*"
                : mapSemType(*listTy->elementType);
            // 无效时（GenericSemType → "auto"），回退到第一个元素的 inferredType
            if (semElemType == "auto" && e.elements.size() > 0 && e.elements[0]) {
                if (auto* rs = dynamic_cast<const RecordSemType*>(e.elements[0]->inferredType)) {
                    if (!rs->canonicalName.empty())
                        semElemType = rs->canonicalName + "*";
                }
            }
            if (semElemType.find("GcObject") == std::string::npos
                && semElemType.find("/*") == std::string::npos
                && semElemType != "auto"
                && semElemType.find("auto") == std::string::npos) {   // G4：拦截 std::function<auto(...)>
                elemType = semElemType;
            } else if (e.elements.size() > 0 && e.elements[0]
                       && dynamic_cast<const RecordExpr*>(e.elements[0].get())) {
                // A3：record 元素类型不可知（Sema 未解析出 record 类型名，即列表元素
                // 是匿名 record 且无匹配声明）→ 报干净错误而非静默退化为 int32_t/GcObject
                // （否则在 C++ 侧产生 Array<int32_t> 与 record 构造之间的模板类型错误）
                error(e, "cannot infer element type of list literal; add an explicit type annotation (e.g. let e: [Point] = [...])");
            }
        }
    }

    // 文本启发式（SemType 未得到有意义类型时）
    if (elemType == "int32_t") {
        const auto& first = elemExprs[0];

        // 检测 FunExpr（闭包）→ 转为 std::function
        if (e.elements[0] && dynamic_cast<const FunExpr*>(e.elements[0].get())) {
            auto* fe = static_cast<const FunExpr*>(e.elements[0].get());
            std::string retType = fe->returnType ? mapType(*fe->returnType) : "auto";
            std::string params;
            for (size_t j = 0; j < fe->params.size(); ++j) {
                if (j > 0) params += ", ";
                params += fe->params[j].type ? mapType(*fe->params[j].type) : "auto";
            }
            elemType = "std::function<" + retType + "(" + params + ")>";
        }
        // 检测 RecordExpr → 用对应的注册堆类型名
        else if (auto* rec = dynamic_cast<const RecordExpr*>(e.elements[0].get())) {
            if (!rec->fields.empty()) {
                std::string firstFieldType = rec->fields[0].value
                    ? genExpr(*rec->fields[0].value, false) : "";
                if (firstFieldType.find("aura_rt::make_string") != std::string::npos)
                    elemType = "aura_rt::GcString*";
                else if (firstFieldType == "true" || firstFieldType == "false")
                    elemType = "bool";
            }
        } else if (first.find("aura_rt::make_string") != std::string::npos
            || first.find("aura_rt::concat") != std::string::npos)
            elemType = "aura_rt::GcString*";
        else if (first == "aura_rt::None")
            elemType = "aura_rt::NoneType";
        else if (first == "true" || first == "false")
            elemType = "bool";
        else if (first.find('.') != std::string::npos
                 && first.find("aura_rt::") == std::string::npos)
            elemType = "double";
    }

    // 生成唯一的列表临时变量名
    int idx = listCounter_++;
    std::string var = "_list_" + std::to_string(idx);

    // IIFE：将多行语句包装为单个表达式
    std::ostringstream oss;
    oss << "[&]() -> aura_rt::Array<" << elemType << ">* {\n";
    oss << "    auto* _raw = aura_rt::Array<" << elemType
        << ">::make(" << e.elements.size() << ");\n";
    oss << "    aura_rt::GcRootHandle<decltype(_raw)> " << var << "(_raw);\n";
    // 每个元素：预求值并用 GcRootHandle 保护（防止 append 内部 alloc 触发 GC 回收临时值）
    // append 内部 ArrayChunk::make 会触发 GC，未保护的临时 GcString* 会被 mark-sweep 回收
    for (size_t i = 0; i < elemExprs.size(); ++i) {
        // #13：Optional 元素列表的元素均为堆指针（aura_rt::Optional<elemCpp>*，含
        // make_optional 装箱结果 / some/none 直通值）→ 一律 GcRootHandle 保护，
        // 防 append 内部 alloc 触发 GC 回收未保护临时值
        // #7：接口视图元素是值类型（含 GcObject* self，非指针）→ 跳过 GcRootHandle
        //（GcRootHandle<视图> 模板参数不成立会编译失败）；self 由 ArrayChunk desc
        // 子偏移（mark+compact）与保守栈扫描保护
        bool isHeap = e.elements[i] && (!listElemCpp.empty()
            || (!elemIsIfaceView && isHeapSemType(e.elements[i]->inferredType)));
        std::string vi = "_e" + std::to_string(idx) + "_" + std::to_string(i);
        oss << "    auto " << vi << " = (" << elemExprs[i] << ");\n";
        if (isHeap) {
            oss << "    aura_rt::GcRootHandle<decltype(" << vi << ")> _eh"
                << idx << "_" << i << "(" << vi << ");\n";
            oss << "    " << var << ".get()->append(_eh" << idx << "_" << i << ".get());\n";
        } else {
            oss << "    " << var << ".get()->append(" << vi << ");\n";
        }
    }
    oss << "    return " << var << ".get();\n";
    oss << "  }()";
    return oss.str();
}

std::string CodeGenerator::genRecordExpr(const RecordExpr& e, bool isCoroutine) {
    // 堆记录类型：用 gc_alloc + IIFE 生成完整堆对象（替代 designated initializer）
    auto getCanonical = [&]() -> std::string {
        if (auto* rs = dynamic_cast<const RecordSemType*>(e.inferredType)) {
            if (!rs->canonicalName.empty()) return rs->canonicalName;
        }
        if (auto* gs = dynamic_cast<const GenericSemType*>(e.inferredType)) {
            if (!gs->resolvedName.empty()) {
                // #2：显式 Optional<X> 上下文（GenericSemType{name=="Optional"}）下
                // 的 record 字面量：propagateCanonicalName 的 GenericSemType 分支把
                // inferredType 物化为 Optional 包裹，需从 resolvedName 提取元素
                // C++ 名作为记录类型（如 "aura_rt::Optional<Point>" → "Point"）
                if (gs->name == "Optional") {
                    size_t lt = gs->resolvedName.find('<');
                    size_t rt = gs->resolvedName.rfind('>');
                    if (lt != std::string::npos && rt != std::string::npos && rt > lt) {
                        std::string elem = gs->resolvedName.substr(lt + 1, rt - lt - 1);
                        if (elem.size() > 1 && elem.back() == '*') elem.pop_back();
                        return elem;
                    }
                }
                return gs->resolvedName;
            }
        }
        return "";
    };
    std::string recType = getCanonical();
    if (!recType.empty()) {
        int idx = recordAllocCounter_++;
        std::string var = "_rec_" + std::to_string(idx);
        std::ostringstream oss;
        oss << "[&]() -> " << recType << "* {\n";
        oss << "    auto* _raw = aura_rt::gc_alloc<" << recType
            << ">(&" << recType << "::_desc);\n";
        oss << "    aura_rt::GcRootHandle<decltype(_raw)> " << var << "(_raw);\n";
        for (auto& f : e.fields) {
            // 堆类型字段值：预求值并用 GcRootHandle 保护
            // #10：按字段声明类型（e.inferredType->fields）装箱（Optional/Variant 字段）
            // #3：接口视图字段（值类型）→ outViewValue 置 true，跳过 GcRootHandle
            bool isViewField = false;
            std::string fval = f.value
                ? genRecordFieldValue(e.inferredType, *f.value, f.name, isCoroutine, &isViewField) : "???";
            if (f.value && isHeapSemType(f.value->inferredType) && !isViewField) {
                oss << "    auto _fv_" << safeName(f.name) << " = (" << fval << ");\n";
                oss << "    aura_rt::GcRootHandle<decltype(_fv_" << safeName(f.name)
                    << ")> _fh_" << safeName(f.name) << "(_fv_" << safeName(f.name) << ");\n";
                oss << "    " << var << ".get()->" << safeName(f.name)
                    << " = _fh_" << safeName(f.name) << ".get();\n";
            } else {
                oss << "    " << var << ".get()->" << safeName(f.name)
                    << " = " << fval << ";\n";
            }
        }
        oss << "    return " << var << ".get();\n";
        oss << "  }()";
        return oss.str();
    }

    // 匿名记录：保持 designated initializer
    std::ostringstream oss;
    oss << "{";
    for (size_t i = 0; i < e.fields.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << "." << e.fields[i].name << " = "
            << (e.fields[i].value ? genExpr(*e.fields[i].value, isCoroutine) : "???");
    }
    oss << "}";
    return oss.str();
}

// ============================================================
// 链式 + 收集 → concat_multi 脱糖
// ============================================================

std::vector<std::string> CodeGenerator::collectStringChain(const BinaryExpr& e,
                                                           bool isCoroutine) {
    std::vector<std::string> parts;

    // 递归左子树：仅当左子是 BinaryExpr(+) 时继续收集
    if (auto* leftBin = dynamic_cast<const BinaryExpr*>(e.left.get())) {
        if (leftBin->op == "+") {
            auto sub = collectStringChain(*leftBin, isCoroutine);
            if (sub.empty()) return {};
            parts.insert(parts.end(), sub.begin(), sub.end());
        } else {
            return {};
        }
    } else {
        // 叶子节点：直接收集（不验证类型）
        parts.push_back(genExpr(*e.left, isCoroutine));
    }

    // 右子节点：直接收集（不验证类型）
    // 类型判定延迟到 genBinaryExpr 生成 concat_multi 时处理
    parts.push_back(genExpr(*e.right, isCoroutine));
    return parts;
}

bool CodeGenerator::isStringExprInChain(const std::string& s) const {
    if (s.find("aura_rt::make_string") != std::string::npos
        || s.find("aura_rt::intern_string") != std::string::npos
        || s.find("->to_string") != std::string::npos
        || s.find(".to_string") != std::string::npos
        || s.find("aura_rt::concat") != std::string::npos
        || s.find("aura_rt::string_concat") != std::string::npos
        || s.find("aura_rt::concat_multi") != std::string::npos
        || s.find("aura_rt::string_of") != std::string::npos) {
        return true;
    }
    auto stripGet = [](const std::string& in) -> std::string {
        if (in.size() > 6 && in.substr(in.size() - 6) == ".get()")
            return in.substr(0, in.size() - 6);
        return in;
    };
    return stringVarNames_.count(stripGet(s)) > 0;
}

// ============================================================
// 二元 / 一元
// ============================================================

std::string CodeGenerator::genBinaryExpr(const BinaryExpr& e, bool isCoroutine) {
    std::string left  = genExpr(*e.left, isCoroutine);
    std::string right = genExpr(*e.right, isCoroutine);

    // C5b: 比较符号 → Comparable 接口分发
    // 判定"record 实现 Comparable"查组合收集 interfaceImplementations_（含 "Comparable"），
    // 不用 inferredType 标记（比较推断为 boolType，无法携带）
    // 生成 <recName>Comparable({0}).less(<recName>Comparable({1}))（虚调用，尊重 override）
    if (e.op == "<" || e.op == "<=" || e.op == ">" || e.op == ">="
        || e.op == "==" || e.op == "!=") {
        auto* lt = dynamic_cast<const RecordSemType*>(e.left->inferredType);
        auto* rt = dynamic_cast<const RecordSemType*>(e.right->inferredType);
        if (lt && rt && !lt->canonicalName.empty()
            && lt->canonicalName == rt->canonicalName) {
            auto recIt = interfaceImplementations_.find(lt->canonicalName);
            if (recIt != interfaceImplementations_.end()
                && recIt->second.count("Comparable") > 0) {
                static const std::map<std::string, std::string> kOpToMethod = {
                    {"<",  "less"}, {"<=", "le"}, {">", "greater"}, {">=", "ge"},
                    {"==", "equal"}, {"!=", "ne"},
                };
                std::string adapter = safeName(lt->canonicalName) + "Comparable";
                std::string method = kOpToMethod.at(e.op);
                std::vector<std::pair<std::string, const SemType*>> cmpArgs;
                cmpArgs.emplace_back(left, e.left->inferredType);
                cmpArgs.emplace_back(right, e.right->inferredType);
                // P1 视图分派：gcConstruct 分配堆适配器（record 指针 {0} 已由
                // genGcRootedArgs 保护，gcConstruct 内 alloc 触发 GC 安全），
                // view() 构造视图，第二个操作数直接传 record 指针（视图方法参数 T=record*）
                std::string callExpr = adapter + "::view(aura_rt::gcConstruct<"
                    + adapter + ">(&" + adapter + "::desc(), {0}))." + method + "({1})";
                return genGcRootedArgs(cmpArgs, callExpr, isCoroutine);
            }
        }
    }

    // 字符串拼接：检测左操作数是否为 GcString*/make_string
    // 使用 aura_rt::concat 代替 string_concat，利用 C++ 重载决议自动处理
    // string + int / int + string / float + string 等组合
    if (e.op == "+") {
        bool leftIsStr  = left.find("aura_rt::make_string") != std::string::npos
                       || left.find("aura_rt::intern_string") != std::string::npos
                       || left.find("->to_string") != std::string::npos
                       || left.find(".to_string") != std::string::npos
                       || left.find("aura_rt::concat") != std::string::npos
                       || left.find("aura_rt::string_concat") != std::string::npos
                       || left.find("aura_rt::string_of") != std::string::npos;
        bool rightIsStr = right.find("aura_rt::make_string") != std::string::npos
                       || right.find("aura_rt::intern_string") != std::string::npos
                       || right.find("->to_string") != std::string::npos
                       || right.find(".to_string") != std::string::npos
                       || right.find("aura_rt::concat") != std::string::npos
                       || right.find("aura_rt::string_concat") != std::string::npos
                       || right.find("aura_rt::string_of") != std::string::npos;

        // 也检测已知字符串类型变量（含 GcRootHandle 包装后的 name.get()）
        auto stripGet = [](const std::string& s) -> std::string {
            if (s.size() > 6 && s.substr(s.size() - 6) == ".get()")
                return s.substr(0, s.size() - 6);
            return s;
        };
        if (!leftIsStr && stringVarNames_.count(stripGet(left))) leftIsStr = true;
        if (!rightIsStr && stringVarNames_.count(stripGet(right))) rightIsStr = true;

        // 兜底：基于 Sema 推断类型识别 string（最可靠）
        // 覆盖从函数参数、字段赋值等路径流入的 string 变量，
        // 这类变量 init 不含 make_string/concat 子串，substring 匹配会漏判。
        auto isStringSemType = [](const SemType* type) -> bool {
            if (!type) return false;
            if (auto* p = dynamic_cast<const PrimSemType*>(type))
                return p->kind == PrimSemType::String;
            return false;
        };
        if (!leftIsStr && isStringSemType(e.left->inferredType)) leftIsStr = true;
        if (!rightIsStr && isStringSemType(e.right->inferredType)) rightIsStr = true;

        // 链式 + 脱糖为 concat_multi（链长 ≥ 3 且链根为 string 时）
        // 放宽触发条件：leftIsStr || rightIsStr（链中可含 int/bool/double）
        // 非 string 节点用 GcString::from 包装
        if (leftIsStr || rightIsStr) {
            auto chain = collectStringChain(e, isCoroutine);
            if (chain.size() >= 3 && isStringExprInChain(chain[0])) {
                // 用 IIFE + GcRootHandle 包裹每个参数，compact 移动对象后自动更新指针
                int hid = argHandleCounter_++;
                std::string result = "[&](){";
                for (size_t i = 0; i < chain.size(); ++i) {
                    std::string expr = isStringExprInChain(chain[i])
                                       ? chain[i]
                                       : "aura_rt::GcString::from(" + chain[i] + ")";
                    result += "auto _a" + std::to_string(hid) + "_" + std::to_string(i)
                            + " = " + expr + ";";
                    result += "aura_rt::GcRootHandle<aura_rt::GcString*> _h"
                            + std::to_string(hid) + "_" + std::to_string(i)
                            + "(_a" + std::to_string(hid) + "_" + std::to_string(i) + ");";
                }
                result += "return aura_rt::concat_multi({";
                for (size_t i = 0; i < chain.size(); ++i) {
                    if (i) result += ", ";
                    result += "_h" + std::to_string(hid) + "_" + std::to_string(i) + ".get()";
                }
                result += "}); }()";
                return result;
            }
        }

        if (leftIsStr || rightIsStr) {
            // 链长=2 也用 GcRootHandle 保护
            std::vector<std::pair<std::string, const SemType*>> gcArgs2;
            gcArgs2.emplace_back(left, e.left->inferredType);
            gcArgs2.emplace_back(right, e.right->inferredType);
            return genGcRootedArgs(gcArgs2, "aura_rt::concat({0}, {1})", isCoroutine);
        }
    }

    // 字符串值比较：== / != 用于 GcString* 时需要用 string_eq 而不是指针比较
    if (e.op == "==" || e.op == "!=") {
        bool leftIsStr  = left.find("aura_rt::make_string") != std::string::npos
                       || left.find("aura_rt::intern_string") != std::string::npos
                       || left.find("aura_rt::concat") != std::string::npos
                       || left.find("aura_rt::string_concat") != std::string::npos
                       || left.find("aura_rt::string_of") != std::string::npos;
        bool rightIsStr = right.find("aura_rt::make_string") != std::string::npos
                       || right.find("aura_rt::intern_string") != std::string::npos
                       || right.find("aura_rt::concat") != std::string::npos
                       || right.find("aura_rt::string_concat") != std::string::npos
                       || right.find("aura_rt::string_of") != std::string::npos;
        if (leftIsStr || rightIsStr) {
            std::vector<std::pair<std::string, const SemType*>> gcArgs;
            gcArgs.emplace_back(left, e.left->inferredType);
            gcArgs.emplace_back(right, e.right->inferredType);
            std::string eq = genGcRootedArgs(gcArgs, "aura_rt::string_eq({0}, {1})", isCoroutine);
            return e.op == "!=" ? ("!" + eq) : eq;
        }
    }

    // 逻辑运算符映射
    std::string op = e.op;
    if (op == "and") op = "&&";
    if (op == "or")  op = "||";
    if (op == "not") op = "!";

    return "(" + left + " " + op + " " + right + ")";
}

std::string CodeGenerator::genUnaryExpr(const UnaryExpr& e, bool isCoroutine) {
    std::string operand = genExpr(*e.operand, isCoroutine);
    if (e.op == "not") return "!(" + operand + ")";
    return "(" + e.op + operand + ")";
}

// ============================================================
// 调用
// ============================================================

std::string CodeGenerator::genCallExpr(const CallExpr& e, bool isCoroutine) {
    std::string calleeName;
    if (auto* id = dynamic_cast<const Identifier*>(e.callee.get()))
        calleeName = id->name;

    // channel 构造函数特殊处理：channel(cap) → new Channel<T>(cap)
    if (calleeName == "channel") {
        std::string targ = expectedTemplateArgs_.empty() ? "int32_t" : expectedTemplateArgs_[0];
        std::string cap = e.args.empty() ? "0" : genExpr(*e.args[0], isCoroutine);
        return "(new aura_rt::Channel<" + targ + ">(" + cap + "))";
    }

    // some(v)/none()：Optional 构造（C++ CTAD 推导 T）
    if (calleeName == "some" && e.args.size() == 1) {
        // #1：显式 Optional<X> 目标下的 some()（genOptionalTargetInit 设置
        // optionalTargetElem_，覆盖条件表达式/传参等非 let 形态）→ 按目标元素
        // 装箱（接口视图+record → record→view；其余显式模板参数）；
        // 无目标元素 → 保持 CTAD（普通 some() 行为不变）。嵌套 some() 时
        // optionalTargetElem_ 已被调用方临时清空（防二次装箱）。
        if (!optionalTargetElem_.empty()) {
            std::string elem = optionalTargetElem_;
            optionalTargetElem_.clear();   // 实参内再嵌套 some() → 恢复 CTAD
            std::string result = genOptionalBoxByElem(elem, *e.args[0], isCoroutine);
            optionalTargetElem_ = elem;
            return result;
        }
        return "aura_rt::make_optional(" + genExpr(*e.args[0], isCoroutine) + ")";
    }
    if (calleeName == "none" && e.args.empty()) {
        // none() 元素类型取用优先级（P1-1/A1）：
        //   1. none() 调用自身的 inferredType —— Sema 在期望上下文（let 注解 / 赋值目标 /
        //      传参形参）已把正确元素反推并挂上（见 ExprInfer none() 分支），覆盖
        //      `let x: Optional<T> = none()` 等形态；元素为 Error（真未知）时不取
        //   2. currentReturnElem_ —— 函数/闭包返回 Optional<T> 上下文填充（return 形态）
        //   3. 都不可得 → 防御性报错（真未知已被 Sema containsErrorElement 拦截，
        //      若仍可达说明回归，报干净错误而非静默伪装成 int32_t）
        std::string elem = optionalElemCppName(e.inferredType);
        if (elem.empty()) elem = currentReturnElem_;
        if (elem.empty()) {
            error(e, "cannot infer element type for none(); add an explicit type annotation");
            elem = "int32_t";  // 占位；driver 检测到 codegen 错误后不会调用 g++
        }
        return "aura_rt::make_none<" + elem + ">()";
    }

    // range(...) → make_range（iota_view）；for-in 的 range 仍走 iota 特判（性能路径）
    if (calleeName == "range") {
        std::vector<std::string> a;
        for (auto& arg : e.args) a.push_back(genExpr(*arg, isCoroutine));
        if (a.size() == 1) return "aura_rt::make_range<int32_t>(0, " + a[0] + ")";
        if (a.size() == 2) return "aura_rt::make_range<int32_t>(" + a[0] + ", " + a[1] + ")";
        if (a.size() == 3) return "aura_rt::make_range<int32_t>(" + a[0] + ", " + a[1] + ", " + a[2] + ")";
        return "aura_rt::make_range<int32_t>(0, 0)";
    }

    // GC 内建函数：gc_force() / gc_stats()
    if (calleeName == "gc_force" && e.args.empty()) {
        return "aura_rt::gc_force_major()";
    }
    if (calleeName == "gc_stats" && e.args.empty()) {
        return "aura_rt::gc_stats_string()";
    }

    // C5.2: 全局内置函数映射（int/float/str 是 C++ 关键字，必须在 safeName 前拦截）
    static const std::map<std::string, std::string> kGlobalFnMap = {
        {"int",   "aura_rt::string_to_int"},
        {"float", "aura_rt::string_to_float"},
        {"str",   "aura_rt::string_of"},
    };
    if (auto gmap = kGlobalFnMap.find(calleeName); gmap != kGlobalFnMap.end()) {
        std::vector<std::string> argExprs;
        for (size_t i = 0; i < e.args.size(); ++i)
            argExprs.push_back(genExpr(*e.args[i], isCoroutine));
        // C5.6: str(obj) 衔接 Stringer：实参 record 实现 Stringer → obj->to_string()
        // 判定用组合收集集合 interfaceImplementations_（与 C3.2 同一数据源）
        if (calleeName == "str" && e.args.size() == 1 && e.args[0]->inferredType) {
            if (auto* rt = dynamic_cast<const RecordSemType*>(e.args[0]->inferredType)) {
                std::string recName = rt->canonicalName;
                auto recIt = interfaceImplementations_.find(recName);
                if (recIt != interfaceImplementations_.end()
                    && recIt->second.count("Stringer") > 0) {
                    std::string adName = safeName(recName) + "Stringer";
                    std::vector<std::pair<std::string, const SemType*>> stArgs;
                    stArgs.emplace_back(argExprs[0], e.args[0]->inferredType);
                    // P1 视图分派：同比较运算符，适配器由 gcConstruct 分配（{0} 已保护），
                    // view().to_string() 转发到 owner->to_string()
                    std::string callExpr = adName + "::view(aura_rt::gcConstruct<"
                        + adName + ">(&" + adName + "::desc(), {0})).to_string()";
                    return genGcRootedArgs(stArgs, callExpr, isCoroutine);
                }
            }
        }
        // 内置默认参数补齐：int 的 base=10（defaultCount 驱动，v1 生成字面量）
        if (auto* fn = BuiltinRegistry::get().findFunction(calleeName, (int)e.args.size())) {
            for (size_t i = argExprs.size(); i < fn->params.size(); ++i)
                argExprs.push_back("10");
        }
        std::string callExpr = gmap->second + "(";
        for (size_t i = 0; i < argExprs.size(); ++i) { if (i > 0) callExpr += ", "; callExpr += "{" + std::to_string(i) + "}"; }
        callExpr += ")";
        // 统一走 genGcRootedArgs 保护（string 参数为堆类型）
        std::vector<std::pair<std::string, const SemType*>> gcArgs;
        for (size_t i = 0; i < argExprs.size(); ++i)
            gcArgs.emplace_back(argExprs[i], i < e.args.size() ? e.args[i]->inferredType : nullptr);
        return genGcRootedArgs(gcArgs, callExpr, isCoroutine);
    }

    bool isCtor = false;
    bool hasUserCtor = false;

    if (!calleeName.empty() && registeredTypes_.count(calleeName) && registeredTypes_[calleeName]) {
        isCtor = true;
        // 检查是否有用户定义的构造函数
        for (auto& d : pendingMethods_) {
            if (d.receiverType == calleeName && d.methodName.empty())  // won't match
                hasUserCtor = true;
        }
        // 暂检查 _ctor 是否存在 — 对于没有构造函数的类型，内联 gc_alloc
        // 简化：总是使用 _ctor（编译器为无自定构造函数的类型生成默认 _ctor）
        // 无用户构造函数 → 生成默认分配
        (void)hasUserCtor;
    }

    std::string calleeExpr = isCtor ? (calleeName + "_ctor") : genExpr(*e.callee, isCoroutine);

    // 泛型构造函数模板参数：
    // ① N2 调用点显式类型实参 B<int>(...) 最优先（targs 来源=显式实参，mapType 映射
    //    int→int32_t、Point→Point*，与声明侧 B_ctor<T> 的 T 一致）；
    // ② 其次用 let/const 类型标注的显式模板实参（expectedTemplateArgs_，genLetStmt
    //    从 `let b: B<int>` 填充）——覆盖"构造形参不含 receiver 泛型 T"的有参/零参
    //    构造（B_ctor<int32_t>(closure)）：collectMethodTParams 使 B_ctor 模板化
    //    （T 来自 receiverTypeArgs），T 不出现在 C++ 形参时 g++ 无法推导，必须显式给出；
    // ③ 无标注时零参构造退回 currentTParams_（当前模板上下文，如泛型函数内 Stack()）；
    //    有参构造让 CTAD 从形参推导（如 Pair(p.second, p.first) → Pair_ctor(B, A)）
    //    ——不能对有参构造用 currentTParams_（是外层函数模板参数，非 receiver 泛型）。
    std::string targs;
    if (isCtor && !e.typeArgs.empty()) {
        targs = "<";
        for (size_t i = 0; i < e.typeArgs.size(); ++i) {
            if (i > 0) targs += ", ";
            targs += e.typeArgs[i] ? mapType(*e.typeArgs[i]) : "int32_t";
        }
        targs += ">";
    } else if (isCtor && !expectedTemplateArgs_.empty()) {
        targs = "<";
        for (size_t i = 0; i < expectedTemplateArgs_.size(); ++i) {
            if (i > 0) targs += ", ";
            targs += expectedTemplateArgs_[i];
        }
        targs += ">";
    } else if (isCtor && e.args.empty() && !currentTParams_.empty()) {
        targs = "<";
        for (size_t i = 0; i < currentTParams_.size(); ++i) {
            if (i > 0) targs += ", ";
            targs += currentTParams_[i];
        }
        targs += ">";
    }

    bool needAwait = false;
    if (isCoroutine && !isCtor) {
        needAwait = coroutineFunctions_.count(calleeExpr) > 0
                 || coroClosureNames_.count(calleeExpr) > 0;
    }

    // 接口参数自动包装（双源：具体 record → 适配器；闭包 → IfaceFunc；接口变量 → 透传）
    // 透传判定完全基于 inferredType（InterfaceSemType），不用 arg 字符串 find 判断
    // ——record 名含接口名子串（如 GreetableUser）或 inferredType 缺失时都会误判
    auto ipIt = fnInterfaceParams_.find(calleeName);
    auto cbIt = fnCallbackParams_.find(calleeName);
    std::vector<std::string> argExprs;
    // G3：被 record→view / XFunc 转换的实参 idx → 视图类型标记（genGcRootedArgs
    // 据此走视图值分支，不生成 GcRootHandle<视图> 坏根；见 makeIfaceViewMarker）
    std::map<size_t, std::unique_ptr<SemType>> viewArgTypes;
    // 先收集实参（保持参数顺序：前面的实参 + 尾部的默认参数）
    for (size_t i = 0; i < e.args.size(); ++i) {
        std::string arg = genExpr(*e.args[i], isCoroutine);
        if (ipIt != fnInterfaceParams_.end()) {
            for (auto& [idx, ifaceName] : ipIt->second) {
                if (idx == i) {
                    const SemType* argTy = e.args[i]->inferredType;
                    if (argTy && dynamic_cast<const InterfaceSemType*>(argTy)) {
                        // 接口变量透传（已在传参处构造适配器）：不包装
                    } else if (auto* rt = dynamic_cast<const RecordSemType*>(argTy)) {
                        // 具体 record → 适配器构造（v1 仅非泛型 record）
                        // 适配器已 GC 化（单继承 GcObject）：
                        // 先 root record 指针（gcConstruct 内 alloc 可能触发 GC），
                        // gcConstruct 分配适配器，view 返回视图传给接口参数
                        std::string recName = rt->canonicalName;
                        std::string adName = (ifaceName == "Iterator")
                            ? safeName(recName) + "Iterator"
                            : safeName(recName) + ifaceName;
                        arg = "[&]() -> auto {\n"
                              "    " + recName + "* _ar = (" + arg + ");\n"
                              "    aura_rt::GcRootHandle<" + recName + "*> _ah(_ar);\n"
                              "    auto* _ad = aura_rt::gcConstruct<" + adName
                              + ">(&" + adName + "::desc(), _ah.get());\n"
                              "    return " + adName + "::view(_ad);\n"
                              "  }()";
                        viewArgTypes[i] = makeIfaceViewMarker(ifaceName);
                    } else if (dynamic_cast<const FuncSemType*>(argTy)) {
                        // 闭包 → XFunc GC 化：值拷贝 std::function 后 gcConstruct 分配适配器
                        // （func 捕获的 GcRootHandle 为全局根 ValueGlobal，alloc 期间安全；
                        //   finalizer 析构 func，见 genInterfaceDecl XFunc）
                        arg = "[&]() -> auto {\n"
                              "    auto _cf = (" + arg + ");\n"
                              "    auto* _cd = aura_rt::gcConstruct<" + ifaceName + "Func>"
                              "(&" + ifaceName + "Func::desc(), std::move(_cf));\n"
                              "    return " + ifaceName + "Func::view(_cd);\n"
                              "  }()";
                        viewArgTypes[i] = makeIfaceViewMarker(ifaceName);
                    }
                    // 其余（inferredType 缺失/非闭包的视图变量等，如闭包内捕获的
                    // 接口视图变量）→ 透传不包装：仅 FuncSemType 才构造 XFunc，
                    // 防未知类型实参被误当闭包生成不存在的 XFunc（#8 回归：内置
                    // 接口无 XFunc 适配器，且视图变量透传分支依据 InterfaceSemType，
                    // 捕获变量 inferredType 缺失时会落入此处）
                    break;
                }
            }
        }
        // 回调参数：包装裸 lambda 为 std::function
        if (cbIt != fnCallbackParams_.end()) {
            for (auto& [idx, ftStr] : cbIt->second) {
                if (idx == i) {
                    // 泛型函数调用点：ftStr 含未绑定泛型变量（如 std::function<T(T)>），
                    // 非泛型调用点无 T 作用域 → 编译失败。改用实参推断的具体 FuncSemType
                    // 生成 std::function 类型（双向推断已将 T 代换为具体类型）；
                    // 实参仍含泛型（调用点在泛型作用域内，T 在作用域）时保持原 ftStr。
                    std::string wrapType = ftStr;
                    if (auto* fst = dynamic_cast<const FuncSemType*>(e.args[i]->inferredType);
                        fst && semTypeIsConcrete(fst)) {
                        wrapType = mapSemType(*fst);
                    }
                    arg = wrapType + "(" + arg + ")";
                    break;
                }
            }
        }
        // G1：Optional/Union 形参装箱——实参裸值（record/值/列表/视图）直传 Optional/Union
        // 形参时 make_optional / make_variant 装箱（take_opt({..})/take_opt_int(5)/
        // take_opt_list([{..}])/take_union({..})/take_u(5)）。fnParamCppTypes_ 同时
        // 覆盖构造函数形参（isCtor 分支 calleeName = 记录名）。genParamBoxing 内部
        // 防二次装箱：some()/none()/已是 Optional 值 → genOptionalTargetInit 直通；
        // 已是 Union 值 → genUnionBoxingImpl idx 匹配失败返回空。
        auto ppIt = fnParamCppTypes_.find(calleeName);
        if (ppIt != fnParamCppTypes_.end() && i < ppIt->second.size()) {
            std::string boxed = genParamBoxing(ppIt->second[i], *e.args[i], isCoroutine);
            if (!boxed.empty()) arg = boxed;
        }
        argExprs.push_back(arg);
    }
    // C5.1/C5.3: 同模块函数 / ctor 默认参数补齐（调用点补实参，支持任意表达式）
    if (isCtor) {
        if (auto ctIt = methodDefaultArgs_.find(calleeName); ctIt != methodDefaultArgs_.end())
            for (size_t k = e.args.size(); k < ctIt->second.size(); ++k)
                if (ctIt->second[k]) argExprs.push_back(genExpr(*ctIt->second[k], isCoroutine));
    } else if (auto fit = fnDefaultArgs_.find(calleeName); fit != fnDefaultArgs_.end()) {
        // M3：泛型函数默认参数闭包引用函数模板 T 时，调用点需将闭包参数/返回类型中
        // 的 T 物化为调用点实参推导的具体类型（生成普通 lambda），并包装为显式
        // std::function<具体类型> 实参——否则裸 lambda（即便普通 lambda）不参与函数
        // 模板 std::function<T(T)> 的实参推导（g++ 'lambda' is not derived from
        // 'std::function<T(T)>'），T 无法从实参推导。仅默认实参含闭包且闭包引用函数
        // 模板泛型时设置映射；方法默认参数（methodDefaultArgs_ 分支）不设（P4-9 已靠
        // receiver 具体类型让模板 lambda 隐式转换，设置会改变既有形态）。
        std::map<std::string, std::string> materialized;
        bool hasFunDefault = false;
        for (size_t k = e.args.size(); k < fit->second.size(); ++k)
            if (fit->second[k] && dynamic_cast<const FunExpr*>(fit->second[k])) {
                hasFunDefault = true; break;
            }
        std::map<std::string, std::string> savedMat;
        if (hasFunDefault) {
            collectDefaultArgGenericMap(calleeName, e.args, materialized);
            if (!materialized.empty()) {
                savedMat = defaultArgMaterializedTypes_;
                defaultArgMaterializedTypes_ = materialized;
            }
        }
        // fnCallbackParams_ 注册的回调形参索引（默认实参闭包须按物化类型包装 std::function）
        auto cbIt = fnCallbackParams_.find(calleeName);
        for (size_t k = e.args.size(); k < fit->second.size(); ++k) {
            if (!fit->second[k]) continue;
            std::string arg = genExpr(*fit->second[k], isCoroutine);
            if (!materialized.empty() && cbIt != fnCallbackParams_.end()) {
                for (auto& [idx, ftStr] : cbIt->second) {
                    if (idx == k) {
                        // 物化映射生效期间 mapType 输出 std::function<int32_t(int32_t)>
                        // （回调形参类型含裸泛型名 T，mapType 递归物化）
                        const TypeExpr* ft = (k < fnParamTypeExprs_[calleeName].size())
                            ? fnParamTypeExprs_[calleeName][k] : nullptr;
                        std::string wrapType = ft ? mapType(*ft) : ftStr;
                        arg = wrapType + "(" + arg + ")";
                        break;
                    }
                }
            }
            argExprs.push_back(arg);
        }
        if (!materialized.empty()) defaultArgMaterializedTypes_ = savedMat;
    }

    std::string prefix = needAwait ? "co_await " : "";
    std::ostringstream oss;
    oss << prefix << calleeExpr << targs << "(";
    for (size_t i = 0; i < argExprs.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << "{" << i << "}";
    }
    oss << ")";
    std::string callExpr = oss.str();

    // 有堆类型参数 → GcRootHandle 保护（含构造函数调用、补齐的默认实参）
    if (!argExprs.empty()) {
        std::vector<std::pair<std::string, const SemType*>> gcArgs;
        for (size_t i = 0; i < argExprs.size(); ++i) {
            const SemType* ty = nullptr;
            auto vit = viewArgTypes.find(i);
            if (vit != viewArgTypes.end()) {
                ty = vit->second.get();   // G3：record→view 转换后的实参按视图类型
            } else if (i < e.args.size()) {
                ty = e.args[i]->inferredType;
            } else if (isCtor) {
                auto ctIt = methodDefaultArgs_.find(calleeName);
                if (ctIt != methodDefaultArgs_.end() && ctIt->second[i])
                    ty = ctIt->second[i]->inferredType;
            } else {
                auto fit = fnDefaultArgs_.find(calleeName);
                if (fit != fnDefaultArgs_.end() && fit->second[i])
                    ty = fit->second[i]->inferredType;
            }
            gcArgs.emplace_back(argExprs[i], ty);
        }
        return genGcRootedArgs(gcArgs, callExpr, isCoroutine);
    }

    // 无堆类型参数 → 直接生成
    std::ostringstream oss2;
    oss2 << prefix << calleeExpr << targs << "(";
    for (size_t i = 0; i < argExprs.size(); ++i) {
        if (i > 0) oss2 << ", ";
        oss2 << argExprs[i];
    }
    oss2 << ")";
    return oss2.str();
}

std::string CodeGenerator::genMethodCall(const MethodCallExpr& e, bool isCoroutine) {
    // P4：联合接收者动态分派——receiver 是 UnionSemType 时生成运行时类型判定分派
    if (e.object && e.object->inferredType) {
        if (auto* u = dynamic_cast<const UnionSemType*>(e.object->inferredType))
            return genUnionDispatch(e, *u, isCoroutine);
    }
    // sync.Mutex() / sync.RWMutex() / sync.Once() / sync.Channel<T>(cap) 构造特殊处理
    // 解析为 MethodCallExpr(object=Identifier("sync"), method="Mutex"/.../"Channel")
    // Aura 暴露 sync.Channel<T>，C++ Runtime 仍叫 ThreadChannel<T>（与协程 Channel<T> 区分）
    if (auto* id = dynamic_cast<const Identifier*>(e.object.get())) {
        if (id->name == "sync") {
            // 无参构造：Mutex / RWMutex / Once / Channel()
            if (e.args.empty()) {
                if (e.method == "Mutex") {
                    return "aura_rt::make_mutex()";
                }
                if (e.method == "RWMutex") {
                    return "aura_rt::make_rwmutex()";
                }
                if (e.method == "Once") {
                    return "aura_rt::make_once()";
                }
                // sync.Channel() 无参 → cap=0（运行时视为 cap=1）
                if (e.method == "Channel") {
                    std::string targ = expectedTemplateArgs_.empty() ? "int32_t" : expectedTemplateArgs_[0];
                    std::string result = "aura_rt::make_thread_channel<" + targ + ">(0)";
                    // 跟踪为 channel 变量（for-in 展开用）
                    if (!currentLetName_.empty()) {
                        channelVarNames_.insert(currentLetName_);
                    }
                    return result;
                }
            } else {
                // sync.Channel<T>(cap) 带参构造
                if (e.method == "Channel") {
                    std::string targ = expectedTemplateArgs_.empty() ? "int32_t" : expectedTemplateArgs_[0];
                    std::string cap = genExpr(*e.args[0], isCoroutine);
                    std::string result = "aura_rt::make_thread_channel<" + targ + ">(" + cap + ")";
                    // 跟踪为 channel 变量（for-in 展开用）
                    if (!currentLetName_.empty()) {
                        channelVarNames_.insert(currentLetName_);
                    }
                    return result;
                }
            }
        }
    }

    std::string obj = genExpr(*e.object, isCoroutine);
    std::ostringstream oss;

    // ============================================================
    // Iterator 桥接方法特判（map/filter/collect/from 直转 runtime，不走虚调用）
    // 模板参数全部由 C++ 参数推导（src: Iterator<T>*, f: lambda → invoke_result_t）
    // ============================================================
    bool objIsIterator = false;
    // 先按 Sema 推断类型判定（覆盖 let 变量/表达式/参数：GenericSemType Iterator 或
    // InterfaceSemType Iterator）；Identifier 的 inferredType 在 Sema 推断时已填充
    if (e.object->inferredType) {
        if (auto* g = dynamic_cast<const GenericSemType*>(e.object->inferredType))
            objIsIterator = g->name == "Iterator";
        else if (auto* is = dynamic_cast<const InterfaceSemType*>(e.object->inferredType))
            objIsIterator = is->name == "Iterator";
    }
    // Iterator.from(...) 静态调用（Identifier "Iterator"，无 inferredType）
    if (auto* id = dynamic_cast<const Identifier*>(e.object.get()))
        if (id->name == "Iterator") objIsIterator = true;
    if (objIsIterator) {
        // 实参（闭包）表达式
        std::vector<std::string> iArgs;
        for (size_t i = 0; i < e.args.size(); ++i)
            iArgs.push_back(genExpr(*e.args[i], isCoroutine));
        if (e.method == "from" && iArgs.size() == 1) {
            // FuncIter 无自动推导（T 与 F 无关联）：T 从闭包返回类型 Optional<T> 显式提取。
            // A2：显式 `-> Optional<string>` 注解物化为 GenericSemType{name=="Optional"}，
            // 与 OptionalSemType 统一经 optionalElemCppName 提取；真未知（确为 Optional
            // 但元素推不出）→ 防御性报错（不静默退 int32_t）。
            // 非 Optional 返回类型（如 `fun()->int`）保持既有 int32_t 兜底（Sema 侧同样
            // 落 int32_t、不报错），避免 CodeGen 与 Sema 判定不一致。
            std::string elem;
            bool retIsOptional = false;
            if (auto* ft = dynamic_cast<const FuncSemType*>(e.args[0]->inferredType)) {
                auto* rt = ft->returnType.get();
                retIsOptional = dynamic_cast<const OptionalSemType*>(rt)
                    || (dynamic_cast<const GenericSemType*>(rt)
                        && static_cast<const GenericSemType*>(rt)->name == "Optional");
                if (retIsOptional) elem = optionalElemCppName(rt);
            }
            if (retIsOptional && elem.empty()) {
                error(e, "cannot infer element type of closure return for 'Iterator.from'; "
                         "annotate the return type (e.g. fun () -> Optional<string>)");
                elem = "int32_t";  // 占位；driver 检测到 codegen 错误后不会调用 g++
            } else if (elem.empty()) {
                elem = "int32_t";  // 非 Optional 返回：既有兜底（与 Sema 一致）
            }
            return "aura_rt::make_iterator_from<" + elem + ">(" + iArgs[0] + ")";
        }
        if (e.method == "map" && iArgs.size() == 1) {
            std::string call = "aura_rt::make_map(" + obj + ", " + iArgs[0] + ")";
            std::vector<std::pair<std::string, const SemType*>> gArgs;
            gArgs.emplace_back(obj, e.object->inferredType);
            gArgs.emplace_back(iArgs[0], e.args[0]->inferredType);
            return genGcRootedArgs(gArgs, call, isCoroutine);
        }
        if (e.method == "filter" && iArgs.size() == 1) {
            std::string call = "aura_rt::make_filter(" + obj + ", " + iArgs[0] + ")";
            std::vector<std::pair<std::string, const SemType*>> gArgs;
            gArgs.emplace_back(obj, e.object->inferredType);
            gArgs.emplace_back(iArgs[0], e.args[0]->inferredType);
            return genGcRootedArgs(gArgs, call, isCoroutine);
        }
        if (e.method == "collect" && iArgs.empty()) {
            std::string call = "aura_rt::collect_all(" + obj + ")";
            std::vector<std::pair<std::string, const SemType*>> gArgs;
            gArgs.emplace_back(obj, e.object->inferredType);
            return genGcRootedArgs(gArgs, call, isCoroutine);
        }
        if (e.method == "next" && iArgs.empty()) {
            // 视图直接调用 next()：值视图 {nextFn, self} 的运行时方法（与 for-in
            // 循环体内 _it.get().next() 同入口，iterator.h Iterator<T>::next()）。
            // 返回 Optional<T>* 为 GC 堆对象，调用点 let/match 绑定会按类型走
            // GcRootHandle 保护，此处仅按 collect 惯例保护接收者视图 self。
            std::string call = obj + ".next()";
            std::vector<std::pair<std::string, const SemType*>> gArgs;
            gArgs.emplace_back(obj, e.object->inferredType);
            return genGcRootedArgs(gArgs, call, isCoroutine);
        }
    }

    // 判断是否是 io 调用
    bool isIoCall = false;
    if (auto* id = dynamic_cast<const Identifier*>(e.object.get())) {
        if (id->name == "io") isIoCall = true;
    }

    // #io.sync = true 或非协程上下文：所有 IO 方法统一加 _sync 后缀，提前返回
    // 非协程上下文（如 try/catch 协程安全模式的 IIFE）不能用 co_await，必须走同步版本
    if (isIoCall && (ioSync_ || !isCoroutine)) {
        std::vector<std::string> syncArgExprs;
        for (size_t i = 0; i < e.args.size(); ++i)
            syncArgExprs.push_back(genExpr(*e.args[i], false));
        std::ostringstream rawOss;
        rawOss << obj << "." << e.method << "_sync(";
        for (size_t i = 0; i < syncArgExprs.size(); ++i) {
            if (i > 0) rawOss << ", ";
            rawOss << "{" << i << "}";
        }
        rawOss << ")";
        std::vector<std::pair<std::string, const SemType*>> syncArgs;
        for (size_t i = 0; i < e.args.size(); ++i)
            syncArgs.push_back({syncArgExprs[i], e.args[i]->inferredType});
        return genGcRootedArgs(syncArgs, rawOss.str(), false);
    }

    // io.* 调用需要 co_await（仅对有异步版本的方法）
    bool needAwait = isIoCall && isCoroutine
                     && BuiltinRegistry::get().methodHasAsync("Io", e.method);

    // channel.send / channel.receive 需要 co_await（协程 channel 专用）
    // sync.ThreadChannel 的 send/receive 是阻塞调用，非协程 awaitable
    if (e.method == "send" || e.method == "receive") {
        // 协程 channel 判定：变量名（channelVarNames_，let/构造跟踪）或 receiver 推断
        // 类型（GenericSemType "channel"，覆盖方法/函数参数等未进 channelVarNames_ 的
        // channel）。sync.Channel 的 inferredType 是 "sync.Channel"，不匹配，不受影响。
        bool isCoroChannel = false;
        if (auto* id = dynamic_cast<const Identifier*>(e.object.get()))
            if (channelVarNames_.count(id->name)) isCoroChannel = true;
        if (!isCoroChannel && e.object->inferredType) {
            if (auto* g = dynamic_cast<const GenericSemType*>(e.object->inferredType))
                if (g->name == "channel") isCoroChannel = true;
        }
        if (isCoroChannel) {
            bool isSyncChannel = false;
            if (auto* id = dynamic_cast<const Identifier*>(e.object.get())) {
                auto it = gcRootTypes_.find(id->name);
                if (it != gcRootTypes_.end() && it->second.find("ThreadChannel") != std::string::npos)
                    isSyncChannel = true;
            }
            if (!isSyncChannel)
                needAwait = needAwait || isCoroutine;
        }
    }

    // 用户自定义协程方法调用：receiver 类型（RecordSemType.canonicalName 截取 '<' 前，
    // 与声明侧 receiverType 对齐）+ 方法名查 coroutineFunctions_（键 = "ReceiverType.
    // methodName"）→ co_await。仅在当前协程上下文加（非协程上下文不能 co_await）。
    // 方法体内调用协程方法（self.xxx()）由 decideCoro 传播标为协程，故 isCoroutine 恒真。
    if (isCoroutine) {
        std::string recvKey;
        if (e.object->inferredType) {
            if (auto* r = dynamic_cast<const RecordSemType*>(e.object->inferredType)) {
                if (!r->canonicalName.empty()) {
                    recvKey = r->canonicalName;
                    size_t lt = recvKey.find('<');
                    if (lt != std::string::npos) recvKey = recvKey.substr(0, lt);
                }
            }
        }
        if (!recvKey.empty() && coroutineFunctions_.count(recvKey + "." + e.method))
            needAwait = true;
    }

    std::string prefix = needAwait ? "co_await " : "";

    // 判断是命名空间限定下的构造调用：math.Pair(...) → math::Pair_ctor(...)
    // 检查条件：对象是导入的命名空间 + (方法名是本地注册的堆类型 或 以大写开头(跨模块类型))
    bool isNsCtor = false;
    // isNs：receiver 是导入的命名空间别名（如 path、io），不应作为表达式参与 GcRootedArgs 包装
    bool isNs = false;
    if (auto* id = dynamic_cast<const Identifier*>(e.object.get())) {
        if (importNsNames_.count(id->name)) {
            isNs = true;
            if (registeredTypes_.count(e.method) && registeredTypes_[e.method]) {
                isNsCtor = true;
            } else if (!e.method.empty() && std::isupper(static_cast<unsigned char>(e.method[0]))) {
                // 跨模块类型：导入命名空间下的 PascalCase 调用视为构造函数
                isNsCtor = true;
            }
        }
    }

    std::string access = "->";  // 默认指针访问

    if (isNsCtor) {
        oss << prefix << obj << "::" << safeName(e.method) << "_ctor";
        if (!expectedTemplateArgs_.empty()) {
            oss << "<";
            for (size_t i = 0; i < expectedTemplateArgs_.size(); ++i) {
                if (i > 0) oss << ", ";
                oss << expectedTemplateArgs_[i];
            }
            oss << ">";
        }
        oss << "(";
    } else {
        // 判断对象是值类型（用 . ）还是指针类型（用 -> ）还是命名空间（用 ::）
        if (auto* id = dynamic_cast<const Identifier*>(e.object.get())) {
            if (importNsNames_.count(id->name)) {
                access = "::";
            } else if (valueTypeVarNames_.count(id->name)) {
                access = ".";
            } else {
                access = "->";
            }
        } else if (e.object->inferredType && isIfaceView(e.object->inferredType)) {
            // #3：对象是视图值（接口视图字段 r.s / Iterator 字段 / 条件表达式结果等）
            // → 用 . 访问（视图是值类型，C++ 不允许 ->；视图字段的接口方法调用）
            access = ".";
        } else {
            access = "->";
        }
        oss << prefix << obj << access << safeName(e.method) << "(";
    }
    // C5.3: 方法默认参数补齐（键 = ReceiverType.methodName）
    // recvTypeKey 从 receiver 的 inferredType 推导：
    //   string → "string"、Array<T> → "[T]"、泛型 T → g->name、自定义类型 → canonicalName
    std::string recvTypeKey;
    if (e.object->inferredType) {
        if (auto* p = dynamic_cast<const PrimSemType*>(e.object->inferredType)) {
            if (p->kind == PrimSemType::String) recvTypeKey = "string";
        } else if (dynamic_cast<const ListSemType*>(e.object->inferredType)) {
            recvTypeKey = "[T]";
        } else if (auto* g = dynamic_cast<const GenericSemType*>(e.object->inferredType)) {
            recvTypeKey = g->name;
        } else if (auto* r = dynamic_cast<const RecordSemType*>(e.object->inferredType)) {
            if (!r->canonicalName.empty()) recvTypeKey = r->canonicalName;
        } else if (auto* is = dynamic_cast<const InterfaceSemType*>(e.object->inferredType)) {
            // G3：接口视图接收者（s.put(...)，s: Stringer 视图）→ 键 = 接口名，
            // 与 genInterfaceDecl 注册的 methodInterfaceParams_/"接口名.methodName"
            // 同键（methodDefaultArgs_/methodParamCppTypes_ 亦然）。此前视图接收者
            // recvTypeKey 恒空，接口方法实参转换/默认参数/装箱全部查表不命中。
            recvTypeKey = is->name;
        }
    }
    // M2：方法默认参数查询键 = 声明侧 receiver 名（与 methodDefaultArgs_ 注册键
    // decl.receiverType + "." + decl.name 一致）。泛型 record 实例化的 canonicalName
    // 含类型实参（如 "Box<int32_t>"）→ 截取 '<' 前为声明名 "Box"；非泛型 canonicalName
    // 无 '<' 原样。仅 methodDefaultArgs_（默认参数补全）使用；装箱/接口参数查表仍用
    // 原 recvTypeKey（形参 C++ 类型含未绑定泛型名 T，无法在调用点直接实例化装箱）。
    std::string methodDefKey = recvTypeKey;
    {
        size_t lt = methodDefKey.find('<');
        if (lt != std::string::npos) methodDefKey = methodDefKey.substr(0, lt);
    }
    // 先收集参数表达式（保持参数顺序：前面的实参 + 尾部的默认参数）
    std::vector<std::string> mArgExprs;
    // G1：方法/接口方法形参 Optional/Union 装箱（键 = recvTypeKey + "." + method，与
    // methodDefaultArgs_ 同机制；record 方法由 genMethodDecl 注册、接口视图方法由
    // genInterfaceDecl 注册）。b.use({..}) / s.put({..}) / Box2({..} ctor 走 genCallExpr
    // isCtor 分支，不在此）→ 裸 record/值/列表直传 Optional/Union 方法形参时装箱。
    auto mpIt = methodParamCppTypes_.find(recvTypeKey + "." + e.method);
    // G3：方法/接口方法接口参数（record 实参直传接口视图形参 → record→view）。
    auto miIt = methodInterfaceParams_.find(recvTypeKey + "." + e.method);
    // G3：被 record→view 转换的实参 idx → 视图类型标记（genGcRootedArgs 据此走
    // 视图值分支，不生成 GcRootHandle<视图> 坏根；见 makeIfaceViewMarker）。
    std::map<size_t, std::unique_ptr<SemType>> viewArgTypes;
    for (size_t i = 0; i < e.args.size(); ++i) {
        std::string marg = genExpr(*e.args[i], isCoroutine);
        // G3：接口参数转换——record 实参直传接口视图形参 → genRecordToViewIIFE
        // （gcConstruct 适配器 + ::view，与 genCallExpr fnInterfaceParams_ 同构）。
        // 视图变量实参（InterfaceSemType）透传不二次包装；Optional/Union 形参
        // （paramCpp 非直连接口名，未注册进 methodInterfaceParams_）不在此处理。
        if (miIt != methodInterfaceParams_.end()) {
            for (auto& [idx, ifaceName] : miIt->second) {
                if (idx == i) {
                    const SemType* argTy = e.args[i]->inferredType;
                    if (argTy && dynamic_cast<const InterfaceSemType*>(argTy)) {
                        // 视图变量透传：不包装
                    } else if (auto* rt = dynamic_cast<const RecordSemType*>(argTy)) {
                        if (!rt->canonicalName.empty()) {
                            marg = genRecordToViewIIFE(marg, rt->canonicalName, ifaceName);
                            viewArgTypes[i] = makeIfaceViewMarker(ifaceName);
                        }
                    }
                    break;
                }
            }
        }
        if (mpIt != methodParamCppTypes_.end() && i < mpIt->second.size()) {
            std::string boxed = genParamBoxing(mpIt->second[i], *e.args[i], isCoroutine);
            if (!boxed.empty()) marg = boxed;
        }
        mArgExprs.push_back(marg);
    }
    // C5.3: 方法默认参数补齐（跨模块 ctor（isNsCtor）默认参数 v1 不支持）
    if (!isNs && !isNsCtor) {
        if (auto mmIt = methodDefaultArgs_.find(methodDefKey + "." + e.method); mmIt != methodDefaultArgs_.end())
            for (size_t k = e.args.size(); k < mmIt->second.size(); ++k)
                if (mmIt->second[k]) mArgExprs.push_back(genExpr(*mmIt->second[k], isCoroutine));
    }
    // C5.4: 跨模块函数默认参数补齐（math.foo(...) 缺参时）
    if (isNs) {
        if (auto* id = dynamic_cast<const Identifier*>(e.object.get())) {
            auto cmIt = crossDefaults_.find(id->name);
            if (cmIt != crossDefaults_.end()) {
                auto fnIt = cmIt->second.find(e.method);
                if (fnIt != cmIt->second.end())
                    for (size_t k = e.args.size(); k < fnIt->second.size(); ++k)
                        if (fnIt->second[k]) mArgExprs.push_back(genExpr(*fnIt->second[k], isCoroutine));
            }
        }
    }
    // 第 i 个参数的 inferredType（实参 → 方法默认 → 跨模块默认），供 GC 保护判断
    // G3：record→view 转换后的实参返回视图类型标记（isIfaceView 判定用）
    auto mArgType = [&](size_t i) -> const SemType* {
        auto vit = viewArgTypes.find(i);
        if (vit != viewArgTypes.end()) return vit->second.get();
        if (i < e.args.size()) return e.args[i]->inferredType;
        if (auto mmIt = methodDefaultArgs_.find(methodDefKey + "." + e.method);
            mmIt != methodDefaultArgs_.end() && i < mmIt->second.size() && mmIt->second[i])
            return mmIt->second[i]->inferredType;
        if (isNs) {
            if (auto* id = dynamic_cast<const Identifier*>(e.object.get())) {
                auto cmIt = crossDefaults_.find(id->name);
                if (cmIt != crossDefaults_.end()) {
                    auto fnIt = cmIt->second.find(e.method);
                    if (fnIt != cmIt->second.end() && i < fnIt->second.size() && fnIt->second[i])
                        return fnIt->second[i]->inferredType;
                }
            }
        }
        return nullptr;
    };
    for (size_t i = 0; i < mArgExprs.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << "{" << (i + 1) << "}";  // {0} = obj, {1..} = args
    }
    oss << ")";
    std::string callExpr = oss.str();

    // Io 调用、值类型对象、命名空间调用：obj 本身无需 GcRootHandle，但参数需要保护
    // （参数可能是返回 GC 指针的临时表达式，方法内部可能触发 GC 回收）
    auto buildRawCall = [&]() {
        std::ostringstream raw;
        raw << prefix << obj << access << safeName(e.method) << "(";
        for (size_t i = 0; i < mArgExprs.size(); ++i) {
            if (i > 0) raw << ", ";
            raw << "{" << i << "}";  // genGcRootedArgs 从 {0} 开始替换
        }
        raw << ")";
        return raw.str();
    };
    if (isIoCall || isNsCtor || isNs) {
        // 用 genGcRootedArgs 包装参数（obj 是值类型/命名空间，不参与包装）
        // isNs：path.new(...) / math.abs(...) 等，receiver 是 namespace 别名，
        // 不能作为表达式求值（不能 `const auto& x = (path);`），必须直接用 obj 名字生成 obj::method(...)
        std::vector<std::pair<std::string, const SemType*>> ioArgs;
        for (size_t i = 0; i < mArgExprs.size(); ++i)
            ioArgs.push_back({mArgExprs[i], mArgType(i)});
        return genGcRootedArgs(ioArgs, buildRawCall(), isCoroutine);
    }
    if (e.object.get() && e.object->inferredType
        && !isHeapSemType(e.object->inferredType)) {
        std::vector<std::pair<std::string, const SemType*>> valArgs;
        for (size_t i = 0; i < mArgExprs.size(); ++i)
            valArgs.push_back({mArgExprs[i], mArgType(i)});
        return genGcRootedArgs(valArgs, buildRawCall(), isCoroutine);
    }

    // 堆类型对象或参数 → GcRootHandle 保护
    std::vector<std::pair<std::string, const SemType*>> gcArgs;
    gcArgs.emplace_back(obj, e.object->inferredType);  // {0} = obj
    for (size_t i = 0; i < mArgExprs.size(); ++i)
        gcArgs.emplace_back(mArgExprs[i], mArgType(i));  // {i+1}
    // 构建带占位符的 callExpr
    std::ostringstream gcCall;
    gcCall << prefix << "{0}" << access << safeName(e.method) << "(";
    for (size_t i = 0; i < mArgExprs.size(); ++i) {
        if (i > 0) gcCall << ", ";
        gcCall << "{" << (i + 1) << "}";
    }
    gcCall << ")";
    std::string callResult = genGcRootedArgs(gcArgs, gcCall.str(), isCoroutine);

    return callResult;
}

std::string CodeGenerator::genMemberAccess(const MemberAccessExpr& e) {
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
    out << "[&]() -> " << retType << " {\n";
    indentLevel_++;
    out << indentStr() << "auto&& _dsp_v = (" << obj << ");\n";
    if (sups.size() == 1) {
        // 单支持变体：运行时类型检查 + 直调
        size_t I = sups[0];
        out << indentStr() << "if (_dsp_v->index() != " << I
            << ") throw aura_rt::make_type_error(\"TypeError: variant active variant has no method '"
            << e.method << "'\");\n";
        out << indentStr();
        if (!retIsVoid) out << "return ";
        // #3：接口视图变体是值类型（Stringer），用 . 访问；其余变体用 ->
        out << "_dsp_v->get<" << I << ">()"
            << (isIfaceView(u.variants[I].get()) ? "." : "->")
            << safeName(e.method) << "(";
        for (size_t i = 0; i < argExprs.size(); ++i) {
            if (i > 0) out << ", ";
            out << argExprs[i];
        }
        if (retIsVoid)
            out << "); return;\n";
        else
            out << ");\n";
    } else {
        // 多变体：switch 分派，default 抛 TypeError
        out << indentStr() << "switch (_dsp_v->index()) {\n";
        indentLevel_++;
        for (size_t k : sups) {
            out << indentStr() << "case " << k << ": ";
            if (!retIsVoid) out << "return ";
            out << "_dsp_v->get<" << k << ">()"
                << (isIfaceView(u.variants[k].get()) ? "." : "->")
                << safeName(e.method) << "(";
            for (size_t i = 0; i < argExprs.size(); ++i) {
                if (i > 0) out << ", ";
                out << argExprs[i];
            }
            if (retIsVoid)
                out << "); return;\n";
            else
                out << ");\n";
        }
        out << indentStr() << "default: throw aura_rt::make_type_error(\"TypeError: variant ("
            << u.toString() << ") active variant has no method '" << e.method << "'\");\n";
        indentLevel_--;
        out << indentStr() << "}\n";
    }
    indentLevel_--;
    out << indentStr() << "}()";
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
            if (auto* u = dynamic_cast<const UnionSemType*>(e.target->inferredType))
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

// ============================================================
// 闭包表达式 → C++20 lambda
// ============================================================

// M3：从「形参类型表达式 + 调用点实参 SemType」递归推导泛型绑定（泛型名 → 具体
// C++ 类型）。调用点 useT(5,10) 缺默认实参 cb 时，cb 默认闭包 fun(x:T)->T 中的 T
// 需按调用点实参物化为 int32_t，否则生成模板 lambda []<typename T>(T x)->T 无法向
// 具体 std::function<int(int)> 函数模板形参推导匹配。currentTParams_ 中的外层模板
// 参数名不物化（由外层声明提供，闭包直接引用即可）。
void CodeGenerator::collectMaterializedFromType(
    const TypeExpr& formal, const SemType& arg,
    std::map<std::string, std::string>& out)
{
    auto notInOuter = [this](const std::string& name) {
        for (auto& tp : currentTParams_)
            if (tp == name) return false;
        return true;
    };
    // 实参 → 可物化的 C++ 类型串；返回空表示不可物化（Error / 未绑定泛型且非外层
    // 模板参数）。未绑定泛型实参仅当它是外层模板参数（如泛型函数体内调用
    // useT2(inc,v) 的 U）时有 C++ 名（其模板参数名），否则 mapSemType 会兜底 "auto"。
    auto argCpp = [this](const SemType& a) -> std::string {
        if (dynamic_cast<const ErrorSemType*>(&a)) return "";
        if (auto* gs = dynamic_cast<const GenericSemType*>(&a)) {
            if (gs->resolvedName.empty()) {
                for (auto& tp : currentTParams_)
                    if (tp == gs->name) return gs->name;
                return "";
            }
        }
        return mapSemType(a);
    };
    // <T> 泛型引用：实参可物化时绑定
    if (auto* g = dynamic_cast<const GenericTypeRef*>(&formal)) {
        if (notInOuter(g->name)) {
            std::string cpp = argCpp(arg);
            if (!cpp.empty()) out[g->name] = cpp;
        }
        return;
    }
    if (auto* n = dynamic_cast<const NamedType*>(&formal)) {
        // 裸名泛型形参（v: T，TypeParser 解析为 NamedType）→ 绑定
        if (n->typeArgs.empty() && notInOuter(n->name)
            && !registeredTypes_.count(n->name)
            && !interfaceNames_.count(n->name)
            && !BuiltinRegistry::get().findType(n->name)) {
            std::string cpp = argCpp(arg);
            if (!cpp.empty()) out[n->name] = cpp;
            return;
        }
        // 泛型类型别名/record 实例化（Transform<T>）：从函数类型实参递归匹配 typeArgs
        if (auto* fst = dynamic_cast<const FuncSemType*>(&arg)) {
            for (size_t k = 0; k < n->typeArgs.size(); ++k)
                if (n->typeArgs[k] && k < fst->paramTypes.size() && fst->paramTypes[k])
                    collectMaterializedFromType(*n->typeArgs[k], *fst->paramTypes[k], out);
        }
        return;
    }
    if (auto* l = dynamic_cast<const ListType*>(&formal)) {
        if (auto* ls = dynamic_cast<const ListSemType*>(&arg))
            if (l->elementType && ls->elementType)
                collectMaterializedFromType(*l->elementType, *ls->elementType, out);
        return;
    }
    if (auto* fn = dynamic_cast<const FunctionType*>(&formal)) {
        if (auto* fst = dynamic_cast<const FuncSemType*>(&arg)) {
            for (size_t k = 0; k < fn->paramTypes.size() && k < fst->paramTypes.size(); ++k)
                if (fn->paramTypes[k] && fst->paramTypes[k])
                    collectMaterializedFromType(*fn->paramTypes[k], *fst->paramTypes[k], out);
            if (fn->returnType && fst->returnType)
                collectMaterializedFromType(*fn->returnType, *fst->returnType, out);
        }
        return;
    }
}

void CodeGenerator::collectDefaultArgGenericMap(
    const std::string& calleeName,
    const std::vector<std::unique_ptr<ASTNode>>& args,
    std::map<std::string, std::string>& out)
{
    auto pIt = fnParamTypeExprs_.find(calleeName);
    if (pIt == fnParamTypeExprs_.end()) return;
    for (size_t i = 0; i < args.size() && i < pIt->second.size(); ++i) {
        const TypeExpr* ft = pIt->second[i];
        if (!ft || !args[i] || !args[i]->inferredType) continue;
        collectMaterializedFromType(*ft, *args[i]->inferredType, out);
    }
}

std::string CodeGenerator::genFunExpr(const FunExpr& e, bool isCoroutine) {
    if (!e.body) return "[]{}";

    // 检测闭包体内是否包含 io.xxx 调用 — 若有则在协程上下文中生成协程 lambda
    // Bug 2-A: 外层函数返回 std::function<...>（fun 类型）时，闭包必须是普通 lambda
    // （协程闭包返回 task<T>，与 std::function<T()> 不兼容），不能协程化。
    // currentReturnCppType_ 在 funSignature/methodSignature 中已赋值。
    bool outerRetIsFunction = currentReturnCppType_.find("std::function<") != std::string::npos;
    bool closureIsCoro = isCoroutine && !ioSync_ && !outerRetIsFunction
                         && IoDetector::scan(*e.body);

    // === 1. 捕获分析（复用 IdRefCollector + DeclaredCollector） ===
    std::set<std::string> allRefs;
    IdRefCollector idCol(allRefs);
    for (auto& s : e.body->stmts)
        if (s) idCol.collectStmt(*s);

    std::set<std::string> declared;
    DeclaredCollector declCol(declared);
    for (auto& s : e.body->stmts)
        if (s) declCol.collectStmt(*s);

    std::set<std::string> paramNames;
    for (auto& p : e.params) paramNames.insert(p.name);

    std::set<std::string> builtins = {"_tasks"};
    std::vector<std::string> captures;
    for (auto& name : allRefs) {
        if (declared.count(name))     continue;
        if (paramNames.count(name))   continue;
        if (builtins.count(name))     continue;
        if (registeredTypes_.count(name)) continue;
        // 内置函数（some/none/str/range/Iterator 等）不捕获——调用点直转 runtime
        if (BuiltinRegistry::get().hasFunctionName(name)) continue;

        auto it = registeredTypes_.find(name);
        if (it != registeredTypes_.end() && it->second) {
            error(e, "cannot capture heap-allocated variable '" + name +
                  "' in closure (not yet supported)");
            continue;
        }
        captures.push_back(name);
    }

    // === 2. 泛型分析（plan12 统一方案） ===
    // 收集闭包参数/返回类型中的所有 GenericTypeRef
    std::set<std::string> genericParams;
    for (auto& p : e.params)
        if (p.type) collectTParams(*p.type, genericParams);
    if (e.returnType) collectTParams(*e.returnType, genericParams);

    // 检测 FunctionType 回调参数 — 需要 F&& + invoke_result_t
    // 对每个 fun(A) -> B 参数，收集 B 的泛型名 → invoke_result_t 推导
    // 仅当泛型名**仅**出现在 FunctionType 返回类型中时才从模板参数移除
    std::vector<size_t> callableParamIndices;
    std::vector<std::string> callableResultGenerics;
    for (size_t i = 0; i < e.params.size(); ++i) {
        if (auto* ft = e.params[i].type
                ? dynamic_cast<const FunctionType*>(e.params[i].type.get())
                : nullptr) {
            callableParamIndices.push_back(i);
            // 此 FunctionType 返回类型的泛型名
            std::set<std::string> retGen;
            if (ft->returnType) collectTParams(*ft->returnType, retGen);
            // 只移除仅在返回类型中出现的泛型（不损害其他地方也用的泛型如 T）
            std::string retGenStr;
            for (auto& g : retGen) {
                // 检查 g 是否在其他参数或返回类型中也出现
                bool appearsElsewhere = false;
                for (size_t j = 0; j < e.params.size(); ++j) {
                    if (j == i) continue;
                    std::set<std::string> other;
                    if (e.params[j].type) collectTParams(*e.params[j].type, other);
                    if (other.count(g)) { appearsElsewhere = true; break; }
                }
                if (!appearsElsewhere && e.returnType) {
                    std::set<std::string> rtGen;
                    collectTParams(*e.returnType, rtGen);
                    // 注意：retGen 就已经是返回类型的泛型，e.returnType 可能包含更多
                    // 简化：检查 g 是否还在闭包级别的返回类型中（非 FunctionType 内部）
                    if (rtGen.count(g) && !retGen.count(g))
                        appearsElsewhere = true;
                }
                if (!retGenStr.empty()) retGenStr += ", ";
                retGenStr += g;
                if (!appearsElsewhere) genericParams.erase(g);
            }
            callableResultGenerics.push_back(retGenStr);
        }
    }

    // 移除仅在返回类型中出现的泛型（不能从参数推导，如 make_tree_mapper 中的 U）
    std::set<std::string> returnOnlyGenerics;
    if (e.returnType) {
        std::set<std::string> retGen;
        collectTParams(*e.returnType, retGen);
        std::set<std::string> paramGen;
        for (auto& p : e.params)
            if (p.type) collectTParams(*p.type, paramGen);
        for (auto& g : retGen) {
            if (!paramGen.count(g)) {
                genericParams.erase(g);
                returnOnlyGenerics.insert(g);
            }
        }
    }

    // 剔除外层函数/方法模板参数（currentTParams_）中的泛型名：闭包类型位置引用外层
    // 模板参数（如泛型方法 `fun(x: T) -> T` 中的 T）时 genericParams 会收集到 T；若
    // 再为闭包声明 `typename T` 将遮蔽外层 template<...>（g++ -Wtemplate-body error）。
    // 外层参数由外层声明、闭包直接引用即可，故从三个泛型集合中剔除；闭包自身新泛型
    // （不在 currentTParams_ 中，如 make_adder 的 T / 闭包独立 U）仍正常声明。
    // 同时剔除 M3 调用点已物化的默认参数闭包泛型名（defaultArgMaterializedTypes_）：
    // 物化后闭包按具体类型生成普通 lambda（[](int32_t x)->int32_t），不得再声明模板
    // 参数——模板 lambda 无法向具体 std::function<int(int)> 函数模板形参推导匹配。
    std::vector<std::string> excludeNames = currentTParams_;
    for (auto& [g, cpp] : defaultArgMaterializedTypes_) excludeNames.push_back(g);
    for (auto& tp : excludeNames) {
        genericParams.erase(tp);
        returnOnlyGenerics.erase(tp);
        // 回调返回类型泛型（callableResultGenerics 为逗号分隔名列表）同样剔除：
        // 否则 `using T = invoke_result_t<...>` 同样遮蔽外层 T。
        for (auto& cs : callableResultGenerics) {
            std::string filtered;
            bool firstPart = true;
            size_t pos = 0;
            while (pos <= cs.size()) {
                size_t comma = cs.find(',', pos);
                std::string part = cs.substr(pos, comma == std::string::npos
                                                 ? std::string::npos : comma - pos);
                size_t ts = part.find_first_not_of(" \t");
                if (ts != std::string::npos) part = part.substr(ts);
                size_t te = part.find_last_not_of(" \t");
                if (te != std::string::npos) part = part.substr(0, te + 1);
                if (!part.empty() && part != tp) {
                    if (!firstPart) filtered += ", ";
                    filtered += part;
                    firstPart = false;
                }
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
            cs = filtered;
        }
    }

    // === 3. 检测是否修改捕获变量（决定 mutable 关键字） ===
    bool needsMutable = !captures.empty() && AssignTargetCollector::anyMatch(*e.body, captures);

    // 扫描：捕获变量被用作调用目标 → 按值捕获的 lambda operator() 为 const，需 mutable
    if (!needsMutable && !captures.empty())
        needsMutable = CallTargetScanner::anyMatch(*e.body, captures);

    // 收集在闭包体内被引用（作为调用参数或直接调用）的捕获变量名
    std::set<std::string> calledCaptures = CaptureArgScanner::collectMatched(*e.body, captures);

    // === 4. 生成 C++ lambda ===
    std::ostringstream oss;

    // 捕获 + 模板参数
    oss << "[";
    for (size_t i = 0; i < captures.size(); ++i) {
        if (i > 0) oss << ", ";
        auto cn = safeName(captures[i]);
        // 递归闭包：let 声明的变量被自身闭包引用 → 按引用捕获
        if (!currentLetName_.empty() && captures[i] == currentLetName_) {
            oss << "&" << cn;
        } else if (gcRootVarNames_.count(captures[i])) {
            // GC 根变量 → init-capture 创建 GcRootHandle 值持有副本（全局根，闭包跨线程安全）
            // 如 [greeting = aura_rt::GcRootHandle<GcString*>(greeting.get(), aura_rt::GcRootScope::Global)]
            std::string type = gcRootTypes_[captures[i]];
            oss << cn << " = aura_rt::GcRootHandle<" << type << ">(" << cn << ".get(), "
                << "aura_rt::GcRootScope::Global)";
        } else if (viewRootVarNames_.count(captures[i])) {
            // 视图根变量 → init-capture 创建 ViewRoot 副本（Global 根，闭包跨线程安全）
            // 复用 relocateGlobalRootPtrs 机制：ThreadLocal 句柄被闭包值捕获进 GC 堆
            // （如 MapIter::fn_ / StringerFunc::func）后 node(this) 悬垂 → 转 Global 根
            // 如 [viewVar = aura_rt::ViewRoot<Iterator<int32_t>>(viewVar.v, viewVar.h.get(), aura_rt::GcRootScope::Global)]
            std::string type = viewRootTypes_[captures[i]];
            oss << cn << " = aura_rt::ViewRoot<" << type << ">(" << cn << ".v, "
                << cn << ".h.get(), aura_rt::GcRootScope::Global)";
        } else {
            oss << cn;
        }
    }

    // 模板参数列表
    bool hasGeneric = !genericParams.empty() || !callableParamIndices.empty();
    if (hasGeneric) {
        oss << "]<";
        bool first = true;
        for (auto& g : genericParams) {
            if (!first) oss << ", ";
            oss << "typename " << g;
            first = false;
        }
        for (size_t ci = 0; ci < callableParamIndices.size(); ++ci) {
            if (!first) oss << ", ";
            oss << "typename F" << ci;
            first = false;
        }
        oss << ">";
    } else {
        oss << "]";
    }

    // 参数列表
    oss << "(";
    for (size_t i = 0; i < e.params.size(); ++i) {
        if (i > 0) oss << ", ";
        bool isCallable = false;
        int  callableIdx = -1;
        for (size_t ci = 0; ci < callableParamIndices.size(); ++ci) {
            if (callableParamIndices[ci] == i) { isCallable = true; callableIdx = (int)ci; break; }
        }
        if (isCallable) {
            oss << "F" << callableIdx << "&& " << safeName(e.params[i].name);
        } else {
            // 参数类型：有显式标注用 mapType（多态闭包 [T] 正确映射 C++ 模板参数）；
            // 无标注用 Sema 反推结果（双向推断：闭包参数从期望类型推断）
            std::string paramType = "auto";
            if (e.params[i].type) {
                paramType = mapType(*e.params[i].type);
            } else if (auto* fst = dynamic_cast<const FuncSemType*>(e.inferredType);
                       fst && i < fst->paramTypes.size() && fst->paramTypes[i]) {
                paramType = mapSemType(*fst->paramTypes[i]);
            }
            oss << paramType << " " << safeName(e.params[i].name);
        }
    }
    oss << ")";

    // mutable 关键字：闭包体修改了按值捕获的变量
    if (needsMutable && !captures.empty())
        oss << " mutable";

    // 返回类型：协程闭包 → task<...>；多态闭包（调用点才确定）→ auto；
    // 无返回标注的闭包优先读 Sema 推断的返回类型（期望反推）；
    // 其余按显式标注映射；无标注且推断为 None/Error 保持 auto
    if (closureIsCoro) {
        if (e.returnType)
            oss << " -> aura_rt::task<" << mapType(*e.returnType) << ">";
        else
            oss << " -> aura_rt::task<void>";
    } else if (e.returnType && (!callableParamIndices.empty() || !returnOnlyGenerics.empty())) {
        oss << " -> auto";        // 多态闭包兜底（invoke_result_t 机制）
    } else if (e.returnType) {
        oss << " -> " << mapType(*e.returnType);
    } else if (auto* fst = dynamic_cast<const FuncSemType*>(e.inferredType);
               fst && fst->returnType
               && !dynamic_cast<const NoneSemType*>(fst->returnType.get())
               && !dynamic_cast<const ErrorSemType*>(fst->returnType.get())) {
        oss << " -> " << mapSemType(*fst->returnType);   // Sema 推断的返回类型（如期望反推 int）
    } else {
        oss << " -> auto";        // 兜底（无标注无推断，如 void 闭包）
    }

    // === 函数体 ===
    // M4/M5：外层闭包自身模板参数压栈——内层闭包引用外层闭包泛型（如 makeU 的 U）
    // 时，内层 genFunExpr 从 currentTParams_ 剔除外层泛型名（复用外层模板参数），
    // 避免内层重声明 `[]<typename U>` 遮蔽外层 → g++ -Wtemplate-body error。
    // 必须在外层闭包自身泛型剔除（上方 excludeNames 用旧 currentTParams_ 算完）之后
    // 压栈：外层闭包仍声明自身模板参数，而内层闭包生成时可见外层泛型名。兄弟闭包
    // 互不影响（退出恢复）。
    auto savedClosureTParams = currentTParams_;
    for (auto& g : genericParams) currentTParams_.push_back(g);

    oss << " {\n";
    indentLevel_++;

    // Bug 2 修复：闭包参数注册到 stringVarNames_ / valueTypeVarNames_
    // 否则 isStringExprInChain 漏判闭包内的 string 参数，用 GcString::from() 包装
    // 已是 GcString* 的变量 → 匹配 from(bool) 隐式转换 → 输出 "true"
    auto savedStringVars = stringVarNames_;
    auto savedValueVars  = valueTypeVarNames_;
    for (auto& p : e.params) {
        registerParamTracking(p);   // 填充临时集合；lambda 结束时由 restore 恢复外层
    }

    // invoke_result_t 推导声明（使用 F&& 完美转发）
    for (size_t ci = 0; ci < callableParamIndices.size(); ++ci) {
        auto* ft = dynamic_cast<const FunctionType*>(
            e.params[callableParamIndices[ci]].type.get());
        if (ft && !ft->paramTypes.empty() && !callableResultGenerics[ci].empty()) {
            // 构建 invoke_result_t<F&&, P1&&, P2&&...>
            std::string allArgs;
            for (size_t pi = 0; pi < ft->paramTypes.size(); ++pi) {
                if (pi > 0) allArgs += ", ";
                allArgs += (ft->paramTypes[pi] ? mapType(*ft->paramTypes[pi]) : "auto");
                allArgs += "&&";
            }
            // 拆分逗号分隔的泛型名（如 "U, V" → 两个 using）
            std::string remaining = callableResultGenerics[ci];
            size_t commaPos;
            while (!remaining.empty()) {
                commaPos = remaining.find(',');
                std::string g = remaining.substr(0, commaPos);
                // trim whitespace
                size_t ts = g.find_first_not_of(" \t");
                if (ts != std::string::npos) g = g.substr(ts);
                size_t te = g.find_last_not_of(" \t");
                if (te != std::string::npos) g = g.substr(0, te + 1);
                if (!g.empty()) {
                    oss << indentStr()
                        << "using " << g
                        << " = typename std::invoke_result_t<F" << ci << "&&, "
                        << allArgs << ">;\n";
                }
                if (commaPos == std::string::npos) break;
                remaining = remaining.substr(commaPos + 1);
            }
        }
    }

    // returnOnlyGenerics via captured callables（如 make_tree_mapper 闭包中的 U）
    // M4：保存外层闭包链的已声明 returnOnlyGenerics 集合——内层闭包生成 using 时感知
    // 外层已声明的别名（跳过重复声明复用外层 U）；生成 body 后恢复，兄弟闭包互不影响
    auto savedDeclaredROG = declaredReturnOnlyGenerics_;
    if (!returnOnlyGenerics.empty() && callableParamIndices.empty() && !calledCaptures.empty()) {
        // 用第一个被调用的捕获变量 + 第一个闭包模板参数（或闭包参数类型）计算
        std::string delegate = *calledCaptures.begin();
        // 尝试从闭包的第一个参数获取输入类型
        // M4：此前只对「NamedType + typeArgs[0] 非空」（如 Tree<T> → T）取类型，其余
        // （首参数为具体 NamedType 如 int，typeArgs 空）兜底 "auto" → 生成
        // std::declval<auto>() 坏 C++。现对具体类型（int → int32_t）与其他类型形态
        // （ListType/FunctionType 等）一律 mapType 整体类型；仅泛型容器保留取 typeArgs[0]
        // （make_tree_mapper 形态：delegate 输入是节点内容泛型 T 而非整棵树）。
        std::string srcType = "auto";
        if (!e.params.empty() && e.params[0].type) {
            if (auto* nt = dynamic_cast<const NamedType*>(e.params[0].type.get())) {
                // root: Tree<T> → srcType = T（delegate 输入为内容泛型）
                if (!nt->typeArgs.empty() && nt->typeArgs[0])
                    srcType = mapType(*nt->typeArgs[0]);
                else
                    srcType = mapType(*nt);   // 具体类型：int → int32_t
            } else {
                srcType = mapType(*e.params[0].type);
            }
        }
        for (auto& g : returnOnlyGenerics) {
            // M4：若 g 已由外层闭包 returnOnlyGenerics 声明 `using g`，内层直接复用外层
            // 别名（嵌套 lambda 体内可见），不再重复声明——否则内层 using U 与外层
            // using U 嵌套遮蔽，且内层按自身 delegate/srcType 推导可能与外层不一致。
            if (declaredReturnOnlyGenerics_.count(g)) continue;
            oss << indentStr()
                << "using " << g << " = decltype("
                << delegate << "(std::declval<" << srcType << ">()));\n";
            declaredReturnOnlyGenerics_.insert(g);
        }
    }

    // C3.2: 闭包返回 Optional<T> 时跟踪元素类型（none() 直转 make_none<T> 用）
    // P1-2：闭包返回含 None 联合 / 含堆联合时同样填充 union 装箱状态（供 genReturnStmt
    // return none()/record→view 装箱），并保存/恢复外层函数状态防污染
    // G2-B：同时保存/覆写/恢复 currentReturnCppType_——否则闭包体的 genReturnStmt
    // （some/裸值装箱、RecordExpr returnIsOptional）读到外层函数的返回类型，外层返回
    // Optional<X> 时闭包内 some(record)/return record 用外层元素 X 装箱（如外层
    // Optional<Iterator<Point>> + 闭包 Optional<Point> → 错用 Iterator 元素）。
    auto savedReturnElem = currentReturnElem_;
    auto savedReturnCppType = currentReturnCppType_;
    auto savedRetVariantTypes = std::move(currentReturnVariantCppTypes_);
    auto savedHasNoneVariant = currentReturnHasNoneVariant_;
    currentReturnVariantCppTypes_.clear();
    currentReturnHasNoneVariant_ = false;
    currentReturnElem_ = optionalElemOf(e.returnType.get());
    // 覆写为闭包自身返回类型（与 lambda 签名 `-> ...` 的 mapType/mapSemType 一致）：
    //   - 显式返回标注 → mapType（funSignature/methodSignature 同机制）
    //   - 无标注但 Sema 推断出具体返回类型 → mapSemType
    //   - 其余（无标注无推断 / void）→ 清空（genReturnStmt 不做任何 Optional/视图装箱）
    if (e.returnType) {
        currentReturnCppType_ = mapType(*e.returnType);
    } else if (auto* fst = dynamic_cast<const FuncSemType*>(e.inferredType);
               fst && fst->returnType
               && !dynamic_cast<const NoneSemType*>(fst->returnType.get())
               && !dynamic_cast<const ErrorSemType*>(fst->returnType.get())) {
        currentReturnCppType_ = mapSemType(*fst->returnType);
    } else {
        currentReturnCppType_.clear();
    }
    if (e.returnType && e.returnType->inferredType) {
        if (auto* u = dynamic_cast<const UnionSemType*>(e.returnType->inferredType)) {
            std::vector<std::string> cppTypes;
            bool hasHeap = false;
            for (auto& v : u->variants) {
                cppTypes.push_back(v ? mapSemType(*v) : "void");
                if (v && isUnionHeapVariant(v.get())) hasHeap = true;
                if (v && dynamic_cast<const NoneSemType*>(v.get()))
                    currentReturnHasNoneVariant_ = true;
            }
            if (hasHeap) currentReturnVariantCppTypes_ = std::move(cppTypes);
        }
    }
    for (auto& s : e.body->stmts) {
        if (s) genStmt(oss, *s, closureIsCoro);
    }
    bool lastIsReturn = !e.body->stmts.empty()
        && dynamic_cast<const ReturnStmt*>(e.body->stmts.back().get());
    // 协程闭包末尾补 co_return; 确保 C++20 将其识别为协程
    if (closureIsCoro) {
        if (!lastIsReturn)
            oss << indentStr() << "co_return;\n";
    } else if (!lastIsReturn && e.returnType
               && mapType(*e.returnType) == "aura_rt::NoneType") {
        // 显式 `-> None` 的闭包返回 NoneType（非 void），体末尾无 return 时
        // GCC 对"非 void 函数走到末尾"的未定义行为路径插入 ud2 非法指令
        // （Bug 2-A 修复前该闭包是协程、会补 co_return，故此前未暴露）
        // → 补 return aura_rt::NoneType{}; 使函数体合法
        oss << indentStr() << "return aura_rt::NoneType{};\n";
    } else if (!lastIsReturn && e.returnType
               && callableParamIndices.empty() && returnOnlyGenerics.empty()) {
        // 防御（#4）：非 None/void 返回类型闭包体末尾无 return（缺显式 return）时
        // 补 `return {};` 兜底，杜绝产出 no-return lambda（g++ 插 ud2 → 运行时崩溃）。
        // Sema inferFunExpr 已拦截非法程序，此处防 match 表达式体等漏网路径。
        // 多态闭包（-> auto，经 returnOnlyGenerics 推导返回类型）补 {} 无法推导故排除；
        // `-> void` 闭包自然走到末尾合法，不补。
        std::string cppRet = mapType(*e.returnType);
        if (cppRet != "void")
            oss << indentStr() << "return {};\n";
    }
    indentLevel_--;
    oss << indentStr() << "}";

    // Bug 2 修复：恢复 stringVarNames_ / valueTypeVarNames_，避免污染外层作用域
    stringVarNames_ = savedStringVars;
    valueTypeVarNames_ = savedValueVars;
    currentReturnElem_ = savedReturnElem;
    currentReturnCppType_ = savedReturnCppType;
    currentReturnVariantCppTypes_ = std::move(savedRetVariantTypes);
    currentReturnHasNoneVariant_ = savedHasNoneVariant;
    declaredReturnOnlyGenerics_ = savedDeclaredROG;   // M4：恢复外层闭包链声明状态
    currentTParams_ = savedClosureTParams;            // M4/M5：恢复外层模板参数栈

    lastClosureIsCoro_ = closureIsCoro;
    return oss.str();
}

} // namespace Aura
