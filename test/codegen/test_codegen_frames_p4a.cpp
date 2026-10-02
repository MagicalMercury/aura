// ============================================================
// test/codegen/test_codegen_frames_p4a.cpp — feature-18 P4a 批 4：
//   元数据编号（T9–T12） + 生成码形态（C1–C5 / C9 / C10 / C11 / C-新 / C-新2）
//
// 依据：change.md §6.1（T9–T12）/ §6.2（C1–C11）/ §11.8.3（遗留 1、2）/ §11.9.2、§11.9.3。
// 被测实现（**本批不改** —— R1/R2 红线）：
//   src/CodeGen/DeclFun.cpp（A7 帧注入）/ StmtControl.cpp（A8 抛出点行号）/
//   StmtGen.cpp（A9 语句级调用点行号）/ MetaCollect.{h,cpp}（A1/A4）/ MetaEmit.cpp（A1b）/
//   runtime/logical_stack.h、runtime/meta.h（T1–T3 在 test/rt/test_logical_stack_p4a.cpp）。
//
// ⚠️ 分工与既有文件的边界（§3 禁碰清单）：`test/codegen/test_codegen_meta.cpp` 本批
//    **只**改 D4 的两处断言 ⇒ 本文件的元数据用例**自带**多模块 helper（下 §B），不引用它。
//
// ✅ **收窄批（2026-10-02）已清零批 4 的三条「必红」断言**：
//   · C4 后半：「无 throws 的纯函数 ⇒ 不注入」—— 层 1 已打通（A1 `fnThrows_` 索引）⇒ **转为正式断言**。
//   · C5 索引：「a[i] ⇒ 注入」—— `FirstCallLineScanner` 新增 `visit(const IndexExpr&)`
//     ⇒ §9-V19 缺口关闭，断言**恢复**。
//   · C10(i)：「该文件内 `std::string prefix` 只有一处定义」—— 该**字面口径本身无效**
//     （ExprCall.cpp 有 3 处同名局部变量）⇒ 批 4 已改以 (i-b)「`prefix = needAwait` 单点」为唯一判据。
//   ⇒ 另新增 C-新5（跳过不可抛、继续找后面的可抛）与 C-新6（未知 callee ⇒ 保守注入）两条回归用例。
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

#ifndef AURA_PROJECT_ROOT
#define AURA_PROJECT_ROOT "."
#endif
const std::string kRepoRoot = AURA_PROJECT_ROOT;

// ============================================================
// A. 文本工具（测试侧独立实现，不复用被测代码）
// ============================================================

int countOccurrences(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return 0;
    int n = 0;
    for (size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

// 取包含 needle 的**整行**
std::string rowContaining(const std::string& text, const std::string& needle) {
    const size_t p = text.find(needle);
    if (p == std::string::npos) return std::string();
    size_t s = text.rfind('\n', p);
    s = (s == std::string::npos) ? 0 : s + 1;
    size_t e = text.find('\n', p);
    if (e == std::string::npos) e = text.size();
    return text.substr(s, e - s);
}

// `marker` 起直到末尾的子串（用于「只在某段里找」）
std::string sectionFrom(const std::string& text, const std::string& marker) {
    const size_t p = text.find(marker);
    return (p == std::string::npos) ? std::string() : text.substr(p);
}

// 取 `sig` 之后、首个**顶格 `}`** 之前的文本 = 函数体（生成的函数体收尾 `}` 在行首）
std::string bodyOf(const std::string& text, const std::string& sig) {
    const size_t p = text.find(sig);
    if (p == std::string::npos) return std::string();
    const size_t start = p + sig.size();
    const size_t end = text.find("\n}", start);
    if (end == std::string::npos) return std::string();
    return text.substr(start, end - start);
}

// 解析 `key ... = N;`（失败 = -1）
long tableValue(const std::string& meta, const std::string& key) {
    size_t p = meta.find(key);
    if (p == std::string::npos) return -1;
    p = meta.find('=', p);
    if (p == std::string::npos) return -1;
    return std::strtol(meta.c_str() + p + 1, nullptr, 10);
}

// needle 首次出现处的 **1-based 行号**（失败 = -1）
int lineOfOccurrence(const std::string& src, const std::string& needle) {
    const size_t p = src.find(needle);
    if (p == std::string::npos) return -1;
    int line = 1;
    for (size_t i = 0; i < p; ++i)
        if (src[i] == '\n') ++line;
    return line;
}

// 路径归一：去转义（`\\` → `\`）+ 反斜杠→正斜杠（生成码里 file 列是转义形态）
std::string canon(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == '\\') { out += '/'; ++i; }
        else if (s[i] == '\\') out += '/';
        else out += s[i];
    }
    return out;
}

// ---- kSymbolTable 一行（name / file / defLine / materialize 是否 nullptr）----
struct SymRow {
    std::string   name;
    std::string   file;       // **转义形态**（原样取自产物）
    unsigned long defLine = 0;
    bool          matNull = false;
};

std::vector<SymRow> parseSymbolRows(const std::string& meta) {
    std::vector<SymRow> rows;
    const std::string marker = "kSymbolTable[] = {";
    size_t p = meta.find(marker);
    if (p == std::string::npos) return rows;
    p = meta.find('\n', p);
    while (p != std::string::npos) {
        const size_t e = meta.find('\n', p + 1);
        const std::string line =
            meta.substr(p + 1, (e == std::string::npos ? meta.size() : e) - (p + 1));
        if (line.find("};") != std::string::npos) break;
        const size_t q1 = line.find('"');
        const size_t q2 = (q1 == std::string::npos) ? std::string::npos : line.find('"', q1 + 1);
        const size_t q3 = (q2 == std::string::npos) ? std::string::npos : line.find('"', q2 + 1);
        const size_t q4 = (q3 == std::string::npos) ? std::string::npos : line.find('"', q3 + 1);
        if (q1 != std::string::npos && q4 != std::string::npos) {
            SymRow r;
            r.name    = line.substr(q1 + 1, q2 - q1 - 1);
            r.file    = line.substr(q3 + 1, q4 - q3 - 1);
            // defLine：file 字面量收尾引号之后**先跳过非数字**（`", "`）再取数
            size_t d = q4 + 1;
            while (d < line.size() && !std::isdigit(static_cast<unsigned char>(line[d]))) ++d;
            r.defLine = std::strtoul(line.c_str() + d, nullptr, 10);
            // materialize = 倒数第 2 个逗号与最后一个逗号之间的字段
            const size_t last = line.rfind(',');
            if (last != std::string::npos) {
                const size_t prev = line.rfind(',', last == 0 ? 0 : last - 1);
                if (prev != std::string::npos)
                    r.matNull = (line.substr(prev + 1, last - prev - 1).find("nullptr")
                                 != std::string::npos);
            }
            rows.push_back(r);
        }
        if (e == std::string::npos) break;
        p = e;
    }
    return rows;
}

int indexOfName(const std::vector<SymRow>& rows, const std::string& name) {
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i].name == name) return static_cast<int>(i);
    return -1;
}

