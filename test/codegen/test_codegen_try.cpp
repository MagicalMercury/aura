// ============================================================
// test_codegen_try.cpp — CodeGen 输出单元测试：try/catch 生成形态
//
// feature-18 P2（IIFE 退役 + try 体原地路径 + sync 收集器 Error 值化）
// 依据：change.md §4.1（形态断言清单）/ §4.2（CMake 登记）/ §7.3 M-R2
//        / §7.4 缺口①②（try 内 return 绕过 catchBody / 空路径不生成 file 填装）
//        / §9-E1（路径必须转义）/ §9-V1（compileSource 出处）
//
// 形态基线：2026-09-30 用 build/aurac.exe --cpp 实测的生成码（P2 落地后）：
//   {                                       ← _tk_hold 生命期块
//       bool _tk_err = false;
//       aura_rt::Error _tk_hold{};
//       aura_rt::GcRootHandle<decltype(_tk_hold.kind)> _tk_kind_h(_tk_hold.kind);  ← 5 个 Ref 句柄
//       ...（message/extra/file/stack）
//       try { <try 体原地生成：可含 co_await / sync 块> }
//       } catch (const aura_rt::Error& _e) { <只赋值 + 置标志，禁 co_await> }
//       if (_tk_err) { auto& e = _tk_hold; <catchBody：co_await 合法> }
//   }
// ============================================================
#include "framework/test_framework.h"
#include "framework/test_helpers.h"

#include <string>

using namespace aura_test;

namespace {

// 统计 needle 在 haystack 中出现的次数（用于「恰好 5 个句柄」这类数量断言）
int countOccurrences(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return 0;
    int n = 0;
    for (size_t p = haystack.find(needle); p != std::string::npos;
         p = haystack.find(needle, p + needle.size()))
        ++n;
    return n;
}

// try/catch 新形态的三段锚点（实测形态，见文件头）
const char* kHandlerHead = "} catch (const aura_rt::Error& _e) {";
const char* kCatchBodyHead = "if (_tk_err) {";

} // namespace

// ============================================================
// ① try 体内调用协程函数 ⇒ 无 IIFE（无 std::variant 承装）
//    P2 前：initExpr 是 aura_rt::task<int>，塞进 std::variant<Result, Error>
//    ⇒ bug-90「could not convert 'task<int>' to 'std::variant<int, Error>'」。
//    ⚠️ failCoro 内的 io.println 是挂起点 ⇒ failCoro 被判为协程（bug-90 T2 同形）。
// ============================================================
TEST(CodeGenTry, CoroCallInsideTryHasNoIIFE) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun failCoro(io: Io) throws -> int {"
        "   io.println(\"  [failCoro] entering\")"
        "   throw { kind = \"boom\", message = \"coro\" }"
        " }"
        " fun main(io: Io) throws {"
        "   try { let a = failCoro(io) } catch (e) { io.println(\"caught\") }"
        " }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    const std::string& impl = unit.impl;
    EXPECT_NOT_CONTAINS(impl, "std::variant<");
    EXPECT_NOT_CONTAINS(impl, "[&]() -> std::variant");
    // 原地路径标志 + catchBody 落在正常流程分支
    EXPECT_CONTAINS(impl, "bool _tk_err = false;");
    EXPECT_CONTAINS(impl, "if (_tk_err) {");
    // try 体原地生成：协程调用（co_await failCoro）必须落在 try 块内，
    // 而不是被挪进「返回 variant 的 lambda」。
    size_t tryPos    = impl.find("try {");
    size_t handler   = impl.find(kHandlerHead);
    ASSERT_TRUE(tryPos != std::string::npos);
    ASSERT_TRUE(handler != std::string::npos && handler > tryPos);
    std::string trySeg = impl.substr(tryPos, handler - tryPos);
    EXPECT_CONTAINS(trySeg, "co_await");
    EXPECT_CONTAINS(trySeg, "failCoro(");
}

