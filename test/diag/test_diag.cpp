// ============================================================
// test_diag.cpp — DiagnosticEngine 单元测试
//
// 覆盖：严重级别（Error/Warning/Note）、错误码、maxErrors 上限、
//       源码行提取、print 输出格式、mergeFrom、reset、文件归属快照
// ============================================================
#include "framework/test_framework.h"
#include "framework/test_helpers.h"

#include <sstream>

using namespace aura_test;

// ============================================================
// 严重级别与计数
// ============================================================
TEST(Diag, ErrorCountsAndMessages) {
    Aura::DiagnosticEngine diag;
    diag.error(1, 5, Aura::DiagCode::E010_UndefinedIdent, "undefined 'x'");
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_EQ(diag.errorCount(), 1);
    EXPECT_FALSE(diag.hasWarnings());
    EXPECT_EQ(diag.diagnostics().size(), 1u);
    EXPECT_EQ(diag.errorMessages().size(), 1u);
    // 纯文本消息格式：位置 + 错误码 + 消息
    EXPECT_CONTAINS(diag.errorMessages()[0], "[line 1:5]");
    EXPECT_CONTAINS(diag.errorMessages()[0], "E010");
    EXPECT_CONTAINS(diag.errorMessages()[0], "undefined 'x'");
}

TEST(Diag, WarningAndNoteRecorded) {
    Aura::DiagnosticEngine diag;
    diag.warn(2, 3, "deprecated");
    diag.note(2, 3, "see docs");
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_TRUE(diag.hasWarnings());
    EXPECT_EQ(diag.warningCount(), 1);
    EXPECT_EQ(diag.diagnostics().size(), 2u);
    EXPECT_EQ(diag.diagnostics()[0].severity, Aura::DiagSeverity::Warning);
    EXPECT_EQ(diag.diagnostics()[1].severity, Aura::DiagSeverity::Note);
    // warning 消息带 "warning:" 前缀
    EXPECT_CONTAINS(diag.errorMessages()[0], "warning:");
}

TEST(Diag, LegacyErrorNoCode) {
    // 兼容旧 API：无错误码时 code == None，消息不带 E 前缀
    Aura::DiagnosticEngine diag;
    diag.error(1, 1, "plain error");
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_EQ(diag.diagnostics()[0].code, Aura::DiagCode::None);
    EXPECT_NOT_CONTAINS(diag.errorMessages()[0], "E0");
}

// ============================================================
// 错误码字符串映射
// ============================================================
TEST(Diag, DiagCodeStrings) {
    EXPECT_EQ(std::string(Aura::diagCodeStr(Aura::DiagCode::E001_ExpectedToken)), "E001");
    EXPECT_EQ(std::string(Aura::diagCodeStr(Aura::DiagCode::E002_UnexpectedToken)), "E002");
    EXPECT_EQ(std::string(Aura::diagCodeStr(Aura::DiagCode::E003_BracketMismatch)), "E003");
    EXPECT_EQ(std::string(Aura::diagCodeStr(Aura::DiagCode::E010_UndefinedIdent)), "E010");
    EXPECT_EQ(std::string(Aura::diagCodeStr(Aura::DiagCode::E011_TypeMismatch)), "E011");
    EXPECT_EQ(std::string(Aura::diagCodeStr(Aura::DiagCode::E012_UndefinedGeneric)), "E012");
    EXPECT_EQ(std::string(Aura::diagCodeStr(Aura::DiagCode::E013_MethodNotFound)), "E013");
    EXPECT_EQ(std::string(Aura::diagCodeStr(Aura::DiagCode::E014_MatchNotExhaustive)), "E014");
    EXPECT_EQ(std::string(Aura::diagCodeStr(Aura::DiagCode::E015_ConstReassign)), "E015");
    EXPECT_EQ(std::string(Aura::diagCodeStr(Aura::DiagCode::E016_ThrowsViolation)), "E016");
    EXPECT_EQ(std::string(Aura::diagCodeStr(Aura::DiagCode::E017_NoneStandalone)), "E017");
    EXPECT_EQ(std::string(Aura::diagCodeStr(Aura::DiagCode::E018_SpawnOutsideSync)), "E018");
    EXPECT_EQ(std::string(Aura::diagCodeStr(Aura::DiagCode::E019_ImplMismatch)), "E019");
    EXPECT_EQ(std::string(Aura::diagCodeStr(Aura::DiagCode::None)), "");
}

