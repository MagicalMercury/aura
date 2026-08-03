#pragma once

#include "../AST/ASTNode.h"
#include "../AST/Expr.h"
#include "../AST/Stmt.h"
#include "../AST/Type.h"
#include "../Module/ModuleManager.h"
#include "SemType.h"
#include "SymbolTable.h"
#include "BuiltinRegistry.h"
#include "../Diag/DiagnosticEngine.h"
#include <map>
#include <set>
#include <string>
#include <vector>

namespace Aura {

// ============================================================
// SemAnalyzer — 语义分析器（类型检查 + 符号解析）
//
// 两遍扫描：
//   第 1 遍：声明所有顶层符号（类型、接口、函数签名）
//   第 2 遍：检查函数/方法体（表达式类型推断、返回检查等）
// ============================================================

class SemAnalyzer {
public:
    SemAnalyzer(DiagnosticEngine& diag);

    // 主入口：分析整个程序，返回是否有错误
    [[nodiscard]] bool analyze(const Program& program);

    // #io.sync 配置
    [[nodiscard]] bool getIoSync() const { return ioSync_; }

    // ============ 跨模块导入/导出（Phase A）============
    void importExports(const std::string& alias, const ModuleExports& exports);
    [[nodiscard]] ModuleExports extractExports() const;

    // importExports 辅助：将一个导出函数/构造函数导入为 Function 符号
    void importFuncSymbol(const std::string& name, const FuncExport& f,
                          const std::string& alias = "");

    // 错误列表
    const std::vector<std::string>& errors() const { return diag_.errorMessages(); }

private:
    // ============ 错误记录 ============
    void error(const ASTNode& node, const std::string& msg);
    void error(const ASTNode& node, DiagCode code, const std::string& msg, const std::string& hint = "");
    void error(int line, int col, const std::string& msg);
    void error(int line, int col, DiagCode code, const std::string& msg, const std::string& hint = "");

    // ============ 语义类型工具 ============
    // AST 类型 → 语义类型
    [[nodiscard]] std::unique_ptr<SemType> resolveType(const TypeExpr& astType);
    [[nodiscard]] std::unique_ptr<SemType> resolveNamedType(const std::string& name);

    // 从 Aura 类型名构造 SemType（int→intType；其他注册类型→GenericSemType；None/未知→Error）
    [[nodiscard]] std::unique_ptr<SemType> semTypeFromAuraName(const std::string& name);
    // 从 C++ 类型名映射回 Aura SemType（供 resolvedName 元素类型提取）
    [[nodiscard]] std::unique_ptr<SemType> semTypeFromCppName(const std::string& cppName);

    // 从迭代器/列表/泛型通道类型推导元素类型（for / sync for 迭代变量类型）
    [[nodiscard]] std::unique_ptr<SemType> elemTypeOf(const SemType* iterType);

    // 类型等价性
    [[nodiscard]] bool isAssignable(const SemType& target, const SemType& source) const;

    // 函数签名匹配辅助：参数列表 + 返回值 + throws
    [[nodiscard]] bool matchFuncSig(
        const std::vector<std::unique_ptr<SemType>>& aParams,
        const SemType* aReturn, bool aThrows,
        const std::vector<std::unique_ptr<SemType>>& bParams,
        const SemType* bReturn, bool bThrows) const;

    // 从 BuiltinRegistry 返回类型构造 SemType（在 inferCall/inferMethodCall 三处复用）
    // objType: 调用对象类型（用于 Generic 返回类型解析，如 [T].slice → 与 objType 相同列表类型）
    [[nodiscard]] std::unique_ptr<SemType> semTypeFromBuiltinReturn(
        const ReturnTypeInfo& ret, const SemType* objType = nullptr);

    // 泛型代换：将类型中所有 GenericSemType 替换为具体类型
    [[nodiscard]] std::unique_ptr<SemType> substitute(
        const SemType& type,
        const std::string& genericName,
        const SemType& concrete);

