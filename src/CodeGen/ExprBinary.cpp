#include "CodeGen.h"
#include "../Sema/BuiltinRegistry.h"
#include "../Sema/SemType.h"
#include <sstream>

namespace Aura {

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
    // bug-64：比较（==/!=）一侧为裸 none()，另一侧为 Optional<T>（元素可为模板期
    // 未绑定泛型名）时，把对端元素 C++ 名临时注入 currentReturnElem_，使 genCallExpr
    // none() 分支生成 make_none<elem>()（elem 为模板参数名时由 g++ 实例化推导）。
    // 仿 genConditionalExpr save/restore；注入须在 genExpr(left/right) 之前完成。
    std::string savedElem = currentReturnElem_;
    if (e.op == "==" || e.op == "!=") {
        std::string injectElem;
        if (isNoneCallExpr(*e.left) && e.right->inferredType)
            injectElem = optionalElemCppName(e.right->inferredType);
        else if (isNoneCallExpr(*e.right) && e.left->inferredType)
            injectElem = optionalElemCppName(e.left->inferredType);
        if (!injectElem.empty()) currentReturnElem_ = injectElem;
    }
    std::string left  = genExpr(*e.left, isCoroutine);
    std::string right = genExpr(*e.right, isCoroutine);
    currentReturnElem_ = savedElem;   // 恢复（left/right 已生成）

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
        // 泛型模板参数短路（bug-15/bug-23 统一修复点）：left/right 任一操作数 inferredType
        // 为裸 GenericSemType（resolvedName 空 = 未实例化模板参数）且 name 属于当前模板
        // 上下文 currentTParams_（泛型函数 DeclFun.cpp:66-67 / 泛型方法 :411-412 / 泛型闭包
        // ExprClosure.cpp:576-577 均压栈）→ 模板实例化前无法静态区分 T=string（GcString*）
        // 还是数值 → 生成 aura_rt::plus_generic，由 runtime if constexpr 延迟判定
        // （任一侧 string → concat，否则原生 +），经 genGcRootedArgs 包装（未绑定泛型实参
        // 依赖 bug-14 的 if constexpr 延迟判定：T=值类型不生成 GcRootHandle 假根）。
        // 必须置于 substring 判定之前：否则 `x + "!"` 仍被右侧 intern_string 子串抢先判
        // rightIsStr（bug-23 过度判定残留）。
        auto isUnboundGenericInScope = [this](const SemType* type) -> bool {
            if (!type) return false;
            auto* g = dynamic_cast<const GenericSemType*>(type);
            if (!g || !g->resolvedName.empty()) return false;
            for (const auto& tp : currentTParams_)
                if (tp == g->name) return true;
            return false;
        };
        if (isUnboundGenericInScope(e.left->inferredType)
            || isUnboundGenericInScope(e.right->inferredType)) {
            std::vector<std::pair<std::string, const SemType*>> gcArgs;
            gcArgs.emplace_back(left, e.left->inferredType);
            gcArgs.emplace_back(right, e.right->inferredType);
            return genGcRootedArgs(gcArgs, "aura_rt::plus_generic({0}, {1})", isCoroutine);
        }

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

        // bug-70：与 `+` 分支对齐的两层兜底——仅靠生成文本子串会漏判「双字符串变量」
        // 形态（genExpr(Identifier) 只产 `v.get()`，不含 make_string/concat/... 子串），
        // 落到 L239 裸指针比较 → 内容相等的动态字符串判 false（静默错误结果）。
        // 第一层：stringVarNames_ 查表（strip `.get()` 后）；第二层：Sema 推断 string 兜底。
        auto stripGet70 = [](const std::string& s) -> std::string {
            if (s.size() > 6 && s.substr(s.size() - 6) == ".get()")
                return s.substr(0, s.size() - 6);
            return s;
        };
        if (!leftIsStr && stringVarNames_.count(stripGet70(left))) leftIsStr = true;
        if (!rightIsStr && stringVarNames_.count(stripGet70(right))) rightIsStr = true;

        auto isStringSemType70 = [](const SemType* type) -> bool {
            if (!type) return false;
            if (auto* p = dynamic_cast<const PrimSemType*>(type))
                return p->kind == PrimSemType::String;
            return false;
        };
        if (!leftIsStr && isStringSemType70(e.left->inferredType)) leftIsStr = true;
        if (!rightIsStr && isStringSemType70(e.right->inferredType)) rightIsStr = true;

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

} // namespace Aura
