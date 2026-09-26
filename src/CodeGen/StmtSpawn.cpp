#include "CodeGen.h"
#include "../Sema/BuiltinRegistry.h"
#include "../Sema/SemType.h"
#include <sstream>

namespace Aura {

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
    // 检查用户是否已声明 io（feature-14 P2：_tasks 形参已整体退役，不再检测）
    bool hasIo = false;
    for (auto& p : stmt.params) {
        if (p.name == "io") hasIo = true;
    }

    // bug-24：spawn 绑定列表引用 receiver（self/p）→ 该参数是 this 别名（body 内经
    // genIdentifier 映射 self→this），不得声明为 lambda 参数（会产出自引用未用参数 +
    // this 未捕获坏 C++）→ 改为 [this] 捕获、不声明参数、不传实参。
    bool needsThisCapture = false;
    for (auto& p : stmt.params)
        if (!currentReceiverName_.empty() && p.name == currentReceiverName_)
            needsThisCapture = true;

    // 整条 requireSync()->addTask(...) 先写入缓冲 out：显式实参需经
    // genGcRootedArgs 包装（bug-42）保护，须在实参求值前备好完整调用串
    // （占位符 {i}）；同名自动绑定路径无堆临时值风险，直接透出缓冲。
    //
    // feature-14 P2：spawn 产物不再经词法 _tasks 形参传递，改为「生成时刻求值
    // 最近的 sync 域」——requireSync() 在无域时运行时 panic（change.md §3.1 / Q5），
    // SyncContext::addTask 内部绑定 SpawnTask{body, owner}（GC-4 创建时绑定）。
    // ⚠️ 收尾括号配对：本行多一层 '(' （addTask( ），故下方 ":161 的 \"))\" 必须同步
    //    改为 \")\" —— 那行不含 _tasks，grep 不命中，漏改即坏 C++。
    std::ostringstream out;
    // 生成 lambda 签名为显式参数
    out << indentStr() << "aura_rt::requireSync()->addTask([";
    // #56 §1.8：方法上下文 spawn lambda 引用 receiver → init-capture 专属句柄
    //（Global 根：spawn 跨线程/跨挂起逃逸；_sp_this 避免与 _this/_this_root 冲突）。
    // 缺口 3：capture-init 源不写裸 this——闭包/spawn 体内（this 不可见）取外层句柄
    // .get()；方法体直引取入口句柄 .get()（裸 this 可能已因方法体内前置 GC 悬垂）。
    if (needsThisCapture)
        out << "_sp_this = aura_rt::GcRootHandle<" << currentReceiverCppType_
            << "*>(" << receiverThisSourceExpr() << ", aura_rt::GcRootScope::Global)";
    out << "](";
    bool firstParam = true;
    for (size_t i = 0; i < stmt.params.size(); ++i) {
        if (!currentReceiverName_.empty() && stmt.params[i].name == currentReceiverName_)
            continue;   // receiver 参数 → this 捕获，不声明为参数
        if (!firstParam) out << ", ";
        firstParam = false;
        out << (stmt.params[i].type ? mapParamType(*stmt.params[i].type) : "auto")
            << " " << safeName(stmt.params[i].name);
    }
    // #46：body 是否实际引用 io（穿透嵌套 spawn/语言闭包，IdRefCollector 保守收集）。
    // 用户已显式声明 io 参数（hasIo）时无需收集（body 中 io 即该参数）。
    bool bodyRefsIo = false;
    if (!hasIo) {
        std::set<std::string> refs;
        IdRefCollector ioCol(refs);
        for (auto& s : stmt.body) if (s) ioCol.collectStmt(*s);
        bodyRefsIo = refs.count("io") > 0;
    }
    // #46 兜底：调用需要 io（显式声明同名绑定 / body 引用需追加）但外层无 io → 干净报错
    if ((hasIo || bodyRefsIo) && !ioInScope_) {
        error(stmt, "spawn requires an 'io' variable in the enclosing scope; "
                    "add an 'io: Io' parameter to the enclosing function");
        return;
    }
    // 自动追加 io（仅 body 实际引用 io 时，按需）
    // feature-14 P2：_tasks 形参不再追加（域归属改由 requireSync() 运行时解析）
    if (!hasIo && bodyRefsIo) out << ", aura_rt::Io& io";
    out << ") -> aura_rt::task<void> {\n";
    insideSpawn_ = true;
    // #56 §1.8：spawn lambda 体生成期间隔离外层闭包/方法句柄映射（外层闭包内再
    // spawn 引用 self 亦映射自身捕获句柄，不依赖外层 _this_root 可见性）；体后恢复。
    // 缺口 1：协程 lambda init-capture 的 GcRootHandle 存在 g++ 暂存窗口（globalRoots_
    // 中被 GC 更新的根 ≠ 任务体首段实际读取的句柄）→ task 体首段触发 GC 后句柄不
    // 重定位 → 悬垂崩溃。修复：body 首语句物化 frame-local 句柄 _sp_this_f（从捕获句柄
    // 取当前值注册新根；物化先于任何 GC，源值必为对象当前地址），体内 self 恒映射
    // 物化句柄（frame 内地址固定 + 注册正确 → GC 可重定位，实测对照组）。
    std::string savedClosureHandle = currentClosureThisHandle_;
    std::string savedMethodHandle = currentMethodThisHandle_;
    if (needsThisCapture) currentClosureThisHandle_ = "_sp_this_f";
    // #46：spawn lambda 签名含 io（用户声明 or 按需追加）→ body 生成期间 io 参数在
    // lambda 作用域内可见，置 ioInScope_（save/restore）供嵌套 spawn 判定。
    bool savedIoInScope = ioInScope_;
    if (hasIo || bodyRefsIo) ioInScope_ = true;

