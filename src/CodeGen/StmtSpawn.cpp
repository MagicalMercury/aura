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
    // 检查用户是否已声明 io / _tasks
    bool hasIo = false;
    bool hasTasks = false;
    for (auto& p : stmt.params) {
        if (p.name == "io") hasIo = true;
        if (p.name == "_tasks") hasTasks = true;
    }

    // 整条 _tasks.push_back(...) 先写入缓冲 out：显式实参需经 genGcRootedArgs 包装
    // （bug-42）保护，须在实参求值前备好完整调用串（占位符 {i}）；同名自动绑定路径
    // 无堆临时值风险，直接透出缓冲。
    std::ostringstream out;
    // 生成 lambda 签名为显式参数
    out << indentStr() << "_tasks.push_back([](";
    for (size_t i = 0; i < stmt.params.size(); ++i) {
        if (i > 0) out << ", ";
        out << (stmt.params[i].type ? mapParamType(*stmt.params[i].type) : "auto")
            << " " << safeName(stmt.params[i].name);
    }
    // 自动追加 io 和 _tasks（如果用户未声明）
    if (!hasIo) out << ", aura_rt::Io& io";
    if (!hasTasks) out << ", std::vector<aura_rt::task<void>>& _tasks";
    out << ") -> aura_rt::task<void> {\n";
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
            if (s) genStmt(out, *s, true);
    }

    insideSpawn_ = false;
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
        for (size_t i = 0; i < stmt.params.size(); ++i) {
            if (i > 0) out << ", ";
            // 同名自动绑定：外层 GcRootHandle 变量 → 传 .get() 裸指针（guards 已析构，
            // gcRootVarNames_ 已恢复外层状态）
            std::string pname = safeName(stmt.params[i].name); // 同名自动绑定
            out << (gcRootVarNames_.count(pname) ? pname + ".get()" : pname);
        }
    }
    if (!hasIo) out << ", io";
    if (!hasTasks) out << ", _tasks";
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
        cpp << genGcRootedArgs(gcArgs, out.str(), true) << ";\n";
    } else {
        cpp << out.str() << ";\n";
    }
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
    bool explicitArgs = !stmt.args.empty();
    std::vector<std::string> captureItems;   // 非 io 参数捕获项（裸名 或 init-capture）
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
            captureItems.push_back(pname);
        }
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
    // 显式参数已在 Sema 中注册为只读符号，此处直接生成体
    for (auto& s : stmt.body) {
        if (s) genStmt(cpp, *s, false);  // 非协程！
    }
    insideSpawn_ = false;
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

} // namespace Aura
