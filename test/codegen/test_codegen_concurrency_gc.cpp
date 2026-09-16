// ============================================================
// test_codegen_concurrency_gc.cpp — CodeGen 输出单元测试：spawn / sync thread / channel 并发 / GC root 与 desc / Batch / 跨模块
// ============================================================
// 由超大单文件 test_codegen.cpp（272 个 TEST(CodeGen, ...)）按主题拆分而来；
// 2026-09-06 重构：内容为原样搬移，未改动任何断言 / 源码 / 语义。
// ============================================================
#include "framework/test_framework.h"
#include "framework/test_helpers.h"

#include <filesystem>
#include <fstream>
#include <string>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace aura_test;

TEST(CodeGen, SpawnMultiParamChannelFirstNoGet) {
    // 2 参 spawn，channel 首参（修复前 body 误生 ch.get()->send → 坏 C++）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {"
        " let ch: channel<int> = channel(10)"
        " sync {"
        "   spawn (ch: channel<int>, x: int) {"
        "     ch.send(x); ch.close()"
        "   }(ch, 2)"
        "   spawn (ch: channel<int>) {"
        "     for v in ch { io.println(str(v)) }"
        "   }"
        " } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // body 内 ch 接收者经 GcRootHandle 中间变量保护（_hN_M.get()->send）
    EXPECT_CONTAINS(unit.impl, ".get()->send(");
    // 不得对 lambda 形参裸指针误调 .get()（修复前形态）
    EXPECT_NOT_CONTAINS(unit.impl, "ch.get()->send(");
    EXPECT_NOT_CONTAINS(unit.impl, "ch.get()->is_done(");
}

