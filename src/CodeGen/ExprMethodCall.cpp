#include "CodeGen.h"
#include "../Sema/BuiltinRegistry.h"
#include "../Sema/SemType.h"
#include <cctype>
#include <sstream>

namespace Aura {

// #48：跨模块（isNs）装箱点裸词物化替换所需的本地副本（ExprCall.cpp 同名 file-static
// 工具不可跨编译单元引用；本文件仅在下方 isNs 装箱分支使用）。
// 判定 s 中是否含"裸词" token（两侧为字母/数字/_ 之外的独立标识符，如 "aura_rt::
// Optional<T>*" 的 T）。
static bool containsBareToken(const std::string& s, const std::string& token) {
    if (token.empty() || s.size() < token.size()) return false;
    auto isIdChar = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '_';
    };
    for (size_t i = 0; i + token.size() <= s.size(); ++i) {
        if (s.compare(i, token.size(), token) == 0
            && (i == 0 || !isIdChar(s[i - 1]))
            && (i + token.size() >= s.size() || !isIdChar(s[i + token.size()])))
            return true;
    }
    return false;
}

// 将 s 中所有"裸词" token 整体替换为 repl（如 "aura_rt::Optional<T>*" 中 T → int32_t）。
static std::string replaceBareToken(std::string s, const std::string& token,
                                    const std::string& repl) {
    if (token.empty()) return s;
    auto isIdChar = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '_';
    };
    std::string out;
    out.reserve(s.size() + repl.size() * 2);
    size_t i = 0;
    while (i < s.size()) {
        if (s.compare(i, token.size(), token) == 0
            && (i == 0 || !isIdChar(s[i - 1]))
            && (i + token.size() >= s.size() || !isIdChar(s[i + token.size()]))) {
            out += repl;
            i += token.size();
        } else {
            out += s[i++];
        }
    }
    return out;
}

// bug-22：sync.ThreadChannel 判定共享辅助（ExprMethodCall.cpp / StmtControl.cpp 共用）。
// 主判定查 inferredType 为 GenericSemType{name=="sync.Channel"}（Sema 填充，不受 IterVarGuard
// 屏蔽，覆盖 spawn 参数/字段/函数参数等未进 gcRootTypes_ 的形态）；gcRootTypes_ 查
// "ThreadChannel" 仅作兜底（inferredType 缺失时的兼容回退）。主判定优先、兜底让位——若
// inferredType 已推得（非 sync.Channel 具体类型）直接返回 false，不再查 gcRootTypes_，
// 根除嵌套 spawn + 混 channel 类型 + 同名三层反向误判（协程 channel 同名参数被误判 sync →
// send 裸调用丢弃 recv_awaiter 静默不发送）。
bool CodeGenerator::isSyncChannelType(const SemType* inferredType,
                                      const std::string& varName) const {
    if (inferredType) {
        if (auto* g = dynamic_cast<const GenericSemType*>(inferredType))
            if (g->name == "sync.Channel") return true;
        return false;  // 已推得具体类型但非 sync.Channel → 不查兜底
    }
    if (varName.empty()) return false;
    auto it = gcRootTypes_.find(varName);
    return it != gcRootTypes_.end()
        && it->second.find("ThreadChannel") != std::string::npos;
}