// ---- `kModuleBase[] = { ... };` 的数值 ----
std::vector<unsigned long> parseModuleBase(const std::string& meta) {
    std::vector<unsigned long> out;
    const std::string marker = "kModuleBase[]";
    size_t p = meta.find(marker);
    if (p == std::string::npos) return out;
    p = meta.find('{', p);
    if (p == std::string::npos) return out;
    const size_t end = meta.find("};", p);
    if (end == std::string::npos) return out;
    for (size_t i = p + 1; i < end; ++i) {
        if (std::isdigit(static_cast<unsigned char>(meta[i]))) {
            out.push_back(std::strtoul(meta.c_str() + i, nullptr, 10));
            while (i < end && std::isdigit(static_cast<unsigned char>(meta[i]))) ++i;
        }
    }
    return out;
}

// ---- FrameGuard 引用：`_lsg_<defLine>(symbolIndexAt(<m>u, <s>u), <defLine>u);` ----
struct GuardRef {
    unsigned long moduleIdx = 0;
    unsigned long seq       = 0;
    unsigned long defLine   = 0;
};

std::vector<GuardRef> parseGuards(const std::string& text) {
    std::vector<GuardRef> out;
    const std::string key = "symbolIndexAt(";
    size_t p = 0;
    while ((p = text.find(key, p)) != std::string::npos) {
        p += key.size();
        const size_t comma = text.find(',', p);
        if (comma == std::string::npos) break;
        GuardRef g;
        g.moduleIdx = std::strtoul(text.c_str() + p, nullptr, 10);
        size_t s = comma + 1;
        while (s < text.size() && (text[s] == ' ' || text[s] == '\t')) ++s;
        g.seq = std::strtoul(text.c_str() + s, nullptr, 10);
        const size_t lsg = text.rfind("_lsg_", p);
        if (lsg != std::string::npos)
            g.defLine = std::strtoul(text.c_str() + lsg + 5, nullptr, 10);
        out.push_back(g);
        p = comma;
    }
    return out;
}

// ---- `_lsg_<n>` 名集合（提交前不去重 ⇒ 可判重名）----
std::vector<std::string> parseGuardNames(const std::string& text) {
    std::vector<std::string> names;
    const std::string key = "_lsg_";
    size_t p = 0;
    while ((p = text.find(key, p)) != std::string::npos) {
        size_t e = p + key.size();
        while (e < text.size() && std::isdigit(static_cast<unsigned char>(text[e]))) ++e;
        names.push_back(text.substr(p, e - p));
        p = e;
    }
    return names;
}

// ============================================================
// B. 单文件 Inline 路径（可注入 collector / 可传 sourcePath）
//    —— 与 test_codegen_meta.cpp 的 genWithMeta 同形（此处自带一份，避免改那个文件）
// ============================================================
struct InlineOut {
    Aura::CompileUnit unit;
    bool              ok = false;
};

InlineOut genInline(const std::string& src, Aura::DiagnosticEngine& diag,
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
    // ⚠️ SemAnalyzer 必须存活到 generate 返回（AST 的 inferredType 指向其 typeStore_）
    Aura::SemAnalyzer sema(diag);
    (void)sema.analyze(*program);
    if (diag.hasErrors()) return out;
    Aura::CodeGenConfig cfg;                       // 默认 Inline
    Aura::MetaCollector meta(sourcePath, Aura::ModuleManager::sanitizeId(moduleName));
    Aura::CodeGenerator cg(diag);
    out.unit = cg.generate(*program, moduleName, {}, std::string(), cfg, {}, {},
                           sourcePath, Aura::MetadataSink::collect(meta));
    out.ok = true;
    return out;
}

// ============================================================
// C. 多文件 External 路径（≥3 模块 —— T9 / C-新2 需要）
//    复刻 main.cpp compileMultiFile 的真实链路（§4.2-O9 ①–⑤），**本文件自带**；
//    相对既有 helper 的唯一差异：逐模块调用 `setThrowSiteModuleIdx(i)`（复刻 main.cpp:477），
//    —— 缺它则**所有模块的 moduleIdx 都是 0** ⇒ C-新2 的恒等判据无意义。
// ============================================================
struct MultiOut {
    std::map<std::string, Aura::CompileUnit> units;
    std::string                metaHeader;
    std::string                metaImpl;
    std::vector<std::string>   orderedModules;
    std::string                error;
};

