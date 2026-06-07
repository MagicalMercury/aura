#pragma once

#include "../AST/ASTNode.h"
#include "../AST/Expr.h"
#include "../AST/Stmt.h"
#include "../AST/Type.h"
#include "../Lexer.h"
#include "../Token.h"
#include <functional>
#include <ostream>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace Aura {

// ============================================================
// PendingMethod — 暂存方法签名，供 genRecordStruct 嵌入 struct
// ============================================================
struct PendingMethod {
    std::string receiverType;
    std::string methodName;
    std::string returnTypeStr;
    std::vector<std::string> paramTypes;
    std::vector<std::string> paramNames;
};

// ============================================================
// CompileUnit — 一个编译单元的输出文件
//
// 每个 .aura 文件翻译为一个 .cpp 文件，可能有对应的 .h。
// header  — #include / 前向声明 / 接口定义
// impl    — 函数实现
// footer  — 标准入口（main 函数所在编译单元）
// ============================================================
struct CompileUnit {
    std::string moduleName;    // 文件名（去 .aura）
    std::string nsName;        // C++ 命名空间（如 aura_mod_math_utils，空 = 无命名空间包裹）
    std::string header;        // 头文件内容（类型定义、接口类、函数声明）
    std::string impl;          // 实现文件内容（函数体、_desc 实例）
    std::string footer;        // 主入口 int main(...)
    bool        hasMain = false;
};

// ============================================================
// ImportInfo — 一条 import 信息（供 CodeGen 生成 #include + 别名）
// ============================================================
struct CodeGenImport {
    std::string path;       // 导入路径（用于 #include 解析）
    std::string alias;      // as 别名（空 = 无别名）
    std::string modName;    // 模块名（用作命名空间别名键，如 "math_utils"）
    bool        isBuiltin;  // 内置模块
    std::string nsName;     // 目标命名空间
};

// ============================================================
// CoroDecision — 协程判定结果
//
// plan §4.8: 编译器扫描函数体决定是否生成协程。
//   Plain     → 普通 C++ 函数（直接返回 T）
//   Coroutine → C++20 协程（返回 task<T>）
// ============================================================
enum class CoroDecision {
    Plain,     // 无需挂起 → 普通函数
    Coroutine, // 需要挂起 → task<T> + co_await/co_return
};

// ============================================================
// TypeMapEntry — 一条类型映射记录
//
// plan §4.1: 类型映射规则表
// ============================================================
struct TypeMapEntry {
    std::string auraName;      // Aura 类型名（如 "int", "User"）
    std::string cppType;       // C++ 类型名（如 "int32_t", "User*"）
    bool        isPointer;     // 是否为指针类型（堆对象）
    bool        isGeneric;     // 是否为泛型参数
};

// ============================================================
// CodeGenerator — Aura → C++20 翻译器
// ============================================================
class CodeGenerator {
public:
    CodeGenerator();

    // -- 主入口 --
    // 生成一个编译单元（.aura → .cpp/.h）
    // imports: 该模块的 import 列表（用于生成 #include 和命名空间别名）
    // nsName:  该模块自己的 C++ 命名空间（空 = 不包裹命名空间）
    [[nodiscard]] CompileUnit generate(const Program& program,
                                        const std::string& moduleName = "main",
                                        const std::vector<CodeGenImport>& imports = {},
                                        const std::string& nsName = "");

    // -- 协程判定入口 --
    [[nodiscard]] CoroDecision decideCoro(const FunDecl& decl);
    [[nodiscard]] CoroDecision decideCoro(const MethodDecl& decl);

    // -- 错误 --
    const std::vector<std::string>& errors() const { return errors_; }

private:
    // ============================================================
    // spawn 变量捕获分析（plan3: 协程 + lambda 按值捕获 = UB）
    // ============================================================

    // 收集 Stmt 子树中所有 Identifier 引用名
    void collectIdRefs(const Stmt& stmt, std::set<std::string>& out) const;
    void collectIdRefsExpr(const ASTNode& expr, std::set<std::string>& out) const;

    // 收集 spawn 体内局部声明的变量名（let/const/for item/match binding/catch var）
    void collectDeclared(const Stmt& stmt, std::set<std::string>& out) const;

    // ============================================================
    // 泛型模板参数收集
    // ============================================================

    // 递归扫描类型表达式，收集所有泛型引用名（GenericTypeRef + NamedType.typeArgs）
    void collectTParams(const TypeExpr& type, std::set<std::string>& out) const;
    // 从函数声明中收集模板参数（params + returnType + receiverTypeArgs）
    [[nodiscard]] std::vector<std::string> collectFunTParams(const FunDecl& decl) const;
    [[nodiscard]] std::vector<std::string> collectMethodTParams(const MethodDecl& decl) const;

    // ============================================================
    // 类型映射
    // ============================================================