// ============================================================
// ② catchBody 内可含 co_await（原地路径使其合法）；handler 段内**无** co_await
//    C++ 标准禁止 catch handler 内 co_await（GCC: await expressions are not
//    permitted in handlers）⇒ handler 只赋值 + catchBody 挪到 if (_tk_err) 之后。
// ============================================================
TEST(CodeGenTry, CatchBodyCanCoAwait) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun work(io: Io) -> int { return 1 }"
        " fun main(io: Io) throws {"
        "   try { let a = work(io) } catch (e) { sync { let b = work(io) } }"
        " }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    const std::string& impl = unit.impl;
    size_t hStart = impl.find(kHandlerHead);
    size_t bStart = impl.find(kCatchBodyHead);
    ASSERT_TRUE(hStart != std::string::npos);
    ASSERT_TRUE(bStart != std::string::npos && bStart > hStart);
    std::string handlerSeg = impl.substr(hStart, bStart - hStart);
    size_t hNext = impl.find(kHandlerHead, bStart);
    std::string bodySeg = impl.substr(
        bStart, (hNext == std::string::npos ? impl.size() : hNext) - bStart);
    // handler 段：只做值拷贝 + 置标志，不得出现 co_await
    EXPECT_NOT_CONTAINS(handlerSeg, "co_await");
    EXPECT_CONTAINS(handlerSeg, "_tk_err = true;");
    // catchBody 段：sync 块生成的 co_await 合法存在
    EXPECT_CONTAINS(bodySeg, "co_await");
}

// ============================================================
// ③ sync 收集器承载完整 Error 值 + 5 个 Ref 模式句柄 + 单值重抛
//    P2 前：_u5msg/_u5kind 双标量 + Value 模式句柄（GcRootHandle(val, ThreadLocal)）
//    ⇒ compact 只更新句柄内部副本，变量本身仍悬垂（既有隐患）。
//    ⚠️ sync 块内须有**被驱动的 future**（failCoro 含 io.println ⇒ 协程）才会产生收集器。
// ============================================================
TEST(CodeGenTry, SyncCollectorSingleErrorValue) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun failCoro(io: Io) throws -> int {"
        "   io.println(\"  [failCoro] entering\")"
        "   throw { kind = \"boom\", message = \"sync-in-try\" }"
        " }"
        " fun work(io: Io) -> int { return 1 }"
        " fun main(io: Io) throws {"
        "   try {"
        "     sync { let a = failCoro(io); let w = work(io) }"
        "   } catch (e) { io.println(\"caught\") }"
        " }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    const std::string& impl = unit.impl;
    // 承装槽：完整 Error 值对象（取代 _u5msg/_u5kind 双标量）
    EXPECT_CONTAINS(impl, "aura_rt::Error _u5err0{};");
    // 5 个 Ref 模式句柄：单参构造（绑定 _u5err0 的成员地址）⇒ 结尾是 `);`
    // ⚠️ 句柄名用的是**短后缀**（msg，非 message）——实测形态见 change.md §3.5(a)
    const char* fieldNames[5]  = {"kind", "message", "extra", "file", "stack"};
    const char* handleSuffix[5] = {"kind", "msg",     "extra", "file", "stack"};
    for (int i = 0; i < 5; ++i) {
        std::string f = fieldNames[i];
        std::string h = handleSuffix[i];
        EXPECT_CONTAINS(impl, "aura_rt::GcRootHandle<decltype(_u5err0." + f +
                              ")> _u5err0_" + h + "_h(_u5err0." + f + ");");
        // Value 模式形态（带 scope 实参）不得出现
        EXPECT_NOT_CONTAINS(impl, "_u5err0_" + h + "_h(_u5err0." + f + ", ");
    }
    EXPECT_EQ(countOccurrences(impl, "aura_rt::GcRootHandle<decltype(_u5err0."), 5);
    // 双标量旧形态退役
    EXPECT_NOT_CONTAINS(impl, "_u5kind0");
    EXPECT_NOT_CONTAINS(impl, "_u5msg0");
    // 块尾重抛完整 Error 值（含 extra/file/line/stack）
    EXPECT_CONTAINS(impl, "if (_u5has0) throw _u5err0;");
}

