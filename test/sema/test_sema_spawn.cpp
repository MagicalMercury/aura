// ============================================================
// test_sema_spawn.cpp — Sema spawn 约束单元测试
//
// 覆盖：隐式捕获拒绝、同名自动绑定、显式传参、sync thread
//       内必须显式传参、参数只读、spawn 调用形态
// ============================================================
#include "framework/test_framework.h"
#include "framework/test_helpers.h"

using namespace aura_test;

// ============================================================
// 隐式捕获拒绝
// ============================================================
TEST(SemaSpawn, ImplicitCaptureRejected) {
    // spawn 闭包体内引用的外部变量必须出现在参数列表
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let io2 = io; sync { spawn { io2.println(\"x\") } } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
}

// ============================================================
// 同名自动绑定
// ============================================================
TEST(SemaSpawn, SameNameAutoBind) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { sync { spawn (io: Io) { io.println(\"x\") } } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaSpawn, ExplicitArgForm) {
    // 参数名与外部变量不一致时显式传实参
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let my_io = io;"
        " sync { spawn (io: Io) { io.println(\"x\") }(my_io) } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// 参数只读
// ============================================================
TEST(SemaSpawn, ParamReadonly) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { sync { spawn (x: int) { x = 5 } } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorCode(diag, Aura::DiagCode::E015_ConstReassign));
}

// ============================================================
// sync thread 内约束
// ============================================================
TEST(SemaSpawn, ThreadSpawnNoParamListRejected) {
    // sync thread 内即使同名也不能省略参数列表
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { sync thread { spawn { io.println(\"x\") } } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
}

TEST(SemaSpawn, ThreadSpawnWithParamOk) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { sync thread { spawn (io: Io) { io.println(\"x\") } } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaSpawn, ThreadSpawnWithMutexParam) {
    // sync thread 内共享数据需 Mutex 保护
    // 同名自动绑定：无显式实参列表时参数名绑定外层同名变量——i 需在外层定义，
    // 否则干净报错（cannot bind spawn parameter 'i'，修复前是坏 C++ 存量用例）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) {"
        " let counter = [0]; let m = sync.Mutex(); let i = 0;"
        " sync thread(max = 4) {"
        "   spawn (io: Io, m: sync.Mutex, counter: [int], i: int) {"
        "     lock (m) { counter.append(i) } } } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// spawn 调用形态
// ============================================================
TEST(SemaSpawn, CallForm) {
    // spawn func(args) 调用形态
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun worker(io: Io, n: int) { }"
        " fun main(io: Io) { sync { spawn worker(io, 1) } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaSpawn, CallFormOutsideSync) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun worker(io: Io, n: int) { }"
        " fun main(io: Io) { spawn worker(io, 1) }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorCode(diag, Aura::DiagCode::E018_SpawnOutsideSync));
}

// ============================================================
// sync(max) 有界并发
// ============================================================
TEST(SemaSpawn, SyncMaxWithSpawn) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { sync(max = 2) { spawn (io: Io) { io.println(\"x\") } } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaSpawn, SyncForMax) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let a = [1, 2, 3]; sync for(max = 2) x in a { io.println(x) } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// 多参 spawn + GcRootHandle 首参（Sema 层语法回归；CodeGen 层生成
// 断言见 test_codegen.cpp SpawnMultiParam*NoGet / SpawnRecordFirstNoGet）
// ============================================================
TEST(SemaSpawn, TwoParamChannelFirstOk) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let ch: channel<int> = channel(10)"
        " sync { spawn (ch: channel<int>, x: int) { ch.send(x) }(ch, 1) } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaSpawn, ThreeParamChannelFirstOk) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let ch1: channel<int> = channel(10);"
        " let ch2: channel<int> = channel(10)"
        " sync { spawn (ch1: channel<int>, ch2: channel<int>, z: int)"
        "   { ch1.send(z) }(ch1, ch2, 1) } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaSpawn, TwoParamRecordFirstOk) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { x: int, y: int }"
        " fun main(io: Io) { let p: Point = { x = 1, y = 2 }"
        " sync { spawn (p: Point, x: int) { io.println(str(p.x + x)) }(p, 1) } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// 同名自动绑定存在性校验（problem.txt「spawn 无显式实参时同名自动绑定不校验
// 外层同名变量存在性」修复）：无显式实参列表时，参数名绑定外层同名变量；
// 外层缺失 → 干净报错（不再落 g++ "'x' was not declared" 坏 C++）
// ============================================================
TEST(SemaSpawn, MissingOuterBindingRejected) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let a = 1"
        " sync { spawn (a: int, missing_outer: int) { io.println(str(a)) } } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag,
        "cannot bind spawn parameter 'missing_outer': no outer variable of that name"));
}

TEST(SemaSpawn, LoopVarAutoBindOk) {
    // README 设计用途 `spawn (io: Io, i: int)` 绑定循环变量 i——不误伤
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { sync { for i in range(3) {"
        " spawn (io: Io, i: int) { io.println(str(i)) } } } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaSpawn, ExplicitArgsSkipsBindingCheck) {
    // 显式实参列表形态不校验同名绑定（异名实参合法，已有 ExplicitArgForm 回归）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let my_io = io"
        " sync { spawn (io: Io) { io.println(\"x\") }(my_io) } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaSpawn, ThreadMissingOuterBindingRejected) {
    // sync thread 路径（genSpawnAsThread 捕获列表同样引用外层同名变量）缺外层变量 → 干净报错
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let m = sync.Mutex()"
        " sync thread(max = 2) { spawn (io: Io, m: sync.Mutex, missing_outer: int) {"
        "   io.println(\"x\") } } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag,
        "cannot bind spawn parameter 'missing_outer': no outer variable of that name"));
}

TEST(SemaSpawn, ThreadSameNameBindOk) {
    // sync thread 内同名自动绑定（外层有同名变量）不误伤
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let m = sync.Mutex()"
        " sync thread(max = 2) { spawn (io: Io, m: sync.Mutex) { io.println(\"x\") } } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// spawn 显式实参数量/类型校验（bug-21）：修复前从不 inferExpr/校验 args →
// 数量多/少、类型不匹配、未定义标识符全部静默放行 → CodeGen 按位置生成实参 →
// g++ too many/few / invalid conversion（坏 C++）。修复后 Sema 干净报错。
// ============================================================
TEST(SemaSpawn, ArgsCountMismatchExtra) {
    // 实参数 > 参数数 → 干净报错「spawn argument count mismatch」
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let ch: channel<int> = channel(10)"
        " sync { spawn (c: channel<int>) { c.send(42) }(ch, 3) } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "spawn argument count mismatch"));
    EXPECT_TRUE(hasErrorContaining(diag, "2 args for 1 parameters"));
}

TEST(SemaSpawn, ArgsCountMismatchMissing) {
    // 实参数 < 参数数（io 也占形参位）→ 干净报错
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let ch: channel<int> = channel(10)"
        " sync { spawn (c: channel<int>, x: int) { c.send(x) }(ch) } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "spawn argument count mismatch"));
    EXPECT_TRUE(hasErrorContaining(diag, "1 args for 2 parameters"));
}

TEST(SemaSpawn, ArgsCountMismatchIoVariant) {
    // io 显式声明参数变体，实参数 < 参数数 → 干净报错（io 占位不豁免数量）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) {"
        " sync { spawn (io: Io, x: int) { io.println(\"x\") }(io) } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "spawn argument count mismatch"));
}

TEST(SemaSpawn, ArgsTypeMismatchStrInt) {
    // 实参 string → 形参 int → 干净报错「spawn argument type mismatch」
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) {"
        " sync { spawn (x: int) { io.println(\"x\") }(\"hello\") } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "spawn argument type mismatch"));
}

TEST(SemaSpawn, ArgsTypeMismatchRecordInt) {
    // 实参 record → 形参 int → 干净报错
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Point = { a: int, b: int }"
        " fun main(io: Io) { let p = Point { a = 1, b = 2 }"
        " sync { spawn (x: int) { io.println(\"x\") }(p) } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "spawn argument type mismatch"));
}

TEST(SemaSpawn, ArgsTypeMismatchListInt) {
    // 实参 list<int> → 形参 int → 干净报错
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) {"
        " sync { spawn (x: int) { io.println(\"x\") }([1, 2, 3]) } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "spawn argument type mismatch"));
}

TEST(SemaSpawn, ArgsUndefinedIdentifierRejected) {
    // 实参引用未定义标识符：args 从不 inferExpr 是根因之一 → 修复后干净报错
    // （修复前落 g++ 'undefined_var' was not declared 坏 C++）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) {"
        " sync { spawn (x: int) { io.println(\"x\") }(undefined_var) } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "undefined_var"));
}

TEST(SemaSpawn, ArgsCorrectNotFlagged) {
    // 正确数量+类型（channel + int）不误伤
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let ch: channel<int> = channel(10)"
        " sync { spawn (c: channel<int>, x: int) { c.send(x) }(ch, 3) } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaSpawn, ArgsSameNameShadowingOk) {
    // 实参与参数名同（遮蔽形态）：inferExpr(args) 必须在外层作用域执行——
    // 若进入参数作用域后再推断，args 的 x 被参数 x 遮蔽 → 误报。
    // 外层求值语义下 x 解析到外层 let x，不误伤。
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let x = 3"
        " sync { spawn (x: int) { io.println(str(x)) }(x) } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// feature-14 U1：显式捕获强制（自由变量未列入捕获名单）
//
// 背景：此前只拒「完全无参数列表」的 spawn（ImplicitCaptureRejected），
// 但「有参数列表、只是漏了一个名字」的形态一路放行到 CodeGen，
// 生成裸标识符 -> g++ "'k' was not declared"（静默坏 C++）。
// 下方各条各针对一个断言点。
// ============================================================
TEST(SemaSpawn, BodyUsesUncapturedOuterVarRejected) {
    // 正例：body 引用 k，捕获名单只有 m -> 必须报错
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let k = 7; let m = 9"
        " sync { spawn (m: int) { io.println(str(k) + str(m)) } } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    bool msgFound = false;
    for (auto& m : diag.errorMessages())
        if (m.find("spawn body references 'k'") != std::string::npos) msgFound = true;
    EXPECT_TRUE(msgFound);
}

TEST(SemaSpawn, BodyAllOuterVarsCapturedOk) {
    // 对照：同一段代码把 k 补进捕获名单 -> 应通过
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let k = 7; let m = 9"
        " sync { spawn (k: int, m: int) { io.println(str(k) + str(m)) } } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaSpawn, BodyLocalDeclNotReported) {
    // body 自己声明的局部变量不是自由变量 -> 不得报错
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let m = 1; sync {"
        " spawn (m: int) { let t = m + 1; io.println(str(t)) } } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaSpawn, BodyBuiltinAndLoopVarNotReported) {
    // 内置函数名 str / 循环变量 i 都不属于「未捕获外层变量」——
    // 前者不捕获（BuiltinRegistry），后者是 body 内声明。
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let m = 1; sync {"
        " spawn (m: int) { for i in range(m) { io.println(str(i)) } } } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaSpawn, BodyNestedSpawnParamNotReported) {
    // 内层 spawn 的形参名（g）在外层 body 不是自由变量 -> 不得报错
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let n = 3; sync {"
        " spawn (n: int) { spawn (g: int) { io.println(str(g)) }(n) } } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaSpawn, CallFormNotAffectedByCaptureCheck) {
    // 调用形态 spawn func(args) 无闭包体，不走捕获检查
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun worker(io: Io, n: int) { io.println(str(n)) }"
        " fun main(io: Io) { let total = 3"
        " sync { spawn worker(io, total) } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// feature-14 P3：spawn 约束改「函数级可达性」
//
// 语义：函数体内含 spawn 合法 ⟺ 该函数可被 sync 块经调用链（直接/传递）
//       可达，或该函数自身体内词法含 sync 块（自身根）。
// 边界：本组只覆盖 **Sema 的 E018**。CodeGen 侧另有 `ioInScope_` 闸门
//       （spawn 需外层作用域有 io），本组用例一律让相关函数带 io 形参或
//       不触发该闸门，避免与 E018 混淆。
// ============================================================

TEST(SemaSpawnP3, CalleeReachableFromSync) {
    // ① sync { f() }，f 内含 spawn → 合法（P3 放行；P3 前报 E018）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun f(io: Io) throws { spawn (io: Io) { io.println(\"t\") } }"
        " fun main(io: Io) throws { sync { f(io) } }",
        diag);
    EXPECT_FALSE(hasErrorCode(diag, Aura::DiagCode::E018_SpawnOutsideSync));
}

TEST(SemaSpawnP3, TwoLevelChainReachableFromSync) {
    // ② 两级链 sync → f → g（g 含 spawn）→ 合法
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun g(io: Io) throws { spawn (io: Io) { io.println(\"g\") } }"
        " fun f(io: Io) throws { g(io) }"
        " fun main(io: Io) throws { sync { f(io) } }",
        diag);
    EXPECT_FALSE(hasErrorCode(diag, Aura::DiagCode::E018_SpawnOutsideSync));
}

TEST(SemaSpawnP3, NotReachableStillReportsE018) {
    // ③ 无 sync 可达（main 不被 sync 调用）→ 仍报 E018
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun f(io: Io) throws { spawn (io: Io) { io.println(\"t\") } }"
        " fun main(io: Io) throws { f(io) }",
        diag);
    EXPECT_TRUE(hasErrorCode(diag, Aura::DiagCode::E018_SpawnOutsideSync));
}

TEST(SemaSpawnP3, SelfSyncBlockIsRoot) {
    // ④ 规则 1 验收：fun f() { sync { spawn } } 自身 sync → 合法（自身根）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun f(io: Io) throws { sync { spawn (io: Io) { io.println(\"t\") } } }"
        " fun main(io: Io) throws { f(io) }",
        diag);
    EXPECT_FALSE(hasErrorCode(diag, Aura::DiagCode::E018_SpawnOutsideSync));
}

TEST(SemaSpawnP3, SyncThreadIsRoot) {
    // ⑤ U3 验收：sync thread { f() } 同样是 SyncContext 域 → 入根集
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun f(io: Io) throws { spawn (io: Io) { io.println(\"t\") } }"
        " fun main(io: Io) throws { sync thread { f(io) } }",
        diag);
    EXPECT_FALSE(hasErrorCode(diag, Aura::DiagCode::E018_SpawnOutsideSync));
}

TEST(SemaSpawnP3, ClosureBodySpawnCountsAsContaining) {
    // ⑥ U1 验收：闭包体内含 spawn → 外层函数算「含 spawn」（穿透闭包体），
    //    外层被 sync 可达 → 合法
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun f(io: Io) throws { let cb = fun (io2: Io) throws {"
        " spawn (io: Io) { io.println(\"t\") }(io2) }; cb(io) }"
        " fun main(io: Io) throws { sync { f(io) } }",
        diag);
    EXPECT_FALSE(hasErrorCode(diag, Aura::DiagCode::E018_SpawnOutsideSync));
}

TEST(SemaSpawnP3, UndecidableCallExemptsE018) {
    // 规则 2 兜底（R4）：含 spawn 的函数体内存在「静态不可判调用」→ 一律不报 E018
    // （闭包变量调用 cb()；宁漏勿误，最高危风险是误报阻塞合法代码）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun f(io: Io) throws { let cb = fun () { }; cb();"
        " spawn (io: Io) { io.println(\"t\") } }"
        " fun main(io: Io) throws { f(io) }",
        diag);
    EXPECT_FALSE(hasErrorCode(diag, Aura::DiagCode::E018_SpawnOutsideSync));
}
