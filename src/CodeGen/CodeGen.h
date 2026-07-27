#pragma once

#include "../AST/ASTNode.h"
#include "../AST/Expr.h"
#include "../AST/Stmt.h"
#include "../AST/Type.h"
#include "../ASTWalker.h"
#include "../Diag/DiagnosticEngine.h"
#include <map>
#include <ostream>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace Aura {

class SemAnalyzer;

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
// CodeGenConfig — 编译器配置（来自 SemAnalyzer 的 #config 指令）
// ============================================================
struct CodeGenConfig {
    bool ioSync = false;  // #io.sync = true → 同步模式

    // 从 SemAnalyzer 中提取所有 #config 配置项（对外唯一入口）
    void setConfig(const SemAnalyzer& sema);
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
    CodeGenerator(DiagnosticEngine& diag);

    // -- 主入口 --
    // 生成一个编译单元（.aura → .cpp/.h）
    [[nodiscard]] CompileUnit generate(const Program& program,
                                        const std::string& moduleName = "main",
                                        const std::vector<CodeGenImport>& imports = {},
                                        const std::string& nsName = "",
                                        const CodeGenConfig& config = {});

    // -- 协程判定入口 --
    [[nodiscard]] CoroDecision decideCoro(const FunDecl& decl);
    [[nodiscard]] CoroDecision decideCoro(const MethodDecl& decl);

    // -- 错误 --
    const std::vector<std::string>& errors() const { return diag_.errorMessages(); }

public:
    // ============================================================
    // AST 遍历 Visitor（基于 ASTWalker 模板）
    // 供 genSpawnStmt / genFunExpr 等复用
    // ============================================================

    // IdRefCollector — 收集 Stmt/Expr 子树中所有 Identifier 引用名
    class IdRefCollector {
    public:
        explicit IdRefCollector(std::set<std::string>& out) : out_(out) {}
        bool collectStmt(const Stmt& stmt)  { return StmtWalker<IdRefCollector>::walk(stmt, *this); }
        bool collectExpr(const ASTNode& e)  { return ExprWalker<IdRefCollector>::walk(e, *this); }
        // --- Stmt visit ---
        bool visit(const BlockStmt& n, IdRefCollector& self) { for (auto& s : n.stmts) if (s) self.collectStmt(*s); return false; }
        bool visit(const ReturnStmt& n, IdRefCollector& self) { if (n.expr) self.collectExpr(*n.expr); return false; }
        bool visit(const ThrowStmt& n, IdRefCollector& self)  { if (n.expr) self.collectExpr(*n.expr); return false; }
        bool visit(const IfStmt& n, IdRefCollector& self) { if (n.condition) self.collectExpr(*n.condition); if (n.thenBranch) self.collectStmt(*n.thenBranch); for (auto& ei : n.elseIfs) { if (ei.condition) self.collectExpr(*ei.condition); if (ei.body) self.collectStmt(*ei.body); } if (n.elseBranch) self.collectStmt(*n.elseBranch); return false; }
        bool visit(const WhileStmt& n, IdRefCollector& self) { if (n.condition) self.collectExpr(*n.condition); if (n.body) self.collectStmt(*n.body); return false; }
        bool visit(const ForStmt& n, IdRefCollector& self)   { if (n.iterable) self.collectExpr(*n.iterable); if (n.body) self.collectStmt(*n.body); return false; }
        bool visit(const LoopStmt& n, IdRefCollector& self)   { if (n.body) self.collectStmt(*n.body); return false; }
        bool visit(const TryCatchStmt& n, IdRefCollector& self) { if (n.tryBody) self.collectStmt(*n.tryBody); if (n.catchBody) self.collectStmt(*n.catchBody); return false; }
        bool visit(const SyncStmt& n, IdRefCollector& self)   { if (n.body) self.collectStmt(*n.body); return false; }
        bool visit(const SyncForStmt& n, IdRefCollector& self) { if (n.iterable) self.collectExpr(*n.iterable); if (n.body) self.collectStmt(*n.body); return false; }
        bool visit(const SpawnStmt& n, IdRefCollector& self)  { for (auto& sb : n.body) if (sb) self.collectStmt(*sb); return false; }
        bool visit(const MatchStmt& n, IdRefCollector& self) { if (n.expr) self.collectExpr(*n.expr); for (auto& c : n.cases) { if (c.body) { if (auto* cb = dynamic_cast<const BlockStmt*>(c.body.get())) self.collectStmt(*cb); else self.collectExpr(*c.body); } } return false; }
        bool visit(const ExprStmt& n, IdRefCollector& self)   { if (n.expr) self.collectExpr(*n.expr); return false; }
        bool visit(const LetDecl& n, IdRefCollector& self)    { if (n.initializer) self.collectExpr(*n.initializer); return false; }
        bool visit(const ConstDecl& n, IdRefCollector& self)  { if (n.initializer) self.collectExpr(*n.initializer); return false; }
        bool visit(const BreakStmt&, IdRefCollector&)    { return false; }
        bool visit(const ContinueStmt&, IdRefCollector&) { return false; }
        // --- Expr visit ---
        bool visit(const Identifier& n, IdRefCollector&) { out_.insert(n.name); return false; }
        bool visit(const BinaryExpr& n, IdRefCollector& self) { self.collectExpr(*n.left); self.collectExpr(*n.right); return false; }
        bool visit(const UnaryExpr& n, IdRefCollector& self) { self.collectExpr(*n.operand); return false; }
        bool visit(const CallExpr& n, IdRefCollector& self) { self.collectExpr(*n.callee); for (auto& a : n.args) self.collectExpr(*a); return false; }
        bool visit(const MethodCallExpr& n, IdRefCollector& self) { self.collectExpr(*n.object); for (auto& a : n.args) self.collectExpr(*a); return false; }
        bool visit(const MemberAccessExpr& n, IdRefCollector& self) { self.collectExpr(*n.object); return false; }
        bool visit(const IndexExpr& n, IdRefCollector& self) { self.collectExpr(*n.object); self.collectExpr(*n.index); return false; }
        bool visit(const AssignExpr& n, IdRefCollector& self) { self.collectExpr(*n.target); self.collectExpr(*n.value); return false; }
        bool visit(const ErrorPropagationExpr& n, IdRefCollector& self) { self.collectExpr(*n.expr); return false; }
        bool visit(const PipeExpr& n, IdRefCollector& self) { self.collectExpr(*n.left); self.collectExpr(*n.right); return false; }
        bool visit(const RecordExpr& n, IdRefCollector& self) { for (auto& f : n.fields) if (f.value) self.collectExpr(*f.value); return false; }
        bool visit(const ListExpr& n, IdRefCollector& self) { for (auto& e : n.elements) if (e) self.collectExpr(*e); return false; }
        bool visit(const IntLiteral&, IdRefCollector&)    { return false; }
        bool visit(const FloatLiteral&, IdRefCollector&)   { return false; }
        bool visit(const StringLiteral&, IdRefCollector&)  { return false; }
        bool visit(const BoolLiteral&, IdRefCollector&)    { return false; }
        bool visit(const NoneLiteral&, IdRefCollector&)    { return false; }
        bool visit(const FunExpr& n, IdRefCollector& self) { if (n.body) for (auto& s : n.body->stmts) if (s) self.collectStmt(*s); return false; }
    private:
        std::set<std::string>& out_;
    };

