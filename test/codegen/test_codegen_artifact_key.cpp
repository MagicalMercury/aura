// ============================================================
// test/codegen/test_codegen_artifact_key.cpp — bug-94 回归：
//   依赖 #include 名 与 thunk 前缀必须用【依赖自己的】身份键（artifactKey）。
//
// 覆盖改动点：
//   · src/CodeGen/CodeGen.cpp  —— 依赖 #include 名 = artifactKey（空串回落裸 stem）
//   · src/main.cpp             —— collector nsStem = sanitizeId(身份键)
//   · src/Module/ModuleManager —— computeModuleKeys / identityKeyOf 的唯一性
//
// ⚠️ 本文件只断言 CodeGen 侧的 artifactKey 链；键值唯一性由
//    test/module/test_module_scan.cpp 的 `ModuleScan.Bug94*` 2 例守护。
// ============================================================
#include "framework/test_framework.h"
#include "framework/test_helpers.h"

#include "CodeGen/MetaCollect.h"      // MetaCollector / MetaSymbolRec
#include "CodeGen/CodeGen.h"          // CodeGenImport / CodeGenerator
#include "Module/ModuleManager.h"

#include <string>

using namespace aura_test;

// ============================================================
// bug-94：依赖 #include 名与 thunk 前缀必须用【依赖自己的】身份键
// ============================================================
TEST(CodeGenArtifactKey, DependencyIncludeUsesArtifactKey) {
    Aura::DiagnosticEngine diag;
    const std::string src =
        "fun main(io: Io) {\n"
        "    io.println(\"x\")\n"
        "}\n";
    diag.setSourceView(src); diag.reset();
    Aura::ModuleManager mgr(diag); mgr.loadBuiltinAurai();
    Aura::Lexer lexer(src);
    auto toks = lexer.scanAll();
    Aura::Parser parser(std::move(toks), diag);
    auto program = parser.parse();
    ASSERT_TRUE(program != nullptr);
    Aura::SemAnalyzer sema(diag);
    (void)sema.analyze(*program);
    ASSERT_FALSE(diag.hasErrors());

    // 手动构造「撞车工程」的依赖项：源路径裸 stem = "util"，身份键 = "a_util"
    Aura::CodeGenImport ci;
    ci.path        = "a/util.aura";
    ci.alias       = "ua";
    ci.modName     = "util";
    ci.isBuiltin   = false;
    ci.nsName      = "aura_mod_util";
    ci.artifactKey = "a_util";

    Aura::CodeGenerator cg(diag);
    Aura::CodeGenConfig cfg; cfg.setConfig(sema);
    auto unit = cg.generate(*program, "main", {ci}, std::string(), cfg, {}, {},
                            "", Aura::NullMetadata);

    EXPECT_CONTAINS(unit.header, "#include \"a_util.aura.h\"");       // 用身份键
    EXPECT_NOT_CONTAINS(unit.header, "#include \"util.aura.h\"");     // 不得回落裸 stem
    EXPECT_CONTAINS(unit.impl, "namespace ua = aura_mod_util;");      // 命名空间别名不受影响
}

// ============================================================
// bug-94：两模块（同 nsName、不同身份键）的 thunk 名必须不同
// ============================================================
TEST(CodeGenArtifactKey, ThunkPrefixFollowsIdentityKey) {
    Aura::MetaCollector a("D:/p/a/util.aura", "a_util");
    Aura::MetaCollector b("D:/p/b/util.aura", "b_util");
    Aura::MetaSymbolRec ra; ra.name = "fa"; ra.kind = 0;   // 0 = Fn（`MetaCollect.h:19`：`uint8_t kind`，0=Fn 1=Method 2=Ctor 3=BuiltinFn）
    Aura::MetaSymbolRec rb; rb.name = "fb"; rb.kind = 0;
    (void)a.collectSymbol(ra);
    (void)b.collectSymbol(rb);
    const std::string na = a.symbols().front().thunkName;
    const std::string nb = b.symbols().front().thunkName;
    EXPECT_EQ(na, "_aura_mat_a_util_0");
    EXPECT_EQ(nb, "_aura_mat_b_util_0");
    EXPECT_TRUE(na != nb);
}