    // 从一对 (形参类型, 实参类型) 中递归收集泛型→具体映射
    // conflict: 同一泛型变量被绑定到不兼容类型时置 true（保留第一个绑定，由调用方报错）
    void collectGenericMapping(
        const SemType& formal, const SemType& actual,
        std::map<std::string, std::unique_ptr<SemType>>& map,
        bool& conflict) const;

    // 调用参数检查：数量 + 逐参数类型 + 泛型映射收集（inferCall/inferMethodCall 4 处复用）
    void checkCallArgs(
        const ASTNode& callNode,                         // 错误定位（CallExpr / MethodCallExpr）
        const std::string& calleeName,
        const std::string& role,                         // 错误文案："function" / "constructor"
        const std::vector<const SemType*>& formalTypes,  // 形参类型（nullptr = 无标注，跳过）
        const std::vector<std::unique_ptr<ASTNode>>& args,
        std::map<std::string, std::unique_ptr<SemType>>& genericMap,
        size_t defaultCount = 0);                        // 尾部默认参数个数（C3.1 保证连续）

    // 默认参数声明规则：尾部连续、类型可赋值、泛型参数拒绝（C3.1）
    void checkDefaultArgRules(const ASTNode& declNode,
                              const std::vector<Param>& params);

    // throws 兼容性检查：非 throws 上下文调用 throws 函数（E016）
    void checkThrowsContext(const ASTNode& callNode, const std::string& calleeName, bool calleeThrows);

    // 将泛型映射代换到返回类型
    [[nodiscard]] std::unique_ptr<SemType> applyGenericMap(
        std::unique_ptr<SemType> result,
        const std::map<std::string, std::unique_ptr<SemType>>& genericMap);

    // ============ 声明注册（第 1 遍） ============
    void declareTopLevel(const Program& program);
    void declareDecl(const Decl& decl);
    // 接口符号注册（用户接口与内置 .aurai 接口共用）
    void declareInterface(const InterfaceDecl& i);

    // ============ 体检查（第 2 遍） ============
    void checkProgram(const Program& program);
    void checkDecl(const Decl& decl);
    void checkFunBody(const FunDecl& decl);
    void checkMethodBody(const MethodDecl& decl);
    void checkStmt(const Stmt& stmt);

    // ============ 语句检查 ============
    void checkBlock(const BlockStmt& stmt);
    void checkLetDecl(const LetDecl& decl);
    void checkConstDecl(const ConstDecl& decl);
    void checkReturnStmt(const ReturnStmt& stmt);
    void checkThrowStmt(const ThrowStmt& stmt);
    void checkIfStmt(const IfStmt& stmt);
    void checkWhileStmt(const WhileStmt& stmt);
    void checkForStmt(const ForStmt& stmt);
    void checkLoopStmt(const LoopStmt& stmt);
    void checkMatchStmt(const MatchStmt& stmt);
    void checkTryCatchStmt(const TryCatchStmt& stmt);
    void checkSyncStmt(const SyncStmt& stmt);
    void checkSyncForStmt(const SyncForStmt& stmt);
    void checkSpawnStmt(const SpawnStmt& stmt);
    void checkLockStmt(const LockStmt& stmt);   // lock (m) { } 块语句
    void checkExprStmt(const ExprStmt& stmt);

    // None 不能作为独立类型标注（E017）
    bool rejectStandaloneNone(const Decl& decl, const TypeExpr* type);
    // sync 系 max 表达式类型检查（"sync" / "sync thread" / "sync for"）
    void checkSyncMax(const ASTNode& maxExpr, const std::string& kindName);

