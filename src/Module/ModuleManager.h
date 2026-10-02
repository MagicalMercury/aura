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
    // record 方法导出：record canonicalName（本地未限定）→ 方法签名。
    // 供导入模块合入方法表（importedMethods_），否则跨模块 record 方法调用在
    // Sema 查不到方法 → 被 bug-01 的 E013 误伤（跨模块已注册方法调用必须放行）。
    std::unordered_map<std::string, std::vector<InterfaceSemType::MethodSig>> methods;
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
// feature-13 C2（2026-09-17）：声明级扫描产物（DeclUnit）
//
// 由 ModuleManager::scanDeclarations() 从单个 .aura 文件的【声明级扫描】产出：
// 只含符号骨架（类型/接口/函数签名 + import 图 + hasMain），不含任何函数体。
//
// 定位：C2 只提供能力，不接入主流程（接入是 C4）。C3 的汇总段将把若干
// DeclUnit 合并为 GlobalSymbolTable；本结构即「每单元的收集形态」。
//
// ⚠️ deps 语义与 parseModule 一致（ModuleManager.cpp）：内置模块【只进 imports、
//     不进 deps】；deps 只装解析后的用户模块绝对路径。
// ============================================================

// 一个类型/接口声明的骨架
struct DeclSkeleton {
    std::string name;                      // 类型/接口名
    std::vector<std::string> typeParams;   // 泛型形参名（如 "T", "U"）
    bool isInterface = false;              // true = interface 声明
    bool isPublic = false;
    bool hasBody = false;                  // type X = ... 是否有右侧类型表达式
};

// 一个函数/方法声明的骨架
struct FuncSkeleton {
    std::string name;                      // 函数名（含 .aurai 的 "path.new" 形态）
    std::vector<std::string> paramTypes;   // 参数类型串（尽力而为：取不到类型时为空串）
    std::string returnType;                // 返回类型串（尽力而为；无返回类型 = 空串）
    bool throws = false;
    bool isPublic = false;
    bool hasBody = false;                  // 是否有实体 body（.aurai 的桥接标记 -> false）
    bool hasCppImpl = false;               // 桥接标记（三连点）
    std::string receiverType;              // 方法：接收者类型名（普通函数 = 空串）
};

// 一个编译单元的声明骨架（= scanDeclarations 的返回值）
struct DeclUnit {
    std::string sourcePath;                // .aura 源文件绝对路径
    std::string moduleName;                // module 声明优先 / 文件 stem 兜底（C0 逻辑）
    bool hasExplicitModule = false;        // 是否有显式 module 声明
    std::vector<ImportInfo> imports;       // 复用既有 ImportInfo（与 parseModule 同语义）
    std::vector<std::string> deps;         // 解析后的绝对路径（仅用户模块；内置不进）
    bool hasMain = false;                  // 是否定义 fun main(io: Io)
    std::vector<DeclSkeleton> types;
    std::vector<FuncSkeleton> funcs;
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
    bool         hasMain = false;         // 是否定义了 fun main(io: Io) 函数
    // feature-13 C0（2026-09-17）：本模块是否带显式 `module <name>` 声明。
    // 判定用：显式 → nsName 统一无哈希（同 module 多文件共享 namespace）；
    //         隐式 → stem 派生 + 目录哈希防撞（见 pathToNs / C3 冲突检测）。
    bool         hasExplicitModule = false;

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
// feature-13 汇总段：GlobalSymbolTable（v1 收敛形态，D-C1）
//
// 若干 DeclUnit 经 scanAll 合并为一张全局表。v1 只承载
// 「依赖图 / 入口验证 / module 冲突」——环检测 / 拓扑分层 /
// 入口验证 / 冲突判定全部直接读 units（每单元的 deps/hasMain/
// moduleName 已由 C2 扫描对齐）。
//
// ⚠️ depGraph / symbols（完整 SymbolEntry 索引）按 D-C1 不建：
//   「符号表取代 exports」+ 完整符号索引留 v2 / feature-10。
// ============================================================
struct GlobalSymbolTable {
    std::unordered_map<std::string, DeclUnit> units;   // sourcePath → DeclUnit
};

// ============================================================
// ModuleManager — 模块管理器
// ============================================================
class ModuleManager {
public:
    ModuleManager(DiagnosticEngine& diag);

    // 从入口文件开始，递归加载所有依赖模块
    bool loadAll(const std::string& entryPath);

