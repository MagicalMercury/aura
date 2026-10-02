// ============================================================
// test_codegen_meta.cpp — CodeGen 输出单元测试：符号元数据表（feature-18 P3）
//
// 依据：change.md §4.1（单文件 8 例）/ §4.2（多文件端到端 ⑨⑩ + 其 **O9/O34/O39 修订块**）
//        / §4.3（CMake 登记）/ §3.5(c)(d)(e)（收集挂钩 / thunk / metaImpl 生产点）
//        / §3.2（MetaCollector / MetaMerger）/ §3.3（MetaEmit 两形态）
//        / §11.8-O41-(g)（批 3 修复：thunk 表项悬空 ⇒ **后置 prune**）
//
// 本文件两类用例：
//   (A) 单文件 Inline 路径（①–⑧ + O41-(g) 回归）—— 直调 `cg.generate`（`compileSource` **不注入
//       collector / 不传 sourcePath**（B31）⇒ 这些断言在 compileSource 下恒不可能成立）。
//   (B) 多文件 External 端到端（⑨⑩）—— 需要**整套新基础设施** `compileMultiModuleWithMeta`
//       （🔴 O9 实锤：既有 `compileMultiModuleCg`（`test_codegen_concurrency_gc.cpp:448`）
//        **只对入口模块调 generate、不写盘、不调 g++** ⇒ B35「复刻 compileMultiFile 完整流程」失实）。
//
// ⚠️ 本批（批 3）**只编译不运行**：测试的运行（含下面 long-running 用例）留给批 4。
// ============================================================
#include "framework/test_framework.h"
#include "framework/test_helpers.h"

#include "CodeGen/MetaCollect.h"   // MetaCollector / MetaMerger / MetaSymbolRec
#include "CodeGen/MetaEmit.h"      // emitMetaHeader / emitMetaImpl

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace aura_test;

