// ============================================================
// test_helpers.h — Aura 编译器测试公共辅助函数
//
// 提供：源码 → Token / AST / 语义分析 / CodeGen 的便捷封装，
// 以及错误信息检索工具。
// ============================================================
#pragma once

#include "Lexer.h"
#include "Parser.h"
#include "AST/ASTNode.h"
#include "AST/Stmt.h"
#include "AST/Expr.h"
#include "AST/Type.h"
#include "Sema/SemAnalyzer.h"
#include "CodeGen/CodeGen.h"
#include "Module/ModuleManager.h"
#include "Diag/DiagnosticEngine.h"

#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace Aura {
// 让 TokType 可被 << 输出，便于 EXPECT_EQ 失败时打印
inline std::ostream& operator<<(std::ostream& os, TokType t) {
    return os << tokTypeName(t);
}

// 让 InterfaceMethodSig::BodyKind 可被 << 输出
inline std::ostream& operator<<(std::ostream& os,
                                InterfaceMethodSig::BodyKind k) {
    switch (k) {
        case InterfaceMethodSig::BodyKind::Pure:        return os << "Pure";
        case InterfaceMethodSig::BodyKind::DefaultAura: return os << "DefaultAura";
        case InterfaceMethodSig::BodyKind::CppBridge:   return os << "CppBridge";
    }
    return os << "?";
}

// 让 DiagCode / DiagSeverity 可被 << 输出（EXPECT_EQ 失败时打印）
inline std::ostream& operator<<(std::ostream& os, DiagCode c) {
    const char* s = diagCodeStr(c);
    return os << (s[0] ? s : "None");
}

inline std::ostream& operator<<(std::ostream& os, DiagSeverity s) {
    switch (s) {
        case DiagSeverity::Error:   return os << "Error";
        case DiagSeverity::Warning: return os << "Warning";
        case DiagSeverity::Note:    return os << "Note";
    }
    return os << "?";
}
} // namespace Aura