    // DeclaredCollector — 收集 Stmt 子树中的局部变量声明
    class DeclaredCollector {
    public:
        explicit DeclaredCollector(std::set<std::string>& out) : out_(out) {}
        bool collectStmt(const Stmt& stmt) { return StmtWalker<DeclaredCollector>::walk(stmt, *this); }
        bool visit(const LetDecl& n, DeclaredCollector&)     { out_.insert(n.name); return false; }
        bool visit(const ConstDecl& n, DeclaredCollector&)   { out_.insert(n.name); return false; }
        bool visit(const ForStmt& n, DeclaredCollector& self){ out_.insert(n.itemName); if (n.body) for (auto& s : n.body->stmts) if (s) self.collectStmt(*s); return false; }
        bool visit(const TryCatchStmt& n, DeclaredCollector&){ out_.insert(n.catchVar); return false; }
        bool visit(const MatchStmt& n, DeclaredCollector&)   { for (auto& c : n.cases) if (auto* tp = dynamic_cast<const TypePattern*>(c.pattern.get())) if (!tp->varName.empty()) out_.insert(tp->varName); return false; }
        bool visit(const BlockStmt& n, DeclaredCollector& self){ for (auto& s : n.stmts) if (s) self.collectStmt(*s); return false; }
        bool visit(const SyncStmt& n, DeclaredCollector& self) { if (n.body) for (auto& sb : n.body->stmts) if (sb) self.collectStmt(*sb); return false; }
        bool visit(const SyncForStmt& n, DeclaredCollector& self){ out_.insert(n.itemName); if (n.body) for (auto& sb : n.body->stmts) if (sb) self.collectStmt(*sb); return false; }
        bool visit(const SpawnStmt& n, DeclaredCollector& self){ for (auto& sb : n.body) if (sb) self.collectStmt(*sb); return false; }
        bool visit(const ReturnStmt&,  DeclaredCollector&) { return false; }
        bool visit(const ThrowStmt&,   DeclaredCollector&) { return false; }
        bool visit(const IfStmt&,      DeclaredCollector&) { return false; }
        bool visit(const WhileStmt&,   DeclaredCollector&) { return false; }
        bool visit(const LoopStmt&,    DeclaredCollector&) { return false; }
        bool visit(const ExprStmt&,    DeclaredCollector&) { return false; }
        bool visit(const BreakStmt&,   DeclaredCollector&) { return false; }
        bool visit(const ContinueStmt&,DeclaredCollector&) { return false; }
        bool visit(const FunExpr&,     DeclaredCollector&) { return false; }
    private:
        std::set<std::string>& out_;
    };