    // 将 Aura 类型表达式映射为 C++ 类型字符串
    // 如 NamedType("int") → "int32_t"
    //    NamedType("User") → "User*"
    //    ListType(Int) → "aura_rt::Array<int32_t>*"
    //    UnionType({User, None}) → "std::variant<User*, aura_rt::NoneType>"
    [[nodiscard]] std::string mapType(const TypeExpr& type);
    [[nodiscard]] std::string mapNamedType(const std::string& name);
    [[nodiscard]] std::string mapGenericRef(const GenericTypeRef& g);

    // 值类型映射（不加 *）
    [[nodiscard]] std::string mapValueType(const TypeExpr& type);

    // 判断类型是否为值类型（int/float/bool/None）
    [[nodiscard]] bool isValueType(const std::string& auraName) const;

    // 检查类型是否是注册的堆对象类型（记录/接口/泛型记录）
    [[nodiscard]] bool isHeapType(const std::string& auraName) const;

    // 注册一个用户定义的类型名
    void registerTypeName(const std::string& auraName, bool isHeap);

    // ============================================================
    // 声明输出
    // ============================================================

    void genDecl(std::ostream& h, std::ostream& cpp,
                 const Decl& decl, CompileUnit& unit);

    // --- 类型声明 (§4.2, §4.4) ---
    void genTypeDecl(std::ostream& h, std::ostream& cpp, const TypeDecl& decl);
    void genRecordStruct(std::ostream& h, std::ostream& cpp,
                         const std::string& name, const RecordType& body,
                         const std::vector<std::string>& templateParams = {});

    // --- TypeDescriptor 生成 (§4.2, §4.10) ---
    void genTypeDescriptor(std::ostream& cpp,
                           const std::string& structName,
                           const std::vector<std::string>& templateParams,
                           const std::vector<std::string>& ptrFieldNames);

    // --- 接口声明 (§4.5) ---
    void genInterfaceDecl(std::ostream& h, const InterfaceDecl& decl);

    // --- 函数/方法声明 + 实现 ---
    void genFunDecl(std::ostream& h, std::ostream& cpp,
                    const FunDecl& decl);
    void genMethodDecl(std::ostream& h, std::ostream& cpp,
                       const MethodDecl& decl);
    void genConstructor(std::ostream& cpp,
                        const MethodDecl& decl);

    // --- 函数签名 ---
    [[nodiscard]] std::string funSignature(const FunDecl& decl,
                                              const std::vector<std::string>& tparams = {});
    [[nodiscard]] std::string constructorSignature(const MethodDecl& decl,
                                                       const std::vector<std::string>& tparams = {});

    // --- 函数体 ---
    void genBlock(std::ostream& cpp, const BlockStmt& block, bool isCoroutine);
    void genFunctionPrologue(std::ostream& cpp, const FunDecl& decl);
    void genFunctionEpilogue(std::ostream& cpp, const FunDecl& decl);

    // --- 主入口 (§5) ---
    void genMainEntry(std::ostream& cpp, const FunDecl& mainDecl, const std::string& nsName = "");

    // ============================================================
    // 语句输出
    // ============================================================

    void genStmt(std::ostream& cpp, const Stmt& stmt, bool isCoroutine);
    void genLetStmt(std::ostream& cpp, const LetDecl& decl);
    void genConstStmt(std::ostream& cpp, const ConstDecl& decl);
    void genReturnStmt(std::ostream& cpp, const ReturnStmt& stmt, bool isCoroutine);
    void genThrowStmt(std::ostream& cpp, const ThrowStmt& stmt);
    void genIfStmt(std::ostream& cpp, const IfStmt& stmt, bool isCoroutine);
    void genWhileStmt(std::ostream& cpp, const WhileStmt& stmt, bool isCoroutine);
    void genForStmt(std::ostream& cpp, const ForStmt& stmt, bool isCoroutine);
    void genLoopStmt(std::ostream& cpp, const LoopStmt& stmt, bool isCoroutine);
    void genBreakStmt(std::ostream& cpp);
    void genContinueStmt(std::ostream& cpp);
    void genTryCatchStmt(std::ostream& cpp, const TryCatchStmt& stmt, bool isCoroutine);
    void genSyncStmt(std::ostream& cpp, const SyncStmt& stmt, bool isCoroutine);

    // 原始 try/catch（非协程模式回退，被 genTryCatchStmt 复用）
    void genTryCatchRaw(std::ostream& cpp, const TryCatchStmt& stmt, bool isCoroutine);
    void genSpawnStmt(std::ostream& cpp, const SpawnStmt& stmt, bool isCoroutine);
    void genMatchStmt(std::ostream& cpp, const MatchStmt& stmt, bool isCoroutine);
    void genExprStmt(std::ostream& cpp, const ExprStmt& stmt, bool isCoroutine);

    // ============================================================
    // 表达式输出
    // ============================================================

