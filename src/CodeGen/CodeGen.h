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

// P4/P3b：SemType 层级前向声明（genUnionDispatch / genMatchStmt 用）
struct SemType;
struct UnionSemType;
struct OptionalSemType;

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
    // 跨模块函数默认参数：模块命名空间名 → (函数名 → 默认值表达式数组)（C5.4）
    // AST 指针来自依赖模块 ModuleInfo.exports（常驻内存，多文件 CodeGen 并行只读）
    using CrossModuleDefaults = std::map<std::string,
        std::map<std::string, std::vector<const ASTNode*>>>;

    CodeGenerator(DiagnosticEngine& diag);

    // -- 主入口 --
    // 生成一个编译单元（.aura → .cpp/.h）
    // crossDefaults: 跨模块函数默认参数表（C5.4，多文件模式由 main.cpp 构造）
    [[nodiscard]] CompileUnit generate(const Program& program,
                                        const std::string& moduleName = "main",
                                        const std::vector<CodeGenImport>& imports = {},
                                        const std::string& nsName = "",
                                        const CodeGenConfig& config = {},
                                        const CrossModuleDefaults& crossDefaults = {});

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
        bool visit(const SpawnStmt& n, IdRefCollector& self)  { if (n.callExpr) return self.collectExpr(*n.callExpr); for (auto& sb : n.body) if (sb) self.collectStmt(*sb); return false; }
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
        bool visit(const ConditionalExpr& n, IdRefCollector& self) { if (n.cond) self.collectExpr(*n.cond); if (n.thenBranch) self.collectExpr(*n.thenBranch); if (n.elseBranch) self.collectExpr(*n.elseBranch); return false; }
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

    // DeclaredCollector — 收集 Stmt/Expr 子树中的局部变量声明
    // 与 IdRefCollector 对称：也遍历嵌套闭包（FunExpr）——收集内层闭包参数名与体内
    // 声明，使外层闭包捕获分析排除内层闭包局部名（修复「外层闭包捕获分析误捕获内层
    // 闭包参数名」缺陷：此前 DeclaredCollector 不深入 FunExpr，内层闭包参数/体内声明
    // 被 IdRefCollector 收集、却不被 declared 排除 → 误入外层闭包捕获列表，见
    // problem.txt 对应条目）。
    class DeclaredCollector {
    public:
        explicit DeclaredCollector(std::set<std::string>& out) : out_(out) {}
        bool collectStmt(const Stmt& stmt) { return StmtWalker<DeclaredCollector>::walk(stmt, *this); }
        bool collectExpr(const ASTNode& expr) { return ExprWalker<DeclaredCollector>::walk(expr, *this); }
        // --- Stmt visit ---
        bool visit(const LetDecl& n, DeclaredCollector& self)     { out_.insert(n.name); if (n.initializer) self.collectExpr(*n.initializer); return false; }
        bool visit(const ConstDecl& n, DeclaredCollector& self)   { out_.insert(n.name); if (n.initializer) self.collectExpr(*n.initializer); return false; }
        bool visit(const ForStmt& n, DeclaredCollector& self)     { out_.insert(n.itemName); if (n.iterable) self.collectExpr(*n.iterable); if (n.body) for (auto& s : n.body->stmts) if (s) self.collectStmt(*s); return false; }
        bool visit(const SyncForStmt& n, DeclaredCollector& self) { out_.insert(n.itemName); if (n.iterable) self.collectExpr(*n.iterable); if (n.body) for (auto& sb : n.body->stmts) if (sb) self.collectStmt(*sb); return false; }
        bool visit(const TryCatchStmt& n, DeclaredCollector& self){ out_.insert(n.catchVar); if (n.tryBody) for (auto& s : n.tryBody->stmts) if (s) self.collectStmt(*s); if (n.catchBody) for (auto& s : n.catchBody->stmts) if (s) self.collectStmt(*s); return false; }
        bool visit(const MatchStmt& n, DeclaredCollector& self)   { for (auto& c : n.cases) if (auto* tp = dynamic_cast<const TypePattern*>(c.pattern.get())) if (!tp->varName.empty()) out_.insert(tp->varName); if (n.expr) self.collectExpr(*n.expr); for (auto& c : n.cases) { if (c.body) { if (auto* cb = dynamic_cast<const BlockStmt*>(c.body.get())) self.collectStmt(*cb); else self.collectExpr(*c.body); } } return false; }
        bool visit(const BlockStmt& n, DeclaredCollector& self)   { for (auto& s : n.stmts) if (s) self.collectStmt(*s); return false; }
        bool visit(const SyncStmt& n, DeclaredCollector& self)    { if (n.body) for (auto& sb : n.body->stmts) if (sb) self.collectStmt(*sb); return false; }
        bool visit(const SpawnStmt& n, DeclaredCollector& self)   { if (n.callExpr) return self.collectExpr(*n.callExpr); for (auto& sb : n.body) if (sb) self.collectStmt(*sb); return false; }
        bool visit(const ReturnStmt& n, DeclaredCollector& self)  { if (n.expr) self.collectExpr(*n.expr); return false; }
        bool visit(const ThrowStmt& n, DeclaredCollector& self)   { if (n.expr) self.collectExpr(*n.expr); return false; }
        bool visit(const ExprStmt& n, DeclaredCollector& self)    { if (n.expr) self.collectExpr(*n.expr); return false; }
        bool visit(const IfStmt& n, DeclaredCollector& self) {
            if (n.condition) self.collectExpr(*n.condition);
            if (n.thenBranch) self.collectStmt(*n.thenBranch);
            for (auto& ei : n.elseIfs) { if (ei.condition) self.collectExpr(*ei.condition); if (ei.body) self.collectStmt(*ei.body); }
            if (n.elseBranch) self.collectStmt(*n.elseBranch);
            return false;
        }
        bool visit(const WhileStmt& n, DeclaredCollector& self) { if (n.condition) self.collectExpr(*n.condition); if (n.body) self.collectStmt(*n.body); return false; }
        bool visit(const LoopStmt& n, DeclaredCollector& self)   { if (n.body) self.collectStmt(*n.body); return false; }
        bool visit(const BreakStmt&,    DeclaredCollector&) { return false; }
        bool visit(const ContinueStmt&, DeclaredCollector&) { return false; }
        // --- Expr visit ---
        bool visit(const FunExpr& n, DeclaredCollector& self) {
            for (auto& p : n.params) out_.insert(p.name);          // 内层闭包参数名
            if (n.body) for (auto& s : n.body->stmts) if (s) self.collectStmt(*s);  // 体内声明
            return false;
        }
        bool visit(const BinaryExpr& n, DeclaredCollector& self) { if (n.left) self.collectExpr(*n.left); if (n.right) self.collectExpr(*n.right); return false; }
        bool visit(const UnaryExpr& n, DeclaredCollector& self) { if (n.operand) self.collectExpr(*n.operand); return false; }
        bool visit(const CallExpr& n, DeclaredCollector& self) { if (n.callee) self.collectExpr(*n.callee); for (auto& a : n.args) if (a) self.collectExpr(*a); return false; }
        bool visit(const MethodCallExpr& n, DeclaredCollector& self) { if (n.object) self.collectExpr(*n.object); for (auto& a : n.args) if (a) self.collectExpr(*a); return false; }
        bool visit(const MemberAccessExpr& n, DeclaredCollector& self) { if (n.object) self.collectExpr(*n.object); return false; }
        bool visit(const IndexExpr& n, DeclaredCollector& self) { if (n.object) self.collectExpr(*n.object); if (n.index) self.collectExpr(*n.index); return false; }
        bool visit(const AssignExpr& n, DeclaredCollector& self) { if (n.target) self.collectExpr(*n.target); if (n.value) self.collectExpr(*n.value); return false; }
        bool visit(const ErrorPropagationExpr& n, DeclaredCollector& self) { if (n.expr) self.collectExpr(*n.expr); return false; }
        bool visit(const PipeExpr& n, DeclaredCollector& self) { if (n.left) self.collectExpr(*n.left); if (n.right) self.collectExpr(*n.right); return false; }
        bool visit(const ConditionalExpr& n, DeclaredCollector& self) { if (n.cond) self.collectExpr(*n.cond); if (n.thenBranch) self.collectExpr(*n.thenBranch); if (n.elseBranch) self.collectExpr(*n.elseBranch); return false; }
        bool visit(const ListExpr& n, DeclaredCollector& self) { for (auto& e : n.elements) if (e) self.collectExpr(*e); return false; }
        bool visit(const RecordExpr& n, DeclaredCollector& self) { for (auto& f : n.fields) if (f.value) self.collectExpr(*f.value); return false; }
        bool visit(const Identifier&,     DeclaredCollector&) { return false; }
        bool visit(const IntLiteral&,     DeclaredCollector&) { return false; }
        bool visit(const FloatLiteral&,   DeclaredCollector&) { return false; }
        bool visit(const StringLiteral&,  DeclaredCollector&) { return false; }
        bool visit(const BoolLiteral&,    DeclaredCollector&) { return false; }
        bool visit(const NoneLiteral&,    DeclaredCollector&) { return false; }
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
    // M3：调用点补默认参数闭包物化——从「函数形参类型表达式 + 调用点实参 SemType」
    // 推导函数模板泛型绑定（泛型名 → 具体 C++ 类型），供 genFunExpr 生成普通 lambda。
    // 如 useT(inc:<T>, v:T) 调用 useT(5,10) → T → "int32_t"。currentTParams_ 中的外层
    // 模板参数名不物化（由外层提供，闭包直接引用即可）。
    void collectDefaultArgGenericMap(
        const std::string& calleeName,
        const std::vector<std::unique_ptr<ASTNode>>& args,
        std::map<std::string, std::string>& out);
    void collectMaterializedFromType(
        const TypeExpr& formal, const SemType& arg,
        std::map<std::string, std::string>& out);
    // 从函数声明中收集模板参数（params + returnType + receiverTypeArgs）
    [[nodiscard]] std::vector<std::string> collectFunTParams(const FunDecl& decl) const;
    [[nodiscard]] std::vector<std::string> collectMethodTParams(const MethodDecl& decl) const;
    // 返回类型是否为泛型函数类型别名（NamedType、函数式 non-heap 模板别名，
    // 如 Mapper<A,U> = fun([T], fun(T)->U) -> [U]）。plan12 对称：此类返回的闭包
    // 自身泛型（U）由闭包声明/调用点推断（invoke_result_t），函数/方法不收集其
    // 模板参数、返回类型写 auto（否则 'U' was not declared + receiver 多拼）。
    [[nodiscard]] bool isFuncAliasRet(const TypeExpr* retType) const;

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

    // 从返回类型 TypeExpr 提取 Optional<T> 的 T（C++ 名）；非 Optional 返回空
    [[nodiscard]] std::string optionalElemOf(const TypeExpr* retType);

    // 从 Optional 语义类型提取元素 C++ 名（P1-1/A1/A2）：
    //   OptionalSemType{T} → 元素（非 Error）C++ 名；GenericSemType{name=="Optional"}
    //   → 从 resolvedName 提取 <...> 内元素（显式 `Optional<T>` 注解物化形态，
    //   mapSemType 对其会加 * 尾缀不适用）。元素未知 / 非 Optional → 返回空。
    [[nodiscard]] std::string optionalElemCppName(const SemType* optType);

    // #2：Optional 元素 C++ 名递归补齐堆 record 的 '*'（声明侧/初始化器/消费侧共用）。
    // 从 resolvedName 提取的元素 C++ 名可能缺失内嵌堆 record 的 '*'（Sema 的
    // cppNameOfTypeExpr 对 record 裸名不加 '*'，对 ListType/Iterator 内嵌 record 同样
    // 不加）→ 递归补全：容器（Array<X>* / Iterator<X> / Optional<X>*）处理内嵌元素，
    // 叶子（值类型 / 接口值视图 / 已带 '*' 指针）保持。与 mapSemType 的容器语义一致
    // （Optional<[Point]> / Optional<Iterator<Point>> 内嵌 record 也要补 '*'）。
    [[nodiscard]] std::string finalizeCppElem(const std::string& elem);

    // 值类型映射（不加 *）
    [[nodiscard]] std::string mapValueType(const TypeExpr& type);

    // 判断类型是否为值类型（int/float/bool/None）
    [[nodiscard]] bool isValueType(const std::string& auraName) const;

    // 检查类型是否是注册的堆对象类型（记录/接口/泛型记录）
    [[nodiscard]] bool isHeapType(const std::string& auraName) const;

    // 判断 SemType 是否对应 GC 堆对象指针（用于 GcRootHandle 包装决策）
    [[nodiscard]] bool isHeapSemType(const SemType* type) const;

    // P1：视图类型判定（值视图 { 函数指针, self }，非 GC 堆对象）
    //   - 内置 Iterator<T>（GenericSemType "Iterator"）
    //   - 接口视图（InterfaceSemType：Stringer/Comparable/用户接口）
    // 视图不能被 GcRootHandle<View> 包裹（视图非指针，模板参数不成立）
    [[nodiscard]] bool isIfaceView(const SemType* t) const;
    // 联合变体堆封装判定：堆类型 或 视图类型
    // （视图含 self GC 指针，放 std::variant 内部 GC 不可见 → 必须 aura_rt::Variant<T...>* 封装，
    //   descForI 按 self 子偏移扫描；与 isHeapSemType 的"传参包装"语义不同，勿混用）
    [[nodiscard]] bool isUnionHeapVariant(const SemType* t) const;

    // P3b：识别 none() 调用（Optional 占位构造），联合赋值时特判为 NoneType 值
    [[nodiscard]] static bool isNoneCallExpr(const ASTNode& e);

    // 判断 C++ 类型字符串是否为 GC 指针类型（如 GcString*, User*, Array<T>*）
    [[nodiscard]] bool isGcPointerType(const std::string& cppType) const;

    // 判定 C++ 类型名是否为接口视图类型（aura_rt::Iterator<T> / 用户接口名[<...>] / 内置接口名[<...>]）
    [[nodiscard]] bool isIfaceViewTypeName(const std::string& cppType) const;

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
    // 生成接口视图结构体引用的用户 record C++ 前向声明（接口视图结构体在 record
    // struct 完整定义（第三遍 B）之前生成，引用后置 record 时需先前向声明）
    void emitIfaceRecordForwardDecls(std::ostream& h, const InterfaceDecl& decl);
    // 生成"类型 × 接口"适配器（方案 B：record 保持不动，适配器持值持有根）
    void genIfaceAdapter(std::ostream& h, const std::string& recordName,
                         const InterfaceDecl& iface);

    // --- 函数/方法声明 + 实现 ---
    // 清理函数级变量跟踪状态（6 处调用点统一；新增跟踪集合时必须同步此处）
    void clearVarTrackingState();
    // 参数类型跟踪注册（3 处共有：string / 用户接口 / 值类型 NamedType；不含 _raw 语义）
    void registerParamTracking(const Param& p);
    // decl 函数参数特有（签名已生成 varName_raw，函数体入口 GcRootHandle/ViewRoot 包裹）：
    // 接口视图参数 → viewRootVarNames_ + viewRootTypes_[decltype(_raw)]；
    // GC 指针参数 → gcRootVarNames_ + gcRootTypes_[decltype(_raw)]
    void registerRawParamTracking(const Param& p);
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
    void genSpawnCallAsCoro(std::ostream& cpp, const SpawnStmt& stmt);   // 调用形态（协程版）：spawn func(args)
    void genSpawnCallAsThread(std::ostream& cpp, const SpawnStmt& stmt); // 调用形态（线程版）：spawn func(args)
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
    // P4：联合接收者动态分派——生成运行时类型判定（单变体检查直调 / 多变体 switch），
    // 激活变体不支持该调用时抛 TypeError（make_type_error）
    [[nodiscard]] std::string genUnionDispatch(const MethodCallExpr& e,
                                               const UnionSemType& u,
                                               bool isCoroutine);
    // P4：联合索引分派（v[i] 在变体集合上按 index_ 分派）
    [[nodiscard]] std::string genUnionIndexDispatch(const IndexExpr& e,
                                                    const UnionSemType& u,
                                                    bool isCoroutine);
    // P3b：隐式装箱——目标为含堆联合（Variant 指针）、值为非联合值（int/string/record/list...）
    // 时，按值 SemType 定位变体索引 I，生成 make_variant<T1,...>(I, &tmp)。
    // 非含堆联合（std::variant 路径）或不匹配时返回空串（调用方保持原逻辑）。
    [[nodiscard]] std::string genUnionBoxing(const UnionSemType& u,
                                             const ASTNode& init,
                                             bool isCoroutine);
    // 装箱核心：按变体 C++ 类型字符串列表定位索引并生成 make_variant（let/const/assign/return 共用）
    [[nodiscard]] std::string genUnionBoxingImpl(const std::vector<std::string>& cppTypes,
                                                 const ASTNode& init,
                                                 bool isCoroutine);
    // record → 接口视图适配器 view() 预转换 IIFE（提取自 genLetStmt §3.10，供其与 genUnionBoxingImpl 共用）
    // expr: 已生成的 record 指针表达式字符串；recName: record canonicalName；viewCppType: 视图 C++ 类型
    [[nodiscard]] std::string genRecordToViewIIFE(const std::string& expr,
                                                  const std::string& recName,
                                                  const std::string& viewCppType);

    // G1：Optional/Union 形参实参装箱（genCallExpr / genMethodCall 共用）。paramCpp 为
    // 形参 C++ 类型名（mapType 产物）：aura_rt::Optional<X>* → genOptionalTargetInit；
    // aura_rt::Variant<...>* → genUnionBoxingImpl；其余返回空串（不装箱）。
    [[nodiscard]] std::string genParamBoxing(const std::string& paramCpp,
                                             const ASTNode& arg,
                                             bool isCoroutine);

    // #2：Optional 目标装箱 IIFE——make_optional<elemCpp>(值) 并 GcRootHandle 保护堆值。
    // OptionalSemType（折叠 union）/ GenericSemType{name=="Optional"}（显式注解）共用；
    // elemCpp 为空返回空串（调用方回退原逻辑）。值表达式自身已是 Optional 时不适用（调用方判断）。
    [[nodiscard]] std::string genOptionalBoxIIFE(const std::string& elemCpp,
                                                 const ASTNode& initExpr,
                                                 bool isCoroutine);

    // #1：显式 Optional<X> 目标（GenericSemType{name=="Optional"} / NamedType Optional）
    // 初始化器装箱：some(arg) / 裸值直赋 / record 字面量 → make_optional<X>(...)，
    // 元素为接口视图时 record→view 预转换、元素为 std::function 时显式模板参数；
    // initializer 已是 Optional 值（防二次装箱）时返回其裸表达式。elemCpp 为空返回空串。
    [[nodiscard]] std::string genOptionalTargetInit(const ASTNode& init,
                                                    const std::string& elemCpp,
                                                    bool isCoroutine);
    // #1：按元素 C++ 类型与值形态生成 make_optional<elemCpp>(值)（record→view / 显式模板参数）
    [[nodiscard]] std::string genOptionalBoxByElem(const std::string& elemCpp,
                                                   const ASTNode& val,
                                                   bool isCoroutine);
    // #1：接口视图元素装箱 IIFE——ViewRoot 保护视图 self 后 make_optional<elemCpp>(视图值)
    [[nodiscard]] std::string genOptionalViewValueBox(const std::string& elemCpp,
                                                       const std::string& viewExpr);

    // #10：record 字段值装箱——按字段声明类型（RecordSemType.fields 中与 fieldName
    // 同名字段的声明类型）生成字段值表达式：
    //   - 字段声明为 Optional（OptionalSemType / GenericSemType{name=="Optional"}）：
    //     复用 genOptionalTargetInit 装箱（some(arg)/none()/已是 Optional 值防二次装箱，
    //     裸值/record 字面量 → make_optional<elem>）
    //   - 字段声明为 UnionSemType：genUnionBoxing 装箱（含堆联合 Variant 路径；
    //     全值联合 std::variant 返回空 → 直赋靠隐式构造）
    //   - 字段声明为接口视图（InterfaceSemType：Stringer / 用户接口 / 泛型接口
    //     Comparable<Point>）且字段值为 record → genRecordToViewIIFE 做 record→view
    //     转换；outViewValue（可选输出）置 true 表示结果/字段是值视图，调用方须跳过
    //     GcRootHandle（视图非指针，GcRootHandle<视图> 编译失败）。
    //   - 其余字段类型（纯 record/list/Iterator）或 recType 非 RecordSemType
    //     （无法查字段声明类型）→ 直接 genExpr，不装箱。
    [[nodiscard]] std::string genRecordFieldValue(const SemType* recType,
                                                  const ASTNode& fieldValue,
                                                  const std::string& fieldName,
                                                  bool isCoroutine,
                                                  bool* outViewValue = nullptr);

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
    [[nodiscard]] std::string genConditionalExpr(const ConditionalExpr& e, bool isCoroutine);

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

    // === 接口适配器（Interface 改造）===
    // receiverType → 其 impl 的接口名 → 接口类型实参的 C++ 类型名列表（空 = 非泛型接口）
    // 组合收集；键 = AST 接收者名，非泛型下与 C++ 类型名一致
    std::map<std::string, std::map<std::string, std::vector<std::string>>> interfaceImplementations_;
    // 已生成适配器组合名缓存（record 名 + 接口名）
    std::set<std::string> ifaceAdapterCache_;
    // receiverType → 非构造方法名集合（结构匹配组合收集 + 适配器默认方法转发判定）
    std::map<std::string, std::set<std::string>> recordMethods_;
    // 泛型接口适配器上下文的"接口泛型形参名 → 具体 C++ 类型"映射（tmap，如 T → "Point*"）。
    // 仅 genIfaceAdapter 期间非空（构造后设置、退出前恢复）；非空时 mapType / mapGenericRef
    // 对泛型形参名做代换——含容器/复合类型内嵌 T（Optional<T> / [T] / Iterator<T> /
    // Transform<T> / Box2<T> 等 NamedType.typeArgs / ListType / FunctionType / UnionType
    // 子树），对应 Sema 侧 substitute 在 TypeExpr 层的递归实现
    std::map<std::string, std::string> ifaceTypeMap_;

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
    int unionBoxingCounter_ = 0;  // P3b 隐式装箱临时变量名计数器

    // 当前正在生成的函数的协程状态
    bool currentFunctionIsCoroutine_ = false;

    // 当前函数的模板参数列表（用于调用泛型构造函数时传递类型参数）
    std::vector<std::string> currentTParams_;
    std::string              currentLetName_;    // 当前 let 声明的变量名
    bool                     ioSync_ = false;    // 来自 CodeGenConfig

    // M4：当前嵌套闭包链中已由 returnOnlyGenerics 机制声明 `using <名>` 的泛型名集合。
    // genFunExpr 生成 `using U = decltype(...)` 前查此集合：若 U 已由外层闭包声明（内层
    // 闭包引用外层闭包 returnOnlyGenerics 的 U），跳过重复声明、直接复用外层别名——
    // C++ 嵌套 lambda 体内对外层 lambda 局部类型别名可见（已验证）。避免内层 using U
    // 与外层 using U 嵌套遮蔽且内层按自身 delegate/srcType 重新推导与外层不一致。
    // genFunExpr 进入时保存、退出时恢复（兄弟闭包互不影响）。
    std::set<std::string> declaredReturnOnlyGenerics_;

    // 当前函数的 C++ 返回类型（用于 genReturnStmt 生成正确的 RecordExpr 构造）
    std::string currentReturnCppType_;

    // P3b：当前函数返回"含堆联合"时其变体 C++ 类型列表（顺序 = 声明顺序；空 = 非含堆联合返回）
    // 由 funSignature/methodSignature 设置，genReturnStmt 隐式装箱使用
    std::vector<std::string> currentReturnVariantCppTypes_;

    // P1-2：当前函数返回类型是"含 None 变体的 UnionSemType"（全值联合 std::variant
    // 或含堆联合 Variant* 均置 true）。genReturnStmt 对 return none() 生成 None 变体
    // 值（全值联合 → aura_rt::None 直接构造 variant；含堆联合 → 上方列表装箱）。
    bool currentReturnHasNoneVariant_ = false;

    // 当前函数返回 Optional<T> 的元素类型 T（C++ 名），空 = 非 Optional
    // none() 直转 make_none<T> 时使用（C3.2）
    std::string currentReturnElem_;

    // 字符串类型变量名集合（用于 genBinaryExpr 检测 string + T 拼接）
    std::set<std::string> stringVarNames_;

    // 当前函数内已注册为 GcRootHandle 的变量名集合
    // genIdentifier 遇到这些变量名时生成 .get()
    std::set<std::string> gcRootVarNames_;

    // P1：栈上视图变量名集合（接口视图 / 迭代器视图，经 ViewRoot 包裹）
    // 视图含 GC 指针 self，compact 不重写栈上裸指针（GC 只重写 GcRootHandle），
    // 必须用 ViewRoot 注册 self 为 GcRootHandle，GC 后 get() 重建视图取最新 self。
    // genIdentifier 遇到这些变量名时生成 .get()
    std::set<std::string> viewRootVarNames_;

    // P2b：视图变量名 → 视图 C++ 类型名（genFunExpr 闭包捕获转 Global ViewRoot 用）
    // 与 viewRootVarNames_ 一一对应填充；key 与 viewRootVarNames_ 保持一致
    std::unordered_map<std::string, std::string> viewRootTypes_;

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

    // G1：函数名 → 形参 C++ 类型名列表（长度 = 形参总数）。供 genCallExpr 判定
    // Optional/Union 形参并装箱（make_optional / make_variant）；与 fnInterfaceParams_
    // 同机制，在 genFunDecl A 遍注册（调用点可能先于定义生成）。构造函数形参表也
    // 注册于此（键 = 记录名，genCallExpr isCtor 分支的 calleeName = 记录名）。
    std::map<std::string, std::vector<std::string>> fnParamCppTypes_;

    // G1：方法键 "ReceiverType.methodName" → 形参 C++ 类型名列表（与 methodDefaultArgs_
    // 键机制一致，含接口视图方法：接口名.methodName 由 genInterfaceDecl 注册）。
    // 供 genMethodCall 判定 Optional/Union 形参并装箱。
    std::map<std::string, std::vector<std::string>> methodParamCppTypes_;

    // G3：方法键 "ReceiverType.methodName" → 接口形参位置（idx, 接口名）。与
    // methodParamCppTypes_ 同键机制（record 方法由 genMethodDecl、接口方法由
    // genInterfaceDecl 注册）。供 genMethodCall 判定 record 实参直传接口视图形参时
    // 做 record→view 转换（gcConstruct 适配器 + ::view，对照 genCallExpr 的
    // fnInterfaceParams_）；视图变量实参透传不二次包装。
    std::map<std::string, std::vector<std::pair<size_t, std::string>>> methodInterfaceParams_;

    // 函数名 → 默认值表达式指针数组（长度 = 形参总数；nullptr = 无默认值）
    // 同模块函数调用点补默认实参（C5.1）；AST 指针来自本模块 Program，生命周期安全
    std::map<std::string, std::vector<const ASTNode*>> fnDefaultArgs_;

    // 函数名 → 形参类型表达式列表（长度 = 形参总数）。genFunDecl A 遍注册
    // （调用点可能先于定义生成）。供调用点补默认参数闭包时从已传实参推导函数
    // 模板泛型绑定（M3：useT(inc:<T>, v:T, ...) 调用 useT(5,10) → T=int32_t）。
    std::map<std::string, std::vector<const TypeExpr*>> fnParamTypeExprs_;

    // M3：调用点补默认参数闭包时，闭包参数/返回类型中引用的函数模板泛型名 → 调用点
    // 物化后的 C++ 具体类型（如 T → "int32_t"）。仅函数默认参数补全路径设置（方法
    // methodDefaultArgs_ 不设：P4-9 已靠 receiver 具体类型让模板 lambda 隐式转换），
    // 作用域为补全的 genExpr 调用（生成后恢复）。genFunExpr / mapType / mapGenericRef
    // 据其生成普通 lambda（[](int32_t x)->int32_t），避免模板 lambda 无法向具体
    // std::function<int(int)> 函数模板形参推导匹配（g++ no matching / couldn't deduce）。
    std::map<std::string, std::string> defaultArgMaterializedTypes_;

    // 方法键 "ReceiverType.methodName" / ctor 键 "ReceiverType" → 默认值表达式数组（C5.3）
    std::map<std::string, std::vector<const ASTNode*>> methodDefaultArgs_;

    CrossModuleDefaults crossDefaults_;

    // let/const 声明中类型标注的显式模板参数（如 math.Pair<float, bool> → {"float", "bool"}）
    // genLetStmt 设置，genMethodCall 的 ns-ctor 路径消费后清空
    std::vector<std::string> expectedTemplateArgs_;

    // #1：当前显式 Optional<X> 初始化器的目标元素 C++ 名（空 = 非 Optional 目标）。
    // genOptionalTargetInit 设置/恢复，使条件表达式/传参等复合初始化器内嵌的
    // some() 感知目标元素（genCallExpr some() 分支消费）；嵌套 some() 时临时清空
    // 以保持 CTAD。绝不依赖 expectedTemplateArgs_（可能被非 Optional 泛型标注污染）。
    std::string optionalTargetElem_;

    // 错误类型泄漏兜底：error_type 到达 CodeGen 时只报一次干净错误
    // （Sema 应已拦截；此处防漏网之鱼变成 C++ 模板错误）
    bool reportedErrorType_ = false;

    // 错误列表
    DiagnosticEngine& diag_;
};

} // namespace Aura
