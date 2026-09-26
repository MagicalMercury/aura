// ============================================================
// test_codegen_coro.cpp — CodeGen 输出单元测试：协程判定 / 传播 / 事件循环 / 通道 send/for-in co_await
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

// ============================================================
// 协程判定
// ============================================================
TEST(CodeGen, PlainFunctionIsNotCoro) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource("fun add(a: int, b: int) -> int { return a + b }", diag);
    EXPECT_FALSE(diag.hasErrors());
    // 普通函数：非 task 返回
    EXPECT_NOT_CONTAINS(unit.impl, "aura_rt::task");
}

TEST(CodeGen, IoCallMakesCoro) {
    // io.println 是异步调用 → 生成协程 task
    Aura::DiagnosticEngine diag;
    auto unit = compileSource("fun main(io: Io) { io.println(\"x\") }", diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::task<void> aura_main");
    EXPECT_CONTAINS(unit.impl, "co_await");
}

TEST(CodeGen, SyncBlockMakesCoro) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) { sync { spawn (io: Io) { io.println(\"x\") } } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::task<void> aura_main");
}

// ============================================================
// 方法协程化（2026-08-29，problem.txt「方法体内异步操作未协程化」）
// 方法体内含异步操作（io / sync / spawn / channel receive）→ 方法签名包
// aura_rt::task<ret>（声明侧 struct 内嵌 + 定义侧一致），调用点 co_await
// ============================================================
TEST(CodeGen, MethodIoCallMakesCoro) {
    // 方法体内 io 异步调用 → 方法签名 task<void>（此前 void Point::say + co_await 坏 C++）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int }"
        " fun (self Point) Point(x: int) { self.x = x }"
        " fun (self Point) say(io: Io) { io.println(str(self.x)) }"
        " fun main(io: Io) { let p = Point(1); p.say(io) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // struct 内声明 + 定义侧签名均包 task
    EXPECT_CONTAINS(unit.header, "aura_rt::task<void> say(aura_rt::Io io);");
    EXPECT_CONTAINS(unit.impl, "aura_rt::task<void> Point::say(aura_rt::Io io)");
    // main 调用点 co_await
    EXPECT_CONTAINS(unit.impl, "co_await");
}

TEST(CodeGen, MethodSyncBlockMakesCoro) {
    // 方法体内 sync{spawn} → 方法签名 task<void>
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int }"
        " fun (self Point) Point(x: int) { self.x = x }"
        " fun (self Point) run(io: Io) {"
        "   sync { spawn (io: Io) { io.println(\"spawn\") } } }"
        " fun main(io: Io) { let p = Point(1); p.run(io) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "aura_rt::task<void> run(aura_rt::Io io);");
    EXPECT_CONTAINS(unit.impl, "aura_rt::task<void> Point::run(aura_rt::Io io)");
    // feature-14 P2 同步：sync 块任务等待由 when_all(_tasks) 改为本块 SyncContext
    //（SyncContextScope + requireSync()->addTask + co_await _ctx.wait_all()），
    // 本用例意图「方法体内 sync{spawn} 使方法成为协程」不变，故换锚到新等待形态。
    EXPECT_CONTAINS(unit.impl, "aura_rt::SyncContextScope _scope(");
    EXPECT_CONTAINS(unit.impl, "co_await _ctx.wait_all();");
    EXPECT_NOT_CONTAINS(unit.impl, "when_all");
}

TEST(CodeGen, GenericMethodIoCallMakesCoro) {
    // 泛型方法体内异步 → 模板方法签名 task<void>（定义入 header）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Box<T> = { v: T }"
        " fun (self Box<T>) Box(v: T) { self.v = v }"
        " fun (self Box<T>) runGen(io: Io) { io.println(str(self.v)) }"
        " fun main(io: Io) { let b: Box<int> = Box(5); b.runGen(io) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "aura_rt::task<void> runGen(aura_rt::Io io);");
    EXPECT_CONTAINS(unit.header, "aura_rt::task<void> Box<T>::runGen(aura_rt::Io io)");
    EXPECT_CONTAINS(unit.header, "co_await");
}

TEST(CodeGen, MethodCallCoroMethodCoAwait) {
    // 方法体内调用已标协程的方法（self.helper()）→ 传播标为协程 + 调用点 co_await
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int }"
        " fun (self Point) Point(x: int) { self.x = x }"
        " fun (self Point) helper(io: Io) { io.println(\"h\") }"
        " fun (self Point) outer(io: Io) { self.helper(io) }"
        " fun main(io: Io) { let p = Point(1); p.outer(io) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // outer 因调用协程方法 helper 被传播标为协程
    EXPECT_CONTAINS(unit.impl, "aura_rt::task<void> Point::outer(aura_rt::Io io)");
    EXPECT_CONTAINS(unit.impl, "co_await");
    // outer 体内调用 helper 生成 this->helper（co_await 前缀在 IIFE 外）
    EXPECT_CONTAINS(unit.impl, "->helper(");
}