    // bug-75: spawn 形参中的函数类型（fun 值）注册进 callableObjVars_ —— body 内对
    // 形参的直呼 g(x) 命中 ExprCall isFunValueCall -> g->invoke(g, x)。spawn lambda
    // 形参实为裸 aura_rt::CallableObj<R,...>*（非 GcRootHandle），直呼即坏 C++。
    // 与 DeclFun.cpp:177（函数形参）/ ExprClosure.cpp:1367（闭包形参）同源同机制；
    // spawn 形态此前漏注册 —— bug-75 唯一根因（协程/线程两形态同缺口）。
    auto savedSpawnCallableVarsS = callableObjVars_;
    for (auto& spS : stmt.params) {
        if (isFunctionTypedParam(spS))
            callableObjVars_.insert(safeName(spS.name));
    }
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

        // 缺口 1：task body 首语句物化 frame-local 句柄（见上注释）
        if (needsThisCapture)
            out << "aura_rt::GcRootHandle<" << currentReceiverCppType_
                << "*> _sp_this_f(_sp_this.get(), aura_rt::GcRootScope::Global);\n";

        for (auto& s : stmt.body)
            if (s) genStmt(out, *s, true);
    }
    callableObjVars_ = savedSpawnCallableVarsS;   // bug-75: 恢复外层集合
    ioInScope_ = savedIoInScope;

    insideSpawn_ = false;
    currentClosureThisHandle_ = savedClosureHandle;
    currentMethodThisHandle_ = savedMethodHandle;
    out << indentStr() << "    co_return;\n";
    out << indentStr() << "}(";

    // 实参：同名自动绑定 or 显式传入
    if (!stmt.args.empty()) {
        // 显式实参：占位符 {i}，由 genGcRootedArgs 替换为受保护实参变量
        for (size_t i = 0; i < stmt.args.size(); ++i) {
            if (i > 0) out << ", ";
            out << "{" << i << "}";
        }
    } else {
        bool firstArg = true;
        for (size_t i = 0; i < stmt.params.size(); ++i) {
            if (!currentReceiverName_.empty() && stmt.params[i].name == currentReceiverName_)
                continue;   // receiver 参数不传实参（由 [this] 捕获提供）
            if (!firstArg) out << ", ";
            firstArg = false;
            // 同名自动绑定：外层 GcRootHandle 变量 → 传 .get() 裸指针（guards 已析构，
            // gcRootVarNames_ 已恢复外层状态）
            std::string pname = safeName(stmt.params[i].name); // 同名自动绑定
            out << (gcRootVarNames_.count(pname) ? pname + ".get()" : pname);
        }
    }
    if (!hasIo && bodyRefsIo) out << ", io";   // #46：仅 body 实际引用 io 时传 io
    // feature-14 P2：实参不再追加 _tasks。
    // ⚠️ 收尾括号必须是 "))" —— 两层：
    //    ① 闭合任务体 lambda 的立即调用 `}(io`  → 一个 ')'
    //    ② 闭合 L47/L51 的 `addTask(`             → 一个 ')'
    //    （曾误改为单个 ")" → 生成 `}(io);` 少一层 → g++ 报 "expected ')' before ';'"）
    out << "))";

    if (!stmt.args.empty()) {
        // bug-42：spawn 显式实参走 genGcRootedArgs 包装（与普通调用对齐）——多实参
        // 求值期间，若前序实参产生堆临时值（如 concat string）且后序实参求值触发 GC，
        // 该临时值未被根保护 → 悬垂/回收。genGcRootedArgs 逐参 GcRootHandle 保护后再
        // 调用；实参 inferredType 由 bug-21 Sema 校验（inferExpr）提供。
        std::vector<std::pair<std::string, const SemType*>> gcArgs;
        gcArgs.reserve(stmt.args.size());
        for (size_t i = 0; i < stmt.args.size(); ++i)
            gcArgs.emplace_back(genExpr(*stmt.args[i], true), stmt.args[i]->inferredType);
        std::string callText = genGcRootedArgs(gcArgs, out.str(), true);
        flushHoistPrefix(cpp);   // #31：实参协程 outer 前缀先落盘
        cpp << callText << ";\n";
    } else {
        cpp << out.str() << ";\n";
    }
}

