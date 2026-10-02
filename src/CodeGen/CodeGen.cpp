#include "CodeGen.h"
#include "../Sema/SemAnalyzer.h"
#include "MetaCollect.h"   // feature-18 P3：MetaCollector / MetaMerger / MetaSymbolRec / MetaTypeRec
#include "MetaEmit.h"      // feature-18 P3：thunkSignature / emitTablesInline
#include <algorithm>
#include <filesystem>
#include <sstream>
#include <unordered_set>

namespace Aura {

// ============================================================
// CodeGenConfig
// ============================================================

void CodeGenConfig::setConfig(const SemAnalyzer& sema) {
    ioSync = sema.getIoSync();
}

// ============================================================
// 构造 & 主入口
// ============================================================

CodeGenerator::CodeGenerator(DiagnosticEngine& diag) : diag_(diag) {
}

// ============================================================
// feature-14 P2（§3.5 隐式 future）——future 变量登记 / 别名链解析
//
// 语义：sync 块内 `let a = workerA(io)` 不立即等待——a 绑定 lazy task，
// 真正的等待推迟到「a 被当值消费」的那一刻（消费点就地 co_await）。
// 见 change.md §3.5 与 §8.3 约束 1（块外保持现状）。
// ============================================================

void CodeGenerator::registerFutureVar(const std::string& name) {
    if (name.empty()) return;
    // 重新登记同一名字 → 抹掉旧的别名边（避免 t2 仍指向已换值的源头）
    clearFutureVar(name);
    futureVars_.insert(name);
}

void CodeGenerator::clearFutureVar(const std::string& name) {
    if (name.empty()) return;
    // `name = <新值>` 覆盖同名变量 → 该名字不再是 future（load-store 值语义，
    // 与 map 的 key-only set 语义一致；GC-2 下析构已完成的帧无副作用）。
    futureAliasOf_.erase(name);
    for (auto it = futureAliasOf_.begin(); it != futureAliasOf_.end(); ) {
        if (it->second == name) it = futureAliasOf_.erase(it);
        else ++it;
    }
}

// ============================================================
// feature-14 U5（change.md §3.5「U5 驱动语句生成」）
//
// 登记 / 块尾驱动生成。与 registerFutureVar 的关系见 CodeGen.h 的注释：
//   futureVars_         = 此刻活跃（消费点即 clear）
//   futureBlockStack_   = 本块声明过的全部（块尾驱动用）
// ============================================================

void CodeGenerator::registerFutureForDrive(const std::string& name) {
    if (name.empty()) return;
    if (futureBlockStack_.empty()) return;   // 防御：块外登记不产生驱动
    auto& names = futureBlockStack_.back().names;
    for (auto& n : names) if (n == name) return;   // 已登记（同名重声明）→ 幂等
    names.push_back(name);
}

void CodeGenerator::genFutureDrive(std::ostream& cpp,
                                   const FutureBlockFrame& frame) {
    if (frame.names.empty()) return;


    // (乙1) 异常语义（change.md §3.5「未消费 future 的异常语义」）：
    //   驱动**全部** future 到完成（强保证，不因首个异常中断），
    //   记录**首个** aura_rt::Error 值，其余仅 stderr 记录，末尾重抛。
    //
    // GC 根化：Error 内嵌 GcString*（runtime/types.h 的 kind/message），而协程帧
    //   不在 GC 保守扫描范围（registerStackRoots 全仓仅 task.cpp 一处 = 仅 main 帧）
    //   → 跨驱动语句存活期间必须显式根化，否则驱动下一个 future 时若触发 compact，
    //     搬运走的 message（`_u5err<sfx>.message`）会悬垂，末尾 throw 出悬垂指针 → 用户 try-catch UAF。
    //   形态对齐既有做法 src/CodeGen/StmtTry.cpp（现为 `_tk_hold` + 5 个 Ref 句柄）。
    //   kind 理论安全（intern_string 注册为全局根），
    //   但保持一致根化，防将来 kind 来源变化。
    //
    // ⚠️ 现在用 `Error _u5err<sfx>` 直存 + 5 个 Ref 句柄绑成员地址
    //    （非 optional ⇒ 成员地址恒有效 ⇒ 可安全绑 Ref 句柄）。
    // ⚠️ 无 sync 上下文（u5ErrSuffix_ 为空：理论上仅 sync 块内会声明 future，
    //    此处兜底为「裸驱动，不做异常包裹」，避免生成引用未声明变量的代码）。
    std::string sfx = u5ErrSuffix_;
    if (!sfx.empty()) {
        writeLine(cpp, "try { co_await " + frame.names[0] + "; } catch (const aura_rt::Error& _u5e" +
                       sfx + ") {");
        indentLevel_++;
        writeLine(cpp, "if (!_u5has" + sfx + ") { _u5has" + sfx + " = true; _u5err" + sfx +
                       " = _u5e" + sfx + "; }");
        writeLine(cpp, "else std::fprintf(stderr, \"[aura_rt] additional sync error\\n\");");
        indentLevel_--;
        writeLine(cpp, "}");
    } else {
        writeLine(cpp, "co_await " + frame.names[0] + ";");
    }
    for (size_t i = 1; i < frame.names.size(); ++i) {
        if (!sfx.empty()) {
            writeLine(cpp, "try { co_await " + frame.names[i] + "; } catch (const aura_rt::Error& _u5e" +
                           sfx + ") {");
            indentLevel_++;
            writeLine(cpp, "if (!_u5has" + sfx + ") { _u5has" + sfx + " = true; _u5err" + sfx +
                           " = _u5e" + sfx + "; }");
            writeLine(cpp, "else std::fprintf(stderr, \"[aura_rt] additional sync error\\n\");");
            indentLevel_--;
            writeLine(cpp, "}");
        } else {
            writeLine(cpp, "co_await " + frame.names[i] + ";");
        }
    }
}

std::string CodeGenerator::resolveFutureVar(const std::string& name) const {
    if (name.empty() || !futureVars_.count(name)) return std::string();
    // 沿别名链走到源头；带步数上限防环（正常登记不会成环，防御性）
    std::string cur = name;
    for (int guard = 0; guard < 64; ++guard) {
        auto it = futureAliasOf_.find(cur);
        if (it == futureAliasOf_.end()) break;
        if (!futureVars_.count(it->second)) break;   // 链尾已被注销
        if (it->second == cur) break;
        cur = it->second;
    }
    return cur;
}

CompileUnit CodeGenerator::generate(const Program& program,
                                     const std::string& moduleName,
                                     const std::vector<CodeGenImport>& imports,
                                     const std::string& nsName,
                                     const CodeGenConfig& config,
                                     const CrossModuleDefaults& crossDefaults,
                                     const CrossModuleParamSemTypes& crossParamSemTypes,
                                     const std::string& sourcePath,
                                     MetadataSink metaCollector) {
    sourceFile_ = sourcePath;     // feature-18 P2：Error.file 的真实来源
    // ---- feature-18 P3 §3.5(a)：与 sourceFile_ 同处赋值 ----
    nsName_          = nsName;    // §3.4(c)/V2：缓存的命名空间（genDecl 分派内没有 `unit`，只能用成员）
    metaCollector_   = metaCollector;  // per-module 收集目标（线程私有）；NullMetadata = 本趟不收集 ⇒ 不注入帧/行号（§11.11 门控）
    metaMode_        = config.metaMode;  // 🔴 O5：CodeGenerator 逐字段拷贝配置，**没有** config_ 成员
    ioSync_ = config.ioSync;
    crossDefaults_ = crossDefaults;   // C5.4: 跨模块函数默认参数表
    crossModuleParamSemTypes_ = crossParamSemTypes;  // bug-06: 跨模块函数形参 SemType 表
    CompileUnit unit;
    unit.moduleName = moduleName;
    unit.nsName     = nsName;

    std::ostringstream header, impl;
    headerStream_ = &header;
    implStream_   = &impl;

    // ---- feature-18 P3 §3.5(b)：External（多文件）模式注入 aura.meta.h ----
    // 🔴 O5：用成员 `metaMode_`（`config_` **不存在** —— CodeGenerator 逐字段拷贝配置）。
    // ⚠️ 置于 `aura_rt.h` **之前**：§4.1-③ 断言「External ⇒ unit.header 以 aura.meta.h 开头」。
    // ⚠️ **P3 原记「单文件 unit.header 逐字不变」的红线已于 P4a 批 2 作废**（见下方 P4a 段）：
    //    A7 的帧注入对**泛型函数**（体入 header，`DeclFun.cpp:190 needsHeader = !tparams.empty()`）
    //    必然改写单文件 header ⇒ 该「逐字不变」前提**本身已被 P4a 打破**，与 include 无关。
    //    ⇒ 现有效红线为：`test_codegen_meta.cpp` 的 **② InlineModeHeaderUnchanged**
    //      （「注入 collector」vs「不注入」两侧 header **逐字节一致**）——
    //      新增的 include 按 `metaMode_` 分支加 ⇒ **两侧同加 ⇒ 该断言仍成立**（批 2 已核）。
    if (metaMode_ == CodeGenConfig::MetaMode::External) {
        header << "#include \"aura.meta.h\"\n";
    }

    // 公共头
    header << "#include \"aura_rt.h\"\n";

    // ---- 🔴 feature-18 P4a 批 2（A7 的连带必需项，实施期实测发现）----
    // Inline（单文件）模式必须在此同时注入 `#include "meta.h"`：
    //   泛型函数/方法（模板 / auto 返回 —— `genFunDecl` 的 needsHeader 分支、`genMethodDecl`
    //   的 `!tparams.empty() || retType=="auto"`）的**函数体写在 header 段**，而 Inline 的
    //   `meta.h`（含 `aura_rt::meta::symbolIndexAt`）原只在 **metaImpl 段**（main.cpp 中
    //   header → metaImpl → impl 的次序）注入 ⇒ 帧注入行出现在 header 段时 `aura_rt::meta`
    //   尚未声明 ⇒ 生成码 `error: 'aura_rt::meta' has not been declared`（实测：
    //   `example/test.aura` 5 处，全在 header 段、全为本批注入行）。
    //   ⇒ 与 External 模式（header 首行已 `#include "aura.meta.h"`）对齐，此处补同效 include。
    // ⚠️ 该行**推翻了**上方 `:160` 注释所记的「单文件 unit.header 逐字不变」前提（该前提未
    //   预见「体入 header 的泛型函数」这一形态 —— 属 change.md §3.2 / 简报的前提缺口，
    //   已在批 2 回报 §1/§6 报红）。meta.h 只含 extern 声明 + inline 函数 ⇒ 无 multiple definition。
    if (metaMode_ != CodeGenConfig::MetaMode::External) {
        header << "#include \"meta.h\"\n";
    }

    // 生成 import 对应的 #include（头文件中包含依赖模块的 .h）
     for (auto& imp : imports) {
         if (imp.isBuiltin) {
             // 内置模块已通过 aura_rt.h 引入，此处生成注释说明
             header << "// using builtin: " << imp.path << "\n";
         } else {
            // 用户模块：相对路径 #include
            std::string depStem = std::filesystem::path(imp.path).stem().string();
            header << "#include \"" << depStem << ".aura.h\"\n";
        }
    }
    header << "\n";

    // 生成翻译单元级别的命名空间别名
     // 有 alias 时只生成别名，屏蔽原名
     for (auto& imp : imports) {
         bool hasAlias = !imp.alias.empty() && imp.alias != imp.modName;
         if (!hasAlias) {
             // 无别名：用模块名
             if (imp.isBuiltin) {
                 impl << "namespace " << imp.modName
                      << " = aura_rt::" << imp.path << ";\n";
             } else {
                 impl << "namespace " << imp.modName << " = " << imp.nsName << ";\n";
             }
             importNsNames_.insert(imp.modName);
         }
         if (!imp.alias.empty()) {
             // 有别名：生成别名（别名 ≠ 原名时）
             impl << "namespace " << imp.alias
                  << " = " << (imp.isBuiltin ? std::string("aura_rt::") + imp.path : imp.nsName) << ";\n";
             importNsNames_.insert(imp.alias);
         }
     }
     if (!imports.empty()) impl << "\n";

    // 打开命名空间（若有）— header 和 impl 都需要
    if (!nsName.empty()) {
        header << "namespace " << nsName << " {\n\n";
        impl << "namespace " << nsName << " {\n\n";
    }

    // 第一遍：注册所有类型名和接口名
    for (auto& d : program.decls) {
        if (!d) continue;
        if (auto* t = dynamic_cast<const TypeDecl*>(d.get()))
            registerTypeName(t->name, true);
        if (auto* i = dynamic_cast<const InterfaceDecl*>(d.get()))
            interfaceNames_.insert(i->name);
        if (auto* f = dynamic_cast<const FunDecl*>(d.get())) {
            registerTypeName(f->name, false);
            // feature-06（阶段 B）：具名函数直呼快路径判别集合（B3a）
            declaredFunNames_.insert(f->name == "main" ? "aura_main" : f->name);
        }
        if (auto* m = dynamic_cast<const MethodDecl*>(d.get())) {
            if (m->isConstructor)
                registerTypeName(m->receiverType, true);
            // 适配器默认方法转发判定数据：record 非构造方法名集合
            if (!m->isConstructor && !m->receiverType.empty())
                recordMethods_[m->receiverType].insert(m->name);
            // 显式 impl 组合收集：record 方法声明 → (receiverType, 接口名 → 类型实参)
            // 接口实现必须显式 impl（无结构匹配）；只收集非泛型 record
            // （receiverTypeArgs 为空）——泛型 record 的 C++ 类型名是 "Stack<T>"（含模板参数），
            // 适配器类型名无法对应。v1 泛型 record 接接口 → Sema 报错
            if (!m->implInterface.empty() && m->receiverTypeArgs.empty()) {
                std::vector<std::string> argCpp;
                for (auto& ta : m->implTypeArgs)
                    argCpp.push_back(ta ? mapType(*ta) : "???");
                interfaceImplementations_[m->receiverType][m->implInterface] = std::move(argCpp);
            }
        }
    }

    // 适配器生成需遍历的接口集合 = 用户接口 + 内置接口（interfaces.aurai：Stringer/Comparable/Iterator）
    allIfaces_.clear();
    for (auto& d : program.decls) {
        if (auto* i = dynamic_cast<const InterfaceDecl*>(d.get()))
            allIfaces_.push_back(i);
    }
    for (auto& i : BuiltinRegistry::get().auraiInterfaces())
        allIfaces_.push_back(i.get());

    // 第二遍：协程判定（固定点迭代，bug-02）
    // decideCoro 单调：直接挂起点（io.xxx 异步 / channel send-receive）或调用已标协程者
    // 即判 Coroutine，且 coroutineFunctions_ 只增不减 → 反复扫描直至一轮无新增即收敛。
    // 这使判定与声明顺序无关：外层函数/方法调用「后置声明」的协程函数/方法时，后续轮会
    // 补标（原单遍 for 按声明顺序扫描，外层先扫时后置者不在集合 → 判 Plain → 调用点不
    // co_await → task 立即析构 → 协程体静默不执行）。已标协程者判定单调、重扫结果不变，
    // 跳过仅优化性能；kMax 截断理论不可达（有限集合单调必收敛），仅作防死循环保险。
    constexpr int kMaxCoroPasses = 16;
    bool changed = true;
    for (int pass = 0; pass < kMaxCoroPasses && changed; ++pass) {
        changed = false;
        for (auto& d : program.decls) {
            if (!d) continue;
            if (auto* f = dynamic_cast<const FunDecl*>(d.get())) {
                if (coroutineFunctions_.count(f->name)) continue;
                if (decideCoro(*f) == CoroDecision::Coroutine) {
                    coroutineFunctions_.insert(f->name);
                    changed = true;
                }
            }
            if (auto* m = dynamic_cast<const MethodDecl*>(d.get())) {
                std::string key = m->receiverType + "." + m->name;
                if (coroutineFunctions_.count(key)) continue;
                if (decideCoro(*m) == CoroDecision::Coroutine) {
                    // 方法键 = "ReceiverType.methodName"（与 methodDefaultArgs_/methodParamCppTypes_
                    // 同键格式）：避免同名方法跨不同 receiver 互相污染协程判定。
                    coroutineFunctions_.insert(key);
                    changed = true;
                }
            }
        }
        // feature-12 批次 3 · 5.1b（2026-09-16）：接口默认方法协程登记。
        // 接口默认方法在视图 struct 内生成成员函数，体内若含 sync/spawn
        // （生成产物含 co_await）→ 必须协程化（返回 aura_rt::task<R>），
        // 否则非协程函数内 co_await → 坏 C++。
        // 键格式与调用点（ExprMethodCall 的 recvTypeKey 推导）、声明侧
        //（methodDefaultArgs_/methodParamCppTypes_ 的 "接口名.methodName"）同源。
        // 登记后才能向外传染：实现方（record impl）体内调用该视图方法时
        // 本身也被标协程 → 调用点才会加 co_await 解包。
        for (auto* iface : allIfaces_) {
            if (!iface) continue;
            for (auto& im : iface->methods) {
                if (!im.defaultBody) continue;
                std::string key = iface->name + "." + im.name;
                if (coroutineFunctions_.count(key)) continue;
                if (decideCoro(*im.defaultBody)) {
                    coroutineFunctions_.insert(key);
                    changed = true;
                }
            }
        }
    }

    // ============================================================
    // feature-18 收窄批 A1（2026-10-02）：填充「可抛性索引」`fnThrows_`
    // ------------------------------------------------------------
    // 与 coroutineFunctions_ 同为「名字 → 属性」范式，但**无需固定点迭代**：
    //   `throws` 是声明自带的静态属性，不传染（调用者是否 throws 由 Sema 检查，
    //   CodeGen 只关心「被调者是否可抛」）。
    // 键格式必须与 coroutineFunctions_ / frameSeqOf_ 严格同源，否则查不到 ⇒ 退化为
    //   「未知 ⇒ 注入」—— 不报错但白付性能（收窄失效）。
    // ⚠️ 同名多载取**并集**（任一可抛 ⇒ 可抛）：保守，宁多注入不漏注入。
    fnThrows_.clear();
    for (auto& d : program.decls) {
        if (!d) continue;
        if (auto* f = dynamic_cast<const FunDecl*>(d.get())) {
            bool& slot = fnThrows_[f->name];
            slot = slot || f->throws;
        } else if (auto* m = dynamic_cast<const MethodDecl*>(d.get())) {
            if (m->isConstructor) continue;   // 与 genDecl 的符号收集面一致（构造器不入）
            bool& slot = fnThrows_[m->receiverType + "." + m->name];
            slot = slot || m->throws;
        }
    }

    // 第三遍：生成代码
    // Phase 3-⑤：pendingMethods_ 收集（方法签名 mapType 映射）之前，先对所有函数/
    // 联合/命名类型别名执行 genTypeDecl（DeclGen.cpp:37）的 registerTypeName(name,
    // false) 覆盖，消除「别名在收集时为堆、在 genTypeDecl A 遍后才覆盖为非堆」的
    // 时序窗口——否则声明侧（genRecordStruct 内嵌方法签名）元素多 '*' 而定义侧
    // （genMethodDecl 第三遍 B）无 '*' → g++ no declaration matches。record 类型
    // 恒堆（genTypeDecl RecordType 分支不覆盖），跳过。
    for (auto& d : program.decls) {
        if (!d) continue;
        if (auto* t = dynamic_cast<const TypeDecl*>(d.get())) {
            if (!t->type) continue;
            if (!dynamic_cast<const RecordType*>(t->type.get()))
                registerTypeName(t->name, false);
        }
    }
    // Phase 3-⑤b：pendingMethods_ 收集前预填充 typeAliasTemplateParams_（与
    // genTypeDecl 相同的模板参数扫描）——否则 struct 内嵌方法声明的 isFuncAliasRet
    // 判定（返回泛型函数类型别名 → auto，M1）在收集时因 typeAliasTemplateParams_
    // 尚未填充（第三遍 A 的 genTypeDecl 才填充）而失效。
    for (auto& d : program.decls) {
        if (!d) continue;
        auto* t = dynamic_cast<const TypeDecl*>(d.get());
        if (!t || !t->type) continue;
        std::vector<std::string> tp = t->typeParams;
        if (tp.empty()) {
            if (auto* rec = dynamic_cast<const RecordType*>(t->type.get())) {
                for (auto& f : rec->fields)
                    if (f.type && dynamic_cast<const GenericTypeRef*>(f.type.get()))
                        tp.push_back(dynamic_cast<const GenericTypeRef*>(f.type.get())->name);
            } else {
                std::set<std::string> tpSet;
                collectTParams(*t->type, tpSet);
                tp.assign(tpSet.begin(), tpSet.end());
            }
        }
        if (!tp.empty())
            typeAliasTemplateParams_[t->name] = tp;
    }
    // 先收集方法信息（供 genRecordStruct 嵌入声明）
    pendingMethods_.clear();
    for (auto& d : program.decls) {
        if (auto* m = dynamic_cast<const MethodDecl*>(d.get())) {
            if (!m->isConstructor) {
                PendingMethod pm;
                pm.receiverType  = m->receiverType;
                pm.methodName    = m->name;
                pm.returnTypeStr = m->returnType ? mapType(*m->returnType) : "void";
                // M1/M5：返回泛型函数类型别名（Mapper<A,U>）或直接写泛型函数类型
                // （fun(U,T)->U，含"未在方法模板参数中的闭包自身泛型"）→ struct 内声明
                // 写 auto（闭包自身泛型 U/T 未在方法模板参数中，显式写
                // std::function<U(U,T)> 会 'U' was not declared；auto 由方法定义体推导，
                // 与 genMethodDecl 一致）。返回泛型均在方法模板参数中的（如
                // Box<T>::identity() -> fun(T)->T）保持显式返回类型。
                if (m->returnType && isFuncAliasRet(m->returnType.get()))
                    pm.returnTypeStr = "auto";
                else if (m->returnType
                         && dynamic_cast<const FunctionType*>(m->returnType.get())) {
                    auto mtp = collectMethodTParams(*m);
                    std::set<std::string> retGen;
                    collectTParams(*m->returnType, retGen);
                    for (auto& g : retGen)
                        if (std::find(mtp.begin(), mtp.end(), g) == mtp.end()) {
                            pm.returnTypeStr = "auto";
                            break;
                        }
                }
                // 方法 NoneType 返回 → void（无条件，与定义侧 genMethodDecl 统一；否则
                // struct 内声明 `aura_rt::NoneType zero();` 与类外定义 `void Point::zero()`
                // 返回类型不匹配 → C++ 编译错误）。task<void> 有 return_void、
                // task<NoneType> 没有 → 协程分支直接包已映射的 void。
                if (pm.returnTypeStr == "aura_rt::NoneType")
                    pm.returnTypeStr = "void";
                // 方法协程化：体内含异步操作（sync/spawn/io/channel）的方法 → struct 内
                // 声明包 aura_rt::task<ret>（与定义侧 genMethodDecl 对称）。auto 返回
                // （泛型闭包）保持 auto（无法表达 task<auto>，声明/定义两侧一致）。
                if (coroutineFunctions_.count(m->receiverType + "." + m->name)
                    && pm.returnTypeStr != "auto") {
                    pm.returnTypeStr = "aura_rt::task<" + pm.returnTypeStr + ">";
                }
                for (auto& p : m->params) {
                    pm.paramTypes.push_back(p.type ? mapType(*p.type) : "auto");
                    pm.paramNames.push_back(p.name);
                }
                // bug-07：struct 内方法声明的模板前缀参数 = collectMethodTParams 中
                // 非 receiver 泛型部分（receiver 泛型已在 struct 模板作用域内）。方法
                // 自身裸泛型（apply(f: fun(U)->U) 的 U）须声明为类内函数模板。
                {
                    std::vector<std::string> mtp = collectMethodTParams(*m);
                    std::set<std::string> recvGen(m->receiverTypeArgs.begin(),
                                                  m->receiverTypeArgs.end());
                    for (auto& g : mtp)
                        if (!recvGen.count(g)) pm.templateParams.push_back(g);
                }
                pendingMethods_.push_back(std::move(pm));
            }
        }
    }

    // 内置接口（interfaces.aurai：Stringer/Comparable/Iterator）不在 program.decls 中。
    // 此遍历仍必须在第三遍 A（函数前向声明）之前：genInterfaceDecl 的 A 遍会注册
    // 接口方法形参 C++ 类型（methodParamCppTypes_ / methodInterfaceParams_），调用点
    // 可能先于接口声明生成。
    // 注（bug-85 方案 B3）：自 Iterator/Stringer/Comparable 的 C++ 视图定义移入
    // runtime 公共头（via #include "aura_rt.h"）后，本处对这些内置接口不再产出
    // struct 文本（见 genInterfaceDecl 的内置接口 return），产物中亦不再有
    // 「先声明否则模板实参未声明」问题（P1-2 由 runtime 头承接）。
    for (auto& i : BuiltinRegistry::get().auraiInterfaces())
        genInterfaceDecl(header, *i);

    // feature-12 批次 1（方案 F）：泛型闭包 struct 的 header 插入点——
    // 这里已经过“类型/接口声明”且命名空间已打开、函数声明/定义
    // 尚未写入。闭包 struct 必须落在【函数定义之前】（否则
    // 函数内 return [&]() -> __GcUClosure_N* ... 引用未声明类型）。
    const size_t closureSplicePos = header.tellp() >= 0
        ? static_cast<size_t>(header.tellp()) : 0;

    // 第三遍 A：先生成所有声明（避免前向引用问题）
    for (auto& d : program.decls) {
        if (!d) continue;
        if (diag_.errorCount() > 10) break;  // 错误过多，停止生成
        genDecl(header, impl, *d, unit, true);
    }

    // 接口适配器收尾（第三遍 A 之后）：为所有 record × 接口组合生成适配器——
    // 此时所有 record struct 已完整定义，适配器内联方法体可安全解引用 record 方法
    for (auto* i : allIfaces_) {
        for (auto& [rec, ifaces] : interfaceImplementations_) {
            if (!ifaces.count(i->name)) continue;
            std::string key = rec + i->name;
            if (ifaceAdapterCache_.count(key)) continue;
            ifaceAdapterCache_.insert(key);
            genIfaceAdapter(header, rec, *i);
        }
    }

    // 第三遍 B：再生成所有定义
    if (diag_.errorCount() <= 10) {
        for (auto& d : program.decls) {
            if (!d) continue;
            if (diag_.errorCount() > 10) break;
            genDecl(header, impl, *d, unit, false);
        }
    }

    // ============================================================
    // feature-18 P3 §3.5(d)：`materialize` thunk 生成
    //  ⚠️ 必须在 impl 段的 namespace 块内（B13：`:193` 开 → 下方 `}` 闭），且落在其引用的
    //     record/fun 的**声明之后**（V3 位置试验：位置 P1 rc=1；本 splice 点在 B 遍定义之后 ⇒ P2/P3 形态）。
    //  ⚠️ 严格按 V3 实测的三行模板 + §9-V3-a 的 save/restore 权威清单。
    // ============================================================
    if (metaCollector_) {
        // 🔴 O41-(g)（批 3）：**实际生成的 thunk 名字集合** —— 循环内每发射一个 thunk 就登记，
        //   循环之后用它剪枝 `symbols_`（见下方 `pruneUnmaterialized` 调用点）。
        //   ⚠️ 只在**确实写出签名行之后**才登记 ⇒ 与产物严格一一对应（由产物驱动，不可能分叉）。
        std::set<std::string> emittedThunkNames;
        // 局部：声明侧 TypeExpr → SemType（供签名构造）。
        // ⚠️ 实测差异（见回报 §6）：Sema **不**把**形参** TypeExpr 的解析结果写回
        //   `TypeExpr::inferredType`（全仓仅**返回类型**写回：`BodyChecker.cpp:144`/`:225`；
        //   形参走 `DeclChecker.cpp:423` 的 `resolvedType → SymParam`，与 AST 节点无关）
        //   ⇒ 形参在 CodeGen 侧**没有现成 SemType 可取**。此处 inferredType 优先；缺失时
        //   只兜底 NamedType 的基础类型名；仍不可得 ⇒ nullptr（该符号**不生成 thunk**）。
        auto semTypeOfDeclType = [](const TypeExpr* t) -> std::unique_ptr<SemType> {
            if (!t) return nullptr;
            if (t->inferredType) return t->inferredType->clone();
            if (auto* nt = dynamic_cast<const NamedType*>(t)) {
                if (nt->name == "int")   return std::make_unique<PrimSemType>(PrimSemType::Int);
                if (nt->name == "float") return std::make_unique<PrimSemType>(PrimSemType::Float);
                if (nt->name == "bool")  return std::make_unique<PrimSemType>(PrimSemType::Bool);
            }
            return nullptr;
        };

        // 🔴 O1：遍历**本地 collector**（线程私有）—— 不遍历共享容器、不依赖全局 index。
        for (const MetaSymbolRec& rec : metaCollector_->symbols()) {   // 🔴 O6：引用（非指针）
            // ---- 反查该记录对应的 AST 声明（取 C++ 侧信息：函数名 / 接收者类型 / 方法名）----
            const FunDecl*    fd = nullptr;
            const MethodDecl* md = nullptr;
            for (auto& d : program.decls) {
                if (!d) continue;
                if (rec.kind == 0) {
                    if (auto* ff = dynamic_cast<const FunDecl*>(d.get()))
                        if (ff->name == rec.name) { fd = ff; break; }
                } else if (rec.kind == 1) {
                    if (auto* mm = dynamic_cast<const MethodDecl*>(d.get()))
                        if (!mm->isConstructor
                            && (mm->receiverType + "." + mm->name) == rec.name) { md = mm; break; }
                }
            }
            if (!fd && !md) continue;
            if (md && !md->receiverTypeArgs.empty()) continue;   // 泛型接收者的 C++ 类型含裸 T ⇒ 跳过

            // ---- 构造签名（两个生成器共用的 `spec.sig`）----
            auto sig = std::make_unique<FuncSemType>();
            bool sigOk = true;
            const std::vector<Param>& params = fd ? fd->params : md->params;
            const TypeExpr* retT = fd ? fd->returnType.get() : md->returnType.get();
            for (const Param& p : params) {
                auto st = semTypeOfDeclType(p.type.get());
                if (!st) { sigOk = false; break; }
                sig->paramTypes.push_back(std::move(st));
            }
            if (sigOk && retT) {
                auto rt = semTypeOfDeclType(retT);
                if (!rt) sigOk = false; else sig->returnType = std::move(rt);
            }
            if (!sigOk) continue;   // 签名不可静态构造 ⇒ 跳过（详见回报 §6）

            // ---- ① save（§9-V3-a 权威清单）----
            //  · **必须** save/restore：`lastClosureCppBase_` / `lastClosureCppBaseIsCoro_`
            //    （`ExprClosureArgs.cpp:371-372` **无条件覆写、从不还原**；消费点 `StmtLet.cpp:698`
            //     缺 `initIsNewClosure` 守卫 ⇒ 会污染后续 `let f = <fun 值>` 的类型推导）。
            //  · ⚠️⚠️ **绝不能** restore：`erasedCounter_`（V3 实测**非幂等**：两次调用产物文本不同）
            //    —— restore 会让同一 thunk 体内两次调用产出**同名函数局部类 ⇒ 重定义**。必须保持单调。
            const std::string savedCppBase       = lastClosureCppBase_;
            const bool        savedCppBaseIsCoro = lastClosureCppBaseIsCoro_;

            // ⚠️ 实测差异（见回报 §6）：change.md §3.5(d) 的代码块写 `cpp <<` —— 但**本函数
            //   （`generate()`）里 impl 流叫 `impl`**（`std::ostringstream header, impl;`），
            //   `cpp` 是 `genDecl`/各 gen* 的形参名 ⇒ 照抄**编不过**。此处一律写 `impl`。
            impl << MetaEmit::thunkSignature(unit.nsName, rec.thunkName)   // 🔴 O17 唯一拼法；O40-(b) 只出签名行
                 << " {\n";                                                // splice 点已在 namespace 块内
            // 🔴 O41-(g)：**以产物为准**登记 —— 签名行已写出 ⇒ 本符号的 thunk 真实存在
            //   （后面所有 `continue` 都在此点之前 ⇒ 集合与产物一一对应）。
            emittedThunkNames.insert(rec.thunkName);
            indentLevel_++;
            std::string prod;   // ② 生成器产物（IIFE）
            if (md) {
                // ① 接收者绑定（仅方法形态；契约 §0.4-③：非空 ⇒ 接收者在 recvOrNull[0]）
                const std::string recvCpp = mapNamedType(md->receiverType);
                impl << indentStr() << recvCpp << " __mat_recv = static_cast<" << recvCpp
                     << ">(static_cast<aura_rt::GcObject*>(recv->v.p));\n";
                ErasedWrapSpec sp;
                sp.kind        = 3;                  // 方法值 → cap_recv 槽派生
                sp.sig         = sig.get();
                sp.expr        = "__mat_recv";
                sp.recvCppType = recvCpp;
                sp.member      = md->name;
                prod = genCallableObjValueWrap(sp);  // ExprClosureArgs.cpp:281
            } else {
                Identifier id;                        // 合成「函数名引用」节点（genFnRefCallableObjValue 的入参形态）
                id.name = fd->name;
                id.inferredType = sig.get();
                prod = genFnRefCallableObjValue(id);  // ExprClosureArgs.cpp:493
            }
            if (!prod.empty()) {
                impl << indentStr() << "auto* __o = " << prod << ";\n";
                // ③ ⚠️ **必需**：产物是 `CallableObj<sig>*`，**不是** `CallableErased*`（V3-②，
                //    裸 `return` ⇒ `cannot convert`）⇒ 必须再经 `genErasedWrap(kind=0)` 复合。
                ErasedWrapSpec sp0;
                sp0.kind = 0;
                sp0.sig  = sig.get();
                sp0.expr = "__o";
                impl << indentStr() << "return " << genErasedWrap(sp0) << ";\n";
            } else {
                // 生成器判该符号不可静态物化（签名含未绑定泛型等）⇒ 保底返回空指针（不产出坏 C++）。
                impl << indentStr() << "(void)recv;\n";
                impl << indentStr() << "return nullptr;\n";
            }
            indentLevel_--;
            impl << "}\n";

            // ---- ② restore（**不含** `erasedCounter_` —— 见上方禁令）----
            lastClosureCppBase_       = savedCppBase;
            lastClosureCppBaseIsCoro_ = savedCppBaseIsCoro;
        }

        // ============================================================
        // 🔴🔴 O41-(g) 修复（批 3，第一优先）：**后置剪枝**
        //
        // 问题：上面循环对「签名不可静态构造」的符号是 `continue`（**不发 thunk**），
        //   但 A 遍 `genDecl` **已经**把表项登记进 `symbols_`（`materialize = &_aura_mat_x_y`）
        //   ⇒ 表项**悬空** ⇒ 链接期 `undefined reference`。
        // 裁定：**由实际产物驱动**（不是「两处同一判定」）—— 用本循环**实际生成的** thunk
        //   名字集合过滤 `symbols_`：没有 thunk 的符号从表里删掉。
        //   ⇒ **不变量：`kSymbolTable` 每条记录的 `materialize` 都指向真实 thunk 或 `nullptr`（降级占位）。**
        //     ⚠️ feature-18 P4a 批 1（A1b-③，change.md §8.1 A1b / R3）：原文只写「都指向本 TU 内真实存在的
        //        thunk」—— **已不成立**：A1 的「降级不删」会让未发射 thunk 的帧记录**留在表里**并把
        //        `thunkName` 清空（如 `main`，`isFrame == true` 恒在收集面）⇒ 该记录的物化列必须是
        //        `nullptr`（由 `MetaEmit.cpp` 的 A1b 渲染，见 `renderTables` 的符号表段）。
        // ⚠️ **必须在 `finalize()` 之前**（下方 §3.5(e) 才 addModule + finalize；此刻全局 index
        //   尚未分配 ⇒ 剪枝不影响索引确定性）；且**只动本模块的 collector**（线程私有，零共享）。
        // ============================================================
        metaCollector_->pruneUnmaterialized(emittedThunkNames);
    }

    // 关闭命名空间（若有）
    if (!nsName.empty()) {
        header << "} // namespace " << nsName << "\n";
        impl << "} // namespace " << nsName << "\n";
    }

    // 检测 main 函数并生成入口（必须在命名空间之外）
    for (auto& d : program.decls) {
        if (auto* f = dynamic_cast<const FunDecl*>(d.get())) {
            if (f->name == "main") {
                unit.hasMain = true;
                std::ostringstream footerStream;
                genMainEntry(footerStream, *f, nsName);
                unit.footer = footerStream.str();
                break;
            }
        }
    }

    // feature-12 批次 1（方案 F）：泛型闭包的文件作用域 struct 定义落盘
    //（必须在 impl 使用点之前——它们是 B 遍生成函数体时收集的）
    // 位置：插入 header 的 closureSplicePos（函数声明/定义之前）。
    { 
        std::string hstr = header.str();
        header.str(std::string());
        header.seekp(0);
        std::string block;
        {
            std::ostringstream tmp;
            flushClosureHeader(tmp);
            block = tmp.str();
        }
        if (!block.empty()) {
            size_t pos = closureSplicePos <= hstr.size() ? closureSplicePos : hstr.size();
            header << hstr.substr(0, pos) << block << hstr.substr(pos);
        } else {
            header << hstr;
        }
    }

    // ============================================================
    // feature-18 P3 §3.5(e)：类型收集（🔴 O8 补落点）+ `unit.metaImpl` 的生产点（🔴 O3 补）
    // ============================================================
    if (metaCollector_) {
        // ⚠️ 时机必须在 B 遍之后（`registeredTypes_` 会被 A/B 两遍填充）；此处即 generate() 末尾。
        // 🔴 **批 2 实测（见回报 §6）**：`registeredTypes_` **混装了函数名**——
        //   `generate()` 的第一遍对每个 `FunDecl` 调 `registerTypeName(f->name, false)`
        //   （`CodeGen.cpp:203-206`，语义 =「非堆的具名值」）⇒ 直接遍历会把 `main` / `probe_fn`
        //   当成类型写进 `kTypeTable`（实测产物：`{ main, TypeKindBits::Record, ... }`）。
        //   而 §3.5(c)/O8 明确要求「`kTypeTable` 本阶段**只含用户声明类型**」⇒ 必须过滤。
        std::set<std::string> fnNames;
        for (auto& d : program.decls)
            if (auto* ff = dynamic_cast<const FunDecl*>(d.get())) fnNames.insert(ff->name);
        // 🔴 O27：`registeredTypes_` 实为 `std::unordered_map<std::string,bool>`（CodeGen.h:845）⇒
        //   必须结构化绑定遍历（原写 `for (const auto* t : registeredTypes_)` **编不过**）。
        for (const auto& [name, isHeap] : registeredTypes_) {
            if (fnNames.count(name)) continue;   // 函数名不是类型（见上方注释）
            MetaTypeRec trec;
            trec.name = name;
            // ⚠️ 实测差异（见回报 §6）：change.md §3.5(c)/O8 的补块写 `kTypeRecord` / `kTypeHeap`
            //   两个**未定义**的标识符（§3.1 的枚举是 `TypeKindBits::Record = 1u<<0` 等，且
            //   **没有任何 "Heap" 位**）⇒ 照抄**编不过**。本批按 §3.1 的枚举取值：一律置 Record 位。
            //   ⚠️ `registeredTypes_` 的 value 语义 ≈「是否堆类型」（`DeclFun.cpp:35`/`:143-144` 消费），
            //     但 `meta::TypeKindBits` **无对应位** ⇒ 本阶段无处落（不臆造位；记录为待审取舍）。
            (void)isHeap;
            trec.kind   = (1u << 0);   // TypeKindBits::Record
            trec.nsName = nsName_;
            trec.descSymbol = "";      // 🔴 O11：基础投影阶段 desc 恒为空串（不解析 `_desc` 列）
            // ⚠️ 范围说明：`kTypeTable` 本阶段只含**用户声明类型**（内建/容器类型不收集 ——
            //   它们的 desc 是手写的，见 §1.1-B3）。
            metaCollector_->collectType(std::move(trec));
        }
    }

    // §3.5(e)：Inline 模式且注入了 collector ⇒ 生产 metaImpl（复用 MetaMerger 的**单模块形态**，
    //   这样 emitTablesInline 只需一套签名 —— O1 的类设计天然支持）。
    if (metaMode_ == CodeGenConfig::MetaMode::Inline && metaCollector_) {
        MetaMerger single;
        single.addModule(*metaCollector_);
        single.finalize();
        unit.metaImpl = MetaEmit::emitTablesInline(single);   // 表定义段（自含 include + thunk 前置声明）
    }
    // External 模式：metaImpl 留空（表由 aura.meta.cpp 承载，见 §3.6b）

    unit.header = header.str();
    unit.impl   = impl.str();
    return unit;
}

// ============================================================
// 调度
// ============================================================

void CodeGenerator::genDecl(std::ostream& h, std::ostream& cpp,
                             const Decl& decl, CompileUnit& /*unit*/,
                             bool declarationsOnly) {
    if (auto* t = dynamic_cast<const TypeDecl*>(&decl)) {
        // 类型声明在 A 遍生成（B 遍跳过，避免重复）
        if (declarationsOnly)
            { genTypeDecl(h, cpp, *t); return; }
        return;
    }
    if (auto* i = dynamic_cast<const InterfaceDecl*>(&decl)) {
        // 接口声明在 A 遍生成；适配器在第三遍 A 之后统一生成
        // （需 record struct 已完整定义，见 genDecl 后的 iface 收尾循环）
        if (declarationsOnly)
            { genInterfaceDecl(h, *i); return; }
        return;
    }
    if (auto* f = dynamic_cast<const FunDecl*>(&decl)) {
        // ---- feature-18 P3 §3.5(c)：收集挂钩（**仅 A 遍** —— B 遍会重复；
        //      现成范式：类型声明在 A 遍生成、B 遍跳过，`CodeGen.cpp:487-489`）----
        // ⚠️ P3 原排除 `main` 的理由（**历史，已被 P4a 批 1 推翻**）：其 C++ 名是 `aura_main`
        //    （`namedFnCppName` 不做该映射、`declaredFunNames_` 存的是 "aura_main"）⇒ 物化 thunk
        //    会生成对 `main` 的错误直呼；且 main 不是一等函数值。
        // 🔴 feature-18 P4a 批 1（A2，change.md §3.1.2 修法B / CP1c）：**main 也收集** ——
        //    理由：`genFunDecl` **会**为 main 走生成路径（`CodeGen.cpp:592-602` 还要据它产入口）
        //    ⇒ 若 main 无 seq，P4a 的 `FrameGuard` 注入（批 2）在 `frameSeqOf_` 里**查不到编号**
        //    ⇒ 帧表缺 main 行、且其后编号整体错位（`kModuleBase + seq` 与全局 index 不再恒等）。
        //    · `isFrame = true`（帧表**要**有 main 行）；
        //    · `materialize` 列**走空占位**、**不生成 thunk**：main 的形参是 `io: Io`（非
        //      int/float/bool）⇒ 下方 thunk 生成循环在 `sigOk` 判负处 `continue`（`:509`）
        //      ⇒ 本记录被 `pruneUnmaterialized` **降级**（A1：`thunkName` 清空 ⇒ 表项渲染
        //      `nullptr`，A1b）⇒ 既不会「错误直呼 main」，也不产生悬空表项；
        //    · `name` 用 **"main"**（用户可见名，change.md §9-N3）；**不是** `aura_main`；
        //    · 构造器不动（`!m->isConstructor` 保留 —— change.md §3.1.2 缺口 D：构造器走
        //      `genConstructor` 早退，P4a 既不收集也不注入，留 P4b）。
        if (declarationsOnly && metaCollector_) {
            MetaSymbolRec rec;
            rec.name    = f->name;                              // 裁定②：函数 = 裸名
            rec.file    = sourceFile_;                          // 与 Error.file 同源
            rec.defLine = static_cast<uint32_t>(decl.line);     // §9-V1：ASTNode::line
            rec.kind    = 0;                                    // SymbolKind::Fn
            // flags：本阶段只定义两位（余位恒 0 —— §3.5(c)/O26-②）。
            // ⚠️ 实测差异（见回报 §6）：change.md §3.5(c) 直接写 `kSymThrows` / `kSymCoroutine`，
            //   但这两个枚举量定义在 `runtime/meta.h` 的 `aura_rt::meta` 中，而 `src/`
            //   **从不 include runtime 头**（全仓零命中）⇒ 照抄**编不过**。故此处写等值字面量
            //   （bit0 = throws、bit1 = coroutine，与 runtime/meta.h 的 SymbolFlags 对齐）。
            rec.flags   = (f->throws ? (1u << 0) : 0u)
                        | (coroutineFunctions_.count(f->name) ? (1u << 1) : 0u);
            rec.ownerName = "";                                 // 函数无 owner
            rec.nsName    = nsName_;                            // 🔴 O26-①：genDecl 内 `unit` 被注释掉（B9）
            rec.isFrame   = true;                               // §3.2.2：函数/方法恒进帧表
            // 🔴 feature-18 P4a 批 1（A3，change.md §3.1.3）：把本模块 seq 记进 `frameSeqOf_`（B 遍注入查）。
            //   ⚠️ 键**必须**在 `std::move(rec)` **之前**取 —— `collectSymbol` 是**按值收**，
            //     move 之后调用方这份 `rec.name` 已是空串（而 `rec.seqInModule` 也仍是默认 0，
            //     **不能**用它代替返回值）。本键 == `rec.name` == `f->name`（函数 = 裸名）。
            const std::string symKey = rec.name;
            frameSeqOf_[symKey] = metaCollector_->collectSymbol(std::move(rec));
        }
        genFunDecl(h, cpp, *f, declarationsOnly);
        return;
    }
    if (auto* m = dynamic_cast<const MethodDecl*>(&decl)) {
        // ---- feature-18 P3 §3.5(c)：收集挂钩（仅 A 遍）----
        // ⚠️ 构造器不收集：其物化形态是 `kind=2`（`genCallableObjValueWrap` 的 `_ctor` 转发），
        //    与本批的 kind=0/1 模板不同；`change.md §3.5(c)` 未覆盖该形态（本批不涉及）。
        if (declarationsOnly && metaCollector_ && !m->isConstructor) {
            MetaSymbolRec rec;
            rec.name    = m->receiverType + "." + m->name;      // 裁定②：方法 = ReceiverType.method
            rec.file    = sourceFile_;
            rec.defLine = static_cast<uint32_t>(decl.line);
            rec.kind    = 1;                                    // SymbolKind::Method
            rec.flags   = (m->throws ? (1u << 0) : 0u)
                        | (coroutineFunctions_.count(m->receiverType + "." + m->name)
                               ? (1u << 1) : 0u);
            rec.ownerName = m->receiverType;
            rec.nsName    = nsName_;
            rec.isFrame   = true;
            // 🔴 feature-18 P4a 批 1（A3，change.md §3.1.3）：同函数侧 —— 键 = `rec.name`
            //   （= `ReceiverType.method`）；同样必须在 `std::move(rec)` 之前取。
            const std::string symKey = rec.name;
            frameSeqOf_[symKey] = metaCollector_->collectSymbol(std::move(rec));
        }
        genMethodDecl(h, cpp, *m, declarationsOnly);
        return;
    }
}

// ============================================================
// 输出辅助
// ============================================================

void CodeGenerator::newline(std::ostream& os) { os << '\n'; }

void CodeGenerator::indent(std::ostream& os) {
    for (int i = 0; i < indentLevel_; ++i) os << "    ";
}

void CodeGenerator::dedent(std::ostream&) { /* no-op */ }

void CodeGenerator::writeLine(std::ostream& os, const std::string& line) {
    // #31：语句写出前先落盘待写 outer 前缀（保证 auto _aX_Y 声明先于引用它的语句）。
    // C++ 参数求值先序于函数体：调用 writeLine(genExpr(...)) 时实参 genExpr 先执行
    //（outer 进缓冲）→ 进入函数体 flush 先落盘 → 语句后输出——机制性保证顺序。
    flushHoistPrefix(os);
    indent(os);
    os << line << '\n';
}

void CodeGenerator::flushHoistPrefix(std::ostream& os) {
    if (hoistPrefixPending_.empty()) return;
    // 逐行补缩进落盘（outer 语句生成时未带 indent，保持与函数体缩进风格一致）
    std::istringstream iss(hoistPrefixPending_);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty()) indent(os);
        os << line << '\n';
    }
    hoistPrefixPending_.clear();
}

