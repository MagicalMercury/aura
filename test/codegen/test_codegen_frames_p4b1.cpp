// ============================================================
// test/codegen/test_codegen_frames_p4b1.cpp — feature-18 **P4b-1** B5：
//   协程上下文 lambda 的**匿名帧注入**（四处）+ 匿名帧区形态
//
// 依据：change.md 裁定④（四处）/ 裁定⑧（两级帧区）/ §3.1.2 缺口 C / §9-N4 /
//       §8.2 B0b / §6.2 C6–C8；§0.2 判据②（门控双路径）。
//
// 被测实现（**本批新增/修改**）：
//   src/CodeGen/{MetaCollect.{h,cpp}, MetaEmit.cpp}（匿名帧区渲染 / kFrameCount 公式 /
//   kAnonFrameBase）/ src/CodeGen/CodeGen.{h,cpp}（emitAnonFrame）/
//   src/CodeGen/{StmtSpawn,StmtSync,ExprClosureCallableObj,ExprClosureOldPath}.cpp（四处注入）
//   runtime/meta.h（anonFrameIndexAt / kAnonFrameBase / kAnonFrameCount）
//
// ⚠️ 本文件的 helper 是**自带一份**（复刻 test_codegen_frames_p4a.cpp 的 §B 段）——
//    不改那个文件（§3 禁碰清单的最小改动面原则）。
// ============================================================
#include "framework/test_framework.h"
#include "framework/test_helpers.h"

#include "CodeGen/MetaCollect.h"
#include "CodeGen/MetaEmit.h"

#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

using namespace aura_test;

