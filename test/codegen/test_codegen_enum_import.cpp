// ============================================================
// test_codegen_enum_import.cpp — 批 6（B2）：跨模块（导入）enum 的 CodeGen 装填
//   （change.md §4.9.4-L18）
//
// 为什么需要本文件（**单测机制侦查结论**）：
//   · `test/framework/test_helpers.h` 的 `analyzeFile` / `analyzeModuleGraph`
//     （:156-198）只做**多文件 Sema**，**不跑 CodeGen**；
//   · 同文件的 `compileSource`（:117-147）只做**单文件** CodeGen（`imports = {}`）。
//   ⇒ 「多文件 + CodeGen」在既有框架里**没有**现成机制 ⇒ 本文件自带最小多文件
//     CodeGen 驱动（口径逐条对齐 `src/main.cpp` 的 `runCgModule`：每模块独立
//     CodeGenerator + 依赖模块的 `CodeGenImport` 表 + 独立 diag）。
//   配套的手工两步铁律（`aurac` + `g++ -fsyntax-only`）在 `scripts/b6_cases/two_step.sh`
//   —— 文本断言锁**形态**，g++ 锁**可编译性**，两者互补（缺 g++ 步 = 把静默坏 C++ 当通过）。
// ============================================================
#include "framework/test_framework.h"
#include "framework/test_helpers.h"
#include "Module/ModuleManager.h"
#include "Sema/SemAnalyzer.h"
#include "CodeGen/CodeGen.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace aura_test;