namespace {

// ============================================================
// 绝对路径来源（🔴 O34）
//
// `test/CMakeLists.txt:202` 是 `add_test(NAME aura_tests COMMAND aura_tests)`，**没有
// `WORKING_DIRECTORY`** ⇒ ctest 下 **CWD = `test/build`** ⇒ ⑩ 的 `-I runtime` /
// `runtime/build/libaura_rt.a` **两处相对路径都解析失败**。
// 裁定（change.md §4.2-O34）：**修法 ①（绝对路径）**，而不是给 `add_test` 加 WORKING_DIRECTORY
// （那会改变既有用例的运行目录 ⇒ 有外溢风险）。
//
// ⚠️ 「二选一，写死一处」（§4.2 明写）：本档取 **CMake `target_compile_definitions`**
//    —— `AURA_PROJECT_ROOT` **已经**由 `test/CMakeLists.txt:190-192` 定义并被
//    `framework/test_main.cpp:21-27`、`integration/test_examples.cpp:20` 消费 ⇒ **零新增改动**。
// ============================================================
#ifndef AURA_PROJECT_ROOT
#define AURA_PROJECT_ROOT "."
#endif
const char* const kRepoRoot = AURA_PROJECT_ROOT;

// ============================================================
// 小工具
// ============================================================

// needle 在 haystack 中的出现次数
int countOccurrences(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return 0;
    int n = 0;
    for (size_t p = haystack.find(needle); p != std::string::npos;
         p = haystack.find(needle, p + needle.size()))
        ++n;
    return n;
}

// 反斜杠翻倍（**测试侧独立复算**，不复用被测的 escapeStringLiteral ⇒ 不循环自证）
std::string doubleBackslashes(const std::string& s) {
    std::string out;
    out.reserve(s.size() * 2);
    for (char c : s) {
        if (c == '\\') out += "\\\\";
        else           out += c;
    }
    return out;
}

// 取 `text` 中包含 `needle` 的**整行**
std::string rowContaining(const std::string& text, const std::string& needle) {
    size_t p = text.find(needle);
    if (p == std::string::npos) return std::string();
    size_t s = text.rfind('\n', p);
    s = (s == std::string::npos) ? 0 : s + 1;
    size_t e = text.find('\n', p);
    if (e == std::string::npos) e = text.size();
    return text.substr(s, e - s);
}

// 只在某个段（如 `kSymbolTable[] = {`）之后取行 —— 帧表在前会先命中同名符号
std::string sectionRow(const std::string& text, const std::string& sectionMarker,
                       const std::string& needle) {
    size_t s = text.find(sectionMarker);
    if (s == std::string::npos) return std::string();
    return rowContaining(text.substr(s), needle);
}

// `kSymbolTable` 段：按表序抽取每行的首个字符串字面量（= 符号名）
std::vector<std::string> symbolTableNames(const std::string& meta) {
    std::vector<std::string> names;
    const std::string marker = "kSymbolTable[] = {";
    size_t p = meta.find(marker);
    if (p == std::string::npos) return names;
    p = meta.find('\n', p);
    while (p != std::string::npos) {
        size_t e = meta.find('\n', p + 1);
        const std::string line =
            meta.substr(p + 1, (e == std::string::npos ? meta.size() : e) - (p + 1));
        if (line.find("};") != std::string::npos) break;
        size_t q1 = line.find('"');
        if (q1 != std::string::npos) {
            size_t q2 = line.find('"', q1 + 1);
            if (q2 != std::string::npos) names.push_back(line.substr(q1 + 1, q2 - q1 - 1));
        }
        if (e == std::string::npos) break;
        p = e;
    }
    return names;
}

// 解析 `kSymbolCount   = N;` 的 N（失败 = -1）
long symbolCountValue(const std::string& meta) {
    size_t p = meta.find("kSymbolCount");
    if (p == std::string::npos) return -1;
    p = meta.find('=', p);
    if (p == std::string::npos) return -1;
    return std::strtol(meta.c_str() + p + 1, nullptr, 10);
}

// 从 SymbolInfo 表行里取 defLine（第 3 字段：`{ name, file, DEFLINE, SymbolKind::… }`）
bool parseDefLine(const std::string& row, unsigned long& out) {
    size_t k = row.find("SymbolKind");
    if (k == std::string::npos) return false;
    size_t c1 = row.rfind(',', k);
    if (c1 == std::string::npos || c1 == 0) return false;
    size_t c0 = row.rfind(',', c1 - 1);
    if (c0 == std::string::npos) return false;
    std::string num = row.substr(c0 + 1, c1 - c0 - 1);
    size_t a = num.find_first_not_of(" \t");
    if (a == std::string::npos) return false;
    size_t b = num.find_last_not_of(" \t");
    num = num.substr(a, b - a + 1);
    if (num.empty()) return false;
    for (char c : num)
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    out = std::strtoul(num.c_str(), nullptr, 10);
    return true;
}

// ============================================================
// 🔴 O41-(g)（批 3）**不变量自查**工具
//
//   「`kSymbolTable` 每条记录的 `materialize` 都指向本 TU 内**真实存在**的 thunk」
//
// 做法：从 `kSymbolTable` 段抽 `&<thunkRef>`（含 `ns::` 限定），取短名，断言该短名在
// **本模块 impl**（多文件时 = 各模块 impl 的拼接）里确有**定义**（`name(const aura_rt::CallArg* recv)`
// 形——注意定义行尾是 ` {`、声明行尾是 `;`，此处只匹配签名前缀，两者都能命中，故再断言 `{`）。
// ============================================================
bool everyMaterializeHasThunk(const std::string& meta, const std::string& impl) {
    const std::string marker = "kSymbolTable[] = {";
    size_t s = meta.find(marker);
    if (s == std::string::npos) return false;
    const std::string seg = meta.substr(s);
    int checked = 0;
    size_t p = 0;
    while ((p = seg.find('&', p)) != std::string::npos) {
        size_t e = seg.find(' ', p);
        if (e == std::string::npos) break;
        std::string ref = seg.substr(p + 1, e - p - 1);
        size_t c = ref.rfind("::");
        const std::string name = (c == std::string::npos) ? ref : ref.substr(c + 2);
        if (name.rfind("_aura_mat_", 0) == 0) {
            ++checked;
            const std::string defNeedle = name + "(const aura_rt::CallArg* recv) {";
            if (impl.find(defNeedle) == std::string::npos) return false;
        }
        p = e;
    }
    return checked > 0;
}

// ============================================================
// (A) 单文件路径 helper：直调 `cg.generate`（可注入 collector / 可传 sourcePath）
// ============================================================
struct MetaGenOut {
    Aura::CompileUnit                   unit;
    std::unique_ptr<Aura::MetaCollector> meta;   // nullptr = 未注入
    bool                                ok = false;   // true = Sema 无错、generate 已跑
};

MetaGenOut genWithMeta(const std::string& src,
                       Aura::DiagnosticEngine& diag,
                       bool injectCollector,
                       const std::string& sourcePath = std::string(),
                       const std::string& moduleName = "main",
                       const std::string& nsName = std::string(),
                       Aura::CodeGenConfig::MetaMode mode = Aura::CodeGenConfig::MetaMode::Inline) {
    MetaGenOut out;
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
    // ⚠️ SemAnalyzer 必须**存活到 generate 返回**：Sema 把 AST `inferredType` 指向
    //    analyzer 的 `typeStore_`，提前析构 ⇒ 悬垂（test_codegen_try.cpp:211-213 踩过）。
    Aura::SemAnalyzer sema(diag);
    (void)sema.analyze(*program);
    if (diag.hasErrors()) return out;

    Aura::CodeGenConfig cfg;
    cfg.metaMode = mode;
    // 🔴 O7（change.md §3.6a）：模块身份**构造时传入**（单文件 nsName="" ⇒ 不能靠 unit.nsName）
    if (injectCollector) {
        out.meta = std::make_unique<Aura::MetaCollector>(
            sourcePath, Aura::ModuleManager::sanitizeId(moduleName));
    }
    Aura::CodeGenerator cg(diag);
    out.unit = cg.generate(*program, moduleName, {}, nsName, cfg,
                           {}, {}, sourcePath, Aura::MetadataSink::of(out.meta.get()));
    out.ok = true;
    return out;
}

// 常用源码片段
const char* const kSrcSimple =
    "fun probe_fn(a: int) -> int { return a + 1 }\n"
    "fun main(io: Io) { io.println(str(probe_fn(1))) }\n";

// ============================================================
// (B) 多文件 helper：`compileMultiModuleWithMeta`
//
// 复刻 `main.cpp` `compileMultiFile` 的**真实链路**（§4.2-O9 展开的 ①–⑤）：
//   ① scanAll + checkModuleConflicts + hasCycleOn + loadAllScanned + topologicalLayersOn
//   ② 确定性模块序 = 层序 × **同层路径字典序**（复刻 main.cpp:116-125 flattenLayersDeterministic）
//   ③ 逐模块 SemAnalyzer（**保持存活到 CodeGen 之后**）+ 按层注入 exports + 主线程 merge 诊断
//   ④ 逐模块 `MetaCollector`（**线程私有语义**）+ `cg.generate(..., External, &local)`
//   ⑤ 主线程按 orderedModules 序 `MetaMerger::addModule` + `finalize()` + 两段文本（**不写盘**）
//
// ⚠️ 默认**单线程**（测试无需并行）。🔴 O35 要求的**并行回归变体**（O1 的真回归）
//    本批**未实现**（用例脆弱性退路）：见 change.md §4.2-O35 的明文退路
//    「保留单线程 ⑤ + 在 §5 验证步加一条**手工**并行对比（记录在实施批的回报里）」
//    —— 手工对比结果见批 3 回报 §7（`aurac -S -j1` vs `-j4` 产物逐字节一致）。
// ============================================================
struct MultiMetaResult {
    std::map<std::string, Aura::CompileUnit> units;    // 逐模块（含 metaImpl/thunk）
    std::string metaHeader;                            // aura.meta.h 文本
    std::string metaImpl;                              // aura.meta.cpp 文本
    std::vector<std::string> orderedModules;           // 层序 × 同层字典序
    std::string error;                                 // 非空 = 早退原因
};

MultiMetaResult compileMultiModuleWithMeta(const std::string& entryPath,
                                           Aura::DiagnosticEngine& diag) {
    MultiMetaResult out;
    Aura::ModuleManager mgr(diag);
    mgr.loadBuiltinAurai();

    // ---- ① 声明级扫描 + 汇总段判定（main.cpp:281-304 同形）----
    if (!mgr.scanAll(entryPath))       { out.error = "scanAll failed";            return out; }
    if (mgr.checkModuleConflicts())    { out.error = "module conflicts";          return out; }
    if (mgr.hasCycleOn())              { out.error = "dependency cycle";          return out; }
    if (!mgr.loadAllScanned())         { out.error = "loadAllScanned failed";     return out; }
    const std::vector<std::vector<std::string>> layers = mgr.topologicalLayersOn();

    // ---- ② 确定性模块序（复刻 main.cpp:116-125 的 flattenLayersDeterministic）----
    for (const auto& layer : layers) {
        std::vector<std::string> sorted = layer;
        std::sort(sorted.begin(), sorted.end());
        for (const std::string& p : sorted) {
            auto* mod = mgr.moduleAt(p);
            if (!mod || mod->isBuiltin) continue;
            out.orderedModules.push_back(p);
        }
    }
    if (out.orderedModules.empty()) { out.error = "no user modules"; return out; }

    // ---- ③ 逐模块 Sema（保持存活）+ 诊断 merge（main.cpp:328-382 同形，单线程）----
    std::map<std::string, std::unique_ptr<Aura::DiagnosticEngine>> moduleDiags;
    std::map<std::string, std::unique_ptr<Aura::SemAnalyzer>>      moduleSemas;
    for (const auto& layer : layers) {
        std::vector<Aura::ModuleInfo*> tasks;
        for (const std::string& p : layer) {
            auto* mod = mgr.moduleAt(p);
            if (!mod || mod->isBuiltin || !mod->ast) continue;
            tasks.push_back(mod);
            auto md = std::make_unique<Aura::DiagnosticEngine>();
            md->setSourceView(Aura::readFile(mod->sourcePath));
            md->setFileName(mod->sourcePath);
            moduleDiags[mod->sourcePath] = std::move(md);
        }
        for (Aura::ModuleInfo* mod : tasks) {
            auto sema = std::make_unique<Aura::SemAnalyzer>(*moduleDiags[mod->sourcePath]);
            for (const std::string& depPath : mod->deps) {
                auto depIt = mgr.modules().find(depPath);
                if (depIt == mgr.modules().end()) continue;
                std::string alias;
                for (const auto& imp : mod->imports)
                    if (imp.path == depPath) { alias = imp.alias; break; }
                sema->importExports(alias, depIt->second.exports);
            }
            (void)sema->analyze(*mod->ast);
            mod->exports = sema->extractExports();
            moduleSemas[mod->sourcePath] = std::move(sema);
        }
        for (Aura::ModuleInfo* mod : tasks) diag.mergeFrom(*moduleDiags[mod->sourcePath]);
    }
    if (diag.hasErrors()) { out.error = "sema errors"; return out; }

    // ---- ④ 逐模块 CodeGen（External 模式；每模块一个 collector）----
    std::map<std::string, std::unique_ptr<Aura::MetaCollector>> collectors;
    for (const std::string& modPath : out.orderedModules) {
        auto* mod = mgr.moduleAt(modPath);
        if (!mod || !mod->ast) continue;

        // import 信息 + 跨模块默认参数/形参 SemType 表（main.cpp:420-464 同形）
        std::vector<Aura::CodeGenImport> cgImports;
        for (const auto& imp : mod->imports) {
            Aura::CodeGenImport ci;
            ci.path = imp.path; ci.alias = imp.alias; ci.isBuiltin = imp.isBuiltin;
            if (!imp.isBuiltin) {
                auto it = mgr.modules().find(imp.path);
                if (it != mgr.modules().end()) {
                    ci.nsName  = it->second.nsName;
                    ci.modName = it->second.moduleName;
                }
            } else {
                ci.modName = imp.path;
            }
            cgImports.push_back(ci);
        }
        Aura::CodeGenerator::CrossModuleDefaults crossDefaults;
        Aura::CodeGenerator::CrossModuleParamSemTypes crossParamSemTypes;
        for (const auto& imp : mod->imports) {
            if (imp.isBuiltin) continue;
            auto it = mgr.modules().find(imp.path);
            if (it == mgr.modules().end()) continue;
            const std::string nsKey = imp.alias.empty() ? it->second.moduleName : imp.alias;
            auto& modDefaults  = crossDefaults[nsKey];
            auto& modSemTypes  = crossParamSemTypes[nsKey];
            for (const auto& [fnName, f] : it->second.exports.funcs) {
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

        // 🔴 O1：collector **在本模块的作用域内构造**（线程私有语义；此处单线程，形态与 main.cpp:471 同）
        collectors[modPath] = std::make_unique<Aura::MetaCollector>(
            mod->sourcePath, Aura::ModuleManager::sanitizeId(Aura::stemOf(mod->sourcePath)));

        Aura::CodeGenerator cg(*moduleDiags[mod->sourcePath]);   // per-module diag（main.cpp:475 同）
        Aura::CodeGenConfig cfg;
        cfg.metaMode = Aura::CodeGenConfig::MetaMode::External;
        out.units[modPath] = cg.generate(*mod->ast, mod->moduleName, cgImports, mod->nsName,
                                         cfg, crossDefaults, crossParamSemTypes,
                                         mod->sourcePath, Aura::MetadataSink::of(collectors[modPath].get()));
    }

    // ---- ⑤ 主线程合并 + 两段文本（main.cpp:557-569 同形；本 helper **不写盘**）----
    Aura::MetaMerger merger;
    for (const std::string& p : out.orderedModules) {
        auto it = collectors.find(p);
        if (it == collectors.end() || !it->second) continue;
        merger.addModule(*it->second);
    }
    merger.finalize();
    out.metaHeader = Aura::MetaEmit::emitMetaHeader(merger);
    out.metaImpl   = Aura::MetaEmit::emitMetaImpl(merger);
    return out;
}

// ============================================================
// 临时目录 helper（🔴 O39-①：RAII 收尾）
//
// 断言失败 / 抛异常时也要清理（否则 Temp 堆积）⇒ 用 scope-guard 而非「末尾手工 remove_all」。
// ⚠️ 目录名带 pid（并发 runner 不互撞）；路径**绝对**（O34）。
// ============================================================
std::string makeMetaTempDir(const std::string& tag) {
    auto dir = std::filesystem::temp_directory_path() /
               ("f18p3_" + tag + "_" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir.string();
}

class TempDirGuard {
public:
    explicit TempDirGuard(std::string dir) : dir_(std::move(dir)) {}
    ~TempDirGuard() {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);   // 任何路径（含 EXPECT 失败）都清理
    }
    TempDirGuard(const TempDirGuard&) = delete;
    TempDirGuard& operator=(const TempDirGuard&) = delete;
    const std::string& path() const { return dir_; }
private:
    std::string dir_;
};

// 两模块 + 入口的最小多文件工程（写入 dir，返回入口 .aura 绝对路径）
std::string writeTwoModuleProject(const std::string& dir) {
    Aura::writeFile(dir + "/mod_a.aura", "pub fun fa(x: int) -> int { return x + 1 }\n");
    Aura::writeFile(dir + "/mod_b.aura", "pub fun fb(x: int) -> int { return x + 2 }\n");
    const std::string entry = dir + "/main.aura";
    Aura::writeFile(entry,
        "import \"mod_a.aura\" as a\n"
        "import \"mod_b.aura\" as b\n"
        "fun main(io: Io) { io.println(str(a.fa(1) + b.fb(2))) }\n");
    return entry;
}

} // namespace

// ============================================================
// ① InlineModeEmitsTables —— 默认 Inline ⇒ metaImpl 含表；header 不注入 include
// ============================================================
TEST(CodeGenMeta, InlineModeEmitsTables) {
    Aura::DiagnosticEngine diag;
    auto r = genWithMeta(kSrcSimple, diag, /*injectCollector=*/true);
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.meta != nullptr);
    // §4.1-①：表定义段三件套
    EXPECT_CONTAINS(r.unit.metaImpl, "kSymbolTable");
    EXPECT_CONTAINS(r.unit.metaImpl, "kFrameTable");
    EXPECT_CONTAINS(r.unit.metaImpl, "kSymbolCount");
    // 单文件 Inline：表自含在本 TU ⇒ **不得**注入 aura.meta.h（§0.3 回归面红线）
    EXPECT_NOT_CONTAINS(r.unit.header, "#include \"aura.meta.h\"");
}

// ============================================================
// ② InlineModeHeaderUnchanged —— ⭐ 回归面红线：同一 .aura 源，「注入 collector」与
//    「不注入 collector」两次生成的 `unit.header` **逐字节一致**
//    ⚠️ P4a 批 2 订正：P3 原措辞「P3 不得改单文件 header」已**作废** —— P4a 的 A7 帧注入对
//    **泛型函数**（体入 header）必然改写单文件 header；且 Inline 段新增的 `#include "meta.h"`
//    按 `metaMode_` 分支添加 ⇒ 本节两侧**同加** ⇒ 断言仍成立。
//    本用例的**真实语义** = 「collector 的开关**不**影响 header」（这才是要守的红线）。
// ============================================================
TEST(CodeGenMeta, InlineModeHeaderUnchanged) {
    Aura::DiagnosticEngine dWith, dWithout;
    auto with    = genWithMeta(kSrcSimple, dWith,    /*injectCollector=*/true);
    auto without = genWithMeta(kSrcSimple, dWithout, /*injectCollector=*/false);
    ASSERT_TRUE(with.ok);
    ASSERT_TRUE(without.ok);
    EXPECT_EQ(with.unit.header, without.unit.header);     // 逐字节一致
    // 差异只应落在 metaImpl（注入侧有表、未注入侧为空）
    EXPECT_FALSE(with.unit.metaImpl.empty());
    EXPECT_TRUE(without.unit.metaImpl.empty());
}

// ============================================================
// ③ ExternalModeInjectsInclude —— External ⇒ header 以 aura.meta.h 开头；metaImpl 为空
//    （表由 aura.meta.cpp 承载）
// ============================================================
TEST(CodeGenMeta, ExternalModeInjectsInclude) {
    Aura::DiagnosticEngine diag;
    auto r = genWithMeta(kSrcSimple, diag, /*injectCollector=*/true,
                         /*sourcePath=*/std::string(), /*moduleName=*/"main",
                         /*nsName=*/std::string(),
                         Aura::CodeGenConfig::MetaMode::External);
    ASSERT_TRUE(r.ok);
    const std::string& h = r.unit.header;
    // ⚠️ 用字面量长度而非硬编码数字（`#include "aura.meta.h"` + '\n' = 23 字节）
    const std::string expectPrefix = "#include \"aura.meta.h\"\n";
    ASSERT_TRUE(h.size() >= expectPrefix.size());
    EXPECT_EQ(h.substr(0, expectPrefix.size()), expectPrefix);
    EXPECT_TRUE(r.unit.metaImpl.empty());
}

// ============================================================
// ④ SymbolTableHasFileAndDefLine —— 表项的 file = 传入的真实路径；defLine = 声明行（**非 0**）
//    ⚠️ `compileSource`（test_helpers.h:117-137）**只传 5 参**（sourcePath 缺省 ""）且
//       **不注入 collector** ⇒ 本用例必须直调 `cg.generate(..., sourcePath, &mc)`。
// ============================================================
TEST(CodeGenMeta, SymbolTableHasFileAndDefLine) {
    Aura::DiagnosticEngine diag;
    const std::string winPath = "D:\\you\\Aura\\probe\\meta\\sym.aura";
    auto r = genWithMeta(kSrcSimple, diag, /*injectCollector=*/true, winPath);
    ASSERT_TRUE(r.ok);
    const std::string row = sectionRow(r.unit.metaImpl, "kSymbolTable[] = {", "\"probe_fn\"");
    ASSERT_TRUE(!row.empty());
    // file = 传入的真实路径（转义形态；独立复算期望值）
    EXPECT_CONTAINS(row, "\"" + doubleBackslashes(winPath) + "\"");
    // defLine = 声明行，且**非 0**（§4.1-④）
    unsigned long defLine = 0;
    ASSERT_TRUE(parseDefLine(row, defLine));
    EXPECT_NE(defLine, 0ul);
    // 附带证据：probe_fn 在第 1 行（ASTNode::line 为 1-based —— 实测形态）
    EXPECT_EQ(defLine, 1ul);
}

// ============================================================
// ⑤ MethodNameIsQualified —— 方法表项 `name == "Type.method"`；函数为**裸名**（裁定 ②）
// ============================================================
TEST(CodeGenMeta, MethodNameIsQualified) {
    Aura::DiagnosticEngine diag;
    auto r = genWithMeta(
        "type Probe = { v: int }\n"
        "fun (self Probe) bump(d: int) -> int { return self.v + d }\n"
        "fun plainFn(a: int) -> int { return a + 1 }\n"
        "fun main(io: Io) { io.println(str(plainFn(1))) }\n",
        diag, /*injectCollector=*/true);
    ASSERT_TRUE(r.ok);
    // 方法：限定名 ReceiverType.method
    EXPECT_CONTAINS(r.unit.metaImpl, "\"Probe.bump\"");
    // 函数：裸名（行首即 `"plainFn", ` —— 不得带任何前缀限定）
    EXPECT_CONTAINS(r.unit.metaImpl, "\"plainFn\", ");
    EXPECT_NOT_CONTAINS(r.unit.metaImpl, "\"Probe.plainFn\"");
}

// ============================================================
// ⑥ EscapeInFileLiteral —— sourcePath 含 `\` ⇒ 产物中 file 字面量为**转义形态**（E1 护栏）
//    （未转义 ⇒ GCC unknown escape sequence + 丢反斜杠）
// ============================================================
TEST(CodeGenMeta, EscapeInFileLiteral) {
    Aura::DiagnosticEngine diag;
    const std::string winPath = "D:\\you\\Aura\\probe\\esc\\x.aura";
    auto r = genWithMeta(kSrcSimple, diag, /*injectCollector=*/true, winPath);
    ASSERT_TRUE(r.ok);
    const std::string symRow = sectionRow(r.unit.metaImpl, "kSymbolTable[] = {", "\"probe_fn\"");
    ASSERT_TRUE(!symRow.empty());
    EXPECT_CONTAINS(symRow, "\"" + doubleBackslashes(winPath) + "\"");
    // 未转义形态不得出现
    EXPECT_NOT_CONTAINS(symRow, "\"" + winPath + "\"");
    // 帧表的 file 列同源 ⇒ 同样必须转义
    const std::string frameRow = sectionRow(r.unit.metaImpl, "kFrameTable[]  = {", "\"probe_fn\"");
    ASSERT_TRUE(!frameRow.empty());
    EXPECT_CONTAINS(frameRow, "\"" + doubleBackslashes(winPath) + "\"");
    EXPECT_NOT_CONTAINS(frameRow, "\"" + winPath + "\"");
}

// ============================================================
// ⑦ IndexDeterministic —— 同一输入**连续生成两次** ⇒ `unit.metaImpl` 逐字节一致
//    （索引确定性的最小验证；两次生成用**各自独立的** CodeGenerator + collector，
//      即「两次编译运行」语义 —— `erasedCounter_` 的单调性只在同一实例内累加）
// ============================================================
TEST(CodeGenMeta, IndexDeterministic) {
    const char* const src =
        "type Probe = { v: int }\n"
        "fun probe_fn(a: int) -> int { return a + 1 }\n"
        "fun probe_fn2(b: int, c: int) -> int { return b + c }\n"
        "fun main(io: Io) { io.println(str(probe_fn(1) + probe_fn2(1, 2))) }\n";
    Aura::DiagnosticEngine d1, d2;
    auto first  = genWithMeta(src, d1, /*injectCollector=*/true);
    auto second = genWithMeta(src, d2, /*injectCollector=*/true);
    ASSERT_TRUE(first.ok);
    ASSERT_TRUE(second.ok);
    EXPECT_FALSE(first.unit.metaImpl.empty());
    EXPECT_EQ(first.unit.metaImpl, second.unit.metaImpl);
    // 索引连续无重号（0..kSymbolCount-1 的分配序 ⇒ 表序即分配序）
    const std::vector<std::string> names = symbolTableNames(first.unit.metaImpl);
    EXPECT_EQ(static_cast<long>(names.size()), symbolCountValue(first.unit.metaImpl));
    const std::set<std::string> uniq(names.begin(), names.end());
    EXPECT_EQ(uniq.size(), names.size());
}

// ============================================================
// ⑧ ThunkEmittedForSymbols —— `unit.impl` 含 thunk 定义
//    🔴 O33 修订（终审）：thunk 的 namespace 包裹**视 `nsName` 而定** ——
//       单测路径（`nsName=""`）⇒ **全局裸符号** `_aura_mat_main_0`，**无**包裹。
// ============================================================
TEST(CodeGenMeta, ThunkEmittedForSymbols) {
    Aura::DiagnosticEngine diag;
    // moduleName = "main" ⇒ nsStem = sanitizeId("main") = "main"（🔴 O7：模块身份构造时传入）
    auto r = genWithMeta(kSrcSimple, diag, /*injectCollector=*/true);
    ASSERT_TRUE(r.ok);
    const std::string& impl = r.unit.impl;
    const std::string sig = "_aura_mat_main_0(const aura_rt::CallArg* recv)";
    EXPECT_CONTAINS(impl, sig);
    // O33：单测路径是**全局裸符号** ⇒ 该行不得是 namespace 包裹形态
    const std::string line = rowContaining(impl, sig);
    ASSERT_TRUE(!line.empty());
    EXPECT_TRUE(line.rfind("namespace ", 0) != 0);
    // 表项的 materialize 指向**同一个** thunk（O41-(g) 不变量的正向面）
    const std::string row = sectionRow(r.unit.metaImpl, "kSymbolTable[] = {", "\"probe_fn\"");
    ASSERT_TRUE(!row.empty());
    EXPECT_CONTAINS(row, "&_aura_mat_main_0");
}

// ============================================================
// 🔴 O41-(g) **回归用例**（批 3 修复的护栏）
//
// 形参类型不可物化 ⇒ 该符号**不发 thunk**（`CodeGen.cpp` thunk 循环 `sigOk=false` ⇒ `continue`）。
// 修复前：表项**已登记**（`materialize = &_aura_mat_main_0`）⇒ 悬空引用 ⇒ 链接期
//   `undefined reference to _aura_mat_main_0`。
// 修复后：**后置 prune**（`MetaCollector::pruneUnmaterialized`）把该符号从表里删掉
//   ⇒ 「表里没有它」+「剩下的表项都有真 thunk」。
//
// ⚠️ 为什么 `Probe` 形参不可物化：Sema **不**把**形参** TypeExpr 的解析结果写回
//    `TypeExpr::inferredType`（全仓只有**返回类型**写回：`BodyChecker.cpp:144`/`:225`、
//    `ExprInferMisc.cpp:313`）⇒ 兜底只剩 `NamedType` 的 int/float/bool。
// ============================================================
TEST(CodeGenMeta, PruneDropsSymbolWithoutThunk) {
    Aura::DiagnosticEngine diag;
    auto r = genWithMeta(
        "type Probe = { v: int }\n"
        "fun noMat(p: Probe) -> int { return 1 }\n"     // ← 形参不可物化（seq 0）
        "fun okFn(a: int) -> int { return a + 1 }\n"    // ← 可物化（NamedType int 兜底；seq 1）
        "fun main(io: Io) { io.println(str(okFn(1))) }\n",
        diag, /*injectCollector=*/true);
    ASSERT_TRUE(r.ok);

    // ① 🔴 D4 改写（P4a 批 4）：原断言 `EXPECT_NOT_CONTAINS(metaImpl, "\"noMat\"")` 随
    //    **A1「prune 降级不删」**（change.md §3.1.2 修法 A / §3.1.5）**作废** —— 该符号
    //    **仍入表**（保编号），仅物化列降级为 `nullptr`（`thunkName` 清空 ⇒ emit 渲染裸 nullptr，
    //    见 A1b）。属**设计意图变更**，非「为绿而改」。依据：change.md §3.1.5 + §11.7.2-D4。
    EXPECT_CONTAINS(r.unit.metaImpl, "\"noMat\"");
    {
        const std::string noMatRow = sectionRow(r.unit.metaImpl, "kSymbolTable[] = {", "\"noMat\"");
        ASSERT_TRUE(!noMatRow.empty());                       // 仍在符号表
        EXPECT_CONTAINS(noMatRow, "nullptr },");              // materialize == nullptr（不带 &）
        EXPECT_NOT_CONTAINS(noMatRow, "&_aura_mat_");
        EXPECT_CONTAINS(sectionRow(r.unit.metaImpl, "kFrameTable[]  = {", "\"noMat\""),
                        "\"noMat\"");                         // 帧表项也仍在（name 可渲染）
    }
    // ② 它本来就没被发射 thunk（跳过侧）——回归把「表项降级」与「thunk 不存在」绑在一起
    EXPECT_NOT_CONTAINS(r.unit.impl, "noMat(const aura_rt::CallArg* recv)");

    // ③ 可物化的符号在表里，且表项指向**真实存在**的 thunk
    const std::string row = sectionRow(r.unit.metaImpl, "kSymbolTable[] = {", "\"okFn\"");
    ASSERT_TRUE(!row.empty());
    // ⚠️ 序号保真：noMat 占 seq 0、okFn 占 seq 1 —— prune **不回退** `seqSymbol_`（否则重名）
    EXPECT_CONTAINS(row, "&_aura_mat_main_1");
    EXPECT_CONTAINS(r.unit.impl, "_aura_mat_main_1(const aura_rt::CallArg* recv) {");

    // ④ **不变量**：表里每条 materialize 的 thunk 都在本 TU 有定义
    EXPECT_TRUE(everyMaterializeHasThunk(r.unit.metaImpl, r.unit.impl));
}

// ============================================================
// ⑨ MultiModuleGeneratesAuraMeta（§4.2）—— 多模块 External：产出 aura.meta.h/.cpp
//    表内容含**两个模块**的符号；跨模块索引**连续且无重号**
// ============================================================
TEST(CodeGenMeta, MultiModuleGeneratesAuraMeta) {
    const std::string dir = makeMetaTempDir("multi_gen");
    TempDirGuard guard(dir);                                   // 🔴 O39-①
    const std::string entry = writeTwoModuleProject(dir);

    Aura::DiagnosticEngine diag;
    MultiMetaResult r = compileMultiModuleWithMeta(entry, diag);
    ASSERT_FALSE(diag.hasErrors());
    ASSERT_TRUE(r.error.empty());
    ASSERT_TRUE(r.orderedModules.size() >= 3);                 // 入口 + 两个依赖模块

    // aura.meta.h = **纯 extern 声明**（红线①：不含定义）
    EXPECT_FALSE(r.metaHeader.empty());
    EXPECT_CONTAINS(r.metaHeader, "extern const SymbolInfo  kSymbolTable[];");
    EXPECT_CONTAINS(r.metaHeader, "extern const uint32_t    kSymbolCount;");
    EXPECT_NOT_CONTAINS(r.metaHeader, "kSymbolTable[] =");

    // aura.meta.cpp = **唯一定义** + thunk 前置声明段
    EXPECT_FALSE(r.metaImpl.empty());
    EXPECT_CONTAINS(r.metaImpl, "kSymbolTable[] = {");

    // 两个模块的符号都在（表段）
    EXPECT_CONTAINS(r.metaImpl, "\"fa\", ");
    EXPECT_CONTAINS(r.metaImpl, "\"fb\", ");

    // 跨模块索引：条数 == kSymbolCount（连续分配），且**无重号**
    const std::vector<std::string> names = symbolTableNames(r.metaImpl);
    EXPECT_GE(names.size(), 2u);
    EXPECT_EQ(static_cast<long>(names.size()), symbolCountValue(r.metaImpl));
    const std::set<std::string> uniq(names.begin(), names.end());
    EXPECT_EQ(uniq.size(), names.size());
    EXPECT_EQ(uniq.count("fa"), 1u);
    EXPECT_EQ(uniq.count("fb"), 1u);

    // thunk 前置声明段 = **非空 `thunkName`** 的记录各一份（表项要取地址）。
    // ⚠️ D4 改写（P4a 批 4）：原断言 `== names.size()` 随 **A2（main 也收集）** 起**必红**
    //    —— `names` 现在**含 main**，而 main 的 `thunkName` 为空（A1 降级不删 ⇒ A1b 跳过前置声明）
    //    ⇒ 恒差 1。改后的判据 = 「全部记录数 − 物化列为 `nullptr` 的记录数」
    //    （`nullptr },` 只出现在 kSymbolTable 的空物化行：帧表/类型表的行尾是 `0 },`）。
    //    依据：change.md §11.7.2-D4（A2 的连带；属**修正错误判据**，非为绿而改）。
    const int nullMatRows = countOccurrences(r.metaImpl, "nullptr },");
    EXPECT_GE(nullMatRows, 1);                                  // 至少 main 一行（无 thunk）
    EXPECT_EQ(countOccurrences(r.metaImpl, "(const aura_rt::CallArg* recv);"),
              static_cast<int>(names.size()) - nullMatRows);

    // 🔴 O41-(g) 不变量（跨模块面）：表里的 `&<ns>::<thunk>` 都指向**本 TU 存在**的定义
    //    （定义在各模块自己的 .cpp 的 impl 段 ⇒ 此处拼接所有模块 impl 后自查）
    std::string allImpls;
    for (const auto& [path, unit] : r.units) allImpls += unit.impl;
    EXPECT_TRUE(everyMaterializeHasThunk(r.metaImpl, allImpls));
}

// ============================================================
// ⑩ MultiModuleLinksAndRuns（§4.2）—— ⭐ **端到端**：真落盘 + 真调 `g++` 编译 + 链接
//
// （本用例 **long-running**：含真 `g++` 调用，秒级。）
//
// 这是 §6-R1 的**唯一**判据：MinGW 下跨 TU `extern const 表 + extern 计数` 的 ODR 形态
// （B7：无既有先例）只能靠「真链接一次」证明。
//
// ⚠️ O34：**全绝对路径**（`add_test` 无 WORKING_DIRECTORY ⇒ ctest 下 CWD = test/build）。
// ⚠️ O39：`TempDirGuard` RAII 包 tmpDir（断言失败/异常也清理）。
// ⚠️ 语义须与 `main.cpp:588-595` 一致（`-I <outDir> -I runtime` + `libaura_rt.a`）
//    —— O34 后形式改为绝对路径，**语义（两个 include 目录 + 库）不变**。
//
// 🔴 **已知阻塞（与本档无关，实测复现）**：多文件真链接会被**既有**的
//    `aura_rt::g_syncStack` emutls TLS init 重复定义阻塞（bug-86 家族）：
//      ld.exe: mod_a.aura.cpp: multiple definition of `TLS init function for aura_rt::g_syncStack';
//              main.aura.cpp: first defined here
//      collect2.exe: error: ld returned 1 exit status
//    根因：`runtime/builtin/sync_context.h:170` 的 `inline thread_local std::vector<SyncContext*>
//    g_syncStack;` —— MinGW 为**每个包含该头的 TU** 各发一份 TLS init function（非 COMDAT 合并）。
//    ⚠️ **本用例刻意不**加 `-Wl,--allow-multiple-definition`（那是**掩盖**，不是修复）
//    ⇒ 该断言当前**如实报红**；处置建议见批 3 回报 §7（登记独立缺陷 / 测试基建）。
// ============================================================
TEST(CodeGenMeta, MultiModuleLinksAndRuns) {
    const std::string dir = makeMetaTempDir("meta_link");
    TempDirGuard guard(dir);
    const std::string entry = writeTwoModuleProject(dir);

    Aura::DiagnosticEngine diag;
    MultiMetaResult r = compileMultiModuleWithMeta(entry, diag);
    ASSERT_FALSE(diag.hasErrors());
    ASSERT_TRUE(r.error.empty());
    ASSERT_TRUE(r.orderedModules.size() >= 3);

    // ---- 真落盘（形态与 main.cpp:486-503 / :566-569 一致）----
    Aura::writeFile(dir + "/aura.meta.h",   r.metaHeader);
    Aura::writeFile(dir + "/aura.meta.cpp", r.metaImpl);
    std::vector<std::string> cppPaths;
    cppPaths.push_back(dir + "/aura.meta.cpp");
    bool hasMain = false;
    for (const std::string& modPath : r.orderedModules) {
        auto it = r.units.find(modPath);
        ASSERT_TRUE(it != r.units.end());
        const std::string stem = Aura::stemOf(modPath);
        Aura::writeFile(dir + "/" + stem + ".aura.h", it->second.header);
        std::string body = "#include \"" + stem + ".aura.h\"\n";
        body += it->second.impl;
        if (!it->second.footer.empty()) { body += "\n"; body += it->second.footer; }
        Aura::writeFile(dir + "/" + stem + ".aura.cpp", body);
        cppPaths.push_back(dir + "/" + stem + ".aura.cpp");
        if (it->second.hasMain) hasMain = true;
    }
    ASSERT_TRUE(hasMain);

    // ---- 真调 g++（绝对路径；语义与 main.cpp:588-595 一致）----
    const std::string repo = kRepoRoot;
    std::string cmd = "g++ -std=gnu++20 -fcoroutines -w -I \"" + dir + "\" -I \"" +
                      repo + "/runtime\"";
    for (const std::string& c : cppPaths) cmd += " \"" + c + "\"";
    cmd += " \"" + repo + "/runtime/build/libaura_rt.a\" -o \"" + dir + "/a.exe\" 2>&1";
    const int rc = std::system(cmd.c_str());

    // R1 的唯一判据：链接成功（rc == 0）
    EXPECT_EQ(rc, 0);
}