// feature-12 批次 1（方案 F）：把闭包 struct 定义写入 header（文件作用域）。
// 调用时机：unit.header = header.str() 之前、命名空间已打开、B 遍已跑完。
// 拼接顺序：closureHeaderStack_ 里每层是「该层收集到的定义（子先父后已就序）」，
//           按栈序（外层在前）拼接即可——因为每层内部已保证子定义在其之前。
void CodeGenerator::flushClosureHeader(std::ostream& header) {
        if (closureHeaderStream_.empty()) return;
    header << "\n    // ==== feature-12：泛型闭包（多态值形态）文件作用域定义 ====\n";
    header << closureHeaderStream_;
    if (closureHeaderStream_.back() != '\n') header << '\n';
    closureHeaderStream_.clear();
    closureHeaderStack_.clear();
}

std::string CodeGenerator::indentStr() const {
    return std::string(static_cast<size_t>(indentLevel_) * 4, ' ');
}

std::string CodeGenerator::safeName(const std::string& name) const {
    // C++ 关键字/保留字冲突保护 — 编译器遇到冲突变量名时加后缀 _
    static const std::unordered_set<std::string> cppKeywords = {
        "class", "template", "typename", "auto", "const", "new",
        "delete", "virtual", "override", "default", "static", "enum",
        "double", "float", "int", "bool", "char", "short", "long",
        "void", "switch", "case", "break", "continue", "return",
        "if", "else", "for", "while", "do", "goto", "try", "catch",
        "throw", "namespace", "using", "public", "private", "protected",
        "struct", "union", "operator", "sizeof", "this", "true", "false",
        "nullptr", "noexcept", "mutable", "explicit", "export",
        "volatile", "register", "extern", "inline", "typedef",
        "friend", "constexpr", "consteval", "constinit", "decltype",
        "concept", "requires", "co_await", "co_return", "co_yield",
        "alignas", "alignof", "and", "and_eq", "bitand", "bitor",
        "compl", "not", "not_eq", "or", "or_eq", "xor", "xor_eq"
    };
    if (cppKeywords.count(name)) return name + "_";
    return name;
}