TEST(CodeGen, SpawnThreeParamChannelFirstNoGet) {
    // 3 参 spawn，前两参 channel（修复前前两参均误生 .get()）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {"
        " let ch1: channel<int> = channel(10)"
        " let ch2: channel<int> = channel(10)"
        " sync {"
        "   spawn (ch1: channel<int>, ch2: channel<int>, z: int) {"
        "     ch1.send(z); ch2.send(z * 2); ch1.close(); ch2.close()"
        "   }(ch1, ch2, 7)"
        "   spawn (ch1: channel<int>) { for v in ch1 { io.println(str(v)) } }"
        "   spawn (ch2: channel<int>) { for v in ch2 { io.println(str(v)) } }"
        " } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_NOT_CONTAINS(unit.impl, "ch1.get()->send(");
    EXPECT_NOT_CONTAINS(unit.impl, "ch2.get()->send(");
    EXPECT_NOT_CONTAINS(unit.impl, "ch1.get()->close(");
    EXPECT_NOT_CONTAINS(unit.impl, "ch2.get()->close(");
}

TEST(CodeGen, SpawnRecordFirstNoGet) {
    // 2 参 spawn，record 首参（修复前 body 误生 p.get()->x + x）
    // 注：let p: Point = { x = 3, y = 4 } 初始化会正确生成 p.get()->x = 3
    // （外层 GcRootHandle 字段赋值），故用 body 内算术形态精确断言
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) {"
        " let p: Point = { x = 3, y = 4 }"
        " sync {"
        "   spawn (p: Point, x: int) { io.println(str(p.x + x)) }(p, 5)"
        " } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // body 内 p 为裸指针形参，字段访问 p->x（非误生 p.get()->x）
    EXPECT_CONTAINS(unit.impl, "p->x + x");
    EXPECT_NOT_CONTAINS(unit.impl, "p.get()->x + x");
}

TEST(CodeGen, SpawnMultiParamSyncMaxNoGet) {
    // sync(max=N) 有界块内多参 spawn（同源 bounded_sync 路径）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {"
        " let ch: channel<int> = channel(10)"
        " sync(max = 2) {"
        "   spawn (ch: channel<int>, x: int) { ch.send(x); ch.close() }(ch, 9)"
        "   spawn (ch: channel<int>) { for v in ch { io.println(str(v)) } }"
        " } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, ".get()->send(");
    EXPECT_NOT_CONTAINS(unit.impl, "ch.get()->send(");
}

// ============================================================
// bug-10（2026-08-31）：sync thread 内 spawn 显式实参被静默忽略
// 根因：genSpawnAsThread 捕获列表只按参数名生成（不读 stmt.args）→ 实参被丢弃 / 坏 C++ /
//       外层同名静默绑错。修复：显式实参按位置 init-capture——
//       - 值类型实参：裸 init-capture `x = <expr>`（外层作用域求值）
//       - 堆类型实参：init-capture 持 GcRootHandle Global 根（对齐 ExprClosure 跨线程捕获
//         先例，worker 线程 GC 扫描可见，避免裸 .get() 指针悬垂）；body 内参数名 .get() 联动
//       - io 参数保持 &io 引用捕获（位置对齐，跳过 io 勿收缩 args 索引）
//       - 同名自动绑定（无实参）保持裸名捕获不误伤；bug-72 起：GC 根 / 视图根命中项改
//         同名 init-capture 的 Global 根（跨线程析构安全），非 GC 根值绑定仍为裸名
// ============================================================
TEST(CodeGen, SyncThreadSpawnExplicitArgsInitCapture) {
    // 主线：sync thread 内 spawn 显式实参（实参与参数名异）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {"
        " let ch: sync.Channel<int> = sync.Channel(10)"
        " sync thread {"
        "   spawn (ch: sync.Channel<int>, x: int) { ch.send(x) }(ch, 3)"
        " }"
        " ch.close() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 值类型实参 x → 裸 init-capture x = 3（修复前 [ch, x] 按参数名裸捕获、实参被丢弃）
    EXPECT_CONTAINS(unit.impl, "x = 3");
    // 堆类型实参 ch → GcRootHandle Global 根 init-capture（跨线程安全）
    EXPECT_CONTAINS(unit.impl, "GcRootScope::Global");
    EXPECT_CONTAINS(unit.impl,
        "GcRootHandle<aura_rt::ThreadChannel<int32_t>*>(ch.get(), aura_rt::GcRootScope::Global)");
    // 不再出现修复前按参数名捕获的裸名形态
    EXPECT_NOT_CONTAINS(unit.impl, "submit([ch, x]");
}

TEST(CodeGen, SyncThreadSpawnExplicitHeapArgGlobalRoot) {
    // 堆类型实参（record / GcString）→ Global 根 init-capture + body 内参数名 .get() 联动
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) throws {"
        " let ch: sync.Channel<string> = sync.Channel(10)"
        " let p: Point = { x = 10, y = 20 }"
        " sync thread {"
        "   spawn (c: sync.Channel<string>, pt: Point, tag: string) {"
        "     c.send(\"pt: \" + pt.x + \",\" + pt.y + \" tag: \" + tag)"
        "   }(ch, p, \"hello\")"
        " }"
        " ch.close() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // record 实参 → GcRootHandle<Point*> Global 根
    EXPECT_CONTAINS(unit.impl, "GcRootHandle<Point*>(p.get(), aura_rt::GcRootScope::Global)");
    // GcString 字面量实参 → GcRootHandle<GcString*> Global 根
    EXPECT_CONTAINS(unit.impl,
        "GcRootHandle<aura_rt::GcString*>(aura_rt::intern_string(\"hello\"), aura_rt::GcRootScope::Global)");
    // body 内 record 参数名经 .get() 解引用（字段访问 pt.x → pt.get()->x 形态）
    EXPECT_CONTAINS(unit.impl, ".get()->x");
}

TEST(CodeGen, SyncThreadSpawnAutoBindGcRootGlobal) {
    // bug-10 对照 + bug-72：同名自动绑定（无显式实参）中 GC 根变量（ch 为堆对象）改同名
    // init-capture 的 Global 根（提交线程注册 / worker 线程析构跨线程安全）；
    // 非 GC 根值绑定（x: int）仍是裸名捕获
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {"
        " let ch: sync.Channel<int> = sync.Channel(10)"
        " let x = 3"
        " sync thread {"
        "   spawn (ch: sync.Channel<int>, x: int) { ch.send(x) }"
        " }"
        " ch.close() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl,
        "submit([ch = aura_rt::GcRootHandle<aura_rt::ThreadChannel<int32_t>*>(ch.get(), "
        "aura_rt::GcRootScope::Global), x]() mutable");
    // 值类型绑定不受影响（不升级 Global 根、不加 init-capture）
    EXPECT_NOT_CONTAINS(unit.impl, "x = aura_rt::GcRootHandle");
}

TEST(CodeGen, SyncThreadSpawnIoParamArgsAligned) {
    // bug-10：io 参数在 params 中占位，(io,x)(io,3) 中 args[0]=io 跳过、args[1]=3 用于 x
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {"
        " sync thread {"
        "   spawn (io: Io, x: int) { io.println(\"x: \" + x) }(io, 3)"
        " }"
        " io.println(\"done\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "submit([x = 3, &io]");
    EXPECT_NOT_CONTAINS(unit.impl, "submit([io");
}

// ============================================================
// M2：泛型 record 方法默认参数不补全（problem.txt 条目）
//  注册键（DeclGen genMethodDecl A 遍）= decl.receiverType + "." + name = "Box.useCb"（声明名）；
//  查询键（ExprGen genMethodCall）原为 recvTypeKey = 实例化 canonicalName "Box<int32_t>"
//  → 键不匹配 → 默认参数不补全 → g++ too few arguments。
//  修复：方法默认参数查询键归一化为声明侧 receiver 名（canonicalName 截取 '<' 前）。
// ============================================================
TEST(CodeGen, GenericRecordMethodDefaultArgsFilled) {
    // 泛型 record 方法带默认参数（闭包 + 非闭包 int），缺参调用须补全默认实参
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Box<T> = { value: T }"
        " fun (self Box<T>) useCb(v: T, cb: fun(T)->T = fun(x: T)->T { return x }) -> T { return cb(v) }"
        " fun (self Box<T>) useAll(v: T, n: int = 7) -> int { return n }"
        " fun main(io: Io) throws {"
        " let b: Box<int> = { value = 5 }"
        " let r1 = b.useCb(5)"
        " let r2 = b.useAll(1) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // useCb(5) 补默认闭包实参（模板 lambda，外层 T 无遮蔽；调用点 2 实参）
    // feature-12 批次 2（bug-83 修复，2026-09-15）：默认闭包实参由「模板 lambda」
    // 改为 F 家族具名 struct（genGcUClosure）——泛型域闭包统一走文件作用域
    // 模板 struct + 成员函数模板 operator()（多态值语义保留）。
    // 产物形态（used/探针实证）：
    //   struct __GcUClosure_0 final : aura_rt::CallableObjBase {
    //       template <typename T> T operator()(T x) { return x; } ... };
    EXPECT_CONTAINS(unit.header, "struct __GcUClosure_0 final : aura_rt::CallableObjBase");
    EXPECT_CONTAINS(unit.header, "template <typename T>");
    EXPECT_CONTAINS(unit.header, "T operator()(T x)");
    EXPECT_CONTAINS(unit.impl, "useCb(_a1_1, _a1_2)");
    // useAll(1) 补 int 默认值 7（调用点 2 实参）
    EXPECT_CONTAINS(unit.impl, "useAll(_a2_1, _a2_2)");
    // 方法签名模板参数只含 T（feature-06：函数类型形参 → CallableObj 指针）
    EXPECT_CONTAINS(unit.header, "T Box<T>::useCb(T v, aura_rt::CallableObj<T, T>* cb_raw)");
}

TEST(CodeGen, NonGenericRecordMethodDefaultArgsStillFilled) {
    // M2 回归：非泛型 record 方法默认参数（canonicalName 无 '<'，归一化原样）
    // 不得被误伤——仍须补全默认实参
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Pt = { x: int, y: int }"
        " fun (self Pt) sumAll(scale: int = 2, tag: string = \"t\") -> string { return str(self.x + scale) + tag }"
        " fun main(io: Io) throws {"
        " let p: Pt = { x = 1, y = 2 }"
        " let r1 = p.sumAll()"
        " let r2 = p.sumAll(10) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // sumAll() 补 2 + "t"（字符串默认参数经 GC 保护 _hN_2.get()）
    EXPECT_CONTAINS(unit.impl, "sumAll(_a1_1, _h1_2.get())");
    // sumAll(10) 补 "t"
    EXPECT_CONTAINS(unit.impl, "sumAll(_a2_1, _h2_2.get())");
}

TEST(CodeGen, SpawnClosureForInCoroChannelCoAwaitReceive) {
    // bug-11/22 对照角：协程 channel<T> 直接在 spawn 闭包内 for-in（channelVarNames_ 命中，
    // 非 inSyncThreadBlock_）→ 走协程 receive 路径（is_done + co_await receive）。
    // 与 SyncChannel...ForInBlockingReceive（sync 阻塞路径 is_none）互补：不得误落阻塞路径。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {"
        " let ch: channel<int> = channel(10)"
        " sync {"
        "   spawn (ch: channel<int>) {"
        "     for i in range(5) { ch.send(i) }"
        "     ch.close()"
        "   }"
        "   spawn (ch: channel<int>) {"
        "     let sum: int = 0"
        "     for v in ch { sum = sum + v }"
        "     io.println(\"spawn sum=\" + sum)"
        "   }"
        " }"
        " io.println(\"done\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // spawn 闭包内 for-in 协程 channel：协程 receive 路径（co_await ch->receive + is_done 终止）
    EXPECT_CONTAINS(unit.impl, "co_await ch->receive");
    EXPECT_CONTAINS(unit.impl, "ch->is_done()) break");
    // 不得误落 sync 阻塞路径（is_none 终止）
    EXPECT_NOT_CONTAINS(unit.impl, "is_none()) break");
}

TEST(CodeGen, RecordFieldForInPreEvalGcRoot) {
    // bug-11 方向①非 Identifier 形态（record 字段 b.ch）：预求值 + GcRootHandle 保护，
    // 循环统一 _ch.get()（防协程 co_await 挂起期间 GC compact 悬垂）。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Box2 = { ch: channel<int> }"
        " fun main(io: Io) {"
        " let ch: channel<int> = channel(10)"
        " let b = Box2 { ch = ch }"
        " sync {"
        "   spawn (b: Box2) {"
        "     for i in range(5) { b.ch.send(i) }"
        "     b.ch.close()"
        "   }"
        " }"
        " let sum: int = 0"
        " for v in b.ch { sum = sum + v }"
        " io.println(\"done\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 预求值 + GC 根保护
    EXPECT_CONTAINS(unit.impl, "auto _ch_raw = ");
    EXPECT_CONTAINS(unit.impl, "GcRootHandle<decltype(_ch_raw)> _ch(_ch_raw, aura_rt::GcRootScope::ThreadLocal);");
    // 循环体统一引用 _ch.get()（协程 receive 路径）
    EXPECT_CONTAINS(unit.impl, "co_await _ch.get()->receive");
    // 不落默认 range-for（坏 C++）
    EXPECT_NOT_CONTAINS(unit.impl, "for (auto v : *");
}

TEST(CodeGen, CallReturnForInPreEvalOnce) {
    // bug-11 方向①非 Identifier 形态（函数调用返回 channel）：预求值一次性求值——
    // 带副作用表达式（getCh()）仅求值一次（auto _ch_raw = getCh();），循环引用 _ch.get()。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun getCh() -> channel<int> { return channel(10) }"
        " fun main(io: Io) {"
        " let ch = getCh()"
        " sync {"
        "   spawn (ch: channel<int>) {"
        "     for i in range(5) { ch.send(i) }"
        "     ch.close()"
        "   }"
        " }"
        " let sum: int = 0"
        " for v in getCh() { sum = sum + v }"
        " io.println(\"done\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 预求值：iterable 一次性求值到 _ch_raw（而非循环内每轮重调 getCh()）
    EXPECT_CONTAINS(unit.impl, "auto _ch_raw = getCh();");
    EXPECT_CONTAINS(unit.impl, "GcRootHandle<decltype(_ch_raw)> _ch(_ch_raw, aura_rt::GcRootScope::ThreadLocal);");
    // 循环引用 _ch.get()，不再出现 range-for 重求值
    EXPECT_CONTAINS(unit.impl, "co_await _ch.get()->receive");
    EXPECT_NOT_CONTAINS(unit.impl, "for (auto v : *");
}

TEST(CodeGen, SyncThreadCoroChannelForInCleanError) {
    // bug-11 方向④：协程 channel 在 sync thread 块体内 for-in → 干净报错
    // （recv_awaiter 无 is_none/unwrap，生成阻塞 receive 会坏 C++）。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {"
        " let ch: channel<int> = channel(10)"
        " sync thread {"
        "   for v in ch { io.println(\"v=\" + v) }"
        " }"
        " io.println(\"done\") }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "cannot iterate coroutine channel in sync thread block"));
}

TEST(CodeGen, CoroMethodClosureRefsSelfUsesGcRootHandle) {
    // 协程方法（io.println → co_await）内闭包引用 self.inc：裸 [this] 跨挂起 GC compact
    // 悬垂 → 决策 (a)：捕获 GcRootHandle<RecType*> init-capture，body 经 .get() 解引用
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Counter<T> = { inc: T }"
        " fun (self Counter<T>) use_closure(io: Io) throws {"
        "   let f = fun(x: T) -> T { return x + self.inc }"
        "   io.println(str(f(5))) }"
        " fun main(io: Io) throws {"
        " let c: Counter<int> = { inc = 10 }"
        " c.use_closure(io) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 协程方法体在 header；闭包引用 self → CallableObj 捕获槽 cap_recv（desc offsetof
    // 追踪，GC compact 重写捕获指针）；填槽源取入口句柄 _this.get()（协程方法入口
    // _this 为 Global 根）
    EXPECT_CONTAINS(unit.header, "Counter<T>* cap_recv;");
    EXPECT_CONTAINS(unit.header, "__o_h.get()->cap_recv = _this.get();");
    EXPECT_CONTAINS(unit.header, "__c_h.get()->cap_recv->inc");
    // 不得退化为裸 [this]（跨挂起裸指针悬垂）
    EXPECT_NOT_CONTAINS(unit.header, "[this](T x)");
}

TEST(CodeGen, SyncThreadSpawnClosureCoroChannelForInCleanError) {
    // bug-11 方向④变体：协程 channel 在 sync thread 块内 spawn 闭包体 for-in
    // （inSyncThreadBlock_ 恒 true，genSpawnAsThread 不改标志）→ 同样干净报错。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {"
        " let ch: channel<int> = channel(10)"
        " sync thread {"
        "   spawn (ch: channel<int>, io: Io) {"
        "     for v in ch { io.println(\"v=\" + v) }"
        "   }"
        " }"
        " io.println(\"done\") }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "cannot iterate coroutine channel in sync thread block"));
}

TEST(CodeGen, SyncThreadSyncChannelForInBlockingControl) {
    // bug-11 方向④对照组：sync.Channel 在 sync thread 块内 for-in → 不报错，
    // 走阻塞 receive 路径（is_none 终止）——不误伤 sync.ThreadChannel。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {"
        " let ch: sync.Channel<int> = sync.Channel(10)"
        " sync thread {"
        "   spawn (ch: sync.Channel<int>) {"
        "     for i in range(5) { ch.send(i) }"
        "     ch.close()"
        "   }"
        "   spawn (ch: sync.Channel<int>, io: Io) {"
        "     for v in ch { io.println(\"v=\" + v) }"
        "   }"
        " }"
        " io.println(\"done\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 阻塞 receive 循环（is_none 终止），非协程 receive 路径（is_done + co_await）
    EXPECT_CONTAINS(unit.impl, "is_none()) break");
    EXPECT_NOT_CONTAINS(unit.impl, "is_done()) break");
    EXPECT_NOT_CONTAINS(unit.impl, "co_await ch.get()->receive");
}

// ============================================================
// bug-06：跨模块泛型函数默认参数闭包引用函数模板 T（isNs 调用）
// 同模块 M3（ExprCall.cpp fnDefaultArgs_ 分支）已修；跨模块形态 genMethodCall isNs
// 分支此前缺 std::function 包装 + 泛型物化（无 fnCallbackParams_/fnParamTypeExprs_，
// 须经 crossModuleParamSemTypes_ 取得形参 SemType）。
// ============================================================
namespace {
// 创建唯一临时目录 + 写模块文件（与 test_sema_modules.cpp 一致）
std::string cgModTempDir() {
    auto base = std::filesystem::temp_directory_path();
    auto dir = base / ("aura_cg_mod_" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir.string();
}
std::string writeCgModAura(const std::string& dir, const std::string& name,
                           const std::string& content) {
    std::string path = (std::filesystem::path(dir) / name).string();
    std::ofstream f(path);
    f << content;
    return path;
}
// 多文件 CodeGen：加载模块图 → 逐模块 Sema（保持 SemAnalyzer 存活到 codegen 之后！
// Sema 将 AST inferredType 设为 analyzer typeStore_ 的指针，analyzeModuleGraph 返回即
// 销毁 analyzer → 悬垂；main.cpp 同样以 moduleSemas 存活期间做 codegen）→ 为入口模块
// 构造 crossDefaults/crossModuleParamSemTypes（与 main.cpp compileMultiFile 的 CodeGen
// 流程一致）→ generate。返回入口模块 CompileUnit。
Aura::CompileUnit compileMultiModuleCg(const std::string& entryPath,
                                       Aura::DiagnosticEngine& diag) {
    Aura::CompileUnit unit;
    Aura::ModuleManager mgr(diag);
    mgr.loadBuiltinAurai();
    if (!mgr.loadAll(entryPath)) return unit;
    if (mgr.hasCycle()) return unit;
    std::map<std::string, std::unique_ptr<Aura::SemAnalyzer>> moduleSemas;
    auto layers = mgr.topologicalLayers();
    std::map<std::string, std::unique_ptr<Aura::DiagnosticEngine>> moduleDiags;
    for (auto& layer : layers) {
        std::vector<Aura::ModuleInfo*> tasks;
        for (auto* mod : layer) {
            if (mod->isBuiltin || !mod->ast) continue;
            tasks.push_back(mod);
            auto modDiag = std::make_unique<Aura::DiagnosticEngine>();
            modDiag->setSourceView(Aura::readFile(mod->sourcePath));
            modDiag->setFileName(mod->sourcePath);
            moduleDiags[mod->sourcePath] = std::move(modDiag);
        }
        for (auto* mod : tasks) {
            auto sema = std::make_unique<Aura::SemAnalyzer>(*moduleDiags[mod->sourcePath]);
            for (auto& depPath : mod->deps) {
                auto depIt = mgr.modules().find(depPath);
                if (depIt == mgr.modules().end()) continue;
                std::string alias;
                for (auto& imp : mod->imports)
                    if (imp.path == depPath) { alias = imp.alias; break; }
                sema->importExports(alias, depIt->second.exports);
            }
            (void)sema->analyze(*mod->ast);
            mod->exports = sema->extractExports();
            moduleSemas[mod->sourcePath] = std::move(sema);
        }
        for (auto* mod : tasks) diag.mergeFrom(*moduleDiags[mod->sourcePath]);
    }
    if (diag.hasErrors()) return unit;
    const Aura::ModuleInfo* entry = nullptr;
    for (auto& [p, m] : mgr.modules())
        if (m.hasMain) { entry = &m; break; }
    if (!entry) return unit;
    diag.setSourceView(Aura::readFile(entry->sourcePath));
    diag.setFileName(entry->sourcePath);
    Aura::CodeGenerator::CrossModuleDefaults crossDefaults;
    Aura::CodeGenerator::CrossModuleParamSemTypes crossParamSemTypes;
    std::vector<Aura::CodeGenImport> cgImports;
    for (auto& imp : entry->imports) {
        Aura::CodeGenImport ci;
        ci.path = imp.path; ci.alias = imp.alias; ci.isBuiltin = imp.isBuiltin;
        if (!imp.isBuiltin) {
            auto it = mgr.modules().find(imp.path);
            if (it == mgr.modules().end()) continue;
            ci.nsName = it->second.nsName; ci.modName = it->second.moduleName;
        }
        cgImports.push_back(ci);
        if (imp.isBuiltin) continue;
        auto it = mgr.modules().find(imp.path);
        if (it == mgr.modules().end()) continue;
        std::string nsKey = imp.alias.empty() ? it->second.moduleName : imp.alias;
        auto& modDefaults = crossDefaults[nsKey];
        auto& modSemTypes = crossParamSemTypes[nsKey];
        for (auto& [fnName, f] : it->second.exports.funcs) {
            std::vector<const Aura::ASTNode*> defaults(f.params.size(), nullptr);
            std::vector<const Aura::SemType*> pts(f.params.size(), nullptr);
            bool any = false;
            for (size_t i = 0; i < f.params.size(); ++i) {
                if (f.params[i].defaultExpr) { defaults[i] = f.params[i].defaultExpr.get(); any = true; }
                pts[i] = f.params[i].type.get();
            }
            if (any) modDefaults[fnName] = std::move(defaults);
            modSemTypes[fnName] = std::move(pts);
        }
    }
    Aura::CodeGenerator cg(diag);
    // moduleSemas / mgr / moduleDiags 均存活至 generate 返回（AST inferredType 指针安全）
    return cg.generate(*entry->ast, entry->moduleName, cgImports, entry->nsName,
                       Aura::CodeGenConfig(), crossDefaults, crossParamSemTypes);
}
} // namespace

TEST(CodeGen, CrossModuleGenericDefaultClosureMaterialized) {
    // bug-06 主线：跨模块泛型函数默认参数闭包引用函数模板 T，缺 cb 调用
    // m.useT(5,10) → 补默认闭包 fun(x:T)->T：T 物化为 int32_t（普通 lambda）+ std::function 包装
    auto dir = cgModTempDir();
    writeCgModAura(dir, "mod_main.aura",
        "pub fun useT(inc: <T>, v: T, cb: fun(T) -> T = fun(x: T) -> T { return x }) -> T {\n"
        "    return cb(v)\n"
        "}\n");
    std::string entry = writeCgModAura(dir, "main.aura",
        "import \"mod_main.aura\" as m\n"
        "fun main(io: Io) { let r = m.useT(5, 10); io.println(\"useT result: \" + str(r)) }\n");
    Aura::DiagnosticEngine diag;
    auto unit = compileMultiModuleCg(entry, diag);
    EXPECT_FALSE(diag.hasErrors());
    // 物化 + CallableObj 包装：默认闭包经 IIFE gc_alloc_callable 分配（T→int32_t）
    EXPECT_CONTAINS(unit.impl,
        "aura_rt::CallableObj<int32_t, int32_t>*([&]() -> aura_rt::CallableObj<int32_t, int32_t>* {");
    // 不得残留模板 lambda（T 应被物化剔除）
    EXPECT_NOT_CONTAINS(unit.impl, "[]<typename T>");
    std::filesystem::remove_all(dir);
}

TEST(CodeGen, CrossModuleGenericExplicitClosureWrapped) {
    // bug-06 显式全实参：m.useT(5, 10, fun(x:int)->int{...}) → 具体闭包包 std::function
    auto dir = cgModTempDir();
    writeCgModAura(dir, "mod_main.aura",
        "pub fun useT(inc: <T>, v: T, cb: fun(T) -> T = fun(x: T) -> T { return x }) -> T {\n"
        "    return cb(v)\n"
        "}\n");
    std::string entry = writeCgModAura(dir, "main.aura",
        "import \"mod_main.aura\" as m\n"
        "fun main(io: Io) { let r = m.useT(5, 10, fun(x: int) -> int { return x * 3 }); io.println(str(r)) }\n");
    Aura::DiagnosticEngine diag;
    auto unit = compileMultiModuleCg(entry, diag);
    EXPECT_FALSE(diag.hasErrors());
    // 显式闭包实参经 IIFE gc_alloc_callable 分配（具体 FuncSemType）
    EXPECT_CONTAINS(unit.impl, "auto _a0_2 = ([&]() -> aura_rt::CallableObj<int32_t, int32_t>* {");
    std::filesystem::remove_all(dir);
}

TEST(CodeGen, CrossModuleGenericDefaultClosureString) {
    // bug-06 string 实例化：m.useT(\"a\", \"b\") → T 物化为 aura_rt::GcString*
    auto dir = cgModTempDir();
    writeCgModAura(dir, "mod_main.aura",
        "pub fun useT(inc: <T>, v: T, cb: fun(T) -> T = fun(x: T) -> T { return x }) -> T {\n"
        "    return cb(v)\n"
        "}\n");
    std::string entry = writeCgModAura(dir, "main.aura",
        "import \"mod_main.aura\" as m\n"
        "fun main(io: Io) { let r = m.useT(\"a\", \"b\"); io.println(r) }\n");
    Aura::DiagnosticEngine diag;
    auto unit = compileMultiModuleCg(entry, diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl,
        "aura_rt::CallableObj<aura_rt::GcString*, aura_rt::GcString*>*([&]() -> aura_rt::CallableObj<aura_rt::GcString*, aura_rt::GcString*>* {");
    std::filesystem::remove_all(dir);
}

TEST(CodeGen, SameModuleGenericDefaultClosureNotRegressed) {
    // bug-06 同模块对照（M3 已修）：单模块 compileSource 路径不得被跨模块改动破坏
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun useT(inc: <T>, v: T, cb: fun(T) -> T = fun(x: T) -> T { return x }) -> T { return cb(v) }"
        " fun main(io: Io) { let r = useT(5, 10); io.println(str(r)) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    // feature-06：默认闭包实参 IIFE 分配（调用点内联 CallableObj 闭包）
    EXPECT_CONTAINS(unit.impl, "useT(5, 10, [&]() -> aura_rt::CallableObj<int32_t, int32_t>* {");
}

TEST(CodeGen, CrossModuleOptionalParamBoxed) {
    // bug-06 附注 3：跨模块函数 Optional 形参 + 裸值实参（isNs 调用的 mpIt 装箱不命中
    // methodParamCppTypes_）→ 经 crossModuleParamSemTypes_ 形参 SemType 复用 genParamBoxing
    // 生成 make_optional<int32_t>(5)，否则裸 int 直传 Optional<int32_t>* 形参坏 C++。
    auto dir = cgModTempDir();
    writeCgModAura(dir, "mod_opt.aura",
        "pub fun retOpt(o: Optional<int>) -> Optional<int> { return o }\n");
    std::string entry = writeCgModAura(dir, "main.aura",
        "import \"mod_opt.aura\" as m\n"
        "fun main(io: Io) { let r = m.retOpt(5); io.println(str(r)) }\n");
    Aura::DiagnosticEngine diag;
    auto unit = compileMultiModuleCg(entry, diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "make_optional<int32_t>(");
    std::filesystem::remove_all(dir);
}

TEST(CodeGen, SameModuleNonCtorGenericOptionalBoxed) {
    // bug-48 泄漏点 B：同模块非 ctor 泛型函数 o: Optional<T> 形参 + 裸值实参
    // takeFn(5,9,7) → fnCallMat 物化 {T:int32_t} → make_optional<int32_t>(7)，
    // 不得泄漏 make_optional<T>（修复前 fnParamCppTypes_ A 遍裸 T 直用）。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun takeFn(inc: <T>, v: T, o: Optional<T>) -> int { return 1 }"
        " fun main(io: Io) { let r = takeFn(5, 9, 7); io.println(str(r)) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "make_optional<int32_t>(");
    EXPECT_NOT_CONTAINS(unit.impl, "make_optional<T>(");
}

TEST(CodeGen, CrossModuleGenericOptionalBoxedNoNone) {
    // bug-48 泄漏点 A：跨模块泛型 o: Optional<T> 形参 + 裸值实参（函数体无 none()，
    // 规避既有「泛型体内裸 none() 元素推断」独立缺口）→ cmMat 物化 + paramCpp 裸词
    // 替换 → make_optional<int32_t>(7)。
    auto dir = cgModTempDir();
    writeCgModAura(dir, "mod_opt_gen.aura",
        "pub fun takeOpt(inc: <T>, v: T, o: Optional<T>) -> int {\n"
        "    return 1\n"
        "}\n");
    std::string entry = writeCgModAura(dir, "main.aura",
        "import \"mod_opt_gen.aura\" as m\n"
        "fun main(io: Io) { let r = m.takeOpt(5, 9, 7); io.println(str(r)) }\n");
    Aura::DiagnosticEngine diag;
    auto unit = compileMultiModuleCg(entry, diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "make_optional<int32_t>(");
    EXPECT_NOT_CONTAINS(unit.impl, "make_optional<T>(");
    std::filesystem::remove_all(dir);
}

// ============================================================
// 批次 9：#56 方法 this 入口保护 / #57 无标注 let 泛型 record / #52 构造闭包 self
// ============================================================

TEST(CodeGen, Batch56MethodThisEntryRoot) {
    // #56：非协程方法体入口 _this（ThreadLocal）——self 直引生成 "_this.get()"
    //（接口默认方法/视图不触发，见既有 InterfaceDefaultMethodClosureRefsSelfCapturesThis）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int }"
        " fun (self Point) get() -> int { gc_force(); return self.x }"
        " fun main(io: Io) { let p: Point = { x = 3 }; io.println(str(p.get())) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::GcRootHandle<Point*> _this(this, aura_rt::GcRootScope::ThreadLocal);");
    EXPECT_CONTAINS(unit.impl, "return _this.get()->x;");
    // 入口包裹必须先于 gc_force（否则 compact 后 this 悬垂）
    size_t rootPos = unit.impl.find("_this(this, aura_rt::GcRootScope::ThreadLocal)");
    size_t gcPos = unit.impl.find("gc_force_major()");
    EXPECT_TRUE(rootPos != std::string::npos);
    EXPECT_TRUE(gcPos != std::string::npos);
    EXPECT_TRUE(rootPos < gcPos);
}

TEST(CodeGen, Batch56CoroMethodThisGlobalRoot) {
    // #56：协程方法（io.println → co_await）入口 _this 用 Global（帧跨挂起）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Calc = { v: int }"
        " fun (self Calc) compute(io: Io) -> int { io.println(\"c\"); gc_force(); return self.v }"
        " fun main(io: Io) throws { let c: Calc = { v = 3 }; io.println(str(c.compute(io))) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::GcRootHandle<Calc*> _this(this, aura_rt::GcRootScope::Global);");
    EXPECT_CONTAINS(unit.impl, "return _this.get()->v;");
}

TEST(CodeGen, Batch56SpawnExplicitParamSelfSpThis) {
    // #56 §1.8 修改点 1：spawn 显式传参形态（方法内 spawn (self, io) 引用 self）→
    // 手拼 lambda init-capture _sp_this（Global），body self → 物化句柄 "_sp_this_f.get()"
    //（缺口 1：协程 lambda init-capture 的 GcRootHandle 存在 g++ 暂存窗口 → task 体
    //  首语句物化 frame-local 句柄，体内映射恒指向物化句柄）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Counter = { inc: int }"
        " fun (self Counter) run(io: Io) throws {"
        "   sync { spawn (self, io) { self.inc = self.inc + 1; io.println(str(self.inc)) } } }"
        " fun main(io: Io) throws { let c: Counter = { inc = 41 }; c.run(io) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    // capture-init 源取方法入口句柄 _this.get()（缺口 3：不写裸 this）
    EXPECT_CONTAINS(unit.impl,
        "_sp_this = aura_rt::GcRootHandle<Counter*>(_this.get(), aura_rt::GcRootScope::Global)");
    // 缺口 1：task body 首语句物化 frame-local 句柄（_sp_this_f）
    EXPECT_CONTAINS(unit.impl,
        "aura_rt::GcRootHandle<Counter*> _sp_this_f(_sp_this.get(), aura_rt::GcRootScope::Global);");
    EXPECT_CONTAINS(unit.impl, "_sp_this_f.get()->inc");
    // 不得退化为裸 [this]（body self 映射 _this.get() 时 _this 不可见 → 坏 C++）
    EXPECT_NOT_CONTAINS(unit.impl, "push_back([this]");
}

TEST(CodeGen, Batch56SpawnCallFormSelfSpThis) {
    // #56 §1.8 修改点 2：spawn 调用形态（方法内 spawn self.method(...)）→ _sp_this
    // init-capture（源 _this.get()）+ body 物化 _sp_this_f（缺口 1 同款）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Counter = { inc: int, sum: int }"
        " fun (self Counter) add(v: int) { self.sum = self.inc + v }"
        " fun (self Counter) run(io: Io) throws { sync { spawn self.add(5) } }"
        " fun main(io: Io) throws { let c: Counter = { inc = 40, sum = 0 }; c.run(io) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl,
        "_sp_this = aura_rt::GcRootHandle<Counter*>(_this.get(), aura_rt::GcRootScope::Global)");
    EXPECT_CONTAINS(unit.impl,
        "aura_rt::GcRootHandle<Counter*> _sp_this_f(_sp_this.get(), aura_rt::GcRootScope::Global);");
    // 调用实参（接收者）经物化句柄求值（genGcRootedArgs 包裹 → _h0_0.get()->add）
    EXPECT_CONTAINS(unit.impl, "auto _a0_0 = (_sp_this_f.get());");
    EXPECT_CONTAINS(unit.impl, "return _h0_0.get()->add(_a0_1);");
}

TEST(CodeGen, Batch56SyncThreadForSelfSpThis) {
    // #56 §1.8 修改点 3：sync thread for（StmtSync 线程版）引用 self → _sp_this init-capture
    //（普通 lambda 非协程，无缺口 1 暂存窗口 → 不物化，body 直接映射 _sp_this.get()）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Counter = { base: int }"
        " fun (self Counter) run(ch: sync.Channel<int>) {"
        "   sync thread for i in range(10) { ch.send(self.base + i) }"
        "   ch.close() }"
        " fun main(io: Io) { let c: Counter = { base = 100 }"
        "   let ch: sync.Channel<int> = sync.Channel(20); c.run(ch) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "_stx.submit([_sp_this = aura_rt::GcRootHandle<Counter*>(_this.get(), aura_rt::GcRootScope::Global)");
    EXPECT_CONTAINS(unit.impl, "_sp_this.get()->base");
    EXPECT_NOT_CONTAINS(unit.impl, "submit([this");
}

TEST(CodeGen, Batch56SyncForCoroSelfSpThisMaterialized) {
    // 缺口 2b（StmtSync 协程版 sync for 落实 §1.8）：协程方法内 sync for 引用 self →
    // init-capture _sp_this（源 _this.get()）+ body 首语句物化 _sp_this_f（缺口 1 同款）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Counter = { base: int }"
        " fun (self Counter) run(io: Io, ch: channel<int>) throws {"
        "   sync for i in range(3) { ch.send(self.base + i) }"
        "   ch.close() }"
        " fun main(io: Io) throws { let c: Counter = { base = 100 }"
        "   let ch: channel<int> = channel(10); c.run(io, ch) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl,
        "push_back([_sp_this = aura_rt::GcRootHandle<Counter*>(_this.get(), aura_rt::GcRootScope::Global)](auto i,");
    EXPECT_CONTAINS(unit.impl,
        "aura_rt::GcRootHandle<Counter*> _sp_this_f(_sp_this.get(), aura_rt::GcRootScope::Global);");
    EXPECT_CONTAINS(unit.impl, "_sp_this_f.get()->base");
    EXPECT_NOT_CONTAINS(unit.impl, "push_back([this]");
}

TEST(CodeGen, Batch56ThreadSpawnCallFormSelfSpThis) {
    // 缺口 2a（genSpawnCallAsThread 落实 §1.8）：sync thread 块内 spawn self.method(...)
    // → init-capture _sp_this（线程版普通 lambda，不物化，body 直接映射 _sp_this.get()）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Counter = { inc: int, sum: int }"
        " fun (self Counter) work(v: int) { self.sum = self.inc + v }"
        " fun (self Counter) run(io: Io) { sync thread { spawn self.work(5) } }"
        " fun main(io: Io) { let c: Counter = { inc = 40, sum = 0 }; c.run(io) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl,
        "_stx.submit([_sp_this = aura_rt::GcRootHandle<Counter*>(_this.get(), aura_rt::GcRootScope::Global)]");
    // 接收者经捕获句柄求值（genGcRootedArgs 包裹 → _h0_0.get()->work）
    EXPECT_CONTAINS(unit.impl, "auto _a0_0 = (_sp_this.get());");
    EXPECT_NOT_CONTAINS(unit.impl, "submit([this");
}

TEST(CodeGen, Batch56NestedSpawnInitUsesOuterHandle) {
    // 缺口 3（嵌套 spawn capture-init 引用裸 this）：方法内 spawn (self, io) 体内再
    // spawn self.work → 内层 init-capture 源取外层物化句柄 _sp_this_f.get()（非裸 this，
    // 外层 lambda 未捕获 this → 裸 this 坏 C++）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Counter = { inc: int }"
        " fun (self Counter) work(v: int) { self.inc = self.inc + v }"
        " fun (self Counter) run(io: Io) throws {"
        "   sync { spawn (self, io) {"
        "     sync { spawn self.work(1) }"
        "     io.println(str(self.inc)) } } }"
        " fun main(io: Io) throws { let c: Counter = { inc = 41 }; c.run(io) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    // 外层 spawn init-capture（方法体直引 → _this.get()）
    EXPECT_CONTAINS(unit.impl,
        "_sp_this = aura_rt::GcRootHandle<Counter*>(_this.get(), aura_rt::GcRootScope::Global)");
    // 内层 spawn init-capture 源 = 外层物化句柄（缺口 3 修复核心）
    EXPECT_CONTAINS(unit.impl,
        "_sp_this = aura_rt::GcRootHandle<Counter*>(_sp_this_f.get(), aura_rt::GcRootScope::Global)");
    // 内层 body 亦物化自身句柄
    EXPECT_CONTAINS(unit.impl,
        "aura_rt::GcRootHandle<Counter*> _sp_this_f(_sp_this.get(), aura_rt::GcRootScope::Global);");
    // 不得在内层 capture-init 出现裸 this（this 在外层 lambda 不可见）
    EXPECT_NOT_CONTAINS(unit.impl, "GcRootHandle<Counter*>(this, aura_rt::GcRootScope::Global)");
}

TEST(CodeGen, Batch56ThreadSpawnExplicitReceiverSelf) {
    // 排查落点：sync thread 块内 spawn (self, io) 显式传参引用 self（genSpawnAsThread）
    // → receiver 参数不捕获裸名（外层无 self 变量），改 init-capture _sp_this + body 映射
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Counter = { inc: int, sum: int }"
        " fun (self Counter) work(v: int) { self.sum = self.inc + v }"
        " fun (self Counter) run(io: Io) {"
        "   sync thread { spawn (self, io) { self.work(1) } } }"
        " fun main(io: Io) { let c: Counter = { inc = 40, sum = 0 }; c.run(io) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl,
        "_stx.submit([_sp_this = aura_rt::GcRootHandle<Counter*>(_this.get(), aura_rt::GcRootScope::Global)");
    EXPECT_CONTAINS(unit.impl, "_sp_this.get()->work(1)");
    // 不得捕获裸 [self]（外层无此 C++ 变量 → 坏 C++）
    EXPECT_NOT_CONTAINS(unit.impl, "submit([self");
    EXPECT_NOT_CONTAINS(unit.impl, "submit([this");
}

TEST(CodeGen, Batch56SyncThreadBodyBuiltinNotCaptured) {
    // 关联调研发现 (a)：sync thread for / spawn 体顶层调用内置函数名（gc_force 等）→
    // freeVars 收集器此前仅排除类型名，gc_force 被当自由变量值捕获 [.., gc_force] →
    // g++ 未声明。修复：收集器排除 BuiltinRegistry 函数名
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Counter = { inc: int }"
        " fun (self Counter) bump(v: int) { self.inc = self.inc + v }"
        " fun main(io: Io) {"
        "   let c: Counter = { inc = 0 }"
        "   sync thread for i in range(3) { gc_force(); c.bump(i) }"
        "   io.println(str(c.inc)) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    // gc_force 不进捕获列表（bug-72 起 GC 根 c 为 Global 根 init-capture；仍无 gc_force）
    EXPECT_CONTAINS(unit.impl,
        "_stx.submit([i, c = aura_rt::GcRootHandle<Counter*>(c.get(), "
        "aura_rt::GcRootScope::Global)]() mutable");
    EXPECT_NOT_CONTAINS(unit.impl, "gc_force]()");
    // 调用点直转 runtime
    EXPECT_CONTAINS(unit.impl, "aura_rt::gc_force_major();");
}

TEST(CodeGen, Batch57UnannotatedLetGenericRecordRootConcrete) {
    // #57 主案：无标注 let 接 Tree<string> 实例 → canonicalName 含 '<' 放行
    // "Tree<...>*" → _raw + GcRootHandle（修复前裸 auto 无根）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Tree<T> = { value: T, children: [Tree<T>] }"
        " fun build() -> Tree<string> {"
        "   let leaf: Tree<string> = { value = \"leaf\", children = [] }"
        "   return { value = \"root\", children = [leaf] } }"
        " fun main(io: Io) { let t = build(); gc_force(); io.println(t.value) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "Tree<aura_rt::GcString*>* t_raw = build();");
    EXPECT_CONTAINS(unit.impl, "aura_rt::GcRootHandle<Tree<aura_rt::GcString*>*> t(t_raw, aura_rt::GcRootScope::ThreadLocal);");
}

TEST(CodeGen, Batch57UnannotatedLetGenericRecordRootGenericCtx) {
    // #57 泛型上下文：模板方法内 let t = self.build()（返回 Tree<T>，canonicalName 含
    // 模板形参 '<'）→ 放行 "Tree<T>*" → _raw + GcRootHandle<Tree<T>*>（模板上下文合法）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Tree<T> = { value: T, children: [Tree<T>] }"
        " type Box<T> = { val: T }"
        " fun (self Box<T>) build() -> Tree<T> { return { value = self.val, children = [] } }"
        " fun (self Box<T>) top() -> T { let t = self.build(); gc_force(); return t.value }"
        " fun main(io: Io) throws { let b: Box<string> = { val = \"root\" }; io.println(b.top()) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    std::string all = unit.header + "\n" + unit.impl;
    EXPECT_CONTAINS(all, "Tree<T>* t_raw = ");
    EXPECT_CONTAINS(all, "aura_rt::GcRootHandle<Tree<T>*> t(t_raw, aura_rt::GcRootScope::ThreadLocal);");
}

TEST(CodeGen, Batch52CtorSelfRootAndClosureInitCapture) {
    // #52：ctor self → _raw + GcRootHandle + gcRootVarNames_ 注册（闭包自动 init-capture
    // GcRootHandle<fullType*>(self.get(), Global)；返回 self.get()）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Counter = { inc: int, add: fun(int) -> int }"
        " fun (self Counter) Counter(inc: int) {"
        "   self.inc = inc"
        "   self.add = fun(x: int) -> int { return x + self.inc } }"
        " fun main(io: Io) { let c = Counter(10); io.println(str(c.add(5))) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "Counter* self_raw = aura_rt::gc_alloc<Counter>(&Counter::_desc);");
    EXPECT_CONTAINS(unit.impl, "aura_rt::GcRootHandle<Counter*> self(self_raw, aura_rt::GcRootScope::ThreadLocal);");
    // 闭包捕获 self → CallableObj 捕获槽 cap_self（填槽源 self.get()，desc offsetof 追踪）
    EXPECT_CONTAINS(unit.impl, "Counter* cap_self;");
    EXPECT_CONTAINS(unit.impl, "__o_h.get()->cap_self = self.get();");
    EXPECT_CONTAINS(unit.impl, "self.get()->inc");
    EXPECT_CONTAINS(unit.impl, "return self.get();");
    // 不得退化为裸 [self] 捕获（悬垂面）
    EXPECT_NOT_CONTAINS(unit.impl, "[self](int32_t x)");
}

// ============================================================
// 批次 11（#31 / #45 / #46）CodeGen 文本锚测试
// ============================================================
TEST(CodeGen, Batch11CoroOuterHoistBeforeLetReturn) {
    // #31 形态 A/B：协程调用 / co_await 实参置于 return / let 表达式上下文时，
    // genGcRootedArgs 的 outer 前缀（auto _aX_Y = ...）必须落盘为承接语句之前的
    // 独立语句（修复前拼入表达式 → `co_return auto _a1_0 = ...` / `int32_t r = auto
    // _a4_1 = (cb);` 坏 C++；方案 P 后 outer 入 hoistPrefixPending_ 由语句边界落盘）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Transform = fun(int) -> int"
        " type Box = { value: int }"
        " fun (self Box) apply(cb: Transform, ch: channel<int>) -> int {"
        "   return cb(ch.receive()) }"
        " fun main(io: Io) {"
        "   let b: Box = { value = 42 }"
        "   let cb: Transform = fun(x: int) -> int { return x + 1 }"
        "   let ch: channel<int> = channel(10)"
        "   sync { spawn (ch: channel<int>) {\n ch.send(100)\n ch.close()\n }(ch) }"
        "   let r = b.apply(cb, ch)"
        "   io.println(\"r=\" + str(r)) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 形态 A：co_await 承接 lambda（Box::apply 内 ch.receive()）先于其实参提升前缀声明
    size_t pA = unit.impl.find("co_await [&]() -> auto {");
    size_t pR = unit.impl.find("auto _a0_0 = (ch.get());");
    EXPECT_TRUE(pA != std::string::npos && pR != std::string::npos && pA < pR);
    // 形态 B：非堆实参 cb 的前缀声明（auto _aX_Y = (cb);）落盘于 co_await 承接
    // lambda 体内、先于实际使用行（return ...apply(...)）；cb 为函数类型形参 →
    // CallableObj invoke 接线（feature-06）
    // feature-07 Step 3：调用点编号随 G6 守卫（Box::apply 内 cb(...) 的 `_cb0`
    // 物化）新增而位移：_h5_* -> _h4_*。断言「非堆实参前缀声明先于实际使用行」。
    size_t pB = unit.impl.find("auto _a4_1 = (cb);");
    size_t pL = unit.impl.find("return _h4_0.get()->apply(_h4_1.get(), _h4_2.get());");
    EXPECT_TRUE(pB != std::string::npos && pL != std::string::npos && pB < pL);
    // Box::apply 体内回调 cb(...) 走 G6 加固形态（callee 单次物化 `_cb0`）
    EXPECT_CONTAINS(unit.impl, "auto* _cb0 = (cb);");
    // 坏 C++ 特征（outer 前缀被拼入 let/return 前缀表达式）不得出现
    EXPECT_NOT_CONTAINS(unit.impl, "co_return auto ");
    EXPECT_NOT_CONTAINS(unit.impl, "= auto _a");
}

TEST(CodeGen, Batch11CoroOuterExprStmtNoBadCpp) {
    // #31 表达式语句上下文对照（control_coro_outer_branch_int 等价）：协程调用作独立
    // 语句（丢弃结果）→ genExprStmt 整串写行，不得生成拼入前缀的坏 C++（修复前后
    // 均正常，验证 flush 落盘不引入孤立前缀）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Transform = fun(int) -> int"
        " type Box = { value: int }"
        " fun (self Box) apply(cb: Transform, ch: channel<int>) -> int {"
        "   return cb(ch.receive()) }"
        " fun main(io: Io) {"
        "   let b: Box = { value = 42 }"
        "   let cb: Transform = fun(x: int) -> int { return x + 1 }"
        "   let ch: channel<int> = channel(10)"
        "   sync { spawn (ch: channel<int>) {\n ch.send(100)\n ch.close()\n }(ch) }"
        "   b.apply(cb, ch)"
        "   io.println(\"done\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_NOT_CONTAINS(unit.impl, "= auto _a");
    EXPECT_NOT_CONTAINS(unit.impl, "co_return auto ");
}

TEST(CodeGen, Batch11SyncWhenAllNoStdMove) {
    // #45：无界 sync 收尾 when_all 引用收参——生成 when_all(_tasks)（无 std::move）。
    // 修复前 when_all(std::move(_tasks)) 按值 move 后，外层 spawn 任务体内内层 spawn
    // push 落已 move-from 本地容器 → 内层任务孤儿化永不执行（文本锚；语义由端到端
    // 复现 repro_nested_spawn_same_name/_tmp_nested_io_probe sum=10/5 行打印验证）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {"
        "  sync { spawn (io: Io) { io.println(\"x\") } } }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "co_await aura_rt::when_all(_tasks);");
    EXPECT_NOT_CONTAINS(unit.impl, "when_all(std::move");
}

TEST(CodeGen, Batch46NoIoFnSpawnPureDataOmitsIoParam) {
    // #46 主案：无 io 形参函数内 sync spawn（闭包不用 io）→ 不生成 io 参数/实参
    //（修复前无条件追加 `aura_rt::Io& io` + 调用点传 io → 外层无 io 变量 g++ 坏 C++
    // 'io' was not declared；修复后按需追加 → 纯数据任务编译生成）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun worker(ch: channel<int>) {"
        "  sync { spawn (ch: channel<int>) { ch.send(1) }(ch) } }"
        " fun main(io: Io) {"
        "   let ch: channel<int> = channel(10)"
        "   worker(ch) }", diag);
    EXPECT_FALSE(diag.hasErrors());
    // 自动追加形态（引用参）与显式声明形态（值参）的 io 参数片段均不得出现
    EXPECT_NOT_CONTAINS(unit.impl, ", aura_rt::Io& io");
    EXPECT_NOT_CONTAINS(unit.impl, "aura_rt::Io io, ");
    // 调用点不得再传 io（旧 `}(ch.get(), io, _tasks));` 形态）
    EXPECT_NOT_CONTAINS(unit.impl, ", io, _tasks)");
}

TEST(CodeGen, Batch46ExplicitIoParamNoOuterIoCleanError) {
    // #46 兜底：spawn 闭包显式声明 io 参数（Sema 同名绑定跳过 io 放行）但所在函数无
    // io 形参 → CodeGen ioInScope_ 兜底干净报错（driver 不再生成坏 C++）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun worker() {"
        "  sync thread { spawn (io: Io) { io.println(\"x\") } } }"
        " fun main(io: Io) { worker() }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "requires an 'io'"));
}

TEST(CodeGen, Batch46SpawnIoKeepsParam) {
    // #46 不误伤：main(io) + sync 内 spawn 显式声明 io 参数（io.println）→ io 参数
    // 保持生成（有 io 的合法形态不得被按需追加逻辑误删）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {"
        "  sync { spawn (io: Io) { io.println(\"x\") } } }", diag);
    EXPECT_FALSE(diag.hasErrors());
    // 显式 io 参数（值参 aura_rt::Io io）保持追加在 spawn lambda 签名
    EXPECT_CONTAINS(unit.impl,
        "aura_rt::Io io, std::vector<aura_rt::task<void>>& _tasks");
}

