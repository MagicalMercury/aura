#include "CodeGen.h"
#include "../Sema/BuiltinRegistry.h"
#include "../Sema/SemType.h"
#include <sstream>

namespace Aura {

// ============================================================
// match（plan §4.7）
// ============================================================

void CodeGenerator::genMatchStmt(std::ostream& cpp, const MatchStmt& stmt,
                                  bool isCoroutine) {
    std::string expr = genExpr(*stmt.expr, isCoroutine);

    // 匹配值类别（依据 Sema inferredType）：
    //   - UnionSemType 含堆变体 → aura_rt::Variant<T...>*（->is<I>() / ->get<I>()）
    //   - OptionalSemType → aura_rt::Optional<T>*（is_none() / unwrap()，P3a 折叠的 T | None）
    //   - 其他（含全值 std::variant）→ 现有 holds_alternative / get 路径
    const SemType* mt = stmt.expr ? stmt.expr->inferredType : nullptr;
    bool isVariantPtr = false;   // aura_rt::Variant<T...>*
    bool isOptional   = false;   // aura_rt::Optional<T>*
    std::string elemCppType;                 // Optional 元素 C++ 类型（步骤 5 用）
    bool elemIsHeap = false;                 // Optional 元素是否为堆类型（步骤 5 用）
    std::vector<std::string> gcTmpVars;      // 本分支临时注册的 GC 根变量名（步骤 4/5 注册、步骤 6 清理）
    std::vector<std::string> variantCppTypes;  // 各变体 C++ 类型（索引对应；std::variant 与 Variant 路径共用）
    if (auto* u = dynamic_cast<const UnionSemType*>(mt)) {
        for (auto& v : u->variants) {
            if (v && isUnionHeapVariant(v.get())) isVariantPtr = true;
            variantCppTypes.push_back(v ? mapSemType(*v) : "void");
        }
    } else if (auto* os = dynamic_cast<const OptionalSemType*>(mt)) {
        isOptional = true;
        // 步骤 5：提取 Optional 元素堆判定（binding 保护用）
        // isHeapSemType 为成员函数：定义于 ExprGen.cpp L12，声明于 CodeGen.h L257（跨文件调用无障碍）
        elemCppType = mapSemType(*os->elementType);
        elemIsHeap = isHeapSemType(os->elementType.get());
        if (!elemIsHeap && !elemCppType.empty() && elemCppType.back() == '*')
            elemIsHeap = true;  // C++ 名以 * 结尾回退判定
    } else if (auto* gs = dynamic_cast<const GenericSemType*>(mt)) {
        // 显式 `Optional<T>` 注解（GenericSemType{name=="Optional"}）按 Optional 处理：
        // 与 OptionalSemType 对称——None 常量 → is_none()、类型模式绑定到元素值（unwrap）。
        // 若只修 Sema 不修此处，Generic 形态走普通路径会生成恒 true 条件
        // （None/类型模式分支恒命中）→ 运行期语义错，必须两端同修。
        if (gs->name == "Optional") {
            isOptional = true;
            // 元素 C++ 名经 optionalElemCppName 提取（TypeMap.cpp:245-284）：对堆 record
            // 自动补 *、对接口/Iterator 值视图不加 *；元素堆判定走 * 后缀回退（与
            // OptionalSemType 分支的指针后缀回退一致；接口视图值拷贝不包裹）
            elemCppType = optionalElemCppName(mt);
            if (!elemCppType.empty() && elemCppType.back() == '*')
                elemIsHeap = true;
        }
    }

    // 使用 if/else 链代替 std::visit，以正确支持 co_await
    // plan2 §4.7: match → std::visit，但 co_await 无法在 visitor 泛型 lambda 中使用
    // 改用 std::holds_alternative + std::get 替代方案
    cpp << indentStr() << "{\n";
    indentLevel_++;
    if (isVariantPtr || isOptional) {
        // 与 genLetStmt L428-432 / genUnionBoxingImpl L189-190 对齐：Ref 模式（T& 构造）
        // GC compact 经 ptr_ref_ 直接更新 _match_val 变量本身，后续 _match_val->... 拼接零改动
        writeLine(cpp, "auto _match_val = " + expr + ";");
        writeLine(cpp, "aura_rt::GcRootHandle<decltype(_match_val)> _match_rh(_match_val);");
    } else {
        // 全值 std::variant / 普通类型：无 GC 指针跨栈窗口，保持现状 auto&&（避免拷贝）
        writeLine(cpp, "auto&& _match_val = " + expr + ";");
    }

    // P5：常量匹配条件生成（联合/含堆 Variant/Optional 路径先判定变体再比值；非联合直接比较）
    auto genConstCond = [&](const ASTNode* lit) -> std::string {
        // None 常量：匹配 None 变体（Variant is<I> / Optional is_none / std::variant holds_alternative）
        if (dynamic_cast<const NoneLiteral*>(lit)) {
            if (isVariantPtr) {
                for (size_t k = 0; k < variantCppTypes.size(); ++k)
                    if (variantCppTypes[k] == "aura_rt::NoneType")
                        return "_match_val->is<" + std::to_string(k) + ">()";
                return "false";
            }
            if (isOptional) return "_match_val->is_none()";
            if (mt && dynamic_cast<const UnionSemType*>(mt)) {
                for (size_t k = 0; k < variantCppTypes.size(); ++k)
                    if (variantCppTypes[k] == "aura_rt::NoneType")
                        return "std::holds_alternative<aura_rt::NoneType>(_match_val)";
            }
            return "true";  // 非联合 None：值恒为 None，直接命中
        }

        std::string litExpr = genExpr(*lit, isCoroutine);
        // 定位字面量对应的变体 C++ 类型（联合 / Variant 路径）
        auto findVariantIdx = [&]() -> int {
            std::string want;
            if (dynamic_cast<const IntLiteral*>(lit)) want = "int32_t";
            else if (dynamic_cast<const FloatLiteral*>(lit)) want = "double";
            else if (dynamic_cast<const BoolLiteral*>(lit)) want = "bool";
            else if (dynamic_cast<const StringLiteral*>(lit)) want = "aura_rt::GcString*";
            if (want.empty()) return -1;
            for (size_t k = 0; k < variantCppTypes.size(); ++k)
                if (variantCppTypes[k] == want) return static_cast<int>(k);
            return -1;
        };

        if (isVariantPtr) {
            int idx = findVariantIdx();
            if (idx < 0) return "false";
            std::string prefix = "_match_val->is<" + std::to_string(idx) + ">() && ";
            std::string cmp = "_match_val->get<" + std::to_string(idx) + ">()";
            if (dynamic_cast<const StringLiteral*>(lit))
                return prefix + "aura_rt::string_eq(" + cmp + ", " + litExpr + ")";
            return prefix + cmp + " == " + litExpr;
        }
        if (isOptional) {
            if (dynamic_cast<const StringLiteral*>(lit))
                return "!_match_val->is_none() && aura_rt::string_eq(_match_val->unwrap(), " + litExpr + ")";
            return "!_match_val->is_none() && _match_val->unwrap() == " + litExpr;
        }
        if (mt && dynamic_cast<const UnionSemType*>(mt)) {
            int idx = findVariantIdx();
            if (idx < 0) return "false";
            std::string t = variantCppTypes[static_cast<size_t>(idx)];
            if (dynamic_cast<const StringLiteral*>(lit))
                return "std::holds_alternative<" + t + ">(_match_val) && aura_rt::string_eq(std::get<" + t + ">(_match_val), " + litExpr + ")";
            return "std::holds_alternative<" + t + ">(_match_val) && std::get<" + t + ">(_match_val) == " + litExpr;
        }
        // 非联合：直接比较
        if (dynamic_cast<const StringLiteral*>(lit))
            return "aura_rt::string_eq(_match_val, " + litExpr + ")";
        return "_match_val == " + litExpr;
    };

    for (size_t i = 0; i < stmt.cases.size(); ++i) {
        auto& c = stmt.cases[i];
        std::string branchIntro = (i > 0) ? "} else " : "";
        // P2b：本 case 临时注册的视图分支变量名（接口视图分支 ViewRoot 绑定，
        // 分支体生成完毕后 viewRootVarNames_.erase 移除，见循环末尾）
        std::string viewTmpVar;

        if (auto* tp = dynamic_cast<const TypePattern*>(c.pattern.get())) {
            std::string cppType = mapNamedType(tp->typeName);
            std::string cond;
            std::string binding;
            if (isVariantPtr) {
                // 定位变体索引（TypePattern 类型 == 某变体 C++ 类型）
                // 前缀匹配：类型模式写裸名（如 Iterator），mapNamedType 返回
                // aura_rt::Iterator，需匹配实例化变体 aura_rt::Iterator<T>
                int idx = -1;
                for (size_t k = 0; k < variantCppTypes.size(); ++k)
                    if (variantCppTypes[k] == cppType ||
                        (!cppType.empty() && variantCppTypes[k].rfind(cppType + "<", 0) == 0)) {
                        idx = static_cast<int>(k); break;
                    }
                if (idx >= 0) {
                    // 命中后 cppType 用变体真实 C++ 类型名：
                    // 视图判定（isIfaceViewTypeName）与 ViewRoot 模板参数依赖完整类型名
                    cppType = variantCppTypes[idx];
                    cond = "_match_val->is<" + std::to_string(idx) + ">()";
                    if (!tp->varName.empty()) {
                        std::string varName = safeName(tp->varName);
                        if (isIfaceViewTypeName(cppType)) {
                            // 缺口 3 修复：接口视图变体分支用 ViewRoot 包裹绑定值
                            // ViewRoot 持 self（适配器指针），分支体内 alloc 触发 GC 时
                            // self 由 GcRootHandle 更新（compact 后 .get() 取最新地址）
                            binding = "auto " + varName + "_raw = _match_val->get<"
                                    + std::to_string(idx) + ">();"
                                    + " aura_rt::ViewRoot<" + cppType + "> " + varName + "("
                                    + varName + "_raw, aura_rt::GcRootScope::ThreadLocal);";
                            // 分支体内访问 varName 走 .get()：临时注册，分支体结束后移除
                            viewRootVarNames_.insert(varName);
                            // 视图是值类型（成员访问用 "."，genMethodCall 靠此判定）
                            valueTypeVarNames_.insert(varName);
                            viewTmpVar = varName;
                        } else {
                            // 步骤 4：值拷贝到独立栈变量 +（堆指针变体）GcRootHandle Ref 模式包裹
                            // 原 auto& 为 Variant storage 槽位引用，分支体内 alloc 后悬垂；
                            // varName_raw 为独立栈变量（get 返回 T&，auto 拷贝为 T 值），
                            // varName 句柄经 ptr_ref_ 引用之 → compact 更新 varName_raw 本体，
                            // genIdentifier 对 varName 生成 .get()（与函数参数 _raw 模式一致）
                            std::string rawName = varName + "_raw";
                            binding = "auto " + rawName + " = _match_val->get<"
                                      + std::to_string(idx) + ">();";
                            if (isGcPointerType(cppType)) {
                                binding += " aura_rt::GcRootHandle<decltype(" + rawName
                                           + ")> " + varName + "(" + rawName + ");";
                                gcRootVarNames_.insert(varName);
                                gcRootTypes_[varName] = "decltype(" + rawName + ")";
                                gcTmpVars.push_back(varName);
                            } else {
                                // 值类型变体：仅拷贝（无 GC 指针，无需包裹）
                                binding = "auto " + varName + " = _match_val->get<"
                                          + std::to_string(idx) + ">();";
                            }
                        }
                    }
                } else {
                    cond = "false";  // 类型模式与任何变体不匹配（Sema 应已拦截）
                }
            } else if (isOptional) {
                cond = "!_match_val->is_none()";
                if (!tp->varName.empty()) {
                    // 步骤 5：值拷贝到独立栈变量 +（堆元素）GcRootHandle Ref 模式包裹
                    // 原裸指针拷贝在分支体内 alloc 后悬垂；值元素仅拷贝不包裹
                    std::string varName = safeName(tp->varName);
                    if (isIfaceViewTypeName(elemCppType)) {
                        // 接口/Iterator 视图元素（Optional<Greetable> / Optional<Iterator<int>>）：
                        // 与 variant 接口视图分支（L2083-2095）一致用 ViewRoot 包裹——
                        // 视图是值类型（成员访问用 "."，genMethodCall 靠 valueTypeVarNames_ 判定），
                        // 分支体内 alloc 后 .get() 重建视图取最新 self（GcRootHandle 不能包裹视图值）
                        binding = "auto " + varName + "_raw = _match_val->unwrap();"
                                + " aura_rt::ViewRoot<" + elemCppType + "> " + varName + "("
                                + varName + "_raw, aura_rt::GcRootScope::ThreadLocal);";
                        viewRootVarNames_.insert(varName);
                        valueTypeVarNames_.insert(varName);
                        viewTmpVar = varName;
                    } else if (elemIsHeap) {
                        std::string rawName = varName + "_raw";
                        binding = "auto " + rawName + " = _match_val->unwrap();"
                                + " aura_rt::GcRootHandle<decltype(" + rawName
                                + ")> " + varName + "(" + rawName + ");";
                        gcRootVarNames_.insert(varName);
                        gcRootTypes_[varName] = "decltype(" + rawName + ")";
                        gcTmpVars.push_back(varName);
                    } else {
                        // 值元素：仅拷贝（无 GC 指针，无需包裹）
                        binding = "auto " + varName + " = _match_val->unwrap();";
                    }
                }
            } else if (mt && dynamic_cast<const UnionSemType*>(mt)) {
                // 全值联合（std::variant 路径）：holds_alternative / get
                cond = "std::holds_alternative<" + cppType + ">(_match_val)";
                if (!tp->varName.empty())
                    binding = "auto& " + safeName(tp->varName) +
                              " = std::get<" + cppType + ">(_match_val);";
            } else {
                // 普通类型：类型静态已知，条件恒真，直接绑定
                cond = "true";
                if (!tp->varName.empty())
                    binding = "auto&& " + safeName(tp->varName) + " = _match_val;";
            }
            cpp << indentStr() << branchIntro << "if (" << cond << ") {\n";
            indentLevel_++;
            if (!binding.empty()) writeLine(cpp, binding);
        } else if (dynamic_cast<const ConstantPattern*>(c.pattern.get())
                   || dynamic_cast<const GroupPattern*>(c.pattern.get())) {
            // P5：常量 / `|` 分组匹配（if/else if 链，语义等价 C++ switch 多 case 合并）
            std::vector<const ASTNode*> lits;
            if (auto* cp = dynamic_cast<const ConstantPattern*>(c.pattern.get())) {
                if (cp->value) lits.push_back(cp->value.get());
            } else if (auto* gp = dynamic_cast<const GroupPattern*>(c.pattern.get())) {
                for (auto& a : gp->alts)
                    if (auto* ap = dynamic_cast<const ConstantPattern*>(a.get()))
                        if (ap->value) lits.push_back(ap->value.get());
            }
            std::string cond;
            for (size_t k = 0; k < lits.size(); ++k) {
                if (k > 0) cond += " || ";
                cond += genConstCond(lits[k]);
            }
            if (cond.empty()) cond = "false";  // 防御：空分组
            cpp << indentStr() << branchIntro << "if (" << cond << ") {\n";
            indentLevel_++;
        } else {
            // wildcard 落在 else 分支
            cpp << indentStr() << branchIntro << "{\n";
            indentLevel_++;
        }

        if (c.body) {
            if (auto* b = dynamic_cast<const BlockStmt*>(c.body.get())) {
                genBlock(cpp, *b, isCoroutine);
            } else {
                std::string bodyExpr = genExpr(*c.body, isCoroutine);
                writeLine(cpp, bodyExpr + ";");
            }
        }

        // P2b：接口视图分支的临时 viewRootVarNames_ 注册，分支体生成完毕后移除
        // （嵌套闭包捕获 varName 的分支体内仍能查到，走 genFunExpr 的 Global 转换）
        if (!viewTmpVar.empty()) {
            viewRootVarNames_.erase(viewTmpVar);
            valueTypeVarNames_.erase(viewTmpVar);
        }
        // 步骤 4/5：清理本分支临时注册的 GC 根（成对 erase 两个集合，防泄漏）
        for (auto& v : gcTmpVars) {
            gcRootVarNames_.erase(v);
            gcRootTypes_.erase(v);
        }
        gcTmpVars.clear();

        indentLevel_--;
    }

    // 关闭最后一个 if/else 分支
    cpp << indentStr() << "}\n";
    indentLevel_--;
    cpp << indentStr() << "}\n";
}

// ============================================================
// 表达式语句
// ============================================================

void CodeGenerator::genExprStmt(std::ostream& cpp, const ExprStmt& stmt,
                                 bool isCoroutine) {
    if (stmt.expr)
        writeLine(cpp, genExpr(*stmt.expr, isCoroutine) + ";");
}

} // namespace Aura