std::string CodeGenerator::genMethodCall(const MethodCallExpr& e, bool isCoroutine) {
    // P4：联合接收者动态分派——receiver 是 UnionSemType 时生成运行时类型判定分派
    if (e.object && e.object->inferredType) {
        if (auto* u = dynamic_cast<const UnionSemType*>(e.object->inferredType))
            return genUnionDispatch(e, *u, isCoroutine);
    }
    // sync.Mutex() / sync.RWMutex() / sync.Once() / sync.Channel<T>(cap) 构造特殊处理
    // 解析为 MethodCallExpr(object=Identifier("sync"), method="Mutex"/.../"Channel")
    // Aura 暴露 sync.Channel<T>，C++ Runtime 仍叫 ThreadChannel<T>（与协程 Channel<T> 区分）
    if (auto* id = dynamic_cast<const Identifier*>(e.object.get())) {
        if (id->name == "sync") {
            // 无参构造：Mutex / RWMutex / Once / Channel()
            if (e.args.empty()) {
                if (e.method == "Mutex") {
                    return "aura_rt::make_mutex()";
                }
                if (e.method == "RWMutex") {
                    return "aura_rt::make_rwmutex()";
                }
                if (e.method == "Once") {
                    return "aura_rt::make_once()";
                }
                // sync.Channel() 无参 → cap=0（运行时视为 cap=1）
                if (e.method == "Channel") {
                    std::string targ = expectedTemplateArgs_.empty() ? "int32_t" : expectedTemplateArgs_[0];
                    std::string result = "aura_rt::make_thread_channel<" + targ + ">(0)";
                    // 跟踪为 channel 变量（for-in 展开用）
                    if (!currentLetName_.empty()) {
                        channelVarNames_.insert(currentLetName_);
                    }
                    return result;
                }
            } else {
                // sync.Channel<T>(cap) 带参构造
                if (e.method == "Channel") {
                    std::string targ = expectedTemplateArgs_.empty() ? "int32_t" : expectedTemplateArgs_[0];
                    std::string cap = genExpr(*e.args[0], isCoroutine);
                    std::string result = "aura_rt::make_thread_channel<" + targ + ">(" + cap + ")";
                    // 跟踪为 channel 变量（for-in 展开用）
                    if (!currentLetName_.empty()) {
                        channelVarNames_.insert(currentLetName_);
                    }
                    return result;
                }
            }
        }
    }

    std::string obj = genExpr(*e.object, isCoroutine);
    std::ostringstream oss;

    // ============================================================
    // Iterator 桥接方法特判（map/filter/collect/from 直转 runtime，不走虚调用）
    // 模板参数全部由 C++ 参数推导（src: Iterator<T>*, f: lambda → invoke_result_t）
    // ============================================================
    bool objIsIterator = false;
    // 先按 Sema 推断类型判定（覆盖 let 变量/表达式/参数：GenericSemType Iterator 或
    // InterfaceSemType Iterator）；Identifier 的 inferredType 在 Sema 推断时已填充
    if (e.object->inferredType) {
        if (auto* g = dynamic_cast<const GenericSemType*>(e.object->inferredType))
            objIsIterator = g->name == "Iterator";
        else if (auto* is = dynamic_cast<const InterfaceSemType*>(e.object->inferredType))
            objIsIterator = is->name == "Iterator";
    }
    // Iterator.from(...) 静态调用（Identifier "Iterator"，无 inferredType）
    if (auto* id = dynamic_cast<const Identifier*>(e.object.get()))
        if (id->name == "Iterator") objIsIterator = true;
    if (objIsIterator) {
        // 实参（闭包）表达式
        std::vector<std::string> iArgs;
        std::vector<const SemType*> iArgTypes;   // 包装后的实参类型标记（nullptr = 非堆）
        for (size_t i = 0; i < e.args.size(); ++i) {
            std::string arg = genExpr(*e.args[i], isCoroutine);
            const SemType* argTy = e.args[i]->inferredType;
            // feature-06（D1）：runtime Iterator 新路径形态（MapFnIter/FilterFnIter/
            // FuncFnIter，iterator.h）直接以 CallableObj<...>* 指针槽装载（GC 堆回调，
            // desc 追踪）——新路径闭包实参（CallableObj 派生指针 IIFE / 根化 CallableObj
            // 值 .get()）直传，不再包 Global 根转发 lambda（机制性消灭 map/filter/from 的
            // 手工包根）；旧路径 lambda 实参（泛型/协程/ViewRoot 捕获闭包）保持透传
            //（runtime F=可调用值承载，MapIter/FilterIter/FuncIter 旧类保留）。
            // 实参类型保留（FuncSemType，isHeapSemType=true）→ genGcRootedArgs 生成
            // ThreadLocal 根保护调用窗口内的回调指针，装载进 GC 槽前不悬垂。
            iArgs.push_back(arg);
            iArgTypes.push_back(argTy);
        }
        auto typedArg = [&](size_t i) { return i < iArgTypes.size() ? iArgTypes[i] : nullptr; };
        if (e.method == "from" && iArgs.size() == 1) {
            // FuncIter 无自动推导（T 与 F 无关联）：T 从闭包返回类型 Optional<T> 显式提取。
            // A2：显式 `-> Optional<string>` 注解物化为 GenericSemType{name=="Optional"}，
            // 与 OptionalSemType 统一经 optionalElemCppName 提取；真未知（确为 Optional
            // 但元素推不出）→ 防御性报错（不静默退 int32_t）。
            // 非 Optional 返回类型（如 `fun()->int`）保持既有 int32_t 兜底（Sema 侧同样
            // 落 int32_t、不报错），避免 CodeGen 与 Sema 判定不一致。
            std::string elem;
            bool retIsOptional = false;
            if (auto* ft = dynamic_cast<const FuncSemType*>(e.args[0]->inferredType)) {
                auto* rt = ft->returnType.get();
                retIsOptional = dynamic_cast<const OptionalSemType*>(rt)
                    || (dynamic_cast<const GenericSemType*>(rt)
                        && static_cast<const GenericSemType*>(rt)->name == "Optional");
                if (retIsOptional) elem = optionalElemCppName(rt);
            }
            if (retIsOptional && elem.empty()) {
                error(e, "cannot infer element type of closure return for 'Iterator.from'; "
                         "annotate the return type (e.g. fun () -> Optional<string>)");
                elem = "int32_t";  // 占位；driver 检测到 codegen 错误后不会调用 g++
            } else if (elem.empty()) {
                elem = "int32_t";  // 非 Optional 返回：既有兜底（与 Sema 一致）
            }
            return "aura_rt::make_iterator_from<" + elem + ">(" + iArgs[0] + ")";
        }
        if (e.method == "map" && iArgs.size() == 1) {
            std::string call = "aura_rt::make_map(" + obj + ", " + iArgs[0] + ")";
            std::vector<std::pair<std::string, const SemType*>> gArgs;
            gArgs.emplace_back(obj, e.object->inferredType);
            gArgs.emplace_back(iArgs[0], typedArg(0));
            return genGcRootedArgs(gArgs, call, isCoroutine);
        }
        if (e.method == "filter" && iArgs.size() == 1) {
            std::string call = "aura_rt::make_filter(" + obj + ", " + iArgs[0] + ")";
            std::vector<std::pair<std::string, const SemType*>> gArgs;
            gArgs.emplace_back(obj, e.object->inferredType);
            gArgs.emplace_back(iArgs[0], typedArg(0));
            return genGcRootedArgs(gArgs, call, isCoroutine);
        }
        if (e.method == "collect" && iArgs.empty()) {
            std::string call = "aura_rt::collect_all(" + obj + ")";
            std::vector<std::pair<std::string, const SemType*>> gArgs;
            gArgs.emplace_back(obj, e.object->inferredType);
            return genGcRootedArgs(gArgs, call, isCoroutine);
        }
        if (e.method == "next" && iArgs.empty()) {
            // 视图直接调用 next()：值视图 {nextFn, self} 的运行时方法（与 for-in
            // 循环体内 _it.get().next() 同入口，iterator.h Iterator<T>::next()）。
            // 返回 Optional<T>* 为 GC 堆对象，调用点 let/match 绑定会按类型走
            // GcRootHandle 保护，此处仅按 collect 惯例保护接收者视图 self。
            std::string call = obj + ".next()";
            std::vector<std::pair<std::string, const SemType*>> gArgs;
            gArgs.emplace_back(obj, e.object->inferredType);
            return genGcRootedArgs(gArgs, call, isCoroutine);
        }
    }

    // 判断是否是 io 调用
    bool isIoCall = false;
    if (auto* id = dynamic_cast<const Identifier*>(e.object.get())) {
        if (id->name == "io") isIoCall = true;
    }

    // feature-06（B3a）：闭包字段调用 b.f(args)（bug-13 形态：record 字段声明为函数
    // 类型，Sema 按方法调用推断）——字段 C++ 类型 = CallableObj 指针，须 invoke 接线：
    // b->f->invoke(b->f, args)。判定：receiver 为 record、method 名非其方法集成员、
    // 命中 RecordSemType 字段且字段声明类型为 FuncSemType。
    bool closureFieldCall = false;
    if (!isIoCall && e.object && e.object->inferredType) {
        if (auto* rs = dynamic_cast<const RecordSemType*>(e.object->inferredType)) {
            std::string recKey = rs->canonicalName;
            size_t lt = recKey.find('<');
            if (lt != std::string::npos) recKey = recKey.substr(0, lt);
            auto mIt = recordMethods_.find(recKey);
            bool isRealMethod = mIt != recordMethods_.end() && mIt->second.count(e.method);
            if (!isRealMethod) {
                for (auto& f : rs->fields) {
                    if (f.name == e.method && f.type
                        && dynamic_cast<const FuncSemType*>(f.type.get())) {
                        closureFieldCall = true;
                        break;
                    }
                }
            }
        }
    }

    // bug-38：Io 方法 Path 形参 + 字符串实参 → aura_rt::path::new_ 包装（string→Path
    // 隐式转换）。内置 Io 方法参数 Sema 不做 isAssignable 检查（仅 inferExpr），字符串
    // 直传 Path 形参会生成 GcString* → 无法转 const Path& 坏 C++。包装后为 Path 值
    // （非 GC），以 nullptr 标记非堆使 genGcRootedArgs 不生成 GcRootHandle 包裹。
    auto ioPathWrap = [&](size_t i, const std::string& expr, const SemType* argTy)
        -> std::pair<std::string, const SemType*> {
        if (isIoCall) {
            auto* entry = BuiltinRegistry::get().findMethod("Io", e.method, (int)e.args.size());
            if (entry && i < entry->params.size() && entry->params[i].typeName == "Path") {
                if (auto* p = dynamic_cast<const PrimSemType*>(argTy))
                    if (p->kind == PrimSemType::String)
                        return {"aura_rt::path::new_(" + expr + ")", nullptr};
            }
        }
        return {expr, argTy};
    };

    // #io.sync = true 或非协程上下文：所有 IO 方法统一加 _sync 后缀，提前返回
    // 非协程上下文（如 try/catch 协程安全模式的 IIFE）不能用 co_await，必须走同步版本
    // bug-37：仅对「有 _sync 变体」的 Io 方法加后缀——file_exists/cwd（kSyncIoMethods，
    // 无异步版本）无 _sync 变体，须落到普通路径生成 io.xxx()（需在 isIoCall 前已判）。
    if (isIoCall && (ioSync_ || !isCoroutine)
        && BuiltinRegistry::get().methodHasAsync("Io", e.method)) {
        std::vector<std::string> syncArgExprs;
        std::vector<std::pair<std::string, const SemType*>> syncArgs;
        for (size_t i = 0; i < e.args.size(); ++i) {
            auto [wexpr, wtype] =
                ioPathWrap(i, genExpr(*e.args[i], false), e.args[i]->inferredType);
            syncArgExprs.push_back(wexpr);
            syncArgs.push_back({wexpr, wtype});
        }
        std::ostringstream rawOss;
        rawOss << obj << "." << e.method << "_sync(";
        for (size_t i = 0; i < syncArgExprs.size(); ++i) {
            if (i > 0) rawOss << ", ";
            rawOss << "{" << i << "}";
        }
        rawOss << ")";
        return genGcRootedArgs(syncArgs, rawOss.str(), false);
    }

    // io.* 调用需要 co_await（仅对有异步版本的方法）
    bool needAwait = isIoCall && isCoroutine
                     && BuiltinRegistry::get().methodHasAsync("Io", e.method);

    // channel.send / channel.receive 需要 co_await（协程 channel 专用）
    // sync.ThreadChannel 的 send/receive 是阻塞调用，非协程 awaitable
    if (e.method == "send" || e.method == "receive") {
        // 协程 channel 判定：变量名（channelVarNames_，let/构造跟踪）或 receiver 推断
        // 类型（GenericSemType "channel"，覆盖方法/函数参数等未进 channelVarNames_ 的
        // channel）。sync.Channel 的 inferredType 是 "sync.Channel"，不匹配，不受影响。
        bool isCoroChannel = false;
        if (auto* id = dynamic_cast<const Identifier*>(e.object.get()))
            if (channelVarNames_.count(id->name)) isCoroChannel = true;
        if (!isCoroChannel && e.object->inferredType) {
            if (auto* g = dynamic_cast<const GenericSemType*>(e.object->inferredType))
                if (g->name == "channel") isCoroChannel = true;
        }
        if (isCoroChannel) {
            // bug-22：isSyncChannel 主判定改查 receiver inferredType（GenericSemType
            // "sync.Channel"，Sema 填充不受 IterVarGuard 屏蔽，覆盖 spawn 参数/字段/函数
            // 参数形态）；gcRootTypes_ 查 "ThreadChannel" 仅作兜底（inferredType 缺失时）。
            // 主判定优先、兜底让位——根除嵌套 spawn + 混 channel 类型 + 同名的反向误判。
            std::string varName;
            if (auto* id = dynamic_cast<const Identifier*>(e.object.get()))
                varName = id->name;
            bool isSyncChannel = isSyncChannelType(
                e.object ? e.object->inferredType : nullptr, varName);
            if (!isSyncChannel)
                needAwait = needAwait || isCoroutine;
        }
    }

    // 用户自定义协程方法调用：receiver 类型（RecordSemType.canonicalName 截取 '<' 前，
    // 与声明侧 receiverType 对齐）+ 方法名查 coroutineFunctions_（键 = "ReceiverType.
    // methodName"）→ co_await。仅在当前协程上下文加（非协程上下文不能 co_await）。
    // 方法体内调用协程方法（self.xxx()）由 decideCoro 传播标为协程，故 isCoroutine 恒真。
    if (isCoroutine) {
        std::string recvKey;
        if (e.object->inferredType) {
            if (auto* r = dynamic_cast<const RecordSemType*>(e.object->inferredType)) {
                if (!r->canonicalName.empty()) {
                    recvKey = r->canonicalName;
                    size_t lt = recvKey.find('<');
                    if (lt != std::string::npos) recvKey = recvKey.substr(0, lt);
                }
            }
        }
        if (!recvKey.empty() && coroutineFunctions_.count(recvKey + "." + e.method))
            needAwait = true;
    }

    std::string prefix = needAwait ? "co_await " : "";

    // 判断是命名空间限定下的构造调用：math.Pair(...) → math::Pair_ctor(...)
    // 检查条件：对象是导入的命名空间 + (方法名是本地注册的堆类型 或 以大写开头(跨模块类型))
    bool isNsCtor = false;
    // isNs：receiver 是导入的命名空间别名（如 path、io），不应作为表达式参与 GcRootedArgs 包装
    bool isNs = false;
    if (auto* id = dynamic_cast<const Identifier*>(e.object.get())) {
        if (importNsNames_.count(id->name)) {
            isNs = true;
            if (registeredTypes_.count(e.method) && registeredTypes_[e.method]) {
                isNsCtor = true;
            } else if (!e.method.empty() && std::isupper(static_cast<unsigned char>(e.method[0]))) {
                // 跨模块类型：导入命名空间下的 PascalCase 调用视为构造函数
                isNsCtor = true;
            }
        }
    }

    std::string access = "->";  // 默认指针访问

    if (isNsCtor) {
        oss << prefix << obj << "::" << safeName(e.method) << "_ctor";
        if (!expectedTemplateArgs_.empty()) {
            oss << "<";
            for (size_t i = 0; i < expectedTemplateArgs_.size(); ++i) {
                if (i > 0) oss << ", ";
                oss << expectedTemplateArgs_[i];
            }
            oss << ">";
        }
        oss << "(";
    } else {
        // 判断对象是值类型（用 . ）还是指针类型（用 -> ）还是命名空间（用 ::）
        if (auto* id = dynamic_cast<const Identifier*>(e.object.get())) {
            if (importNsNames_.count(id->name)) {
                access = "::";
            } else if (valueTypeVarNames_.count(id->name)) {
                access = ".";
            } else {
                access = "->";
            }
        } else if (e.object->inferredType && isIfaceView(e.object->inferredType)) {
            // #3：对象是视图值（接口视图字段 r.s / Iterator 字段 / 条件表达式结果等）
            // → 用 . 访问（视图是值类型，C++ 不允许 ->；视图字段的接口方法调用）
            access = ".";
        } else {
            access = "->";
        }
        std::string fieldName = safeName(e.method);
        if (closureFieldCall) {
            // feature-06（B3a）：闭包字段调用 → obj->f->invoke(obj->f, args)
            std::string m = obj + access + fieldName;
            oss << prefix << m << "->invoke(" << m << ", ";
        } else {
            oss << prefix << obj << access << fieldName << "(";
        }
    }
    // C5.3: 方法默认参数补齐（键 = ReceiverType.methodName）
    // recvTypeKey 从 receiver 的 inferredType 推导：
    //   string → "string"、Array<T> → "[T]"、泛型 T → g->name、自定义类型 → canonicalName
    std::string recvTypeKey;
    if (e.object->inferredType) {
        if (auto* p = dynamic_cast<const PrimSemType*>(e.object->inferredType)) {
            if (p->kind == PrimSemType::String) recvTypeKey = "string";
        } else if (dynamic_cast<const ListSemType*>(e.object->inferredType)) {
            recvTypeKey = "[T]";
        } else if (auto* g = dynamic_cast<const GenericSemType*>(e.object->inferredType)) {
            recvTypeKey = g->name;
        } else if (auto* r = dynamic_cast<const RecordSemType*>(e.object->inferredType)) {
            if (!r->canonicalName.empty()) recvTypeKey = r->canonicalName;
        } else if (auto* is = dynamic_cast<const InterfaceSemType*>(e.object->inferredType)) {
            // G3：接口视图接收者（s.put(...)，s: Stringer 视图）→ 键 = 接口名，
            // 与 genInterfaceDecl 注册的 methodInterfaceParams_/"接口名.methodName"
            // 同键（methodDefaultArgs_/methodParamCppTypes_ 亦然）。此前视图接收者
            // recvTypeKey 恒空，接口方法实参转换/默认参数/装箱全部查表不命中。
            recvTypeKey = is->name;
        }
    }
    // M2：方法默认参数查询键 = 声明侧 receiver 名（与 methodDefaultArgs_ 注册键
    // decl.receiverType + "." + decl.name 一致）。泛型 record 实例化的 canonicalName
    // 含类型实参（如 "Box<int32_t>"）→ 截取 '<' 前为声明名 "Box"；非泛型 canonicalName
    // 无 '<' 原样。仅 methodDefaultArgs_（默认参数补全）使用；装箱/接口参数查表仍用
    // 原 recvTypeKey（形参 C++ 类型含未绑定泛型名 T，无法在调用点直接实例化装箱）。
    std::string methodDefKey = recvTypeKey;
    {
        size_t lt = methodDefKey.find('<');
        if (lt != std::string::npos) methodDefKey = methodDefKey.substr(0, lt);
    }
    // 先收集参数表达式（保持参数顺序：前面的实参 + 尾部的默认参数）
    std::vector<std::string> mArgExprs;
    // G1：方法/接口方法形参 Optional/Union 装箱（键 = recvTypeKey + "." + method，与
    // methodDefaultArgs_ 同机制；record 方法由 genMethodDecl 注册、接口视图方法由
    // genInterfaceDecl 注册）。b.use({..}) / s.put({..}) / Box2({..} ctor 走 genCallExpr
    // isCtor 分支，不在此）→ 裸 record/值/列表直传 Optional/Union 方法形参时装箱。
    // bug-05：泛型 record 方法（recvTypeKey 为实例化 canonicalName，含 '<'）时注册键为
    // 声明名（DeclFun.cpp methodParamCppTypes_ 注册键 = decl.receiverType + "." + decl.name，
    // receiverType 无 <>）→ 原键 "Box<int32_t>.pick" 永不命中。原键未命中且 recvTypeKey
    // 含 '<' 时 fallback methodDefKey（截 '<' 前）+ "." + method 查声明侧表（与
    // methodDefaultArgs_ 的 methodDefKey 先例同键归一化）。「原键未命中才 fallback」
    // 保留非泛型路径零改动。mpInstFallback 标记命中是否经 fallback（泛型路径），供
    // 下方装箱前对形参 C++ 类型做调用点实例化（形参含裸泛型名 T）。
    auto mpIt = methodParamCppTypes_.find(recvTypeKey + "." + e.method);
    bool mpInstFallback = false;
    if (mpIt == methodParamCppTypes_.end()
        && recvTypeKey.find('<') != std::string::npos) {
        mpIt = methodParamCppTypes_.find(methodDefKey + "." + e.method);
        if (mpIt != methodParamCppTypes_.end()) mpInstFallback = true;
    }
    // G3：方法/接口方法接口参数（record 实参直传接口视图形参 → record→view）。
    // bug-05（方案 3）：泛型 record 方法同键不匹配 → 同法 methodDefKey 归一化查询
    // （接口名不含 T → 键归一化即够，record→view 转换本身与泛型无关）。
    auto miIt = methodInterfaceParams_.find(recvTypeKey + "." + e.method);
    if (miIt == methodInterfaceParams_.end()
        && recvTypeKey.find('<') != std::string::npos)
        miIt = methodInterfaceParams_.find(methodDefKey + "." + e.method);
    // G3：被 record→view 转换的实参 idx → 视图类型标记（genGcRootedArgs 据此走
    // 视图值分支，不生成 GcRootHandle<视图> 坏根；见 makeIfaceViewMarker）。
    std::map<size_t, std::unique_ptr<SemType>> viewArgTypes;
    // #48：跨模块（isNs）调用的泛型形参物化映射（{T:"int32_t"}）——依赖模块 exports
    // 的形参 SemType 若为物化 GenericSemType{Optional, resolvedName="aura_rt::Optional<T>"}，
    // mapSemType 直接输出含裸 T 的 C++ 串 → 装箱 make_optional<T> 坏 C++（跨模块无
    // fnParamCppTypes_/targValues 值源，只能从实参剥壳物化）。惰性收集一次供全循环使用。
    std::map<std::string, std::string> cmMat;
    if (isNs) {
        if (auto* id = dynamic_cast<const Identifier*>(e.object.get())) {
            auto cmPIt = crossModuleParamSemTypes_.find(id->name);
            if (cmPIt != crossModuleParamSemTypes_.end()) {
                auto fnPIt = cmPIt->second.find(e.method);
                if (fnPIt != cmPIt->second.end())
                    collectDefaultArgGenericMapFromSemTypes(fnPIt->second, e.args, cmMat);
            }
        }
    }
    for (size_t i = 0; i < e.args.size(); ++i) {
        std::string marg = genExpr(*e.args[i], isCoroutine);
        // G3：接口参数转换——record 实参直传接口视图形参 → genRecordToViewIIFE
        // （gcConstruct 适配器 + ::view，与 genCallExpr fnInterfaceParams_ 同构）。
        // 视图变量实参（InterfaceSemType）透传不二次包装；Optional/Union 形参
        // （paramCpp 非直连接口名，未注册进 methodInterfaceParams_）不在此处理。
        if (miIt != methodInterfaceParams_.end()) {
            for (auto& [idx, ifaceName] : miIt->second) {
                if (idx == i) {
                    const SemType* argTy = e.args[i]->inferredType;
                    if (argTy && dynamic_cast<const InterfaceSemType*>(argTy)) {
                        // 视图变量透传：不包装
                    } else if (auto* rt = dynamic_cast<const RecordSemType*>(argTy)) {
                        if (!rt->canonicalName.empty()) {
                            marg = genRecordToViewIIFE(marg, rt->canonicalName, ifaceName);
                            viewArgTypes[i] = makeIfaceViewMarker(ifaceName);
                        }
                    }
                    break;
                }
            }
        }
        // feature-06（阶段 B）：方法 FunctionType 形参回调（methodCallbackParams_ 消费）——
        // 闭包实参已是 CallableObj 基指针（新路径产物/根化值），方法模板形参
        // CallableObj<U,U>* 由 g++ 从实参静态类型推导——旧 std::function 包装退役
        //（bug-07 双分支单路径化；表保留作函数类型形参标记）。
        auto mcbIt = methodCallbackParams_.find(methodDefKey + "." + e.method);
        if (mcbIt != methodCallbackParams_.end()) {
            for (auto& [idx, ftStr] : mcbIt->second) {
                if (idx == i) {
                    (void)ftStr;   // 透传（旧路径 lambda 值形态阶段 C 再评估）
                    break;
                }
            }
        }
        // feature-06（阶段 B）：跨模块函数（isNs）FunctionType 形参回调——同款单路径
        // 化：闭包实参已是 CallableObj 基指针，形参（跨模块函数 C++ 侧同为
        // CallableObj<...>*）由 g++ 推导——旧 std::function 包装删除（mapSemTypeKeepGeneric
        // 现亦输出 CallableObj 形态，保持含外层模板参数原串直传即可）。
        if (isNs) {
            if (auto* id = dynamic_cast<const Identifier*>(e.object.get())) {
                auto cmPIt = crossModuleParamSemTypes_.find(id->name);
                if (cmPIt != crossModuleParamSemTypes_.end()) {
                    auto fnPIt = cmPIt->second.find(e.method);
                    if (fnPIt != cmPIt->second.end() && i < fnPIt->second.size()) {
                        const SemType* formal = fnPIt->second[i];
                        if (!formal) {
                            // 防御：形参 SemType 缺失（正常不会发生）
                        } else if (dynamic_cast<const FuncSemType*>(formal)) {
                            // 直传（实参 CallableObj 基指针/旧路径 lambda——lambda 值
                            // 无法向 CallableObj 形参推导，属旧路径残留，阶段 C 评估）
                        } else if (semTypeIsConcrete(formal) || !cmMat.empty()) {
                            // bug-06 附注 3：跨模块 Optional/Union 形参装箱（mArgExprs 装箱
                            // 的 mpIt 查 methodParamCppTypes_ 对 isNs 调用不命中 → 跨模块函数
                            // Optional 形参 + 裸值实参坏 C++）。用形参 SemType（依赖模块 exports
                            // 的 SymParam.type）mapSemType 得 C++ 类型后复用 genParamBoxing。
                            // #48：物化 GenericSemType{Optional}（显式 `o: Optional<T>` 注解，
                            // resolvedName="aura_rt::Optional<T>"）被 semTypeIsConcrete 判 true
                            // → mapSemType 输出含裸 T 串 → make_optional<T> 坏 C++ → 装箱前
                            // 按 cmMat（实参剥壳物化结果）裸词替换（T→int32_t）。mapSemType
                            // 兜底 "auto" 时 containsBareToken 不命中 → genParamBoxing 返回空
                            // → marg 不变（review 预判 B 防御链闭合）。
                            std::string paramCpp = mapSemType(*formal);
                            bool boxable = true;
                            if (!cmMat.empty()) {
                                for (auto& [g, cpp] : cmMat)
                                    if (containsBareToken(paramCpp, g))
                                        paramCpp = replaceBareToken(std::move(paramCpp), g, cpp);
                                // 防御：替换后仍含 cmMat 键裸词（物化不全）→ 不装箱
                                // （该路径 Sema 已报 cannot infer，driver 不调 g++）
                                for (auto& [g, cpp] : cmMat)
                                    if (containsBareToken(paramCpp, g)) { boxable = false; break; }
                            }
                            if (boxable) {
                                std::string boxed = genParamBoxing(
                                    paramCpp, *e.args[i], isCoroutine);
                                if (!boxed.empty()) marg = boxed;
                            }
                        }
                    }
                }
            }
        }
        if (mpIt != methodParamCppTypes_.end() && i < mpIt->second.size()) {
            // bug-05：fallback 命中（泛型 record 方法）→ 形参 C++ 类型含裸泛型名
            // （Optional<T> 的 T），调用点非模板作用域须先实例化再装箱（否则
            // make_optional<T> 泄漏坏 C++）。非 fallback（非泛型路径）零改动。
            std::string pCpp = mpInstFallback
                ? instantiateMethodParamCpp(methodDefKey, mpIt->second[i], recvTypeKey)
                : mpIt->second[i];
            std::string boxed = genParamBoxing(pCpp, *e.args[i], isCoroutine);
            if (!boxed.empty()) marg = boxed;
        }
        mArgExprs.push_back(marg);
    }
    // C5.3: 方法默认参数补齐（跨模块 ctor（isNsCtor）默认参数 v1 不支持）
    if (!isNs && !isNsCtor) {
        if (auto mmIt = methodDefaultArgs_.find(methodDefKey + "." + e.method); mmIt != methodDefaultArgs_.end())
            for (size_t k = e.args.size(); k < mmIt->second.size(); ++k)
                if (mmIt->second[k]) mArgExprs.push_back(genExpr(*mmIt->second[k], isCoroutine));
    }
    // C5.4: 跨模块函数默认参数补齐（math.foo(...) 缺参时）
    if (isNs) {
        if (auto* id = dynamic_cast<const Identifier*>(e.object.get())) {
            auto cmIt = crossDefaults_.find(id->name);
            if (cmIt != crossDefaults_.end()) {
                auto fnIt = cmIt->second.find(e.method);
                if (fnIt != cmIt->second.end()) {
                    // bug-06：跨模块默认参数补全仿同模块 M3（ExprCall.cpp:493-536）：
                    //  1) hasFunDefault 检测（默认实参含 FunExpr）；
                    //  2) 从已传实参 + 形参 SemType 收集泛型物化映射，并 save-restore
                    //     作用域化设置 defaultArgMaterializedTypes_——物化查询全局生效，
                    //     若不作用域化，泛型函数体内调用跨模块函数时外层模板参数名（U）
                    //     若与物化表键同名会被 mapSemType/mapType 误物化；
                    //  3) 默认实参闭包按物化后的形参 FuncSemType 包装 std::function
                    //     （mapSemType 递归物化形参中的裸泛型名 T → std::function<int32_t(int32_t)>）。
                    // 跨模块函数形参信息经 crossModuleParamSemTypes_（依赖模块 exports 的
                    // SymParam.type）取得，fnParamTypeExprs_/fnCallbackParams_ 仅本模块不可用。
                    std::vector<const SemType*>* psts = nullptr;
                    if (auto pIt = crossModuleParamSemTypes_.find(id->name);
                        pIt != crossModuleParamSemTypes_.end()) {
                        auto fIt = pIt->second.find(e.method);
                        if (fIt != pIt->second.end()) psts = &fIt->second;
                    }
                    std::map<std::string, std::string> materialized;
                    bool hasFunDefault = false;
                    for (size_t k = e.args.size(); k < fnIt->second.size(); ++k)
                        if (fnIt->second[k] && dynamic_cast<const FunExpr*>(fnIt->second[k])) {
                            hasFunDefault = true; break;
                        }
                    std::map<std::string, std::string> savedMat;
                    if (hasFunDefault && psts) {
                        collectDefaultArgGenericMapFromSemTypes(*psts, e.args, materialized);
                        if (!materialized.empty()) {
                            savedMat = defaultArgMaterializedTypes_;
                            defaultArgMaterializedTypes_ = materialized;
                        }
                    }
                    for (size_t k = e.args.size(); k < fnIt->second.size(); ++k) {
                        if (!fnIt->second[k]) continue;
                        std::string marg = genExpr(*fnIt->second[k], isCoroutine);
                        if (!materialized.empty() && psts && k < psts->size()
                            && dynamic_cast<const FuncSemType*>((*psts)[k]))
                            marg = mapSemType(*(*psts)[k]) + "(" + marg + ")";
                        mArgExprs.push_back(marg);
                    }
                    if (!materialized.empty()) defaultArgMaterializedTypes_ = savedMat;
                }
            }
        }
    }
    // 第 i 个参数的 inferredType（实参 → 方法默认 → 跨模块默认），供 GC 保护判断
    // G3：record→view 转换后的实参返回视图类型标记（isIfaceView 判定用）
    auto mArgType = [&](size_t i) -> const SemType* {
        auto vit = viewArgTypes.find(i);
        if (vit != viewArgTypes.end()) return vit->second.get();
        if (i < e.args.size()) return e.args[i]->inferredType;
        if (auto mmIt = methodDefaultArgs_.find(methodDefKey + "." + e.method);
            mmIt != methodDefaultArgs_.end() && i < mmIt->second.size() && mmIt->second[i])
            return mmIt->second[i]->inferredType;
        if (isNs) {
            if (auto* id = dynamic_cast<const Identifier*>(e.object.get())) {
                auto cmIt = crossDefaults_.find(id->name);
                if (cmIt != crossDefaults_.end()) {
                    auto fnIt = cmIt->second.find(e.method);
                    if (fnIt != cmIt->second.end() && i < fnIt->second.size() && fnIt->second[i])
                        return fnIt->second[i]->inferredType;
                }
            }
        }
        return nullptr;
    };
    for (size_t i = 0; i < mArgExprs.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << "{" << (i + 1) << "}";  // {0} = obj, {1..} = args
    }
    oss << ")";
    std::string callExpr = oss.str();

    // Io 调用、值类型对象、命名空间调用：obj 本身无需 GcRootHandle，但参数需要保护
    // （参数可能是返回 GC 指针的临时表达式，方法内部可能触发 GC 回收）
    auto buildRawCall = [&]() {
        std::ostringstream raw;
        if (closureFieldCall) {
            std::string m = obj + access + safeName(e.method);
            raw << prefix << m << "->invoke(" << m;
            for (size_t i = 0; i < mArgExprs.size(); ++i)
                raw << ", {" << i << "}";
            raw << ")";
        } else {
            raw << prefix << obj << access << safeName(e.method) << "(";
            for (size_t i = 0; i < mArgExprs.size(); ++i) {
                if (i > 0) raw << ", ";
                raw << "{" << i << "}";  // genGcRootedArgs 从 {0} 开始替换
            }
            raw << ")";
        }
        return raw.str();
    };
    if (isIoCall || isNsCtor || isNs) {
        // 用 genGcRootedArgs 包装参数（obj 是值类型/命名空间，不参与包装）
        // isNs：path.new(...) / math.abs(...) 等，receiver 是 namespace 别名，
        // 不能作为表达式求值（不能 `const auto& x = (path);`），必须直接用 obj 名字生成 obj::method(...)
        std::vector<std::pair<std::string, const SemType*>> ioArgs;
        for (size_t i = 0; i < mArgExprs.size(); ++i) {
            auto [wexpr, wtype] = ioPathWrap(i, mArgExprs[i], mArgType(i));
            ioArgs.push_back({wexpr, wtype});
        }
        return genGcRootedArgs(ioArgs, buildRawCall(), isCoroutine);
    }
    if (e.object.get() && e.object->inferredType
        && !isHeapSemType(e.object->inferredType)) {
        std::vector<std::pair<std::string, const SemType*>> valArgs;
        for (size_t i = 0; i < mArgExprs.size(); ++i)
            valArgs.push_back({mArgExprs[i], mArgType(i)});
        return genGcRootedArgs(valArgs, buildRawCall(), isCoroutine);
    }

    // 堆类型对象或参数 → GcRootHandle 保护
    std::vector<std::pair<std::string, const SemType*>> gcArgs;
    gcArgs.emplace_back(obj, e.object->inferredType);  // {0} = obj
    for (size_t i = 0; i < mArgExprs.size(); ++i)
        gcArgs.emplace_back(mArgExprs[i], mArgType(i));  // {i+1}
    // 构建带占位符的 callExpr
    std::ostringstream gcCall;
    if (closureFieldCall) {
        std::string m = "{0}" + access + safeName(e.method);
        gcCall << prefix << m << "->invoke(" << m;
        for (size_t i = 0; i < mArgExprs.size(); ++i)
            gcCall << ", {" << (i + 1) << "}";
        gcCall << ")";
    } else {
        gcCall << prefix << "{0}" << access << safeName(e.method) << "(";
        for (size_t i = 0; i < mArgExprs.size(); ++i) {
            if (i > 0) gcCall << ", ";
            gcCall << "{" << (i + 1) << "}";
        }
        gcCall << ")";
    }
    std::string callResult = genGcRootedArgs(gcArgs, gcCall.str(), isCoroutine);

    return callResult;
}

} // namespace Aura
