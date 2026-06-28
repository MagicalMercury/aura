// ============================================================
// Aura Compiler — main entry
//
// Usage:
//   aurac <input.aura> --cpp <file>        # single-file: C++ output path
//   aurac <input.aura> --cpp <dir>         # multi-file:  output directory for .cpp/.h
//   aurac <input.aura> --ast <file>        # output AST only
//   aurac <input.aura> -G0                  # compile with -g -O0 (default)
//   aurac <input.aura> -G1                  # compile with -O3
//   aurac <input.aura> -s                   # compile with -Os (size, overrides -G)
//
// Options:
//   --ast <path>     Write AST dump to file
//   --cpp <path>     Single-file: write C++ file. Multi-file: output directory for .cpp/.h
//   -S               Stop after generating .cpp (no g++ compile)
//   -o <dir>         Output directory (default: same as input)
//   -G0              g++ flags: -g -O0 (debug, default)
//   -G1              g++ flags: -O3 (optimized)
//   -s               g++ flags: -Os (size-optimized, overrides -G)
// ============================================================

#include "Lexer.h"
#include "Parser.h"
// #include "ASTPrinter.h"  // DEPRECATED
#include "Sema/SemAnalyzer.h"
#include "CodeGen/CodeGen.h"
#include "Module/ModuleManager.h"
#include "Diag/DiagnosticEngine.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

// ============================================================
// 命令行参数解析
// ============================================================
struct CliOptions {
    std::string inputPath;       // 输入 .aura 文件
    // std::string astOutput;       // AST 输出路径  // DEPRECATED
    std::string cppOutput;       // C++ 翻译输出路径（单文件=文件，多文件=目录）
    std::string outputDir;       // 输出目录（-o，多文件模式下是 .cpp/.h 输出目录）
    std::string exeOutput;       // exe 输出路径（多文件模式下由 -o 指定）
    bool        stopAfterCpp = false; // -S：只生成 cpp 不编译
    int         gLevel       = 0;     // -G0（默认 debug）或 -G1（release）
    bool        sizeOptimize = false; // -s：-Os 体积优化
};

CliOptions parseArgs(const std::vector<std::string_view>& args) {
    CliOptions opts;
    for (size_t i = 0; i < args.size(); ++i) {
        std::string_view arg = args[i];
        if (arg == "-o" && i + 1 < args.size()) {
            opts.exeOutput = args[++i];
        } else if (false /* DEPRECATED: --ast */ && arg == "--ast" && i + 1 < args.size()) {
            // opts.astOutput = args[++i];  // DEPRECATED
        } else if (arg == "--cpp" && i + 1 < args.size()) {
            opts.cppOutput = args[++i];
        } else if (arg == "-S") {
            opts.stopAfterCpp = true;
        } else if (arg == "-G0") {
            opts.gLevel = 0;
        } else if (arg == "-G1") {
            opts.gLevel = 1;
        } else if (arg == "-s") {
            opts.sizeOptimize = true;
        } else if (!arg.empty() && arg[0] != '-') {
            opts.inputPath = arg;
        }
    }
    return opts;
}

// ============================================================
// 编译 g++ 参数
// ============================================================
std::string gccFlags(const CliOptions& opts) {
    if (opts.sizeOptimize) return "-Os";
    if (opts.gLevel == 0)  return "-g -O0";
    return "-O3";
}