// ============================================================
// maxErrors 上限
// ============================================================
TEST(Diag, MaxErrorsLimit) {
    Aura::DiagnosticEngine diag;
    diag.setMaxErrors(2);
    for (int i = 0; i < 10; ++i)
        diag.error(i + 1, 1, Aura::DiagCode::E010_UndefinedIdent, "err " + std::to_string(i));
    EXPECT_EQ(diag.errorCount(), 2);
    EXPECT_EQ(diag.diagnostics().size(), 2u);
    EXPECT_EQ(diag.errorMessages().size(), 2u);
}

TEST(Diag, MaxErrorsZeroDropsAll) {
    // maxErrors <= 0：report 中 errorCount_ >= maxErrors_ 恒成立 → 全部丢弃
    Aura::DiagnosticEngine diag;
    diag.setMaxErrors(0);
    for (int i = 0; i < 100; ++i)
        diag.error(1, 1, Aura::DiagCode::None, "e");
    EXPECT_EQ(diag.errorCount(), 0);
    EXPECT_EQ(diag.diagnostics().size(), 0u);
}

// ============================================================
// reset / mergeFrom
// ============================================================
TEST(Diag, ResetClearsEverything) {
    Aura::DiagnosticEngine diag;
    diag.error(1, 1, Aura::DiagCode::E010_UndefinedIdent, "a");
    diag.warn(1, 1, "w");
    diag.reset();
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_FALSE(diag.hasWarnings());
    EXPECT_EQ(diag.errorCount(), 0);
    EXPECT_EQ(diag.warningCount(), 0);
    EXPECT_EQ(diag.diagnostics().size(), 0u);
    EXPECT_EQ(diag.errorMessages().size(), 0u);
}

TEST(Diag, MergeFromCombines) {
    Aura::DiagnosticEngine a;
    Aura::DiagnosticEngine b;
    a.error(1, 1, Aura::DiagCode::E010_UndefinedIdent, "from a");
    b.error(2, 2, Aura::DiagCode::E011_TypeMismatch, "from b");
    b.warn(3, 3, "warn b");
    a.mergeFrom(b);
    EXPECT_EQ(a.errorCount(), 2);
    EXPECT_EQ(a.warningCount(), 1);
    EXPECT_EQ(a.diagnostics().size(), 3u);
    EXPECT_CONTAINS(a.errorMessages()[0], "from a");
    EXPECT_CONTAINS(a.errorMessages()[1], "from b");
}

TEST(Diag, MergePreservesAllEvenOverMax) {
    // mergeFrom 不检查 maxErrors_：汇总时全部保留
    Aura::DiagnosticEngine a;
    Aura::DiagnosticEngine b;
    a.setMaxErrors(1);
    a.error(1, 1, Aura::DiagCode::None, "a1");
    a.error(1, 1, Aura::DiagCode::None, "a2");  // 被 maxErrors 截断
    b.error(1, 1, Aura::DiagCode::None, "b1");
    b.error(1, 1, Aura::DiagCode::None, "b2");
    a.mergeFrom(b);
    EXPECT_EQ(a.errorCount(), 3);  // a 的 1 条 + b 的 2 条
    EXPECT_EQ(a.diagnostics().size(), 3u);
}