namespace {

std::string enumImportTempDir() {
    auto base = std::filesystem::temp_directory_path();
    auto dir = base / ("aura_cg_enum_import_" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir.string();
}

std::string writeEnumImportAura(const std::string& dir, const std::string& name,
                                const std::string& content) {
    std::string path = (std::filesystem::path(dir) / name).string();
    std::ofstream f(path);
    f << content;
    return path;
}

// 多文件 Sema（按拓扑层注入依赖 exports，与 analyzeModuleGraph 同口径）
//   + 逐模块 CodeGen（与 main.cpp runCgModule 同口径）→ sourcePath → CompileUnit
std::map<std::string, Aura::CompileUnit>
compileModuleGraph(const std::string& entry, Aura::DiagnosticEngine& diag) {
    std::map<std::string, Aura::CompileUnit> units;
    Aura::ModuleManager mgr(diag);
    mgr.loadBuiltinAurai();
    if (!mgr.loadAll(entry)) return units;
    if (mgr.hasCycle()) return units;
    auto layers = mgr.topologicalLayers();

    std::map<std::string, std::unique_ptr<Aura::DiagnosticEngine>> diags;
    std::map<std::string, std::unique_ptr<Aura::SemAnalyzer>> semas;
    for (auto& layer : layers) {
        std::vector<Aura::ModuleInfo*> tasks;
        for (auto* mod : layer) {
            if (mod->isBuiltin || !mod->ast) continue;
            tasks.push_back(mod);
            auto d = std::make_unique<Aura::DiagnosticEngine>();
            d->setSourceView(Aura::readFile(mod->sourcePath));
            d->setFileName(mod->sourcePath);
            diags[mod->sourcePath] = std::move(d);
        }
        for (auto* mod : tasks) {
            auto sema = std::make_unique<Aura::SemAnalyzer>(*diags[mod->sourcePath]);
            for (auto& depPath : mod->deps) {
                auto depIt = mgr.modules().find(depPath);
                if (depIt == mgr.modules().end()) continue;
                std::string alias;
                for (auto& imp : mod->imports)
                    if (imp.path == depPath) { alias = imp.alias; break; }
                sema->importExports(alias, depIt->second.exports);
            }
            (void)sema->analyze(*mod->ast);
            mod->exports = sema->extractExports();
            semas[mod->sourcePath] = std::move(sema);
        }
        for (auto* mod : tasks) diag.mergeFrom(*diags[mod->sourcePath]);
    }

    for (auto& layer : layers) {
        for (auto* mod : layer) {
            if (mod->isBuiltin || !mod->ast) continue;
            std::vector<Aura::CodeGenImport> cgImports;
            for (auto& imp : mod->imports) {
                Aura::CodeGenImport ci;
                ci.path = imp.path;
                ci.alias = imp.alias;
                ci.isBuiltin = imp.isBuiltin;
                if (!imp.isBuiltin) {
                    auto it = mgr.modules().find(imp.path);
                    if (it != mgr.modules().end()) {
                        ci.nsName = it->second.nsName;
                        ci.modName = it->second.moduleName;
                    }
                } else {
                    ci.modName = imp.path;
                }
                cgImports.push_back(ci);
            }
            Aura::CodeGenerator cg(*diags[mod->sourcePath]);
            units[mod->sourcePath] = cg.generate(*mod->ast, mod->moduleName, cgImports,
                                                 mod->nsName, Aura::CodeGenConfig{},
                                                 {}, {}, "", Aura::NullMetadata);
        }
    }
    return units;
}

// 取某模块的产物（按源文件 stem 找）
const Aura::CompileUnit* unitOf(const std::map<std::string, Aura::CompileUnit>& units,
                                const std::string& stem) {
    for (auto& [path, u] : units)
        if (std::filesystem::path(path).stem().string() == stem) return &u;
    return nullptr;
}

bool has(const std::string& s, const std::string& sub) {
    return s.find(sub) != std::string::npos;
}

// 模块 A：enum（形态 1）+ 方法 + 持有该 enum 的 record（避免「模块 A 自己的函数签名
//   含 enum」——那是另一族既有缺陷：枚举值在元数据 thunk 里被当 GcObject*，与本批无关）
const char* kModA =
    "pub enum type Color = { Red, Green, Blue }\n"
    "pub fun (self Color) hex() -> int { return 1 }\n"
    "pub fun (self Color) is_red() -> bool { return self == Color.Red }\n"
    "pub type Holder = { c: Color }\n"
    "pub fun mk_holder() -> Holder { return Holder { c = Color.Red } }\n";

} // namespace

// ============================================================
// ① 导入 enum 的**值**：类型标注必须按值（无 `*`、无 GcRootHandle）、方法调用用 `.`
// ============================================================
TEST(CodeGenEnumImport, ImportedEnumValueTypeAndMethodCall) {
    auto dir = enumImportTempDir();
    writeEnumImportAura(dir, "modA.aura", kModA);
    std::string entry = writeEnumImportAura(dir, "modB.aura",
        "import \"modA.aura\" as a\n"
        "fun main(io: Io) {\n"
        "    let h = a.mk_holder()\n"
        "    let c: a.Color = h.c\n"
        "    let n = c.hex()\n"
        "    io.println(str(n))\n"
        "}\n");
    Aura::DiagnosticEngine diag;
    auto units = compileModuleGraph(entry, diag);
    EXPECT_FALSE(diag.hasErrors());
    const Aura::CompileUnit* b = unitOf(units, "modB");
    EXPECT_TRUE(b != nullptr);
    if (b) {
        // 值语义：`a::Color c = …`（不得是 `a::Color* c_raw = …` + GcRootHandle）
        EXPECT_TRUE(has(b->impl, "a::Color c = "));
        EXPECT_FALSE(has(b->impl, "a::Color* "));
        EXPECT_FALSE(has(b->impl, "GcRootHandle<a::Color*>"));
        // 方法调用走成员访问（值 ⇒ `.`，不是 `->`）
        EXPECT_TRUE(has(b->impl, "c.hex()"));
        EXPECT_FALSE(has(b->impl, "c->hex("));
        // 导入 enum 不是泛型形参（曾产出虚假 `template<typename Color>`）
        EXPECT_FALSE(has(b->header, "template<typename Color>"));
        EXPECT_FALSE(has(b->impl, "template<typename Color>"));
    }
    const Aura::CompileUnit* a = unitOf(units, "modA");
    EXPECT_TRUE(a != nullptr);
    if (a) {
        // 本单元声明的 enum：壳内方法声明带 `const`；方法体带值接收者入口拷贝
        EXPECT_TRUE(has(a->header, "int32_t hex() const;"));
        EXPECT_TRUE(has(a->impl, "const Color self = *this;"));
    }
    std::filesystem::remove_all(dir);
}

// ============================================================
// ② 导入 enum 的 **match**：tag 比较里的壳名必须带模块别名前缀（`a::Color::_Tag::…`）
// ============================================================
TEST(CodeGenEnumImport, ImportedEnumMatchUsesQualifiedShellName) {
    auto dir = enumImportTempDir();
    writeEnumImportAura(dir, "modA.aura", kModA);
    std::string entry = writeEnumImportAura(dir, "modB.aura",
        "import \"modA.aura\" as a\n"
        "fun main(io: Io) {\n"
        "    let h = a.mk_holder()\n"
        "    let c: a.Color = h.c\n"
        "    match c {\n"
        "        Red => { io.println(\"r\") }\n"
        "        Green => { io.println(\"g\") }\n"
        "        Blue => { io.println(\"b\") }\n"
        "    }\n"
        "}\n");
    Aura::DiagnosticEngine diag;
    auto units = compileModuleGraph(entry, diag);
    EXPECT_FALSE(diag.hasErrors());
    const Aura::CompileUnit* b = unitOf(units, "modB");
    EXPECT_TRUE(b != nullptr);
    if (b) {
        EXPECT_TRUE(has(b->impl, "a::Color::_Tag::Red"));
        // 裸 `Color::_Tag::` 不得出现（本命名空间内不可见 ⇒ 坏 C++）
        EXPECT_FALSE(has(b->impl, " == Color::_Tag::"));
    }
    std::filesystem::remove_all(dir);
}

// ============================================================
// ③ 反向：**本单元声明**的 enum 产物逐字不变（F-1 的口径不破：declModule 为空 ⇒ 不加前缀）
// ============================================================
TEST(CodeGenEnumImport, LocalEnumArtifactUnchanged) {
    auto dir = enumImportTempDir();
    writeEnumImportAura(dir, "modA.aura", kModA);
    std::string entry = writeEnumImportAura(dir, "modB.aura",
        "import \"modA.aura\" as a\n"
        "fun main(io: Io) { let h = a.mk_holder() }\n");
    Aura::DiagnosticEngine diag;
    auto units = compileModuleGraph(entry, diag);
    EXPECT_FALSE(diag.hasErrors());
    const Aura::CompileUnit* a = unitOf(units, "modA");
    EXPECT_TRUE(a != nullptr);
    if (a) {
        EXPECT_TRUE(has(a->header, "enum class _Tag : uint8_t { Red = 0, Green = 1, Blue = 2 };"));
        EXPECT_TRUE(has(a->header, "static const Color Red;"));
        // 类外定义 / 匹配比较都用**裸**壳名（本模块内可见）
        EXPECT_TRUE(has(a->impl, "const Color Color::Red{Color::_Tag::Red};"));
        EXPECT_FALSE(has(a->impl, "aura_mod_modA::Color::_Tag::Red"));
    }
    std::filesystem::remove_all(dir);
}