MultiOut genMulti(const std::string& entryPath, Aura::DiagnosticEngine& diag) {
    MultiOut out;
    Aura::ModuleManager mgr(diag);
    mgr.loadBuiltinAurai();

    if (!mgr.scanAll(entryPath))    { out.error = "scanAll failed";        return out; }
    if (mgr.checkModuleConflicts()) { out.error = "module conflicts";      return out; }
    if (mgr.hasCycleOn())           { out.error = "dependency cycle";      return out; }
    if (!mgr.loadAllScanned())      { out.error = "loadAllScanned failed"; return out; }
    const std::vector<std::vector<std::string>> layers = mgr.topologicalLayersOn();

    // 确定性模块序：层序 × 同层路径字典序（复刻 main.cpp flattenLayersDeterministic）
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

    // 逐模块 Sema（保持存活）+ 诊断 merge
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

    // 逐模块 CodeGen（External；每模块一个 collector）
    std::map<std::string, std::unique_ptr<Aura::MetaCollector>> collectors;
    for (size_t mi = 0; mi < out.orderedModules.size(); ++mi) {
        const std::string& modPath = out.orderedModules[mi];
        auto* mod = mgr.moduleAt(modPath);
        if (!mod || !mod->ast) continue;

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
        Aura::CodeGenerator::CrossModuleDefaults      crossDefaults;
        Aura::CodeGenerator::CrossModuleParamSemTypes crossParamSemTypes;
        for (const auto& imp : mod->imports) {
            if (imp.isBuiltin) continue;
            auto it = mgr.modules().find(imp.path);
            if (it == mgr.modules().end()) continue;
            const std::string nsKey = imp.alias.empty() ? it->second.moduleName : imp.alias;
            auto& modDefaults = crossDefaults[nsKey];
            auto& modSemTypes = crossParamSemTypes[nsKey];
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

        collectors[modPath] = std::make_unique<Aura::MetaCollector>(
            mod->sourcePath, Aura::ModuleManager::sanitizeId(Aura::stemOf(mod->sourcePath)));

        Aura::CodeGenerator cg(*moduleDiags[mod->sourcePath]);
        // 🔴 复刻 main.cpp:477 —— moduleIdx = orderedModules 下标（缺此则恒 0）
        cg.setThrowSiteModuleIdx(static_cast<uint32_t>(mi));
        Aura::CodeGenConfig cfg;
        cfg.metaMode = Aura::CodeGenConfig::MetaMode::External;
        out.units[modPath] = cg.generate(*mod->ast, mod->moduleName, cgImports, mod->nsName,
                                         cfg, crossDefaults, crossParamSemTypes,
                                         mod->sourcePath, Aura::MetadataSink::of(collectors[modPath].get()));
    }

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

// 临时目录（RAII 收尾；名带 pid）
std::string makeTempDir(const std::string& tag) {
    auto dir = std::filesystem::temp_directory_path() /
               ("f18p4a4_" + tag + "_" + std::to_string(::getpid()));
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
        std::filesystem::remove_all(dir_, ec);
    }
    TempDirGuard(const TempDirGuard&) = delete;
    TempDirGuard& operator=(const TempDirGuard&) = delete;
    const std::string& path() const { return dir_; }
private:
    std::string dir_;
};

// ---- 共用的探针源码 ----
const char* const kSingleSrc =
    "type Player = { hp: int }\n"                                   // 1
    "fun touch(p: Player) -> int { return p.hp }\n"                 // 2
    "fun plain(a: int) -> int { return a + 1 }\n"                   // 3
    "fun (self Player) bump(d: int) -> int { return self.hp + d }\n"// 4
    "fun main(io: Io) { io.println(str(plain(1))) }\n";             // 5

// T11：noMat 形参类型不可物化 ⇒ 降级（thunkName 清空）但**保留记录与编号**
const char* const kPruneSrc =
    "type Probe = { v: int }\n"                                     // 1
    "fun noMat(p: Probe) -> int { return 1 }\n"                     // 2
    "fun okFn(a: int) -> int { return a + 1 }\n"                    // 3
    "fun main(io: Io) { io.println(str(okFn(1))) }\n";              // 4

// T12：main 必须入帧表但无 thunk
const char* const kMainSrc =
    "fun helper(a: int) -> int { return a + 1 }\n"                  // 1
    "fun main(io: Io) { io.println(str(helper(1))) }\n";            // 2

// C3 / C9：字面 record 构造抛出（genThrowStmt 分支 (a)）
const char* const kThrowSrc =
    "fun leaf() throws -> int {\n"                                  // 1
    "    throw { kind = \"boom\", message = \"m\" }\n"               // 2
    "}\n"                                                           // 3
    "fun main(io: Io) throws {\n"                                   // 4
    "    let v = leaf()\n"                                          // 5
    "    io.println(str(v))\n"                                      // 6
    "}\n";                                                          // 7

// C4：throws 调用 vs 非 throws 调用
const char* const kThrowsFilterSrc =
    "fun leaf() throws -> int {\n"                                  // 1
    "    throw { kind = \"boom\", message = \"m\" }\n"               // 2
    "}\n"                                                           // 3
    "fun user() throws -> int {\n"                                  // 4
    "    return leaf()\n"                                           // 5
    "}\n"                                                           // 6
    "fun pure(a: int) -> int { return a + 1 }\n"                    // 7
    "fun usesPure(x: int) -> int { return pure(x) }\n"              // 8
    "fun main(io: Io) throws {\n"                                   // 9
    "    let v = user()\n"                                          // 10
    "    io.println(str(v) + str(usesPure(2)))\n"                    // 11
    "}\n";                                                          // 12

// C5：层 2 可抛点（索引 / unwrap / 函数值调用）
const char* const kLayer2Src =
    "fun main(io: Io) {\n"                                          // 1
    "    let a: [int] = [1, 2, 3]\n"                                // 2
    "    let x = a[0]\n"                                            // 3  索引
    "    let o: Optional<int> = some(3)\n"                          // 4
    "    let v = o.unwrap()\n"                                      // 5  unwrap
    "    let f = fun(y: int) -> int { return y + 1 }\n"             // 6
    "    let z = f(2)\n"                                            // 7  函数值调用
    "    io.println(str(x + v + z))\n"                              // 8
    "}\n";                                                          // 9

// C11：rethrow（err 已带坐标 ⇒ Error.line 必须保留原值）
const char* const kRethrowSrc =
    "fun leaf() throws -> int {\n"                                  // 1
    "    throw { kind = \"boom\", message = \"m\" }\n"               // 2
    "}\n"                                                           // 3
    "fun rethrower() throws -> int {\n"                             // 4
    "    try {\n"                                                   // 5
    "        return leaf()\n"                                       // 6
    "    } catch (e) {\n"                                           // 7
    "        throw e\n"                                             // 8
    "    }\n"                                                       // 9
    "}\n"                                                           // 10
    "fun main(io: Io) throws {\n"                                   // 11
    "    try {\n"                                                   // 12
    "        io.println(str(rethrower()))\n"                        // 13
    "    } catch (e) {\n"                                           // 14
    "        io.println(\"caught\")\n"                              // 15
    "    }\n"                                                       // 16
    "}\n";                                                          // 17

// C10：语句级调用点注入的**覆盖闸门**（恰好 2 条含调用点的值求值语句）
const char* const kCallGateSrc =
    "fun pure(a: int) -> int { return a + 1 }\n"                    // 1
    "fun caller(x: int) -> int { return pure(x) }\n"                // 2  ← 1 条
    "fun main(io: Io) { io.println(str(caller(1))) }\n";            // 3  ← 1 条

// C-新5（收窄批 A2）：同一语句「**不可抛调用在前、可抛调用在后**」⇒ 必须注入**后者**行号。
//   验点 = A2 的行为变化「不可抛调用点 ⇒ 跳过本节点但**继续递归**」（原先命中即终止 ⇒ 会漏掉后面）。
//   多行表达式由行尾 `+` 续行（实测 `aurac` rc=0；同一语句跨 6/7 两行，行号因而可区分）。
const char* const kSkipThenThrowSrc =
    "fun pure(a: int) -> int { return a + 1 }\n"                    // 1
    "fun boom() throws -> int {\n"                                  // 2
    "    throw { kind = \"boom\", message = \"m\" }\n"               // 3
    "}\n"                                                           // 4
    "fun caller() throws -> int {\n"                                // 5
    "    let x = pure(1) +\n"                                       // 6  ← 不可抛调用（前）
    "            boom()\n"                                          // 7  ← 可抛调用（后）
    "    return x\n"                                                // 8
    "}\n";                                                          // 9

// C-新6（收窄批 A1/A3）：**未知 callee** ⇒ 仍注入（验保守原则「查不到 ⇒ 视可抛」）。
//   `g` 是局部闭包值（**非**声明函数名）⇒ 不在 `fnThrows_` ⇒ 未知 ⇒ 注入。
const char* const kUnknownCalleeSrc =
    "fun main(io: Io) {\n"                                          // 1
    "    let g = fun(y: int) -> int { return y + 1 }\n"             // 2
    "    let z = g(2)\n"                                            // 3  ← 未知 callee
    "    io.println(str(z))\n"                                      // 4
    "}\n";                                                          // 5

} // namespace