// ============================================================
// ④ M-R2：compileSource 只传 5 参 ⇒ sourcePath == "" ⇒ 表达式路径不生成 file 填装
//    （保持 file == nullptr 判据，且不产生 intern_string("") 的额外分配）
// ============================================================
TEST(CodeGenTry, ThrowWithEmptySourceFileKeepsFileNull) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) throws {"
        "   try { let a = 1 } catch (e) { throw e }"
        " }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    const std::string& impl = unit.impl;
    // 表达式路径确有生成（catchBody 内 `throw e`）
    EXPECT_CONTAINS(impl, "throw _e;");
    // 空 sourcePath ⇒ 不生成填装（M-R2）
    EXPECT_NOT_CONTAINS(impl, "intern_string(\"\")");
    EXPECT_NOT_CONTAINS(impl, "auto _f = ");
    EXPECT_NOT_CONTAINS(impl, "_e.file = _f;");
}

// ============================================================
// ⑤ §7.4 缺口①（R4）：try 体内 `return` ⇒ 绕过 catchBody
//    IIFE 时代 `return` 出不了 lambda（本就不允许）；原地生成后合法 ⇒
//    生成码的 `return 1;` 必须位于 try 块内（handler 之前），
//    catchBody（`if (_tk_err) { ... return 2; }`）在其之后 ⇒ 执行路径不经过 catchBody。
// ============================================================
TEST(CodeGenTry, TryReturnBypassesCatchBody) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun pick() throws -> int {"
        "   try { return 1 } catch (e) { return 2 }"
        " }"
        " fun main(io: Io) throws { let v = pick(); io.println(\"done\") }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    const std::string& impl = unit.impl;
    size_t tryPos   = impl.find("try {");
    size_t retPos   = impl.find("return 1;");
    size_t catchPos = impl.find(kHandlerHead);
    size_t bodyPos  = impl.find(kCatchBodyHead);
    ASSERT_TRUE(tryPos != std::string::npos);
    ASSERT_TRUE(retPos != std::string::npos);
    ASSERT_TRUE(catchPos != std::string::npos);
    ASSERT_TRUE(bodyPos != std::string::npos);
    EXPECT_TRUE(tryPos < retPos);      // return 1; 在 try 块内（原地路径才可能）
    EXPECT_TRUE(retPos < catchPos);    // 位于 handler 之前 ⇒ 执行 return 即离开，不落到 catchBody
    EXPECT_TRUE(catchPos < bodyPos);   // catchBody 在 handler 之后的正常流程分支
    EXPECT_CONTAINS(impl.substr(bodyPos), "return 2;");  // catchBody 本身仍在（仅 _tk_err 时可达）
    EXPECT_NOT_CONTAINS(impl, "std::variant<");
}