// ============================================================
// 错误
// ============================================================

void CodeGenerator::error(const ASTNode& node, const std::string& msg) {
    diag_.error(node, "codegen: " + msg);
}

// ============================================================
// feature-18 P4a 批 2（A7）：函数/方法体入口帧注入
// ============================================================

void CodeGenerator::emitEntryFrame(std::ostream& out, const std::string& symKey,
                                   uint32_t defLine, const ASTNode& node) {
    // 🔴 **P4a 修正（2026-10-02，主 Agent 定位）：帧注入必须随 meta 收集门控。**
    //    根因：`frameSeqOf_` 只在 A 遍「`declarationsOnly && metaCollector_`」时填充
    //    （`CodeGen.cpp:737/767`）⇒ **`metaCollector_ == nullptr` 时它恒为空**。
    //    而本函数原先**无条件**查表并 `error(...)` ⇒ 在「不注入 collector」的编译路径上
    //    （`test/framework/test_helpers.h:135` 的 `compileSource` 用默认 `CodeGenConfig{}`）
    //    **每个函数都报错** ⇒ 实测 **416 个单测转红**（全量 `1419 / 1003 passed / 416 failed`）。
    //    ⚠️ `aurac` CLI 恒注入 collector ⇒ **掩盖了该缺陷**（批 2/3 的端到端验证因此未发现）。
    //    ✅ 语义：无元数据表 ⇒ 无 `kModuleBase`/`kSymbolTable` ⇒ **无帧编号可言** ⇒ 不注入
    //       （与 P3 §4.4 的「门控」语义一致：本特性随元数据表启用）。
    if (!metaCollector_) return;

    auto it = frameSeqOf_.find(symKey);
    if (it == frameSeqOf_.end()) {
        // 🔴 R5（简报 §2）：**禁止静默跳过**。静默 = 该函数无帧 ⇒ Error.stack traceback
        //   断链，且「缺帧」不改变任何可观测行为 ⇒ **批 4 的单测无法发现**。
        //   此处记诊断错误 ⇒ main.cpp 的 `diag.hasErrors()` 判负 ⇒ 编译以非 0 退出。
        error(node, "feature-18 frame injection: frameSeqOf_ has no seqInModule for symbol '"
                    + symKey + "'（A 遍收集钩子未登记该符号）—— 帧注入失败，禁止静默继续");
        return;
    }
    // ⚠️ 命名空间必须写全 `aura_rt::meta::symbolIndexAt`（**不是** `aura_rt::symbolIndexAt`）：
    //    symbolIndexAt 定义在 aura_rt::meta（runtime/meta.h:19 开命名空间、:99 定义）⇒ 写错编不过。
    // 变量名含定义行号 ⇒ 同一作用域唯一（P4a 每个函数/方法体恒 1 处注入；P4b 的 lambda 另有其行号）。
    // 编号二元组 (moduleIdx, seqInModule) 为编译期常量，运行期由 symbolIndexAt 换算成全局 index
    //   （= kModuleBase[moduleIdx] + seqInModule，runtime/meta.h:99-101）。
    out << "  aura_rt::FrameGuard _lsg_" << defLine
        << "(aura_rt::meta::symbolIndexAt(" << throwSiteModuleIdx_ << "u, "
        << it->second << "u), " << defLine << "u);\n";
}