    // ============ 表达式类型推断 ============
    [[nodiscard]] std::unique_ptr<SemType> inferExpr(const ASTNode& expr);
    [[nodiscard]] std::unique_ptr<SemType> inferIntLiteral(const IntLiteral& e);
    [[nodiscard]] std::unique_ptr<SemType> inferFloatLiteral(const FloatLiteral& e);
    [[nodiscard]] std::unique_ptr<SemType> inferStringLiteral(const StringLiteral& e);
    [[nodiscard]] std::unique_ptr<SemType> inferBoolLiteral(const BoolLiteral& e);
    [[nodiscard]] std::unique_ptr<SemType> inferNoneLiteral();
    [[nodiscard]] std::unique_ptr<SemType> inferIdentifier(const Identifier& e);
    [[nodiscard]] std::unique_ptr<SemType> inferListExpr(const ListExpr& e);
    [[nodiscard]] std::unique_ptr<SemType> inferRecordExpr(const RecordExpr& e);
    [[nodiscard]] std::unique_ptr<SemType> inferBinaryExpr(const BinaryExpr& e);
    [[nodiscard]] std::unique_ptr<SemType> inferUnaryExpr(const UnaryExpr& e);
    [[nodiscard]] std::unique_ptr<SemType> inferCall(const CallExpr& e);
    [[nodiscard]] std::unique_ptr<SemType> inferMethodCall(const MethodCallExpr& e);
    [[nodiscard]] std::unique_ptr<SemType> inferMemberAccess(const MemberAccessExpr& e);
    [[nodiscard]] std::unique_ptr<SemType> inferIndexExpr(const IndexExpr& e);
    [[nodiscard]] std::unique_ptr<SemType> inferAssign(const AssignExpr& e);
    [[nodiscard]] std::unique_ptr<SemType> inferErrorPropagation(const ErrorPropagationExpr& e);
    [[nodiscard]] std::unique_ptr<SemType> inferPipe(const PipeExpr& e);

    // --- 闭包 ---
    [[nodiscard]] std::unique_ptr<SemType> inferFunExpr(const FunExpr& e);

    // ============ match 穷尽性检查 ============
    bool isMatchExhaustive(const SemType& matchedType,
                           const std::vector<MatchCase>& cases);

    // ============ 当前上下文 ============
    // 当前正在检查的函数的返回类型（用于 return 检查）
    std::unique_ptr<SemType> currentReturnType_;
    bool currentFunctionThrows_ = false;
    int  loopDepth_ = 0;      // 循环嵌套深度（替代 insideLoop_ 的 bool，配合同步块边界栈判定 break/continue 跨出）
    bool insideSync_ = false; // spawn 仅在 sync 块内合法
    bool inSyncThreadBlock_ = false;  // sync thread 块内（禁止嵌套 / 无参 spawn）
    bool inLockBlock_ = false;        // lock 块内（禁止 return/break/continue 跨出）
    int  insideTry_  = 0;    // try 块嵌套深度（>0 时 ! 不报 non-throwing）

    // ============ 同步块边界栈 ============
    // 记录进入 sync/spawn 块时的循环深度，用于拦截 return/break/continue 跨出块
    // （生成代码会跳过 co_await when_all / _stx waitGroup 析构）
    struct SyncBoundary {
        std::string kind;       // "sync" / "sync thread" / "sync for" / "sync thread for" / "spawn"
        int loopDepthAtEntry;   // 进入块时的 loopDepth_
    };
    std::vector<SyncBoundary> syncBoundaryStack_;

    // RAII：进入/退出同步块边界（push/pop syncBoundaryStack_）
    class SyncBoundaryGuard {
    public:
        SyncBoundaryGuard(SemAnalyzer& sema, std::string kind)
            : sema_(sema) {
            sema_.syncBoundaryStack_.push_back({std::move(kind), sema_.loopDepth_});
        }
        ~SyncBoundaryGuard() { sema_.syncBoundaryStack_.pop_back(); }
        SyncBoundaryGuard(const SyncBoundaryGuard&) = delete;
        SyncBoundaryGuard& operator=(const SyncBoundaryGuard&) = delete;
    private:
        SemAnalyzer& sema_;
    };