// ============================================================
// ⑥ §9-E1：sourcePath（真实路径，Windows 含 `\`）必须经 escapeStringLiteral
//    转义后拼进 C++ 字符串字面量（未转义 ⇒ GCC unknown escape sequence + 丢反斜杠）。
//    ⚠️ compileSource（test/framework/test_helpers.h:117-137）只传 5 参、无
//       sourcePath ⇒ 此处**直调 cg.generate**（change.md §4.1 允许的替代做法）。
//    ⚠️ 不能复用 analyzeSource()：其局部 SemAnalyzer 在返回前析构 ⇒ 交给 CodeGen
//       时 SemType 等已悬垂（实测 SIGSEGV in funSignature）。故按 compileSource 的
//       既有流水线在本用例内展开，保持 sema 存活。
// ============================================================
TEST(CodeGenTry, EscapeStringLiteralIsUsedForFile) {
    const std::string src =
        "fun failCoro(io: Io) throws -> int {"
        "   throw { kind = \"boom\", message = \"x\" }"
        " }"
        " fun main(io: Io) throws { try { let a = failCoro(io) } catch (e) { throw e } }";

    Aura::DiagnosticEngine diag;
    diag.setSourceView(src);
    diag.reset();
    Aura::ModuleManager mgr(diag);
    mgr.loadBuiltinAurai();
    Aura::Lexer lexer(src);
    auto tokens = lexer.scanAll();
    Aura::Parser parser(std::move(tokens), diag);
    auto prog = parser.parse();
    ASSERT_TRUE(prog != nullptr);
    loadImportedBuiltins(mgr, *prog);
    Aura::SemAnalyzer sema(diag);
    (void)sema.analyze(*prog);
    ASSERT_FALSE(diag.hasErrors());

    const std::string winPath = "D:\\you\\Aura\\probe\\esc\\x.aura";
    Aura::CodeGenerator cg(diag);
    auto unit = cg.generate(*prog, "main", {}, "", {}, {}, {}, winPath);
    const std::string& impl = unit.impl;

    // 期望形态：字面量里每个 `\` 已翻倍
    std::string doubled;
    for (char c : winPath) {
        if (c == '\\') doubled += "\\\\";
        else           doubled += c;
    }
    EXPECT_CONTAINS(impl, "intern_string(\"" + doubled + "\")");
    // E1 失效形态（未转义单反斜杠）不得出现
    EXPECT_NOT_CONTAINS(impl, "intern_string(\"" + winPath + "\")");

    // RecordExpr 路径：6 参显式构造已带 file + line。
    // §3.9（P3）起 `throwSite` 由常量 `kThrowSiteUnknown` 改为**计算值**（P3 设计意图变更，非实现缺陷；
    //   依据 `change.md §4.4` 例外块，2026-10-02 更新）。P2 原断言依据注释「throwSite 本阶段仍 Unknown」，
    //   而该「本阶段」正是被 P3 §3.9 终结的阶段 ⇒ 旧断言必然失配。
    //   本用例单文件（moduleIdx=0）且仅 1 个 throw ⇒ site = localSeq = 0（未超界、不 clamp）。
    //   下仅断言**形态**，不写死具体数字（数字随 throw 点顺序变）。
    EXPECT_CONTAINS(impl, "_hk.get(), _hm.get(), nullptr, _f, ");   // 6 参显式构造 + file 在位
    {
        size_t recP = impl.find("throw aura_rt::Error(");
        ASSERT_TRUE(recP != std::string::npos);
        size_t recE = impl.find('\n', recP);
        std::string recThrow = impl.substr(recP, (recE == std::string::npos ? impl.size() : recE) - recP);
        size_t recClose = recThrow.rfind(')');
        ASSERT_TRUE(recClose != std::string::npos);
        size_t recComma = recThrow.rfind(", ", recClose);
        ASSERT_TRUE(recComma != std::string::npos);
        size_t commas = 0;   // 6 个实参 ⇒ 5 个分隔逗号（§3.9 的 6 参显式构造形态）
        for (size_t k = recThrow.find('('); k != std::string::npos && k < recClose; ++k)
            if (recThrow.compare(k, 2, ", ") == 0) ++commas;
        EXPECT_EQ(commas, size_t(5));
        std::string site = recThrow.substr(recComma + 2, recClose - (recComma + 2));
        EXPECT_FALSE(site.empty());                                         // 第 6 参非空
        EXPECT_EQ(site.find_first_not_of("0123456789"), std::string::npos); // 第 6 参 = 数字 site id（非旧常量）
    }
    // 表达式路径：预 intern 前置 + 「未填则补」（file/line 坐标整体）
    EXPECT_CONTAINS(impl, "auto _f = aura_rt::intern_string(\"" + doubled + "\");");
    EXPECT_CONTAINS(impl, "if (_e.file == nullptr) { _e.file = _f; _e.line = ");
    // M-R1 不变式：预 intern 必须出现在 `_e` 构造之前（唯一分配点落在 _e 存续期之外）
    size_t fPos = impl.find("auto _f = ");
    size_t ePos = impl.find("auto _e = (");
    ASSERT_TRUE(fPos != std::string::npos);
    ASSERT_TRUE(ePos != std::string::npos);
    EXPECT_TRUE(fPos < ePos);
}

