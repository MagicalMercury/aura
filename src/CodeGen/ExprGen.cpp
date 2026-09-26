#include "CodeGen.h"
#include "../Sema/BuiltinRegistry.h"
#include "../Sema/SemType.h"
#include <cctype>
#include <functional>
#include <sstream>

namespace Aura {

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
    // feature-06（阶段 B）：fun(A)->R 值 = GC 堆 CallableObj（捕获槽 desc 追踪，
    // 存储/传参按堆保护——机制性消灭闭包 GcRootHandle 手工包根缺陷族 #14/#24/
    // #32/#52/#55）。例外（旧 lambda 路径值非堆）：闭包自身泛型形态——未绑定泛型
    // 且非外层模板参数（currentTParams_ 提供）时值是模板 lambda/auto，非 GC 指针，
    // 生成 GcRootHandle<lambda> 会编译失败（change.md §5 风险首行的未绑定判定）。
    if (auto* f = dynamic_cast<const FuncSemType*>(type)) return !funcTypeHasOwnUnboundGeneric(f);
    // feature-06（阶段 C）：裸 Callable 值 = GC 堆 CallableErased（desc 追踪 target
    // 槽）——存储/传参按堆保护（GcRootHandle），与第 2 层 CallableObj 同机制。
    if (dynamic_cast<const CallableSemType*>(type)) return true;
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

// 未绑定泛型判定：GenericSemType 且 resolvedName 空（未实例化的模板参数，如泛型
// 方法/函数体内的 T）。此类类型实例化前无法静态判断是否为 GC 指针——T 可实例化
// 为 int/float 等值类型，也可实例化为 record/GcString*/Optional*/Variant* 等继承
// GcObject 的堆类型。若按 isHeapSemType 默认堆直接包装，会生成 GcRootHandle<int>
// 假根（GC mark 扫描读 int 当根指针 → 0xC0000005），故必须延迟到 C++ 编译期判定
// （方案 A：if constexpr，仿 runtime\gc\gc.h:716-725 gc_write_barrier_generic 与
// src\CodeGen\ExprAccess.cpp 字段赋值写屏障未绑定泛型分支的既有先例）。
bool CodeGenerator::isUnboundGenericSemType(const SemType* type) {
    if (auto* g = dynamic_cast<const GenericSemType*>(type))
        return g->resolvedName.empty() && g->name != "Iterator";
    return false;
}

bool CodeGenerator::isDeferredGcRoot(const SemType* t) const {
    return isHeapSemType(t) && !isIfaceView(t) && isUnboundGenericSemType(t);
}

// feature-06：FuncSemType 是否含"非外层模板提供的未绑定泛型"（闭包自身泛型形态，
// 如 fun([T], fun(T)->U)->[U] 中 T/U 为闭包自身泛型——C++ 值为模板 lambda / auto，
// 非 GC 堆对象）。递归容器（ListSemType/FuncSemType 参数返回/Optional）——
// compose/retry/mapper 形态的泛型函数别名实例（Mapper<T,U> 展开）参数/返回内嵌
// 自身泛型，须与直接参数/返回同等判定（否则误判为 CallableObj 堆值）。
bool CodeGenerator::funcTypeHasOwnUnboundGeneric(const FuncSemType* f) const {
    if (!f) return false;
    std::function<bool(const SemType*)> scan = [&](const SemType* t) -> bool {
        if (!t) return false;
        if (dynamic_cast<const ErrorSemType*>(t)) return false;
        if (auto* g = dynamic_cast<const GenericSemType*>(t)) {
            // 未实例化泛型（模板参数形态）。外层 C++ 模板参数（currentTParams_）
            // 由声明提供——实例化后恒为 CallableObj<具体>*（堆）；闭包自身泛型
            // （不在 currentTParams_）→ 旧 lambda 路径值（非堆）。
            if (g->resolvedName.empty() && g->name != "Iterator") {
                for (auto& tp : currentTParams_)
                    if (tp == g->name) return false;
                // feature-06：M3 调用点已物化的默认参数闭包泛型名（defaultArgMaterializedTypes_）
                // ——调用点补默认闭包实参时 T 已按实参推导为具体类型（mapType 直出
                // int32_t），闭包按具体签名走 CallableObj 新路径（IIFE），不算"自有未
                // 绑定泛型"（否则走旧路径裸 lambda → 无法向 CallableObj 形参推导，坏 C++）
                if (defaultArgMaterializedTypes_.count(g->name)) return false;
                return true;
            }
            return false;   // 已物化（resolvedName 非空）——内部泛型词属 C++ 文本
        }
        if (auto* l = dynamic_cast<const ListSemType*>(t))
            return scan(l->elementType.get());
        if (auto* o = dynamic_cast<const OptionalSemType*>(t))
            return scan(o->elementType.get());
        if (auto* f2 = dynamic_cast<const FuncSemType*>(t)) {
            if (scan(f2->returnType.get())) return true;
            for (auto& p : f2->paramTypes)
                if (scan(p.get())) return true;
            return false;
        }
        return false;
    };
    if (scan(f->returnType.get())) return true;
    for (auto& p : f->paramTypes)
        if (scan(p.get())) return true;
    return false;
}

bool CodeGenerator::listContainsUnboundGeneric(const ListSemType* ls) {
    if (!ls || !ls->elementType) return false;
    if (auto* g = dynamic_cast<const GenericSemType*>(ls->elementType.get()))
        return g->resolvedName.empty() && g->name != "Iterator";
    if (auto* inner = dynamic_cast<const ListSemType*>(ls->elementType.get()))
        return listContainsUnboundGeneric(inner);
    return false;
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

// ============================================================
// 表达式总调度
// ============================================================

std::string CodeGenerator::genExpr(const ASTNode& expr, bool isCoroutine) {
    // feature-14 P2（§3.5 隐式 future）——**消费点收口**。
    //
    // 为什么收口在 genExpr 入口而不是散落各生成分支：future 变量「被当值消费」
    // 的形态极多（实参传递、str()、字段访问基址、二元运算、索引、返回……），
    // 逐分支插 co_await 必漏（Phase 0 §5.1 实测 ExprCall 的 prefix 被 8 个分支
    // 复用；ExprMethodCall 另有一套）。在总调度处统一处理 = 「消费即兑现」。
    //
    // ⚠️ 唯一例外：作为赋值/管道/条件/错误传播的**左值或惰性承载者**时不得兑现——
    //    那是「转运 future」而不是「消费 future」，见下方 skipAsNonValue 判定。
    // ⚠️ 只在协程上下文兑现（isCoroutine）：非协程上下文根本没有 co_await 可用，
    //    且 future 变量只在协程函数体内生成（sync 块体 isCoroutine=true）。
    // ⚠️ 还原：本函数返回后 insideSyncBlock_ 不变（消费点不改变域归属）。
    if (isCoroutine) {
        if (auto* id = dynamic_cast<const Identifier*>(&expr)) {
            std::string src = resolveFutureVar(safeName(id->name));
            if (!src.empty()) {
                // 兑现：先把 co_await 提到**当前语句外层**（hoist），再在表达式位置
                // 返回这个临时变量的裸名。
                //
                // ⚠️ 为什么必须 hoist（实测 p3 报错，这是本改造最关键的一处）：
                //    调用点大量被包进 `[&]() -> auto { ... }()` IIFE（genGcRootedArgs
                //    的实参保护路径）。IIFE 的返回类型是 **deduced return type**，
                //    C++ 标准明令禁止在「返回类型推导」的函数体内出现 co_await
                //    （g++: "co_await cannot be used in a function with a deduced
                //    return type"）。消费点直接返回 "co_await a" 会落进那个 IIFE。
                //    → 用项目既有的 hoist 机制（genGcRootedArgs 的 outer 前缀同款，
                //      CodeGen.h 的 hoistPrefixPending_ / flushHoistPrefix）把
                //      `auto _awN = (co_await t);` 提升为语句，表达式位置只留变量名。
                //
                // ⚠️ 命名解析走 resolveFutureVar（源头变量名）而非原始名——别名形态
                //    `let t2 = t` 里 t2 只是拷贝名，承载 task 的是 t。
                // ⚠️ task<T> 是**值**（非 GC 指针）→ 不加 .get()。
                clearFutureVar(safeName(id->name));
                std::string avName = "_aw" + std::to_string(awaitHoistCounter_++) + "_"
                                   + safeName(src);
                hoistPrefixPending_ += "auto " + avName + " = (co_await "
                                     + safeName(src) + ");\n";
                return avName;
            }
        }
    }
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

// feature-12 批次 3（5.1）：receiver 映射的【值/指针】形态判定。
// 接口默认方法 receiver 是栈上视图【值】（DeclGen.cpp 生成 ViewRoot 包裹 + 句柄
// `_self_root`），`_self_root.get()` 产出的是视图值拷贝 —— 成员访问必须用 `.`；
// record 等方法 receiver 是 GC 指针，入口句柄 `.get()` 产出指针 —— 用 `->`。
// genIdentifier 的 receiver 分支只负责产出取值表达式，成员访问运算符由调用方
// （genMethodCall / genFieldAccess）按本判定选择。
bool CodeGenerator::receiverMapsToViewValue() const {
    if (!currentClosureThisHandle_.empty()) return false;   // 闭包句柄 → 指针形态
    if (currentReceiverName_.empty()) return false;
    return isIfaceViewTypeName(currentReceiverCppType_);
}

std::string CodeGenerator::genIdentifier(const Identifier& e) {
    // 方法/构造函数体内的接收者名（如 self, p）映射为 C++ 的 this
    if (!currentReceiverName_.empty() && e.name == currentReceiverName_) {
        // feature-06（阶段 B）：CallableObj 闭包体内 receiver 映射——先查捕获槽
        // 映射（cap_recv 槽承载句柄，取槽值即最新地址），再落入旧句柄机制。
        if (!currentClosureCaptures_.empty()) {
            auto ccit = currentClosureCaptures_.find(safeName(e.name));
            if (ccit != currentClosureCaptures_.end()) return ccit->second;
        }
        // bug-24：闭包体引用 receiver → 经闭包句柄 "_this_root.get()" 解引用
        //（#56 统一后协程/非协程闭包均句柄捕获，此分支恒先于方法体分支）
        if (!currentClosureThisHandle_.empty())
            return currentClosureThisHandle_ + ".get()";
        // #56：方法体直引 self → 入口句柄 "_this.get()"（GC 后取最新地址）。
        // feature-12 批次 3（5.1）：接口默认方法现也设 currentMethodThisHandle_
        //（"_self_root"），取值为视图值（视图恒定 {fnPtr, self}；self 由 ViewRoot
        // 内部 GcRootHandle 保活/重定位）。
        if (!currentMethodThisHandle_.empty())
            return currentMethodThisHandle_ + ".get()";
        return "this";
    }

    std::string name = safeName(e.name);

    // feature-06（阶段 B）：CallableObj 闭包体内捕获名映射——body 内经
    // currentClosureCaptures_ 将捕获变量引用改为槽位访问（"__c->cap_x"）。
    // 在 receiver/句柄映射之后、gcRoot/ViewRoot 之前判定：新路径闭包体内捕获
    // 变量一律由槽承载（槽即 GC 追踪对象，无需再经 .get() 解引用）。
    if (!currentClosureCaptures_.empty()) {
        auto ccit = currentClosureCaptures_.find(name);
        if (ccit != currentClosureCaptures_.end()) return ccit->second;
    }

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

std::string CodeGenerator::receiverThisSourceExpr() const {
    // 闭包体/spawn 体上下文：裸 this 不可见（lambda 只 init-capture 句柄），
    // 取外层闭包/物化句柄的最新地址作为内层句柄的源值
    if (!currentClosureThisHandle_.empty())
        return currentClosureThisHandle_ + ".get()";
    // 方法体直引：裸 this 可能已因方法体内前置 GC compact 悬垂（#56 主修复动机），
    // 取方法入口句柄（GC 会更新其内部值）为源——顺带消除「spawn/闭包定义前已 GC」
    // 的捕获悬垂隐患
    if (!currentMethodThisHandle_.empty())
        return currentMethodThisHandle_ + ".get()";
    // 无句柄上下文（接口默认方法 / 视图 this）：C++ 成员函数内 this 直接可见
    return "this";
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
        // bug-03/bug-04：return 语句上下文的空列表兜底——用当前函数/闭包的 C++ 返回类型
        // 提取列表元素类型（currentReturnCppType_ 形如 "aura_rt::Array<X>*" → X），生成
        // aura_rt::Array<X>::make(0)。修复泛型方法体 `return []` 被 currentTParams_[0]
        // 兜底生成 Array<T>::make(0)（与返回类型 Array<Transform<T>>* 不匹配）的坏 C++。
        // 仅 return 上下文生效（inReturnValueCtx_，genReturnStmt RAII 置位）：非 return 场景
        // （无标注 let/实参/字段初始化）不得用返回类型替换现状 currentTParams_ 兜底，
        // 否则破坏 A==U 取巧通过的现状用例（repro_aeqU 守回归）。X 非空且不含 "auto" 才用
        // （auto 返回/协程 task 兜底/多态闭包签名被守卫拒绝，回退 currentTParams_[0]）。
        // 提取 = 首 '<' 后 / 末 '>' 前（仿 StmtLet.cpp 空列表修复，任意嵌套深度成立）。
        if (inReturnValueCtx_ && currentReturnCppType_.find("aura_rt::Array<") == 0) {
            size_t rs = currentReturnCppType_.find('<');
            size_t re = currentReturnCppType_.rfind('>');
            if (rs != std::string::npos && re != std::string::npos && re > rs) {
                std::string retElem = currentReturnCppType_.substr(rs + 1, re - rs - 1);
                if (!retElem.empty()
                    && retElem != "auto"
                    && retElem.find("auto") == std::string::npos)
                    return "aura_rt::Array<" + retElem + ">::make(0)";
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
    // feature-06（阶段 C）：元素类型为裸 Callable（CallableSemType → Array<CallableErased*>）
    // → 元素逐一包装为 CallableErased 值（函数形态 → genErasedWrap；值拷贝透传）
    bool elemIsCallableErased = false;
    if (e.inferredType) {
        if (auto* lt0 = dynamic_cast<const ListSemType*>(e.inferredType))
            if (lt0->elementType)
                elemIsCallableErased =
                    dynamic_cast<const CallableSemType*>(lt0->elementType.get()) != nullptr;
    }
    for (auto& elem : e.elements) {
        // #13：Optional 元素列表——元素值经 genOptionalTargetInit 按目标元素 C++ 类型
        // 装箱：some(arg)/none()/已 Optional 值（Optional 变量、返回 Optional 的调用）直通，
        // 裸值（record 字面量等）make_optional<elemCpp> 装箱；非 Optional 元素保持 genExpr。
        // #7：接口视图元素且值为 record → record→view 转换（否则 Array<视图>::append
        // 直传 record 指针类型不匹配）；元素已是视图值（range 产 Iterator 等）直用。
        if (elem) {
            if (elemIsCallableErased) {
                elemExprs.push_back(genErasedInitValue(*elem, nullptr));
            } else if (!listElemCpp.empty()) {
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

    // #55/#29：递归计算列表元素 C++ 类型串（未绑定泛型 → 裸名，模板上下文合法），
    // 供嵌套泛型列表（[[T]]）的元素类型推导使用
    std::function<std::string(const SemType*)> listElemCppOf;
    listElemCppOf = [&](const SemType* t) -> std::string {
        if (auto* ls = dynamic_cast<const ListSemType*>(t))
            return "aura_rt::Array<" + listElemCppOf(ls->elementType.get()) + ">*";
        if (auto* g = dynamic_cast<const GenericSemType*>(t))
            return g->name;
        return mapSemType(*t);
    };

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
            // #55/#29：元素为未绑定泛型（T）或嵌套泛型列表（[[T]]）→ mapSemType 产
            // "auto"/"Array<auto>*" 不可用。改用泛型名（模板上下文合法）：IIFE 返回
            // Array<T>*，append(T 值)，T=string 实例化即 Array<GcString*>*（不再默认
            // int32_t 兜底坏 C++）；与 #29 元素 if constexpr 保护兼容。
            // 审查后修正（问题 B）：嵌套 [[T]] 时 mapSemType 产 "Array<auto>*"
            // （含 auto 但不等于 "auto"）→ 精确匹配漏判 → IIFE 返回 Array<int32_t>*
            // 坏 C++。改为包含判定；内部回退逻辑不变。G4 拦截（下方 find("auto")
            // 报错分支）仍会拦截残留 "Array<auto>*"。
            if (semElemType.find("auto") != std::string::npos) {
                if (auto* g = dynamic_cast<const GenericSemType*>(listTy->elementType.get())) {
                    if (g->name != "Iterator") semElemType = g->name;
                } else if (auto* ls2 = dynamic_cast<const ListSemType*>(listTy->elementType.get())) {
                    if (listContainsUnboundGeneric(ls2))
                        semElemType = "aura_rt::Array<"
                            + listElemCppOf(ls2->elementType.get()) + ">*";
                }
            }
            // 无效时回退到第一个元素的 inferredType（现状保留）
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

        // 检测 FunExpr（闭包）→ 转为 CallableObj 元素类型（feature-06：闭包值 =
        // GC 堆 CallableObj 指针）
        if (e.elements[0] && dynamic_cast<const FunExpr*>(e.elements[0].get())) {
            auto* fe = static_cast<const FunExpr*>(e.elements[0].get());
            std::string retType = fe->returnType ? mapType(*fe->returnType) : "void";
            std::string params;
            for (size_t j = 0; j < fe->params.size(); ++j) {
                if (j > 0) params += ", ";
                params += fe->params[j].type ? mapType(*fe->params[j].type) : "auto";
            }
            elemType = "aura_rt::CallableObj<" + retType + (params.empty() ? "" : ", " + params) + ">*";
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
    oss << "    aura_rt::GcRootHandle<decltype(_raw)> " << var << "(_raw, aura_rt::GcRootScope::ThreadLocal);\n";
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
        // #29：元素为未绑定泛型（isHeap 判 true 但类型是裸 T）→ if constexpr 延迟判定：
        // T=值类型 → is_convertible false → 不包装（消除 GcRootHandle<int> 假根）；
        // T=堆（record/GcString*/Optional*/Variant* 继承 GcObject）→ 仍保护（不漏保护）。
        bool deferred = isHeap && e.elements[i]
            && isDeferredGcRoot(e.elements[i]->inferredType);
        std::string vi = "_e" + std::to_string(idx) + "_" + std::to_string(i);
        oss << "    auto " << vi << " = (" << elemExprs[i] << ");\n";
        if (isHeap && !deferred) {
            oss << "    aura_rt::GcRootHandle<decltype(" << vi << ")> _eh"
                << idx << "_" << i << "(" << vi << ");\n";
            oss << "    " << var << ".get()->append(_eh" << idx << "_" << i << ".get());\n";
        } else if (deferred) {
            std::string hi = "_eh" + std::to_string(idx) + "_" + std::to_string(i);
            oss << "    if constexpr (std::is_convertible_v<decltype(" << vi << "), aura_rt::GcObject*>) {\n";
            oss << "        aura_rt::GcRootHandle<decltype(" << vi << ")> " << hi << "(" << vi << ");\n";
            oss << "        " << var << ".get()->append(" << hi << ".get());\n";
            oss << "    } else {\n";
            oss << "        " << var << ".get()->append(" << vi << ");\n";
            oss << "    }\n";
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
        oss << "    aura_rt::GcRootHandle<decltype(_raw)> " << var << "(_raw, aura_rt::GcRootScope::ThreadLocal);\n";
        for (auto& f : e.fields) {
            // 堆类型字段值：预求值并用 GcRootHandle 保护
            // #10：按字段声明类型（e.inferredType->fields）装箱（Optional/Variant 字段）
            // #3：接口视图字段（值类型）→ outViewValue 置 true，跳过 GcRootHandle
            bool isViewField = false;
            std::string fval = f.value
                ? genRecordFieldValue(e.inferredType, *f.value, f.name, isCoroutine, &isViewField) : "???";
            bool isHeapF = f.value && isHeapSemType(f.value->inferredType) && !isViewField;
            bool deferredF = isHeapF && f.value && isDeferredGcRoot(f.value->inferredType);
            if (isHeapF && !deferredF) {
                oss << "    auto _fv_" << safeName(f.name) << " = (" << fval << ");\n";
                oss << "    aura_rt::GcRootHandle<decltype(_fv_" << safeName(f.name)
                    << ")> _fh_" << safeName(f.name) << "(_fv_" << safeName(f.name) << ");\n";
                oss << "    " << var << ".get()->" << safeName(f.name)
                    << " = _fh_" << safeName(f.name) << ".get();\n";
            } else if (deferredF) {
                std::string fv = "_fv_" + safeName(f.name);
                std::string fh = "_fh_" + safeName(f.name);
                oss << "    auto " << fv << " = (" << fval << ");\n";
                oss << "    if constexpr (std::is_convertible_v<decltype(" << fv
                    << "), aura_rt::GcObject*>) {\n";
                oss << "        aura_rt::GcRootHandle<decltype(" << fv << ")> " << fh
                    << "(" << fv << ");\n";
                oss << "        " << var << ".get()->" << safeName(f.name)
                    << " = " << fh << ".get();\n";
                oss << "    } else {\n";
                oss << "        " << var << ".get()->" << safeName(f.name)
                    << " = " << fv << ";\n";
                oss << "    }\n";
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

} // namespace Aura
