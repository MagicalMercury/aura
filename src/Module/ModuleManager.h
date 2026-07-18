#pragma once
// ============================================================
// ModuleManager — Aura 多文件模块管理器
//
// plan7 & plan8:
//   依赖扫描 → 循环检测 → 拓扑分层 → 按层编译
//
// 每个 .aura 文件为一个模块，解析后 AST 常驻内存。
// 内置模块（import path）不加载源文件，仅记录。
// ============================================================

#include "../AST/Stmt.h"
#include "../Diag/DiagnosticEngine.h"
#include "../Sema/SemType.h"
#include "../Sema/Symbol.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Aura {

// ============================================================
// FuncExport — 一个导出函数的签名
// ============================================================
struct FuncExport {
    std::vector<SymParam> params;
    std::unique_ptr<SemType> returnType;
    bool throws = false;
};

// ============================================================
// ModuleExports — 一个模块的导出表
// ============================================================
struct ModuleExports {
    std::string nsName;                                            // C++ 命名空间
    std::unordered_map<std::string, std::unique_ptr<SemType>> types; // 类型名 → SemType
    std::unordered_map<std::string, FuncExport> funcs;             // 函数名 → 签名
    std::unordered_map<std::string, FuncExport> ctors;             // 构造函数名 → 签名
};

// ============================================================
// ImportInfo — 一条 import 语句的解析结果
// ============================================================
struct ImportInfo {
    std::string path;        // 导入路径
    std::string alias;       // as 别名（空 = 无别名）
    bool        isBuiltin;   // 内置模块 / 外部包
};

// ============================================================
// ModuleInfo — 一个模块的元信息
// ============================================================
struct ModuleInfo {
    std::string sourcePath;              // .aura 源文件绝对路径
    std::string moduleName;              // 模块名（去扩展名，用于命名空间生成）
    std::string nsName;                  // C++ 命名空间（如 aura_mod_utils_math）
    std::string cppPath;                 // 生成 .gen.cpp 的路径
    std::string hdrPath;                 // 生成 .h 的路径（多文件模式）
    int         layer = -1;              // 拓扑层号
    bool        isBuiltin = false;       // 内置模块（不编译，仅 #include）
    bool        hasMain = false;         // 是否定义了 fun main(io: Io) 函数

    // 依赖：本模块 import 的用户模块 sourcePath 列表
    std::vector<std::string> deps;

    // import 信息列表（用于生成 #include 和命名空间别名）
    std::vector<ImportInfo> imports;

    // AST 常驻内存（§4.1 一次解析，编译阶段直接使用）
    std::unique_ptr<Program> ast;

    // 本模块的导出表（SemAnalyzer 分析后填充）
    ModuleExports exports;
};

// ============================================================
// ModuleManager — 模块管理器
// ============================================================
class ModuleManager {
public:
    ModuleManager(DiagnosticEngine& diag);

    // 从入口文件开始，递归加载所有依赖模块
    bool loadAll(const std::string& entryPath);

    // 加载 builtins/ 下的 .aurai 文件（始终加载：io.aurai）
    void loadBuiltinAurai();

    // 按需加载单个 .aurai 文件（如 import path → loadAuraiFile("path.aurai")）
    void loadAuraiFile(const std::string& baseName);

    // 循环依赖检测（Tarjan 算法）
    bool hasCycle();

    // 拓扑分层（Kahn BFS）
    // 返回：按层排列的 ModuleInfo 指针，同层的可并行编译
    std::vector<std::vector<ModuleInfo*>> topologicalLayers();

    // 入口点验证：确保有且仅有一个模块含 fun main(io: Io)
    bool validateEntry(std::string& outEntryModule);

    // 获取所有模块信息
    const std::unordered_map<std::string, ModuleInfo>& modules() const { return modules_; }

    // 获取错误信息
    const std::vector<std::string>& errors() const { return diag_.errorMessages(); }

private:
    // 解析单个 .aura 文件，返回 ModuleInfo（AST 常驻）
    // sourcePath 是文件路径，relativeTo 是导入者的目录（用于相对路径解析）
    ModuleInfo parseModule(const std::string& sourcePath);

    // 将模块路径转为 C++ 命名空间
    static std::string pathToNs(const std::string& path);

    // 将路径转为合法的 C++ 标识符
    static std::string sanitizeId(const std::string& path);

    // 解析 import 语句的路径为绝对路径
    std::string resolveImportPath(const std::string& importPath,
                                   const std::string& importerDir) const;

    // 内置模块白名单
    bool isKnownBuiltin(const std::string& name) const;

    std::unordered_map<std::string, ModuleInfo> modules_;
    DiagnosticEngine& diag_;
};

// ============================================================
// 工具：读取文件内容
// ============================================================
std::string readFile(const std::string& path);
void writeFile(const std::string& path, const std::string& content);
std::string stemOf(const std::string& path);

} // namespace Aura