namespace {

// ---- 文本工具（测试侧独立实现）----
int countOccurrences(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return 0;
    int n = 0;
    for (size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

// 取 `key ... = N;` 的 N（失败 = -1）
long tableValue(const std::string& meta, const std::string& key) {
    size_t p = meta.find(key);
    if (p == std::string::npos) return -1;
    p = meta.find('=', p);
    if (p == std::string::npos) return -1;
    return std::strtol(meta.c_str() + p + 1, nullptr, 10);
}

// 单文件 Inline 路径（**注入 collector** —— 走「门控 ON」这条产品路径）
struct InlineOut {
    Aura::CompileUnit unit;
    bool               ok = false;
};

// ---- 单文件 helper（与 test_codegen_frames_p4a.cpp §B 同形）----
InlineOut genMeta(const std::string& src, Aura::DiagnosticEngine& diag,
                  const std::string& sourcePath = std::string(),
                  const std::string& moduleName = "main") {
    InlineOut out;
    diag.setSourceView(src);
    diag.reset();
    Aura::ModuleManager mgr(diag);
    mgr.loadBuiltinAurai();
    Aura::Lexer lexer(src);
    auto tokens = lexer.scanAll();
    Aura::Parser parser(std::move(tokens), diag);
    auto program = parser.parse();
    if (!program) return out;
    loadImportedBuiltins(mgr, *program);
    Aura::SemAnalyzer sema(diag);          // ⚠️ 必须活到 generate 返回（Pitfall 13）
    (void)sema.analyze(*program);
    if (diag.hasErrors()) return out;
    Aura::CodeGenConfig cfg;               // 默认 Inline
    Aura::MetaCollector meta(sourcePath, Aura::ModuleManager::sanitizeId(moduleName));
    Aura::CodeGenerator cg(diag);
    out.unit = cg.generate(*program, moduleName, {}, std::string(), cfg, {}, {},
                           sourcePath, Aura::MetadataSink::collect(meta));
    out.ok = true;
    return out;
}

// ============================================================
// 源码常量（四处的触发源；语法照 example/used/5.aura 与既有并发用例）
// ============================================================
// ① 块形态 spawn（含显式实参）
const char* kSpawnBlockSrc =
    "type Point = { x: int }\n"
    "fun main(io: Io) {\n"
    "    let p = Point { x = 1 }\n"
    "    sync {\n"
    "        spawn (p: Point, x: int) { io.println(str(p.x + x)) }(p, 5)\n"
    "    }\n"
    "    io.println(\"done\")\n"
    "}\n";

// ② 调用形态 spawn（协程版：被调是协程函数）
const char* kSpawnCallSrc =
    "fun spawnWorker(n: int, io: Io) { io.println(str(n)) }\n"
    "fun main(io: Io) {\n"
    "    sync {\n"
    "        spawn spawnWorker(7, io)\n"
    "    }\n"
    "    io.println(\"done\")\n"
    "}\n";

// ③ sync for（唯一带协程 lambda 的 sync 形态）
const char* kSyncForSrc =
    "fun main(io: Io) {\n"
    "    let arr = [1, 2, 3]\n"
    "    sync for x in arr {\n"
    "        io.println(str(x))\n"
    "    }\n"
    "}\n";

// ④ 协程闭包（闭包体内含挂起点 io.println ⇒ closureBodyIsCoro；
//    语法 = Aura 的 `fun(x: int) { … }` 闭包字面量，见 example/used/2.aura:9/:19）
const char* kClosureSrc =
    "fun main(io: Io) {\n"
    "    sync {\n"
    "        let f = fun(x: int) { io.println(str(x)) }\n"
    "        f(1)\n"
    "    }\n"
    "}\n";

} // namespace

// ============================================================
// C6 CodeGenFrameP4b1.SpawnLambdaBlockForm（change.md §6.2 C6）
// ============================================================
TEST(CodeGenFrameP4b1, SpawnLambdaBlockForm) {
    Aura::DiagnosticEngine diag;
    InlineOut r = genMeta(kSpawnBlockSrc, diag, "D:/you/Aura/probe/p4b1/spawn_block.aura");
    ASSERT_FALSE(diag.hasErrors());
    ASSERT_TRUE(r.ok);

    // ① 帧守卫**确实**注入（`_lsga_` 是匿名帧家族，与 P4a 的 `_lsg_` 不同族）
    EXPECT_TRUE(countOccurrences(r.unit.impl, "aura_rt::FrameGuard _lsga_") >= 1);
    // ② 编号走**匿名帧区**换算函数（**不是** symbolIndexAt）
    EXPECT_TRUE(countOccurrences(r.unit.impl, "aura_rt::meta::anonFrameIndexAt(") >= 1);
    // ③ 注入点 = lambda 头的 `{` 之后：守卫必须**紧跟** lambda 体开括号（本源码只有一处
    //    协程 lambda ⇒ 守卫到头的距离很小，且中间不得再出现任何 `task<void> {` 头）
    const size_t head = r.unit.impl.find("-> aura_rt::task<void> {");
    ASSERT_TRUE(head != std::string::npos);
    const size_t guard = r.unit.impl.find("_lsga_", head);
    ASSERT_TRUE(guard != std::string::npos);
    EXPECT_TRUE(guard > head);
    EXPECT_TRUE(guard - head < 200);                       // 守卫紧跟头（未被别的语句顶开）
    EXPECT_TRUE(r.unit.impl.find("-> aura_rt::task<void> {", head + 1)
                > guard || r.unit.impl.find("-> aura_rt::task<void> {", head + 1)
                           == std::string::npos);          // 守卫在下一处 lambda 头之前
    // ④ 帧表：匿名帧**已登记**且落在匿名区
    EXPECT_TRUE(r.unit.metaImpl.find("kAnonFrameBase") != std::string::npos);
    EXPECT_TRUE(r.unit.metaImpl.find("<spawn@") != std::string::npos);
    EXPECT_EQ(tableValue(r.unit.metaImpl, "kFrameCount"),
              tableValue(r.unit.metaImpl, "kSymbolCount")
              + tableValue(r.unit.metaImpl, "kAnonFrameCount"));
    EXPECT_TRUE(tableValue(r.unit.metaImpl, "kAnonFrameCount") >= 1);
    // ⑤ 第一条匿名帧的编号 = (moduleIdx=0, seq=0) —— 编译期常量、可逐字断言
    EXPECT_TRUE(r.unit.impl.find("anonFrameIndexAt(0u, 0u)") != std::string::npos);
}

// ============================================================
// C7 CodeGenFrameP4b1.SpawnLambdaCallForm（§6.2 C7 / GLM 🟡-1）
// ============================================================
TEST(CodeGenFrameP4b1, SpawnLambdaCallForm) {
    Aura::DiagnosticEngine diag;
    InlineOut r = genMeta(kSpawnCallSrc, diag, "D:/you/Aura/probe/p4b1/spawn_call.aura");
    ASSERT_FALSE(diag.hasErrors());
    ASSERT_TRUE(r.ok);

    EXPECT_TRUE(countOccurrences(r.unit.impl, "aura_rt::FrameGuard _lsga_") >= 1);
    EXPECT_TRUE(countOccurrences(r.unit.impl, "aura_rt::meta::anonFrameIndexAt(") >= 1);
    // 帧表里应能看到**调用形态**的专属帧名（与块形态可区分 ⇒ 两处落点都真的生效）
    EXPECT_TRUE(r.unit.metaImpl.find("<spawn(call)@") != std::string::npos);
    EXPECT_EQ(tableValue(r.unit.metaImpl, "kFrameCount"),
              tableValue(r.unit.metaImpl, "kSymbolCount")
              + tableValue(r.unit.metaImpl, "kAnonFrameCount"));
}

// ============================================================
// C8 CodeGenFrameP4b1.SyncForLambda（§6.2 C8）
// ============================================================
TEST(CodeGenFrameP4b1, SyncForLambda) {
    Aura::DiagnosticEngine diag;
    InlineOut r = genMeta(kSyncForSrc, diag, "D:/you/Aura/probe/p4b1/sync_for.aura");
    ASSERT_FALSE(diag.hasErrors());
    ASSERT_TRUE(r.ok);

    EXPECT_TRUE(countOccurrences(r.unit.impl, "aura_rt::FrameGuard _lsga_") >= 1);
    EXPECT_TRUE(countOccurrences(r.unit.impl, "aura_rt::meta::anonFrameIndexAt(") >= 1);
    EXPECT_TRUE(r.unit.metaImpl.find("<sync@") != std::string::npos);
}

// ============================================================
// C-闭包 CodeGenFrameP4b1.ClosureLambda（§3.2 第四处 / §9-V4）
//   协程闭包的可调用体入口 ⇒ 匿名帧（落在 impl 或 header —— 两条闭包路径不同）
// ============================================================
TEST(CodeGenFrameP4b1, ClosureLambda) {
    Aura::DiagnosticEngine diag;
    InlineOut r = genMeta(kClosureSrc, diag, "D:/you/Aura/probe/p4b1/closure.aura");
    ASSERT_FALSE(diag.hasErrors());
    ASSERT_TRUE(r.ok);

    const std::string both = r.unit.impl + "\n" + r.unit.header;
    EXPECT_TRUE(countOccurrences(both, "aura_rt::FrameGuard _lsga_") >= 1);
    EXPECT_TRUE(countOccurrences(both, "aura_rt::meta::anonFrameIndexAt(") >= 1);
    EXPECT_TRUE(r.unit.metaImpl.find("<closure@") != std::string::npos);
}

// ============================================================
// 门控 OFF（R7 / §0.2 判据② 的**另一条路径**）：`NullMetadata` ⇒ **零注入**
//   ⚠️ 本用例的存在理由：P4a 曾因只验 CLI（collector ON）路径而把「416 个单测红」
//      藏了一整批（change.md §11.11）。frame/行号已由 P4a 的
//      `CodeGenFrame.NoCollectorProducesNoInjection` 兜住；**本用例兜匿名帧**。
// ============================================================
TEST(CodeGenFrameP4b1, NoCollectorProducesZeroAnonInjection) {
    for (const char* src : { kSpawnBlockSrc, kSpawnCallSrc, kSyncForSrc, kClosureSrc }) {
        Aura::DiagnosticEngine diag;
        Aura::CompileUnit unit = compileSource(src, diag, "main");   // ← 框架入口：显式 NullMetadata
        EXPECT_FALSE(diag.hasErrors());
        ASSERT_TRUE(!unit.impl.empty());
        EXPECT_NOT_CONTAINS(unit.impl, "_lsga_");
        EXPECT_NOT_CONTAINS(unit.impl, "anonFrameIndexAt");
        EXPECT_NOT_CONTAINS(unit.header, "_lsga_");
        EXPECT_NOT_CONTAINS(unit.header, "anonFrameIndexAt");
        EXPECT_NOT_CONTAINS(unit.impl, "kFrameTable");
        EXPECT_NOT_CONTAINS(unit.header, "kFrameTable");
        EXPECT_NOT_CONTAINS(unit.impl, "kAnonFrameBase");
    }
}

// ============================================================
// 匿名帧区自洽 CodeGenFrameP4b1.AnonFrameRegionIsSelfConsistent
//   · `kFrameCount == kSymbolCount + kAnonFrameCount`
//   · `kAnonFrameBase` 条数 == `kModuleCount`（每模块一项，含「无匿名帧的模块」占位）
//   · 表内**恰好**有一条 `<spawn@…>` 与一条 `<sync@…>`（每处注入 1 次，不重复登记）
// ============================================================
TEST(CodeGenFrameP4b1, AnonFrameRegionIsSelfConsistent) {
    // 同一模块内同时含 ① 块形态 spawn 与 ③ sync for ⇒ 匿名帧 ≥ 2
    const std::string src =
        "fun main(io: Io) {\n"
        "    let arr = [1, 2, 3]\n"
        "    sync {\n"
        "        spawn (x: int) { io.println(str(x * 2)) }(4)\n"
        "    }\n"
        "    sync for x in arr { io.println(str(x)) }\n"
        "}\n";
    Aura::DiagnosticEngine diag;
    InlineOut r = genMeta(src, diag, "D:/you/Aura/probe/p4b1/both.aura");
    ASSERT_FALSE(diag.hasErrors());
    ASSERT_TRUE(r.ok);

    const long sc = tableValue(r.unit.metaImpl, "kSymbolCount");
    const long fc = tableValue(r.unit.metaImpl, "kFrameCount");
    const long ac = tableValue(r.unit.metaImpl, "kAnonFrameCount");
    ASSERT_TRUE(sc > 0);
    EXPECT_EQ(fc, sc + ac);
    EXPECT_TRUE(ac >= 2);
    // 匿名帧（`<…@line>`）在符号表里**不存在**（两级帧区互不混入）——
    // 帧表尾段有它们，但 `kSymbolTable` 里没有 `<` 开头的 Aura 名。
    const size_t symStart = r.unit.metaImpl.find("kSymbolTable[]");
    ASSERT_TRUE(symStart != std::string::npos);
    const std::string symPart = r.unit.metaImpl.substr(symStart,
        r.unit.metaImpl.find("kTypeTable") - symStart);
    EXPECT_TRUE(symPart.find("\"<") == std::string::npos);           // 符号名不带 `<` 前缀
    EXPECT_TRUE(r.unit.metaImpl.find("<spawn@") != std::string::npos);
    EXPECT_TRUE(r.unit.metaImpl.find("<sync@")  != std::string::npos);
    // 每个注入点的 seq 唯一（变量名唯一性自证：`_lsga_` 名不重名）
    std::vector<std::string> names;
    for (size_t p = r.unit.impl.find("_lsga_"); p != std::string::npos;
         p = r.unit.impl.find("_lsga_", p + 1)) {
        size_t e = p + 6;
        while (e < r.unit.impl.size() &&
               (std::isdigit(static_cast<unsigned char>(r.unit.impl[e])) || r.unit.impl[e] == '_'))
            ++e;
        names.push_back(r.unit.impl.substr(p, e - p));
    }
    EXPECT_TRUE(names.size() >= 2);
    for (size_t i = 0; i < names.size(); ++i)
        for (size_t j = i + 1; j < names.size(); ++j)
            EXPECT_TRUE(names[i] != names[j]);                        // 无重名
}