// ============================================================
// 单文件编译（原有逻辑，无 import 或仅内置模块）
// ============================================================
int compileSingleFile(const CliOptions& opts, Aura::DiagnosticEngine& diag) {
    std::string source = Aura::readFile(opts.inputPath);
    diag.setSourceView(source);
    diag.reset();
    std::string moduleName = Aura::stemOf(opts.inputPath);

    // 词法 + 语法
    Aura::Lexer lexer(source);
    auto tokens = lexer.scanAll();
    Aura::Parser parser(std::move(tokens), diag);
    auto program = parser.parse();

    if (!program) {
        std::cerr << "Error: failed to parse program.\n";
        return 1;
    }

    // 语义分析（尽力模式：Parser 有非致命错误也继续）
    Aura::SemAnalyzer sema(diag);
    (void)sema.analyze(*program);

    // C++ 代码生成（仅在无错误时生成可用代码）
    if (!diag.hasErrors()) {
        Aura::CodeGenerator cg(diag);
        Aura::CodeGenConfig cfg;
        cfg.setConfig(sema);
        auto unit = cg.generate(*program, moduleName, {}, "", cfg);
        if (diag.hasErrors()) {
            std::cerr << "Compilation failed with " << diag.errorCount() << " error(s):\n";
            diag.print(std::cerr);
            return 1;
        }

        // 确定 cpp 输出路径
        std::string cppPath = opts.cppOutput;
        if (cppPath.empty()) {
            std::string dir = opts.outputDir.empty()
                ? std::filesystem::path(opts.inputPath).parent_path().string()
                : opts.outputDir;
            cppPath = dir.empty() ? (moduleName + ".gen.cpp")
                                  : (dir + "/" + moduleName + ".gen.cpp");
        }

        // 写出
        {
            std::ostringstream fullCpp;
            fullCpp << unit.header;
            fullCpp << "// ============================================================\n";
            fullCpp << "// " << moduleName << ".aura → C++20 translation\n";
            fullCpp << "// ============================================================\n\n";
            fullCpp << unit.impl;
            if (!unit.footer.empty()) fullCpp << "\n" << unit.footer;
            Aura::writeFile(cppPath, fullCpp.str());
        }

        if (opts.stopAfterCpp) return 0;

        // 编译
        std::string exePath = std::filesystem::path(cppPath).replace_extension(".exe").string();
        std::ostringstream compileCmd;
        compileCmd << "g++ -std=gnu++20 -fcoroutines " << gccFlags(opts)
                   << " -w -I runtime"
                   << " \"" << cppPath << "\""
                   << " runtime/build/libaura_rt.a"
                   << " -o \"" << exePath << "\""
                   << " 2>&1";

        int compileRet = std::system(compileCmd.str().c_str());
        if (compileRet != 0) {
            std::cerr << "Compilation failed.\n";
            return 1;
        }

        if (opts.cppOutput.empty())
            std::filesystem::remove(cppPath);

        return 0;
    }

    // 有错误 → 统一报告
    std::cerr << "Compilation failed with " << diag.errorCount() << " error(s):\n";
    diag.print(std::cerr);
    return 1;
}

// ============================================================
// 多文件编译（有用户模块 import）
// keepIntermediate: true = 保留 .cpp/.h 中间文件
// ============================================================
int compileMultiFile(const CliOptions& opts, bool keepIntermediate, Aura::DiagnosticEngine& diag) {
    diag.reset();
    Aura::ModuleManager mgr(diag);

    // 1. 加载所有模块
    if (!mgr.loadAll(opts.inputPath)) {
        std::cerr << "Compilation failed with " << diag.errorCount() << " error(s):\n";
        diag.print(std::cerr);
        return 1;
    }

    // 2. 循环检测
    if (mgr.hasCycle()) {
        diag.print(std::cerr);
        return 1;
    }

    // 3. 拓扑分层
    auto layers = mgr.topologicalLayers();

    // 4. 入口点验证
    std::string entryModulePath;
    if (!mgr.validateEntry(entryModulePath)) {
        diag.print(std::cerr);
        return 1;
    }

    // 5. 语义分析各模块
    for (auto& layer : layers) {
        for (auto* mod : layer) {
            if (mod->isBuiltin) continue;
            if (!mod->ast) continue;
            Aura::SemAnalyzer sema(diag);
            (void)sema.analyze(*mod->ast);
        }
    }
    if (diag.hasErrors()) {
        std::cerr << "Compilation failed with " << diag.errorCount() << " error(s):\n";
        diag.print(std::cerr);
        return 1;
    }

    // 6. 确定输出目录
    std::string outDir = opts.outputDir;
    if (outDir.empty()) {
        outDir = std::filesystem::absolute(opts.inputPath).parent_path().string();
    }
    // 确保目录存在
    std::filesystem::create_directories(outDir);

    // 7. 收集所有生成的 .cpp 文件路径
    std::vector<std::string> allCppPaths;

    // 7. 按层编译各模块
    for (auto& layer : layers) {
        for (auto* mod : layer) {
            if (mod->isBuiltin) continue;

            // 构建 import 信息列表
            std::vector<Aura::CodeGenImport> cgImports;
            for (auto& imp : mod->imports) {
                Aura::CodeGenImport ci;
                ci.path      = imp.path;
                ci.alias     = imp.alias;
                ci.isBuiltin = imp.isBuiltin;
                if (!imp.isBuiltin) {
                    auto it = mgr.modules().find(imp.path);
                    if (it != mgr.modules().end()) {
                        ci.nsName   = it->second.nsName;
                        ci.modName  = it->second.moduleName;
                    }
                } else {
                    ci.modName = imp.path; // 内置模块名即命名空间键
                }
                cgImports.push_back(ci);
            }

            // 代码生成
            Aura::CodeGenerator cg(diag);
            auto unit = cg.generate(*mod->ast, mod->moduleName, cgImports, mod->nsName);

            if (diag.hasErrors()) {
                std::cerr << "CodeGen errors in " << mod->sourcePath << ":\n";
                diag.print(std::cerr);
                return 1;
            }

            // 写出头文件
            std::string hdrPath = outDir + "/" + mod->moduleName + ".aura.h";
            {
                std::ostringstream hdr;
                hdr << unit.header;
                if (!unit.footer.empty()) {
                    // footer 属于 .cpp，不属于 .h
                }
                Aura::writeFile(hdrPath, hdr.str());
            }

            // 写出实现文件
            std::string cppPath = outDir + "/" + mod->moduleName + ".aura.cpp";
            {
                std::ostringstream implCpp;
                implCpp << "#include \"" << mod->moduleName << ".aura.h\"\n";
                implCpp << unit.impl;
                if (!unit.footer.empty()) implCpp << "\n" << unit.footer;
                Aura::writeFile(cppPath, implCpp.str());
            }

            allCppPaths.push_back(cppPath);

            std::cerr << "  compiled: " << mod->moduleName << ".aura\n";
        }
    }

    // 入口模块总是在最后一层或接近最后一层
    std::string entryModuleName = Aura::stemOf(entryModulePath);
    // -o 指定 exe，否则用入口模块名
    std::string exePath = opts.exeOutput;
    if (exePath.empty()) {
        exePath = outDir + "/" + entryModuleName + ".exe";
    }

    if (opts.stopAfterCpp) {
        std::cerr << "Output: " << outDir << "\n";
        return 0;
    }

    // 8. 链接
    std::ostringstream compileCmd;
    compileCmd << "g++ -std=gnu++20 -fcoroutines " << gccFlags(opts)
               << " -w -I " << outDir
               << " -I runtime";
    for (auto& cpp : allCppPaths)
        compileCmd << " \"" << cpp << "\"";
    compileCmd << " runtime/build/libaura_rt.a"
               << " -o \"" << exePath << "\""
               << " 2>&1";

    int compileRet = std::system(compileCmd.str().c_str());
    if (compileRet != 0) {
        std::cerr << "Compilation failed.\n";
        return 1;
    }

    // 清理中间文件
    if (!keepIntermediate) {
        for (auto& cpp : allCppPaths)
            std::filesystem::remove(cpp);
        for (auto& [path, info] : mgr.modules()) {
            if (!info.isBuiltin) {
                std::string hdr = outDir + "/" + info.moduleName + ".aura.h";
                std::filesystem::remove(hdr);
            }
        }
    }

    std::cerr << "Output: " << exePath << "\n";
    return 0;
}