// ============================================================
// T9 Meta.ModuleBasePrefixSum   （🔴 必须 ≥3 模块；断言「符号**条数**」而非 seq）
// ============================================================
TEST(Meta, ModuleBasePrefixSum) {
    const std::string dir = makeTempDir("t9");
    TempDirGuard guard(dir);
    Aura::writeFile(dir + "/mod_a.aura",
                    "pub fun fa1(x: int) -> int { return x + 1 }\n"
                    "pub fun fa2(x: int) -> int { return x + 2 }\n");
    Aura::writeFile(dir + "/mod_b.aura",
                    "pub fun fb1(x: int) -> int { return x + 3 }\n"
                    "pub fun fb2(x: int) -> int { return x + 4 }\n");
    const std::string entry = dir + "/main.aura";
    Aura::writeFile(entry,
                    "import \"mod_a.aura\" as a\n"
                    "import \"mod_b.aura\" as b\n"
                    "fun main(io: Io) { io.println(str(a.fa1(1) + b.fb1(2))) }\n");

    Aura::DiagnosticEngine diag;
    MultiOut r = genMulti(entry, diag);
    ASSERT_FALSE(diag.hasErrors());
    ASSERT_TRUE(r.error.empty());
    ASSERT_TRUE(r.orderedModules.size() >= 3);            // 🔴 简报 T9：≥3 模块

    const std::vector<SymRow>       rows = parseSymbolRows(r.metaImpl);
    const std::vector<unsigned long> base = parseModuleBase(r.metaImpl);
    const long cnt  = tableValue(r.metaImpl, "kSymbolCount");
    const long mcnt = tableValue(r.metaImpl, "kModuleCount");

    EXPECT_EQ(mcnt, static_cast<long>(r.orderedModules.size()));
    ASSERT_EQ(static_cast<long>(base.size()), mcnt);      // kModuleBase 每模块一项
    ASSERT_EQ(static_cast<long>(rows.size()), cnt);

    // ① 前缀和恒等式：base[m] + count(模块 m) == base[m+1]（**count = 表行按 file 列分组**）
    long acc = 0;
    bool prefixOk = true;
    for (size_t m = 0; m < r.orderedModules.size(); ++m) {
        if (base[m] != static_cast<unsigned long>(acc)) prefixOk = false;
        long n = 0;
        for (const SymRow& s : rows)
            if (canon(s.file) == canon(r.orderedModules[m])) ++n;
        acc += n;
    }
    EXPECT_TRUE(prefixOk);
    EXPECT_EQ(acc, cnt);

    // ② 🔴 简报判据（🟡-3）：kModuleBase[last] + **最后模块的符号条数** == kSymbolCount
    const std::string& lastMod = r.orderedModules.back();
    long lastCount = 0;
    for (const SymRow& s : rows)
        if (canon(s.file) == canon(lastMod)) ++lastCount;
    EXPECT_EQ(static_cast<long>(base.back()) + lastCount, cnt);

    // ③ 编号连续无重号（表序即分配序）
    const std::set<std::string> uniq([&] {
        std::set<std::string> u;
        for (const SymRow& s : rows) u.insert(s.name);
        return u;
    }());
    EXPECT_EQ(static_cast<long>(uniq.size()), cnt);
    EXPECT_EQ(uniq.count("fa1"), 1u);
    EXPECT_EQ(uniq.count("main"), 1u);
}

// ============================================================
// T10 Meta.SingleFileModuleBaseIsZero
// ============================================================
TEST(Meta, SingleFileModuleBaseIsZero) {
    Aura::DiagnosticEngine diag;
    InlineOut r = genInline(kSingleSrc, diag, "D:/you/Aura/probe/t10/x.aura");
    ASSERT_TRUE(r.ok);
    const std::vector<unsigned long> base = parseModuleBase(r.unit.metaImpl);
    ASSERT_EQ(static_cast<long>(base.size()), 1l);        // 单文件 = 1 个模块
    EXPECT_EQ(base[0], 0ul);
    EXPECT_EQ(tableValue(r.unit.metaImpl, "kModuleCount"), 1l);
}

// ============================================================
// T11 Meta.PruneKeepsFrameNumbering   （⚠️ 按 §3.1.5 新语义：prune 降级后**无「剪掉」可测**）
//   ① 降级：记录**仍入表**，但 materialize == nullptr（thunkName 清空 ⇒ 不渲染 `&`、不发 thunk）
//   ② 编号无洞：FrameGuard 的 seqInModule 连续 0..N-1（无「剪掉一条 ⇒ 后续错位」）
//   ③ 该符号**仍有帧表项**（name 可渲染）
// ============================================================
TEST(Meta, PruneKeepsFrameNumbering) {
    Aura::DiagnosticEngine diag;
    InlineOut r = genInline(kPruneSrc, diag, "D:/you/Aura/probe/t11/p.aura");
    ASSERT_TRUE(r.ok);

    const std::vector<SymRow> rows = parseSymbolRows(r.unit.metaImpl);
    const int iNoMat = indexOfName(rows, "noMat");
    ASSERT_TRUE(iNoMat >= 0);                              // ① 记录仍在表里（降级不删）

    // ① 物化列降级为 nullptr；且**没有**为它发射 thunk（seq 0）
    EXPECT_TRUE(rows[iNoMat].matNull);
    EXPECT_NOT_CONTAINS(r.unit.impl, "_aura_mat_main_0(const aura_rt::CallArg* recv)");
    // 对照：可物化的 okFn（seq 1）照常发 thunk
    EXPECT_CONTAINS(r.unit.impl, "_aura_mat_main_1(const aura_rt::CallArg* recv)");

    // ② 编号无洞：FrameGuard 的 seq 连续 0..N-1，且无重号
    const std::vector<GuardRef> guards = parseGuards(r.unit.impl);
    ASSERT_EQ(static_cast<long>(guards.size()), 3l);        // noMat / okFn / main
    std::set<unsigned long> seqs;
    for (const GuardRef& g : guards) seqs.insert(g.seq);
    EXPECT_EQ(static_cast<long>(seqs.size()), static_cast<long>(guards.size()));
    EXPECT_EQ(*seqs.begin(), 0ul);
    EXPECT_EQ(*seqs.rbegin() - *seqs.begin() + 1,
              static_cast<unsigned long>(seqs.size()));     // 连续（区间长度 == 元素数）

    // ③ 仍有帧表项：帧表里 "noMat" 可渲染（name 列非空）
    EXPECT_CONTAINS(sectionFrom(r.unit.metaImpl, "kFrameTable[]  = {"), "\"noMat\"");
    EXPECT_CONTAINS(sectionFrom(r.unit.metaImpl, "kSymbolTable[] = {"), "\"noMat\"");
}

// ============================================================
// T12 Meta.MainIsCollectedAsFrame
//   main 有帧表项、名 "main"、**无 thunk**（materialize == nullptr）、**编号恒等**
// ============================================================
TEST(Meta, MainIsCollectedAsFrame) {
    Aura::DiagnosticEngine diag;
    InlineOut r = genInline(kMainSrc, diag, "D:/you/Aura/probe/t12/m.aura");
    ASSERT_TRUE(r.ok);

    const std::vector<SymRow> rows = parseSymbolRows(r.unit.metaImpl);
    const int iMain = indexOfName(rows, "main");
    ASSERT_TRUE(iMain >= 0);                               // main 入符号表
    EXPECT_TRUE(rows[iMain].matNull);                      // 无 thunk（A2/D2：机制是隐式的）
    EXPECT_CONTAINS(sectionFrom(r.unit.metaImpl, "kFrameTable[]  = {"), "\"main\"");

    // 编号恒等：FrameGuard 的 (moduleIdx, seq) 经 kModuleBase 换算 == 表内下标
    const std::vector<unsigned long> base = parseModuleBase(r.unit.metaImpl);
    ASSERT_TRUE(!base.empty());
    const std::vector<GuardRef> guards = parseGuards(r.unit.impl);
    ASSERT_EQ(static_cast<long>(guards.size()), 2l);        // helper / main
    const GuardRef* mainGuard = nullptr;
    for (const GuardRef& g : guards)
        if (g.defLine == rows[iMain].defLine) mainGuard = &g;
    ASSERT_TRUE(mainGuard != nullptr);
    EXPECT_EQ(base[mainGuard->moduleIdx] + mainGuard->seq, static_cast<unsigned long>(iMain));
    // 对照：helper 的换算也落在自己的表行
    const int iHelper = indexOfName(rows, "helper");
    ASSERT_TRUE(iHelper >= 0);
    for (const GuardRef& g : guards) {
        if (g.defLine == rows[iHelper].defLine)
            EXPECT_EQ(base[g.moduleIdx] + g.seq, static_cast<unsigned long>(iHelper));
    }
}