namespace aura_test {

// ============================================================
// 词法分析
// ============================================================
inline std::vector<Aura::Token> tokenize(const std::string& src) {
    Aura::Lexer lexer(src);
    return lexer.scanAll();
}

// ============================================================
// 解析：源码 → Program（含诊断）
// ============================================================
inline std::unique_ptr<Aura::Program> parseSource(
    const std::string& src, Aura::DiagnosticEngine& diag) {
    diag.setSourceView(src);
    diag.reset();
    Aura::Lexer lexer(src);
    auto tokens = lexer.scanAll();
    Aura::Parser parser(std::move(tokens), diag);
    return parser.parse();
}

// ============================================================
// 语义分析：源码 → Program + diag（含词法/语法/语义全流程）
// ============================================================
// 按需加载源码中 import 声明的内置模块 .aurai（如 import path → path.aurai），
// 与 main.cpp compileSingleFile 行为一致，否则 Sema 找不到 path/math 命名空间。
inline void loadImportedBuiltins(Aura::ModuleManager& mgr,
                                 const Aura::Program& program) {
    for (auto& d : program.decls) {
        if (auto* imp = dynamic_cast<Aura::ImportDecl*>(d.get())) {
            if (imp->isBuiltin) mgr.loadAuraiFile(imp->path + ".aurai");
        }
    }
}

inline std::unique_ptr<Aura::Program> analyzeSource(
    const std::string& src, Aura::DiagnosticEngine& diag,
    bool loadBuiltins = true) {
    diag.setSourceView(src);
    diag.reset();
    Aura::ModuleManager mgr(diag);
    if (loadBuiltins) mgr.loadBuiltinAurai();
    Aura::Lexer lexer(src);
    auto tokens = lexer.scanAll();
    Aura::Parser parser(std::move(tokens), diag);
    auto program = parser.parse();
    if (!program) return nullptr;
    loadImportedBuiltins(mgr, *program);
    Aura::SemAnalyzer sema(diag);
    (void)sema.analyze(*program);
    return program;
}

// ============================================================
// 完整编译：源码 → CompileUnit（CodeGen 输出）
// ============================================================
inline Aura::CompileUnit compileSource(
    const std::string& src, Aura::DiagnosticEngine& diag,
    const std::string& moduleName = "main",
    const Aura::CodeGenConfig& cfg = {}) {
    diag.setSourceView(src);
    diag.reset();
    Aura::ModuleManager mgr(diag);
    mgr.loadBuiltinAurai();
    Aura::Lexer lexer(src);
    auto tokens = lexer.scanAll();
    Aura::Parser parser(std::move(tokens), diag);
    auto program = parser.parse();
    Aura::CompileUnit unit;
    if (!program) return unit;
    loadImportedBuiltins(mgr, *program);
    Aura::SemAnalyzer sema(diag);
    (void)sema.analyze(*program);
    if (diag.hasErrors()) return unit;
    Aura::CodeGenerator cg(diag);
    // 🔴 **显式声明「本框架默认不收集元数据」**（2026-10-02 主 Agent，见 change.md §11.11）：
    //    原先只写 5 个实参、靠末位默认值 ⇒ `metaCollector == nullptr`。而 feature-18 P4a 的
    //    `emitEntryFrame` 当时**无条件**查 `frameSeqOf_` 并硬报错 ⇒ **411 个走本函数的用例
    //    全部转红**（另有 5 个在 test_codegen_concurrency_gc.cpp 直调 `generate` 的 7 参形态）。
    //    ⇒ 现在把 8 个可选参数**全部显式列出**，其中 `nullptr` = 「本框架这趟不收集元数据」
    //      —— 这是**合法模式**（不发射元数据表 ⇒ 不注入帧/行号，产物与 feature-17 逐字一致），
    //      由 `CodeGenFrame.NoCollectorProducesNoInjection` 用例显式断言。
    //    ⚠️ 若将来需要本框架也收集元数据，改这里的 `nullptr` 为 `&collector` 即可（单点）。
    return cg.generate(*program, moduleName, /*imports=*/{}, /*nsName=*/"", cfg,
                       /*crossDefaults=*/{}, /*crossParamSemTypes=*/{},
                       /*sourcePath=*/"", /*metaCollector=*/Aura::NullMetadata);
}

// ============================================================
// 多文件模块分析：按拓扑层分析所有模块
// 与 main.cpp compileMultiFile 的 Sema 流程一致：
//   每模块独立 SemAnalyzer/DiagnosticEngine，依赖模块导出表按层注入，
//   诊断按层内顺序 merge 到主 diag。
// 返回 false = 有错误（含循环依赖）
// ============================================================
inline bool analyzeModuleGraph(Aura::ModuleManager& mgr,
                               Aura::DiagnosticEngine& diag) {
    if (mgr.hasCycle()) return false;
    auto layers = mgr.topologicalLayers();
    std::map<std::string, std::unique_ptr<Aura::DiagnosticEngine>> moduleDiags;
    std::map<std::string, std::unique_ptr<Aura::SemAnalyzer>> moduleSemas;
    for (auto& layer : layers) {
        std::vector<Aura::ModuleInfo*> tasks;
        for (auto* mod : layer) {
            if (mod->isBuiltin || !mod->ast) continue;
            tasks.push_back(mod);
            auto modDiag = std::make_unique<Aura::DiagnosticEngine>();
            modDiag->setSourceView(Aura::readFile(mod->sourcePath));
            modDiag->setFileName(mod->sourcePath);
            moduleDiags[mod->sourcePath] = std::move(modDiag);
        }
        for (auto* mod : tasks) {
            auto sema = std::make_unique<Aura::SemAnalyzer>(*moduleDiags[mod->sourcePath]);
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
            moduleSemas[mod->sourcePath] = std::move(sema);
        }
        for (auto* mod : tasks) diag.mergeFrom(*moduleDiags[mod->sourcePath]);
    }
    return !diag.hasErrors();
}

// 分析一个 .aura 文件（含用户模块 import 依赖，递归加载）
inline bool analyzeFile(const std::string& path, Aura::DiagnosticEngine& diag) {
    diag.reset();
    Aura::ModuleManager mgr(diag);
    mgr.loadBuiltinAurai();
    if (!mgr.loadAll(path)) return false;
    return analyzeModuleGraph(mgr, diag);
}

// ============================================================
// 错误检索工具
// ============================================================
inline std::string joinErrorMessages(const Aura::DiagnosticEngine& diag) {
    std::ostringstream os;
    for (const auto& m : diag.errorMessages()) os << m << "\n";
    return os.str();
}

// 是否存在包含指定子串的错误
inline bool hasErrorContaining(const Aura::DiagnosticEngine& diag,
                               const std::string& substr) {
    for (const auto& m : diag.errorMessages())
        if (m.find(substr) != std::string::npos) return true;
    return false;
}

// 是否存在包含指定子串的警告
inline bool hasWarningContaining(const Aura::DiagnosticEngine& diag,
                                 const std::string& substr) {
    for (const auto& d : diag.diagnostics())
        if (d.severity == Aura::DiagSeverity::Warning &&
            d.message.find(substr) != std::string::npos) return true;
    return false;
}

// 是否存在指定错误码
inline bool hasErrorCode(const Aura::DiagnosticEngine& diag,
                         Aura::DiagCode code) {
    for (const auto& d : diag.diagnostics())
        if (d.severity == Aura::DiagSeverity::Error && d.code == code)
            return true;
    return false;
}

// 是否存在指定错误码且消息含子串
inline bool hasErrorCodeAndMsg(const Aura::DiagnosticEngine& diag,
                               Aura::DiagCode code,
                               const std::string& substr) {
    for (const auto& d : diag.diagnostics())
        if (d.severity == Aura::DiagSeverity::Error && d.code == code &&
            d.message.find(substr) != std::string::npos) return true;
    return false;
}

// 是否存在某条 error 的 fixHint（"= help: ..." 提示行）含指定子串
inline bool hasErrorHintContaining(const Aura::DiagnosticEngine& diag,
                                   const std::string& substr) {
    for (const auto& d : diag.diagnostics())
        if (d.severity == Aura::DiagSeverity::Error &&
            d.fixHint.find(substr) != std::string::npos) return true;
    return false;
}

// 打印 AST（用于调试/快照）
inline std::string dumpAst(const Aura::Program& program) {
    std::ostringstream os;
    program.print(os, 0);
    return os.str();
}

// ============================================================
// 类型安全动态转换辅助
// ============================================================
template <typename T>
inline const T* as(const Aura::ASTNode* node) {
    return dynamic_cast<const T*>(node);
}

// Pattern 不是 ASTNode 子类，单独提供转换
template <typename T>
inline const T* asPattern(const Aura::Pattern* node) {
    return dynamic_cast<const T*>(node);
}

// 从 Program 中取第 idx 个声明
inline const Aura::Decl* declAt(const Aura::Program& program, size_t idx) {
    if (idx >= program.decls.size()) return nullptr;
    return program.decls[idx].get();
}

} // namespace aura_test