// ============================================================
// 主函数
// ============================================================
int main(int argc, char* argv[]) {
    // 1. 解析命令行
    std::vector<std::string_view> args;
    for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
    auto opts = parseArgs(args);

    if (opts.inputPath.empty()) {
        std::cerr << "Usage:\n";
        std::cerr << "  aurac <input.aura>                     # parse → cpp → compile\n";
        std::cerr << "  aurac <input.aura> --cpp <file>        # single-file: C++ output path\n";
        std::cerr << "  aurac <input.aura> --cpp <dir>         # multi-file:  output directory for .cpp/.h\n";
        std::cerr << "  aurac <input.aura> --ast <file>        # output AST only\n";
        std::cerr << "\nOptions:\n";
        std::cerr << "  --ast <path>     Write AST dump to file\n";
        std::cerr << "  --cpp <path>     Single-file: write C++ file. Multi-file: output directory for .cpp/.h\n";
        std::cerr << "  -S               Stop after generating .cpp (no g++ compile)\n";
        std::cerr << "  -o <dir>         Output directory (default: same as input)\n";
        std::cerr << "  -G0              g++ flags: -g -O0 (debug, default)\n";
        std::cerr << "  -G1              g++ flags: -O3 (optimized)\n";
        std::cerr << "  -s               g++ flags: -Os (size-optimized, overrides -G)\n";
        return 1;
    }

    Aura::DiagnosticEngine diag;
    diag.setFileName(opts.inputPath);

    // 2. 快速检查入口文件是否有 import 用户模块
    {
        std::string source = Aura::readFile(opts.inputPath);
        diag.setSourceView(source);
        Aura::Lexer lexer(source);
        auto tokens = lexer.scanAll();
        Aura::Parser parser(std::move(tokens), diag);
        auto program = parser.parse();

        if (program) {
            bool hasUserImport = false;
            for (auto& d : program->decls) {
                if (auto* imp = dynamic_cast<Aura::ImportDecl*>(d.get())) {
                    if (!imp->isBuiltin) {
                        hasUserImport = true;
                        break;
                    }
                }
            }
            if (hasUserImport) {
                // 多文件模式：--cpp 是 .cpp/.h 输出目录，-o 是最终 exe 路径
                opts.outputDir = opts.cppOutput;  // --cpp 指定的目录
                diag.reset();  // 清除快速检查产生的错误
                return compileMultiFile(opts, !opts.cppOutput.empty(), diag);
            }
        }
    }

    // 3. 无用户模块 import → 单文件模式
    diag.reset();  // 清除快速检查产生的错误
    return compileSingleFile(opts, diag);
}
