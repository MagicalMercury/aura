#include "CodeGen.h"
#include "../Sema/BuiltinRegistry.h"
#include "../Sema/SemType.h"
#include <sstream>

namespace Aura {

// ============================================================
// sync / spawn（plan §4.9）
// ============================================================

// ============================================================
// feature-14 U5：sync 块的 (乙1) 异常收集变量（change.md §3.5）
//
// 形态（feature-18 P2：承装**完整 Error 值** + 5 个 Ref 模式根句柄；GC 根化部分对齐 StmtTry.cpp 既有做法）：
//   aura_rt::Error _u5errN{};
//   aura_rt::GcRootHandle<decltype(_u5errN.kind)>    _u5errN_kind_h(_u5errN.kind);
//   aura_rt::GcRootHandle<decltype(_u5errN.message)> _u5errN_msg_h(_u5errN.message);
//   aura_rt::GcRootHandle<decltype(_u5errN.extra)>   _u5errN_extra_h(_u5errN.extra);
//   aura_rt::GcRootHandle<decltype(_u5errN.file)>    _u5errN_file_h(_u5errN.file);
//   aura_rt::GcRootHandle<decltype(_u5errN.stack)>   _u5errN_stack_h(_u5errN.stack);
//   bool _u5hasN = false;
//
// ⚠️ 5 个句柄是 **Ref** 模式（`GcRootHandle<T>(T& ref)`）：绑 `_u5errN` 的**成员地址**、**无 scope 参数**。
// ⚠️ 变量**声明在 sync 块首**（有界/无界分支的 _ctx/_sync 之后），但嵌套块尾的
//    驱动也要写入 → 必须跨块共享（铁律 3）。
// ⚠️ 嵌套 sync 用唯一序号后缀，避免内层同名变量遮蔽外层（铁律 3）。
// ⚠️ 延迟生成（本函数只占后缀、返回它；声明在块尾确认「有驱动」后才写入）：
//    否则「无 future 的 sync 块」会产出空声明污染产物、可能扰动单测断言。
// ============================================================
std::string CodeGenerator::genU5ErrDecls(std::ostream& cpp) {
    (void)cpp;
    // 只做「占后缀 + 记外层」——真正的声明由 emitU5ErrDecls 在确认本块有驱动后
    // 写到**块首位置**（调用方把块首写进缓冲流，见 genU5BufferedSyncBody）。
    std::string sfx = std::to_string(u5ErrCounter_++);
    u5ErrSuffix_ = sfx;
    return sfx;
}

// 块首声明（写在 sync 的 `{` 之后、`_ctx`/`_sync` 之后）。
// 引用点：① 嵌套块尾驱动（写入）；② 块尾重抛（读取）→ 必须块首可见。
void CodeGenerator::emitU5ErrDecls(std::ostream& cpp, const std::string& sfx) {
    if (sfx.empty()) return;
    // GC 根化：Error 内嵌 GcString*（runtime/types.h 的 kind/message）。协程帧不在
    // GC 保守扫描范围（registerStackRoots 全仓仅 task.cpp 一处 = 仅 main 帧）→
    // 跨驱动语句存活期间必须显式根化，否则驱动下一个 future 时若触发 compact，
    // 搬运走的 message（`_u5err<sfx>.message`）会让 `_u5err<sfx>` 悬垂 → 末尾 throw 出悬垂指针 → 用户 try-catch UAF。
    // 形态对齐既有做法 src/CodeGen/StmtTry.cpp（那里现为 `_tk_hold` + 5 个 Ref 句柄）。
    // kind 理论安全（intern_string 注册为全局根，永不回收），
    // 但保持一致根化，防将来 kind 来源变化。
    // ⚠️ 现在**就是**用 `Error _u5err<sfx>` 直存（非 optional ⇒ 成员地址恒有效 ⇒ 可安全绑 Ref 句柄）
    //    → 根化 `_u5err<sfx>` 的 5 个成员字段（kind/message/extra/file/stack）。
    // feature-18 P2：改用「承装完整 Error 值 + 5 个 Ref 模式根句柄」。
    //   为什么必须 Ref 模式：Value 模式只更新**句柄内部副本**，`_u5err` 变量自身的
    //   kind/message 字段不会被 compact 更新 ⇒ 块尾重抛会抛旧地址（既有隐患）。
    //   Ref 模式绑定 `_u5err` 的成员地址 ⇒ compact 原位改写 ✅
    writeLine(cpp, "aura_rt::Error _u5err" + sfx + "{};");
    // Ref 模式：`GcRootHandle<T>(T& ref)` —— **无 scope 参数**（Ref 恒为线程局部根，见 gc.h:50 注释与 gc.h:93 声明）
    //   ⚠️ 不可写成 `GcRootHandle<T>(val, GcRootScope::ThreadLocal)`（那是 **Value** 模式 ⇒ 只更新句柄内部副本）
    writeLine(cpp, "aura_rt::GcRootHandle<decltype(_u5err" + sfx + ".kind)> _u5err" + sfx + "_kind_h(_u5err" + sfx + ".kind);");
    writeLine(cpp, "aura_rt::GcRootHandle<decltype(_u5err" + sfx + ".message)> _u5err" + sfx + "_msg_h(_u5err" + sfx + ".message);");
    writeLine(cpp, "aura_rt::GcRootHandle<decltype(_u5err" + sfx + ".extra)> _u5err" + sfx + "_extra_h(_u5err" + sfx + ".extra);");
    writeLine(cpp, "aura_rt::GcRootHandle<decltype(_u5err" + sfx + ".file)> _u5err" + sfx + "_file_h(_u5err" + sfx + ".file);");
    writeLine(cpp, "aura_rt::GcRootHandle<decltype(_u5err" + sfx + ".stack)> _u5err" + sfx + "_stack_h(_u5err" + sfx + ".stack);");
    writeLine(cpp, "bool _u5has" + sfx + " = false;");
}

void CodeGenerator::genU5ErrRethrow(std::ostream& cpp, const std::string& suffix) {
    // feature-18 P2：重抛**完整** Error 值（含 extra/file/line/stack），
    //   原实现只抛 {kind, message} ⇒ 诊断信息在 sync 边界被截断。
    // 末尾重抛：首个 Error 以 Error 值形态抛回（Aura try-catch 的捕获类型是
    // catch (const aura_rt::Error&)，故**必须抛 Error**，不能抛 exception_ptr）。
    if (suffix.empty()) return;
    cpp << indentStr() << "if (_u5has" << suffix << ") throw _u5err" << suffix << ";\n";
}

void CodeGenerator::genSyncStmt(std::ostream& cpp, const SyncStmt& stmt,
                                 bool /*isCoroutine*/) {
    // sync thread 分支：多线程模式
    if (stmt.isThread) {
        genSyncThreadStmt(cpp, stmt);
        return;
    }

    // feature-14 P2：块体生成前置 insideSyncBlock_（隐式 future 的块内判据），
    // 两个分支都要覆盖，故在分支外统一 save/restore。
    bool savedInsideSyncBlock = insideSyncBlock_;
    insideSyncBlock_ = true;
    std::string savedU5 = u5ErrSuffix_;   // 嵌套 sync 的外层后缀

    cpp << indentStr() << "{\n";
    indentLevel_++;

    std::string sfx;
    std::string body;
    if (stmt.maxExpr) {
        // 有界版本：sync(max = N) { ... }
        //
        // feature-14 P2：_tasks 形参退役。bounded_sync 内嵌持有 SyncContext
        // （change.md §3.5b）：_sync 同时担当「限流门面」与「本块的同步域」，
        // spawn 经 requireSync() 命中它（域栈 push 在下方 _scope）。
        // ⚠️ 声明顺序：_sync → _scope（_scope 后声明 → 先析构 → 先 pop，
        //    再析构 _sync，满足 owner 生命周期不变量）。
        std::string maxN = genExpr(*stmt.maxExpr, false);
        writeLine(cpp, "aura_rt::bounded_sync _sync(" + maxN + ");");
        writeLine(cpp, "aura_rt::SyncContextScope _scope(_sync.ctx_);");
        sfx = genU5ErrDecls(cpp);
        bool hasDrive = false;
        body = genSyncBodyBuffered(*stmt.body, true, hasDrive);
        if (!hasDrive) sfx.clear();
        emitU5ErrDecls(cpp, sfx);   // 块首声明（仅在本块确有驱动时写出）
        cpp << body;
        writeLine(cpp, "aura_rt::gc_safepoint();");
        writeLine(cpp, "co_await _sync.wait_all();");
        genU5ErrRethrow(cpp, sfx);
    } else {
        // 无界版本（兼容旧语法）：直接用 SyncContext
        writeLine(cpp, "aura_rt::SyncContext _ctx;");
        writeLine(cpp, "aura_rt::SyncContextScope _scope(_ctx);");
        sfx = genU5ErrDecls(cpp);
        bool hasDrive = false;
        body = genSyncBodyBuffered(*stmt.body, true, hasDrive);
        if (!hasDrive) sfx.clear();
        emitU5ErrDecls(cpp, sfx);
        cpp << body;
        writeLine(cpp, "aura_rt::gc_safepoint();");
        writeLine(cpp, "co_await _ctx.wait_all();");
        genU5ErrRethrow(cpp, sfx);
    }

    indentLevel_--;
    cpp << indentStr() << "}\n";

    u5ErrSuffix_ = savedU5;
    insideSyncBlock_ = savedInsideSyncBlock;
}

// ============================================================
// feature-14 U5：把 sync 块体生成到临时流，据此决定块首是否写收集器声明。
//
// 为什么需要两段式：收集器变量必须**块首声明**（嵌套块尾驱动与块尾重抛都引用
// 它们，change.md §3.5 铁律 3），但又**只在「本块确有 future 驱动」时才生成**
// （否则空声明污染产物、可能扰动既有单测断言）。生成器是流式的，故先把块体写进
// ostringstream 探测，再按结果落盘。
// ============================================================
std::string CodeGenerator::genSyncBodyBuffered(const BlockStmt& body,
                                               bool isCoroutine, bool& hasDrive) {
    std::ostringstream buf;
    std::string outerSuffix = u5ErrSuffix_;
    genBlock(buf, body, isCoroutine, /*opensScope=*/true);
    // genBlock 内若遇到嵌套 sync，会改动 u5ErrSuffix_（那是内层块的上下文）；
    // 本层块尾重抛需要本层的后缀 → 恢复。
    u5ErrSuffix_ = outerSuffix;
    // 本块是否真的产生了驱动？genFutureDrive 在有驱动时会写 _u5has<sfx> 判断/描述。
    hasDrive = !(outerSuffix.empty()
                 || buf.str().find("_u5has" + outerSuffix) == std::string::npos);
    // ⚠️ 出参而非控制字符哨兵：旧的 "\x01NOU5\x01" 前缀哨兵与正文同流，
    //    漏剥离即污染产物（曾实测泄漏；且调用点 "erase(0,6)" 在「有驱动」分支
    //    误删正文首 6 字符）。出参在结构上不可能泄漏。
    return buf.str();
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
        std::set<std::string> builtins = {"io"};   // feature-14 P2: _tasks 退役
        std::vector<std::string> freeVars;
        bool ioUsed = false;
        // bug-24：body 引用 receiver（self/p）→ 不进 freeVars（this 别名），lambda 改 [this] 捕获
        bool needsThisCapture = false;
        for (auto& name : allRefs) {
            if (name == stmt.itemName) continue;    // 迭代变量已值捕获
            if (!currentReceiverName_.empty() && name == currentReceiverName_) {
                needsThisCapture = true;
                continue;
            }
            if (declared.count(name)) continue;      // body 内局部声明
            if (name == "io") { ioUsed = true; continue; }
            if (builtins.count(name)) continue;
            if (registeredTypes_.count(name)) continue;  // 函数名/类型名
            // 内置函数名（gc_force/str/range/Iterator 等）不捕获——调用点直转 runtime
            //（关联调研发现 (a)：此前仅排除类型名，gc_force 被当自由变量 → [.., gc_force]
            //  值捕获 → g++ 未声明）
            if (BuiltinRegistry::get().hasFunctionName(name)) continue;
            freeVars.push_back(name);
        }
        // #46 兜底（防御）：线程形态 body 引用 io（&io 引用捕获）但外层无 io → 干净报错
        if (ioUsed && !ioInScope_) {
            error(stmt, "sync for requires an 'io' variable in the enclosing scope; "
                        "add an 'io: Io' parameter to the enclosing function");
            return;
        }

        // spawn body：普通 lambda + _stx.submit（var + freeVars 值捕获 + io 引用捕获）
        // 注：外部变量在主线程作用域仍存活（如 let ch27 的 GcRootHandle），
        //     worker 线程执行期间对象不会被回收，与闭包形态线程版语义一致
        bool oldIoSync = ioSync_;
        bool oldCoroutine = currentFunctionIsCoroutine_;
        ioSync_ = true;                       // 强制 io 方法 _sync 版本
        currentFunctionIsCoroutine_ = false;  // 普通 lambda，禁止 co_await
        cpp << indentStr() << "_stx.submit([";
        bool firstCapture = true;
        // #56 §1.8：sync thread lambda 引用 receiver → init-capture 专属句柄 _sp_this
        //（Global 根跨线程逃逸；capture-init 源同 spawn——缺口 3 取外层句柄 .get() /
        // 入口句柄 .get()，不写裸 this）
        if (needsThisCapture) {
            cpp << "_sp_this = aura_rt::GcRootHandle<" << currentReceiverCppType_
                << "*>(" << receiverThisSourceExpr() << ", aura_rt::GcRootScope::Global)";
            firstCapture = false;
        }
        if (!firstCapture) cpp << ", ";
        cpp << var;
        // bug-72：GC 根 / 视图根自由变量 → Global 根 init-capture（ThreadLocal 句柄副本
        // 在提交线程注册、worker 线程析构 → 摘错 thread-local 根链表 → 扫根 GC UAF）
        for (auto& v : freeVars) cpp << ", " << crossThreadCaptureItem(v);
        if (ioUsed) cpp << ", &io";
        cpp << "]() mutable {\n";
        indentLevel_++;
        insideSpawn_ = true;
        // #56 §1.8：lambda 体生成期间隔离外层闭包/方法句柄映射；体后恢复
        std::string savedClosureHandle = currentClosureThisHandle_;
        std::string savedMethodHandle = currentMethodThisHandle_;
        if (needsThisCapture) currentClosureThisHandle_ = "_sp_this";
        if (stmt.body) genBlock(cpp, *stmt.body, false);   // 非协程！
        insideSpawn_ = false;
        currentClosureThisHandle_ = savedClosureHandle;
        currentMethodThisHandle_ = savedMethodHandle;
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
    //
    // feature-14 P2：块体生成前置 insideSyncBlock_（与 genSyncStmt 同款；
    // sync for 的协程版属 sync 系，隐式 future 的块内判据在此生效）。
    bool savedInsideSyncBlockFor = insideSyncBlock_;
    insideSyncBlock_ = true;
    std::string savedU5For = u5ErrSuffix_;   // feature-14 U5：外层后缀

    std::string u5sfxFor;
    std::string u5spawnBody;   // 缓冲 spawn 体的产物（用于探测是否有块尾驱动）
    if (hasMax) {
        std::string maxN = genExpr(*stmt.maxExpr, false);
        cpp << indentStr() << "{\n";
        indentLevel_++;
        writeLine(cpp, "aura_rt::bounded_sync _sync(" + maxN + ");");
        writeLine(cpp, "aura_rt::SyncContextScope _scope(_sync.ctx_);");
        u5sfxFor = genU5ErrDecls(cpp);   // feature-14 U5：(乙1) 收集器后缀
    } else {
        cpp << indentStr() << "{\n";
        indentLevel_++;
        writeLine(cpp, "aura_rt::SyncContext _ctx;");
        writeLine(cpp, "aura_rt::SyncContextScope _scope(_ctx);");
        u5sfxFor = genU5ErrDecls(cpp);   // feature-14 U5：(乙1) 收集器后缀
    }
    // ⚠️ 位置关键（feature-14 U5 三条约束）：
    //    ① 收集器声明（_u5err<sfx> + 5 个 Ref 句柄 + _u5has<sfx>）必须物理位于 `for` 头**之前**
    //       —— 否则落进循环体 `{` 内，sync 块尾的重抛看不见它们（实测踩过）。
    //    ② 「本块是否有驱动」只能在生成循环体**之后**才知道
    //       —— 驱动由 genFutureDrive 在 genBlock 弹帧时写入。
    //       故不能在 for 头之前下结论。
    //    ③ 循环体生成（=探测）必须在 spawn-lambda 上下文 live 之后跑
    //       —— 否则 body 里 self 映射成裸 `_this` → spawn lambda 内出现未捕获的
    //       `_this`（坏 C++，实测踩过）。
    //    解法：`for` 头 + 循环体**整体**先写入 forBuf（生成期间已满足 ③），
    //    生成完毕后再探测 → 按「声明 → forBuf」落盘（满足 ①②）。
    std::string u5forProbe;   // 循环体探测产物（含块尾驱动）；随 forBuf 一起落盘
    bool u5forDriven = false;
    // 2. Generate for loop over iterable
    //
    // ⚠️ feature-14 U5：for 头**不能立即落盘**——收集器声明必须物理写在
    //    for 头之前，而「本块是否有驱动」要等循环体生成完才知道。
    //    故 for 头与循环体都缓冲到 forBuf，探测后再按「声明 → forBuf」顺序 flush。
    //    缓冲只影响文本落盘时机，不改变缩进：indentStr() 读 indentLevel_，
    //    forBuf 与 cpp 共享同一缩进状态。
    std::ostringstream forBuf;
    bool isRangeCall = false;
    if (auto* call = dynamic_cast<const CallExpr*>(stmt.iterable.get())) {
        auto* id = dynamic_cast<const Identifier*>(call->callee.get());
        if (id && id->name == "range") {
            isRangeCall = true;
            if (call->args.size() == 1) {
                std::string end = genExpr(*call->args[0], true);
                forBuf << indentStr() << "for (auto " << var
                       << " : std::views::iota(0, " << end << ")) {\n";
            } else if (call->args.size() == 2) {
                std::string start = genExpr(*call->args[0], true);
                std::string end   = genExpr(*call->args[1], true);
                forBuf << indentStr() << "for (auto " << var
                       << " : std::views::iota(" << start << ", " << end << ")) {\n";
            }
        }
    }
    if (!isRangeCall) {
        std::string iter = genExpr(*stmt.iterable, true);
        forBuf << indentStr() << "for (auto " << var
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
    // #46：body 是否实际引用 io（穿透嵌套 spawn/语言闭包；builtins 排除前记录——
    // io 不进 freeVars）
    bool refsIo = allRefs.count("io") > 0;
    std::set<std::string> builtins = {"io"};   // feature-14 P2: _tasks 退役
    std::vector<std::string> freeVars;
    // bug-24：body 引用 receiver（self/p）→ 不进 freeVars（this 别名），lambda 改 [this] 捕获
    bool needsThisCapture = false;
    for (auto& name : allRefs) {
        if (name == stmt.itemName) continue;   // 迭代变量已有参数
        if (!currentReceiverName_.empty() && name == currentReceiverName_) {
            needsThisCapture = true;
            continue;
        }
        if (declared.count(name)) continue;     // body 内局部声明
        if (builtins.count(name)) continue;
        if (registeredTypes_.count(name)) continue;  // 函数名/类型名
        // 内置函数名不捕获（同线程版——关联调研发现 (a)）
        if (BuiltinRegistry::get().hasFunctionName(name)) continue;
        freeVars.push_back(name);
    }
    // #46 兜底：body 需 io（追加 io 参数）但外层无 io → 干净报错
    if (refsIo && !ioInScope_) {
        error(stmt, "sync for requires an 'io' variable in the enclosing scope; "
                    "add an 'io: Io' parameter to the enclosing function");
        return;
    }

    // 4. Generate spawn lambda：引用 receiver 时 init-capture 专属句柄 _sp_this
    //（缺口 2b 落实 §1.8，与线程版/协程 spawn 同款；capture-init 源不写裸 this——
    // 缺口 3 取外层句柄 .get() / 入口句柄 .get()）+ 显式参数（var + freeVars + io）
    //
    // feature-14 P2：_tasks 形参退役，改走 requireSync()->addTask(...)；
    // ⚠️ 收尾括号配对：多一层 '('，故下方 ":315 的 \", _tasks));\" 必须同步改为 \"));\"。
    // feature-14 U5：**本块驱动写在 spawn lambda 内部**（循环体尾部由 genFutureDrive
    // 生成），而收集器变量声明在 sync 块作用域（for 头之前）→ lambda 必须按引用捕获
    // 它们，否则驱动写 `_u5has0` 会报「not captured」（实测）。
    // 生命周期安全：lambda 是**立即调用**（尾部 `}(x, io));`）且返回的 task 由本块
    // `_ctx.wait_all()` 在块关闭前等完 → 引用在全部使用期内有效。
    // ⚠️ 捕获列表单独攒成 forCap 字符串（**不**直写 forBuf）：因为
    //    「是否真需要捕获收集器」取决于探测结论 u5forDriven，而该结论此时还没出。
    //    若直写 forBuf，探测为「无驱动」（→ 声明被省掉）时捕获名就指向未声明变量。
    std::string forCap;
    if (needsThisCapture) {
        forCap = "_sp_this = aura_rt::GcRootHandle<" + currentReceiverCppType_
               + "*>(" + receiverThisSourceExpr() + ", aura_rt::GcRootScope::Global)";
    }
    forBuf << indentStr() << "aura_rt::requireSync()->addTask([";
    forBuf << "](auto " << var;
    for (auto& v : freeVars) forBuf << ", auto " << safeName(v);
    if (refsIo) forBuf << ", aura_rt::Io& io";   // #46：body 实际引用 io 才追加
    // feature-14 P2：_tasks 形参已移除（本行原同时收尾 ")"，无需额外配对调整）
    forBuf << ") -> aura_rt::task<void> {\n";
    // 🔵 feature-18 P4b-1 B5 ③（change.md 裁定④/⑧、§8.2 B0b / §6.2 C8）：
    //   **sync-for** 的 `addTask(...)` 协程 lambda 头注入。
    //   ⚠️ `sync { }`（`genSyncStmt`）**没有 lambda**（它是普通作用域 + `co_await`）⇒
    //      本文件唯一的协程上下文 lambda 就在这里（与 change.md CP4 的 `:448` 锚点一致）。
    emitAnonFrame(forBuf, "sync", static_cast<uint32_t>(stmt.line), stmt);
    indentLevel_++;
    insideSpawn_ = true;
    // 缺口 1：task body 首语句物化 frame-local 句柄（协程 lambda init-capture 存在
    // g++ 暂存窗口 → 首段 GC 后不重定位；物化句柄先于任何 GC 注册、地址固定 → GC
    // 可重定位），body 内 self 恒映射物化句柄；体生成期间隔离外层闭包/方法句柄映射
    std::string savedClosureHandle = currentClosureThisHandle_;
    std::string savedMethodHandle = currentMethodThisHandle_;
    if (needsThisCapture) {
        currentClosureThisHandle_ = "_sp_this_f";
        writeLine(forBuf, "aura_rt::GcRootHandle<" + currentReceiverCppType_
                          + "*> _sp_this_f(_sp_this.get(), aura_rt::GcRootScope::Global);");
    }
    // #46：spawn lambda 签名含 io（body 实际引用 io）→ body 生成期间 io 参数在
    // lambda 作用域内可见，置 ioInScope_（save/restore）供嵌套 spawn 判定。
    bool savedIoInScope = ioInScope_;
    if (refsIo) ioInScope_ = true;
    // feature-14 U5：sync-for 的循环体**确实生成了 for 的 `{}`**（上方
    // `for (auto v : ...) {`）→ opensScope=true，块内声明的 future 在循环体尾部驱动。
    // ⚠️ 探测结论：`:197` 那个 genBlock 是**线程版**（isCoroutine=false，不走本段），
    //    协程版走这里 → 驱动落点正确（简报 T4 的「已知坑」已核实）。
    ioInScope_ = savedIoInScope;
    // ---- feature-14 U5: run the probe HERE, with the spawn-lambda context live
    //      (currentClosureThisHandle_ == "_sp_this_f", insideSpawn_ == true),
    //      so the probed body resolves `self` to `_sp_this_f` exactly like the
    //      real body would. Then restore everything.
    if (stmt.body) {
        u5forProbe = genSyncBodyBuffered(*stmt.body, true, u5forDriven);
    }
    // 循环体入缓冲（for 头已在 §2 写入 forBuf；此处接上体，使 forBuf 成为
    // 「for 头 + 循环体」的完整文本，再由下方统一决定是否在前面插声明）。
    forBuf << u5forProbe;
    insideSpawn_ = false;
    currentClosureThisHandle_ = savedClosureHandle;
    currentMethodThisHandle_ = savedMethodHandle;
    // ⚠️ feature-14 U5：u5ErrSuffix_ 必须在循环体生成后恢复为本层后缀再 flush。
    //    genSyncBodyBuffered 已自行恢复（它内部 genBlock 遇嵌套 sync 会改写），此处
    //    再显式恢复一次，保证「本层块尾重抛」与「声明」同后缀。
    u5ErrSuffix_ = u5sfxFor;
    // 循环体收尾（同样是 forBuf 的一部分）
    forBuf << indentStr() << "co_return;\n";
    indentLevel_--;
    forBuf << indentStr() << "}(" << var;
    // bug-72：自由变量经协程 lambda 形参（auto v）承载——形参是句柄副本，注册于创建线程、
    // 随协程帧在任意线程析构 → 实参改传 Global 根（auto 推导同型，体生成零改动）
    for (auto& v : freeVars) forBuf << ", " << crossThreadGlobalArg(v);
    if (refsIo) forBuf << ", io";   // #46：追加了 io 参数才传 io
    // feature-14 P2：_tasks 实参已移除；收尾 "))" 配对 addTask(
    forBuf << "));\n";

    // ==== feature-14 U5 死结的正解：探测完才决定落盘顺序 ====
    // 约束 1：收集器声明必须物理位于 `for` 头**之前**（否则落进循环体 `{` 内，
    //         sync 块尾的重抛看不见 → 无效 C++）。
    // 约束 2：「本块是否有驱动」只能在循环体生成完之后才知道（驱动由
    //         genFutureDrive 在 genBlock 弹帧时写入 forBuf）。
    // 约束 3：探测/生成必须在 spawn-lambda 上下文就绪之后跑（否则 body 里 self
    //         映射成裸 `_this` → spawn lambda 内出现未捕获的 `_this`）。
    // 三条约束的落点：for 头 + 循环体整体缓冲到 forBuf（上方完成，且生成时
    // spawn-lambda 上下文已是 live），此处探测 forBuf 再按「声明 → forBuf」落盘。
    //
    // 探测判据：genFutureDrive 在有收集器上下文时写 `_u5has<sfx>` 判断。
    // u5forDriven 由 genSyncBodyBuffered 出参给出，此处直接用（两边同门）。
    if (u5forDriven) {
        emitU5ErrDecls(cpp, u5sfxFor);   // ① 声明（在 for 头之前）
        // 收集器捕获：驱动写在 spawn lambda 内部、变量在块作用域 → 必须按引用捕获
        // （否则 not captured）。⚠️ 与声明**同门**：仅在真有驱动时才并入捕获列表，
        // 否则捕获名指向未声明的变量（坏 C++）。
        if (!forCap.empty()) forCap += ", ";
        // P2：只捕获承装值与标志（句柄绑定 `_u5err` 成员地址，随其一起可见 ⇒ 不需捕获句柄）
        forCap += "&_u5err" + u5sfxFor + ", &_u5has" + u5sfxFor;
    } else {
        u5sfxFor.clear();                // 无驱动 → 不声明，块尾亦不重抛（两边同门）
    }
    // ② for 头 + ③ 循环体（含块尾驱动）。spawn lambda 的捕获列表在此才拼上：
    //    forBuf 中已有 `...addTask([` 开头与随后的 `](auto ...)`，
    //    故把捕获列表插在 `addTask([` 之后、`]` 之前。
    {
        std::string fb = forBuf.str();
        const std::string kAdd = "aura_rt::requireSync()->addTask([";
        size_t pos = fb.find(kAdd);
        if (pos != std::string::npos) {
            fb.insert(pos + kAdd.size(), forCap);   // 捕获列表插到 `[` 之后
        }
        cpp << fb;
    }

    // 5. L2 safepoint：sync for 循环回边
    writeLine(cpp, "aura_rt::gc_safepoint();");
    indentLevel_--;
    cpp << indentStr() << "}\n";   // close for

    // 6. Close sync block
    writeLine(cpp, "aura_rt::gc_safepoint();");
    if (hasMax) {
        writeLine(cpp, "co_await _sync.wait_all();");
    } else {
        writeLine(cpp, "co_await _ctx.wait_all();");
    }
    // feature-14 U5：全部驱动之后、sync 块闭 `}` 之前重抛首个 Error（(乙1)）。
    // ⚠️ 必须与声明同门：仅在本块确有驱动（u5forDriven）时才生成重抛。
    //    否则会产出引用未声明的 _u5err<sfx>/_u5has<sfx> 的重抛（坏 C++）——
    //    实测：u5sfxFor 非空但探测为「无驱动」时，仅写出了重抛行、声明缺失。
    if (u5forDriven) genU5ErrRethrow(cpp, u5sfxFor);
    indentLevel_--;
    cpp << indentStr() << "}\n";   // close sync block

    u5ErrSuffix_ = savedU5For;
    insideSyncBlock_ = savedInsideSyncBlockFor;
}

} // namespace Aura
