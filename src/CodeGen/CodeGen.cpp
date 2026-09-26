#include "CodeGen.h"
#include "../Sema/SemAnalyzer.h"
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
    //     搬运走的 message 会让 _u5msg 悬垂，末尾 throw 出悬垂指针 → 用户 try-catch UAF。
    //   形态对齐既有做法 src/CodeGen/StmtTry.cpp（那里三处已为 kind/message/extra
    //   生成 GcRootHandle）。kind 理论安全（intern_string 注册为全局根），
    //   但保持一致根化，防将来 kind 来源变化。
    //
    // ⚠️ 不用 std::optional<Error> 直存：GcRootHandle 的 Ref 模式绑定**变量地址**，
    //    optional 未 engaged 时 _u5err->message 的地址无效 → 必须拆成独立标量。
    // ⚠️ 无 sync 上下文（u5ErrSuffix_ 为空：理论上仅 sync 块内会声明 future，
    //    此处兜底为「裸驱动，不做异常包裹」，避免生成引用未声明变量的代码）。
    std::string sfx = u5ErrSuffix_;
    if (!sfx.empty()) {
        writeLine(cpp, "try { co_await " + frame.names[0] + "; } catch (const aura_rt::Error& _u5e" +
                       sfx + ") {");
        indentLevel_++;
        writeLine(cpp, "if (!_u5has" + sfx + ") { _u5has" + sfx + " = true; _u5msg" + sfx +
                       " = _u5e" + sfx + ".message; _u5kind" + sfx + " = _u5e" + sfx + ".kind; }");
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
            writeLine(cpp, "if (!_u5has" + sfx + ") { _u5has" + sfx + " = true; _u5msg" + sfx +
                           " = _u5e" + sfx + ".message; _u5kind" + sfx + " = _u5e" + sfx + ".kind; }");
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
                                     const CrossModuleParamSemTypes& crossParamSemTypes) {
    ioSync_ = config.ioSync;
    crossDefaults_ = crossDefaults;   // C5.4: 跨模块函数默认参数表
    crossModuleParamSemTypes_ = crossParamSemTypes;  // bug-06: 跨模块函数形参 SemType 表
    CompileUnit unit;
    unit.moduleName = moduleName;
    unit.nsName     = nsName;

    std::ostringstream header, impl;
    headerStream_ = &header;
    implStream_   = &impl;

    // 公共头
    header << "#include \"aura_rt.h\"\n";

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
        genFunDecl(h, cpp, *f, declarationsOnly);
        return;
    }
    if (auto* m = dynamic_cast<const MethodDecl*>(&decl)) {
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

} // namespace Aura