    // CoroScanner — 协程挂起点扫描器（实现见 CoroDecide.cpp）
    class CoroScanner;

private:

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
    // 参数类型映射 — 接口类型自动加 const&
    [[nodiscard]] std::string mapParamType(const TypeExpr& type);
    // SemType → C++ 类型（从 ASTNode::inferredType 读取，替代文本启发式）
    [[nodiscard]] std::string mapSemType(const SemType& semType);

    // 值类型映射（不加 *）
    [[nodiscard]] std::string mapValueType(const TypeExpr& type);

    // 判断类型是否为值类型（int/float/bool/None）
    [[nodiscard]] bool isValueType(const std::string& auraName) const;

    // 检查类型是否是注册的堆对象类型（记录/接口/泛型记录）
    [[nodiscard]] bool isHeapType(const std::string& auraName) const;

    // 判断 SemType 是否对应 GC 堆对象指针（用于 GcRootHandle 包装决策）
    [[nodiscard]] bool isHeapSemType(const SemType* type) const;

    // 判断 C++ 类型字符串是否为 GC 指针类型（如 GcString*, User*, Array<T>*）
    [[nodiscard]] bool isGcPointerType(const std::string& cppType) const;

    // 注册一个用户定义的类型名
    void registerTypeName(const std::string& auraName, bool isHeap);

    // ============================================================
    // 声明输出
    // ============================================================

