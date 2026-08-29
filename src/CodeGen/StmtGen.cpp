#include "CodeGen.h"
#include "../Sema/BuiltinRegistry.h"
#include "../Sema/SemType.h"
#include <sstream>

namespace Aura {

// ============================================================
// 块
// ============================================================

void CodeGenerator::genBlock(std::ostream& cpp, const BlockStmt& block,
                              bool isCoroutine) {
    for (auto& s : block.stmts) {
        if (s) genStmt(cpp, *s, isCoroutine);
    }
}

// ============================================================
// 语句调度
// ============================================================

void CodeGenerator::genStmt(std::ostream& cpp, const Stmt& stmt,
                             bool isCoroutine) {
    if (auto* b = dynamic_cast<const BlockStmt*>(&stmt))
        { for (auto& s : b->stmts) if (s) genStmt(cpp, *s, isCoroutine); return; }
    if (auto* l = dynamic_cast<const LetDecl*>(&stmt))
        { genLetStmt(cpp, *l); return; }
    if (auto* cn = dynamic_cast<const ConstDecl*>(&stmt))
        { genConstStmt(cpp, *cn); return; }
    if (auto* r = dynamic_cast<const ReturnStmt*>(&stmt))
        { genReturnStmt(cpp, *r, isCoroutine); return; }
    if (auto* t = dynamic_cast<const ThrowStmt*>(&stmt))
        { genThrowStmt(cpp, *t); return; }
    if (auto* i = dynamic_cast<const IfStmt*>(&stmt))
        { genIfStmt(cpp, *i, isCoroutine); return; }
    if (auto* w = dynamic_cast<const WhileStmt*>(&stmt))
        { genWhileStmt(cpp, *w, isCoroutine); return; }
    if (auto* f = dynamic_cast<const ForStmt*>(&stmt))
        { genForStmt(cpp, *f, isCoroutine); return; }
    if (auto* o = dynamic_cast<const LoopStmt*>(&stmt))
        { genLoopStmt(cpp, *o, isCoroutine); return; }
    if (dynamic_cast<const BreakStmt*>(&stmt))
        { genBreakStmt(cpp); return; }
    if (dynamic_cast<const ContinueStmt*>(&stmt))
        { genContinueStmt(cpp); return; }
    if (auto* tc = dynamic_cast<const TryCatchStmt*>(&stmt))
        { genTryCatchStmt(cpp, *tc, isCoroutine); return; }
    if (auto* s = dynamic_cast<const SyncStmt*>(&stmt))
        { genSyncStmt(cpp, *s, isCoroutine); return; }
    if (auto* sf = dynamic_cast<const SyncForStmt*>(&stmt))
        { genSyncForStmt(cpp, *sf, isCoroutine); return; }
    if (auto* sp = dynamic_cast<const SpawnStmt*>(&stmt))
        { genSpawnStmt(cpp, *sp, isCoroutine); return; }
    if (auto* l = dynamic_cast<const LockStmt*>(&stmt))
        { genLockStmt(cpp, *l, isCoroutine); return; }
    if (auto* m = dynamic_cast<const MatchStmt*>(&stmt))
        { genMatchStmt(cpp, *m, isCoroutine); return; }
    if (auto* e = dynamic_cast<const ExprStmt*>(&stmt))
        { genExprStmt(cpp, *e, isCoroutine); return; }
}

// ============================================================
// 变量声明
// ============================================================

// P3b：识别 none() 调用（Optional 占位构造，make_none<T>）
// 当赋值目标是"含 None 变体的联合"（int | None）时，应生成 NoneType 值 aura_rt::None
bool CodeGenerator::isNoneCallExpr(const ASTNode& e) {
    if (auto* ce = dynamic_cast<const CallExpr*>(&e)) {
        if (auto* id = dynamic_cast<const Identifier*>(ce->callee.get()))
            return id->name == "none" && ce->args.empty();
    }
    return false;
}

// 从 "aura_rt::Optional<X>*" 提取 X（X 可能含嵌套尖括号，如 Optional<Optional<int>*>*）；
// 供 genUnionBoxingImpl 对 Optional 变体的 none() 生成 make_none<X>()
static std::string optionalElemCpp(const std::string& cppType) {
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

    // 空列表 [] 修复：genListExpr 在泛型上下文中可能返回 nullptr 或 Array<T>::make(0)
    // 用 let 声明中的类型标注取正确元素类型
    if (decl.type && (init.find("nullptr") != std::string::npos
                      || init.find("Array<T>") != std::string::npos
                      || init.find("Array<U>") != std::string::npos)) {
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
            viewRootType = is->name;   // P1：接口视图 → ViewRoot 包裹
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

// ============================================================
// 控制流
// ============================================================

void CodeGenerator::genReturnStmt(std::ostream& cpp, const ReturnStmt& stmt,
                                   bool isCoroutine) {
    std::string prefix = isCoroutine ? "co_return" : "return";

    // P3b：函数返回"含堆联合"（Variant 指针）且返回值为非联合值时 → 隐式装箱 make_variant<I>
    // 优先于 RecordExpr 特判：record 字面量作为联合变体时经 genRecordExpr 生成 T* 后装箱
    bool returnIsUnion = !currentReturnVariantCppTypes_.empty() && stmt.expr;
    if (returnIsUnion && !dynamic_cast<const UnionSemType*>(stmt.expr->inferredType)) {
        std::string boxed = genUnionBoxingImpl(currentReturnVariantCppTypes_, *stmt.expr, isCoroutine);
        if (!boxed.empty()) {
            writeLine(cpp, prefix + " " + boxed + ";");
            return;
        }
    }

    // P1-2：函数返回"含 None 变体的全值联合"（std::variant<T..., NoneType>）且
    // return none() → 生成 None 变体值 aura_rt::None（std::variant 由 NoneType 直接
    // 构造），而非 make_none<...>（Optional 指针，类型不匹配）。含堆联合（Variant*）
    // 已由上方 currentReturnVariantCppTypes_ 装箱处理；`-> Point|None` 折叠为
    // Optional 的返回由 none() inferredType/currentReturnElem_ 路径（make_none<elem>）处理。
    if (stmt.expr && isNoneCallExpr(*stmt.expr) && currentReturnHasNoneVariant_
        && currentReturnVariantCppTypes_.empty()) {
        writeLine(cpp, prefix + " aura_rt::None;");
        return;
    }

    // #1：显式 Optional<X> / 折叠 Point|None 返回类型 + 非 record 返回值 → 装箱。
    //   some(p)（元素接口视图 → record→view）、some(lambda)（→ 显式模板参数）、
    //   裸值 p/7（→ make_optional<X>）、已是 Optional 值（→ 裸返回不装箱）。
    //   record 字面量由下方 RecordExpr 分支处理（其 recType 取自 optElem 更直接）；
    //   none() 已由上方处理；`-> int|None` 全值 variant（非 Optional）不落入。
    if (stmt.expr && !dynamic_cast<const RecordExpr*>(stmt.expr.get())
        && currentReturnCppType_.rfind("aura_rt::Optional<", 0) == 0) {
        std::string optElem = optionalElemCpp(currentReturnCppType_);
        if (!optElem.empty()) {
            std::string boxed = genOptionalTargetInit(*stmt.expr, optElem, isCoroutine);
            writeLine(cpp, prefix + " " + boxed + ";");
            return;
        }
    }

    // #3：返回类型为接口视图（-> Stringer / -> Greetable / -> Comparable<Point>）且
    // 返回值为 record 变量/表达式 → record→view 转换（与 genLetStmt 接口视图 let 同构）。
    // record 字面量由下方 RecordExpr 分支处理；返回值已是视图 → 落兜底直返。
    if (stmt.expr && !dynamic_cast<const RecordExpr*>(stmt.expr.get())
        && isIfaceViewTypeName(currentReturnCppType_)) {
        if (auto* rt = dynamic_cast<const RecordSemType*>(stmt.expr->inferredType);
            rt && !rt->canonicalName.empty()) {
            writeLine(cpp, prefix + " " + genRecordToViewIIFE(
                genExpr(*stmt.expr, isCoroutine), rt->canonicalName,
                currentReturnCppType_) + ";");
            return;
        }
    }

    // RecordExpr 在 return 语句中 → 生成 gc_alloc + 字段赋值
    if (stmt.expr && dynamic_cast<const RecordExpr*>(stmt.expr.get())) {
        auto* rec = static_cast<const RecordExpr*>(stmt.expr.get());
        // #2：函数返回 Optional（Point|None 折叠 / 显式 Optional<T> 标注，
        // currentReturnCppType_ = aura_rt::Optional<elem>）且返回 record 字面量
        // → 构造 record 后 make_optional<elem>(record) 装箱返回（否则裸返回 T*
        // 与 Optional<T> 返回类型不匹配，C++ 编译失败）
        bool returnIsOptional = currentReturnCppType_.find("aura_rt::Optional<") == 0;
        std::string optElem;
        if (returnIsOptional) {
            auto lt = currentReturnCppType_.find('<');
            auto rt = currentReturnCppType_.rfind('>');
            if (lt != std::string::npos && rt != std::string::npos && rt > lt)
                optElem = currentReturnCppType_.substr(lt + 1, rt - lt - 1);
            if (optElem.empty()) returnIsOptional = false;  // 提取失败回退原逻辑
        }
        // 优先从表达式 inferredType 取类型，其次从当前函数返回类型
        std::string recType;
        const RecordSemType* rs = dynamic_cast<const RecordSemType*>(stmt.expr->inferredType);
        if (returnIsOptional) {
            recType = optElem;
        } else if (rs && !rs->canonicalName.empty()) {
            recType = rs->canonicalName + "*";
        } else {
            recType = currentReturnCppType_;
        }
        bool isPtr = false;
        if (recType.size() > 1 && recType.back() == '*') {
            recType.pop_back();
            isPtr = true;
        }
        // 如果是 aura_rt::task<T>，提取 T
        // "aura_rt::task<" 长度为 14：内层 T 起始于下标 14，止于末尾 '>' 前（substr 长度 = size - 15）
        // 原 substr(15, size-16) 偏移 1，会漏掉首字符且多截末尾，此处修正
        if (recType.find("aura_rt::task<") == 0 && recType.size() > 15) {
            recType = recType.substr(14, recType.size() - 15);
            if (!recType.empty() && recType.back() == '*') { recType.pop_back(); isPtr = true; }
        }

        if (isPtr) {
            int recIdx = recordAllocCounter_++;
            std::string var = "_rec_" + std::to_string(recIdx);
            writeLine(cpp, "auto* _raw = aura_rt::gc_alloc<" + recType
                      + ">(&" + recType + "::_desc);");
            writeLine(cpp, "aura_rt::GcRootHandle<decltype(_raw)> " + var + "(_raw);");
            for (auto& f : rec->fields) {
                // #10：按字段声明类型（rs->fields）装箱（Optional/Variant 字段）；
                // rs 为 nullptr（returnIsOptional 等场景）时直赋不误伤
                // #3：接口视图字段（值类型）→ outViewValue 置 true，跳过 GcRootHandle
                bool isViewField = false;
                std::string fval = f.value
                    ? genRecordFieldValue(rs, *f.value, f.name, isCoroutine, &isViewField) : "???";
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
            writeLine(cpp, prefix + " "
                + (returnIsOptional
                   ? "aura_rt::make_optional<" + optElem + ">(" + var + ".get())"
                   : var + ".get()")
                + ";");
            return;
        }
    }

    if (stmt.expr)
        writeLine(cpp, prefix + " " + genExpr(*stmt.expr, isCoroutine) + ";");
    else
        writeLine(cpp, prefix + ";");
}

void CodeGenerator::genThrowStmt(std::ostream& cpp, const ThrowStmt& stmt) {
    if (stmt.expr) {
        if (auto* rec = dynamic_cast<const RecordExpr*>(stmt.expr.get())) {
            std::string kind, message;
            for (auto& f : rec->fields) {
                if (f.name == "kind")    kind    = f.value ? genExpr(*f.value, false) : "???";
                if (f.name == "message") message = f.value ? genExpr(*f.value, false) : "???";
            }
            // 预求值 + GcRootHandle 保护 kind/message（Error 浅拷贝裸指针）
            writeLine(cpp, "{");
            writeLine(cpp, "    auto _k = (" + kind + ");");
            writeLine(cpp, "    aura_rt::GcRootHandle<decltype(_k)> _hk(_k);");
            writeLine(cpp, "    auto _m = (" + message + ");");
            writeLine(cpp, "    aura_rt::GcRootHandle<decltype(_m)> _hm(_m);");
            writeLine(cpp, "    throw aura_rt::Error(_hk.get(), _hm.get());");
            writeLine(cpp, "}");
        } else {
            std::string eVal = genExpr(*stmt.expr, false);
            writeLine(cpp, "{");
            writeLine(cpp, "    auto _e = (" + eVal + ");");
            writeLine(cpp, "    aura_rt::GcRootHandle<decltype(_e)> _he(_e);");
            writeLine(cpp, "    throw aura_rt::Error(_he.get());");
            writeLine(cpp, "}");
        }
    } else {
        writeLine(cpp, "throw;");
    }
}

void CodeGenerator::genIfStmt(std::ostream& cpp, const IfStmt& stmt,
                               bool isCoroutine) {
    cpp << indentStr() << "if (" << genExpr(*stmt.condition, isCoroutine) << ") {\n";
    if (stmt.thenBranch) genBlock(cpp, *stmt.thenBranch, isCoroutine);
    cpp << indentStr() << "}";
    for (auto& ei : stmt.elseIfs) {
        cpp << " else if (" << genExpr(*ei.condition, isCoroutine) << ") {\n";
        if (ei.body) genBlock(cpp, *ei.body, isCoroutine);
        cpp << indentStr() << "}";
    }
    if (stmt.elseBranch) {
        cpp << " else {\n";
        genBlock(cpp, *stmt.elseBranch, isCoroutine);
        cpp << indentStr() << "}";
    }
    cpp << '\n';
}

void CodeGenerator::genWhileStmt(std::ostream& cpp, const WhileStmt& stmt,
                                  bool isCoroutine) {
    cpp << indentStr() << "while (" << genExpr(*stmt.condition, isCoroutine) << ") {\n";
    if (stmt.body) genBlock(cpp, *stmt.body, isCoroutine);
    writeLine(cpp, "aura_rt::gc_safepoint();");  // L2 safepoint：长循环可被 GC 暂停
    cpp << indentStr() << "}\n";
}

// 作用域屏蔽 guard：临时移出 GC 根集合/类型集合，析构时恢复
// （修复：gcRootVarNames_ 无作用域清理，与其他作用域同名 GcRootHandle 变量
//   状态残留会导致同名标识符被误判生成 .get()；用于 for 迭代变量、spawn 闭包参数等）
struct IterVarGuard {
    std::set<std::string>& roots;
    std::unordered_map<std::string, std::string>& types;
    std::string name;
    bool wasRoot;
    bool hadType;
    std::string savedType;
    IterVarGuard(std::set<std::string>& r,
                 std::unordered_map<std::string, std::string>& t,
                 const std::string& n)
        : roots(r), types(t), name(n),
          wasRoot(r.erase(n) > 0), hadType(false) {
        auto it = t.find(n);
        if (it != t.end()) { savedType = it->second; t.erase(it); hadType = true; }
    }
    ~IterVarGuard() {
        if (wasRoot) roots.insert(name);
        if (hadType) types[name] = savedType;
    }
};

void CodeGenerator::genForStmt(std::ostream& cpp, const ForStmt& stmt,
                                bool isCoroutine) {
    IterVarGuard iterGuard(gcRootVarNames_, gcRootTypes_, safeName(stmt.itemName));

    // 检测 range() 调用 — 展开为 std::views::iota 或 step 循环
    if (auto* call = dynamic_cast<const CallExpr*>(stmt.iterable.get())) {
        auto* id = dynamic_cast<const Identifier*>(call->callee.get());
        if (id && id->name == "range") {
            std::string var = safeName(stmt.itemName);
            if (call->args.size() == 1) {
                std::string end = genExpr(*call->args[0], isCoroutine);
                cpp << indentStr() << "for (auto " << var
                    << " : std::views::iota(0, " << end << ")) {\n";
            } else if (call->args.size() == 2) {
                std::string start = genExpr(*call->args[0], isCoroutine);
                std::string end   = genExpr(*call->args[1], isCoroutine);
                cpp << indentStr() << "for (auto " << var
                    << " : std::views::iota(" << start << ", " << end << ")) {\n";
            } else if (call->args.size() == 3) {
                std::string start = genExpr(*call->args[0], isCoroutine);
                std::string end   = genExpr(*call->args[1], isCoroutine);
                std::string step  = genExpr(*call->args[2], isCoroutine);
                cpp << indentStr() << "for (auto " << var
                    << " = " << start
                    << "; " << var << " < " << end
                    << "; " << var << " += " << step << ") {\n";
            }
            if (stmt.body) genBlock(cpp, *stmt.body, isCoroutine);
            writeLine(cpp, "aura_rt::gc_safepoint();");  // L2 safepoint
            cpp << indentStr() << "}\n";
            return;
        }
    }

    // 检测 Iterator 遍历：for v in it → while + next()/is_none()/unwrap()
    // 覆盖：内置迭代器表达式（range/map/filter/from 返回值）、Iterator 接口变量/参数、
    //       record 显式 impl Iterator<T>（其 for-in 语义，record 直接可迭代）
    bool iterIsIterator = false;
    if (stmt.iterable->inferredType) {
        auto* ty = stmt.iterable->inferredType;
        if (auto* g = dynamic_cast<const GenericSemType*>(ty))
            iterIsIterator = g->name == "Iterator"
                          || g->resolvedName.find("Iterator") != std::string::npos;
        else if (auto* is = dynamic_cast<const InterfaceSemType*>(ty))
            iterIsIterator = is->name == "Iterator";
        else if (auto* r = dynamic_cast<const RecordSemType*>(ty)) {
            // record 显式 impl Iterator<T> → 用接口元素类型生成 next() 循环
            auto recIt = interfaceImplementations_.find(r->canonicalName);
            if (recIt != interfaceImplementations_.end()
                && recIt->second.count("Iterator") > 0)
                iterIsIterator = true;
        }
    }
    if (iterIsIterator) {
        std::string var = safeName(stmt.itemName);
        std::string itExpr = genExpr(*stmt.iterable, isCoroutine);
        cpp << indentStr() << "{\n";
        indentLevel_++;
        // record impl：GC 化适配器（desc 扫描 owner，GC compact 安全）+ view → 值视图
        //（IIFE 先 root record 指针：gcConstruct 内 alloc 可能触发 GC）
        if (auto* r = dynamic_cast<const RecordSemType*>(stmt.iterable->inferredType)) {
            std::string recName = r->canonicalName;
            std::string adName = safeName(recName) + "Iterator";
            writeLine(cpp, "auto _it_raw = [&]() -> auto {");
            writeLine(cpp, "    " + recName + "* _ar = (" + itExpr + ");");
            writeLine(cpp, "    aura_rt::GcRootHandle<" + recName + "*> _ah(_ar);");
            writeLine(cpp, "    auto* _ad = aura_rt::gcConstruct<" + adName
                      + ">(&" + adName + "::desc(), _ah.get());");
            writeLine(cpp, "    return " + adName + "::view(_ad);");
            writeLine(cpp, "  }();");
        } else {
            // 内置迭代器：表达式即值视图（make_range/make_map/make_filter/视图变量）
            writeLine(cpp, "auto _it_raw = " + itExpr + ";");
        }
        // P1：迭代器视图含 self 裸指针，循环体内 alloc/gc_safepoint 可能触发 GC compact，
        //     compact 不重写栈上裸指针 → ViewRoot 注册 self 为 GcRootHandle，
        //     循环内每次 _it.get() 重建视图取最新 self（与 iter_gc_test 手动 ViewRoot 同机制）
        writeLine(cpp, "aura_rt::ViewRoot<decltype(_it_raw)> _it(_it_raw);");
        cpp << indentStr() << "while (true) {\n";
        indentLevel_++;
        // 视图统一用 .next()（值视图 {nextFn, self}；self 经 ViewRoot 保护）
        writeLine(cpp, "auto _opt = _it.get().next();");
        writeLine(cpp, "if (_opt->is_none()) break;");
        writeLine(cpp, "auto " + var + " = _opt->unwrap();");
        if (stmt.body) genBlock(cpp, *stmt.body, isCoroutine);
        writeLine(cpp, "aura_rt::gc_safepoint();");  // L2 safepoint
        indentLevel_--;
        cpp << indentStr() << "}\n";
        indentLevel_--;
        cpp << indentStr() << "}\n";
        return;
    }

    // 检测 channel 遍历：for val in ch → while + receive 循环
    if (auto* id = dynamic_cast<const Identifier*>(stmt.iterable.get())) {
        if (channelVarNames_.count(id->name)) {
            std::string var = safeName(stmt.itemName);
            // 走 genIdentifier 路径：若 channel 变量被注册为 GcRootHandle（如 sync thread
            // 块内 spawn 参数），自动生成 .get() 解引用；否则原样使用
            std::string chName = genIdentifier(*id);
            // sync thread 内：阻塞 while + receive（不调用 is_done()，避免冗余锁）
            // sync.ThreadChannel.receive() 返回 Optional<T>，关闭且空时返回 None
            if (inSyncThreadBlock_) {
                cpp << indentStr() << "while (true) {\n";
                indentLevel_++;
                writeLine(cpp, "auto _opt = " + chName + "->receive();");
                writeLine(cpp, "if (_opt->is_none()) break;");
                writeLine(cpp, "auto " + var + " = _opt->unwrap();");
                if (stmt.body) genBlock(cpp, *stmt.body, isCoroutine);
                writeLine(cpp, "aura_rt::gc_safepoint();");  // L2 safepoint
                indentLevel_--;
                cpp << indentStr() << "}\n";
                return;
            }
            // 协程路径：区分 coroutine channel（co_await receive）与 sync.ThreadChannel（阻塞 receive）
            // sync.ThreadChannel 的 receive() 返回 Optional<T>*（同步阻塞），非协程 awaitable
            bool isSyncChannel = false;
            auto git = gcRootTypes_.find(id->name);
            if (git != gcRootTypes_.end()
                && git->second.find("ThreadChannel") != std::string::npos)
                isSyncChannel = true;
            if (isSyncChannel) {
                cpp << indentStr() << "while (true) {\n";
                indentLevel_++;
                writeLine(cpp, "auto _opt = " + chName + "->receive();");
                writeLine(cpp, "if (_opt->is_none()) break;");
                writeLine(cpp, "auto " + var + " = _opt->unwrap();");
                if (stmt.body) genBlock(cpp, *stmt.body, isCoroutine);
                writeLine(cpp, "aura_rt::gc_safepoint();");  // L2 safepoint
                indentLevel_--;
                cpp << indentStr() << "}\n";
                return;
            }
            // 协程 channel<T>（原有）：co_await receive
            cpp << indentStr() << "while (true) {\n";
            indentLevel_++;
            writeLine(cpp, "if (" + chName + "->is_done()) break;");
            writeLine(cpp, "auto " + var + " = co_await " + chName + "->receive();");
            if (stmt.body) genBlock(cpp, *stmt.body, isCoroutine);
            writeLine(cpp, "aura_rt::gc_safepoint();");  // L2 safepoint
            indentLevel_--;
            cpp << indentStr() << "}\n";
            return;
        }
    }

    // 默认：数组/列表遍历
    std::string iter = genExpr(*stmt.iterable, isCoroutine);
    cpp << indentStr() << "for (auto " << safeName(stmt.itemName)
        << " : *" << iter << ") {\n";
    if (stmt.body) genBlock(cpp, *stmt.body, isCoroutine);
    writeLine(cpp, "aura_rt::gc_safepoint();");  // L2 safepoint
    cpp << indentStr() << "}\n";
}

void CodeGenerator::genLoopStmt(std::ostream& cpp, const LoopStmt& stmt,
                                 bool isCoroutine) {
    cpp << indentStr() << "while (true) {\n";
    if (stmt.body) genBlock(cpp, *stmt.body, isCoroutine);
    writeLine(cpp, "aura_rt::gc_safepoint();");  // L2 safepoint
    cpp << indentStr() << "}\n";
}

void CodeGenerator::genBreakStmt(std::ostream& cpp) {
    writeLine(cpp, "break;");
}

void CodeGenerator::genContinueStmt(std::ostream& cpp) {
    writeLine(cpp, "continue;");
}

// ============================================================
// try / catch
// ============================================================

void CodeGenerator::genTryCatchStmt(std::ostream& cpp,
                                     const TryCatchStmt& stmt,
                                     bool isCoroutine) {
    if (!isCoroutine || !stmt.tryBody || stmt.tryBody->stmts.empty()) {
        genTryCatchRaw(cpp, stmt, isCoroutine);
        return;
    }

    // === 协程安全模式 ===
    // C++20 协程 + GCC 上 try/catch 有 bug（非 std::exception 异常类型匹配失败）
    // 改用：把 try 体中的"setup"语句包装为普通函数 IIFE，用 variant 传回错误
    //
    // 策略：分析 try 体，找到第一个 LetDecl（含 initializer）作为"抛出版本"，
    // 将其初始值表达式提取到非协程 IIFE 中，其余语句作为 continuation 分支。

    auto& stmts = stmt.tryBody->stmts;

    // 1. 找到 try 体中的第一个 LetDecl（含 initializer）
    const LetDecl* setupLet = nullptr;
    size_t letIdx = 0;
    for (size_t i = 0; i < stmts.size(); ++i) {
        if (auto* let = dynamic_cast<const LetDecl*>(stmts[i].get())) {
            if (let->initializer) { setupLet = let; letIdx = i; break; }
        }
    }

    if (!setupLet) {
        // v1.2 修复：协程模式下无 setupLet 时也用 IIFE + variant 模式
        // 原因：genTryCatchRaw 会在 catch handler 中生成 co_await，违反 C++ 标准
        // （catch handler 内禁止 co_await）
        // 策略：IIFE 执行 try 体所有语句（同步版本），返回 variant<monostate, Error>
        //       成功分支执行后续语句（无 setupLet 时通常无后续）
        //       错误分支执行 catchBody（在协程正常流程中，可含 co_await）
        if (!isCoroutine) {
            genTryCatchRaw(cpp, stmt, isCoroutine);
            return;
        }
        genTryCatchNoSetupIIFE(cpp, stmt, isCoroutine);
        return;
    }

    // 2. 推断结果类型
    // 优先用 SemType 推导（避免 decltype(initExpr) 中嵌套 lambda 在未求值上下文无法捕获变量）
    std::string initExpr = genExpr(*setupLet->initializer, false);
    std::string resultType;
    if (setupLet->type) {
        resultType = mapType(*setupLet->type);
    } else if (setupLet->initializer && setupLet->initializer->inferredType) {
        resultType = mapSemType(*setupLet->initializer->inferredType);
    }
    if (resultType.empty() || resultType == "auto") {
        // 退化：无法从 SemType 推导，用 decltype（仅在 initExpr 不含 lambda 时安全）
        resultType = "decltype(" + initExpr + ")";
    }
    std::string varName = safeName(setupLet->name);
    std::string cv = safeName(stmt.catchVar);

    cpp << indentStr() << "{\n";
    indentLevel_++;

    // 3. 安全 IIFE：在普通函数中 try/catch，返回 variant<Result, Error>
    writeLine(cpp, "auto _try = [&]() -> std::variant<" + resultType + ", aura_rt::Error> {");
    indentLevel_++;
    writeLine(cpp, "try {");
    indentLevel_++;
    writeLine(cpp, "return " + initExpr + ";");
    indentLevel_--;
    writeLine(cpp, "} catch (const aura_rt::Error& _e) {");
    indentLevel_++;
    writeLine(cpp, "return _e;");
    indentLevel_--;
    writeLine(cpp, "}");
    indentLevel_--;
    writeLine(cpp, "}();");

    // 4. 错误分支
    cpp << indentStr() << "if (std::holds_alternative<aura_rt::Error>(_try)) {\n";
    indentLevel_++;
    writeLine(cpp, "auto& " + cv + " = std::get<aura_rt::Error>(_try);");
    // GC 安全：variant 中的 Error 是值嵌入的，GC 不知道其内部结构，
    // 不会自动更新 kind/message/extra 指针。用 GcRootHandle 保护，
    // catchBody 中若有 co_await 触发 GC compact，指针会被自动更新。
    writeLine(cpp, "aura_rt::GcRootHandle<decltype(" + cv + ".kind)> _eh_kind(" + cv + ".kind);");
    writeLine(cpp, "aura_rt::GcRootHandle<decltype(" + cv + ".message)> _eh_msg(" + cv + ".message);");
    writeLine(cpp, "aura_rt::GcRootHandle<decltype(" + cv + ".extra)> _eh_extra(" + cv + ".extra);");
    valueTypeVarNames_.insert(cv);
    if (stmt.catchBody) genBlock(cpp, *stmt.catchBody, isCoroutine);
    valueTypeVarNames_.erase(cv);
    indentLevel_--;
    cpp << indentStr() << "} else {\n";
    indentLevel_++;

    // 5. 成功分支
    writeLine(cpp, "auto " + varName + " = std::get<" + resultType + ">(_try);");
    // GC 安全：resultType 为 GC 指针时用 GcRootHandle 保护（对齐错误分支 kind/message/extra 保护），
    // 后续 stmts 中若触发 GC（co_await compact），varName 指向的堆对象不会被回收/悬垂。
    if (isGcPointerType(resultType)) {
        writeLine(cpp, "aura_rt::GcRootHandle<" + resultType + "> _" + varName + "_root(" + varName + ");");
    }
    for (size_t i = letIdx + 1; i < stmts.size(); ++i) {
        if (stmts[i]) genStmt(cpp, *stmts[i], isCoroutine);
    }

    indentLevel_--;
    cpp << indentStr() << "}\n";
    indentLevel_--;
    cpp << indentStr() << "}\n";
}

// v1.2 修复：协程模式下无 setupLet 的 try/catch 用 IIFE + variant<monostate, Error>
// 避免 catch handler 内生成 co_await（C++ 标准禁止）
// IIFE 内执行 try 体所有语句（同步版本，isCoroutine=false），
// 成功返回 monostate，失败返回 Error；后续在协程正常流程中处理错误分支
void CodeGenerator::genTryCatchNoSetupIIFE(std::ostream& cpp,
                                            const TryCatchStmt& stmt,
                                            bool isCoroutine) {
    std::string cv = safeName(stmt.catchVar);

    cpp << indentStr() << "{\n";
    indentLevel_++;

    // IIFE：普通函数，执行 try 体所有语句（同步版本），返回 variant<monostate, Error>
    writeLine(cpp, "auto _try = [&]() -> std::variant<std::monostate, aura_rt::Error> {");
    indentLevel_++;
    writeLine(cpp, "try {");
    indentLevel_++;
    // try 体语句：同步版本（isCoroutine=false，避免生成 co_await）
    if (stmt.tryBody) {
        for (auto& s : stmt.tryBody->stmts) {
            if (s) genStmt(cpp, *s, false);
        }
    }
    writeLine(cpp, "return std::monostate{};");
    indentLevel_--;
    writeLine(cpp, "} catch (const aura_rt::Error& _e) {");
    indentLevel_++;
    writeLine(cpp, "return _e;");
    indentLevel_--;
    writeLine(cpp, "}");
    indentLevel_--;
    writeLine(cpp, "}();");

    // 错误分支：在协程正常流程中执行 catchBody（可含 co_await）
    cpp << indentStr() << "if (std::holds_alternative<aura_rt::Error>(_try)) {\n";
    indentLevel_++;
    writeLine(cpp, "auto& " + cv + " = std::get<aura_rt::Error>(_try);");
    // GC 安全：variant 中的 Error 是值嵌入的，需 GcRootHandle 保护内部指针
    writeLine(cpp, "aura_rt::GcRootHandle<decltype(" + cv + ".kind)> _eh_kind(" + cv + ".kind);");
    writeLine(cpp, "aura_rt::GcRootHandle<decltype(" + cv + ".message)> _eh_msg(" + cv + ".message);");
    writeLine(cpp, "aura_rt::GcRootHandle<decltype(" + cv + ".extra)> _eh_extra(" + cv + ".extra);");
    valueTypeVarNames_.insert(cv);
    if (stmt.catchBody) genBlock(cpp, *stmt.catchBody, isCoroutine);
    valueTypeVarNames_.erase(cv);
    indentLevel_--;
    cpp << indentStr() << "}\n";

    indentLevel_--;
    cpp << indentStr() << "}\n";
}

void CodeGenerator::genTryCatchRaw(std::ostream& cpp,
                                    const TryCatchStmt& stmt,
                                    bool isCoroutine) {
    cpp << indentStr() << "try {\n";
    if (stmt.tryBody) genBlock(cpp, *stmt.tryBody, isCoroutine);
    std::string cv = safeName(stmt.catchVar);
    cpp << indentStr() << "} catch (aura_rt::Error& " << cv << ") {\n";
    indentLevel_++;
    // GC 安全：Error 在 C++ 异常存储区中（非 GC 堆），GC compact 不会自动更新
    // 其内部的 GcString* 指针（kind/message/extra）。用 GcRootHandle 持有这些指针的地址，
    // GC compact 时会通过 roots_ 更新它们，防止 catchBody 中访问悬垂指针。
    writeLine(cpp, "aura_rt::GcRootHandle<decltype(" + cv + ".kind)> _eh_kind(" + cv + ".kind);");
    writeLine(cpp, "aura_rt::GcRootHandle<decltype(" + cv + ".message)> _eh_msg(" + cv + ".message);");
    writeLine(cpp, "aura_rt::GcRootHandle<decltype(" + cv + ".extra)> _eh_extra(" + cv + ".extra);");
    valueTypeVarNames_.insert(cv);
    if (stmt.catchBody) genBlock(cpp, *stmt.catchBody, isCoroutine);
    valueTypeVarNames_.erase(cv);
    indentLevel_--;
    cpp << indentStr() << "}\n";
}

// (IdRefCollector / DeclaredCollector 定义已移至 CodeGen.h)

// ============================================================
// sync / spawn（plan §4.9）
// ============================================================

void CodeGenerator::genSyncStmt(std::ostream& cpp, const SyncStmt& stmt,
                                 bool /*isCoroutine*/) {
    // sync thread 分支：多线程模式
    if (stmt.isThread) {
        genSyncThreadStmt(cpp, stmt);
        return;
    }

    if (stmt.maxExpr) {
        // 有界版本：sync(max = N) { ... }
        std::string maxN = genExpr(*stmt.maxExpr, false);
        cpp << indentStr() << "{\n";
        indentLevel_++;
        writeLine(cpp, "aura_rt::bounded_sync _sync(" + maxN + ");");
        writeLine(cpp, "auto& _tasks = _sync.tasks();");
        if (stmt.body) genBlock(cpp, *stmt.body, true);
        writeLine(cpp, "aura_rt::gc_safepoint();");
        writeLine(cpp, "co_await _sync.wait_all();");
        indentLevel_--;
        cpp << indentStr() << "}\n";
    } else {
        // 无界版本（兼容旧语法）
        cpp << indentStr() << "{\n";
        writeLine(cpp, "std::vector<aura_rt::task<void>> _tasks;");
        if (stmt.body) genBlock(cpp, *stmt.body, true);
        writeLine(cpp, "aura_rt::gc_safepoint();");
        writeLine(cpp, "co_await aura_rt::when_all(std::move(_tasks));");
        cpp << indentStr() << "}\n";
    }
}

// ============================================================
// sync thread 块：多线程实现
//
// 生成代码结构：
//   {
//       aura_rt::sync_thread_context _stx(maxN);  // RAII: 构造 beginGroup，析构 waitGroup
//       aura_rt::ThreadPool::instance().ensureStarted();
//       // spawn 语句 → _stx.submit([](params) { body });
//       // 析构时 waitGroup 阻塞至所有任务完成
//   }
// ============================================================
void CodeGenerator::genSyncThreadStmt(std::ostream& cpp, const SyncStmt& stmt) {
    cpp << indentStr() << "{\n";
    indentLevel_++;

    // 无界保护：maxExpr=0 表示无界（默认上限 = hardware_concurrency）
    std::string maxArg = stmt.maxExpr ? genExpr(*stmt.maxExpr, false) : "0";
    writeLine(cpp, "aura_rt::sync_thread_context _stx(" + maxArg + ");");
    // 懒启动线程池（首次调用时初始化）
    writeLine(cpp, "aura_rt::ThreadPool::instance().ensureStarted();");

    // 生成块体：spawn 会被分派到 genSpawnAsThread
    // 注意：sync thread 块体以非协程模式生成（isCoroutine=false），
    // 因为内部不能有 co_await，且 spawn body 是普通 lambda
    bool oldInSyncThread = inSyncThreadBlock_;
    inSyncThreadBlock_ = true;
    if (stmt.body) genBlock(cpp, *stmt.body, false);
    inSyncThreadBlock_ = oldInSyncThread;

    // 块结束前触发 safepoint（可能执行延迟的 GC）
    writeLine(cpp, "aura_rt::gc_safepoint();");
    // sync_thread_context 析构会调用 waitGroup，阻塞至所有任务完成
    indentLevel_--;
    cpp << indentStr() << "}\n";
}

void CodeGenerator::genSyncForStmt(std::ostream& cpp, const SyncForStmt& stmt, bool) {
    std::string var = safeName(stmt.itemName);
    bool hasMax = stmt.maxExpr != nullptr;

    // === 线程版：sync thread for ===
    if (stmt.isThread) {
        cpp << indentStr() << "{\n";
        indentLevel_++;
        std::string maxArg = hasMax ? genExpr(*stmt.maxExpr, false) : "0";
        writeLine(cpp, "aura_rt::sync_thread_context _stx(" + maxArg + ");");
        writeLine(cpp, "aura_rt::ThreadPool::instance().ensureStarted();");

        // for 循环头（复用协程版的 range/数组遍历生成逻辑）
        bool isRangeCall = false;
        if (auto* call = dynamic_cast<const CallExpr*>(stmt.iterable.get())) {
            auto* id = dynamic_cast<const Identifier*>(call->callee.get());
            if (id && id->name == "range") {
                isRangeCall = true;
                if (call->args.size() == 1) {
                    std::string end = genExpr(*call->args[0], false);
                    cpp << indentStr() << "for (auto " << var
                        << " : std::views::iota(0, " << end << ")) {\n";
                } else if (call->args.size() == 2) {
                    std::string start = genExpr(*call->args[0], false);
                    std::string end   = genExpr(*call->args[1], false);
                    cpp << indentStr() << "for (auto " << var
                        << " : std::views::iota(" << start << ", " << end << ")) {\n";
                }
            }
        }
        if (!isRangeCall) {
            std::string iter = genExpr(*stmt.iterable, false);
            cpp << indentStr() << "for (auto " << var
                << " : *" << iter << ") {\n";
        }
        indentLevel_++;

        // body 自由变量收集（修复：引用外部变量必须显式捕获）
        std::set<std::string> allRefs;
        IdRefCollector idCol(allRefs);
        if (stmt.body) idCol.collectStmt(*stmt.body);
        std::set<std::string> declared;
        DeclaredCollector declCol(declared);
        if (stmt.body) declCol.collectStmt(*stmt.body);
        std::set<std::string> builtins = {"io", "_tasks"};
        std::vector<std::string> freeVars;
        bool ioUsed = false;
        for (auto& name : allRefs) {
            if (name == stmt.itemName) continue;    // 迭代变量已值捕获
            if (declared.count(name)) continue;      // body 内局部声明
            if (name == "io") { ioUsed = true; continue; }
            if (builtins.count(name)) continue;
            if (registeredTypes_.count(name)) continue;  // 函数名/类型名
            freeVars.push_back(name);
        }

        // spawn body：普通 lambda + _stx.submit（var + freeVars 值捕获 + io 引用捕获）
        // 注：外部变量在主线程作用域仍存活（如 let ch27 的 GcRootHandle），
        //     worker 线程执行期间对象不会被回收，与闭包形态线程版语义一致
        bool oldIoSync = ioSync_;
        bool oldCoroutine = currentFunctionIsCoroutine_;
        ioSync_ = true;                       // 强制 io 方法 _sync 版本
        currentFunctionIsCoroutine_ = false;  // 普通 lambda，禁止 co_await
        cpp << indentStr() << "_stx.submit([" << var;
        for (auto& v : freeVars) cpp << ", " << safeName(v);
        if (ioUsed) cpp << ", &io";
        cpp << "]() mutable {\n";
        indentLevel_++;
        insideSpawn_ = true;
        if (stmt.body) genBlock(cpp, *stmt.body, false);   // 非协程！
        insideSpawn_ = false;
        indentLevel_--;
        writeLine(cpp, "});");
        ioSync_ = oldIoSync;
        currentFunctionIsCoroutine_ = oldCoroutine;

        // 回边 safepoint
        writeLine(cpp, "aura_rt::gc_safepoint();");
        indentLevel_--;
        cpp << indentStr() << "}\n";   // close for
        // _stx 析构自动 waitGroup
        indentLevel_--;
        cpp << indentStr() << "}\n";   // close block
        return;
    }

    // === 协程版（现有逻辑 + 自由变量捕获修复） ===
    // 1. Open sync block
    if (hasMax) {
        std::string maxN = genExpr(*stmt.maxExpr, false);
        cpp << indentStr() << "{\n";
        indentLevel_++;
        writeLine(cpp, "aura_rt::bounded_sync _sync(" + maxN + ");");
        writeLine(cpp, "auto& _tasks = _sync.tasks();");
    } else {
        cpp << indentStr() << "{\n";
        indentLevel_++;
        writeLine(cpp, "std::vector<aura_rt::task<void>> _tasks;");
    }

    // 2. Generate for loop over iterable
    bool isRangeCall = false;
    if (auto* call = dynamic_cast<const CallExpr*>(stmt.iterable.get())) {
        auto* id = dynamic_cast<const Identifier*>(call->callee.get());
        if (id && id->name == "range") {
            isRangeCall = true;
            if (call->args.size() == 1) {
                std::string end = genExpr(*call->args[0], true);
                cpp << indentStr() << "for (auto " << var
                    << " : std::views::iota(0, " << end << ")) {\n";
            } else if (call->args.size() == 2) {
                std::string start = genExpr(*call->args[0], true);
                std::string end   = genExpr(*call->args[1], true);
                cpp << indentStr() << "for (auto " << var
                    << " : std::views::iota(" << start << ", " << end << ")) {\n";
            }
        }
    }
    if (!isRangeCall) {
        std::string iter = genExpr(*stmt.iterable, true);
        cpp << indentStr() << "for (auto " << var
            << " : *" << iter << ") {\n";
    }
    indentLevel_++;

    // 3. body 自由变量收集（修复：现有版本 body 引用外部变量编译失败）
    std::set<std::string> allRefs;
    IdRefCollector idCol(allRefs);
    if (stmt.body) idCol.collectStmt(*stmt.body);
    std::set<std::string> declared;
    DeclaredCollector declCol(declared);
    if (stmt.body) declCol.collectStmt(*stmt.body);
    std::set<std::string> builtins = {"io", "_tasks"};
    std::vector<std::string> freeVars;
    for (auto& name : allRefs) {
        if (name == stmt.itemName) continue;   // 迭代变量已有参数
        if (declared.count(name)) continue;     // body 内局部声明
        if (builtins.count(name)) continue;
        if (registeredTypes_.count(name)) continue;  // 函数名/类型名
        freeVars.push_back(name);
    }

    // 4. Generate spawn lambda：[] 空捕获 + 显式参数（var + freeVars + io + _tasks）
    //    安全模式与旧式 spawn 一致：协程帧在创建时拷贝参数，无 this 野指针 UB
    cpp << indentStr() << "_tasks.push_back([](auto " << var;
    for (auto& v : freeVars) cpp << ", auto " << safeName(v);
    cpp << ", aura_rt::Io& io, std::vector<aura_rt::task<void>>& _tasks"
        << ") -> aura_rt::task<void> {\n";
    indentLevel_++;
    insideSpawn_ = true;
    if (stmt.body) genBlock(cpp, *stmt.body, true);
    insideSpawn_ = false;
    writeLine(cpp, "co_return;");
    indentLevel_--;
    cpp << indentStr() << "}(" << var;
    for (auto& v : freeVars) cpp << ", " << safeName(v);
    cpp << ", io, _tasks));\n";

    // 5. L2 safepoint：sync for 循环回边
    writeLine(cpp, "aura_rt::gc_safepoint();");
    indentLevel_--;
    cpp << indentStr() << "}\n";   // close for

    // 6. Close sync block
    writeLine(cpp, "aura_rt::gc_safepoint();");
    if (hasMax) {
        writeLine(cpp, "co_await _sync.wait_all();");
    } else {
        writeLine(cpp, "co_await aura_rt::when_all(std::move(_tasks));");
    }
    indentLevel_--;
    cpp << indentStr() << "}\n";   // close sync block
}

void CodeGenerator::genSpawnStmt(std::ostream& cpp, const SpawnStmt& stmt,
                                  bool /*isCoroutine*/) {
    // === 调用形态：spawn func(args) / spawn obj.method(args) ===
    if (stmt.callExpr) {
        if (inSyncThreadBlock_)
            genSpawnCallAsThread(cpp, stmt);
        else
            genSpawnCallAsCoro(cpp, stmt);
        return;
    }

    // sync thread 块内的 spawn：分派到线程版本
    if (inSyncThreadBlock_) {
        genSpawnAsThread(cpp, stmt);
        return;
    }

    // === 显式传参模式（spawn (io: Io, n: int) { ... }） ===
    // 检查用户是否已声明 io / _tasks
    bool hasIo = false;
    bool hasTasks = false;
    for (auto& p : stmt.params) {
        if (p.name == "io") hasIo = true;
        if (p.name == "_tasks") hasTasks = true;
    }

    // 生成 lambda 签名为显式参数
    cpp << indentStr() << "_tasks.push_back([](";
    for (size_t i = 0; i < stmt.params.size(); ++i) {
        if (i > 0) cpp << ", ";
        cpp << (stmt.params[i].type ? mapParamType(*stmt.params[i].type) : "auto")
            << " " << safeName(stmt.params[i].name);
    }
    // 自动追加 io 和 _tasks（如果用户未声明）
    if (!hasIo) cpp << ", aura_rt::Io& io";
    if (!hasTasks) cpp << ", std::vector<aura_rt::task<void>>& _tasks";
    cpp << ") -> aura_rt::task<void> {\n";
    insideSpawn_ = true;

    // 屏蔽参数名：闭包参数可能与外层同名 GcRootHandle 变量冲突（gcRootVarNames_ 无
    // 作用域清理），否则参数被误判生成 .get()；块结束（实参生成前）guard 析构恢复外层状态
    {
        // reserve：IterVarGuard 含引用成员 + 用户声明析构函数（C++11 起抑制隐式移动构造），
        // vector 扩容迁移旧元素时用拷贝构造迁移后立即析构旧对象，析构执行 roots.insert(name)
        // 会把该参数名提前恢复回 gcRootVarNames_ → body 生成期间误生 .get()。
        // reserve 后不扩容即无拷贝迁移，各参数名在整个 body 生成期间保持屏蔽。
        std::vector<IterVarGuard> guards;
        guards.reserve(stmt.params.size());
        for (auto& p : stmt.params)
            guards.emplace_back(gcRootVarNames_, gcRootTypes_, safeName(p.name));

        for (auto& s : stmt.body)
            if (s) genStmt(cpp, *s, true);
    }

    insideSpawn_ = false;
    cpp << indentStr() << "    co_return;\n";
    cpp << indentStr() << "}(";

    // 实参：同名自动绑定 or 显式传入
    if (!stmt.args.empty()) {
        for (size_t i = 0; i < stmt.args.size(); ++i) {
            if (i > 0) cpp << ", ";
            cpp << genExpr(*stmt.args[i], true);
        }
    } else {
        for (size_t i = 0; i < stmt.params.size(); ++i) {
            if (i > 0) cpp << ", ";
            // 同名自动绑定：外层 GcRootHandle 变量 → 传 .get() 裸指针（guards 已析构，
            // gcRootVarNames_ 已恢复外层状态）
            std::string pname = safeName(stmt.params[i].name); // 同名自动绑定
            cpp << (gcRootVarNames_.count(pname) ? pname + ".get()" : pname);
        }
    }
    if (!hasIo) cpp << ", io";
    if (!hasTasks) cpp << ", _tasks";
    cpp << "));\n";
}

// 调用形态（协程 sync 块内）：spawn func(args)
// 生成：_tasks.push_back([](auto fv..., Io& io, taskvec& _tasks)
//           -> task<void> { 调用; co_return; }(fv..., io, _tasks));
void CodeGenerator::genSpawnCallAsCoro(std::ostream& cpp, const SpawnStmt& stmt) {
    // 1. 自由变量 = 调用表达式中所有 Identifier - 函数/类型名 - 内置
    std::set<std::string> allRefs;
    IdRefCollector idCol(allRefs);
    idCol.collectExpr(*stmt.callExpr);   // 含 callee + args
    std::set<std::string> builtins = {"io", "_tasks"};
    std::vector<std::string> freeVars;
    for (auto& name : allRefs) {
        if (builtins.count(name)) continue;
        if (registeredTypes_.count(name)) continue;  // 函数名/类型名不捕获
        freeVars.push_back(name);
    }

    // 2. 协程 lambda：[] 空捕获 + 显式参数（复用旧式 spawn 的安全模式）
    cpp << indentStr() << "_tasks.push_back([](";
    for (auto& v : freeVars)
        cpp << "auto " << safeName(v) << ", ";
    cpp << "aura_rt::Io& io, std::vector<aura_rt::task<void>>& _tasks"
        << ") -> aura_rt::task<void> {\n";
    indentLevel_++;
    insideSpawn_ = true;
    // isCoroutine=true：若 callee 为协程函数，genExpr 自动加 co_await；返回值丢弃
    writeLine(cpp, genExpr(*stmt.callExpr, true) + ";");
    insideSpawn_ = false;
    writeLine(cpp, "co_return;");
    indentLevel_--;
    cpp << indentStr() << "}(";
    for (auto& v : freeVars)
        cpp << safeName(v) << ", ";
    cpp << "io, _tasks));\n";
}

// 调用形态（sync thread 块内）：spawn func(args)
// 生成：_stx.submit([fv..., &io]() mutable { 调用; });
void CodeGenerator::genSpawnCallAsThread(std::ostream& cpp, const SpawnStmt& stmt) {
    bool oldIoSync = ioSync_;
    bool oldCoroutine = currentFunctionIsCoroutine_;
    ioSync_ = true;                      // 强制 io 方法 _sync 版本
    currentFunctionIsCoroutine_ = false; // 普通 lambda，禁止 co_await

    // 1. 自由变量 + io 使用检测
    std::set<std::string> allRefs;
    IdRefCollector idCol(allRefs);
    idCol.collectExpr(*stmt.callExpr);
    std::set<std::string> builtins = {"io", "_tasks"};
    std::vector<std::string> freeVars;
    bool ioUsed = false;
    for (auto& name : allRefs) {
        if (name == "io") { ioUsed = true; continue; }
        if (builtins.count(name)) continue;
        if (registeredTypes_.count(name)) continue;
        freeVars.push_back(name);
    }

    // 2. 捕获列表：freeVars 值捕获 + io 引用捕获
    cpp << indentStr() << "_stx.submit([";
    for (size_t i = 0; i < freeVars.size(); ++i) {
        if (i > 0) cpp << ", ";
        cpp << safeName(freeVars[i]);
    }
    if (ioUsed) {
        if (!freeVars.empty()) cpp << ", ";
        cpp << "&io";
    }
    cpp << "]() mutable {";
    indentLevel_++;
    insideSpawn_ = true;
    writeLine(cpp, genExpr(*stmt.callExpr, false) + ";");
    insideSpawn_ = false;
    indentLevel_--;
    cpp << "\n" << indentStr() << "});\n";

    ioSync_ = oldIoSync;
    currentFunctionIsCoroutine_ = oldCoroutine;
}

// ============================================================
// lock 语句：lock (lockExpr) { body }
//
// v1.0 仅 Mutex 分支：生成 RAII guard，生命周期限制在块作用域内。
// _guard 构造时 acquire（m->lock()），析构时 release（m->unlock()）。
// 块结束自动 unlock，无需用户手动操作，且禁止跨函数持有锁。
//
// 注意：lock 块内强制 isCoroutine=false（同步执行）。
//       v1.0 简化：lock 块内调用 io 异步方法需用户自行用 _sync 版本。
// ============================================================
void CodeGenerator::genLockStmt(std::ostream& cpp, const LockStmt& stmt,
                                  bool /*isCoroutine*/) {
    // v1.2: 多锁 lock (e1, e2, ...) { body }
    // - 单锁（lockExprs.size()==1）：走简化路径，与 v1.1 行为一致
    // - 多锁（lockExprs.size()>=2）：按声明顺序构造 variant<Guard>，存入 vector
    //   完整地址排序推到 v1.3（RWMutex.r()/w() 返回 Guard 临时对象，无法参与排序）
    //   当前实现等价于手写嵌套 lock(a) { lock(b) { } }，死锁预防由 L5 运行时检测兜底

    // 求值每个锁表达式，读取 Sema 标注的 inferredType
    struct LockInfo {
        std::string cppExpr;     // 求值后的 C++ 表达式
        std::string typeName;    // Mutex / RWMutexReadView / RWMutexWriteView / Once
    };
    std::vector<LockInfo> locks;
    locks.reserve(stmt.lockExprs.size());
    for (auto& e : stmt.lockExprs) {
        if (!e) continue;
        std::string cppExpr = genExpr(*e, false);
        std::string typeName;
        if (e->inferredType) {
            if (auto* gs = dynamic_cast<const GenericSemType*>(e->inferredType)) {
                typeName = gs->name;
            }
        }
        locks.push_back({cppExpr, typeName});
    }

    // Once 分支（仅单锁，Sema L8 已保证多锁时无 Once）
    if (locks.size() == 1 && locks[0].typeName == "Once") {
        writeLine(cpp, locks[0].cppExpr + "->do_([&] {");
        indentLevel_++;
        if (stmt.body) genBlock(cpp, *stmt.body, false);
        indentLevel_--;
        writeLine(cpp, "});");
        return;
    }

    // 单锁场景：简化路径，不排序
    if (locks.size() == 1) {
        const auto& lk = locks[0];
        cpp << indentStr() << "{\n";
        indentLevel_++;
        if (lk.typeName == "RWMutexReadView" || lk.typeName == "RWMutexWriteView") {
            // lock (rw.r()) { } → auto _guard = rw->r();
            writeLine(cpp, "auto _guard = " + lk.cppExpr + ";");
        } else {
            // Mutex 默认
            writeLine(cpp, "auto _guard = aura_rt::__acquire_lock(" + lk.cppExpr + ");");
        }
        if (stmt.body) genBlock(cpp, *stmt.body, false);
        indentLevel_--;
        cpp << indentStr() << "}\n";
        return;
    }

    // 多锁场景
    // - 全 Mutex：按地址排序后获取（统一锁序，消除锁序反转死锁）
    //   借鉴 std::scoped_lock 的死锁避免思想，但用 safepoint 感知的 Guard 逐个获取
    // - 混合（含 RWMutex.r()/.w()）：按声明顺序获取（RWMutex 返回 Guard 临时对象，
    //   无法参与地址排序；用户需自行保证锁序一致）
    bool allMutex = true;
    for (auto& lk : locks) {
        if (lk.typeName != "Mutex") {
            allMutex = false;
            break;
        }
    }

    if (allMutex) {
        // 全 Mutex：地址排序 + 逐个获取
        cpp << indentStr() << "{\n";
        indentLevel_++;
        // 1. 求值所有锁表达式到数组
        std::string arrInit = "{";
        for (size_t i = 0; i < locks.size(); ++i) {
            if (i > 0) arrInit += ", ";
            arrInit += locks[i].cppExpr;
        }
        arrInit += "}";
        writeLine(cpp, "aura_rt::Mutex* _ms[] = " + arrInit + ";");
        // 2. GcRootHandle 保护每个元素（GC compact 时自动更新指针）
        for (size_t i = 0; i < locks.size(); ++i) {
            writeLine(cpp, "aura_rt::GcRootHandle<aura_rt::Mutex*> _r" +
                         std::to_string(i) + "(_ms[" + std::to_string(i) + "]);");
        }
        // 3. 按地址排序（std::sort 交换数组元素值，GcRootHandle 仍指向数组地址，正确）
        writeLine(cpp, "std::sort(std::begin(_ms), std::end(_ms));");
        // 4. 逐个获取锁（用索引访问，确保读取 GcRootHandle 更新后的最新值）
        writeLine(cpp, "std::vector<aura_rt::Mutex::Guard> _guards;");
        writeLine(cpp, "_guards.reserve(" + std::to_string(locks.size()) + ");");
        writeLine(cpp, "for (size_t _i = 0; _i < sizeof(_ms)/sizeof(_ms[0]); ++_i) {");
        indentLevel_++;
        writeLine(cpp, "_guards.emplace_back(aura_rt::__acquire_lock(_ms[_i]));");
        indentLevel_--;
        writeLine(cpp, "}");
        if (stmt.body) genBlock(cpp, *stmt.body, false);
        // _guards 在块结束析构，按逆序释放锁
        indentLevel_--;
        cpp << indentStr() << "}\n";
        return;
    }

    // 混合场景：按声明顺序获取（无法地址排序，用户需保证锁序一致）
    cpp << indentStr() << "{\n";
    indentLevel_++;
    writeLine(cpp, "std::vector<aura_rt::LockGuardVariant> _guards;");
    writeLine(cpp, "_guards.reserve(" + std::to_string(locks.size()) + ");");
    for (size_t i = 0; i < locks.size(); ++i) {
        const auto& lk = locks[i];
        if (lk.typeName == "RWMutexReadView" || lk.typeName == "RWMutexWriteView") {
            writeLine(cpp, "_guards.emplace_back(" + lk.cppExpr + ");");
        } else {
            // Mutex
            writeLine(cpp, "_guards.emplace_back(aura_rt::__acquire_lock(" + lk.cppExpr + "));");
        }
    }
    if (stmt.body) genBlock(cpp, *stmt.body, false);
    // _guards 在块结束析构，按逆序释放锁
    indentLevel_--;
    cpp << indentStr() << "}\n";
}

// ============================================================
// sync thread 内的 spawn：生成 std::function 并提交到线程池
//
// 生成代码结构：
//   _stx.submit([capture_list]() mutable { body });
//
// 关键约束：
//   1. 使用值捕获 [capture_list] 而非参数传递，避免 lambda 返回值与 submit 签名冲突
//   2. 强制 ioSync_=true（sync thread 内不能用 co_await）
//   3. worker 入口/出口由 ThreadPool 管理，GC registerThread 已在 workerLoop 完成
//   4. mutable 标记：允许 lambda 内修改捕获的变量
// ============================================================
void CodeGenerator::genSpawnAsThread(std::ostream& cpp, const SpawnStmt& stmt) {
    // R3 由 Sema 保证：sync thread 内 spawn 必须显式传参
    // 此处 stmt.params 非空（调用形态已由 genSpawnStmt 分派到 genSpawnCallAsThread）

    bool oldIoSync = ioSync_;
    bool oldCoroutine = currentFunctionIsCoroutine_;
    ioSync_ = true;                     // 强制 io 方法用 _sync 版本（不能用 co_await）
    currentFunctionIsCoroutine_ = false; // sync thread lambda 不是协程，禁止 co_await

    // 生成捕获列表：显式参数按值捕获
    // io 特殊处理：引用捕获（Io 通常不可拷贝，且共享底层 iocp）
    cpp << indentStr() << "_stx.submit([";
    bool hasIo = false;
    std::vector<std::string> valueCaptures;
    for (size_t i = 0; i < stmt.params.size(); ++i) {
        if (stmt.params[i].name == "io") {
            hasIo = true;
            continue;  // io 单独处理
        }
        valueCaptures.push_back(safeName(stmt.params[i].name));
    }
    // 值捕获列表
    for (size_t i = 0; i < valueCaptures.size(); ++i) {
        if (i > 0) cpp << ", ";
        cpp << valueCaptures[i];
    }
    // io 引用捕获（最后添加）
    if (hasIo) {
        if (!valueCaptures.empty()) cpp << ", ";
        cpp << "&io";
    }
    cpp << "]() mutable {";

    // lambda body
    indentLevel_++;
    insideSpawn_ = true;
    // 显式参数已在 Sema 中注册为只读符号，此处直接生成体
    for (auto& s : stmt.body) {
        if (s) genStmt(cpp, *s, false);  // 非协程！
    }
    insideSpawn_ = false;
    indentLevel_--;
    cpp << "\n";

    cpp << indentStr() << "});\n";

    ioSync_ = oldIoSync;
    currentFunctionIsCoroutine_ = oldCoroutine;
}

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