// ============================================================
// bug-59 补修（#31 方案 P 直接流式位点残留：if/else-if/while 条件协程 outer flush 顺序）
// ============================================================
TEST(CodeGen, Batch59CoroOuterFlushBeforeIfWhile) {
    // bug-59：if / while 条件内协程调用 + 非堆实参时，genGcRootedArgs 的 outer 前缀
    //（auto _aX_Y = (实参);）必须先于 `if (`/`while (` 落盘为函数体内独立语句。
    // 修复前 flushHoistPrefix 在 "if ("/"while (" 输出【之后】执行 → if 位点生成
    // C++17 if-init 形态 `if (auto _aX = (cb);...`（作用域非预期）、while 位点生成
    // `while (auto _aX = (cb);...` 坏 C++（while 无 init-statement，g++ '_aX' was
    // not declared）。修复后条件头锚文本为 `if ((co_await`/`while ((co_await`，
    // outer 为其前独立语句（repro59_if_while_cond_coro_outer_flush 同构）。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Transform = fun(int) -> int"
        " type Box = { value: int }"
        " fun (self Box) apply(cb: Transform, ch: channel<int>) -> int {"
        "   return cb(ch.receive()) }"
        " fun main(io: Io) {"
        "   let b: Box = { value = 42 }"
        "   let cb: Transform = fun(x: int) -> int { return x + 1 }"
        "   let ch: channel<int> = channel(10)"
        "   sync { spawn (ch: channel<int>) {\n ch.send(100)\n ch.close()\n }(ch) }"
        "   if b.apply(cb, ch) > 40 { io.println(\"big\") }"
        "   let n: int = 0"
        "   while b.apply(cb, ch) < 100 { n = n + 1; if n > 2 { break } }"
        "   io.println(\"n=\" + str(n)) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 修复后 if 条件头：条件为纯表达式（无 outer 声明被拼入）
    size_t pIf = unit.impl.find("if ((co_await [&]() -> auto {");
    EXPECT_TRUE(pIf != std::string::npos);
    if (pIf != std::string::npos) {
        // outer 前缀（auto _aX_Y = ...）先于 if 条件头落盘，且为完整独立行（在 pIf 前换行结束）
        size_t outer = unit.impl.rfind("auto _a", pIf);
        size_t eol = outer == std::string::npos ? std::string::npos
                     : unit.impl.find('\n', outer);
        EXPECT_TRUE(outer != std::string::npos && eol != std::string::npos && eol < pIf);
    }
    // 修复后 while 条件头同款（修复前坏 C++ 形态 while (auto _aX = (cb);）
    size_t pWhile = unit.impl.find("while ((co_await [&]() -> auto {");
    EXPECT_TRUE(pWhile != std::string::npos);
    if (pWhile != std::string::npos) {
        size_t outer = unit.impl.rfind("auto _a", pWhile);
        size_t eol = outer == std::string::npos ? std::string::npos
                     : unit.impl.find('\n', outer);
        EXPECT_TRUE(outer != std::string::npos && eol != std::string::npos && eol < pWhile);
    }
    // 坏形态否定（outer 声明不得被拼入条件头）
    EXPECT_NOT_CONTAINS(unit.impl, "if (auto _a");
    EXPECT_NOT_CONTAINS(unit.impl, "while (auto _a");
}