    [[nodiscard]] std::string genExpr(const ASTNode& expr, bool isCoroutine);
    [[nodiscard]] std::string genIntLiteral(const IntLiteral& e);
    [[nodiscard]] std::string genFloatLiteral(const FloatLiteral& e);
    [[nodiscard]] std::string genStringLiteral(const StringLiteral& e);
    [[nodiscard]] std::string genBoolLiteral(const BoolLiteral& e);
    [[nodiscard]] std::string genNoneLiteral();
    [[nodiscard]] std::string genIdentifier(const Identifier& e);
    [[nodiscard]] std::string genListExpr(const ListExpr& e, bool isCoroutine);
    [[nodiscard]] std::string genRecordExpr(const RecordExpr& e, bool isCoroutine);
    [[nodiscard]] std::string genBinaryExpr(const BinaryExpr& e, bool isCoroutine);
    [[nodiscard]] std::string genUnaryExpr(const UnaryExpr& e, bool isCoroutine);
    [[nodiscard]] std::string genCallExpr(const CallExpr& e, bool isCoroutine);
    [[nodiscard]] std::string genMethodCall(const MethodCallExpr& e, bool isCoroutine);
    [[nodiscard]] std::string genMemberAccess(const MemberAccessExpr& e);
    [[nodiscard]] std::string genIndexExpr(const IndexExpr& e, bool isCoroutine);
    [[nodiscard]] std::string genAssignExpr(const AssignExpr& e, bool isCoroutine);
    [[nodiscard]] std::string genErrorPropagation(const ErrorPropagationExpr& e, bool isCoroutine);
    [[nodiscard]] std::string genPipeExpr(const PipeExpr& e, bool isCoroutine);

    // ============================================================
    // 协程判定辅助
    // ============================================================

    // 递归扫描 AST 节点中的调用，判断是否触发协程
    bool scanForCoroutine(const ASTNode& node);
    bool scanStmtForCoroutine(const Stmt& stmt);
    bool scanExprForCoroutine(const ASTNode& expr);

    // 判断表达式是否为「可能挂起」的调用
    // plan §4.8: 返回 task<T> 的运行时原语 → 协程
    [[nodiscard]] bool isSuspendingCall(const ASTNode& expr) const;

    // ============================================================
    // 输出辅助
    // ============================================================

    void newline(std::ostream& os);
    void indent(std::ostream& os);
    void dedent(std::ostream& os);
    void writeLine(std::ostream& os, const std::string& line);

    [[nodiscard]] std::string indentStr() const;

    // 将 C++ 关键字/保留字做转义（如变量名和关键字冲突时加后缀）
    [[nodiscard]] std::string safeName(const std::string& name) const;

    // 错误记录
    void error(const ASTNode& node, const std::string& msg);

    // ============================================================
    // 状态
    // ============================================================

    int indentLevel_ = 0;

    // 当前正在输出的两个流（generate 调用期间有效）
    std::ostream* headerStream_ = nullptr;
    std::ostream* implStream_   = nullptr;

    // 已注册的类型名 → 是否为堆对象
    std::unordered_map<std::string, bool> registeredTypes_;

    // 当前作用域内值类型变量名（用于决定 . vs -> 访问）
    std::set<std::string> valueTypeVarNames_;

    // 已声明的接口名（用于方法签名映射）
    std::set<std::string> interfaceNames_;

    // 当前编译单元中已知的需要协程的函数名
    std::set<std::string> coroutineFunctions_;

    // 待嵌入 struct 的方法声明（genRecordStruct 消费）
    std::vector<PendingMethod> pendingMethods_;

    // 当前正在生成的方法/构造函数的接收者名（如 "self", "p"）
    // 用于在 genIdentifier 中将 self/p 映射为 C++ 的 this
    std::string currentReceiverName_;

    // 当前是否在 spawn 块内生成代码（避免嵌套协程 co_await）
    bool insideSpawn_ = false;

    // 列表表达式计数器 — 生成唯一的临时变量名
    int listCounter_ = 0;

    // 当前正在生成的函数的协程状态
    bool currentFunctionIsCoroutine_ = false;

    // 当前函数的模板参数列表（用于调用泛型构造函数时传递类型参数）
    std::vector<std::string> currentTParams_;

    // 字符串类型变量名集合（用于 genBinaryExpr 检测 string + T 拼接）
    std::set<std::string> stringVarNames_;

    // 导入的命名空间名集合（路径名 + 别名，用于 genMethodCall 判断是否用 ::）
    std::set<std::string> importNsNames_;

    // let/const 声明中类型标注的显式模板参数（如 math.Pair<float, bool> → {"float", "bool"}）
    // genLetStmt 设置，genMethodCall 的 ns-ctor 路径消费后清空
    std::vector<std::string> expectedTemplateArgs_;

    // 错误列表
    std::vector<std::string> errors_;
};

} // namespace Aura
