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
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_set>
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

    // feature-13 C0-5 (2026-09-17): register `import <builtin> as <alias>`
    // mappings so that alias-qualified calls (`p.new(...)`, `m.sqrt(...)`)
    // resolve to the real builtin module. Builtin modules are not registered
    // in the symbol table (see inferMethodCall's BuiltinRegistry branch), so
    // the alias must be translated to the real module name at that query.
    void registerBuiltinImportAliases(const Program& program);

    // importExports 辅助：将一个导出函数/构造函数导入为 Function 符号
    void importFuncSymbol(const std::string& name, const FuncExport& f,
                          const std::string& alias = "");

    // 错误列表
    const std::vector<std::string>& errors() const { return diag_.errorMessages(); }

private:
    // ============ 错误记录 ============
    void error(const ASTNode& node, const std::string& msg);
    // 兼容 DiagnosticEngine 的同名带 hint 形态（无错误码 + 修复提示；
    // 用于联合类型收窄等"信息完整但缺指引"的诊断，输出附 "= help: ..." 行）
    void error(const ASTNode& node, const std::string& msg, const std::string& hint);
    void error(const ASTNode& node, DiagCode code, const std::string& msg, const std::string& hint = "");
    void error(int line, int col, const std::string& msg);
    void error(int line, int col, DiagCode code, const std::string& msg, const std::string& hint = "");

    // ============ 语义类型工具 ============
    // AST 类型 → 语义类型
    [[nodiscard]] std::unique_ptr<SemType> resolveType(const TypeExpr& astType);
    [[nodiscard]] std::unique_ptr<SemType> resolveNamedType(const std::string& name) const;

    // 从 Aura 类型名构造 SemType（int→intType；其他注册类型→GenericSemType；None/未知→Error）
    [[nodiscard]] std::unique_ptr<SemType> semTypeFromAuraName(const std::string& name) const;
    // 从 C++ 类型名映射回 Aura SemType（供 resolvedName 元素类型提取）
    [[nodiscard]] std::unique_ptr<SemType> semTypeFromCppName(const std::string& cppName) const;
    // 从"C++ 名 / resolvedName"反解并实例化用户泛型 record（problem.txt 条目 A/B 共享
    // 工具）：输入如 "Tree<int32_t>*" / "Tree<int32_t>" / "Node"——剥尾 '*' 后提取基名
    // （符号表键）查 TypeAlias→RecordSemType 原始定义，含 '<' 时从 <...> 提取实参按
    // typeParams 位置替换形参（仿 applyTypeArgs），再 sealSelfRefs 标注自引用（深层递归
    // 再走 isAssignable 展开必需）。返回实例化 RecordSemType；无法反解（非 record 别名 /
    // 未注册 / 内置泛型）返回 nullptr。
    [[nodiscard]] std::unique_ptr<RecordSemType>
    instantiateUserRecordFromCppName(const std::string& rawName) const;

    // 联合变体是否 GC 不安全（P0 防崩：含堆联合禁止走 std::variant，见 plan/联合类型GC安全问题.md §4.1）
    // 判定目的与 CodeGen isHeapSemType 不同：isHeapSemType 判"要不要根保护"，
    // 本函数判"该值放 std::variant 内部是否 GC 可达"（对 function/接口结论相反是正确）。
    [[nodiscard]] static bool unionVariantGcUnsafe(const SemType& t);

    // 变体能否安全存入 aura_rt::Variant<T...> storage_（P3b 收窄后的声明期 P0 判定）：
    // 仅 function（std::function 值）/嵌套 union（未扁平化）不可存；其余含堆变体
    // （string/record/list/optional/接口视图）由 descForI 指针/视图追踪放行（variant.h）。
    // substitute 泛型实例化二次检查（P3c）必须复用本判定而非 unionVariantGcUnsafe，
    // 保证泛型实例化形态与非泛型声明形态行为一致（bug-65）。
    [[nodiscard]] static bool variantStorageUnsafe(const SemType& t);

    // P4：在单个变体类型上推断方法调用返回类型（联合动态分派用）；
    // 该变体不支持该调用时返回 nullptr
    [[nodiscard]] std::unique_ptr<SemType> inferMethodCallOnVariant(
        const SemType& variantType, const MethodCallExpr& e);

    // 从迭代器/列表/泛型通道类型推导元素类型（for / sync for 迭代变量类型）
    [[nodiscard]] std::unique_ptr<SemType> elemTypeOf(const SemType* iterType) const;

    // 从 record 物化 canonicalName（如 "Runner<int32_t>" / "Pair<int32_t, aura_rt::GcString*>"）
    // 提取 <...> 内类型实参并解析为 SemType（仿 elemTypeOf，供 inferMethodCall record
    // 分支返回类型做 receiver 泛型实参代换；非泛型 canonicalName 返回空列表）
    [[nodiscard]] std::vector<std::unique_ptr<SemType>>
    extractTypeArgsFromCanonicalName(const std::string& canonicalName) const;

    // 类型等价性
    [[nodiscard]] bool isAssignable(const SemType& target, const SemType& source) const;

    // feature-06（阶段 C）：origins 溯源签名集辅助
    // originsOf：从表达式类型提取溯源签名集（FuncSemType → {自身 clone}；
    // CallableSemType → 其 origins 拷贝；其余 → 空）。joinOrigins：并集追加
    //（FuncSemType::equals 语义去重——origins 内签名必须互异，防 union 调用
    // 候选列表重复）。静态成员（不访问 this）。
    static std::vector<std::shared_ptr<const FuncSemType>>
    originsOf(const SemType& t);
    static void joinOrigins(
        std::vector<std::shared_ptr<const FuncSemType>>& dst,
        const std::vector<std::shared_ptr<const FuncSemType>>& src);
    // record 方法签名查找：typeMethods_（本模块）→ importedMethods_（跨模块导入），
    // canonicalName 先全名后基名（泛型 record 物化实例名 → 声明基名）——供
    // Assignability functor 规则 / C4b 方法值 / C4d functor 降级共用。未找到返回 nullptr
    const InterfaceSemType::MethodSig* findRecordMethod(
        const std::string& canonicalName, const std::string& methodName) const;

    // feature-06（阶段 C）：裸 Callable 调用三态派生（erased/单签名/union 逐签名
    // 匹配）+ 期望回流；functor record 调用降级为 invoke 方法调用（C4d）。见
    // CallInfer.cpp。calleeName 用于错误文案/throws 上下文。
    [[nodiscard]] std::unique_ptr<SemType> inferCallableCall(
        const CallExpr& e, const CallableSemType& cs,
        const SemType* expected, const std::string& calleeName);
    [[nodiscard]] std::unique_ptr<SemType> inferFunctorCall(
        const CallExpr& e, const RecordSemType& rec, const std::string& calleeName);
    // 从赋值/绑定 init 的推断类型提取溯源签名集（传播点统一入口）：
    // FuncSemType → {自身 clone}；CallableSemType → 其 origins；functor record →
    // {invoke 方法签名}；含未绑定泛型的签名剔除（无法静态映射 C++ 包装 → erased）。
    [[nodiscard]] std::vector<std::shared_ptr<const FuncSemType>>
    callableOriginsFromType(const SemType& initTy) const;

    // SemType → C++ 类型名（供 ExprInfer 的 Iterator 桥接方法返回类型推导复用）
    [[nodiscard]] std::string semTypeToCppName(const SemType& t) const;

    // 判断类型是否为内置迭代器（Iterator 接口 / GenericSemType(name="Iterator")）
    [[nodiscard]] bool isIteratorType(const SemType* t) const;

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

    // TypeExpr 泛型引用遍历（DeclChecker.cpp / BodyChecker.cpp 共享）
    static void forEachGenericRef(const TypeExpr& type,
                                  const std::function<void(const std::string&)>& fn);
    // 注册 TypeExpr 中所有泛型引用为 GenericParam 符号（checkFunBody/checkMethodBody 复用）
    static void registerTypeGenerics(SymbolTable& symtab, const TypeExpr& type);
    // 注册 FunctionType（泛型函数类型）中的"裸泛型名"（未声明的 NamedType）为 GenericParam
    // （M5 返回侧 + bug-07 参数侧）。直接写泛型函数类型（`fun makeU() -> fun(U) -> U` 返回 /
    // `f: fun(U) -> U` 参数）时，U 是裸 NamedType，registerTypeGenerics 只认 <T>
    // GenericTypeRef 与别名 typeArgs，不收集裸名 → resolveType 报 undefined type 'U'。
    // 仅从 FunctionType 根递归（顶层非 FunctionType 裸 NamedType 不隐式引入，保持 undefined
    // 报错语义）；已注册名（TypeAlias/Interface/GenericParam）跳过（幂等，接口泛型 T 与
    // 方法裸 U 同 scope 时不遮蔽）。须在类型解析前调用（checkFunBody/checkMethodBody）。
    static void registerFuncTypeGenerics(SymbolTable& symtab, const TypeExpr& type);

    // ============ 声明注册（第 1 遍） ============
    void declareTopLevel(const Program& program);
    void declareDecl(const Decl& decl);
    // 接口符号注册（用户接口与内置 .aurai 接口共用）
    void declareInterface(const InterfaceDecl& i);
    // 解析接口方法签名并填充到已注册的接口符号（allowForward=true 为声明时；
    // false 用于 declareTopLevel 末尾二次解析——此时后置类型已注册，占位 → 完整类型）
    void resolveInterfaceMethods(const InterfaceDecl& i, bool allowForward);
    // 递归收集 TypeExpr 中所有 NamedType 引用（含内置泛型实参内，如 Optional<Point> 的 Point）
    static void forEachIfaceNamedRef(const TypeExpr& type,
                                     const std::function<void(const NamedType&)>& fn);
    // 前向注册接口方法签名引用的未注册用户类型名（仿 TypeDecl 前向占位，DeclChecker.cpp）
    // sourceMethod 非空 = 引用来自该接口默认方法体（bug-09 止血），报错注明来源方法名
    void forwardRegisterIfaceType(const NamedType& n, const std::string& sourceMethod = "");
    // declareTopLevel 末尾：二次解析接口方法签名 + 校验前向引用是否为真 undefined
    void finalizeInterfaceSignatures(const Program& program);

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
    // ============ feature-14 P3：spawn 约束改「函数级可达性」 ============
    //
    // 语义变更：spawn 的合法性判定从「词法必须在 sync 块内」（insideSync_，
    // 第 2 遍 checkSpawnStmt 内）改为「所在函数可被某个 sync 块（直接或经调用链）
    // 可达」。判定延后到第 3 遍 applySpawnReachability（analyze() 内
    // checkProgram 之后）——此时第 2 遍已跑完，符号表全、AST 全在手。
    //
    // 三条设计要点（见 scripts/f14_p3_survey_report.md Q4/Q5/Q6）：
    //   ① 自身根：函数体词法含 SyncStmt → 直接入根集（否则
    //      `fun f() { sync { spawn ... } }` 会被误杀 E018）；
    //   ② 保守放行：任何「静态不可判」调用（函数指针 / Callable / functor /
    //      闭包变量 / 接口动态分派 / 跨模块 / 未登记键）不产生边，且使所在
    //      函数整体豁免 E018（宁漏勿误，最高危风险是误报阻塞合法代码）；
    //   ③ 边界：只放宽 E018。CodeGen 的 `ioInScope_` 闸门是另一条独立约束
    //      （spawn 需要 io 才能做异步 I/O），P3 不触碰。
    void applySpawnReachability(const Program& program);
    // 键格式与 CodeGen 同源：函数/方法用裸名（coroutineFunctions_ 口径），
    // 跨模块导入符号用 qualified(alias::name)（与 sync 根集两侧一致）。
    void buildCallGraph(const Program& program);

    // 调用图：调用者键 → 被调者键集合（仅收录「静态可判定」的边）
    std::map<std::string, std::set<std::string>> callGraph_;
    // 函数体内（穿透闭包体 / spawn 体）词法含 spawn 的函数键
    std::set<std::string> spawnContainingFns_;
    // 函数体内「首个」spawn 语句指针 —— 报错定位用（与现状指向 spawn 语句一致）
    std::map<std::string, const SpawnStmt*> spawnStmtOf_;
    // 函数体内出现过「静态不可判调用」的函数键（Rule 2 豁免集）
    std::set<std::string> hasIndirectCall_;
    // sync 根集：其函数体词法含 sync 块，或经调用链被 sync 可达
    std::set<std::string> syncRoots_;
    // 固定点传播结果（含自身根）
    std::set<std::string> reachable_;
    void checkLockStmt(const LockStmt& stmt);   // lock (m) { } 块语句
    void checkExprStmt(const ExprStmt& stmt);

    // 漏 return 检查辅助：语句/语句块是否在所有路径上以 return/throw 终结
    // （checkFunBody / checkMethodBody / inferFunExpr 共用；防止 CodeGen 生成
    //  no-return 函数/lambda → g++ 插 ud2 运行时崩溃）
    static bool blockAllPathsReturn(const BlockStmt& block);
    static bool stmtAllPathsReturn(const Stmt& stmt);

    // None 不能作为独立类型标注（E017）
    bool rejectStandaloneNone(const Decl& decl, const TypeExpr* type);
    // bug-63/bug-66：初始值是否为"显式 None 值"（none() 调用 / none 字面量）——
    // 供 let/const 声明、record 字段、赋值提交点区分 None 返回调用（void 语义无值
    // 可绑 → 拒）与显式 None 值（NoneType 值语义 → 含 None 联合目标下可绑 → 放行）。
    // （static：仅查 AST 语法形态，不访问 this；定义在 StmtChecker.cpp）
    static bool isNoneValueInitializer(const ASTNode* init);
    // sync 系 max 表达式类型检查（"sync" / "sync thread" / "sync for"）
    void checkSyncMax(const ASTNode& maxExpr, const std::string& kindName);

    // ============ feature-14 U1：spawn 闭包自由变量捕获校验 ============
    //
    // 「body 引用了未被捕获的外层变量」此前静默通过 Sema，生成产物里该名字是裸标识符
    // -> 到 g++ 才报 "'k' was not declared"（用户拿到坏 C++ 才知道）。
    // 本组设施在 Sema 层做与 CodeGen 同口径的自由变量分析，提前干净报错。
    //
    // ⚠️ 口径必须与 CodeGen 一致，否则「Sema 放行、CodeGen 也不捕获」的静默坏码
    //    仍会漏网；反之「Sema 拦下 CodeGen 本会按需追加的名字」会造成误报。
    //    两边共同的判定骨架：idRefs - declared - params - 类型名 - 内置函数名 - io。
    //
    // 自由变量收集器重用 src/ASTWalker.h 的 StmtWalker/ExprWalker
    // 框架（与 CodeGen.h 的 IdRefCollector 同底层），避免自造重复的
    // dynamic_cast 分发链。

    // spawn 闭包形态的自由变量校验（U1 主体，定义在 Checker/StmtSync.cpp）
    void checkSpawnClosureCaptures(const SpawnStmt& stmt);
    // ============ 表达式类型推断 ============
    // expected: 期望类型（借用指针，仅同步透传不存储；nullptr = 纯自底向上）
    [[nodiscard]] std::unique_ptr<SemType> inferExpr(const ASTNode& expr,
                                                     const SemType* expected = nullptr);
    [[nodiscard]] std::unique_ptr<SemType> inferIntLiteral(const IntLiteral& e);
    [[nodiscard]] std::unique_ptr<SemType> inferFloatLiteral(const FloatLiteral& e);
    [[nodiscard]] std::unique_ptr<SemType> inferStringLiteral(const StringLiteral& e);
    [[nodiscard]] std::unique_ptr<SemType> inferBoolLiteral(const BoolLiteral& e);
    [[nodiscard]] std::unique_ptr<SemType> inferNoneLiteral();
    [[nodiscard]] std::unique_ptr<SemType> inferIdentifier(const Identifier& e);
    [[nodiscard]] std::unique_ptr<SemType> inferListExpr(const ListExpr& e,
                                                         const SemType* expected = nullptr);
    [[nodiscard]] std::unique_ptr<SemType> inferRecordExpr(const RecordExpr& e,
                                                           const SemType* expected = nullptr);
    // #5：具名 record 字面量 `Point { x = 1, y = 2 }` 类型推断——typeName 显式给出
    // 类型身份：查符号表（undefined type / Interface / 泛型拦截）→ 校验字段（未知/
    // 重复/缺失/类型不匹配）→ 构造带 canonicalName 的 RecordSemType（CodeGen 走
    // gc_alloc）→ 嵌套匿名字段 propagateCanonicalName 下钻。expected 仅作字段值
    // 反推的额外上下文，可为空（无标注 let 也支持）。
    [[nodiscard]] std::unique_ptr<SemType> inferNamedRecordExpr(const RecordExpr& e,
                                                                const SemType* expected);
    // #4：从期望类型中提取可用于字段反推的 record 形态（RecordSemType 直接返回；
    // Optional/Union 取 record 变体元素；Generic 经符号表解析为 record 类型别名），
    // 供 inferRecordExpr 按字段声明类型反推字段值（none()/[] 反推元素）
    [[nodiscard]] const RecordSemType* recordTypeFromExpected(const SemType* expected);
    [[nodiscard]] std::unique_ptr<SemType> inferBinaryExpr(const BinaryExpr& e);
    [[nodiscard]] std::unique_ptr<SemType> inferUnaryExpr(const UnaryExpr& e);
    [[nodiscard]] std::unique_ptr<SemType> inferCall(const CallExpr& e,
                                                     const SemType* expected = nullptr);
    [[nodiscard]] std::unique_ptr<SemType> inferMethodCall(const MethodCallExpr& e);
    [[nodiscard]] std::unique_ptr<SemType> inferMemberAccess(const MemberAccessExpr& e);
    [[nodiscard]] std::unique_ptr<SemType> inferIndexExpr(const IndexExpr& e);
    [[nodiscard]] std::unique_ptr<SemType> inferAssign(const AssignExpr& e);
    [[nodiscard]] std::unique_ptr<SemType> inferErrorPropagation(const ErrorPropagationExpr& e);
    [[nodiscard]] std::unique_ptr<SemType> inferPipe(const PipeExpr& e);
    [[nodiscard]] std::unique_ptr<SemType> inferConditional(const ConditionalExpr& e,
                                                            const SemType* expected = nullptr);

    // --- 闭包 ---
    [[nodiscard]] std::unique_ptr<SemType> inferFunExpr(const FunExpr& e,
                                                        const SemType* expected = nullptr);

    // ============ match 穷尽性检查 ============
    bool isMatchExhaustive(const SemType& matchedType,
                           const std::vector<MatchCase>& cases);

    // ============ 当前上下文 ============
    // 当前正在检查的函数的返回类型（用于 return 检查）
    std::unique_ptr<SemType> currentReturnType_;
    bool currentFunctionThrows_ = false;
    int  loopDepth_ = 0;      // 循环嵌套深度（替代 insideLoop_ 的 bool，配合同步块边界栈判定 break/continue 跨出）
    // feature-14 P3：**已不再用于 spawn 合法性判定**（改由第 3 遍函数级可达性
    // 判定，见 applySpawnReachability）。变量保留：仍是 sync/sync thread/sync for
    // 系块上下文标记，4 处 ScopedValue 设置点（StmtSync.cpp）照旧维护。
    bool insideSync_ = false;
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
    // #1：实参/赋值/分支等表达式上下文是否为「含匿名 record 字面量」的表达式
    // （RecordExpr 或 some(RecordExpr)）——是则需期望类型推断 + canonicalName 传播，
    // 否则匿名 record 无法解析类型（决策 A）
    static bool isRecordLiteralArg(const ASTNode& arg);
    // #1：内置方法 ParamInfo.typeName → SemType（"T" 从 objType 元素类型提取；
    // 基础类型名映射；不可解析返回 nullptr）
    std::unique_ptr<SemType> semTypeFromBuiltinParam(
        const std::string& typeName, const SemType* objType);
    // 递归检测类型中是否含"不可解析"的 error 元素（[] / none() 无上下文 → 元素类型不可知），
    // 用于在 let/const/return/赋值提交点拦截，避免 error_type 泄漏到 CodeGen
    static bool containsErrorElement(const SemType* t);
    // 递归检测类型中是否含"未绑定泛型变量"（调用点未能把 <T> 绑定到具体类型）。
    // 当前作用域内已注册的泛型参数（泛型函数体内的 T）视为可引用，不视为未绑定；
    // 用于拦截未绑定 T 泄漏到 CodeGen 产生 std::function<T(...)> / auto 等 C++ 错误
    [[nodiscard]] bool containsUnresolvedGeneric(const SemType* t) const;
    // G4：递归检测类型中是否含"裸泛型变量"（GenericSemType 且 resolvedName 为空，
    // 无法物化具体类型）。与 containsUnresolvedGeneric 不同——不做 symtab 判定：
    // 类型别名泛型参数全局注册使 lookup 恒命中（掩盖因素），此处仅按字面判定，
    // 供 inferListExpr 判定声明元素类型是否含需用首元素具体类型替代的泛型变量
    [[nodiscard]] bool containsUnboundGenericParam(const SemType* t) const;
    // bug-67：收紧判定辅助——target（列表字面量 elemType）含未绑定泛型形参时，
    // source 须与 target「结构同形 + 未绑定泛型位置同名」（isAssignable 递归到未绑定
    // 泛型 target 恒 true 的放行洞，Assignability L20-29）；target 此子树不含未绑定
    // 泛型时回退 isAssignable 原判定（不改变既有具体类型语义）。inferListExpr 后续
    // 元素校验用（#58 顶层裸泛型判定的递归扩展，定义在 ExprInfer.cpp）
    [[nodiscard]] bool sameShapeWithUnbound(const SemType* target, const SemType* source) const;
    // G4：递归收集类型中所有"裸泛型变量"名（GenericSemType 且 resolvedName 为空，
    // 去重）——供 checkFunBody/checkMethodBody/inferFunExpr 收集函数/闭包签名引用的
    // 泛型参数名，压入 fnGenericStack_（containsUnresolvedGeneric 据此判定可引用）
    static void collectGenericNames(const SemType* t, std::vector<std::string>& out);
    // seal self-referencing GenericSemType to RecordSemType with full canonicalName
    // （static：只修改传入 node 指向的类型，不访问 this——供 const 的
    //  instantiateUserRecordFromCppName 在深层展开时复用标注）
    static void sealSelfRefs(std::unique_ptr<SemType>& node,
                             const std::string& bareName,
                             const std::string& fullName);
    // #4：声明完成后折叠自引用 `Node | None` 字段为 Optional<Node>（对齐 mapType 声明
    // 侧折叠）：解析期 Node 为 GenericSemType 占位、unionVariantGcUnsafe 恒 false 不折叠，
    // 但 mapType 按 Node* 堆指针折叠为 Optional → 字段值 Variant 装箱与 Optional 字段
    // 类型不匹配。仅折叠「恰 2 变体、其一 None、另一为已 sealed 的非内置泛型」联合。
    void foldSelfRefOptionalUnions(std::unique_ptr<SemType>& node);

    // resolveType 辅助：应用泛型实参到类型
    [[nodiscard]] std::unique_ptr<SemType> applyTypeArgs(
        std::unique_ptr<SemType> result,
        const Symbol& sym,
        const std::vector<std::unique_ptr<TypeExpr>>& typeArgs);

    // resolveType 辅助：为 RecordSemType 拼接 C++ canonicalName 并 seal 自引用
    void materializeCanonicalName(
        std::unique_ptr<SemType>& result,
        const NamedType& n);
    // 正在解析中的类型名集合（用于检测自引用，如 Tree<T> = {..., children: [Tree<T>]})
    std::set<std::string> resolvingTypes_;

    // ============ 接口前向引用（problem.txt：接口方法签名引用后置类型） ============
    // 接口方法签名引用后置 record/别名/泛型 record 时前向占位注册（TypeAlias +
    // resolvingTypes_），declareTopLevel 末尾 finalizeInterfaceSignatures 二次解析
    // 覆盖为完整类型。此处记录前向引用的类型名与其 AST 位置（校验真 undefined 报错定位）。
    struct InterfaceFwdRef {
        std::string name;
        const TypeExpr* node;
        std::string sourceMethod;   // bug-09：引用来源方法名（非空 = 默认方法体引用），报错定位
    };
    std::vector<InterfaceFwdRef> ifaceFwdRefs_;

    // ============ 联合类型 P0 拦截去重 ============
    // 同一 UnionType 变体 AST 节点会被 resolveType 多次解析（如 checkLetDecl 的
    // 占位符号 + 显式类型），P0 错误只对每个变体节点报一次
    std::unordered_set<const TypeExpr*> p0ReportedVariants_;

    // ============ #config 配置 ============
    bool ioSync_ = false;       // #io.sync = true → 同步模式
    bool hasAnyPub_ = false;    // 模块级 pub 策略：文件中出现任一 pub 声明 → 仅导出带 pub 的

    // ============ 表达式类型存储 ============
    // 持有 inferExpr 返回的临时 SemType（供 ASTNode::inferredType 指向）
    std::vector<std::unique_ptr<SemType>> typeStore_;

    // ============ G4 泛型函数栈 ============
    // 当前泛型函数/闭包签名引用的泛型参数名集合（每层一个）。类型别名泛型参数
    // （Transform<T> 的 T）经 defineGlobal 泄漏到全局，symtab lookup 无法区分
    // 「当前泛型函数体内可引用」与「调用点未绑定」，containsUnresolvedGeneric
    // 按此栈判定（T 在任一签名中 → 可引用）
    std::vector<std::vector<std::string>> fnGenericStack_;

    // ============ 成员表 ============
    SymbolTable symtab_;
    DiagnosticEngine& diag_;

    // ============ 接口显式 impl（Interface 改造）============
    // receiverType 规范名 → 该 record 类型拥有的方法签名（buildTypeMethods 构建）
    std::map<std::string, std::vector<InterfaceSemType::MethodSig>> typeMethods_;
    // 跨模块导入的 record 方法：限定 canonicalName（如 "math::Pair"）→ 方法签名。
    // 与 typeMethods_ 分离存放——buildTypeMethods 会 clear typeMethods_（每模块只含
    // 本模块声明），而 importedMethods_ 由 importExports 注入且须在 analyze 全程存活，
    // 否则跨模块 record 方法调用被 bug-01 E013 误伤。
    std::map<std::string, std::vector<InterfaceSemType::MethodSig>> importedMethods_;

    // feature-13 C0-5 (2026-09-17): builtin import alias -> real module name
    // (e.g. "p" -> "path"), filled by registerBuiltinImportAliases from
    // `import path as p` declarations. Empty when no aliased builtin import.
    std::map<std::string, std::string> builtinModuleAliases_;
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
