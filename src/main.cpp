// ============================================================
// Aura Compiler — main entry
//
// Usage:
//   aurac <input.aura>                     # parse → cpp → compile → run
//   aurac <input.aura> --cpp <file>        # translate to C++
//   aurac <input.aura> --ast <file>        # output AST only
//   aurac <input.aura> -S                  # stop after C++ (no compile)
//   aurac <input.aura> -o <dir>            # output directory
//
// Options:
//   --ast <path>     Write AST dump to file
//   --cpp <path>     Write translated C++20 code to file
//   -S               Stop after generating .cpp (no g++ compile)
//   -o <dir>         Output directory (default: same as input)
// ============================================================

#include "Lexer.h"
#include "Parser.h"
#include "ASTPrinter.h"
#include "Sema/SemAnalyzer.h"
#include "CodeGen/CodeGen.h"

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
    std::string astOutput;       // AST 输出路径
    std::string cppOutput;       // C++ 翻译输出路径
    std::string outputDir;       // 输出目录（-o）
    bool        stopAfterCpp = false; // -S：只生成 cpp 不编译
};

CliOptions parseArgs(const std::vector<std::string_view>& args) {
    CliOptions opts;
    for (size_t i = 0; i < args.size(); ++i) {
        std::string_view arg = args[i];
        if (arg == "-o" && i + 1 < args.size()) {
            opts.outputDir = args[++i];
        } else if (arg == "--ast" && i + 1 < args.size()) {
            opts.astOutput = args[++i];
        } else if (arg == "--cpp" && i + 1 < args.size()) {
            opts.cppOutput = args[++i];
        } else if (arg == "-S") {
            opts.stopAfterCpp = true;
        } else if (!arg.empty() && arg[0] != '-') {
            opts.inputPath = arg;
        }
    }
    return opts;
}

// ============================================================
// 工具
// ============================================================
std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::cerr << "Error: cannot open '" << path << "'\n";
        std::exit(1);
    }
    std::ostringstream oss;
    oss << f.rdbuf();
    return oss.str();
}

void writeFile(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        std::cerr << "Error: cannot write '" << path << "'\n";
        std::exit(1);
    }
    f << content;
}

std::string stemOf(const std::string& path) {
    namespace fs = std::filesystem;
    return fs::path(path).stem().string();
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
        std::cerr << "Usage: aura <input.aura> [--cpp <file>] [--ast <file>] [-S] [-o <dir>]\n";
        return 1;
    }

    // 2. 读取源码
    std::string source = readFile(opts.inputPath);
    std::string moduleName = stemOf(opts.inputPath);

    // 3. 词法分析
    Aura::Lexer lexer(source);
    auto tokens = lexer.scanAll();

    // 4. 语法分析
    Aura::Parser parser(std::move(tokens));
    auto program = parser.parse();

    if (!parser.errors().empty()) {
        std::cerr << "Parse errors (" << parser.errors().size() << "):\n";
        for (auto& err : parser.errors()) std::cerr << "  " << err << '\n';
        return 1;
    }
    if (!program) {
        std::cerr << "Error: failed to parse program.\n";
        return 1;
    }

    // 5. 语义分析
    Aura::SemAnalyzer sema;
    bool semaOk = sema.analyze(*program);
    if (!semaOk) {
        std::cerr << "Semantic errors:\n";
        for (auto& err : sema.errors()) std::cerr << "  " << err << '\n';
        return 1;
    }

    // 6. AST 输出（仅当显式指定 --ast 时）
    if (!opts.astOutput.empty()) {
        std::ostringstream astOut;
        program->print(astOut, 0);
        std::string astPath = opts.astOutput;
        if (!opts.outputDir.empty() && astPath.find('/') == std::string::npos)
            astPath = opts.outputDir + "/" + astPath;
        writeFile(astPath, astOut.str());
    }

    // 7. C++ 代码生成
    Aura::CodeGenerator cg;
    auto unit = cg.generate(*program, moduleName);

    if (!cg.errors().empty()) {
        std::cerr << "CodeGen errors:\n";
        for (auto& err : cg.errors()) std::cerr << "  " << err << '\n';
        return 1;
    }

    // 8. 确定 cpp 输出路径
    std::string cppPath = opts.cppOutput;
    if (cppPath.empty()) {
        std::string dir = opts.outputDir.empty()
            ? std::filesystem::path(opts.inputPath).parent_path().string()
            : opts.outputDir;
        cppPath = dir.empty() ? (moduleName + ".gen.cpp")
                              : (dir + "/" + moduleName + ".gen.cpp");
    }

    // 9. 写出 cpp 文件
    {
        std::ostringstream fullCpp;
        fullCpp << unit.header;
        fullCpp << "// ============================================================\n";
        fullCpp << "// " << moduleName << ".aura → C++20 translation\n";
        fullCpp << "// ============================================================\n\n";
        fullCpp << unit.impl;
        if (!unit.footer.empty()) fullCpp << "\n" << unit.footer;
        writeFile(cppPath, fullCpp.str());
    }

    // 10. 编译（除非 -S）
    if (opts.stopAfterCpp) {
        return 0;
    }

    // 确定 exe 输出路径
    std::string exePath = std::filesystem::path(cppPath).replace_extension(".exe").string();

    // 构建 g++ 命令（-w 禁止所有 warning，-Werror 不必要因为我们只想看到 fatal errors）
    std::ostringstream compileCmd;
    compileCmd << "g++ -std=gnu++20 -fcoroutines -O0 -g"
               << " -w"                           // suppress all warnings
               << " -I runtime"
               << " \"" << cppPath << "\""
               << " runtime/build/libaura_rt.a"
               << " -o \"" << exePath << "\""
               << " 2>&1";

    int compileRet = std::system(compileCmd.str().c_str());
    if (compileRet != 0) {
        std::cerr << "Compilation failed.\n";
        return 1;
    }

    return 0;
}
