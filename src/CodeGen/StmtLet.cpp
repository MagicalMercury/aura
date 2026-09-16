#include "CodeGen.h"
#include "../Sema/BuiltinRegistry.h"
#include "../Sema/SemType.h"
#include <sstream>

namespace Aura {

// feature-06（递归闭包辅助）："aura_rt::CallableObj<R, P1, P2>*" →
// "std::function<R(P1, P2)>"（递归闭包旧路径 lambda 的 std::function 承载；
// 按顶层逗号分割，兼容内嵌模板逗号）
static std::string callableObjToStdFunction(const std::string& cbTy) {
    static const std::string prefix = "aura_rt::CallableObj<";
    if (cbTy.rfind(prefix, 0) != 0 || cbTy.size() <= prefix.size() + 2)
        return cbTy;
    std::string inner = cbTy.substr(prefix.size(), cbTy.size() - prefix.size() - 2);
    std::vector<std::string> parts;
    std::string cur;
    int depth = 0;
    for (char c : inner) {
        if (c == '<') ++depth;
        else if (c == '>') --depth;
        else if (c == ',' && depth == 0) { parts.push_back(cur); cur.clear(); continue; }
        cur += c;
    }
    if (!cur.empty()) parts.push_back(cur);
    if (parts.empty()) return cbTy;
    for (auto& p : parts) {
        auto b = p.find_first_not_of(" \t");
        auto en = p.find_last_not_of(" \t");
        p = (b == std::string::npos) ? "" : p.substr(b, en - b + 1);
    }
    std::string ret = parts[0];
    std::string ps;
    for (size_t i = 1; i < parts.size(); ++i) {
        if (i > 1) ps += ", ";
        ps += parts[i];
    }
    return "std::function<" + ret + "(" + ps + ")>";
}

void CodeGenerator::genLetStmt(std::ostream& cpp, const LetDecl& decl) {
    // 解构 let a, b = f()：临时元组成根后逐字段绑定（三形态分发与单名 let 一致）
    if (!decl.names.empty()) {
        auto* rs = dynamic_cast<const RecordSemType*>(decl.inferredType);
        if (!rs) { /* Sema 已报错，防御返回 */ return; }
        std::string tvar = "_tup_" + std::to_string(recordAllocCounter_++);
        writeLine(cpp, "auto " + tvar + "_raw = "
                  + genExpr(*decl.initializer, currentFunctionIsCoroutine_) + ";");
        writeLine(cpp, "aura_rt::GcRootHandle<decltype(" + tvar + "_raw)> "
                  + tvar + "(" + tvar + "_raw, aura_rt::GcRootScope::ThreadLocal);");
        for (size_t i = 0; i < decl.names.size() && i < rs->fields.size(); ++i) {
            std::string varName = safeName(decl.names[i]);
            std::string fldType = rs->fields[i].type ? mapSemType(*rs->fields[i].type) : "auto";
            std::string getter = tvar + ".get()->_" + std::to_string(i);
            if (isIfaceViewTypeName(fldType)
                || fldType.rfind("aura_rt::Iterator", 0) == 0) {
                writeLine(cpp, fldType + " " + varName + "_raw = " + getter + ";");
                writeLine(cpp, "aura_rt::ViewRoot<" + fldType + "> " + varName
                          + "(" + varName + "_raw);");
                viewRootVarNames_.insert(varName);
                viewRootTypes_[varName] = fldType;
                valueTypeVarNames_.insert(varName);
            } else if (isGcPointerType(fldType)) {
                writeLine(cpp, fldType + " " + varName + "_raw = " + getter + ";");
                writeLine(cpp, "aura_rt::GcRootHandle<" + fldType + "> " + varName
                          + "(" + varName + "_raw, aura_rt::GcRootScope::ThreadLocal);");
                gcRootVarNames_.insert(varName);
                gcRootTypes_[varName] = fldType;
            } else {
                writeLine(cpp, fldType + " " + varName + " = " + getter + ";");
            }
        }
        currentLetName_.clear();
        expectedTemplateArgs_.clear();
        return;
    }

    // P1：非空 = 视图 let（接口视图 / 迭代器视图）→ 值绑定 + ViewRoot 包裹
    // 视图含 GC 指针 self，compact 不重写栈上裸指针，必须注册 self 为 GcRootHandle
    std::string viewRootType;
    std::string type;
    bool genericListDecl = false;   // #55：未绑定泛型元素列表声明（走 auto + decltype 包装）
    if (decl.type) {
        type = mapType(*decl.type);
    } else {
        // 尝试从推断的 SemType 获取 C++ 类型（仅当可生产合法 C++ 类型时使用）
        type = "auto";
        if (auto* rs = dynamic_cast<const RecordSemType*>(decl.inferredType)) {
            std::string baseName = rs->canonicalName;
            size_t anglePos = baseName.find('<');
            if (anglePos != std::string::npos)
                baseName = baseName.substr(0, anglePos);
            // #57：canonicalName 含 '<' = 已是可拼 C++ 类型串（bug-17 后合法）——
            // 具体实例化 "Tree<aura_rt::GcString*>"（main 内 let 主案）与泛型上下文
            // "Tree<T>"（模板形参保留，C++ template 上下文合法、实例化后恒 GC 对象）
            // 均放行 canonicalName + "*" → 命中 isGcPointerType → _raw + GcRootHandle
            // 包装。typeAliasTemplateParams_ 排除仅防无 '<' 裸名（"Tree*" 缺模板实参
            // 坏 C++，历史防御保留）。
            if (!baseName.empty()
                && (rs->canonicalName.find('<') != std::string::npos
                    || !typeAliasTemplateParams_.count(baseName)))
                type = rs->canonicalName + "*";
        } else if (auto* gs = dynamic_cast<const GenericSemType*>(decl.inferredType)) {
            if (!gs->resolvedName.empty()) {
                // 内置 Iterator：resolvedName 即值视图类型（aura_rt::Iterator<T>，无 *）；
                // unwrap 兜底（2026-08-26）：resolvedName 已含 '*' 或为接口/Iterator
                // 值视图 → 不再追加 '*'（否则 Optional<接口/list> 的 unwrap 元素声明侧
                // 多补 '*'，与 TypeMap::mapSemType 的 GenericSemType 分支判定一致）
                const std::string& rn = gs->resolvedName;
                if (gs->name == "Optional") {
                    // #2：无标注 let 从函数返回类型（GenericSemType{name=="Optional"}）
                    // 取声明类型时，元素 C++ 名经 optionalElemCppName + finalizeCppElem
                    // 递归补全堆 record 的 '*'（含嵌套 [Point]/Iterator<Point> 内嵌
                    // record）；否则 Optional<Point> 的 record 元素缺 '*' →
                    // Optional<Point>* 与返回侧 Optional<Point*>* 不匹配。接口/Iterator
                    // 值视图元素不加 '*'。与初始化器侧（optionalElemCppName）判定一致。
                    std::string optElem = optionalElemCppName(gs);
                    type = optElem.empty() ? rn + "*"
                                           : "aura_rt::Optional<" + optElem + ">*";
                } else if (gs->name == "Iterator" || (!rn.empty() && rn.back() == '*')
                           || isIfaceViewTypeName(rn)) {
                    // #2：迭代器/接口视图/已带 '*' 的 GenericSemType 不再追加 '*'
                    // （否则 Optional<接口/list> 的 unwrap 元素声明侧多补 '*'）。
                    // 内嵌堆 record 元素仍要补 '*'（如 Iterator<Point> 的 unwrap 元素
                    // 声明 → Iterator<Point*>，与返回侧 mapType 一致），经
                    // finalizeCppElem 递归补全；值视图元素（int32_t 等）不受影响。
                    type = finalizeCppElem(rn);
                } else {
                    type = rn + "*";
                }
                if (gs->name == "Iterator" || isIfaceViewTypeName(rn))
                    viewRootType = type;   // P1：迭代器/接口视图 → ViewRoot
            } else if (auto* ti = BuiltinRegistry::get().findType(gs->name)) {
                // BuiltinPrim::Other 类型无显式类型标注时（如 let m = sync.Mutex()）
                // 用 BuiltinRegistry.cppType（如 "aura_rt::Mutex*"）
                // channel<T> 仍要求显式类型标注（需要模板参数）
                type = ti->cppType;
            }
        } else if (auto* ls = dynamic_cast<const ListSemType*>(decl.inferredType)) {
            type = mapSemType(*ls);
            // #55：列表元素链含未绑定泛型（[T]、[[T]]）→ mapSemType 递归产
            // "Array<auto>*" / "Array<Array<auto>*>*"（auto 非法模板实参 → 坏 C++，
            // 且与 genListExpr IIFE 实际返回类型不一致）。标记声明类型为 auto，
            // 由下方 genericListDecl 分支生成 auto + GcRootHandle<decltype>。
            // 用递归判定（listContainsUnboundGeneric）覆盖嵌套 [[T]] 形态。
            if (ls->elementType && listContainsUnboundGeneric(ls)) {
                type = "auto";
                genericListDecl = true;   // #55：触发下方 auto + GcRootHandle<decltype> 包装分支
            }
        } else if (auto* ps = dynamic_cast<const PrimSemType*>(decl.inferredType)) {
            type = mapSemType(*ps);
        } else if (auto* os = dynamic_cast<const OptionalSemType*>(decl.inferredType)) {
            // Optional<T> 推断类型 → 映射为 aura_rt::Optional<T>*
            type = mapSemType(*os);
        } else if (dynamic_cast<const CallableSemType*>(decl.inferredType)) {
            // feature-06（阶段 C）：无标注 Callable 值拷贝（let g = f）→
            // aura_rt::CallableErased*（GC 堆包装——isGcPointerType 根化存储）
            type = "aura_rt::CallableErased*";
        }
    }
    // 提取类型标注中的模板参数（如 math.Pair<float, bool>），供 genMethodCall 用于跨模块构造
    expectedTemplateArgs_.clear();
    if (decl.type) {
        if (auto* nt = dynamic_cast<const NamedType*>(decl.type.get())) {
            if (!nt->typeArgs.empty()) {
                for (auto& ta : nt->typeArgs)
                    expectedTemplateArgs_.push_back(mapType(*ta));
            }
        }
    }

    std::string init;
    if (decl.initializer) {
        currentLetName_ = safeName(decl.name);

        // RecordExpr 作为 let 初始值 → gc_alloc + 字段赋值
        if (auto* rec = dynamic_cast<const RecordExpr*>(decl.initializer.get())) {
            // #2：判断目标是否为 Optional（折叠 union Point|None → OptionalSemType /
            // 显式 Optional<X> 注解 → GenericSemType{name=="Optional"} / C++ 类型前缀
            // aura_rt::Optional<）。目标为 Optional 时 record 字面量不应走 record 构造
            // （gc_alloc<Optional<...>> + ->field 非法），应作为 Optional 元素装箱：
            //   - OptionalSemType 目标 → 下方 OptionalSemType 分支的 make_optional IIFE
            //   - GenericSemType{name=="Optional"} 目标 → else 兜底前的显式装箱分支
            bool targetIsOptional = false;
            if (auto* os = dynamic_cast<const OptionalSemType*>(decl.inferredType)) {
                targetIsOptional = os->elementType != nullptr;
            } else if (auto* gs = dynamic_cast<const GenericSemType*>(decl.inferredType)) {
                targetIsOptional = gs->name == "Optional";
            }
            if (!targetIsOptional && decl.type
                && mapType(*decl.type).find("aura_rt::Optional<") == 0) {
                targetIsOptional = true;
            }
            // 检查是否有可用的记录类型名（含 decl.type 或 inferredType 中的 canonicalName）
            bool hasRecordType = false;
            std::string recType;
            // P3b：目标为联合（decl.inferredType 是 UnionSemType）时不走 record 构造，
            // record 字面量应作为联合变体装箱（make_variant），由下方隐式装箱逻辑处理
            bool targetIsUnion = dynamic_cast<const UnionSemType*>(decl.inferredType) != nullptr;
            if (!targetIsUnion && !targetIsOptional && decl.type) {
                recType = mapType(*decl.type);
                hasRecordType = true;
            } else if (!targetIsUnion && !targetIsOptional) {
                if (auto* rs = dynamic_cast<const RecordSemType*>(decl.inferredType)) {
                    if (!rs->canonicalName.empty()) {
                        recType = rs->canonicalName + "*";
                        hasRecordType = true;
                    }
                }
            }
            if (hasRecordType) {
                bool isPtr = recType.size() > 1 && recType.back() == '*';
                if (isPtr) recType.pop_back();
                if (isPtr) {
                    init = "aura_rt::gc_alloc<" + recType + ">(&" + recType + "::_desc)";
                    std::string var = safeName(decl.name);
                    writeLine(cpp, type + " " + var + "_raw = " + init + ";");
                    writeLine(cpp, "aura_rt::GcRootHandle<" + type + "> " + var + "(" + var + "_raw, aura_rt::GcRootScope::ThreadLocal);");
                    gcRootVarNames_.insert(var);
                    gcRootTypes_[var] = type;
                    int recIdx = recordAllocCounter_++;
                    // #10：字段声明类型来源（RecordSemType.fields 按字段名查声明类型装箱）；
                    // 优先 let 声明推断类型，否则 record 字面量自身推断类型
                    const SemType* fieldRecType = decl.inferredType;
                    if (!dynamic_cast<const RecordSemType*>(fieldRecType))
                        fieldRecType = rec->inferredType;
                    for (auto& f : rec->fields) {
                        // #3：接口视图字段（值类型）→ outViewValue 置 true，跳过 GcRootHandle
                        bool isViewField = false;
                        std::string fval = f.value
                            ? genRecordFieldValue(fieldRecType, *f.value, f.name,
                                                  currentFunctionIsCoroutine_, &isViewField)
                            : "???";
                        // 堆类型字段值：预求值，防止后续字段求值期间 GC 导致裸指针悬垂
                        // 用 recIdx 后缀避免同一作用域内多个 RecordExpr 的 _fv_ 变量名冲突
                        // #30：未绑定泛型字段值 → if constexpr 延迟判定（T=值不包装 /
                        // T=堆仍保护），消除 GcRootHandle<int> 假根
                        bool isHeapF = f.value && isHeapSemType(f.value->inferredType) && !isViewField;
                        bool deferredF = isHeapF && f.value
                            && isDeferredGcRoot(f.value->inferredType);
                        if (isHeapF && !deferredF) {
                            std::string fv = "_fv_" + std::to_string(recIdx) + "_" + safeName(f.name);
                            std::string fh = "_fh_" + std::to_string(recIdx) + "_" + safeName(f.name);
                            writeLine(cpp, "auto " + fv + " = (" + fval + ");");
                            writeLine(cpp, "aura_rt::GcRootHandle<decltype(" + fv
                                      + ")> " + fh + "(" + fv + ");");
                            writeLine(cpp, var + ".get()->" + safeName(f.name)
                                      + " = " + fh + ".get();");
                        } else if (deferredF) {
                            std::string fv = "_fv_" + std::to_string(recIdx) + "_" + safeName(f.name);
                            std::string fh = "_fh_" + std::to_string(recIdx) + "_" + safeName(f.name);
                            writeLine(cpp, "auto " + fv + " = (" + fval + ");");
                            writeLine(cpp, "if constexpr (std::is_convertible_v<decltype(" + fv
                                      + "), aura_rt::GcObject*>) {");
                            writeLine(cpp, "    aura_rt::GcRootHandle<decltype(" + fv + ")> "
                                      + fh + "(" + fv + ");");
                            writeLine(cpp, "    " + var + ".get()->" + safeName(f.name)
                                      + " = " + fh + ".get();");
                            writeLine(cpp, "} else {");
                            writeLine(cpp, "    " + var + ".get()->" + safeName(f.name)
                                      + " = " + fv + ";");
                            writeLine(cpp, "}");
                        } else {
                            writeLine(cpp, var + ".get()->" + safeName(f.name) + " = " + fval + ";");
                        }
                    }
                    currentLetName_.clear();
                    expectedTemplateArgs_.clear();
                    return;
                }
            }
        }

        // P3b：目标为"含 None 变体的联合"（int | None）且初始化为 none() 时，
        // 生成 NoneType 值（aura_rt::None），而非 Optional 指针（make_none<T>）
        bool unionHasNone = false;
        if (auto* u = dynamic_cast<const UnionSemType*>(decl.inferredType)) {
            for (auto& v : u->variants)
                if (v && dynamic_cast<const NoneSemType*>(v.get())) { unionHasNone = true; break; }
        }
        if (unionHasNone && decl.initializer && isNoneCallExpr(*decl.initializer)) {
            // 含 None 联合 + none()：全值联合（std::variant 路径）→ NoneType 值直接赋；
            // 含堆联合（aura_rt::Variant*，如 Iterator<int> | None）→ genUnionBoxing
            // 装箱为 make_variant<..., NoneType>（NoneType 是 POD，直接 &_bx）
            std::string boxed;
            if (auto* u = dynamic_cast<const UnionSemType*>(decl.inferredType)) {
                bool hasHeap = false;
                for (auto& v : u->variants)
                    if (v && isUnionHeapVariant(v.get())) { hasHeap = true; break; }
                if (hasHeap)
                    boxed = genUnionBoxing(*u, *decl.initializer, currentFunctionIsCoroutine_);
            }
            init = boxed.empty() ? "aura_rt::None" : boxed;
        } else {
            // P3b 隐式装箱：目标为含堆联合（Variant 指针）且初始值为非联合值
            // （int/string/record/list 字面量或表达式）→ 生成 make_variant<I> 装箱
            std::string boxed;
            if (auto* u = dynamic_cast<const UnionSemType*>(decl.inferredType)) {
                boxed = genUnionBoxing(*u, *decl.initializer, currentFunctionIsCoroutine_);
            }
            if (!boxed.empty()) {
                init = boxed;
            } else if (auto* os = dynamic_cast<const OptionalSemType*>(decl.inferredType)) {
                // P3a 折叠（T | None → Optional<T>）的 let 初始化：
                //   none() → make_none<elem>；普通值 → make_optional<elem>(值)，
                //   堆值经 GcRootHandle 保护（make_optional 内 alloc 可能触发 GC）
                std::string elemCpp = os->elementType ? mapSemType(*os->elementType) : "";
                // 初始值表达式自身产生 Optional<T> 值（变量引用 / 函数或方法调用返回
                // Optional，如 channel.receive()）→ 直接引用，避免二次装箱；
                // 字面量 / 数组 / record / none() 会被 Sema 目标类型传播为 OptionalSemType，
                // 不能当作"已是 Optional"（否则生成裸值，类型不匹配）
                // G1 条件分支死角：ConditionalExpr 不计入 initIsOptionalValue——顶层
                // inferredType 被 propagateCanonicalName 改写为 Optional，但分支可能为
                // 裸值需逐分支装箱（genOptionalTargetInit 处理，见下方 ConditionalExpr 特判）
                bool initIsOptionalValue = dynamic_cast<const Identifier*>(decl.initializer.get())
                    || dynamic_cast<const CallExpr*>(decl.initializer.get())
                    || dynamic_cast<const MethodCallExpr*>(decl.initializer.get())
                    || dynamic_cast<const IndexExpr*>(decl.initializer.get())
                    || dynamic_cast<const MemberAccessExpr*>(decl.initializer.get());
                if (isNoneCallExpr(*decl.initializer)) {
                    init = "aura_rt::make_none<" + elemCpp + ">()";
                } else if (initIsOptionalValue
                           && decl.initializer->inferredType
                           && dynamic_cast<const OptionalSemType*>(
                               decl.initializer->inferredType)) {
                    init = genExpr(*decl.initializer, currentFunctionIsCoroutine_);
                } else if (dynamic_cast<const ConditionalExpr*>(decl.initializer.get())) {
                    // G1 条件分支死角：`let o: Point|None = flag ? {..} : {..}` ——
                    // 逐分支按目标元素装箱（genOptionalTargetInit 对 ConditionalExpr
                    // 逐分支 make_optional / 直通），否则直赋裸 Point* 三元编译失败。
                    init = genOptionalTargetInit(*decl.initializer, elemCpp,
                                                 currentFunctionIsCoroutine_);
                } else if (!elemCpp.empty()) {
                    init = genOptionalBoxIIFE(elemCpp, *decl.initializer, currentFunctionIsCoroutine_);
                } else {
                    init = genExpr(*decl.initializer, currentFunctionIsCoroutine_);
                }
            } else {
                // #1/#2：显式 Optional<X> 注解（GenericSemType{name=="Optional"} /
                // NamedType Optional）→ 按初始化器形态装箱：
                //   some(arg) / 裸值直赋 / record 字面量（make_optional<elem>(record)）
                //   / none()（make_none<elem>）；元素为接口视图 → record→view，
                //   元素为 std::function → 显式模板参数；已是 Optional 值不装箱
                std::string optElem;
                bool targetIsOptional = false;
                // 优先 decl.type 的 mapType（与声明 C++ 类型一致，接口元素不误加 *；
                //   optionalElemCppName 从 resolvedName 提取接口元素可能带错星号，
                //   如 Comparable<Point> → "Comparable<Point>*" 而非 "Comparable<Point*>"）
                if (decl.type) {
                    if (auto* nt = dynamic_cast<const NamedType*>(decl.type.get());
                        nt && nt->name == "Optional" && nt->typeArgs.size() == 1) {
                        targetIsOptional = true;
                        optElem = mapType(*nt->typeArgs[0]);
                    }
                }
                if (!targetIsOptional) {
                    if (auto* gs = dynamic_cast<const GenericSemType*>(decl.inferredType);
                        gs && gs->name == "Optional") {
                        targetIsOptional = true;
                        optElem = optionalElemCppName(decl.inferredType);
                    }
                }
                if (targetIsOptional && !optElem.empty())
                    init = genOptionalTargetInit(*decl.initializer, optElem,
                                                 currentFunctionIsCoroutine_);
                else
                    init = genExpr(*decl.initializer, currentFunctionIsCoroutine_);
            }
        }
        if (lastClosureIsCoro_) {
            coroClosureNames_.insert(safeName(decl.name));
            lastClosureIsCoro_ = false;
        }
        // feature-07 Step 4（C5）：新路径协程闭包（__invoke 返回 task<T>）——
        // 不进 coroClosureNames_（会被 isFunValueCall 直呼排除误伤），改登记
        // closureTaskVars_ 作为 needAwait 信号（ExprCall L467-471）。
        if (lastClosureIsCoroTask_) {
            closureTaskVars_.insert(safeName(decl.name));
            lastClosureIsCoroTask_ = false;
        }
        // feature-12 批次 1（方案 F，GLM5.3 C′ 方案 2026-09-14）：GcUClosure 闭包
        // 变量登记——**判据改用 Sema 推断类型**（`funcTypeHasOwnUnboundGeneric`），
        // 与顺序无关（Sema 先于 CodeGen 全量完成）。
        //
        // 为什么杀掉原机制（gcUFnRetBases_ 表 + lastClosureGcUBase_ 信号）：
        //   表/信号都依赖「生成顺序」（登记在函数体输出之后、查表在 let 生成时），
        //   跨函数时被调函数可能尚未登记 → 编号错位/查不到。
        //   Sema 判据 by construction 无顺序依赖（feature-07 的 CallableObj 路径
        //   就是这个形态：mapSemType(inferredType)）。
        //
        // 判据可靠性：funcTypeHasOwnUnboundGeneric(inferredType) 为真 ⟺ 值是多态
        // 函数值 ⟹ 必然是 F 产物（现状下所有「inferredType 含自有未绑定泛型」的
        // 闭包都落泛型域 → genGcUClosure）。
        if (!decl.type && decl.inferredType && !init.empty()) {
            if (auto* fst = dynamic_cast<const FuncSemType*>(decl.inferredType);
                fst && funcTypeHasOwnUnboundGeneric(fst)) {
                uClosureVars_.insert(safeName(decl.name));
                gcULetVarNames_.insert(safeName(decl.name));
            }
            // feature-12 批次 1（缺陷 B 修复，2026-09-14）：签名降 auto 的泛型闭包
            // 工厂调用（`let r = retry(...)` / `let w = wrap(...)`）——其 Sema
            // inferredType 泛型**已物化**（如 retry → `(int) -> int`），
            // funcTypeHasOwnUnboundGeneric 恒 false → 上方判据失明。
            // 按「被调函数是否产出 F 产物」补登记（表由 funSignature 在第三遍 A
            // 填充，零顺序依赖）。
            if (auto* ce = dynamic_cast<const CallExpr*>(decl.initializer.get()))
                if (auto* cid = dynamic_cast<const Identifier*>(ce->callee.get()))
                    if (gcUClosureReturningFns_.count(cid->name)) {
                        uClosureVars_.insert(safeName(decl.name));
                        gcULetVarNames_.insert(safeName(decl.name));
                    }
        }
        currentLetName_.clear();
    }

    // 空列表 [] 修复：genListExpr 在泛型上下文中可能返回 nullptr（元素类型未知）或
    // "aura_rt::Array<X>::make(0)"（空列表生成，X 可能来自 currentTParams_ 兜底而非
    // let 目标元素类型）。用 let 声明中的类型标注精确纠正元素类型——触发条件不再硬编码
    // "Array<T>"/"Array<U>"（依赖具体泛型名，闭包内 [U] 兜底 Array<A> 时不命中，
    // bug-04 主线），改为「init 是空列表生成（含 ::make(0) 或 nullptr）且 decl.type 为
    // Array 标注」→ 用 decl.type 的 mapType 精确纠正（bug-04 let 主线）。
    // feature-06 修复：判定收紧为「整体形态匹配」——旧 find("nullptr")/find("::make(0)")
    // 子串匹配会误伤内部合法含 nullptr 的非空列表 IIFE（如 [Callable] 列表元素包装的
    // CallableObj 派生 desc 初始化串 TypeDescriptor{..., 0, nullptr}）→ 2 元素 IIFE
    // 被整体替换为 make(0) → 运行时下标越界。现在：① nullptr 形态须匹配空列表兜底
    // 注释前缀（genListExpr 空元素分支唯一产物）；② make(0) 形态须 init 整体即
    // "aura_rt::Array<...>::make(0)" 表达式（前缀 + 后缀），嵌套于 IIFE 内部的
    // make(0)/nullptr 不再触发。
    bool isEmptyListInit =
        init.find("/* empty list - element type unknown */ nullptr")
            != std::string::npos
        || (init.rfind("aura_rt::Array<", 0) == 0
            && init.size() >= 9
            && init.compare(init.size() - 9, 9, "::make(0)") == 0);
    if (decl.type && isEmptyListInit) {
        std::string arrType = mapType(*decl.type);
        // arrType 形如 "aura_rt::Array<X>*"，取元素类型 X 并生成 make(0)
        if (arrType.find("aura_rt::Array<") == 0) {
            size_t start = arrType.find("<") + 1;
            size_t end = arrType.rfind(">");
            std::string elem = arrType.substr(start, end - start);
            init = "aura_rt::Array<" + elem + ">::make(0)";
        }
    }

    expectedTemplateArgs_.clear();

    // feature-06（阶段 C）：
    // ① 标注裸 Callable 目标（C++ 类型 CallableErased*）→ init 包装为 Erased 值
    //    （函数形态 → genErasedWrap；Callable 值拷贝 → genErasedInitValue 内透传）。
    //    目标签名 sig 来自 decl.inferredType（CallableSemType.origins 单签名——
    //    Sema 传播点 1 已把 init 签名并入）。
    // ② 无标注函数形态引用（let f = double / let h = p.next / let k = Point）→
    //    init 包装为 CallableObj 值（第 2 层；与闭包 IIFE 同存储形态，下方
    //    funValue 特例按类型根化）。
    if (type == "aura_rt::CallableErased*" && decl.initializer) {
        const FuncSemType* sig = nullptr;
        if (auto* dct = dynamic_cast<const CallableSemType*>(decl.inferredType))
            if (dct->origins.size() == 1) sig = dct->origins[0].get();
        init = genErasedInitValue(*decl.initializer, sig);
    } else if (!decl.type && decl.initializer && !init.empty()
               && dynamic_cast<const FuncSemType*>(decl.inferredType)) {
        std::string wrapped = genFnRefCallableObjValue(*decl.initializer);
        if (!wrapped.empty()) init = wrapped;
    }

    std::string varName = safeName(decl.name);

    // feature-06（阶段 B）：无标注 let 推断为函数类型（FuncSemType）→ 值是 GC 堆
    // CallableObj：按类型根化存储（GcRootHandle 包装——机制性消灭闭包手工包根
    // 缺陷族）。触发范围严格限定为"确为 CallableObj 产物的初始化器"：
    //   - 新路径闭包 IIFE（[&]() -> aura_rt::CallableObj<…>）
    //   - 已根化函数值变量的拷贝（.get() 尾缀）
    //   - record fun 字段读取（let back = box.f → 字段槽内 CallableObj 指针）
    // 其余形态（auto 泛型工厂调用 compose()/retry()/make_mapper() 返回模板 lambda、
    // 旧路径 lambda 产物等）保持 auto——值非 CallableObj，不得按 CallableObj 类型
    // 绑定（否则模板 lambda 无法向 CallableObj<…>* 转换，坏 C++）。
    // feature-12 批次 1（方案 F，C′ 2026-09-14）：泛型闭包（__GcUClosure_N）let
    // 根化第 6 路——判据改用 **Sema**（`gcULetVarNames_`，由上方 Sema 判据登记），
    // **不再依赖生成顺序**。
    //
    // ⚠️ 类型串从此不需要：F 的值类型由 C++ 自行推导——生成
    //    auto g_raw = <init>;
    //    GcRootHandle<decltype(g_raw)> g(g_raw, ThreadLocal);
    // 而非显式写 `__GcUClosure_N<...>*`（后者需要跨函数传递类型串 = 顺序依赖之源）。
    if (gcULetVarNames_.count(safeName(decl.name))
        && !decl.type && !init.empty()) {
        // 标记为「按 decltype 定型」——由下方统一生成（见 funValueLetDecltype 之后的尾段）
        gcULetDecltypeVars_.insert(safeName(decl.name));
    } else if (lastClosureIsGcU_ && !lastClosureGcUBase_.empty() && !init.empty()
               && decl.initializer
               && dynamic_cast<const FunExpr*>(decl.initializer.get())) {
        // 字面量闭包（同函数内直接写 fun(...) {...}）：实例化类型已知，直接定型
        type = lastClosureGcUBase_ + "*";
        // ⚠️ 不清信号：下方 CallableObj 路径（initIsNewClosure 分支）亦读该信号。
        //
        // feature-12 批次 2（2026-09-15，桥落地期暴露）：**必须加「init 是 FunExpr
        // 字面量」守卫** —— `lastClosureIsGcU_` / `lastClosureGcUBase_` 是
        // 「**最近一次**闭包生成」的信号（`genGcUClosure` 回填），只在**函数入口**清零
        //（DeclFun.cpp:237），函数体内不清。若 let 的 init 是**内含闭包的调用**
        //（如 `let r1 = b.useCb(5)`，方法默认参数闭包在 init 求值期生成并回填信号），
        // 该信号会**残留**到外层 let → 误把 `r1`（实际返回 int）定型为
        // `__GcUClosure_N*` → 生成 `__GcUClosure_1* r1_raw = ...(返回 int)` → 坏 C++。
        // 收紧后：仅当 init **本身**就是闭包字面量时才用该信号定型。
    }

    bool funValueLetDecltype = false;
    if (!decl.type && decl.inferredType && !init.empty()
        && dynamic_cast<const FuncSemType*>(decl.inferredType)) {
        auto* fst = static_cast<const FuncSemType*>(decl.inferredType);
        // 顶层 CallableObj 闭包 IIFE 前缀（严格前缀——内嵌闭包 IIFE 的
        // "-> aura_rt::CallableObj<" 子串不得误判，防 compose 等包装 IIFE）
        static const std::string kClosurePfx = "[&]() -> aura_rt::CallableObj<";
        bool initIsNewClosure = init.rfind(kClosurePfx, 0) == 0;
        bool initIsRootedVarRef = init.size() >= 6
            && init.compare(init.size() - 6, 6, ".get()") == 0;
        // 具名函数调用返回 CallableObj（非 auto 泛型闭包工厂）→ 值即 CallableObj，
        // 按类型根化（与 make_multiplier()->fun(int)->int 形态；compose()/retry() 等
        // auto 工厂返回模板 lambda，不在 declaredFunRetTypes_ 或返回 auto → 保持 auto）
        bool initIsCallableObjFnCall = false;
        if (auto* ce = dynamic_cast<const CallExpr*>(decl.initializer.get()))
            if (auto* cid = dynamic_cast<const Identifier*>(ce->callee.get())) {
                auto rtIt = declaredFunRetTypes_.find(cid->name);
                if (rtIt != declaredFunRetTypes_.end())
                    initIsCallableObjFnCall = rtIt->second.find("aura_rt::CallableObj<") == 0;
                // feature-12 批次 1（缺陷 B 修复）：签名降 auto 的**形参透传**函数
                // （`fun pick(f: Transform<T>) -> Transform<T> { return f }`）——产物
                // 是形参承载的 CallableObj 指针（实测：体内裸 `return f;`，非 F 产物）。
                // 其 declaredFunRetTypes_ 为 "auto"（isGenClosureRet 命中）故上方
                // 判据漏判 → 这里按「函数名在 aliasRetTransparentFns_」补判，
                // 使其走 CallableObj 静态类型根化（mapSemType），而非 F 的 decltype。
                if (!initIsCallableObjFnCall
                    && aliasRetTransparentFns_.count(cid->name))
                    initIsCallableObjFnCall = true;
            }
        // record fun 字段读取（let back = box.f → box.get()->f）：值即字段槽内的
        // CallableObj 指针（record desc 追踪该槽，compact 会重写）——栈上副本须按
        // 静态类型根化（与显式标注 fun(int)->int 同语义），否则字段对象 compact
        // 搬移后裸指针悬垂、且调用点无 .get() 句柄可派发。record 方法值（p.next）
        // 已由上方 genFnRefCallableObjValue 包装为新 IIFE（initIsNewClosure 命中），
        // 此处仅剩"非方法的 fun 字段"（MemberAccess 目标对象为 record）。
        // bug-74：元素取出的函数值（let g = arr[0]）——初始化器为 IndexExpr（可嵌套
        // nested[0][0]），值即列表元素槽内的 CallableObj 指针，与「record fun 字段
        // 读取」同形态：栈上副本须按静态类型根化，否则 compact 搬移后裸指针悬垂，
        // 且调用点无 .get() 句柄可派发 → 直呼 g(x) 坏 C++。
        bool initIsElementFunValue = false;
        if (dynamic_cast<const IndexExpr*>(decl.initializer.get())
            && decl.initializer->inferredType
            && dynamic_cast<const FuncSemType*>(decl.initializer->inferredType))
            initIsElementFunValue = true;
        bool initIsFunFieldRead = false;
        if (auto* ma = dynamic_cast<const MemberAccessExpr*>(decl.initializer.get())) {
            auto* ro = ma->object && ma->object->inferredType
                ? dynamic_cast<const RecordSemType*>(ma->object->inferredType) : nullptr;
            if (ro) {
                std::string recKey = ro->canonicalName;
                size_t lt = recKey.find('<');
                if (lt != std::string::npos) recKey = recKey.substr(0, lt);
                auto mIt = recordMethods_.find(recKey);
                if (mIt == recordMethods_.end() || !mIt->second.count(ma->member))
                    initIsFunFieldRead = true;
            }
        }
        if ((initIsNewClosure || initIsRootedVarRef || initIsCallableObjFnCall
             || initIsFunFieldRead || initIsElementFunValue)
            && !lastClosureIsCoro_ && !funcTypeHasOwnUnboundGeneric(fst)) {
            // feature-07 Step 4（C6/B2）：新路径闭包（initIsNewClosure 命中）的根化
            // 类型单源取 genFunExprCallableObj 回填的基类 C++ 类型（协程 =
            // CallableObj<task<T>, A...>，非协程 = 与 mapSemType 一致）——消除协程闭包
            // mapSemType(FuncSemType) 产出内层签名与对象基类不一致的双源漂移。
            if (lastClosureIsGcU_ && !lastClosureGcUBase_.empty()) {
                // feature-12 批次 1（F）：具名模板 struct 产物的 C++ 类型为
                // 「实例化后」的 __GcUClosure_N<槽型...>（非模板待推，同工具点回填）
                // ——单源取 lastClosureGcUBase_，消除 mapSemType 与对象类型双源漂移。
                type = lastClosureGcUBase_ + "*";
            } else if (initIsNewClosure && lastClosureCppBaseIsCoro_ && !lastClosureCppBase_.empty()) {
                type = lastClosureCppBase_ + "*";   // 协程形态：task 签名基类（唯一权威源）
            } else if (semTypeIsConcrete(fst)) {
                type = mapSemType(*fst);   // 具体签名 → 静态类型 → isGcPointerType 根化
            } else {
                type = "auto";
                funValueLetDecltype = true;   // 泛型上下文（U 由外层模板提供）→ decltype 根化
            }
        }
    }

    // feature-07 Step 4（C6 补充 —— 带类型标注形态）：协程闭包（__invoke 返回
    // task<T>）的根化类型必须与对象基类一致；标注形态 mapType(FunctionType)
    // 产出的是内层签名（CallableObj<R,A...>*）——与 IIFE 实际返回类型
    // CallableObj<task<R>,A...>* 不匹配（cannot convert）。故在初始化器为
    // 新路径协程闭包 IIFE 时统一改用 lastClosureCppBase_（唯一权威源）。
    if (lastClosureCppBaseIsCoro_ && !lastClosureCppBase_.empty()
        && decl.initializer
        && dynamic_cast<const FuncSemType*>(decl.inferredType))
        type = lastClosureCppBase_ + "*";

    // P1：接口视图类型 let 绑定（let s: Stringer = rec / let c: Comparable<Point> = p）
    // init 为 record 指针 → IIFE gcConstruct 适配器 + view()；
    // 接口变量透传（init 已是视图）→ 直接赋值，不包装
    if (decl.type && !init.empty()) {
        if (dynamic_cast<const NamedType*>(decl.type.get())) {
            if (isIfaceViewTypeName(mapType(*decl.type))) {
                if (decl.initializer
                    && dynamic_cast<const RecordSemType*>(decl.initializer->inferredType)) {
                    auto* rt = dynamic_cast<const RecordSemType*>(decl.initializer->inferredType);
                    // 复用提取出的公共方法（原内联 IIFE，行为一致）
                    init = genRecordToViewIIFE(init, rt->canonicalName, mapType(*decl.type));
                }
                // 接口视图变量：成员访问用 "."（.to_string()/.less() 等）
                valueTypeVarNames_.insert(varName);
                viewRootType = type;   // P1：接口视图 → ViewRoot 包裹（self 跨 GC 保护）
            }
        }
    }
    // 无类型标注的接口视图 let（let g = make_greeting()，inferredType 为 InterfaceSemType）：
    // 同样注册 valueTypeVarNames_，保证 g.greet() 用 "." 访问
    if (!decl.type && decl.inferredType) {
        if (auto* is = dynamic_cast<const InterfaceSemType*>(decl.inferredType)) {
            valueTypeVarNames_.insert(varName);
            // 接口视图 → ViewRoot 包裹。用 mapSemType（非裸 is->name）：泛型接口实例化
            // （Box<T> 自引用返回 Box<T>，bug-20 暴露的独立缺陷）时 typeArgs 须生成完整
            // C++ 名 Box<int32_t>，否则 ViewRoot<Box> 模板实参缺失坏 C++。
            viewRootType = mapSemType(*is);
        }
    }

    // P1：视图 let（接口视图 / 迭代器视图）→ 值绑定 raw + ViewRoot 包裹
    // 视图含 self 裸指针，GC compact 不重写栈上指针，ViewRoot 内 GcRootHandle<GcObject*>
    // 在 GC 时被更新（updateAllReferences 步骤 1），get() 重建视图取最新 self
    // feature-06（递归闭包）：标注函数类型 + init 为旧路径 lambda 且自引用
    //（[&fact] 捕获自身）→ 值无法放入 CallableObj*（lambda 非 CallableObj），
    // 保持旧世界 std::function 两段式承载（先声明后赋值——自引用需变量先于
    // lambda 定义存在）。调用点直呼/自引用均经 std::function operator()。
    if (!viewRootType.empty() && !init.empty()) {
        writeLine(cpp, viewRootType + " " + varName + "_raw = " + init + ";");
        writeLine(cpp, "aura_rt::ViewRoot<" + viewRootType + "> " + varName + "(" + varName + "_raw);");
        viewRootVarNames_.insert(varName);
        viewRootTypes_[varName] = viewRootType;   // P2b：闭包捕获转 Global ViewRoot 用
    } else if (!init.empty() && init[0] == '['
               && init.find("&" + varName) != std::string::npos
               && type.find("aura_rt::CallableObj<") == 0) {
        // 递归闭包（let 变量被自身闭包引用，旧 lambda 路径产物）
        std::string sf = callableObjToStdFunction(type);
        writeLine(cpp, sf + " " + varName + ";");
        writeLine(cpp, varName + " = " + init + ";");
    } else if ((genericListDecl || funValueLetDecltype
                || gcULetDecltypeVars_.count(varName)) && !init.empty()) {
        // #55：未绑定泛型元素列表 → auto 声明 + GcRootHandle<decltype> 包装。
        // decltype(arr_raw) 在 T 实例化后为 Array<X>*（继承 GcObject），模板实参合法、
        // 无假根；gcRootTypes_ 用 decltype 形态与 DeclFun.cpp:55 / StmtMatch.cpp:186 先例一致。
        // feature-06：泛型上下文函数值 let（CallableObj<U,U>* 模板参数形态）同款——
        // decltype(raw) 为 CallableObj<U,U>*（恒 GcObject 派生指针），无假根。
        // feature-12（方案 F，C′）：F 家族多态闭包 let（__GcUClosure_N<...>*）同款——
        // **defer 到 C++ 自行推导类型**，根化不依赖任何跨函数类型串（顺序无关）。
        writeLine(cpp, "auto " + varName + "_raw = " + init + ";");
        writeLine(cpp, "aura_rt::GcRootHandle<decltype(" + varName + "_raw)> " + varName
                  + "(" + varName + "_raw, aura_rt::GcRootScope::ThreadLocal);");
        gcRootVarNames_.insert(varName);
        gcRootTypes_[varName] = "decltype(" + varName + "_raw)";
    } else if (isGcPointerType(type) && !init.empty()) {
        writeLine(cpp, type + " " + varName + "_raw = " + init + ";");
        writeLine(cpp, "aura_rt::GcRootHandle<" + type + "> " + varName + "(" + varName + "_raw, aura_rt::GcRootScope::ThreadLocal);");
        gcRootVarNames_.insert(varName);
        gcRootTypes_[varName] = type;
    } else {
        writeLine(cpp, type + " " + varName +
                  (init.empty() ? ";" : " = " + init + ";"));
    }

    // 跟踪字符串变量（用于后续 string + T 拼接检测 / s = s + x → append 优化）
    // 匹配 make_string / concat / intern_string 三种 string 生成路径
    // Bug 修复：IIFE 包裹的复杂 init（如 let i = float("Infinity")! 生成的
    //   [&]() -> auto { ...intern_string("Infinity")... }()）内部含 intern_string
    //   子串但结果不是 string → 排除 IIFE 顶层 substring 匹配
    //   （IIFE 结果类型由下方 Sema inferredType 判定，string 场景仍会被标记）
    if (!init.empty() && !(init.size() > 4 && init.compare(0, 4, "[&](") == 0) &&
        (init.find("aura_rt::make_string") != std::string::npos ||
         init.find("aura_rt::concat") != std::string::npos ||
         init.find("aura_rt::intern_string") != std::string::npos ||
         init.find("aura_rt::string_of") != std::string::npos)) {
        stringVarNames_.insert(varName);
    }
    // Sema 推断类型为 string → 标记（覆盖 IIFE / 复杂表达式返回 string 的场景）
    if (decl.inferredType) {
        if (auto* p = dynamic_cast<const PrimSemType*>(decl.inferredType)) {
            if (p->kind == PrimSemType::String)
                stringVarNames_.insert(varName);
        }
    }

    // 跟踪值类型变量（如 Path，用 . 而非 ->）
    // 排除 GC 堆指针类型：content=io.read_file(...) 的 init 是 IIFE 包装，
    // 内部可能包含 path::new_(...)，但不能因此把 GcString* 类型的 content 误判为值类型
    if (!gcRootVarNames_.count(varName) && !init.empty() && (
        init.find("path::") != std::string::npos ||
        init.find("Path(") != std::string::npos)) {
        valueTypeVarNames_.insert(varName);
    }
    if (decl.type) {
        if (auto* nt = dynamic_cast<const NamedType*>(decl.type.get())) {
            if (nt->name == "Path" || (!nt->namespacePrefix.empty() && nt->namespacePrefix[0] == "path"))
                valueTypeVarNames_.insert(varName);
        }
    }
    // Phase 4: 通过 Sema 推断类型识别值类型（Io/Path 等）
    if (decl.inferredType) {
        if (auto* g = dynamic_cast<const GenericSemType*>(decl.inferredType)) {
            if (auto* ti = BuiltinRegistry::get().findType(g->name)) {
                if (!ti->isHeap)
                    valueTypeVarNames_.insert(varName);
            }
        }
    }
    // Phase 4 fallback: 通过初始化代码模式检测 Path 值类型
    if (!init.empty()) {
        if (init.find("io.cwd()") != std::string::npos ||
            init.find("io.file_exists") != std::string::npos) {
            valueTypeVarNames_.insert(varName);
        }
    }

    // 跟踪 channel 类型变量（用于后续 method call co_await 判定和 for-in-channel 展开）
    if (!init.empty() && init.find("Channel<") != std::string::npos) {
        channelVarNames_.insert(varName);
    }

    // feature-12 批次 1（方案 F）：GcUClosure 生成信号的【单点】清除。
    // 该信号由 genGcUClosure 在生成点回填（lastClosureIsGcU_ /
    // lastClosureGcUBase_），本函数内有两处消费者（uClosureVars_ 登记 +
    // 根化类型推导）；若任一消费者提前清空，后续消费者恒假 → 变量既不登记
    // 也不根化（T3/T4 缺口的根因）。故统一在此处收尾清除。
    lastClosureIsGcU_ = false;
    lastClosureGcUBase_.clear();
    gcULetDecltypeVars_.clear();
}

void CodeGenerator::genConstStmt(std::ostream& cpp, const ConstDecl& decl) {
    std::string type = decl.type
        ? mapType(*decl.type) : "auto";

    expectedTemplateArgs_.clear();
    if (decl.type) {
        if (auto* nt = dynamic_cast<const NamedType*>(decl.type.get())) {
            if (!nt->typeArgs.empty()) {
                for (auto& ta : nt->typeArgs)
                    expectedTemplateArgs_.push_back(mapType(*ta));
            }
        }
    }

    // P3b：目标为"含 None 变体的联合"且初始化为 none() 时生成 NoneType 值
    std::string init;
    if (decl.initializer) {
        bool unionHasNone = false;
        if (auto* u = dynamic_cast<const UnionSemType*>(decl.inferredType)) {
            for (auto& v : u->variants)
                if (v && dynamic_cast<const NoneSemType*>(v.get())) { unionHasNone = true; break; }
        }
        if (unionHasNone && isNoneCallExpr(*decl.initializer)) {
            init = "aura_rt::None";
        } else {
            // P3b 隐式装箱：目标为含堆联合（Variant 指针）且初始值为非联合值 → make_variant
            std::string boxed;
            if (auto* u = dynamic_cast<const UnionSemType*>(decl.inferredType)) {
                boxed = genUnionBoxing(*u, *decl.initializer, currentFunctionIsCoroutine_);
            }
            if (!boxed.empty())
                init = boxed;
            else
                init = genExpr(*decl.initializer, currentFunctionIsCoroutine_);
        }
    }

    expectedTemplateArgs_.clear();

    // feature-06（阶段 C）：标注裸 Callable 目标 → init 包装为 Erased 值；
    // 无标注函数形态引用 → CallableObj 值包装（同 genLetStmt）
    if (type == "aura_rt::CallableErased*" && decl.initializer) {
        const FuncSemType* sig = nullptr;
        if (auto* dct = dynamic_cast<const CallableSemType*>(decl.inferredType))
            if (dct->origins.size() == 1) sig = dct->origins[0].get();
        init = genErasedInitValue(*decl.initializer, sig);
    } else if (!decl.type && decl.initializer && !init.empty()
               && dynamic_cast<const FuncSemType*>(decl.inferredType)) {
        std::string wrapped = genFnRefCallableObjValue(*decl.initializer);
        if (!wrapped.empty()) init = wrapped;
    } else if (!decl.type && decl.initializer
               && dynamic_cast<const CallableSemType*>(decl.inferredType)) {
        // 无标注 Callable 值拷贝 → CallableErased*（根化）
        type = "aura_rt::CallableErased*";
    }

    std::string varName = safeName(decl.name);

    // feature-06（阶段 B）：无标注 const 推断为函数类型（具体 FuncSemType）→
    // CallableObj 值按静态类型根化（与 genLetStmt 同机制；旧 lambda 产物保持 auto）。
    if (!decl.type && decl.inferredType && !init.empty()
        && dynamic_cast<const FuncSemType*>(decl.inferredType)) {
        auto* fst = static_cast<const FuncSemType*>(decl.inferredType);
        static const std::string kClosurePfx = "[&]() -> aura_rt::CallableObj<";
        bool initIsNewClosure = init.rfind(kClosurePfx, 0) == 0;
        bool initIsOldLambda = init[0] == '[' && !initIsNewClosure;
        bool initIsCallableObjFnCall = false;
        if (auto* ce = dynamic_cast<const CallExpr*>(decl.initializer.get()))
            if (auto* cid = dynamic_cast<const Identifier*>(ce->callee.get())) {
                auto rtIt = declaredFunRetTypes_.find(cid->name);
                if (rtIt != declaredFunRetTypes_.end())
                    initIsCallableObjFnCall = rtIt->second.find("aura_rt::CallableObj<") == 0;
            }
        // record fun 字段读取（const back = box.f）：与 genLetStmt 同——字段槽内
        // CallableObj 指针须按静态类型根化（compact 搬移后调用点 .get() 取新址）
        // bug-74：元素取出的函数值（const g = arr[0]）——与 genLetStmt 同（元素槽内
        // CallableObj 指针须按静态类型根化，compact 搬移后调用点 .get() 取新址）
        bool initIsElementFunValue = false;
        if (dynamic_cast<const IndexExpr*>(decl.initializer.get())
            && decl.initializer->inferredType
            && dynamic_cast<const FuncSemType*>(decl.initializer->inferredType))
            initIsElementFunValue = true;
        bool initIsFunFieldRead = false;
        if (auto* ma = dynamic_cast<const MemberAccessExpr*>(decl.initializer.get())) {
            auto* ro = ma->object && ma->object->inferredType
                ? dynamic_cast<const RecordSemType*>(ma->object->inferredType) : nullptr;
            if (ro) {
                std::string recKey = ro->canonicalName;
                size_t lt = recKey.find('<');
                if (lt != std::string::npos) recKey = recKey.substr(0, lt);
                auto mIt = recordMethods_.find(recKey);
                if (mIt == recordMethods_.end() || !mIt->second.count(ma->member))
                    initIsFunFieldRead = true;
            }
        }
        // feature-07 Step 4（C7/B2）：与 C6 平行——新路径闭包根化类型单源取
        // lastClosureCppBase_（协程 = CallableObj<task<T>, A...>）；非新闭包保持原路径。
        if (!initIsOldLambda && !lastClosureIsCoro_
            && (initIsNewClosure || initIsCallableObjFnCall || initIsFunFieldRead
                || initIsElementFunValue)) {
            if (initIsNewClosure && lastClosureCppBaseIsCoro_ && !lastClosureCppBase_.empty())
                type = lastClosureCppBase_ + "*";   // 协程形态（同 C6）
            else if (semTypeIsConcrete(fst))
                type = mapSemType(*fst);
        }
    }

    // GC 指针类型 const 变量 → 包装为 GcRootHandle
    if (isGcPointerType(type) && !init.empty()) {
        writeLine(cpp, "const " + type + " " + varName + "_raw = " + init + ";");
        writeLine(cpp, "aura_rt::GcRootHandle<" + type + "> " + varName
                  + "(const_cast<" + type + "&>(" + varName + "_raw), aura_rt::GcRootScope::ThreadLocal);");
        gcRootVarNames_.insert(varName);
        gcRootTypes_[varName] = type;
    } else {
        writeLine(cpp, "const " + type + " " + varName +
                  (init.empty() ? ";" : " = " + init + ";"));
    }
}

} // namespace Aura