TEST(CodeGen, Batch59CoroOuterFlushElseIfChain) {
    // bug-59 else-if 位点：else-if 无 init-statement（C++17 if-init 只支持 if）；
    // flush 若就地落盘于 `}` 与 `else if` 之间 → 声明语句插入链中 → g++ 'else'
    // without a previous 'if'（探针实证）。修复：if + 全部 else-if 条件文本先求值，
    // 统一在 if 链【之前】落盘 outer，链结构保持 `} else if (cond) {` 完整。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Transform = fun(int) -> int"
        " type Box = { value: int }"
        " fun (self Box) apply(cb: Transform, ch: channel<int>) -> int {"
        "   return cb(ch.receive()) }"
        " fun main(io: Io) {"
        "   let b: Box = { value = 42 }"
        "   let cb: Transform = fun(x: int) -> int { return x + 1 }"
        "   let ch: channel<int> = channel(10)"
        "   sync { spawn (ch: channel<int>) {\n ch.send(100)\n ch.close()\n }(ch) }"
        "   let v: int = b.apply(cb, ch)"
        "   if v > 100 { io.println(\"gt100\") }"
        "   else if b.apply(cb, ch) > 40 { io.println(\"big\") }"
        "   else { io.println(\"small\") }"
        "   io.println(\"done\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // else-if 条件头为纯表达式、链结构完整（修复前坏形态：outer 插入 } 与 else if 之间）
    EXPECT_CONTAINS(unit.impl, "} else if ((co_await [&]() -> auto {");
    // 坏形态否定：outer 声明不得被拼入条件头 / 插入 if 链中
    EXPECT_NOT_CONTAINS(unit.impl, "else if (auto _a");
    EXPECT_NOT_CONTAINS(unit.impl, "if (auto _a");
    EXPECT_NOT_CONTAINS(unit.impl, "}auto _a");
}