// ============================================================
// 🔵 feature-18 P4b-1 B5：协程上下文 lambda 的**匿名帧**注入
//   change.md：裁定④（四处）/ 裁定⑧（两级帧区）/ §3.1.2 缺口 C / §8.2 B0b / §6.2 C6–C8
// ============================================================
void CodeGenerator::emitAnonFrame(std::ostream& out, const std::string& kindName,
                                  uint32_t defLine, const ASTNode& node) {
    // 🔴 **门控（R7 / change.md §0.2 判据②，P4a 的血泪）**：`NullMetadata` ⇒ **零注入**。
    //   与 `emitEntryFrame`（:905）/ `StmtGen` 的 `setFrameLine` 门控**同源**：
    //   无元数据表 ⇒ 无 `kFrameTable` / `kAnonFrameBase` ⇒ 编号无从谈起 ⇒ 不发守卫。
    //   ⚠️ 门控必须**同时**覆盖「登记」与「发射」两件事 —— 只 gate 一侧会留下
    //      「登记了匿名帧却没有对应 kFrameCount 增量」或反之的错位。
    (void)node;
    if (!metaCollector_) return;

    // 登记 + 取号（**同源**：同一个 `seq` 既进表、又进注入文本）
    const uint32_t seq = metaCollector_->collectAnonFrame(
        MetaAnonFrameRec{ "<" + kindName + "@" + std::to_string(defLine) + ">",
                          sourceFile_, defLine });
    ++anonFrameSeq_;   // 仅作本模块计数自证（登记序号以 collector 返回值为准）

    // 注入形态（§3.1.4 N12 的运行时表达式形态 —— 匿名帧走**另一个区**的换算函数）：
    //     aura_rt::FrameGuard _lsga_<line>_<seq>(
    //         aura_rt::meta::anonFrameIndexAt(<moduleIdx>u, <seq>u), <line>u);
    // ⚠️ 命名空间必须写全 `aura_rt::meta::`（同 `symbolIndexAt`，见 :916 的教训）。
    // ⚠️ RAII：构造 pushFrame、析构 popFrame（异常路径亦配平）—— 与函数帧守卫同一机制。
    out << indentStr() << "aura_rt::FrameGuard _lsga_" << defLine << "_" << seq
        << "(aura_rt::meta::anonFrameIndexAt(" << throwSiteModuleIdx_ << "u, "
        << seq << "u), " << defLine << "u);\n";
}

} // namespace Aura
