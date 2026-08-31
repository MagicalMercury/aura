#include "CodeGen.h"
#include "../Sema/BuiltinRegistry.h"
#include "../Sema/SemType.h"
#include <sstream>

namespace Aura {

void CodeGenerator::genLetStmt(std::ostream& cpp, const LetDecl& decl) {
    // 解构 let a, b = f()：临时元组成根后逐字段绑定（三形态分发与单名 let 一致）
    if (!decl.names.empty()) {
        auto* rs = dynamic_cast<const RecordSemType*>(decl.inferredType);
        if (!rs) { /* Sema 已报错，防御返回 */ return; }
        std::string tvar = "_tup_" + std::to_string(recordAllocCounter_++);
        writeLine(cpp, "auto " + tvar + "_raw = "
                  + genExpr(*decl.initializer, currentFunctionIsCoroutine_) + ";");
        writeLine(cpp, "aura_rt::GcRootHandle<decltype(" + tvar + "_raw)> "
                  + tvar + "(" + tvar + "_raw);");
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
                          + "(" + varName + "_raw);");
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
            if (!baseName.empty()
                && !typeAliasTemplateParams_.count(baseName))
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
        } else if (auto* ps = dynamic_cast<const PrimSemType*>(decl.inferredType)) {
            type = mapSemType(*ps);
        } else if (auto* os = dynamic_cast<const OptionalSemType*>(decl.inferredType)) {
            // Optional<T> 推断类型 → 映射为 aura_rt::Optional<T>*
            type = mapSemType(*os);
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
                    writeLine(cpp, "aura_rt::GcRootHandle<" + type + "> " + var + "(" + var + "_raw);");
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
                        if (f.value && isHeapSemType(f.value->inferredType) && !isViewField) {
                            std::string fv = "_fv_" + std::to_string(recIdx) + "_" + safeName(f.name);
                            std::string fh = "_fh_" + std::to_string(recIdx) + "_" + safeName(f.name);
                            writeLine(cpp, "auto " + fv + " = (" + fval + ");");
                            writeLine(cpp, "aura_rt::GcRootHandle<decltype(" + fv
                                      + ")> " + fh + "(" + fv + ");");
                            writeLine(cpp, var + ".get()->" + safeName(f.name)
                                      + " = " + fh + ".get();");
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
        currentLetName_.clear();
    }

    // 空列表 [] 修复：genListExpr 在泛型上下文中可能返回 nullptr（元素类型未知）或
    // "aura_rt::Array<X>::make(0)"（空列表生成，X 可能来自 currentTParams_ 兜底而非
    // let 目标元素类型）。用 let 声明中的类型标注精确纠正元素类型——触发条件不再硬编码
    // "Array<T>"/"Array<U>"（依赖具体泛型名，闭包内 [U] 兜底 Array<A> 时不命中，
    // bug-04 主线），改为「init 是空列表生成（含 ::make(0) 或 nullptr）且 decl.type 为
    // Array 标注」→ 用 decl.type 的 mapType 精确纠正（bug-04 let 主线）。
    if (decl.type && (init.find("nullptr") != std::string::npos
                      || init.find("::make(0)") != std::string::npos)) {
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

    std::string varName = safeName(decl.name);

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
    if (!viewRootType.empty() && !init.empty()) {
        writeLine(cpp, viewRootType + " " + varName + "_raw = " + init + ";");
        writeLine(cpp, "aura_rt::ViewRoot<" + viewRootType + "> " + varName + "(" + varName + "_raw);");
        viewRootVarNames_.insert(varName);
        viewRootTypes_[varName] = viewRootType;   // P2b：闭包捕获转 Global ViewRoot 用
    } else if (isGcPointerType(type) && !init.empty()) {
        writeLine(cpp, type + " " + varName + "_raw = " + init + ";");
        writeLine(cpp, "aura_rt::GcRootHandle<" + type + "> " + varName + "(" + varName + "_raw);");
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

    std::string varName = safeName(decl.name);

    // GC 指针类型 const 变量 → 包装为 GcRootHandle
    if (isGcPointerType(type) && !init.empty()) {
        writeLine(cpp, "const " + type + " " + varName + "_raw = " + init + ";");
        writeLine(cpp, "aura_rt::GcRootHandle<" + type + "> " + varName
                  + "(const_cast<" + type + "&>(" + varName + "_raw));");
        gcRootVarNames_.insert(varName);
        gcRootTypes_[varName] = type;
    } else {
        writeLine(cpp, "const " + type + " " + varName +
                  (init.empty() ? ";" : " = " + init + ";"));
    }
}

} // namespace Aura