// ============================================================
// C1 CodeGenFrame.FrameGuardInjectedAtFunctionEntry
//   ⚠️ D2 的精确读法：FrameGuard **不是**字面第一行 —— 形参的 GcRootHandle 在它之前
// ============================================================
TEST(CodeGenFrame, FrameGuardInjectedAtFunctionEntry) {
    Aura::DiagnosticEngine diag;
    InlineOut r = genInline(kSingleSrc, diag, "D:/you/Aura/probe/c1/f.aura");
    ASSERT_TRUE(r.ok);

    // (a) GC 指针形参：形参 GcRootHandle 声明**在 FrameGuard 之前**，且二者都在首语句之前
    const std::string touch = bodyOf(r.unit.impl, "int32_t touch(Player* p_raw) {");
    ASSERT_TRUE(!touch.empty());
    const size_t rootT  = touch.find("GcRootHandle<decltype(p_raw)>");
    const size_t guardT = touch.find("FrameGuard");
    const size_t retT   = touch.find("return");
    EXPECT_TRUE(rootT != std::string::npos);
    EXPECT_TRUE(guardT != std::string::npos);
    EXPECT_TRUE(rootT < guardT);                          // D2：句柄先于帧
    EXPECT_TRUE(guardT < retT);                           // 帧在 genBlock 首语句之前
    EXPECT_CONTAINS(touch, "aura_rt::FrameGuard _lsg_");
    EXPECT_CONTAINS(touch, "aura_rt::meta::symbolIndexAt(0u, ");   // 单文件 moduleIdx == 0

    // (b) 无 GC 形参的纯函数：FrameGuard 是函数体**第一条**（除缩进空白外无内容）
    const std::string plain = bodyOf(r.unit.impl, "int32_t plain(int32_t a) {");
    ASSERT_TRUE(!plain.empty());
    const size_t guardP = plain.find("aura_rt::FrameGuard _lsg_");
    ASSERT_TRUE(guardP != std::string::npos);
    bool onlyWs = true;
    for (size_t i = 0; i < guardP; ++i)
        if (!std::isspace(static_cast<unsigned char>(plain[i]))) onlyWs = false;
    EXPECT_TRUE(onlyWs);
}

// ============================================================
// C2 CodeGenFrame.FrameGuardInjectedInMethod
//   同上（方法）：`_this` 的 GcRootHandle 在 FrameGuard 之前
// ============================================================
TEST(CodeGenFrame, FrameGuardInjectedInMethod) {
    Aura::DiagnosticEngine diag;
    InlineOut r = genInline(kSingleSrc, diag, "D:/you/Aura/probe/c2/f.aura");
    ASSERT_TRUE(r.ok);

    const std::string bump = bodyOf(r.unit.impl, "int32_t Player::bump(int32_t d) {");
    ASSERT_TRUE(!bump.empty());
    const size_t rootM  = bump.find("GcRootHandle<Player*> _this");
    const size_t guardM = bump.find("FrameGuard");
    EXPECT_TRUE(rootM != std::string::npos);
    EXPECT_TRUE(guardM != std::string::npos);
    EXPECT_TRUE(rootM < guardM);
    EXPECT_CONTAINS(bump, "aura_rt::meta::symbolIndexAt(0u, ");

    // 方法的符号名是限定名（ReceiverType.method）且**入帧表**
    EXPECT_CONTAINS(sectionFrom(r.unit.metaImpl, "kFrameTable[]  = {"), "\"Player.bump\"");
}

// ============================================================
// C3 CodeGenFrame.SetFrameLineAtThrowSite
//   genThrowStmt 产物含 setFrameLine(<stmt.line>)，且**同一行号**是 Error(...) 的第 5 参
// ============================================================
TEST(CodeGenFrame, SetFrameLineAtThrowSite) {
    Aura::DiagnosticEngine diag;
    InlineOut r = genInline(kThrowSrc, diag, "D:/you/Aura/probe/c3/t.aura");
    ASSERT_TRUE(r.ok);

    const int L = lineOfOccurrence(kThrowSrc, "throw {");
    ASSERT_EQ(L, 2);
    const std::string ls = std::to_string(L);

    EXPECT_CONTAINS(r.unit.impl, "aura_rt::setFrameLine(" + ls + ");");
    // Error(kind, message, extra, file, line, site)：第 5 参 = 同一行号
    EXPECT_CONTAINS(r.unit.impl,
                    "throw aura_rt::Error(_hk.get(), _hm.get(), nullptr, _f, " + ls + ", ");
}

// ============================================================
// C4 CodeGenFrame.SetFrameLineOnlyAtThrowableCalls
//   **收窄批（2026-10-02）已转正式断言**：① `throws` 函数调用 ⇒ 注入；
//   ② **无 throws 的纯函数调用 ⇒ 不注入**（层 1 已打通 —— A1 的 `fnThrows_` 索引）。
//   （批 4 时 ② 因「全注入」被标注为待恢复，见 change.md §11.9.2 / §11.13.3。）
// ============================================================
TEST(CodeGenFrame, SetFrameLineOnlyAtThrowableCalls) {
    Aura::DiagnosticEngine diag;
    InlineOut r = genInline(kThrowsFilterSrc, diag, "D:/you/Aura/probe/c4/t.aura");
    ASSERT_TRUE(r.ok);

    const int LUser = lineOfOccurrence(kThrowsFilterSrc, "return leaf()");
    ASSERT_EQ(LUser, 5);

    // ① 调用**可抛函数**（leaf 带 throws）⇒ 注入
    const std::string user = bodyOf(r.unit.impl, "int32_t user() {");
    ASSERT_TRUE(!user.empty());
    EXPECT_CONTAINS(user, "aura_rt::setFrameLine(" + std::to_string(LUser) + ");");

    // ② ✅ **收窄批转正式断言**：`pure` 声明**无** `throws` ⇒ 层 1 判定「不可抛」⇒ **不注入**。
    //    ⚠️ 这是 A2「跳过 but 继续递归」的直接判据：`usesPure` 体内 `return pure(x)` 唯一
    //    调用点即不可抛调用 ⇒ 扫描器跳过它、继续递归、再无命中 ⇒ 本条语句零注入。
    const std::string usesPure = bodyOf(r.unit.impl, "int32_t usesPure(int32_t x) {");
    ASSERT_TRUE(!usesPure.empty());
    EXPECT_NOT_CONTAINS(usesPure, "aura_rt::setFrameLine(");
}