// ============================================================
// ⑦ §9-E3：空路径守卫必须同样覆盖 **RecordExpr 路径**（`throw { kind, message }`）
//    批 B 用例首跑 dump 实证：空 sourcePath 下 RecordExpr 路径**无条件**生成
//    `intern_string("")` ⇒ 空串非 nullptr，破坏 M-R2「file == nullptr 表示无来源」判据。
//    本用例：空路径 ⇒ 该路径 throw 语句 file/line **坐标整体置空**（nullptr + 0），
//    且**不出现** `intern_string(` 任何形态；真句柄 `_hk`/`_hm` 仍在（不得删）。
// ============================================================
TEST(CodeGenTry, ThrowRecordExprWithEmptySourceFileKeepsFileNull) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun boom() throws {"
        "   throw { kind = \"boom\", message = \"rec\" }"
        " }"
        " fun main(io: Io) throws { boom() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    const std::string& impl = unit.impl;
    // RecordExpr 路径确有生成：预求值 + 真句柄保护 kind/message
    EXPECT_CONTAINS(impl, "aura_rt::GcRootHandle<decltype(_k)> _hk(_k);");
    EXPECT_CONTAINS(impl, "aura_rt::GcRootHandle<decltype(_m)> _hm(_m);");
    // 定位该路径的唯一 throw 语句（截到行尾，避免误判其它 intern_string 用途）
    size_t tPos = impl.find("throw aura_rt::Error(");
    ASSERT_TRUE(tPos != std::string::npos);
    size_t tEnd = impl.find('\n', tPos);
    std::string throwStmt = impl.substr(
        tPos, (tEnd == std::string::npos ? impl.size() : tEnd) - tPos);
    // 真句柄仍在（M-R1/红线：不得删）
    EXPECT_CONTAINS(throwStmt, "_hk.get(), _hm.get()");
    // file/line 坐标整体置空（空路径下同不补）
    // §3.9（P3）起 `throwSite` 由常量 `kThrowSiteUnknown` 改为**计算值**（P3 设计意图变更，非实现缺陷；
    //   依据 `change.md §4.4` 例外块，2026-10-02 更新）。下断言**形态**：坐标整体置空后，第 6 参
    //   = 数字 site id，或（仅 clamp 时）旧常量 `aura_rt::kThrowSiteUnknown`；不写死具体数字。
    {
        const std::string sitePfx = "nullptr, nullptr, 0, ";
        size_t siteP = throwStmt.find(sitePfx);
        ASSERT_TRUE(siteP != std::string::npos);        // 坐标整体置空形态仍在
        const std::string siteTail = throwStmt.substr(siteP + sitePfx.size());  // 例："0);"
        const bool numericSite = siteTail.size() >= 3 &&
            siteTail.compare(siteTail.size() - 2, 2, ");") == 0 &&
            siteTail.find_first_not_of("0123456789") == siteTail.size() - 2;
        EXPECT_TRUE(numericSite || siteTail == "aura_rt::kThrowSiteUnknown);");
    }
    // 空路径不得 intern 任何形态（含 `intern_string("")`）⇒ file 保持 nullptr
    EXPECT_NOT_CONTAINS(throwStmt, "intern_string(");
    EXPECT_NOT_CONTAINS(impl, "intern_string(\"\")");
    // 空路径不得生成预 intern 语句
    EXPECT_NOT_CONTAINS(impl, "auto _f = ");
}
