// ============================================================
// feature-21 Phase 3 · S4：编译期表达式求值器 Evaluator（header）
//
// 权威：change.md §4.6（`change.md:695-805` header / `:807-834` 生命周期 /
//       `:836-890` 表达式分派 / `:892-1043` 7 个 eval* / `:1045-1148` 语句 /
//       `:1152-1340` 调用层 / `:1342-1357` #compile_error）
//       与 §4.6.1（`:1359-1446` 辅助 / `:1448-1534` 内建白名单）
// 简报：scripts/f21_p3_s4_brief.md
//
// 本批范围（S4）：以 §4.6「语句与流程」替换 S3 的 evalBlock 占位，
//   并补齐 S3 登记的缺口——泛型 Unknown 升级（四-2-5）、
//   maxMemoryBytes 计费（四-2-1）、pushFrame/popFrame 接线（四-2-2）、
//   用户方法实参单次求值（四-2-3）、运行期变量判据（四-2-4）。
// ============================================================

#pragma once
#include "DeclIndex.h"
#include "Value.h"
#include "../Diag/DiagnosticEngine.h"
#include <climits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Aura::ct {

struct EvalFailure {
    const ASTNode* node = nullptr;
    std::string message;
    // S5（S4 报红 §5-2）：求值栈快照（由内向外，末项最外层）。
    // `change.md:716-719` 只有 {node, message}，此处按简报 B-2 增补。
    std::vector<std::string> stack;
};

struct EvalLimits {
    std::size_t maxSteps = 1000000;
    std::size_t maxMemoryBytes = 64ull * 1024 * 1024;
    int maxCallDepth = 256;
};

enum class Flow { Normal, Return, Break, Continue };

struct EvalResult {
    Flow flow = Flow::Normal;
    Value value;
};

using TypeEnv = std::map<std::string, std::shared_ptr<TypeInfo>>;

struct Env {
    std::unordered_map<std::string, Value> locals;
    TypeEnv typeEnv;
    Env* parent = nullptr;
    bool lookup(const std::string& name, Value& out) const;
    const std::shared_ptr<TypeInfo>* lookupType(const std::string& name) const;
    void define(const std::string& name, Value v);
    // 赋值目标解析：沿 parent 链找到可写绑定（S5：`x = ...` 需要）。
    Value* lookupMutable(const std::string& name);
};

class Evaluator {
public:
    Evaluator(DiagnosticEngine& diag, const DeclIndex& index,
              std::string modulePath, EvalLimits limits);
    Value evalExpr(const ASTNode& expr, Env& env);
    EvalResult evalBlock(const BlockStmt& block, Env& env);
    Value evalMetaCall(const MetaCallExpr& call, Env& env);
    void setRuntimeNames(std::set<std::string> names);

private:
    Value evalCall(const CallExpr& call, Env& env, bool comptime);
    Value evalMethod(const MethodCallExpr& call, Env& env);
    Value evalBinary(const BinaryExpr& e, Env& env);
    Value evalUnary(const UnaryExpr& e, Env& env);
    Value evalIndex(const IndexExpr& e, Env& env);
    Value evalAssign(const AssignExpr& e, Env& env);
    Value evalMember(const MemberAccessExpr& e, Env& env);
    Value evalMetaMember(const MetaMemberExpr& e, Env& env);
    Value evalRecord(const RecordExpr& e, Env& env);
    Value evalList(const ListExpr& e, Env& env);
    Value callFun(const FunDecl& f, const std::vector<std::string>& typeParams,
                  std::vector<Value> args,
                  const std::vector<std::string>& explicitTypes, Env& callerEnv);
    Value callMethod(const MethodDecl& m, const std::vector<std::string>& typeParams,
                     Value self, std::vector<Value> args,
                     const std::vector<std::string>& explicitTypes, Env& callerEnv);
    Value callBuiltin(const std::string& name, const CallExpr& call, Env& env);
    std::optional<Value> callBuiltinMethod(const std::string& name, const Value& obj,
                                           const MethodCallExpr& call, Env& env);
    Value evalCompileError(const MetaCallExpr& call, Env& env);

    void unify(const TypeExpr& pattern, const std::shared_ptr<TypeInfo>& actual,
               std::map<std::string, std::shared_ptr<TypeInfo>>& typeEnv,
               const ASTNode& at);
    std::shared_ptr<TypeInfo> typeOf(const Value& v);
    std::shared_ptr<TypeInfo> parseTypeInfo(const std::string& name);
    std::string typeNameOf(const TypeExpr& t);
    std::string typeNameOfInfo(const TypeInfo& t);
    bool sameType(const std::shared_ptr<TypeInfo>& a,
                  const std::shared_ptr<TypeInfo>& b);
    bool isForbiddenRoot(const std::string& name) const;

    void step(const ASTNode& node);
    [[noreturn]] void fail(const ASTNode& node, const std::string& msg) {
        // S5：抛出时快照求值栈（`EvalFailure` 已有 stack 字段）。
        throw EvalFailure{&node, msg, stack_};
    }
    void pushFrame(const std::string& fn, int line, int col);
    void popFrame();

    // S4：求值栈 / 调用深度的 RAII 守卫——`evalBlock` 里的 `fail` 以异常
    // 方式展开，成对调用会在异常路径漏 `popFrame`/`--depth_`，故交给析构。
    struct CallGuard {
        Evaluator& ev;
        CallGuard(Evaluator& e, const std::string& fn, int line, int col) : ev(e) {
            e.pushFrame(fn, line, col);
            ++e.depth_;
        }
        ~CallGuard() {
            --ev.depth_;
            ev.popFrame();
        }
        CallGuard(const CallGuard&) = delete;
        CallGuard& operator=(const CallGuard&) = delete;
    };

    // S4：`EvalLimits::maxMemoryBytes` 计费（change.md 只声明字段、无计费点）。
    // 计费口径：每次**新建** list/record/string 按其 payload 递归计一次；
    // `memory_` 单调递增、不回收，作为「累计分配量」上界的近似。
    void chargeMemory(std::size_t bytes, const ASTNode& node);
    std::size_t valueBytes(const Value& v) const;
    bool isBuiltinMethod(const std::string& name) const;
    bool isUnresolvedType(const std::shared_ptr<TypeInfo>& t) const;

    DiagnosticEngine& diag_;
    const DeclIndex& index_;
    std::string modulePath_;
    EvalLimits limits_;
    std::size_t steps_ = 0;
    std::size_t memory_ = 0;
    int depth_ = 0;
    std::set<std::string> runtimeNames_;
    std::vector<std::string> stack_;
};

} // namespace Aura::ct
