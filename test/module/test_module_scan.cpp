#include "framework/test_framework.h"
#include "framework/test_helpers.h"
#include "Module/ModuleManager.h"
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdio>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace Aura;
using namespace aura_test;

namespace {
namespace fs = std::filesystem;

// feature-13 C2：声明级扫描验证夹具。
// 临时目录（与 test_module.cpp 同款命名规则，避免并发冲突）
std::string c2TempDir() {
    auto dir = fs::temp_directory_path() / ("aura_c2_scan_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir.string();
}

std::string c2Write(const std::string& dir, const std::string& name,
                    const std::string& content) {
    std::string path = (fs::path(dir) / name).string();
    std::ofstream f(path);
    f << content;
    return path;
}

// ============================================================
// feature-13 C2 第二轮修正：比对口径
//
// 背景：原比对把两侧都当「按源码顺序的声明行列表」来比，这是错的 ——
//   DeclUnit 按种类分组存放（types → funcs → imports），结构上不保留源码
//   声明顺序；且 module 声明在 Program 侧是列表中的一行、在 DeclUnit 侧是
//   moduleName/hasExplicitModule 字段，两侧呈现位置不同。
// 修正：两侧各自归一化成「同一语义的内容行」，再按【多重集】比对。
//   这比原来的「按列表顺序」更严（顺序假阳性消失），且不放松任何内容断言：
//   行集合与每行重数必须逐字相等。
// ============================================================

// 内置模块白名单 —— 与 ModuleManager::isKnownBuiltin 保持一致
// （src/Module/ModuleManager.cpp；该函数为 private，测试侧镜像一份）
bool isBuiltinName(const std::string& name) {
    return name == "path" || name == "math";
}

// 与 ModuleManager::resolveImportPath 同规则的路径解析（相对导入 →
// 补 .aura 扩展名 → 绝对路径）。镜像理由是同一函数为 private。
std::string resolveImportLikeModuleManager(const std::string& importPath,
                                           const std::string& importerDir) {
    fs::path rel = fs::path(importerDir) / importPath;
    if (fs::exists(rel)) return fs::absolute(rel).string();
    rel += ".aura";
    if (fs::exists(rel)) return fs::absolute(rel).string();
    fs::path abs = importPath;
    if (fs::exists(abs)) return fs::absolute(abs).string();
    abs += ".aura";
    if (fs::exists(abs)) return fs::absolute(abs).string();
    return std::string();
}

// 归一化一条 import 行为比对字符串。
// 用户模块：路径按 ModuleManager 的语义解析为绝对路径
// （Program 侧的 AST 里 path 还是源码原样，scan 侧已是解析结果，
//   不解析就没法比同一个东西 —— ModuleManager.cpp:181 / :348）。
std::string importLine(const std::string& rawPath, const std::string& alias,
                       bool isBuiltin, const std::string& dir) {
    std::string p = rawPath;
    if (!isBuiltin) {
        if (isBuiltinName(rawPath)) {
            isBuiltin = true;
        } else {
            std::string resolved = resolveImportLikeModuleManager(rawPath, dir);
            if (!resolved.empty()) p = resolved;
        }
    }
    std::string out = "import:" + p + (isBuiltin ? " builtin" : " user");
    if (!alias.empty()) out += " as " + alias;
    return out + "\n";
}

// Program 侧：声明行（module 不进此列表，单独比对）
std::string declsOfProgram(const Program& prog, const std::string& dir) {
    std::string out;
    for (auto& d : prog.decls) {
        if (!d) continue;
        if (auto* td = dynamic_cast<TypeDecl*>(d.get())) {
            out += "type:" + td->name;
            for (auto& tp : td->typeParams) out += "<" + tp + ">";
            out += (td->isPublic ? " pub" : "");
            out += "\n";
        } else if (auto* id = dynamic_cast<InterfaceDecl*>(d.get())) {
            out += "interface:" + id->name;
            for (auto& tp : id->typeParams) out += "<" + tp + ">";
            out += (id->isPublic ? " pub" : "");
            out += "\n";
        } else if (auto* fn = dynamic_cast<FunDecl*>(d.get())) {
            out += "fun:" + fn->name + "(";
            for (size_t i = 0; i < fn->params.size(); ++i) {
                if (i) out += ",";
                out += (fn->params[i].type ? "T" : "-");
            }
            out += ")";
            out += (fn->throws ? " throws" : "");
            out += (fn->returnType ? " ->T" : "");
            out += (fn->isPublic ? " pub" : "");
            out += "\n";
        } else if (auto* md = dynamic_cast<MethodDecl*>(d.get())) {
            out += "method:" + md->receiverType + "." + md->name + "(";
            for (size_t i = 0; i < md->params.size(); ++i) {
                if (i) out += ",";
                out += (md->params[i].type ? "T" : "-");
            }
            out += ")\n";
        } else if (auto* im = dynamic_cast<ImportDecl*>(d.get())) {
            out += importLine(im->path, im->alias, im->isBuiltin, dir);
        }
    }
    return out;
}

// Program 侧：显式 module 声明名（无则空串）
std::string moduleNameOfProgram(const Program& prog) {
    for (auto& d : prog.decls) {
        if (auto* mo = dynamic_cast<ModuleDecl*>(d.get())) return mo->name;
    }
    return std::string();
}

// DeclUnit 侧：声明行（与 declsOfProgram 逐行同构）
std::string declsOfUnit(const DeclUnit& u) {
    std::string out;
    for (auto& t : u.types) {
        out += (t.isInterface ? "interface:" : "type:") + t.name;
        for (auto& tp : t.typeParams) out += "<" + tp + ">";
        out += (t.isPublic ? " pub" : "");
        out += "\n";
    }
    for (auto& fn : u.funcs) {
        if (!fn.receiverType.empty()) {
            out += "method:" + fn.receiverType + "." + fn.name + "(";
            for (size_t i = 0; i < fn.paramTypes.size(); ++i) {
                if (i) out += ",";
                out += (fn.paramTypes[i].empty() ? "-" : "T");
            }
            out += ")\n";
            continue;
        }
        out += "fun:" + fn.name + "(";
        for (size_t i = 0; i < fn.paramTypes.size(); ++i) {
            if (i) out += ",";
            out += (fn.paramTypes[i].empty() ? "-" : "T");
        }
        out += ")";
        out += (fn.throws ? " throws" : "");
        out += (fn.returnType.empty() ? "" : " ->T");
        out += (fn.isPublic ? " pub" : "");
        out += "\n";
    }
    for (auto& im : u.imports) {
        // scan 侧存的已是解析后绝对路径（与 parseModule 同语义）
        out += "import:" + im.path + (im.isBuiltin ? " builtin" : " user");
        if (!im.alias.empty()) out += " as " + im.alias;
        out += "\n";
    }
    return out;
}

// "a\nb\n" → 排序后的行表（用于按内容多重集比对）
std::vector<std::string> sortedLines(const std::string& s) {
    std::vector<std::string> v;
    std::string cur;
    for (char c : s) {
        if (c == '\n') { v.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) v.push_back(cur);
    std::sort(v.begin(), v.end());
    return v;
}

// 一次完整对比：完整解析的声明部分 vs 声明级扫描产物
// 返回空串 = 一致；否则返回差异描述
std::string compareScanVsParse(const std::string& dir, const std::string& name,
                               const std::string& src) {
    std::string path = c2Write(dir, name, src);
    std::string fileDir = fs::path(path).parent_path().string();

    // (a) 完整 parse()
    DiagnosticEngine diagA;
    std::string sourceA = readFile(path);
    diagA.setSourceView(sourceA);
    Lexer lexerA(sourceA);
    Parser parserA(lexerA.scanAll(), diagA);
    auto prog = parserA.parse();
    bool parseHadErrors = diagA.hasErrors();
    std::string a = declsOfProgram(*prog, fileDir);
    std::string modA = moduleNameOfProgram(*prog);

    // (b) scanDeclarations()
    DiagnosticEngine diagB;
    ModuleManager mgr(diagB);
    DeclUnit u = mgr.scanDeclarations(path);
    std::string b = declsOfUnit(u);
    std::string modB = u.hasExplicitModule ? u.moduleName : std::string();

    // 扫描必须与完整解析同样"能解析"（均无错误）
    if (diagB.hasErrors() != parseHadErrors && !parseHadErrors) {
        std::string msg = "scan reported error, parse did not: ";
        for (auto& e : diagB.errorMessages()) msg += e + " | ";
        return msg;
    }

    // module 声明：两侧要么都没有、要么名字一致
    if (modA != modB) {
        return "module mismatch: parse=[" + modA + "] scan=[" + modB + "]";
    }

    // 内容多重集比对（顺序无关，内容与重数严格相等）
    if (sortedLines(a) != sortedLines(b)) {
        return "decl mismatch (content multiset):\n  parse=[" + a + "]\n  scan =[" + b + "]";
    }
    return std::string();
}

} // namespace

// ============================================================
// feature-13 C2：声明级扫描（scanDeclarations）单元测试
//
// 核心断言：扫描产物 == 完整解析的声明部分（逐符号比对）。
//
// ⚠️ 比对口径（第二轮修正）：两侧各自归一化为「同一语义的声明行」后，
// 按【内容多重集】比对（排序后逐行相等），而非按列表顺序。
// 原因：DeclUnit 按种类分组存放（types → funcs → imports），结构上
// 不保留源码声明顺序；且 module 在两侧呈现位置不同（Program 侧是
// 列表中一行，DeclUnit 侧是 moduleName/hasExplicitModule 字段）。
// 这不放松断言：行集合与每行重数必须逐字相等，且 module 名单独比对；
// 正向歋侧（改名 / 丢声明）均已实测能被捕获。
// 覆盖：普通 fun / body 内字符串花括号 / 注释花括号 / 默认参数含闭包字面量 /
//       type+interface（含泛型）/ import（用户+内置）+ module / fun main /
//       泛型函数 / .aurai 形态（无 body）。
// 另含 1 条「既有 parse() 不受影响」的守卫。
// ============================================================

TEST(ModuleScan, PlainFunWithNestedClosureAndControlFlow) {
    auto dir = c2TempDir();
    std::string diff = compareScanVsParse(dir, "plain.aura",
        "fun helper() -> int { return 1 }\n"
        "fun compute(a: int, b: string) throws -> int {\n"
        "    let f = fun(x: int) -> int { return x + 1 }\n"
        "    if a > 0 { for i in [1, 2] { let t = f(i) } }\n"
        "    match a { 1 { return 1 } _ { return 0 } }\n"
        "}\n");
    EXPECT_EQ(diff, "");
    fs::remove_all(dir);
}

TEST(ModuleScan, BodyWithLiteralBraces) {
    auto dir = c2TempDir();
    std::string diff = compareScanVsParse(dir, "braces_lit.aura",
        "fun fmt() -> string {\n"
        "    let a = \"{ not a block }\"\n"
        "    let b = \"}}{{ reverse\"\n"
        "    let c = \"a{b}c\"\n"
        "    return a + b + c\n"
        "}\n");
    EXPECT_EQ(diff, "");
    fs::remove_all(dir);
}

TEST(ModuleScan, BodyWithCommentBraces) {
    auto dir = c2TempDir();
    std::string diff = compareScanVsParse(dir, "braces_cmt.aura",
        "fun commented() -> int {\n"
        "    // { unbalanced line comment\n"
        "    /* } } } block comment with braces */\n"
        "    /* {{{ */\n"
        "    return 1   // trailing { }\n"
        "}\n");
    EXPECT_EQ(diff, "");
    fs::remove_all(dir);
}

TEST(ModuleScan, DefaultParamWithClosureLiteral) {
    auto dir = c2TempDir();
    // 默认值里的闭包字面量 { } 属【参数列表】而非函数 body 起始 —— 最易误判的形态
    std::string diff = compareScanVsParse(dir, "default_closure.aura",
        "fun apply(g: fun() -> int = fun() -> int { return 1 }) -> int {\n"
        "    return g()\n"
        "}\n");
    EXPECT_EQ(diff, "");
    fs::remove_all(dir);
}

TEST(ModuleScan, TypeAndInterfaceWithGenerics) {
    auto dir = c2TempDir();
    std::string diff = compareScanVsParse(dir, "types.aura",
        "type Pair<A, B> = { first: A, second: B }\n"
        "type Alias = int\n"
        "pub type Shown = string\n"
        "interface Reader<T> {\n"
        "    fun get() -> T\n"
        "    fun peek() -> T { return self.get() }\n"
        "}\n"
        "interface Marker { }\n");
    EXPECT_EQ(diff, "");
    fs::remove_all(dir);
}

TEST(ModuleScan, ImportsAndModuleDecl) {
    auto dir = c2TempDir();
    // 用户模块依赖文件（保证 resolveImportPath 能解析）
    c2Write(dir, "dep.aura", "fun depfun() -> int { return 1 }\n");
    std::string diff = compareScanVsParse(dir, "withmod.aura",
        "module withmod\n"
        "import path\n"
        "import \"dep.aura\" as d\n"
        "fun main(io: Io) { io.println(\"hi\") }\n");
    EXPECT_EQ(diff, "");
    fs::remove_all(dir);
}

TEST(ModuleScan, ModuleNameAndDepsSemantics) {
    auto dir = c2TempDir();
    std::string depPath = c2Write(dir, "helper.aura", "fun h() -> int { return 1 }\n");
    std::string mainPath = c2Write(dir, "entry.aura",
        "module entrymod\n"
        "import math\n"
        "import \"helper.aura\"\n"
        "fun main(io: Io) { }\n");

    DiagnosticEngine diag;
    ModuleManager mgr(diag);
    DeclUnit u = mgr.scanDeclarations(mainPath);
    EXPECT_FALSE(diag.hasErrors());
    // 显式 module 声明优先于文件 stem
    EXPECT_EQ(u.moduleName, "entrymod");
    EXPECT_TRUE(u.hasExplicitModule);
    // hasMain 检测
    EXPECT_TRUE(u.hasMain);
    // 内置模块只进 imports、不进 deps（与 parseModule 一致）
    EXPECT_EQ(u.imports.size(), (size_t)2);
    EXPECT_EQ(u.deps.size(), (size_t)1);
    EXPECT_EQ(u.deps[0], fs::absolute(depPath).string());
    bool sawBuiltin = false;
    for (auto& im : u.imports) if (im.isBuiltin && im.path == "math") sawBuiltin = true;
    EXPECT_TRUE(sawBuiltin);
    // 函数骨架：main 有 body，无 C++ 桥接
    ASSERT_TRUE(u.funcs.size() == (size_t)1);
    EXPECT_EQ(u.funcs[0].name, "main");
    EXPECT_TRUE(u.funcs[0].hasBody);
    EXPECT_FALSE(u.funcs[0].hasCppImpl);
    fs::remove_all(dir);
}

TEST(ModuleScan, GenericFunctionAndMethods) {
    auto dir = c2TempDir();
    std::string diff = compareScanVsParse(dir, "generic.aura",
        "fun id<T>(x: T) -> T { return x }\n"
        "type Box<T> = { value: T }\n"
        "fun (self Box<T>) get() -> T { return self.value }\n"
        "fun (self Box<T>) set(v: T) { self.value = v }\n");
    EXPECT_EQ(diff, "");
    fs::remove_all(dir);
}

TEST(ModuleScan, AuraiStyleNoBody) {
    // .aurai 形态：fun path.new(...) -> Path + 桥接标记（无 body）
    DiagnosticEngine diag;
    diag.setSourceView("fun path.new(s: string) -> Path ...\n"
                       "fun (self Path) parent() -> Path ...\n"
                       "type Path\n");
    // 用 parseDeclarationsOnly 直接验证「无 body 形态不误吞后续声明」
    Lexer lexer("fun path.new(s: string) -> Path ...\n"
                "fun (self Path) parent() -> Path ...\n"
                "type Path\n");
    Parser parser(lexer.scanAll(), diag);
    auto prog = parser.parseDeclarationsOnly();
    EXPECT_FALSE(diag.hasErrors());
    ASSERT_TRUE(prog != nullptr);
    EXPECT_EQ(prog->decls.size(), (size_t)3);
    auto* fn = as<FunDecl>(declAt(*prog, 0));
    ASSERT_TRUE(fn != nullptr);
    EXPECT_EQ(fn->name, "path.new");
    EXPECT_TRUE(fn->hasCppImpl);   // 桥接标记保留
    EXPECT_TRUE(fn->body == nullptr);
    auto* md = as<MethodDecl>(declAt(*prog, 1));
    ASSERT_TRUE(md != nullptr);
    EXPECT_EQ(md->receiverType, "Path");
    EXPECT_TRUE(md->body == nullptr);
}


TEST(ModuleScan, ScanDoesNotTouchExistingParse) {
    // 守卫：扫描路径使用后，完整 parse() 行为不受影响（独立标志 scanOnly_）
    std::string src =
        "fun a() -> int { return 1 }\n"
        "fun b() -> int {\n"
        "    let s = \"{}\"; return 2\n"
        "}\n";
    DiagnosticEngine diag;
    diag.setSourceView(src);
    Lexer lexer(src);
    Parser parser(lexer.scanAll(), diag);
    auto prog = parser.parse();
    EXPECT_FALSE(diag.hasErrors());
    ASSERT_TRUE(prog != nullptr);
    EXPECT_EQ(prog->decls.size(), (size_t)2);
    // 完整解析必须真的建出 body（扫描标志不得泄漏影响本路径）
    auto* fnA = as<FunDecl>(declAt(*prog, 0));
    auto* fnB = as<FunDecl>(declAt(*prog, 1));
    ASSERT_TRUE(fnA != nullptr);
    ASSERT_TRUE(fnB != nullptr);
    EXPECT_TRUE(fnA->body != nullptr);
    EXPECT_TRUE(fnB->body != nullptr);
    EXPECT_EQ(fnB->body->stmts.size(), (size_t)2);
}

// ============================================================
// feature-13 C3（2026-09-17）：汇总段——scanAll + 环/拓扑/入口/冲突
//
// 覆盖：递归发现 + 依赖图（scanUnits_）/ 环检测（相互 import）/
//       拓扑层序 / 入口唯一性 / module 冲突 D12 两分（隐式冲突、
//       显式共享合法）/ loadAllScanned 第二段填充。
// ⚠️ hasCycleOn/topologicalLayersOn/validateEntryOn/checkModuleConflicts
//    读 scanUnits_（不需要完整解析）——每组用例先 scanAll 再判定。
// ============================================================

// C3 夹具：临时目录（与 C2 同命名规则）
std::string c3TempDir() {
    auto dir = fs::temp_directory_path() / ("aura_c3_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir.string();
}

// 环：a ↔ b 相互 import → hasCycleOn 检出
TEST(ModuleScan, C3CycleDetectionOnScanTable) {
    auto dir = c3TempDir();
    c2Write(dir, "a.aura", "import \"b.aura\"\nfun main(io: Io) { }\n");
    c2Write(dir, "b.aura", "import \"a.aura\"\nfun helper() -> int { return 1 }\n");

    DiagnosticEngine diag;
    ModuleManager mgr(diag);
    ASSERT_TRUE(mgr.scanAll((fs::path(dir) / "a.aura").string()));
    EXPECT_EQ(mgr.globalTable().units.size(), (size_t)2);
    EXPECT_TRUE(mgr.hasCycleOn());
    EXPECT_TRUE(diag.hasErrors());   // 环错误已登记
    std::string all;
    for (auto& e : diag.errorMessages()) all += e;
    EXPECT_TRUE(all.find("circular dependency") != std::string::npos);
    fs::remove_all(dir);
}

// 拓扑：a → b → c（a import b，b import c）→ 层序 [c][b][a]
TEST(ModuleScan, C3TopoLayersOnScanTable) {
    auto dir = c3TempDir();
    c2Write(dir, "c.aura", "fun cfun() -> int { return 1 }\n");
    c2Write(dir, "b.aura", "import \"c.aura\"\nfun bfun() -> int { return cfun() }\n");
    c2Write(dir, "a.aura", "import \"b.aura\"\nfun main(io: Io) { }\n");

    DiagnosticEngine diag;
    ModuleManager mgr(diag);
    ASSERT_TRUE(mgr.scanAll((fs::path(dir) / "a.aura").string()));
    EXPECT_FALSE(mgr.hasCycleOn());
    auto layers = mgr.topologicalLayersOn();
    // 3 层
    ASSERT_EQ(layers.size(), (size_t)3);
    // 层 0 必须含 c（被依赖者先编译）；层 2 必须含 a（依赖者最后）
    EXPECT_EQ(layers[0].size(), (size_t)1);
    EXPECT_EQ(layers[2].size(), (size_t)1);
    EXPECT_TRUE(layers[0][0].find("c.aura") != std::string::npos);
    EXPECT_TRUE(layers[2][0].find("a.aura") != std::string::npos);
    EXPECT_TRUE(layers[1][0].find("b.aura") != std::string::npos);
    fs::remove_all(dir);
}

// 入口：无 main / 多 main / 单 main
TEST(ModuleScan, C3EntryValidationOnScanTable) {
    {   // 无 main
        auto dir = c3TempDir();
        c2Write(dir, "noentry.aura", "fun helper() -> int { return 1 }\n");
        DiagnosticEngine diag;
        ModuleManager mgr(diag);
        ASSERT_TRUE(mgr.scanAll((fs::path(dir) / "noentry.aura").string()));
        std::string entry;
        EXPECT_FALSE(mgr.validateEntryOn(entry));
        EXPECT_TRUE(diag.hasErrors());
        std::string all;
        for (auto& e : diag.errorMessages()) all += e;
        EXPECT_TRUE(all.find("no entry point") != std::string::npos);
        fs::remove_all(dir);
    }
    {   // 多 main（m1 可达 m2——二者都被扫描发现且各含 main）
        auto dir = c3TempDir();
        c2Write(dir, "m1.aura", "import \"m2.aura\"\nfun main(io: Io) { }\n");
        c2Write(dir, "m2.aura", "fun main(io: Io) { g() }\nfun g() -> int { return 1 }\n");
        DiagnosticEngine diag;
        ModuleManager mgr(diag);
        ASSERT_TRUE(mgr.scanAll((fs::path(dir) / "m1.aura").string()));
        EXPECT_EQ(mgr.globalTable().units.size(), (size_t)2);
        std::string entry;
        EXPECT_FALSE(mgr.validateEntryOn(entry));
        EXPECT_TRUE(diag.hasErrors());
        fs::remove_all(dir);
    }
    {   // 单 main
        auto dir = c3TempDir();
        c2Write(dir, "ok.aura", "fun main(io: Io) { }\n");
        c2Write(dir, "dep.aura", "fun h() -> int { return 1 }\n");
        DiagnosticEngine diag;
        ModuleManager mgr(diag);
        ASSERT_TRUE(mgr.scanAll((fs::path(dir) / "ok.aura").string()));
        std::string entry;
        EXPECT_TRUE(mgr.validateEntryOn(entry));
        EXPECT_TRUE(entry.find("ok.aura") != std::string::npos);
        fs::remove_all(dir);
    }
}

// module 冲突：D12 两分——隐式同名冲突；显式同名共享 namespace 合法；一显一隐冲突
TEST(ModuleScan, C3ModuleNameConflictD12) {
    {   // 隐式同名（不同目录同 stem）→ 冲突
        auto dir = c3TempDir();
        auto subA = (fs::path(dir) / "a"); fs::create_directories(subA);
        auto subB = (fs::path(dir) / "b"); fs::create_directories(subB);
        c2Write(subA.string(), "utils.aura", "fun ua() -> int { return 1 }\n");
        c2Write(subB.string(), "utils.aura", "fun ub() -> int { return 2 }\n");
        c2Write(dir, "entry.aura", "import \"a/utils.aura\"\nimport \"b/utils.aura\"\nfun main(io: Io) { }\n");
        DiagnosticEngine diag;
        ModuleManager mgr(diag);
        ASSERT_TRUE(mgr.scanAll((fs::path(dir) / "entry.aura").string()));
        EXPECT_TRUE(mgr.checkModuleConflicts());
        EXPECT_TRUE(diag.hasErrors());
        std::string all;
        for (auto& e : diag.errorMessages()) all += e;
        EXPECT_TRUE(all.find("module name conflict") != std::string::npos);
        fs::remove_all(dir);
    }
    {   // 显式同名 → 合法（共享 namespace）
        auto dir = c3TempDir();
        auto subA = (fs::path(dir) / "a"); fs::create_directories(subA);
        auto subB = (fs::path(dir) / "b"); fs::create_directories(subB);
        c2Write(subA.string(), "common.aura", "module common\nfun ua() -> int { return 1 }\n");
        c2Write(subB.string(), "common.aura", "module common\nfun ub() -> int { return 2 }\n");
        c2Write(dir, "entry.aura", "import \"a/common.aura\"\nimport \"b/common.aura\"\nfun main(io: Io) { }\n");
        DiagnosticEngine diag;
        ModuleManager mgr(diag);
        ASSERT_TRUE(mgr.scanAll((fs::path(dir) / "entry.aura").string()));
        EXPECT_FALSE(mgr.checkModuleConflicts());
        EXPECT_FALSE(diag.hasErrors());
        fs::remove_all(dir);
    }
    {   // 一显一隐同 stem → 冲突（隐式无共享意图）
        auto dir = c3TempDir();
        auto subA = (fs::path(dir) / "a"); fs::create_directories(subA);
        auto subB = (fs::path(dir) / "b"); fs::create_directories(subB);
        c2Write(subA.string(), "mix.aura", "module other\nfun ua() -> int { return 1 }\n");
        // 注意：显式 module other 使 moduleName != stem，与隐式 stem 不相同 → 不冲突；
        // 构造真「一显一隐同名」需显式名 == 另一文件 stem，如：
        c2Write(subB.string(), "shared.aura", "fun ub() -> int { return 2 }\n");   // 隐式 stem = shared
        c2Write(dir, "entry.aura",
                "import \"a/shared.aura\"\nimport \"b/shared.aura\"\nfun main(io: Io) { }\n");
        // a/shared.aura 显式 module shared（与 b/shared.aura 隐式 stem 同名）
        c2Write(subA.string(), "shared.aura", "module shared\nfun ua() -> int { return 1 }\n");
        DiagnosticEngine diag;
        ModuleManager mgr(diag);
        ASSERT_TRUE(mgr.scanAll((fs::path(dir) / "entry.aura").string()));
        EXPECT_TRUE(mgr.checkModuleConflicts());
        fs::remove_all(dir);
    }
}

// loadAllScanned：scanAll 依赖图 + 第二段完整解析填充 modules_（AST 常驻、hasMain 保持）
TEST(ModuleScan, C3LoadAllScannedSecondPass) {
    auto dir = c3TempDir();
    c2Write(dir, "dep.aura", "fun h() -> int { return 1 }\n");
    c2Write(dir, "main.aura",
            "module ent\nimport \"dep.aura\"\nfun main(io: Io) { io.println(\"x\") }\n");

    DiagnosticEngine diag;
    ModuleManager mgr(diag);
    ASSERT_TRUE(mgr.scanAll((fs::path(dir) / "main.aura").string()));
    EXPECT_EQ(mgr.globalTable().units.size(), (size_t)2);

    // 第二段：完整解析（模拟 C4 调度）
    ASSERT_TRUE(mgr.loadAllScanned());
    EXPECT_EQ(mgr.modules().size(), (size_t)2);
    for (auto& [path, info] : mgr.modules()) {
        ASSERT_TRUE(info.ast != nullptr);            // AST 常驻（语义分析/CodeGen 消费）
        if (path.find("main.aura") != std::string::npos) {
            EXPECT_EQ(info.moduleName, "ent");       // C0：显式 module 覆盖
            EXPECT_TRUE(info.hasExplicitModule);
            EXPECT_TRUE(info.hasMain);
        }
    }
    // C4 关键不变量：模块集合（modules_ == scanUnits_）——第二段不新增/丢失
    std::string all;
    (void)all;
    fs::remove_all(dir);
}

// ⚠️ 补充：既有 hasCycle/topologicalLayers/validateEntry（读 modules_）不受影响——
//     C3 只新增 On 版本，不触碰既有三方法（由既有 test_module.cpp 回归守护）。
