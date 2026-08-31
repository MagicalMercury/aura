#include "CodeGen.h"
#include "../Sema/BuiltinRegistry.h"
#include "../Sema/SemType.h"
#include <sstream>

namespace Aura {

// 从 "aura_rt::Optional<X>*" 提取 X（X 可能含嵌套尖括号，如 Optional<Optional<int>*>*）；
// 供 genUnionBoxingImpl 对 Optional 变体的 none() 生成 make_none<X>()
std::string CodeGenerator::optionalElemCpp(const std::string& cppType) {
    static const std::string prefix = "aura_rt::Optional<";
    size_t p = cppType.find(prefix);
    if (p == std::string::npos || cppType.size() < prefix.size() + 2) return "";
    // 尾部固定为 ">*"，去掉后剩余 "aura_rt::Optional<X"
    return cppType.substr(p + prefix.size(), cppType.size() - (p + prefix.size()) - 2);
}

// ============================================================
// Phase 2-③：Optional 元素容器判定（嵌套 Optional / 联合 Variant）
// ============================================================

// 判定元素 C++ 名是否为"嵌套 Optional 元素"（元素本身是 aura_rt::Optional<inner>*，
// 如 Optional<Optional<Point>> 的元素），输出内层元素 C++ 名。
static bool isNestedOptionalElemCpp(const std::string& elemCpp, std::string& inner) {
    static const std::string prefix = "aura_rt::Optional<";
    if (elemCpp.rfind(prefix, 0) != 0 || elemCpp.size() < prefix.size() + 3) return false;
    if (elemCpp.back() != '*') return false;
    size_t rt = elemCpp.rfind('>');
    if (rt == std::string::npos || rt + 1 != elemCpp.size() - 1) return false;
    inner = elemCpp.substr(prefix.size(), rt - prefix.size());
    return !inner.empty();
}

// 按顶层逗号分割 C++ 模板参数列表（"aura_rt::Variant<A, B>*" → {A, B}，含嵌套尖括号）
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

// 判定元素 C++ 名是否为"联合元素"（元素本身是 aura_rt::Variant<...>*，如
// Optional<Iterator<int>|None> 的元素），输出变体 C++ 类型列表（供 genUnionBoxingImpl
// 做 make_variant 装箱）。
static bool isVariantElemCpp(const std::string& elemCpp,
                             std::vector<std::string>& varTypes) {
    static const std::string prefix = "aura_rt::Variant<";
    if (elemCpp.rfind(prefix, 0) != 0 || elemCpp.size() < prefix.size() + 3) return false;
    if (elemCpp.back() != '*') return false;
    size_t rt = elemCpp.rfind('>');
    if (rt == std::string::npos || rt + 1 != elemCpp.size() - 1) return false;
    varTypes = splitCppTemplateArgs(elemCpp.substr(prefix.size(), rt - prefix.size()));
    return !varTypes.empty() && !varTypes[0].empty();
}

// 提取 "some(arg)" 调用的实参（callee 为 some、恰 1 实参），非 some 调用返回 nullptr。
// 嵌套 Optional 元素时：值本身是内层 some(...)（嵌套 some(some(record))）→ 对其实参
// 按内层元素递归装箱（接口视图内层需 record→view，不能走 CTAD 单层）。
static const ASTNode* innerSomeCallArg(const ASTNode& init) {
    if (auto* call = dynamic_cast<const CallExpr*>(&init)) {
        if (auto* id = dynamic_cast<const Identifier*>(call->callee.get());
            id && id->name == "some" && call->args.size() == 1)
            return call->args[0].get();
    }
    return nullptr;
}

// #1：判定初始化器是否"已是 Optional 值"（防二次装箱）：
// Identifier（Optional 变量）/ CallExpr（函数调用返回 Optional）/ MethodCallExpr
// （如 channel.receive()）/ ConditionalExpr / IndexExpr（Optional 元素列表下标
// a[i]，inferredType=元素类型）/ MemberAccessExpr（Optional 字段读取 h.opt，
// inferredType=字段声明类型），且其 inferredType 为 Optional
// （OptionalSemType 或 GenericSemType{name=="Optional"}）。与 genLetStmt OptionalSemType
// 分支的 initIsOptionalValue 判定（L465-475）保持一致；some()/none() 由
// genOptionalTargetInit 在调用前单独处理（不落入本判定）。
// 防误伤：仅按 inferredType 判定直通——非 Optional 元素列表 [Point] 下标 a[0]
// （inferredType=Point）、record 字面量 {..}（不在白名单）不会被误判为"已是 Optional"。
static bool isAlreadyOptionalValue(const ASTNode& init) {
    bool structural = dynamic_cast<const Identifier*>(&init)
        || dynamic_cast<const CallExpr*>(&init)
        || dynamic_cast<const MethodCallExpr*>(&init)
        || dynamic_cast<const ConditionalExpr*>(&init)
        || dynamic_cast<const IndexExpr*>(&init)
        || dynamic_cast<const MemberAccessExpr*>(&init);
    if (!structural || !init.inferredType) return false;
    if (dynamic_cast<const OptionalSemType*>(init.inferredType)) return true;
    if (auto* gs = dynamic_cast<const GenericSemType*>(init.inferredType))
        return gs->name == "Optional";
    return false;
}

// ============================================================
// P3b 隐式装箱（genUnionBoxing）
// 目标为含堆联合（aura_rt::Variant<T...>*）且初始值为非联合值时，
// 按初始值 SemType 定位变体索引 I，生成：
//   [&]() -> auto {
//     auto _bxN = (expr);
//     aura_rt::GcRootHandle<decltype(_bxN)> _bhxN(_bxN);   // 仅堆值
//     return aura_rt::make_variant<T1, T2, ...>(I, &_bhxN.get());
//   }()
// GcRootHandle 保护：make_variant 内部 alloc 触发 GC 时，堆临时值不被回收。
// ============================================================
std::string CodeGenerator::genUnionBoxing(const UnionSemType& u,
                                          const ASTNode& init,
                                          bool isCoroutine) {
    // 变体 C++ 类型列表 + 含堆判定
    std::vector<std::string> cppTypes;
    bool hasHeap = false;
    for (auto& v : u.variants) {
        cppTypes.push_back(v ? mapSemType(*v) : "void");
        if (v && isUnionHeapVariant(v.get())) hasHeap = true;
    }
    if (!hasHeap) return "";  // 全值联合（std::variant 路径）不走装箱
    return genUnionBoxingImpl(cppTypes, init, isCoroutine);
}

// record → 接口视图适配器 view() 预转换 IIFE（提取自 genLetStmt §3.10，供其与 genUnionBoxingImpl 共用）
// 生成：
//   [&]() -> auto {
//     RecName* _ar = (expr);
//     aura_rt::GcRootHandle<RecName*> _ah(_ar);
//     auto* _ad = aura_rt::gcConstruct<RecNameIface>(&RecNameIface::desc(), _ah.get());
//     return RecNameIface::view(_ad);
//   }()
std::string CodeGenerator::genRecordToViewIIFE(const std::string& expr,
                                               const std::string& recName,
                                               const std::string& viewCppType) {
    // viewCppType → 接口声明名（供拼适配器名 safeName(recName) + 声明名，
    // 与 genIfaceAdapter 的 adapterName 一致）：先去模板参数（如
    // "Comparable<Point*>" → "Comparable"）。内置 Iterator 的 C++ 形态带命名空间
    // 前缀（aura_rt::Iterator<T>），需剥掉前缀映射回声明名 "Iterator"
    // （与 genForStmt / ExprGen.cpp 的 Iterator 特判一致），否则会拼出
    // "Pointaura_rt::Iterator" 错误适配器名。
    std::string ifaceBaseName = viewCppType;
    size_t lt = ifaceBaseName.find('<');
    if (lt != std::string::npos) ifaceBaseName = ifaceBaseName.substr(0, lt);
    const std::string nsPrefix = "aura_rt::";
    if (ifaceBaseName.compare(0, nsPrefix.size(), nsPrefix) == 0)
        ifaceBaseName = ifaceBaseName.substr(nsPrefix.size());
    std::string adName = safeName(recName) + ifaceBaseName;
    return "[&]() -> auto {\n"
           "    " + recName + "* _ar = (" + expr + ");\n"
           "    aura_rt::GcRootHandle<" + recName + "*> _ah(_ar);\n"
           "    auto* _ad = aura_rt::gcConstruct<" + adName
           + ">(&" + adName + "::desc(), _ah.get());\n"
           "    return " + adName + "::view(_ad);\n"
           "  }()";
}

std::string CodeGenerator::genOptionalBoxIIFE(const std::string& elemCpp,
                                               const ASTNode& initExpr,
                                               bool isCoroutine) {
    if (elemCpp.empty()) return "";
    // Phase 2-③：元素 C++ 名本身是容器（嵌套 Optional<inner>* / 联合 Variant<...>*）
    // 时，值非内层元素须先内层装箱（make_optional<inner> / make_variant），否则生成
    // make_optional<Optional<inner>*>(裸值) 坏 C++；值已是内层元素（some(Optional 变量)
    // / none() / 内层 some 调用）保持单层（对照 p12b）。
    std::string nestedInner;               // 嵌套 Optional 的内层元素 C++ 名
    std::vector<std::string> variantTypes; // 联合元素变体 C++ 类型列表
    bool nestedOpt = isNestedOptionalElemCpp(elemCpp, nestedInner);
    bool variantElem = !nestedOpt && isVariantElemCpp(elemCpp, variantTypes);

    std::string valueExpr;
    bool isContainer = nestedOpt || variantElem;
    if (nestedOpt) {
        // 嵌套 Optional 元素的值形态分发：
        //   1) 值是内层 some(arg) 调用（嵌套 some(some(record))）→ 对 arg 按内层元素
        //      递归装箱（天然支持任意深度；内层为接口视图时 record→view，不能走 CTAD）
        //   2) 值已是 Optional（some(Optional 变量) / none()）→ 单层直用（值已是
        //      内层元素，防二次装箱，对照 p12b）
        //   3) 其余裸值（record 字面量 / 标量 / record 变量）→ 按内层元素装箱
        const ASTNode* innerSomeArg = innerSomeCallArg(initExpr);
        if (innerSomeArg)
            valueExpr = genOptionalBoxByElem(nestedInner, *innerSomeArg, isCoroutine);
        else if (isAlreadyOptionalValue(initExpr))
            valueExpr = genExpr(initExpr, isCoroutine);
        else
            valueExpr = genOptionalBoxByElem(nestedInner, initExpr, isCoroutine);
    } else if (variantElem) {
        // 联合元素：值非 union（genUnionBoxingImpl idx 匹配失败返回空）时先
        // make_variant 装箱；值已是 union（如 some(u)，u: Iterator<int>|None）保持单层
        std::string ubox = genUnionBoxingImpl(variantTypes, initExpr, isCoroutine);
        valueExpr = ubox.empty() ? genExpr(initExpr, isCoroutine) : ubox;
    } else {
        valueExpr = genExpr(initExpr, isCoroutine);
    }

    int oid = unionBoxingCounter_++;
    std::string ov = "_ox" + std::to_string(oid);
    std::string oh = "_ohx" + std::to_string(oid);
    // 容器元素：内层装箱结果必为 GC 堆指针（make_optional/make_variant 返回），外层
    // make_optional 内 alloc 可能触发 GC → 恒需 GcRootHandle；非容器沿用原
    // isHeapSemType(initExpr.inferredType) 判定
    bool heap = isContainer || isHeapSemType(initExpr.inferredType);
    std::ostringstream oss;
    oss << "[&]() -> auto {\n";
    oss << "    auto " << ov << " = (" << valueExpr << ");\n";
    if (heap)
        oss << "    aura_rt::GcRootHandle<decltype(" << ov << ")> " << oh
            << "(" << ov << ");\n";
    oss << "    return aura_rt::make_optional<" << elemCpp << ">("
        << (heap ? oh + ".get()" : ov) << ");\n";
    oss << "  }()";
    return oss.str();
}

// #1：接口视图元素装箱 IIFE——ViewRoot 保护视图 self 后 make_optional<elemCpp>(视图值)。
// 与 genUnionBoxingImpl 的视图变体 ViewRoot 模式一致（viewExpr 已含 self 的 GcObject*，
// compact 会重写 GcRootHandle 取最新地址，get() 重建视图）。不能复用 genOptionalBoxIIFE：
// isHeapSemType(InterfaceSemType)=true 会生成 GcRootHandle<视图>（视图非指针）→ 编译失败。
std::string CodeGenerator::genOptionalViewValueBox(const std::string& elemCpp,
                                                    const std::string& viewExpr) {
    int oid = unionBoxingCounter_++;
    std::string vv = "_ov" + std::to_string(oid);
    std::string vr = "_ovr" + std::to_string(oid);
    std::ostringstream oss;
    oss << "[&]() -> auto {\n";
    oss << "    auto " << vv << " = (" << viewExpr << ");\n";
    oss << "    aura_rt::ViewRoot<decltype(" << vv << ")> " << vr << "(" << vv << ");\n";
    oss << "    return aura_rt::make_optional<" << elemCpp << ">(" << vr << ".get());\n";
    oss << "  }()";
    return oss.str();
}

// #10/#3：record 字段值装箱——按字段声明类型（RecordSemType.fields 中与 fieldName
// 同名字段的声明类型）生成字段值表达式：
//   - 字段声明为 Optional（OptionalSemType / GenericSemType{name=="Optional"}）：
//     复用 genOptionalTargetInit 装箱（some(arg)/none()/已是 Optional 值防二次装箱，
//     裸值/record 字面量 → make_optional<elem>）
//   - 字段声明为 UnionSemType：genUnionBoxing 装箱（含堆联合 Variant 路径；
//     全值联合 std::variant 返回空 → 直赋靠隐式构造）
//   - 字段声明为接口视图（InterfaceSemType，含泛型接口 Comparable<Point>）且字段值
//     为 record → genRecordToViewIIFE 做 record→view 转换（与 genLetStmt 接口视图
//     let 同构）；字段值已是视图 → 直赋不转换。outViewValue 置 true（视图是值类型，
//     调用方必须跳过 GcRootHandle，否则 GcRootHandle<视图> 编译失败）。
//   - 其余字段类型（纯 record/list/Iterator）或 recType 非 RecordSemType
//     （无法查字段声明类型）→ 直接 genExpr，不装箱（保 t06/t12/t14 不误伤）。
std::string CodeGenerator::genRecordFieldValue(const SemType* recType,
                                               const ASTNode& fieldValue,
                                               const std::string& fieldName,
                                               bool isCoroutine,
                                               bool* outViewValue) {
    if (outViewValue) *outViewValue = false;
    const RecordSemType* rs = dynamic_cast<const RecordSemType*>(recType);
    if (!rs) return genExpr(fieldValue, isCoroutine);
    const SemType* fldTy = nullptr;
    for (auto& ft : rs->fields) {
        if (ft.name == fieldName) { fldTy = ft.type.get(); break; }
    }
    if (!fldTy) return genExpr(fieldValue, isCoroutine);

    if (auto* os = dynamic_cast<const OptionalSemType*>(fldTy)) {
        std::string elem = os->elementType ? mapSemType(*os->elementType) : "";
        if (!elem.empty())
            return genOptionalTargetInit(fieldValue, elem, isCoroutine);
    } else if (auto* gs = dynamic_cast<const GenericSemType*>(fldTy)) {
        if (gs->name == "Optional") {
            std::string elem = optionalElemCppName(fldTy);
            if (!elem.empty())
                return genOptionalTargetInit(fieldValue, elem, isCoroutine);
        } else if (gs->name == "Iterator") {
            // #5：字段声明为 Iterator 视图 + 字段值为 record → record→view 转换
            //（与下方 InterfaceSemType 字段分支同族；视图是值类型，
            //  outViewValue 置 true 让调用方跳过 GcRootHandle）
            if (outViewValue) *outViewValue = true;
            if (auto* rt = dynamic_cast<const RecordSemType*>(fieldValue.inferredType);
                rt && !rt->canonicalName.empty()) {
                std::string viewCppType = mapSemType(*fldTy);
                if (isIfaceViewTypeName(viewCppType))
                    return genRecordToViewIIFE(genExpr(fieldValue, isCoroutine),
                                               rt->canonicalName, viewCppType);
            }
            return genExpr(fieldValue, isCoroutine);
        }
    } else if (auto* u = dynamic_cast<const UnionSemType*>(fldTy)) {
        std::string boxed = genUnionBoxing(*u, fieldValue, isCoroutine);
        if (!boxed.empty()) return boxed;
    } else if (dynamic_cast<const InterfaceSemType*>(fldTy)) {
        // #3：接口视图字段（值类型）→ 调用方跳过 GcRootHandle
        if (outViewValue) *outViewValue = true;
        if (auto* rt = dynamic_cast<const RecordSemType*>(fieldValue.inferredType);
            rt && !rt->canonicalName.empty()) {
            std::string viewCppType = mapSemType(*fldTy);
            if (isIfaceViewTypeName(viewCppType))
                return genRecordToViewIIFE(genExpr(fieldValue, isCoroutine),
                                           rt->canonicalName, viewCppType);
        }
        return genExpr(fieldValue, isCoroutine);
    }
    return genExpr(fieldValue, isCoroutine);
}

// #1：按元素 C++ 类型与值形态生成 make_optional<elemCpp>(值)：
//   - 元素为接口视图（Stringer/Comparable<...>/Iterator<...>）且值为 record →
//     record→view 预转换（genRecordToViewIIFE）后装箱
//   - 元素为接口视图且值已是视图 → ViewRoot 保护后装箱
//   - 其余（普通堆 record / 值类型 / std::function / list 等）→ 显式模板参数
//     make_optional<elemCpp>(值)，堆值经 GcRootHandle 保护
std::string CodeGenerator::genOptionalBoxByElem(const std::string& elemCpp,
                                                 const ASTNode& val,
                                                 bool isCoroutine) {
    if (elemCpp.empty()) return "";
    if (isIfaceViewTypeName(elemCpp)) {
        if (auto* rt = dynamic_cast<const RecordSemType*>(val.inferredType);
            rt && !rt->canonicalName.empty()) {
            return genOptionalViewValueBox(
                elemCpp,
                genRecordToViewIIFE(genExpr(val, isCoroutine), rt->canonicalName, elemCpp));
        }
        if (val.inferredType && isIfaceView(val.inferredType))
            return genOptionalViewValueBox(elemCpp, genExpr(val, isCoroutine));
    }
    return genOptionalBoxIIFE(elemCpp, val, isCoroutine);
}

// #1：显式 Optional<X> 目标的初始化器装箱（genLetStmt else 兜底 / genReturnStmt 共用）。
// 按初始化器形态分发：
//   - some(arg)：对 arg 装箱（元素为接口视图 → record→view；元素为 std::function →
//     显式模板参数；record 字面量 → make_optional<elem>(record)；普通值 → 显式模板参数）
//   - none()：make_none<elem>（P1-1 已修路径，保持）
//   - 已是 Optional 值（Optional 变量 / 调用返回 Optional / 条件表达式）：不装箱
//     （防二次装箱），返回其裸表达式；复合表达式（条件等）内的 some() 经
//     optionalTargetElem_ 感知目标元素（genCallExpr some() 分支消费）
//   - 其余（裸值 / record 字面量）：genOptionalBoxByElem 装箱
std::string CodeGenerator::genOptionalTargetInit(const ASTNode& init,
                                                  const std::string& elemCpp,
                                                  bool isCoroutine) {
    if (elemCpp.empty()) return "";
    std::string savedElem = optionalTargetElem_;
    optionalTargetElem_ = elemCpp;   // 复合初始化器内嵌 some() 感知目标元素

    // G1 条件分支死角：ConditionalExpr 逐分支按目标元素装箱——Sema propagateCanonicalName
    // 会把顶层 inferredType 改写为 Optional（`let o: Optional<Point> = flag ? {..} : {..}`），
    // 若按 isAlreadyOptionalValue 判定会误判"已是 Optional"直赋裸 Point* 三元。逐分支
    // genOptionalTargetInit 使 `flag ? {..} : {..}` / `flag ? {..} : none()` 生成
    // `flag ? make_optional<Point*>(rec) : make_none<Point*>()`（三元分支类型一致）。
    // 分支为 Optional 变量/函数返回时 isAlreadyOptionalValue 命中 → 直通不二次装箱。
    if (auto* cond = dynamic_cast<const ConditionalExpr*>(&init)) {
        std::string t = cond->thenBranch
            ? genOptionalTargetInit(*cond->thenBranch, elemCpp, isCoroutine) : "";
        std::string el = cond->elseBranch
            ? genOptionalTargetInit(*cond->elseBranch, elemCpp, isCoroutine) : "";
        std::string result = "(" + genExpr(*cond->cond, isCoroutine) + " ? " + t + " : " + el + ")";
        optionalTargetElem_ = savedElem;
        return result;
    }

    std::string result;
    // some(arg)：提取实参装箱（arg 内嵌套 some() 需 CTAD → 临时清空目标元素）
    const ASTNode* someArg = nullptr;
    if (auto* call = dynamic_cast<const CallExpr*>(&init)) {
        if (auto* id = dynamic_cast<const Identifier*>(call->callee.get());
            id && id->name == "some" && call->args.size() == 1)
            someArg = call->args[0].get();
    }
    if (someArg) {
        optionalTargetElem_.clear();
        result = genOptionalBoxByElem(elemCpp, *someArg, isCoroutine);
    } else if (isNoneCallExpr(init)) {
        result = "aura_rt::make_none<" + elemCpp + ">()";
    } else if (isAlreadyOptionalValue(init)) {
        // 已是 Optional 值 → 直接裸引用（不装箱）
        result = genExpr(init, isCoroutine);
    } else {
        result = genOptionalBoxByElem(elemCpp, init, isCoroutine);
    }

    optionalTargetElem_ = savedElem;
    return result;
}

std::string CodeGenerator::genUnionBoxingImpl(
    const std::vector<std::string>& cppTypes,
    const ASTNode& init, bool isCoroutine) {
    // 定位变体索引 I：初始值 C++ 类型 == 某变体 C++ 类型
    // （record 需 canonicalName 已传播，如 let 场景 checkLetDecl 的 propagateCanonicalName）
    int idx = -1;
    if (init.inferredType) {
        std::string initCpp = mapSemType(*init.inferredType);
        for (size_t k = 0; k < cppTypes.size(); ++k) {
            if (cppTypes[k] == initCpp) { idx = static_cast<int>(k); break; }
        }
    }
    // none() 赋给含 None 变体的联合（如 Iterator<int> | None）：初始值推断为
    // NoneSemType，直接定位 NoneType 变体（expr 下方特判为 aura_rt::None）
    if (idx < 0 && isNoneCallExpr(init)) {
        for (size_t k = 0; k < cppTypes.size(); ++k)
            if (cppTypes[k] == "aura_rt::NoneType") { idx = static_cast<int>(k); break; }
        // none() → 联合含 Optional<X> 变体（如 u63: int | Optional<string>）：
        // Optional 变体的 None 值 = make_none<X>()（Optional<X>* 堆指针），
        // 与 NoneType 变体（aura_rt::None 值）区分开
        if (idx < 0) {
            for (size_t k = 0; k < cppTypes.size(); ++k)
                if (cppTypes[k].rfind("aura_rt::Optional<", 0) == 0) {
                    idx = static_cast<int>(k); break;
                }
        }
    }
    // 缺口 2 修复：idx 匹配失败但 init 是 record 且某变体是接口视图
    // → record→适配器 view() 预转换后装箱（复用 genRecordToViewIIFE）
    std::string viewCppType;
    if (idx < 0 && init.inferredType
        && dynamic_cast<const RecordSemType*>(init.inferredType)) {
        for (size_t k = 0; k < cppTypes.size(); ++k) {
            if (isIfaceViewTypeName(cppTypes[k])) {
                viewCppType = cppTypes[k];
                idx = static_cast<int>(k);
                break;
            }
        }
    }
    if (idx < 0) return "";  // 未匹配（联合值直接赋值 / Sema 应已报错）

    std::string expr;
    bool isIfaceViewVariant = isIfaceViewTypeName(cppTypes[idx]);
    if (isNoneCallExpr(init)) {
        if (cppTypes[idx] == "aura_rt::NoneType") {
            expr = "aura_rt::None";   // NoneType 变体：none() → NoneType 值
        } else {
            // Optional<X>* 变体（u63: int | Optional<string>）：none() → make_none<X>()
            expr = "aura_rt::make_none<" + optionalElemCpp(cppTypes[idx]) + ">()";
        }
    } else if (!viewCppType.empty()) {
        // 缺口 2：record→适配器 view() 预转换（先生成 record 指针表达式，再包 IIFE）
        auto* rt = dynamic_cast<const RecordSemType*>(init.inferredType);
        expr = genRecordToViewIIFE(genExpr(init, isCoroutine), rt->canonicalName, viewCppType);
    } else {
        expr = genExpr(init, isCoroutine);
    }

    int bid = unionBoxingCounter_++;
    // none() 的推断类型（NoneSemType）会误判堆；NoneType 是 POD 值，无需 GcRootHandle
    bool initIsHeap = isHeapSemType(init.inferredType) && !isNoneCallExpr(init);
    // make_none<X>() 返回 Optional<X>* 堆指针（make_variant 内 alloc 可能触发 GC），需保护
    if (isNoneCallExpr(init) && idx >= 0 && cppTypes[idx] != "aura_rt::NoneType")
        initIsHeap = true;
    std::ostringstream oss;
    oss << "[&]() -> auto {\n";
    oss << "    auto _bx" << bid << " = (" << expr << ");\n";
    if (isIfaceViewVariant) {
        // 缺口 1 修复：接口视图变体用 ViewRoot 包裹（保护 self，不破坏视图）
        // ViewRoot 内部 GcRootHandle<GcObject*> 持 _bx.self（正确的 GC 指针）；
        // 不能用 GcRootHandle<视图值>——compact 会把视图值前 8 字节（函数指针）当指针覆写
        oss << "    aura_rt::ViewRoot<decltype(_bx" << bid << ")> _vr" << bid
            << "(_bx" << bid << ", aura_rt::GcRootScope::ThreadLocal);\n";
        oss << "    auto* _mv" << bid << " = aura_rt::make_variant<";
        for (size_t k = 0; k < cppTypes.size(); ++k) {
            if (k > 0) oss << ", ";
            oss << cppTypes[k];
        }
        // 先 memcpy 视图值（fn 字段不受 GC 影响），再覆写 self：
        // make_variant 内部 alloc 窗口若触发 compact，_bx.self（栈上裸指针）
        // 已悬垂，用 _vr.h.get()（GcRootHandle 由 GC 更新）取最新适配器地址
        oss << ">(" << idx << ", &_bx" << bid << ");\n";
        oss << "    _mv" << bid << "->get<" << idx << ">().self = _vr" << bid << ".h.get();\n";
        oss << "    return _mv" << bid << ";\n";
    } else if (initIsHeap) {
        // 原路径：普通堆值用 GcRootHandle<T> 包裹
        oss << "    aura_rt::GcRootHandle<decltype(_bx" << bid << ")> _bhx"
            << bid << "(_bx" << bid << ");\n";
        oss << "    return aura_rt::make_variant<";
        for (size_t k = 0; k < cppTypes.size(); ++k) {
            if (k > 0) oss << ", ";
            oss << cppTypes[k];
        }
        oss << ">(" << idx << ", &_bhx" << bid << ".get());\n";
    } else {
        // 值/指针裸值：直接取地址
        oss << "    return aura_rt::make_variant<";
        for (size_t k = 0; k < cppTypes.size(); ++k) {
            if (k > 0) oss << ", ";
            oss << cppTypes[k];
        }
        oss << ">(" << idx << ", &_bx" << bid << ");\n";
    }
    oss << "  }()";
    return oss.str();
}

} // namespace Aura