// ============================================================
// C5 CodeGenFrame.SetFrameLineAtIndexOps
//   层 2（索引 / union / unwrap / 函数值调用）⇒ 注入（护栏：防层 1 漏注）
//   ✅ **收窄批（2026-10-02）：索引项已恢复**——`FirstCallLineScanner` 新增
//      `visit(const IndexExpr&)`（层 2 兜底，独立于 callee 的 throws）⇒ §9-V19 缺口关闭。
//      ⚠️ 关键：收窄后「层 2 形态」全靠「**未知 ⇒ 可抛**」兜住（`io.*`/`unwrap`/函数值
//      调用都不命中 `fnThrows_`）⇒ 本用例正是该兜底的回归网。
//   ⚠️ 「union」一项无法用 Aura 源码单独表达（`int | None` 不支持直接 `.unwrap()`，
//      实测 no variant supports method 'unwrap'）⇒ 本用例覆盖其余三项，union 记入遗留。
// ============================================================
TEST(CodeGenFrame, SetFrameLineAtIndexOps) {
    Aura::DiagnosticEngine diag;
    InlineOut r = genInline(kLayer2Src, diag, "D:/you/Aura/probe/c5/l.aura");
    ASSERT_TRUE(r.ok);

    const int LIndex  = lineOfOccurrence(kLayer2Src, "let x = a[0]");
    const int LUnwrap = lineOfOccurrence(kLayer2Src, "let v = o.unwrap()");
    const int LFnVal  = lineOfOccurrence(kLayer2Src, "let z = f(2)");
    ASSERT_EQ(LIndex, 3);
    ASSERT_EQ(LUnwrap, 5);
    ASSERT_EQ(LFnVal, 7);

    // ① 索引（Array 越界 ⇒ IndexError）⇒ 注入  ✅ **收窄批恢复**（§9-V19 已关闭）
    EXPECT_CONTAINS(r.unit.impl, "aura_rt::setFrameLine(" + std::to_string(LIndex) + ");");
    // ② unwrap（None 解包 ⇒ RuntimeError）⇒ 注入（层 2 兜底：接收者 Optional 不命中索引）
    EXPECT_CONTAINS(r.unit.impl, "aura_rt::setFrameLine(" + std::to_string(LUnwrap) + ");");
    // ③ 函数值调用（CallableObj::invoke）⇒ 注入（层 2 兜底：callee 非声明函数名）
    EXPECT_CONTAINS(r.unit.impl, "aura_rt::setFrameLine(" + std::to_string(LFnVal) + ");");
}

// ============================================================
// C-新5（收窄批 A2）CodeGenFrame.SetFrameLineSkipsNonThrowableAndContinues
//   同一语句「不可抛调用在前、可抛调用在后」⇒ 注入**后者**行号；
//   **不得**注入前者行号（验「跳过 but 继续递归」这一行为变化）。
// ============================================================
TEST(CodeGenFrame, SetFrameLineSkipsNonThrowableAndContinues) {
    Aura::DiagnosticEngine diag;
    InlineOut r = genInline(kSkipThenThrowSrc, diag, "D:/you/Aura/probe/cn5/s.aura");
    ASSERT_TRUE(r.ok);

    const int LPure = lineOfOccurrence(kSkipThenThrowSrc, "let x = pure(1) +");
    const int LThrow = lineOfOccurrence(kSkipThenThrowSrc, "boom()\n    return");
    const int LBoomDot = lineOfOccurrence(kSkipThenThrowSrc, "            boom()");
    ASSERT_EQ(LPure, 6);        // 不可抛调用在前
    ASSERT_EQ(LBoomDot, 7);     // 可抛调用在后
    ASSERT_TRUE(LThrow > 0);

    const std::string caller = bodyOf(r.unit.impl, "int32_t caller() {");
    ASSERT_TRUE(!caller.empty());

    // ① 注入**后者**（可抛调用 `boom()` 所在的第 7 行）
    EXPECT_CONTAINS(caller, "aura_rt::setFrameLine(7);");
    // ② **不得**注入前者（不可抛调用 `pure(1)` 所在的第 6 行）
    EXPECT_NOT_CONTAINS(caller, "aura_rt::setFrameLine(6);");
}

// ============================================================
// C-新6（收窄批 A1/A3）CodeGenFrame.SetFrameLineAtUnknownCallee
//   **未知 callee**（局部闭包值 `g`，非声明函数名 ⇒ 不在 `fnThrows_`）⇒ **仍注入**
//   —— 验保守原则「查不到 / 无法判定 ⇒ 视为可抛」（红线 R3）。
// ============================================================
TEST(CodeGenFrame, SetFrameLineAtUnknownCallee) {
    Aura::DiagnosticEngine diag;
    InlineOut r = genInline(kUnknownCalleeSrc, diag, "D:/you/Aura/probe/cn6/u.aura");
    ASSERT_TRUE(r.ok);

    const int LCall = lineOfOccurrence(kUnknownCalleeSrc, "let z = g(2)");
    ASSERT_EQ(LCall, 3);

    // 未知 callee ⇒ 注入（保守）
    EXPECT_CONTAINS(r.unit.impl, "aura_rt::setFrameLine(3);");
    // 且产物确实生成了（排除「空产物恰好通过」）
    EXPECT_CONTAINS(r.unit.impl, "int32_t z");
}

// ============================================================
// C9 CodeGenFrame.SetFrameLineIsIdentityToErrorLine
//   ⚠️ **只在「字面 record 构造」形态**（genThrowStmt 分支 (a)）：setFrameLine(L) 与 Error.line 同值
//   （必须排除 `throw e;` 重抛 —— 那是 C11）
// ============================================================
TEST(CodeGenFrame, SetFrameLineIsIdentityToErrorLine) {
    Aura::DiagnosticEngine diag;
    InlineOut r = genInline(kThrowSrc, diag, "D:/you/Aura/probe/c9/t.aura");
    ASSERT_TRUE(r.ok);

    const int L = lineOfOccurrence(kThrowSrc, "throw {");
    ASSERT_EQ(L, 2);
    const std::string ls = std::to_string(L);

    // 分支 (a) 无条件填 stmt.line ⇒ 两值同源同值
    EXPECT_CONTAINS(r.unit.impl, "aura_rt::setFrameLine(" + ls + ");");
    EXPECT_CONTAINS(r.unit.impl,
                    "throw aura_rt::Error(_hk.get(), _hm.get(), nullptr, _f, " + ls + ", ");
    // 本形态**不**走 `throw _e;` 的守卫填装路径（守卫是重抛分支 (b) 的形态）
    EXPECT_NOT_CONTAINS(r.unit.impl, "if (_e.file == nullptr) { _e.file = _f; _e.line = " + ls + "; }");
}