TEST(CodeGen, ConcreteOptionalNoneCmpMakeNoneInt) {
    // 具体作用域对照：o != none()（o: Optional<int>）→ make_none<int32_t>（修复顺带覆盖）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun cmp(o: Optional<int>) -> bool { return o != none() }"
        " fun main(io: Io) { let r = cmp(some(7)); io.println(str(r)) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::make_none<int32_t>");
}

// ============================================================
// bug-72（2026-09-10）：跨线程 GC 根捕获 UAF
//   CodeGen 在 spawn / sync 的**跨线程**捕获发射处对 GC 根变量做裸名按值捕获 →
//   GcRootHandle 副本在提交线程注册（thread-local 侵入式链表）、却在执行它的 worker
//   线程析构 → unregisterRootThreadLocal 用 worker 的 tl_roots_ 摘链（或无链表直接
//   return）→ 提交线程链表残留已析构节点 → 之后任意扫根 GC heap-use-after-free
//   （ASAN 实测：mark_sweep.cpp 扫根 READ UAF）。
//   修复：GC 根 / 视图根捕获改**同名 init-capture 的 Global 根**（注册/析构线程无关）；
//   协程 sync for 的自由变量经 lambda 形参承载（非 capture）→ 实参改传 Global 根临时值；
//   非 GC 根值捕获与 lambda 体内 .get() 形态保持不变。
// ============================================================
TEST(CodeGen, Bug72CrossThreadGcRootCaptureGlobalRoot) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun work(io: Io, p: Point) { io.println(str(p.x)) }"
        " fun main(io: Io) {"
        "   let p: Point = { x = 1, y = 2 }"
        "   gc_force()"
        "   sync thread(max = 2) {"
        "     spawn (io: Io, p: Point) { io.println(str(p.x)) }"
        "     spawn work(io, p)"
        "   }"
        "   gc_force()"
        "   sync for i in range(2) { io.println(str(p.x + i)) }"
        "   gc_force() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // spawn 块形态「同名自动绑定」+ 调用形态 freeVars → Global 根 init-capture（不再是裸名）
    EXPECT_CONTAINS(unit.impl,
        "[p = aura_rt::GcRootHandle<Point*>(p.get(), aura_rt::GcRootScope::Global), "
        "&io]() mutable");
    EXPECT_NOT_CONTAINS(unit.impl, "_stx.submit([p, &io]()");
    // 协程 sync for：形参 auto p 不变，实参改传 Global 根临时值（auto 推导同型）
    EXPECT_CONTAINS(unit.impl, "](auto i, auto p, aura_rt::Io& io,");
    EXPECT_CONTAINS(unit.impl,
        "}(i, aura_rt::GcRootHandle<Point*>(p.get(), aura_rt::GcRootScope::Global), "
        "io, _tasks));");
    // lambda 体内形态不变（同名 init-capture 遮蔽外层 → .get() 仍指向最新地址）
    EXPECT_CONTAINS(unit.impl, "p.get()->x");
}