TEST(CodeGen, MethodChannelReceiveMakesCoro) {
    // 方法内 channel receive（channel 为方法参数）→ 方法标协程 + co_await ch->receive
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int }"
        " fun (self Point) Point(x: int) { self.x = x }"
        " fun (self Point) recv(io: Io, ch: channel<int>) {"
        "   let v = ch.receive()"
        "   io.println(str(v)) }"
        " fun main(io: Io) {"
        "   let p = Point(1)"
        "   let ch: channel<int> = channel(10)"
        "   p.recv(io, ch) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::task<void> Point::recv(");
    EXPECT_CONTAINS(unit.impl, "co_await");
}

TEST(CodeGen, MethodCoroTaskIntReturn) {
    // 协程方法返回非 void：task<int32_t> + co_return（task<T> 只有 return_value）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Calc = { v: int }"
        " fun (self Calc) Calc(v: int) { self.v = v }"
        " fun (self Calc) compute(io: Io) -> int {"
        "   io.println(\"c\"); return self.v * 2 }"
        " fun main(io: Io) { let c = Calc(21); let r = c.compute(io) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "aura_rt::task<int32_t> compute(aura_rt::Io io);");
    EXPECT_CONTAINS(unit.impl, "aura_rt::task<int32_t> Calc::compute(aura_rt::Io io)");
    // #56：方法体入口 _this GcRootHandle（协程 Global），self → _this.get()
    EXPECT_CONTAINS(unit.impl, "aura_rt::GcRootHandle<Calc*> _this(this, aura_rt::GcRootScope::Global);");
    EXPECT_CONTAINS(unit.impl, "co_return (_this.get()->v * 2);");
}

TEST(CodeGen, NonCoroMethodStaysPlain) {
    // 回归红线：无异步的方法保持普通 void 签名，不包 task
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, y: int }"
        " fun (self Point) Point(x: int, y: int) { self.x = x; self.y = y }"
        " fun (self Point) moveBy(dx: int, dy: int) {"
        "   self.x = self.x + dx; self.y = self.y + dy }"
        " fun main(io: Io) { let p = Point(1, 2); p.moveBy(3, 4) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "void Point::moveBy(int32_t dx, int32_t dy)");
    // 非协程方法调用点直接生成 _h.get()->moveBy(...)（无 co_await 前缀；
    // 若误加 co_await，void 方法调用点会坏 C++，探针运行测试已覆盖）
    EXPECT_CONTAINS(unit.impl, "get()->moveBy(");
}

// ============================================================
// bug-02（2026-08-30）：decideCoro 后置声明协程传播（固定点迭代）
//  外层函数调用「后置声明」的协程函数 → 固定点迭代使外层标协程 → task<void> 签名
//  （修复前单遍扫描，outer 判 Plain → 调用点不 co_await → task 立即析构静默不执行）
// ============================================================
TEST(CodeGen, PosteriorCoroFunctionPropagates) {
    // 后置声明：outer 先声明，调用的 runner2 声明在其后
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun outer(io: Io) { runner2(io) }"
        " fun runner2(io: Io) { io.println(\"runner2 ran\") }"
        " fun main(io: Io) throws { io.println(\"main start\"); outer(io); io.println(\"main done\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // outer 因调用后置协程 runner2 被标为协程（task<void> 定义）
    EXPECT_CONTAINS(unit.impl, "aura_rt::task<void> outer(aura_rt::Io io)");
    EXPECT_CONTAINS(unit.impl, "co_await");
}