// 调用形态（协程 sync 块内）：spawn func(args)
// 生成：aura_rt::requireSync()->addTask(
//           [](auto fv..., Io& io) -> task<void> { 调用; co_return; }(fv..., io));
void CodeGenerator::genSpawnCallAsCoro(std::ostream& cpp, const SpawnStmt& stmt) {
    // 1. 自由变量 = 调用表达式中所有 Identifier - 函数/类型名 - 内置
    std::set<std::string> allRefs;
    IdRefCollector idCol(allRefs);
    idCol.collectExpr(*stmt.callExpr);   // 含 callee + args
    // #46：callExpr 是否实际引用 io（builtins 排除前记录——io 不进 freeVars）
    bool refsIo = allRefs.count("io") > 0;
    // feature-14 P2：_tasks 已从闭包自由变量排除集移除（不再有该内部名）
    std::set<std::string> builtins = {"io"};
    std::vector<std::string> freeVars;
    // bug-24：调用表达式引用 receiver（self/p）→ 不进 freeVars（this 别名），lambda 改 [this] 捕获
    bool needsThisCapture = false;
    for (auto& name : allRefs) {
        if (!currentReceiverName_.empty() && name == currentReceiverName_) {
            needsThisCapture = true;
            continue;
        }
        if (builtins.count(name)) continue;
        if (registeredTypes_.count(name)) continue;  // 函数名/类型名不捕获
        // 内置函数名不捕获（同 StmtSync——关联调研发现 (a)：gc_force 等被当自由变量）
        if (BuiltinRegistry::get().hasFunctionName(name)) continue;
        freeVars.push_back(name);
    }
    // #46 兜底：调用需 io（callExpr 引用 io 需追加）但外层无 io → 干净报错
    if (refsIo && !ioInScope_) {
        error(stmt, "spawn requires an 'io' variable in the enclosing scope; "
                    "add an 'io: Io' parameter to the enclosing function");
        return;
    }

    // 2. 协程 lambda：引用 receiver 时 init-capture 专属句柄 _sp_this（#56 §1.8，
    //    Global 根跨线程/跨挂起；capture-init 源同 genSpawnStmt——缺口 3 取外层句柄
    //    .get() / 入口句柄 .get()，不写裸 this）
    cpp << indentStr() << "aura_rt::requireSync()->addTask([";
    if (needsThisCapture)
        cpp << "_sp_this = aura_rt::GcRootHandle<" << currentReceiverCppType_
            << "*>(" << receiverThisSourceExpr() << ", aura_rt::GcRootScope::Global)";
    cpp << "](";
    // feature-14 P2：_tasks 形参已移除。原尾部为 `..., Io& io, taskvec& _tasks)`
    // 或 `..., taskvec& _tasks)`；去掉 _tasks 后逗号归属重新计算（trailing 参数的前
    // 导逗号）、再补 ")" —— 避免生成 `Io& io, )` 或 `auto fv, )` 坏 C++。
    bool firstParamDone = false;
    for (auto& v : freeVars) {
        if (firstParamDone) cpp << ", ";
        cpp << "auto " << safeName(v);
        firstParamDone = true;
    }
    if (refsIo) {   // #46：callExpr 实际引用 io 才追加
        if (firstParamDone) cpp << ", ";
        cpp << "aura_rt::Io& io";
        firstParamDone = true;
    }
    (void)firstParamDone;
    cpp << ") -> aura_rt::task<void> {\n";
    indentLevel_++;
    insideSpawn_ = true;
    // #56 §1.8：spawn lambda 体生成期间隔离外层闭包/方法句柄映射；体后恢复
    std::string savedClosureHandle = currentClosureThisHandle_;
    std::string savedMethodHandle = currentMethodThisHandle_;
    // bug-75: spawn 形参中的函数类型（fun 值）注册进 callableObjVars_ —— body 内对
    // 形参的直呼 g(x) 命中 ExprCall isFunValueCall -> g->invoke(g, x)。spawn lambda
    // 形参实为裸 aura_rt::CallableObj<R,...>*（非 GcRootHandle），直呼即坏 C++。
    // 与 DeclFun.cpp:177（函数形参）/ ExprClosure.cpp:1367（闭包形参）同源同机制；
    // spawn 形态此前漏注册 —— bug-75 唯一根因（协程/线程两形态同缺口）。
    auto savedSpawnCallableVarsC = callableObjVars_;
    for (auto& spC : stmt.params) {
        if (isFunctionTypedParam(spC))
            callableObjVars_.insert(safeName(spC.name));
    }
    if (needsThisCapture) {
        currentClosureThisHandle_ = "_sp_this_f";
        // 缺口 1：task body 首语句物化 frame-local 句柄（协程 lambda init-capture 的
        // GcRootHandle 存在 g++ 暂存窗口 → 首段 GC 后不重定位；物化句柄先于任何 GC
        // 注册、地址固定 → GC 可重定位），体内 self 恒映射物化句柄
        writeLine(cpp, "aura_rt::GcRootHandle<" + currentReceiverCppType_
                      + "*> _sp_this_f(_sp_this.get(), aura_rt::GcRootScope::Global);");
    }
    // isCoroutine=true：若 callee 为协程函数，genExpr 自动加 co_await；返回值丢弃
    writeLine(cpp, genExpr(*stmt.callExpr, true) + ";");
    insideSpawn_ = false;
    callableObjVars_ = savedSpawnCallableVarsC;   // bug-75: 恢复外层集合
    currentClosureThisHandle_ = savedClosureHandle;
    currentMethodThisHandle_ = savedMethodHandle;
    writeLine(cpp, "co_return;");
    indentLevel_--;
    cpp << indentStr() << "}(";
    // feature-14 P2：_tasks 实参已移除；逗号归属重新计算 + 收尾 ")" 配对 addTask(
    bool firstArgDone = false;
    for (auto& v : freeVars) {
        if (firstArgDone) cpp << ", ";
        cpp << safeName(v);
        firstArgDone = true;
    }
    if (refsIo) {   // #46：追加了 io 参数才传 io
        if (firstArgDone) cpp << ", ";
        cpp << "io";
        firstArgDone = true;
    }
    (void)firstArgDone;
    cpp << "));\n";
}

