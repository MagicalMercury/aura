#include "CodeGen.h"
#include "../Sema/BuiltinRegistry.h"
#include "../Sema/SemType.h"
#include <cctype>
#include <sstream>

namespace Aura {

// ============================================================
// isHeapSemType — 成员方法实现（原 file-static，提升为成员供 StmtGen 使用）
// ============================================================
bool CodeGenerator::isHeapSemType(const SemType* type) const {
    if (!type) return false;
    if (auto* p = dynamic_cast<const PrimSemType*>(type))
        return p->kind == PrimSemType::String;
    if (dynamic_cast<const NoneSemType*>(type)) return false;
    if (dynamic_cast<const ErrorSemType*>(type)) return false;
    // 接口/函数类型不是 GC 堆对象：
    //   - 接口是抽象类，按 const& 传递，不归 GC 管理
    //   - std::function 是 C++ RT 对象，不在 GC 堆中
    // 两者若误判为 heap，genGcRootedArgs 会生成 auto 值拷贝（接口是抽象类→编译失败）
    if (dynamic_cast<const InterfaceSemType*>(type)) return false;
    if (dynamic_cast<const FuncSemType*>(type)) return false;
    if (auto* u = dynamic_cast<const UnionSemType*>(type)) {
        for (auto& v : u->variants)
            if (isHeapSemType(v.get())) return true;
        return false;
    }
    return true;
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
    for (auto& [e, type] : args) {
        if (isHeapSemType(type)) { hasHeap = true; break; }
    }
    if (!hasHeap) {
        // 无堆类型参数：直接替换占位符返回
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
        bool isHeap = isHeapSemType(type);
        if (isHeap) {
            // 堆类型：IIFE 内 auto + GcRootHandle
            inner << "    auto " << vi << " = (" << argExpr << ");\n";
            inner << "    aura_rt::GcRootHandle<decltype(" << vi << ")> _h"
                  << hid << "_" << i << "(" << vi << ");\n";
        } else if (!awaitPrefix.empty()) {
            // 非堆 + 协程调用：IIFE 外 auto 值拷贝，生命周期跨越 co_await
            outer << "auto " << vi << " = (" << argExpr << ");\n";
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
        if (isHeapSemType(args[i].second)) {
            repl = "_h" + std::to_string(hid) + "_" + std::to_string(i) + ".get()";
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
    return "aura_rt::intern_string(\"" + e.value + "\")";
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

    return name;
}

// ============================================================
// 列表 & 记录
// ============================================================

std::string CodeGenerator::genListExpr(const ListExpr& e, bool isCoroutine) {
    if (e.elements.empty()) {
        // 优先用 inferredType 推断空列表元素类型
        if (e.inferredType) {
            auto* listTy = dynamic_cast<const ListSemType*>(e.inferredType);
            if (listTy && listTy->elementType) {
                std::string semElem = mapSemType(*listTy->elementType);
                if (semElem.find("GcObject") == std::string::npos
                    && semElem != "auto")
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
    for (auto& elem : e.elements)
        elemExprs.push_back(elem ? genExpr(*elem, isCoroutine) : "???");

    // 从第一个元素推断列表元素类型
    std::string elemType = "int32_t";  // 默认 int

    // 优先用 SemAnalyzer 推断的类型
    if (e.inferredType) {
        auto* listTy = dynamic_cast<const ListSemType*>(e.inferredType);
        if (listTy && listTy->elementType) {
            std::string semElemType = mapSemType(*listTy->elementType);
            // 无效时（GenericSemType → "auto"），回退到第一个元素的 inferredType
            if (semElemType == "auto" && e.elements.size() > 0 && e.elements[0]) {
                if (auto* rs = dynamic_cast<const RecordSemType*>(e.elements[0]->inferredType)) {
                    if (!rs->canonicalName.empty())
                        semElemType = rs->canonicalName + "*";
                }
            }
            if (semElemType.find("GcObject") == std::string::npos
                && semElemType.find("/*") == std::string::npos
                && semElemType != "auto")
                elemType = semElemType;
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
        bool isHeap = e.elements[i] && isHeapSemType(e.elements[i]->inferredType);
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
            if (!gs->resolvedName.empty()) return gs->resolvedName;
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
            std::string fval = f.value ? genExpr(*f.value, isCoroutine) : "???";
            if (f.value && isHeapSemType(f.value->inferredType)) {
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
                // 基类实例化 Comparable<Point*>：方法参数已是 Point*（record 指针），
                // 只需第一个操作数包适配器，第二个直接传 record 指针
                std::string callExpr = adapter + "({0})." + method + "({1})";
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
                    std::vector<std::pair<std::string, const SemType*>> stArgs;
                    stArgs.emplace_back(argExprs[0], e.args[0]->inferredType);
                    return genGcRootedArgs(stArgs, "{0}->to_string()", isCoroutine);
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
    // 零参构造函数（如 Stack()）需要 currentTParams_ 推断类型
    // 有参构造函数让 CTAD 从参数推导（如 Pair(p.second, p.first) → Pair_ctor(B, A)）
    std::string targs;
    if (isCtor && e.args.empty() && !currentTParams_.empty()) {
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
                        std::string recName = rt->canonicalName;
                        arg = safeName(recName) + ifaceName + "(" + arg + ")";
                    } else {
                        // 闭包/其他路径：直接包装为 IfaceFunc
                        arg = ifaceName + "Func(" + arg + ")";
                    }
                    break;
                }
            }
        }
        // 回调参数：包装裸 lambda 为 std::function
        if (cbIt != fnCallbackParams_.end()) {
            for (auto& [idx, ftStr] : cbIt->second) {
                if (idx == i) {
                    arg = ftStr + "(" + arg + ")";
                    break;
                }
            }
        }
        argExprs.push_back(arg);
    }
    // C5.1/C5.3: 同模块函数 / ctor 默认参数补齐（调用点补实参，支持任意表达式）
    if (isCtor) {
        if (auto ctIt = methodDefaultArgs_.find(calleeName); ctIt != methodDefaultArgs_.end())
            for (size_t k = e.args.size(); k < ctIt->second.size(); ++k)
                if (ctIt->second[k]) argExprs.push_back(genExpr(*ctIt->second[k], isCoroutine));
    } else if (auto fit = fnDefaultArgs_.find(calleeName); fit != fnDefaultArgs_.end()) {
        for (size_t k = e.args.size(); k < fit->second.size(); ++k)
            if (fit->second[k]) argExprs.push_back(genExpr(*fit->second[k], isCoroutine));
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
            if (i < e.args.size()) {
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
    if (auto* id = dynamic_cast<const Identifier*>(e.object.get())) {
        if (channelVarNames_.count(id->name) && (e.method == "send" || e.method == "receive")) {
            bool isSyncChannel = false;
            auto it = gcRootTypes_.find(id->name);
            if (it != gcRootTypes_.end() && it->second.find("ThreadChannel") != std::string::npos)
                isSyncChannel = true;
            if (!isSyncChannel)
                needAwait = needAwait || isCoroutine;
        }
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
        }
    }
    // 先收集参数表达式（保持参数顺序：前面的实参 + 尾部的默认参数）
    std::vector<std::string> mArgExprs;
    for (size_t i = 0; i < e.args.size(); ++i)
        mArgExprs.push_back(genExpr(*e.args[i], isCoroutine));
    // C5.3: 方法默认参数补齐（跨模块 ctor（isNsCtor）默认参数 v1 不支持）
    if (!isNs && !isNsCtor) {
        if (auto mmIt = methodDefaultArgs_.find(recvTypeKey + "." + e.method); mmIt != methodDefaultArgs_.end())
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
    auto mArgType = [&](size_t i) -> const SemType* {
        if (i < e.args.size()) return e.args[i]->inferredType;
        if (auto mmIt = methodDefaultArgs_.find(recvTypeKey + "." + e.method);
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

std::string CodeGenerator::genIndexExpr(const IndexExpr& e, bool isCoroutine) {
    std::string obj   = genExpr(*e.object, false);
    std::string idx   = genExpr(*e.index, isCoroutine);
    return "(*" + obj + ")[" + idx + "]";
}

// ============================================================
// 赋值
// ============================================================

std::string CodeGenerator::genAssignExpr(const AssignExpr& e, bool isCoroutine) {
    std::string target = genExpr(*e.target, isCoroutine);
    std::string value  = genExpr(*e.value, isCoroutine);

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

// ============================================================
// 闭包表达式 → C++20 lambda
// ============================================================

std::string CodeGenerator::genFunExpr(const FunExpr& e, bool isCoroutine) {
    if (!e.body) return "[]{}";

    // 检测闭包体内是否包含 io.xxx 调用 — 若有则在协程上下文中生成协程 lambda
    bool closureIsCoro = isCoroutine && !ioSync_ && IoDetector::scan(*e.body);

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
            std::string paramType = e.params[i].type ? mapType(*e.params[i].type) : "auto";
            oss << paramType << " " << safeName(e.params[i].name);
        }
    }
    oss << ")";

    // mutable 关键字：闭包体修改了按值捕获的变量
    if (needsMutable && !captures.empty())
        oss << " mutable";

    // 返回类型：协程闭包 → task<...>；有 callable 推导 → auto
    if (closureIsCoro) {
        if (e.returnType)
            oss << " -> aura_rt::task<" << mapType(*e.returnType) << ">";
        else
            oss << " -> aura_rt::task<void>";
    } else if (e.returnType && (!callableParamIndices.empty() || !returnOnlyGenerics.empty())) {
        oss << " -> auto";
    } else if (e.returnType) {
        oss << " -> " << mapType(*e.returnType);
    } else {
        oss << " -> auto";
    }

    // === 函数体 ===
    oss << " {\n";
    indentLevel_++;

    // Bug 2 修复：闭包参数注册到 stringVarNames_ / valueTypeVarNames_
    // 否则 isStringExprInChain 漏判闭包内的 string 参数，用 GcString::from() 包装
    // 已是 GcString* 的变量 → 匹配 from(bool) 隐式转换 → 输出 "true"
    auto savedStringVars = stringVarNames_;
    auto savedValueVars  = valueTypeVarNames_;
    for (auto& p : e.params) {
        std::string pname = safeName(p.name);
        if (!p.type) continue;
        std::string ptype = mapType(*p.type);
        // string 参数 → stringVarNames_
        if (ptype.find("aura_rt::GcString*") != std::string::npos)
            stringVarNames_.insert(pname);
        // 接口参数 → valueTypeVarNames_（引用用 . 不是 ->）
        if (auto* nt = dynamic_cast<const NamedType*>(p.type.get()))
            if (interfaceNames_.count(nt->name))
                valueTypeVarNames_.insert(pname);
        // 值类型参数 → valueTypeVarNames_
        if (auto* nt = dynamic_cast<const NamedType*>(p.type.get()))
            if ((registeredTypes_.count(nt->name) && !registeredTypes_[nt->name])
                || (BuiltinRegistry::get().findType(nt->name) != nullptr
                    && !BuiltinRegistry::get().isHeapType(nt->name)))
                valueTypeVarNames_.insert(pname);
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
    if (!returnOnlyGenerics.empty() && callableParamIndices.empty() && !calledCaptures.empty()) {
        // 用第一个被调用的捕获变量 + 第一个闭包模板参数（或闭包参数类型）计算
        std::string delegate = *calledCaptures.begin();
        // 尝试从闭包的第一个参数获取输入类型
        std::string srcType = "auto";
        if (!e.params.empty() && e.params[0].type) {
            auto* nt = dynamic_cast<const NamedType*>(e.params[0].type.get());
            if (nt) {
                // root: Tree<T> → srcType = T
                if (!nt->typeArgs.empty() && nt->typeArgs[0])
                    srcType = mapType(*nt->typeArgs[0]);
            }
        }
        for (auto& g : returnOnlyGenerics) {
            oss << indentStr()
                << "using " << g << " = decltype("
                << delegate << "(std::declval<" << srcType << ">()));\n";
        }
    }

    for (auto& s : e.body->stmts) {
        if (s) genStmt(oss, *s, closureIsCoro);
    }
    // 协程闭包末尾补 co_return; 确保 C++20 将其识别为协程
    if (closureIsCoro) {
        bool lastIsReturn = !e.body->stmts.empty()
            && dynamic_cast<const ReturnStmt*>(e.body->stmts.back().get());
        if (!lastIsReturn)
            oss << indentStr() << "co_return;\n";
    }
    indentLevel_--;
    oss << indentStr() << "}";

    // Bug 2 修复：恢复 stringVarNames_ / valueTypeVarNames_，避免污染外层作用域
    stringVarNames_ = savedStringVars;
    valueTypeVarNames_ = savedValueVars;

    lastClosureIsCoro_ = closureIsCoro;
    return oss.str();
}

} // namespace Aura