    void genDecl(std::ostream& h, std::ostream& cpp,
                 const Decl& decl, CompileUnit& unit,
                 bool declarationsOnly = false);

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
                    const FunDecl& decl, bool declarationsOnly = false);
    void genMethodDecl(std::ostream& h, std::ostream& cpp,
                       const MethodDecl& decl, bool declarationsOnly = false);
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
    void genSyncThreadStmt(std::ostream& cpp, const SyncStmt& stmt);  // sync thread 多线程
    void genSyncForStmt(std::ostream& cpp, const SyncForStmt& stmt, bool isCoroutine);

    // 原始 try/catch（非协程模式回退，被 genTryCatchStmt 复用）
    void genTryCatchRaw(std::ostream& cpp, const TryCatchStmt& stmt, bool isCoroutine);
    // v1.2：协程模式下无 setupLet 的 try/catch 用 IIFE + variant<monostate, Error>
    void genTryCatchNoSetupIIFE(std::ostream& cpp, const TryCatchStmt& stmt, bool isCoroutine);
    void genSpawnStmt(std::ostream& cpp, const SpawnStmt& stmt, bool isCoroutine);
    void genSpawnAsThread(std::ostream& cpp, const SpawnStmt& stmt);  // sync thread 内的 spawn
    void genLockStmt(std::ostream& cpp, const LockStmt& stmt, bool isCoroutine);  // lock (m) { }
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

    // 为 GC 堆类型参数生成 IIFE + GcRootHandle 包装
    // args: (expr_string, inferredType) 对；callExpr: 包装后的调用表达式
    [[nodiscard]] std::string genGcRootedArgs(
        const std::vector<std::pair<std::string, const SemType*>>& args,
        const std::string& callExpr, bool isCoroutine);
    [[nodiscard]] std::string genMemberAccess(const MemberAccessExpr& e);
    [[nodiscard]] std::string genIndexExpr(const IndexExpr& e, bool isCoroutine);
    [[nodiscard]] std::string genAssignExpr(const AssignExpr& e, bool isCoroutine);
    [[nodiscard]] std::string genErrorPropagation(const ErrorPropagationExpr& e, bool isCoroutine);
    [[nodiscard]] std::string genPipeExpr(const PipeExpr& e, bool isCoroutine);

    // --- 闭包 ---
    [[nodiscard]] std::string genFunExpr(const FunExpr& e, bool isCoroutine);

    // 收集 BinaryExpr(+, left, right) 的所有 string 操作数，链长 ≥ 3 时用于 concat_multi
    [[nodiscard]] std::vector<std::string> collectStringChain(const BinaryExpr& e,
                                                              bool isCoroutine);

    // 判定生成的 C++ 表达式是否为 GcString* 类型（用于 concat_multi 的 GcString::from 转换）
    [[nodiscard]] bool isStringExprInChain(const std::string& s) const;

    // 写屏障辅助：检测赋值目标是否为 GC 对象字段（obj.get()->field 或 this->field）
    [[nodiscard]] bool isGcFieldAssignment(const std::string& target) const;

    // 写屏障辅助：将 "obj.get()->field" 分解为 (parentObj="obj.get()", fieldAddr="&(obj.get()->field)")
    [[nodiscard]] std::pair<std::string, std::string>
    decomposeFieldAccess(const std::string& target) const;

    // ============================================================
    // 协程判定辅助
    // ============================================================

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
    std::set<std::string> coroClosureNames_;  // let 绑定的协程闭包名
    bool lastClosureIsCoro_ = false;           // genFunExpr → genLetStmt 传递

    // 待嵌入 struct 的方法声明（genRecordStruct 消费）
    std::vector<PendingMethod> pendingMethods_;

    // 类型别名的模板参数表（type Name<T,...> = ... 或内部含泛型引用的类型别名）
    std::unordered_map<std::string, std::vector<std::string>> typeAliasTemplateParams_;

    // 当前正在生成的方法/构造函数的接收者名（如 "self", "p"）
    // 用于在 genIdentifier 中将 self/p 映射为 C++ 的 this
    std::string currentReceiverName_;

    // 当前是否在 spawn 块内生成代码（避免嵌套协程 co_await）
    bool insideSpawn_ = false;

    // 当前是否在 sync thread 块内（控制 spawn 生成分派到 genSpawnAsThread）
    bool inSyncThreadBlock_ = false;

    // 列表表达式计数器 — 生成唯一的临时变量名
    int listCounter_ = 0;
    int recordAllocCounter_ = 0;
    int argHandleCounter_ = 0;  // concat_multi 参数 GcRootHandle 变量名计数器

    // 当前正在生成的函数的协程状态
    bool currentFunctionIsCoroutine_ = false;

    // 当前函数的模板参数列表（用于调用泛型构造函数时传递类型参数）
    std::vector<std::string> currentTParams_;
    std::string              currentLetName_;    // 当前 let 声明的变量名
    bool                     ioSync_ = false;    // 来自 CodeGenConfig

    // 当前函数的 C++ 返回类型（用于 genReturnStmt 生成正确的 RecordExpr 构造）
    std::string currentReturnCppType_;

    // 字符串类型变量名集合（用于 genBinaryExpr 检测 string + T 拼接）
    std::set<std::string> stringVarNames_;

    // 当前函数内已注册为 GcRootHandle 的变量名集合
    // genIdentifier 遇到这些变量名时生成 .get()
    std::set<std::string> gcRootVarNames_;

    // GC 根变量名 → C++ 类型映射（如 "greeting" → "aura_rt::GcString*"）
    // genFunExpr 的 init-capture 需要类型信息生成 GcSharedRoot<T>
    std::unordered_map<std::string, std::string> gcRootTypes_;

    // 导入的命名空间名集合（路径名 + 别名，用于 genMethodCall 判断是否用 ::）
    std::set<std::string> importNsNames_;

    // channel 类型变量名集合（用于 genMethodCall 的 co_await 判定和 genForStmt 展开）
    std::set<std::string> channelVarNames_;

    // record struct 的字段名集合（key = struct name），用于检测方法名与字段名冲突
    std::unordered_map<std::string, std::set<std::string>> structFieldNames_;

    // 函数名 → 其接口类型参数的位置（用于 genCallExpr 中自动包装闭包为 InterfaceFunc）
    // 第一层 map: 函数名 → pair(参数索引, 接口类型名)
    std::map<std::string, std::vector<std::pair<size_t, std::string>>> fnInterfaceParams_;

    // 函数名 → 其 FunctionType 参数的位置（用于 genCallExpr 中包装裸 lambda 为 std::function）
    std::map<std::string, std::vector<std::pair<size_t, std::string>>> fnCallbackParams_;

    // let/const 声明中类型标注的显式模板参数（如 math.Pair<float, bool> → {"float", "bool"}）
    // genLetStmt 设置，genMethodCall 的 ns-ctor 路径消费后清空
    std::vector<std::string> expectedTemplateArgs_;

    // 错误列表
    DiagnosticEngine& diag_;
};

} // namespace Aura