    // feature-13 C2（2026-09-17）：声明级扫描单个 .aura 文件 -> DeclUnit。
    // 与 parseModule 的关键差异：
    //   - 用 Parser::parseDeclarationsOnly()（跳过所有函数体，不建 body 节点）
    //   - 不把 AST 常驻（本方法只产出骨架；A2 决策：扫描只出符号，AST 丢弃）
    //   - 不触发 loadAuraiFile（内置 .aurai 的按需加载是 parseModule 的职责）
    // 不接入主流程（接入是 C4）；不修改 parseModule 行为（C2 零回归前提）。
    DeclUnit scanDeclarations(const std::string& sourcePath);

    // feature-13 C3（2026-09-17）：声明级【递归发现】全部依赖 -> 汇总表 scanUnits_。
    // 与 loadAll 的关系：loadAll = 完整解析 + 依赖发现（现状主流程）；
    // scanAll = 只用声明级扫描完成依赖发现（f13 真增量：不需要完整解析即可
    // 得到依赖图 / 入口 / module 冲突）。scanAll 不触发 loadAuraiFile、
    // 不填充 modules_（modules_ 的填充是 C4 的第二段 loadAllScanned 职责）。
    bool scanAll(const std::string& entryPath);
    const GlobalSymbolTable& globalTable() const { return scanUnits_; }

    // feature-18 P3（批 2）：🔴 `sanitizeId` 由 `private:` 段**移入 `public:` 段** ——
    //   依据：`main.cpp` 构造 `MetaCollector` 需 `Aura::ModuleManager::sanitizeId(stem)` 生成
    //   thunk 的 `nsStem`（change.md §3.6a/§3.6b 的写法），而原声明位于 `private:`（本文件 `:215` 起）
    //   ⇒ 外部**够不着**。这与 O40-(a)（`escapeStringLiteral`）**同族**：**「名字存在 ≠ 你能调」**。
    //   ⚠️ 只移动声明：签名不变、函数体不变、既有调用点语义不变。
    static std::string sanitizeId(const std::string& path);
    // 非 const 访问（C4 主流程第二段写入需要）
    std::unordered_map<std::string, ModuleInfo>& modulesInternal() { return modules_; }
    ModuleInfo* moduleAt(const std::string& sourcePath) {
        auto it = modules_.find(sourcePath);
        return it == modules_.end() ? nullptr : &it->second;
    }

    // feature-13 C3：基于扫描表（scanUnits_）的环检测 / 拓扑 / 入口 / 冲突。
    // 与 hasCycle / topologicalLayers / validateEntry 的区别：读扫描表而非
    // modules_（依赖图在【完整解析之前】就可判定——f13 真增量）。
    // ⚠️ 不改动既有三方法（零回归）；C4 主流程切换后既有三方法若无消费方
    //    再由后续批次收口（本批不删）。
    bool checkModuleConflicts() const;                       // D12 两分判定
    bool hasCycleOn() const;                                 // DFS 三色（扫描表）
    std::vector<std::vector<std::string>> topologicalLayersOn() const;  // Kahn（返回 path 分组）
    bool validateEntryOn(std::string& outEntry) const;       // main(io: Io) 唯一性

    // feature-13 C4：第二段完整解析——对扫描表每个单元 parseModule 填充 modules_。
    // 依赖发现已由 scanAll 完成；本方法不再递归（A2：第二段重新完整解析）。
    // modules_ 填充顺序与调度序无关（runSemaModule 按拓扑层消费）。
    bool loadAllScanned();

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

    // （feature-18 P3：`sanitizeId` 声明已移入 public 段 —— 见彼处注释）

    // 解析 import 语句的路径为绝对路径
    std::string resolveImportPath(const std::string& importPath,
                                   const std::string& importerDir) const;

    // 内置模块白名单
    bool isKnownBuiltin(const std::string& name) const;

    std::unordered_map<std::string, ModuleInfo> modules_;
    // feature-13 C3：声明级扫描的汇总表（scanAll 填充；种族检测/拓扑/入口/冲突读它）
    GlobalSymbolTable scanUnits_;
    DiagnosticEngine& diag_;
};

// ============================================================
// 工具：读取文件内容
// ============================================================
std::string readFile(const std::string& path);
void writeFile(const std::string& path, const std::string& content);
std::string stemOf(const std::string& path);

} // namespace Aura