// ============================================================
// 源码行提取与 print 输出
// ============================================================
TEST(Diag, PrintShowsSourceLineAndCaret) {
    Aura::DiagnosticEngine diag;
    diag.setSourceView("let x = 1\nlet y = 2\n");
    diag.setFileName("test.aura");
    diag.error(2, 5, Aura::DiagCode::E015_ConstReassign, "cannot reassign");
    std::ostringstream os;
    diag.print(os);
    std::string out = os.str();
    EXPECT_CONTAINS(out, "error[E015]");
    EXPECT_CONTAINS(out, "cannot reassign");
    EXPECT_CONTAINS(out, "test.aura:2:5");
    EXPECT_CONTAINS(out, "let y = 2");   // 源码行
    EXPECT_CONTAINS(out, "^");           // caret
}

TEST(Diag, PrintShowsFixHint) {
    Aura::DiagnosticEngine diag;
    diag.setSourceView("x = 1\n");
    diag.error(1, 1, Aura::DiagCode::E010_UndefinedIdent, "undefined 'x'",
               "declare it with let");
    std::ostringstream os;
    diag.print(os);
    EXPECT_CONTAINS(os.str(), "help: declare it with let");
}

TEST(Diag, PrintWithoutFileNameUsesLine) {
    Aura::DiagnosticEngine diag;
    diag.setSourceView("abc\n");
    diag.error(1, 1, Aura::DiagCode::None, "msg");
    std::ostringstream os;
    diag.print(os);
    EXPECT_CONTAINS(os.str(), "line 1:1");
}

TEST(Diag, SourceLineWithCrLf) {
    // 行尾 \r 应被去除（被查行不是末行，末行含尾 \n 属正常）
    Aura::DiagnosticEngine diag;
    diag.setSourceView("line one\r\nline two\r\nline three\n");
    diag.error(2, 1, Aura::DiagCode::None, "m");
    std::ostringstream os;
    diag.print(os);
    std::string out = os.str();
    EXPECT_CONTAINS(out, "line two");
    EXPECT_NOT_CONTAINS(out, "line two\r");
}

TEST(Diag, SourceLineOutOfRangeEmpty) {
    Aura::DiagnosticEngine diag;
    diag.setSourceView("one\n");
    diag.error(99, 1, Aura::DiagCode::None, "m");
    std::ostringstream os;
    diag.print(os);
    // 越界行不打印源码行，但仍打印错误头
    EXPECT_CONTAINS(os.str(), "error");
}

// ============================================================
// 文件归属快照
// ============================================================
TEST(Diag, FileNameSnapshottedOnReport) {
    Aura::DiagnosticEngine diag;
    diag.setFileName("mod_a.aura");
    diag.error(1, 1, Aura::DiagCode::None, "in a");
    diag.setFileName("mod_b.aura");
    diag.error(1, 1, Aura::DiagCode::None, "in b");
    // 每条诊断快照报告时的文件名
    EXPECT_EQ(diag.diagnostics()[0].file, "mod_a.aura");
    EXPECT_EQ(diag.diagnostics()[1].file, "mod_b.aura");
}

// ============================================================
// 与编译流程集成的错误码验证
// ============================================================
TEST(Diag, SemaProducesExpectedCodes) {
    // const 重新赋值 → E015（当前实现中唯一稳定带错误码的语义错误）
    Aura::DiagnosticEngine diag;
    analyzeSource("fun main(io: Io) { const c = 1; c = 2 }", diag);
    EXPECT_TRUE(hasErrorCode(diag, Aura::DiagCode::E015_ConstReassign));

    // 未定义标识符 → 报错（消息定位；当前实现 code=None）
    Aura::DiagnosticEngine diag2;
    analyzeSource("fun main(io: Io) { let x = undefined_var }", diag2);
    EXPECT_TRUE(diag2.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag2, "undefined identifier 'undefined_var'"));

    // 类型不匹配 → 报错（消息定位）
    Aura::DiagnosticEngine diag3;
    analyzeSource("fun f(a: int) -> string { return a }", diag3);
    EXPECT_TRUE(diag3.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag3, "return type mismatch"));
}

TEST(Diag, ParserProducesSyntaxCodes) {
    // 缺表达式 → 报错（消息定位；当前实现 code=None）
    Aura::DiagnosticEngine diag;
    parseSource("fun main(io: Io) { let x = }", diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "expected expression"));
}