// ============================================================
// bug-73：sync thread 块内调用形态 spawn 协程函数 → 返回的 lazy task 被
// submit(std::function<void()>) 擦除丢弃 → 协程体静默不执行。修复：worker 内
// 用 aura_rt::run_to_completion(...) 驱动至完成（仅在 callee 为协程时包装）。
// ============================================================

TEST(CodeGen, Bug73SyncThreadSpawnCoroutineCallRunToCompletion) {
    // 主案：被 spawn 的具名函数含 io 调用 → decideCoro 判 Coroutine（返回 task<void>）
    // → 线程版须包 run_to_completion（否则返回值被 std::function<void()> 擦除）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun work(io: Io, n: int) { io.println(\"plain n=\" + str(n)) }"
        " fun main(io: Io) {"
        "   let k = 7"
        "   sync thread(max = 2) { spawn work(io, k) }"
        "   io.println(\"A done\") }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "_stx.submit([k, &io]() mutable {");
    EXPECT_CONTAINS(unit.impl, "aura_rt::run_to_completion([&]() -> auto {");
}

TEST(CodeGen, Bug73SyncThreadSpawnCoroutineMethodRunToCompletion) {
    // 方法形态：同步线程块内 spawn obj.method(...)，方法体含 io → 协程方法
    //（coroutineFunctions_ 键 "Worker.job"）→ 同样须 run_to_completion 驱动
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Worker = { tag: int }"
        " fun (self Worker) job(io: Io, k: int) { io.println(\"m n=\" + str(k + self.tag)) }"
        " fun main(io: Io) { let w: Worker = { tag = 5 }"
        "   let k = 2"
        "   sync thread(max = 2) { spawn w.job(io, k) }"
        "   io.println(\"M done\") }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::run_to_completion(");
    EXPECT_CONTAINS(unit.impl, "->job(");
}

TEST(CodeGen, Bug73SyncThreadSpawnPlainCalleeNotWrapped) {
    // 对照：非协程 callee（无 io / 无挂起点）→ 保持原 submit 形态（不包
    // run_to_completion，避免对普通调用的返回值做 task 模板实例化）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun bump(v: int) -> int { return v + 1 }"
        " fun main(io: Io) { let k = 6"
        "   sync thread(max = 2) { spawn bump(k) }"
        "   io.println(\"P done\") }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "_stx.submit([k]() mutable {");
    EXPECT_NOT_CONTAINS(unit.impl, "run_to_completion");
}

TEST(CodeGen, Bug73CoroSyncBlockSpawnCallNotWrapped) {
    // 对照：协程 sync 块内调用形态 spawn 走 _tasks + when_all（task 被 co_await 等待）
    // → 不得额外包 run_to_completion（该路径修复前就正常）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun work(io: Io, n: int) { io.println(\"D plain n=\" + str(n)) }"
        " fun main(io: Io) { let k = 7"
        "   sync(max = 2) { spawn work(io, k) }"
        "   io.println(\"D done\") }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "_tasks.push_back(");
    EXPECT_NOT_CONTAINS(unit.impl, "run_to_completion");
}

// ============================================================
// feature-07 Step 3（A1/A2/A3 + G6 加固）CodeGen 文本锚测试
// ============================================================

TEST(CodeGen, Feature07Step3FunTypeParamCallableObjParamInvoke) {
    // A1/A2/A3：函数类型形参（`fun(int) -> int` 具名别名 / 内联函数类型）在函数 /
    // 方法体生成期分流摘出 callableParamIndices → 形参类型映射为
    // `aura_rt::CallableObj<R, A...>*`（别名 typedef 一并改写），形参名注册进
    // callableObjVars_；body 内直呼 f(x) 经 isFunValueCall 判定转 invoke 槽接线
    // （修复前走旧路径 std::function 直呼 → 坏 C++）。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Box = { value: int }"
        " type Transform = fun(Box) -> int"
        " fun use(f: Transform, b: Box) -> int {"
        "   return f(b) }"
        " fun main(io: Io) {"
        "   let b: Box = { value = 5 }"
        "   let g: Transform = fun(q: Box) -> int { return q.value + 1 }"
        "   let r = use(g, b)"
        "   io.println(\"r=\" + str(r)) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 别名 typedef → CallableObj 指针
    EXPECT_CONTAINS(unit.header, "using Transform = aura_rt::CallableObj<int32_t, Box*>*;");
    // 形参承载 CallableObj 指针（非 std::function）
    EXPECT_CONTAINS(unit.header, "int32_t use(Transform f, Box* b_raw)");
    EXPECT_NOT_CONTAINS(unit.header, "std::function");
    // body 内 f(b) → invoke 槽接线（callableObjVars_ 注册命中），实参经 GcRootHandle 保护
    EXPECT_CONTAINS(unit.impl, "f->invoke(f, _h0_0.get());");
    // 不得退化为直呼 f(...)（CallableObj 指针不可作函数调用）
    EXPECT_NOT_CONTAINS(unit.impl, "f(_h0_0.get())");
}

TEST(CodeGen, Feature07Step3G6CalleeSingleMaterialize) {
    // G6 加固（change.md §8 风险表第 9 行）：`callee->invoke(callee, args)` 双读
    // callee 时，实参若含 GC 触发点则裸指针快照可能搬移悬垂。实参全非堆类型 →
    // genGcRootedArgs 不产 IIFE → 就地生成守卫 IIFE：实参在调用点先求值，callee
    // 在 lambda 体内取一次（`_cbN`），两次使用之间无 GC 触发点；co_await 实参留在
    // 调用点（不进推导返回类型上下文）。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Transform = fun(int) -> int"
        " type Box = { value: int }"
        " fun (self Box) run(f: Transform, v: int) -> int { return f(v) }"
        " fun main(io: Io) {"
        "   let b: Box = { value = 1 }"
        "   let g: Transform = fun(x: int) -> int { return x * 2 }"
        "   let r = b.run(g, 21)"
        "   io.println(\"r=\" + str(r)) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 守卫 IIFE 形态：实参作 lambda 调用实参、callee 单次物化 `_cb0`
    EXPECT_CONTAINS(unit.impl,
        "[&](auto&&... _as) -> auto { auto* _cb0 = (f); "
        "return _cb0->invoke(_cb0, static_cast<decltype(_as)>(_as)...); }(v);");
    // 双读形态（callee 表达式出现两次）不得残留
    EXPECT_NOT_CONTAINS(unit.impl, "f->invoke(f, ");
}

// bug-75（2026-09-11）：协程 spawn lambda 形参从未注册 callableObjVars_
// 根因：全仓注册点仅 DeclFun.cpp:177/528/736 + ExprClosure.cpp:1367，spawn 体窗口空白
// 修法：StmtSpawn.cpp 在四个 spawn 形态的 lambda 体窗口内注册形参名 + 体后 saved 恢复
TEST(CodeGen, Bug75SpawnBodyFunParamCallInvoke) {
    // spawn 体内 fun 型形参直呼 f(k) → invoke 槽接线（修复前走旧路径直呼 → 坏 C++）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Transform = fun(int) -> int"
        " fun main(io: Io) {"
        "   let g: Transform = fun(x: int) -> int { return x * 7 }"
        "   sync {"
        "     spawn (f: Transform, k: int) {"
        "       let v = f(k)"
        "       io.println(str(v))"
        "     }(g, 6)"
        "   } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // spawn lambda 形参承载 CallableObj 指针（非 std::function）
    EXPECT_CONTAINS(unit.impl, "[](Transform f, int32_t k,");
    EXPECT_NOT_CONTAINS(unit.impl, "std::function");
    // 体内 f(k) → 守卫 IIFE 单次物化 callee 的 invoke 接线
    EXPECT_CONTAINS(unit.impl,
        "[&](auto&&... _as) -> auto { auto* _cb0 = (f); "
        "return _cb0->invoke(_cb0, static_cast<decltype(_as)>(_as)...); }(k);");
    // 不得退化为直呼 f(k)（CallableObj 指针不可作函数调用）
    EXPECT_NOT_CONTAINS(unit.impl, "f->invoke(f, ");
}

TEST(CodeGen, Bug75SpawnCallAsCoroBodyFunParamInvoke) {
    // 调用形态 spawn（genSpawnCallAsCoro）体内 fun 型形参调用 → 同样接线
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Transform = fun(int) -> int"
        " fun (self Counter) job(f: Transform, k: int) { let v = f(k) }"
        " type Counter = { n: int }"
        " fun main(io: Io) {"
        "   let g: Transform = fun(x: int) -> int { return x + 1 }"
        "   let c: Counter = { n = 0 }"
        "   sync { spawn c.job(g, 3) } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 调用形态体内 fun 型形参调用仍须 invoke 接线（不出现裸直呼）
    EXPECT_CONTAINS(unit.impl, "invoke(_cb0");
    EXPECT_NOT_CONTAINS(unit.impl, "std::function");
}