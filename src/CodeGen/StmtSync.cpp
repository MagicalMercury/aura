#include "CodeGen.h"
#include "../Sema/BuiltinRegistry.h"
#include "../Sema/SemType.h"
#include <sstream>

namespace Aura {

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
        writeLine(cpp, "co_await aura_rt::when_all(_tasks);");   // #45：引用收参，不再 std::move
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
        for (auto& v : freeVars) cpp << ", " << safeName(v);
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
    // #46：body 是否实际引用 io（穿透嵌套 spawn/语言闭包；builtins 排除前记录——
    // io 不进 freeVars）
    bool refsIo = allRefs.count("io") > 0;
    std::set<std::string> builtins = {"io", "_tasks"};
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
    // 缺口 3 取外层句柄 .get() / 入口句柄 .get()）+ 显式参数（var + freeVars + io + _tasks）
    cpp << indentStr() << "_tasks.push_back([";
    if (needsThisCapture) {
        cpp << "_sp_this = aura_rt::GcRootHandle<" << currentReceiverCppType_
            << "*>(" << receiverThisSourceExpr() << ", aura_rt::GcRootScope::Global)";
        // capture 列表仅此一项（迭代变量/自由变量是 lambda 参数，非捕获），无需分隔逗号
    }
    cpp << "](auto " << var;
    for (auto& v : freeVars) cpp << ", auto " << safeName(v);
    if (refsIo) cpp << ", aura_rt::Io& io";   // #46：body 实际引用 io 才追加
    cpp << ", std::vector<aura_rt::task<void>>& _tasks"
        << ") -> aura_rt::task<void> {\n";
    indentLevel_++;
    insideSpawn_ = true;
    // 缺口 1：task body 首语句物化 frame-local 句柄（协程 lambda init-capture 存在
    // g++ 暂存窗口 → 首段 GC 后不重定位；物化句柄先于任何 GC 注册、地址固定 → GC
    // 可重定位），body 内 self 恒映射物化句柄；体生成期间隔离外层闭包/方法句柄映射
    std::string savedClosureHandle = currentClosureThisHandle_;
    std::string savedMethodHandle = currentMethodThisHandle_;
    if (needsThisCapture) {
        currentClosureThisHandle_ = "_sp_this_f";
        writeLine(cpp, "aura_rt::GcRootHandle<" + currentReceiverCppType_
                      + "*> _sp_this_f(_sp_this.get(), aura_rt::GcRootScope::Global);");
    }
    // #46：spawn lambda 签名含 io（body 实际引用 io）→ body 生成期间 io 参数在
    // lambda 作用域内可见，置 ioInScope_（save/restore）供嵌套 spawn 判定。
    bool savedIoInScope = ioInScope_;
    if (refsIo) ioInScope_ = true;
    if (stmt.body) genBlock(cpp, *stmt.body, true);
    ioInScope_ = savedIoInScope;
    insideSpawn_ = false;
    currentClosureThisHandle_ = savedClosureHandle;
    currentMethodThisHandle_ = savedMethodHandle;
    writeLine(cpp, "co_return;");
    indentLevel_--;
    cpp << indentStr() << "}(" << var;
    for (auto& v : freeVars) cpp << ", " << safeName(v);
    if (refsIo) cpp << ", io";   // #46：追加了 io 参数才传 io
    cpp << ", _tasks));\n";

    // 5. L2 safepoint：sync for 循环回边
    writeLine(cpp, "aura_rt::gc_safepoint();");
    indentLevel_--;
    cpp << indentStr() << "}\n";   // close for

    // 6. Close sync block
    writeLine(cpp, "aura_rt::gc_safepoint();");
    if (hasMax) {
        writeLine(cpp, "co_await _sync.wait_all();");
    } else {
        writeLine(cpp, "co_await aura_rt::when_all(_tasks);");   // #45：引用收参，不再 std::move
    }
    indentLevel_--;
    cpp << indentStr() << "}\n";   // close sync block
}

} // namespace Aura