// ============================================================
// C11 CodeGenFrame.RethrowKeepsOriginalErrorLine   （§11.6-🟡-5 新增）
//   `throw err;`（err 带坐标）⇒ ① setFrameLine 写**本行** stmt.line；
//                            ② Error.line 保留原值（守卫 `if (_e.file == nullptr)`）；
//                            ③ 二者不同且各自正确
// ============================================================
TEST(CodeGenFrame, RethrowKeepsOriginalErrorLine) {
    Aura::DiagnosticEngine diag;
    InlineOut r = genInline(kRethrowSrc, diag, "D:/you/Aura/probe/c11/r.aura");
    ASSERT_TRUE(r.ok);

    const int LRethrow  = lineOfOccurrence(kRethrowSrc, "throw e");
    const int LOriginal = lineOfOccurrence(kRethrowSrc, "throw {");
    ASSERT_EQ(LRethrow, 8);
    ASSERT_EQ(LOriginal, 2);
    ASSERT_TRUE(LRethrow != LOriginal);

    // ① setFrameLine 写本行
    EXPECT_CONTAINS(r.unit.impl, "aura_rt::setFrameLine(" + std::to_string(LRethrow) + ");");

    // ② Error.line 的填装**带守卫** ⇒ err 已带坐标时保留原值
    EXPECT_CONTAINS(r.unit.impl, "if (_e.file == nullptr) { _e.file = _f; _e.line = "
                                     + std::to_string(LRethrow) + "; }");
    EXPECT_EQ(countOccurrences(r.unit.impl, "_e.line = " + std::to_string(LRethrow)), 1);
    // 该赋值只出现在守卫行内（行内即有 `if (_e.file == nullptr)`）
    EXPECT_CONTAINS(rowContaining(r.unit.impl, "_e.line = " + std::to_string(LRethrow)),
                    "if (_e.file == nullptr)");

    // ③ 原抛出点（leaf 的字面构造）行号仍在（两者不同、各自正确）
    EXPECT_CONTAINS(r.unit.impl, "aura_rt::setFrameLine(" + std::to_string(LOriginal) + ");");
}

// ============================================================
// C10 CodeGenFrame.AllCallExitsCovered
//   §9-V5：确认 `prefix` 在该文件内**只有一处定义**（"单点改造即全覆盖"的论据）
//   ⚠️ §9-V5 的字面口径（grep `std::string prefix` 计数 == 1）**必红**：
//      实测 ExprCall.cpp 有 3 处（:16 `static const std::string prefix = "aura_rt::Optional<"`、
//      :145 `const std::string prefix = "aura_rt::Variant<"`、:724 调用点前缀）
//      ⇒ 前两处与本批无关的同名局部变量 ⇒ 保留原判据（R3：不放宽），并附修正口径作为补充。
//   另：生成码侧覆盖闸门（2 条含调用点的值求值语句 ⇒ 2 条 setFrameLine）
// ============================================================
TEST(CodeGenFrame, AllCallExitsCovered) {
    // (i) ⚠️ **主 Agent 裁定（2026-10-02）：§9-V5 的字面口径作废** ——
    //    实测 `std::string prefix` 在 `ExprCall.cpp` 有 **3** 处（`:16`/`:145` 是与本批无关的
    //    同名局部变量，`:724` 才是调用点前缀）⇒ 字面计数**不是有效判据**（同名污染）。
    //    ⇒ **以 (i-b) 的修正口径为唯一判据**（下方）。旧字面断言已移除（**非放宽**：原判据本身无效）。
    const std::string ec = Aura::readFile(kRepoRoot + "/src/CodeGen/ExprCall.cpp");
    const std::string em = Aura::readFile(kRepoRoot + "/src/CodeGen/ExprMethodCall.cpp");
    ASSERT_TRUE(!ec.empty());
    ASSERT_TRUE(!em.empty());
    // EXPECT_EQ(countOccurrences(ec, "std::string prefix"), 1);   // ← 无效判据，已作废
    // EXPECT_EQ(countOccurrences(em, "std::string prefix"), 1);

    // (i-b) ✅ **修正口径（唯一有效判据）**：**调用点**前缀（needAwait 那个）各只有一处定义
    EXPECT_EQ(countOccurrences(ec, "prefix = needAwait"), 1);
    EXPECT_EQ(countOccurrences(em, "prefix = (needAwait"), 1);

    // (ii) 生成码侧闸门（**收窄批更新** —— 原为「全注入 ⇒ 2 条」）：
    Aura::DiagnosticEngine diag;
    InlineOut r = genInline(kCallGateSrc, diag, "D:/you/Aura/probe/c10/g.aura");
    ASSERT_TRUE(r.ok);
    //      `kCallGateSrc` 有 **2 条含调用点的值求值语句**（源码事实，值不变）：
    //        · `return pure(x)`        —— `pure` 无 `throws` ⇒ **不可抛** ⇒ 收窄后**不注入**；
    //        · `io.println(str(caller(1)))` —— `println`（I/O 族，层 2）与内建 `str` 均无法
    //          静态判定为不可抛 ⇒ **注入**（保守原则）。
    //      ⇒ 收窄后 `setFrameLine` 恰 **1** 条（批 3 全注入时为 2）。
    //      ⚠️ 这不是「为绿放宽」：判据随**语义变化**更新（§6.2 C4 的收窄批转正即此变化的规范面）。
    EXPECT_EQ(countOccurrences(kCallGateSrc, "return pure(x)"), 1);
    EXPECT_EQ(countOccurrences(kCallGateSrc, "str(caller(1))"), 1);
    EXPECT_EQ(countOccurrences(r.unit.impl, "aura_rt::setFrameLine("), 1);
}

// ============================================================
// C-新 CodeGenFrame.FrameGuardCountMatchesFrameCount   （§11.8.3 遗留 2）
//   较大的 .aura ⇒ `FrameGuard _lsg_` 数 **== kFrameCount**；且 `_lsg_n` **无重名**
// ============================================================
TEST(CodeGenFrame, FrameGuardCountMatchesFrameCount) {
    std::string src = "type Plane = { hp: int }\n";
    for (int i = 1; i <= 12; ++i)
        src += "fun f" + std::to_string(i) + "(a: int) -> int { return a + " + std::to_string(i) + " }\n";
    src += "fun (self Plane) bump(d: int) -> int { return self.hp + d }\n";
    src += "fun (self Plane) drop(d: int) -> int { return self.hp - d }\n";
    src += "fun main(io: Io) { let p = Plane { hp = 1 } io.println(str(f1(1) + p.bump(2) + p.drop(3))) }\n";

    Aura::DiagnosticEngine diag;
    InlineOut r = genInline(src, diag, "D:/you/Aura/probe/cnew/big.aura");
    ASSERT_TRUE(r.ok);

    const long fc = tableValue(r.unit.metaImpl, "kFrameCount");
    const long sc = tableValue(r.unit.metaImpl, "kSymbolCount");
    ASSERT_TRUE(fc >= 10);                                 // 「较大」生成码（遗留 2 的覆盖强度）
    EXPECT_EQ(fc, sc);                                     // O12：帧表与符号表严格平行

    const int nGuard = countOccurrences(r.unit.impl, "aura_rt::FrameGuard _lsg_");
    EXPECT_EQ(nGuard, static_cast<int>(fc));

    const std::vector<std::string> names = parseGuardNames(r.unit.impl);
    EXPECT_EQ(static_cast<long>(names.size()), fc);
    const std::set<std::string> uniq(names.begin(), names.end());
    EXPECT_EQ(uniq.size(), names.size());                  // 🔴 无重名
}