// bug-73：调用形态 spawn 的目标是否为协程函数（具名函数 / 协程闭包 / 协程方法）。
//
// 判定口径与 genCall / genMethodCall 的 co_await 判定同源（bug-02 固定点收敛后的
// coroutineFunctions_）：
//   - 具名函数：键 = 函数名（coroutineFunctions_）
//   - 协程闭包值：coroClosureNames_ 按名登记
//   - 方法：键 = "ReceiverType.methodName"，receiver 类型取 inferredType 的
//     RecordSemType.canonicalName 截断 '<'（ExprMethodCall.cpp 同款口径）
// 判定为真 ⇒ 生成的调用返回 lazy aura_rt::task<T> ⇒ 线程版必须显式驱动
//（否则返回值被 submit 的 std::function<void()> 擦除 → 协程体静默不执行）。
bool CodeGenerator::spawnCallTargetIsCoroutine(const ASTNode& callExpr) const {
    if (auto* call = dynamic_cast<const CallExpr*>(&callExpr)) {
        auto* id = dynamic_cast<const Identifier*>(call->callee.get());
        if (!id) return false;
        return coroutineFunctions_.count(id->name) > 0
            || coroClosureNames_.count(id->name) > 0;
    }
    if (auto* mc = dynamic_cast<const MethodCallExpr*>(&callExpr)) {
        if (!mc->object || !mc->object->inferredType) return false;
        auto* rec = dynamic_cast<const RecordSemType*>(mc->object->inferredType);
        if (!rec || rec->canonicalName.empty()) return false;
        std::string recvKey = rec->canonicalName;
        size_t lt = recvKey.find('<');
        if (lt != std::string::npos) recvKey = recvKey.substr(0, lt);
        return coroutineFunctions_.count(recvKey + "." + mc->method) > 0;
    }
    return false;
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
    std::set<std::string> builtins = {"io"};   // feature-14 P2: _tasks 退役
    std::vector<std::string> freeVars;
    bool ioUsed = false;
    // bug-24：调用表达式引用 receiver（self/p）→ 不进 freeVars（this 别名），lambda 改 [this] 捕获
    bool needsThisCapture = false;
    for (auto& name : allRefs) {
        if (!currentReceiverName_.empty() && name == currentReceiverName_) {
            needsThisCapture = true;
            continue;
        }
        if (name == "io") { ioUsed = true; continue; }
        if (builtins.count(name)) continue;
        if (registeredTypes_.count(name)) continue;
        // 内置函数名不捕获（同 StmtSync——关联调研发现 (a)：gc_force 等被当自由变量）
        if (BuiltinRegistry::get().hasFunctionName(name)) continue;
        freeVars.push_back(name);
    }
    // #46 兜底：线程形态 callExpr 引用 io（&io 引用捕获）但外层无 io → 干净报错
    if (ioUsed && !ioInScope_) {
        error(stmt, "spawn requires an 'io' variable in the enclosing scope; "
                    "add an 'io: Io' parameter to the enclosing function");
        ioSync_ = oldIoSync;
        currentFunctionIsCoroutine_ = oldCoroutine;
        return;
    }

    // 2. 捕获列表：引用 receiver 时 init-capture 专属句柄 _sp_this（#56 §1.8，
    //    Global 根跨线程逃逸；capture-init 源同 genSpawnStmt——缺口 3 取外层句柄
    //    .get() / 入口句柄 .get()，不写裸 this）+ freeVars 值捕获 + io 引用捕获
    cpp << indentStr() << "_stx.submit([";
    bool firstCapture = true;
    if (needsThisCapture) {
        cpp << "_sp_this = aura_rt::GcRootHandle<" << currentReceiverCppType_
            << "*>(" << receiverThisSourceExpr() << ", aura_rt::GcRootScope::Global)";
        firstCapture = false;
    }
    for (size_t i = 0; i < freeVars.size(); ++i) {
        if (!firstCapture) cpp << ", ";
        firstCapture = false;
        // bug-72：GC 根 / 视图根自由变量 → Global 根 init-capture（ThreadLocal 句柄副本
        // 在提交线程注册、worker 线程析构 → 摘错 thread-local 根链表 → 扫根 GC UAF）
        cpp << crossThreadCaptureItem(freeVars[i]);
    }
    if (ioUsed) {
        if (!firstCapture) cpp << ", ";
        cpp << "&io";
    }
    cpp << "]() mutable {";
    indentLevel_++;
    insideSpawn_ = true;
    // #56 §1.8：lambda 体生成期间隔离外层闭包/方法句柄映射（body 内 self → 捕获句柄
    // .get()）；体后恢复。线程版为普通 lambda（非协程），无缺口 1 的协程暂存窗口，
    // 直接映射 init-capture 句柄（move 进线程池队列时注册正确迁移）
    std::string savedClosureHandle = currentClosureThisHandle_;
    std::string savedMethodHandle = currentMethodThisHandle_;
    // bug-75: spawn 形参中的函数类型（fun 值）注册进 callableObjVars_ —— body 内对
    // 形参的直呼 g(x) 命中 ExprCall isFunValueCall -> g->invoke(g, x)。spawn lambda
    // 形参实为裸 aura_rt::CallableObj<R,...>*（非 GcRootHandle），直呼即坏 C++。
    // 与 DeclFun.cpp:177（函数形参）/ ExprClosure.cpp:1367（闭包形参）同源同机制；
    // spawn 形态此前漏注册 —— bug-75 唯一根因（协程/线程两形态同缺口）。
    auto savedSpawnCallableVarsT = callableObjVars_;
    for (auto& spT : stmt.params)
        if (isFunctionTypedParam(spT))
            callableObjVars_.insert(safeName(spT.name));
    if (needsThisCapture) currentClosureThisHandle_ = "_sp_this";
    // bug-73：callee 为协程函数时调用返回 lazy task<void>/<T>——submit 的
    // std::function<void()> 会擦除返回值并立即析构（initial_suspend = suspend_always）
    // → 协程体静默不执行。改在 worker 内显式驱动至完成（run_to_completion 内部
    // resume 至 completion + 异常重抛；抛出后由 workerLoop 记入 group 异常列表）。
    std::string callText = genExpr(*stmt.callExpr, false);
    if (spawnCallTargetIsCoroutine(*stmt.callExpr))
        callText = "aura_rt::run_to_completion(" + callText + ")";
    writeLine(cpp, callText + ";");
    insideSpawn_ = false;
    callableObjVars_ = savedSpawnCallableVarsT;   // bug-75: 恢复外层集合
    currentClosureThisHandle_ = savedClosureHandle;
    currentMethodThisHandle_ = savedMethodHandle;
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
    bool explicitArgs = !stmt.args.empty();
    std::vector<std::string> captureItems;   // 非 io 参数捕获项（裸名 或 init-capture）
    // #56 §1.8（线程版同款）：params 中的 receiver 名（self/p）是 this 别名——不声明为
    // lambda 参数、不捕获裸名（外层无此 C++ 变量），改 init-capture 专属句柄 _sp_this
    //（Global 根跨线程逃逸；capture-init 源同其余落点——缺口 3 取外层句柄 .get() /
    // 入口句柄 .get()，不写裸 this）
    bool needsThisCapture = false;
    for (auto& p : stmt.params)
        if (!currentReceiverName_.empty() && p.name == currentReceiverName_)
            needsThisCapture = true;
    if (needsThisCapture)
        captureItems.push_back("_sp_this = aura_rt::GcRootHandle<" + currentReceiverCppType_
                               + "*>(" + receiverThisSourceExpr()
                               + ", aura_rt::GcRootScope::Global)");
    // 值类型显式实参参数：body 生成期间屏蔽外层同名 GcRootHandle 残留（否则误生 .get()）
    std::vector<IterVarGuard> valueGuards;
    valueGuards.reserve(stmt.params.size()); // 防扩容拷贝迁移提前析构恢复（同 genSpawnStmt）
    // 堆类型显式实参的 handle 名（body 生成期间注册 gcRootVarNames_ → 参数名自动 .get()）
    std::vector<std::pair<std::string, std::string>> heapInitNames; // (name, cppType)
    for (size_t i = 0; i < stmt.params.size(); ++i) {
        if (stmt.params[i].name == "io") {
            hasIo = true;
            continue;  // io 单独处理
        }
        // receiver 参数 → this 别名，由 _sp_this init-capture 提供（同协程版 genSpawnStmt）
        if (!currentReceiverName_.empty() && stmt.params[i].name == currentReceiverName_)
            continue;
        std::string pname = safeName(stmt.params[i].name);
        // bug-10：显式实参 init-capture。args[i] 索引与 params[i] 位置对齐（io 在 params
        // 中占位，如 (io,x)(io,3) 中 args[0]=io 对应 params[0]=io），跳过 io 时勿收缩索引。
        if (explicitArgs && i < stmt.args.size() && stmt.args[i]) {
            const SemType* argTy = stmt.args[i]->inferredType;
            if (argTy && isHeapSemType(argTy) && !isIfaceView(argTy)) {
                // 堆类型实参：init-capture 持 GcRootHandle Global 根（对齐 ExprClosure
                // L479-484 跨线程捕获先例——ThreadLocal 根对跨线程闭包不安全，须 Global）。
                // 裸值 init-capture 捕获 .get() 裸指针，worker 线程 GC 扫描看不到提交线程
                // 栈上源值 → 悬垂，故必须 Global 根形态。T 用 mapSemType 得实参 C++ 指针类型。
                std::string expr = genExpr(*stmt.args[i], false);
                std::string typeName = mapSemType(*argTy);
                captureItems.push_back(pname + " = aura_rt::GcRootHandle<" + typeName
                                       + ">(" + expr + ", aura_rt::GcRootScope::Global)");
                heapInitNames.emplace_back(pname, typeName);
            } else {
                // 值类型实参（含接口视图——isIfaceView 无法 GcRootHandle 包裹，按值处理）：
                // 裸 init-capture，实参在捕获初始化器求值（提交线程、外层作用域）。
                captureItems.push_back(pname + " = " + genExpr(*stmt.args[i], false));
                // 值捕获为裸值，body 内参数名须为裸标识符；屏蔽外层同名 GcRootHandle 残留
                valueGuards.emplace_back(gcRootVarNames_, gcRootTypes_, pname);
            }
        } else {
            // 同名自动绑定（args 空 或 数量不匹配兜底——bug-21 Sema 已拦截数量不匹配，兜底为死代码）
            // bug-72：外层同名变量若为 GC 根 / 视图根 → Global 根 init-capture（裸名值捕获的
            // ThreadLocal 副本跨线程析构会摘错根链表 → 后续扫根 GC UAF）
            captureItems.push_back(crossThreadCaptureItem(stmt.params[i].name));
        }
    }
    // #46 兜底：线程形态 spawn 显式声明 io 参数（&io 引用捕获）但外层无 io → 干净报错
    if (hasIo && !ioInScope_) {
        error(stmt, "spawn requires an 'io' variable in the enclosing scope; "
                    "add an 'io: Io' parameter to the enclosing function");
        ioSync_ = oldIoSync;
        currentFunctionIsCoroutine_ = oldCoroutine;
        return;
    }
    // 值捕获列表
    for (size_t i = 0; i < captureItems.size(); ++i) {
        if (i > 0) cpp << ", ";
        cpp << captureItems[i];
    }
    // io 引用捕获（最后添加）
    if (hasIo) {
        if (!captureItems.empty()) cpp << ", ";
        cpp << "&io";
    }
    cpp << "]() mutable {";

    // 堆类型显式实参：body 生成期间注册 gcRootVarNames_/gcRootTypes_，参数名引用自动
    // 生成 .get()（闭包体内 GcRootHandle 解引用取最新指针）；body 结束后恢复外层状态
    struct SavedHeapRoot { std::string name; bool wasRoot; bool hadType; std::string savedType; };
    std::vector<SavedHeapRoot> savedHeapRoots;
    savedHeapRoots.reserve(heapInitNames.size());
    for (auto& hp : heapInitNames) {
        SavedHeapRoot sr;
        sr.name = hp.first;
        sr.wasRoot = gcRootVarNames_.count(hp.first) > 0;
        auto it = gcRootTypes_.find(hp.first);
        sr.hadType = it != gcRootTypes_.end();
        if (sr.hadType) sr.savedType = it->second;
        gcRootVarNames_.insert(hp.first);
        gcRootTypes_[hp.first] = hp.second;
        savedHeapRoots.push_back(std::move(sr));
    }

    // lambda body
    indentLevel_++;
    insideSpawn_ = true;
    // #56 §1.8：body 生成期间隔离外层闭包/方法句柄映射（body 内 self → 捕获句柄
    // .get()）；体后恢复。线程版为普通 lambda（非协程），无缺口 1 协程暂存窗口，
    // 直接映射 init-capture 句柄（move 进线程池队列时注册正确迁移）
    std::string savedClosureHandle = currentClosureThisHandle_;
    std::string savedMethodHandle = currentMethodThisHandle_;
    // bug-75: spawn 形参中的函数类型（fun 值）注册进 callableObjVars_ —— body 内对
    // 形参的直呼 g(x) 命中 ExprCall isFunValueCall -> g->invoke(g, x)。spawn lambda
    // 形参实为裸 aura_rt::CallableObj<R,...>*（非 GcRootHandle），直呼即坏 C++。
    // 与 DeclFun.cpp:177（函数形参）/ ExprClosure.cpp:1367（闭包形参）同源同机制；
    // spawn 形态此前漏注册 —— bug-75 唯一根因（协程/线程两形态同缺口）。
    auto savedSpawnCallableVarsT2 = callableObjVars_;
    for (auto& spT2 : stmt.params)
        if (isFunctionTypedParam(spT2))
            callableObjVars_.insert(safeName(spT2.name));
    if (needsThisCapture) currentClosureThisHandle_ = "_sp_this";
    // 显式参数已在 Sema 中注册为只读符号，此处直接生成体
    for (auto& s : stmt.body) {
        if (s) genStmt(cpp, *s, false);  // 非协程！
    }
    insideSpawn_ = false;
    callableObjVars_ = savedSpawnCallableVarsT2;   // bug-75: 恢复外层集合
    currentClosureThisHandle_ = savedClosureHandle;
    currentMethodThisHandle_ = savedMethodHandle;
    indentLevel_--;
    cpp << "\n";

    // 恢复外层 gcRootVarNames_/gcRootTypes_ 状态
    for (auto& sr : savedHeapRoots) {
        if (!sr.wasRoot) gcRootVarNames_.erase(sr.name);
        if (sr.hadType) gcRootTypes_[sr.name] = sr.savedType;
        else gcRootTypes_.erase(sr.name);
    }

    cpp << indentStr() << "});\n";

    ioSync_ = oldIoSync;
    currentFunctionIsCoroutine_ = oldCoroutine;
}