TEST(CodeGen, ForwardCoroFunctionStillPropagates) {
    // 对照组：协程 runner2 声明在前，outer 调用在后 → 不误伤，仍判协程
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun runner2(io: Io) { io.println(\"runner2 ran\") }"
        " fun outer(io: Io) { runner2(io) }"
        " fun main(io: Io) throws { io.println(\"main start\"); outer(io); io.println(\"main done\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::task<void> outer(aura_rt::Io io)");
}

TEST(CodeGen, CoroCycleMutualRecursionPropagates) {
    // 循环依赖形态：a↔b 互相调用，b 含 io.println（直接挂起点），a 仅调 b。
    // 固定点迭代第 1 轮 b 判协程，第 2 轮 a（调 b）判协程 → 两轮收敛，a/b 均 task<void>。
    // （区别于单向后置/前置传播：双向循环需多轮迭代才收敛）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun a(io: Io, n: int) { b(io, n) }"
        " fun b(io: Io, n: int) { io.println(\"b ran n=\" + str(n)); if n > 0 { a(io, n - 1) } }"
        " fun main(io: Io) throws { io.println(\"main start\"); a(io, 1); io.println(\"main done\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 循环链上 a、b 均判协程（定义侧 task<void>）
    EXPECT_CONTAINS(unit.impl, "aura_rt::task<void> a(aura_rt::Io io, int32_t n)");
    EXPECT_CONTAINS(unit.impl, "aura_rt::task<void> b(aura_rt::Io io, int32_t n)");
    // a 调用 b 生成 co_await（修复前 a 判 Plain → 无 co_await → 链断静默不执行）
    EXPECT_CONTAINS(unit.impl, "co_await");
}

// ============================================================
// bug-16（2026-08-30）：非协程 main 直接调用，不生成 run_event_loop
//  genMainEntry 按 coroutineFunctions_ 分派：main 无挂起点 → aura_main 返回 void
//  → footer 直接 `::aura_main(io); return 0;`（修复前恒走
//  `auto t = ::aura_main(io); run_event_loop(t);` → deduced type 'void' 坏 C++）
// ============================================================
TEST(CodeGen, NonCoroMainDirectCallNoEventLoop) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) { let x = 1 + 2; let y = x * 3; let s = \"sum:\" }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_TRUE(unit.hasMain);
    // 入口直接调用 aura_main（非协程返回 void），不包 auto t / run_event_loop
    EXPECT_CONTAINS(unit.footer, "::aura_main(io);");
    EXPECT_NOT_CONTAINS(unit.footer, "run_event_loop");
}