// ============================================================
// C-新2 CodeGenFrame.ModuleBaseInExternalMode   （§11.8.3 遗留 1：批 2 未实测）
//   **多文件 External 模式**下 kModuleBase / kModuleCount 正确生成，且 symbolIndexAt 恒等
// ============================================================
TEST(CodeGenFrame, ModuleBaseInExternalMode) {
    const std::string dir = makeTempDir("cnew2");
    TempDirGuard guard(dir);
    Aura::writeFile(dir + "/mod_a.aura",
                    "pub fun fa1(x: int) -> int { return x + 1 }\n"
                    "pub fun fa2(x: int) -> int { return x + 2 }\n");
    Aura::writeFile(dir + "/mod_b.aura",
                    "pub fun fb1(x: int) -> int { return x + 3 }\n"
                    "pub fun fb2(x: int) -> int { return x + 4 }\n");
    const std::string entry = dir + "/main.aura";
    Aura::writeFile(entry,
                    "import \"mod_a.aura\" as a\n"
                    "import \"mod_b.aura\" as b\n"
                    "fun main(io: Io) { io.println(str(a.fa1(1) + b.fb1(2))) }\n");

    Aura::DiagnosticEngine diag;
    MultiOut r = genMulti(entry, diag);
    ASSERT_FALSE(diag.hasErrors());
    ASSERT_TRUE(r.error.empty());
    ASSERT_TRUE(r.orderedModules.size() >= 3);

    const std::vector<SymRow>        rows = parseSymbolRows(r.metaImpl);
    const std::vector<unsigned long> base = parseModuleBase(r.metaImpl);
    const long cnt  = tableValue(r.metaImpl, "kSymbolCount");
    const long mcnt = tableValue(r.metaImpl, "kModuleCount");

    // ① External 模式确实渲染了 kModuleBase / kModuleCount，且条数与模块数一致
    EXPECT_FALSE(r.metaImpl.find("kModuleBase[]") == std::string::npos);
    ASSERT_EQ(static_cast<long>(base.size()), mcnt);
    EXPECT_EQ(mcnt, static_cast<long>(r.orderedModules.size()));
    ASSERT_EQ(static_cast<long>(rows.size()), cnt);

    // ② 每模块的符号行在表内**连续**且按 orderedModules 序（前缀和的前提）
    size_t cursor = 0;
    for (size_t m = 0; m < r.orderedModules.size(); ++m) {
        const unsigned long b = base[m];
        EXPECT_EQ(b, static_cast<unsigned long>(cursor));
        while (cursor < rows.size() &&
               canon(rows[cursor].file) == canon(r.orderedModules[m]))
            ++cursor;
    }
    EXPECT_EQ(static_cast<long>(cursor), cnt);

    // ③ 🔴 symbolIndexAt 恒等：每模块 impl 的 FrameGuard (m, s) 换算出的全局下标，
    //    其表行的 defLine 必须与 FrameGuard 的 defLine 一致，且该行属**本模块**
    int checked = 0;
    for (size_t mi = 0; mi < r.orderedModules.size(); ++mi) {
        auto it = r.units.find(r.orderedModules[mi]);
        ASSERT_TRUE(it != r.units.end());
        for (const GuardRef& g : parseGuards(it->second.impl)) {
            EXPECT_EQ(g.moduleIdx, static_cast<unsigned long>(mi));   // moduleIdx == 模块下标
            const unsigned long idx = base[g.moduleIdx] + g.seq;
            ASSERT_TRUE(idx < rows.size());
            EXPECT_EQ(rows[idx].defLine, g.defLine);                  // 恒等：指向同一符号
            EXPECT_TRUE(canon(rows[idx].file) == canon(r.orderedModules[mi]));
            ++checked;
        }
    }
    EXPECT_GE(checked, 3);                                            // 三模块各至少 1 帧
}

// ============================================================
// C-新3 CodeGenFrame.NoCollectorProducesNoInjection
//   **显式契约用例**（change.md §11.11 的兜底）：
//   `metaCollector == nullptr` 是**合法模式** —— 不发射元数据表 ⇒ **零注入**，且**不报错**。
//
//   ⚠️ 本用例存在的理由：feature-18 P4a 批 2/3 的端到端验证**全走 `aurac` CLI**
//      （恒注入 collector），而这条「无 collector」路径**当时无人显式断言** ⇒
//      让一个阻塞级缺陷（416 个单测红）藏了一整批。见 change.md §11.11 / §0.2 判据②。
//   ⚠️ 刻意走框架的 `compileSource`（见 test_helpers.h：它**显式传 `nullptr`**）——
//      因为 411/416 个红就是从这个入口进来的。
// ============================================================
TEST(CodeGenFrame, NoCollectorProducesNoInjection) {
    Aura::DiagnosticEngine diag;
    Aura::CompileUnit unit = compileSource(kSingleSrc, diag, "main");

    // (a) 无 collector **不是错误**（这是 §11.11 修复前唯一失败的断言）
    EXPECT_FALSE(diag.hasErrors());

    // (b) 产物**零注入**：帧守卫 / 行号 / 帧表 三样都不该出现
    ASSERT_TRUE(!unit.impl.empty());
    EXPECT_NOT_CONTAINS(unit.impl, "aura_rt::FrameGuard");
    EXPECT_NOT_CONTAINS(unit.impl, "aura_rt::setFrameLine");
    EXPECT_NOT_CONTAINS(unit.impl,   "kFrameTable");
    EXPECT_NOT_CONTAINS(unit.header, "kFrameTable");
    EXPECT_NOT_CONTAINS(unit.impl,   "aura_rt::meta::symbolIndexAt");

    // (c) 正对照：产物确实生成了（不是"空产物恰好通过 (b)"）
    EXPECT_CONTAINS(unit.impl, "int32_t plain(");
    EXPECT_CONTAINS(unit.impl, "int32_t touch(");
}

// ============================================================
// C-新4 CodeGenFrame.NoCollectorIsIdentityToFeature17
//   门控的**逐字**保证：同一份源码，`nullptr` 路径的产物
//   **与「把注入相关 include/表全关掉」的形态一致** —— 用最直接的方式落定：
//   产物里不含任何 feature-18 元数据/逻辑栈符号。
// ============================================================
TEST(CodeGenFrame, NoCollectorIsIdentityToFeature17) {
    Aura::DiagnosticEngine diag;
    Aura::CompileUnit unit = compileSource(kMainSrc, diag, "main");
    EXPECT_FALSE(diag.hasErrors());
    ASSERT_TRUE(!unit.impl.empty());

    for (const char* sym : {"FrameGuard", "setFrameLine", "kFrameTable", "kSymbolTable",
                            "kModuleBase", "kModuleCount", "kThrowSite",
                            "meta::symbolIndexAt", "captureLogicalStack"}) {
        // ⚠️ 本框架的 EXPECT_* 不支持 gtest 的 `<<` 消息流 ⇒ 只留断言，
        //    符号名见本用例注释与 change.md §11.11。
        EXPECT_TRUE(unit.impl.find(sym) == std::string::npos);
        EXPECT_TRUE(unit.header.find(sym) == std::string::npos);
    }
}