// ============================================================
// bug-72：跨线程捕获发射处的 GC 根捕获形态
//
// 机制：GcRootHandle / ViewRoot 内的句柄是 thread-local 侵入式链表节点（注册与注销都用
// 「当前线程」的 tl_roots_）。spawn / sync thread 的 lambda 在**提交线程**构造（句柄注册
// 进提交线程链表），却在**执行它的 worker 线程**析构 → unregisterRootThreadLocal 用
// worker 的 tl_roots_ 摘链（worker 无链表时直接 return）→ 提交线程链表残留已析构节点 →
// 之后任意扫根 GC 读取已释放内存（ASAN 实测：mark_sweep.cpp 扫根 READ heap-use-after-free）。
//
// 修复：命中的 GC 根 / 视图根捕获改**同名 init-capture 的 Global 根**——注册进
// globalRoots_（互斥保护、按 ptr_ref_ 值查找），注册线程与析构线程无关；lambda 体内
// `name.get()` 形态不变（同名 init-capture 遮蔽外层变量），生成代码其余部分零改动。
// 同名命中两项时优先 GcRootHandle（与 genIdentifier 判定顺序一致）。
// 返回：未命中 → 裸名（原行为不变）。
// ============================================================
std::string CodeGenerator::crossThreadCaptureItem(const std::string& rawName) const {
    const std::string cn = safeName(rawName);
    if (gcRootVarNames_.count(cn)) {
        auto it = gcRootTypes_.find(cn);
        if (it != gcRootTypes_.end())
            return cn + " = aura_rt::GcRootHandle<" + it->second + ">(" + cn
                   + ".get(), aura_rt::GcRootScope::Global)";
    } else if (viewRootVarNames_.count(cn)) {
        auto it = viewRootTypes_.find(cn);
        if (it != viewRootTypes_.end())
            return cn + " = aura_rt::ViewRoot<" + it->second + ">(" + cn + ".v, " + cn
                   + ".h.get(), aura_rt::GcRootScope::Global)";
    }
    return cn;
}

// 同上，但返回**实参表达式**：协程 lambda 的自由变量经形参（`auto v`）承载，不是 capture。
// 形参同样是句柄副本（注册于创建线程、随协程帧在任意线程析构）→ 实参改传 Global 根临时值；
// `auto` 推导类型与修复前完全一致（GcRootHandle<T> / ViewRoot<T>），lambda 体生成零改动。
std::string CodeGenerator::crossThreadGlobalArg(const std::string& rawName) const {
    const std::string cn = safeName(rawName);
    if (gcRootVarNames_.count(cn)) {
        auto it = gcRootTypes_.find(cn);
        if (it != gcRootTypes_.end())
            return "aura_rt::GcRootHandle<" + it->second + ">(" + cn
                   + ".get(), aura_rt::GcRootScope::Global)";
    } else if (viewRootVarNames_.count(cn)) {
        auto it = viewRootTypes_.find(cn);
        if (it != viewRootTypes_.end())
            return "aura_rt::ViewRoot<" + it->second + ">(" + cn + ".v, " + cn
                   + ".h.get(), aura_rt::GcRootScope::Global)";
    }
    return cn;
}

} // namespace Aura
