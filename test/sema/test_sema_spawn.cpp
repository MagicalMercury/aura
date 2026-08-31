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