TEST(CodeGen, CoroMainKeepsRunEventLoop) {
    // 对照组：协程 main（io.println 异步）仍走 run_event_loop，不误伤
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) { io.println(\"x\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.footer, "aura_rt::run_event_loop(t);");
}

// ============================================================
// bug-22（2026-08-30）：sync.ThreadChannel 在协程 sync 块内 spawn 闭包 send 被 co_await void
// 根因：spawn 参数名与外层 let 同名 ch → channelVarNames_ 泄漏命中 isCoroChannel=true，
// 而 IterVarGuard 屏蔽 gcRootTypes_ → isSyncChannel 判定（只查 gcRootTypes_）失败 → send 被
// co_await void（ThreadChannel::send 返回 void 非 awaitable）→ 坏 C++。
// 修复：isSyncChannel 主判定改查 receiver inferredType（GenericSemType "sync.Channel"，Sema
// 填充不受 IterVarGuard 屏蔽），gcRootTypes_ 查 "ThreadChannel" 仅作兜底（inferredType 缺失时）；
// StmtControl.cpp genForStmt 同法改查 iterable inferredType。
// 断言：sync.Channel 的 spawn 同名参数 send 生成裸 `->send(`（不被 co_await 包裹）；
// 对照组协程 channel 同名参数 send 仍被 co_await（不误伤）。
// ============================================================

// 判定 cpp 中第一个 "->send(" 是否被 "co_await [&]() -> auto {" IIFE 包裹：
// 向前找最近的 co_await IIFE 头，若其与 send 之间无 "}();"（前一语句结束），则该 co_await
// 直接包裹 send 所在 IIFE → 被 co_await。
static bool firstSendWrappedInCoAwait(const std::string& cpp) {
    const std::string marker = "co_await [&]() -> auto {";
    auto sendPos = cpp.find("->send(");
    if (sendPos == std::string::npos) return false;
    auto pre = cpp.substr(0, sendPos);
    auto m = pre.rfind(marker);
    if (m == std::string::npos) return false;
    std::string between = pre.substr(m + marker.size());
    return between.find("}();") == std::string::npos;
}

TEST(CodeGen, SyncChannelSpawnParamSameNameSendNoCoAwait) {
    // bug-22 主线：协程 sync 块内 spawn 参数名 = 外层 let 名 ch + sync.ThreadChannel send
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {"
        " let ch: sync.Channel<int> = sync.Channel(10)"
        " sync {"
        "   spawn (ch: sync.Channel<int>) {"
        "     for i in range(5) { ch.send(i) }"
        "     ch.close()"
        "   }(ch)"
        " }"
        " io.println(\"sum\")"
        " io.println(\"done\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "->send(");
    // 修复点：isSyncChannel 主判定改查 inferredType → 裸 send（不生成 co_await 包裹）
    EXPECT_FALSE(firstSendWrappedInCoAwait(unit.impl));
}

TEST(CodeGen, CoroChannelSpawnParamSameNameSendCoAwait) {
    // 对照组：协程 channel<T> 的 spawn 同名参数 send 本就需 co_await（needAwait=true 是
    // 正确行为），修复 isSyncChannel 判定后不得误伤——仍生成 co_await 包裹。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {"
        " let ch: channel<int> = channel(10)"
        " sync {"
        "   spawn (ch: channel<int>) {"
        "     for i in range(5) { ch.send(i) }"
        "     ch.close()"
        "   }(ch)"
        " }"
        " io.println(\"sum\")"
        " io.println(\"done\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "->send(");
    EXPECT_TRUE(firstSendWrappedInCoAwait(unit.impl));  // 协程 channel send 仍 co_await
}

TEST(CodeGen, SyncChannelSpawnParamSameNameForInBlockingReceive) {
    // bug-22 双表现之二：spawn 闭包内 for-in sync.ThreadChannel 须回阻塞 receive 路径
    // （is_none/unwrap 对 Optional<T>* 合法）；修复后不落入协程 receive 路径（is_done +
    // co_await receive）。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {"
        " let ch: sync.Channel<int> = sync.Channel(10)"
        " sync {"
        "   spawn (ch: sync.Channel<int>) {"
        "     for v in ch { io.println(\"v=\" + v) }"
        "   }(ch)"
        " }"
        " io.println(\"done\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 阻塞 receive 循环（is_none 终止），非协程 receive 路径（is_done + co_await）
    EXPECT_CONTAINS(unit.impl, "is_none()) break");
    EXPECT_NOT_CONTAINS(unit.impl, "is_done()) break");
    EXPECT_NOT_CONTAINS(unit.impl, "co_await ch->receive");
    EXPECT_NOT_CONTAINS(unit.impl, "co_await ch.get()->receive");
}

// ============================================================
// bug-11（2026-08-31）：for-in channel 识别/生成覆盖不全
// 根因①：genForStmt channel 分支（StmtControl.cpp）入口只认 Identifier+channelVarNames_
// （只注册 let 变量）→ channel 作函数/方法参数、record 字段、调用返回、spawn 参数时走默认
// range-for `for (auto v : *ch)` → Channel<T> 无 begin/end 坏 C++。
// 根因②（连带）：CoroScanner::visit(ForStmt) 不判 channel → 纯 for-in channel 函数判 Plain
// → co_await 落非协程函数坏 C++。
// 根因③：genForStmt inSyncThreadBlock_ 分支对所有 channelVarNames_ 命中变量无条件生成
// `_opt->is_none()`——协程 channel 的 recv_awaiter 无该方法 → 坏 C++。
// 修复：方向①入口判定放开（inferredType 为 channel/sync.Channel）+ 非 Identifier 形态
// 「预求值 + GcRootHandle 保护」（auto _ch_raw = <expr>; + GcRootHandle + 循环 _ch.get()）；
// 方向② visit(ForStmt) 补协程 channel 挂起判定；方向④ sync thread 内协程 channel 干净报错。
// ============================================================

TEST(CodeGen, ChannelParamForInCoroReceive) {
    // bug-11 方向①+②主线：channel 作函数参数 + for-in（参数不在 channelVarNames_）。
    // 修复后：worker 被标协程（仅 for-in channel 触发，方向②），生成协程 receive 路径
    // （is_done + co_await receive），不再落默认 range-for。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun worker(ch: channel<int>) {"
        " let sum: int = 0"
        " for v in ch { sum = sum + v }"
        " }"
        " fun main(io: Io) {"
        " let ch: channel<int> = channel(10)"
        " sync {"
        "   spawn (ch: channel<int>) {"
        "     for i in range(5) { ch.send(i) }"
        "     ch.close()"
        "   }"
        " }"
        " worker(ch)"
        " io.println(\"done\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 方向②：worker 仅含 for-in channel → 仍判协程（task<void> + co_await receive）
    EXPECT_CONTAINS(unit.header, "aura_rt::task<void> worker(");
    // 方向①：走 channel 分支协程 receive 路径，非默认 range-for
    EXPECT_CONTAINS(unit.impl, "co_await ch.get()->receive");
    EXPECT_NOT_CONTAINS(unit.impl, "for (auto v : *");
}

TEST(CodeGen, PureForInChannelMarksCoroutine) {
    // bug-11 方向②：函数体内仅 for-in 局部 channel（无其他挂起点）→ 标协程。
    // 修复前 decideCoro 判 Plain → co_await 落非协程函数坏 C++。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun worker(ch: channel<int>) {"
        " let sum: int = 0"
        " for v in ch { sum = sum + v }"
        " }"
        " fun main(io: Io) {"
        " let ch: channel<int> = channel(10)"
        " sync {"
        "   spawn (ch: channel<int>) {"
        "     for i in range(5) { ch.send(i) }"
        "     ch.close()"
        "   }"
        " }"
        " worker(ch)"
        " io.println(\"done\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 关键：仅 for-in channel 的函数必须被标为协程（生成 task<void> + co_await）
    EXPECT_CONTAINS(unit.header, "aura_rt::task<void> worker(");
    EXPECT_CONTAINS(unit.impl, "co_await ch.get()->receive");
}