    // RAII：保存并临时设置一个标量成员，析构恢复
    // （loopDepth_/insideSync_/inSyncThreadBlock_/inLockBlock_）
    template <typename T>
    class ScopedValue {
    public:
        ScopedValue(T& var, T newVal) : var_(var), old_(var) { var_ = newVal; }
        ~ScopedValue() { var_ = old_; }
        ScopedValue(const ScopedValue&) = delete;
        ScopedValue& operator=(const ScopedValue&) = delete;
    private:
        T& var_;
        T old_;
    };

    // RAII：函数体检查上下文（保存/恢复 currentReturnType_ + currentFunctionThrows_）
    // 支持闭包体检查嵌套在外部函数体检查中时，外层返回类型不被内层覆盖
    class FnCtxGuard {
    public:
        FnCtxGuard(SemAnalyzer& s, std::unique_ptr<SemType> ret, bool throws)
            : s_(s) {
            oldRet_   = std::move(s_.currentReturnType_);
            oldThrows_ = s_.currentFunctionThrows_;
            s_.currentReturnType_ = std::move(ret);
            s_.currentFunctionThrows_ = throws;
        }
        ~FnCtxGuard() {
            s_.currentReturnType_ = std::move(oldRet_);
            s_.currentFunctionThrows_ = oldThrows_;
        }
        FnCtxGuard(const FnCtxGuard&) = delete;
        FnCtxGuard& operator=(const FnCtxGuard&) = delete;
    private:
        SemAnalyzer& s_;
        std::unique_ptr<SemType> oldRet_;
        bool oldThrows_;
    };

    // ============ 递归类型解析 ============
    void propagateCanonicalName(const ASTNode& expr, const SemType* type);
    // seal self-referencing GenericSemType to RecordSemType with full canonicalName
    void sealSelfRefs(std::unique_ptr<SemType>& node,
                      const std::string& bareName,
                      const std::string& fullName);

    // resolveType 辅助：应用泛型实参到类型
    [[nodiscard]] std::unique_ptr<SemType> applyTypeArgs(
        std::unique_ptr<SemType> result,
        const Symbol& sym,
        const std::vector<std::unique_ptr<TypeExpr>>& typeArgs);

    // resolveType 辅助：为 RecordSemType 拼接 C++ canonicalName 并 seal 自引用
    void materializeCanonicalName(
        std::unique_ptr<SemType>& result,
        const NamedType& n);
    // 正在解析中的类型名集合（用于检测自引用，如 Tree<T> = {..., children: [Tree<T>]}）
    std::set<std::string> resolvingTypes_;

    // ============ #config 配置 ============
    bool ioSync_ = false;       // #io.sync = true → 同步模式
    bool hasAnyPub_ = false;    // 模块级 pub 策略：文件中出现任一 pub 声明 → 仅导出带 pub 的

    // ============ 表达式类型存储 ============
    // 持有 inferExpr 返回的临时 SemType（供 ASTNode::inferredType 指向）
    std::vector<std::unique_ptr<SemType>> typeStore_;

    // ============ 成员表 ============
    SymbolTable symtab_;
    DiagnosticEngine& diag_;

    // ============ 接口显式 impl（Interface 改造）============
    // receiverType 规范名 → 该 record 类型拥有的方法签名（buildTypeMethods 构建）
    std::map<std::string, std::vector<InterfaceSemType::MethodSig>> typeMethods_;
    // 第 1 遍末尾统一构建（resolveType 安全时刻）
    void buildTypeMethods(const Program& program);
    // receiverType 规范名（查符号表 RecordSemType.canonicalName）
    [[nodiscard]] std::string recordTypeKey(const std::string& receiverType) const;
    // receiverType 规范名 → 显式 impl 的接口名集合（declareDecl 收集；结构匹配已移除，
    // 接口实现必须显式声明 impl。isAssignable / Comparable 校验据此判定）
    std::map<std::string, std::set<std::string>> recordImplIfaces_;
    // 显式 impl 完整性验证：record 声明 impl 接口 → 接口所有非默认方法必须已实现
    // （第 1 遍末尾调用；报错定位用带 impl 的方法声明节点）
    void verifyImplCompleteness(const Program& program);
};

} // namespace Aura
