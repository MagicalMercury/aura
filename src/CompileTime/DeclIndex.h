// ============================================================
// feature-21 Phase 3 · S2：编译期可见声明索引 DeclIndex
//
// 权威：change.md §4.5（`change.md:525-691`）
// 简报：scripts/f21_p3_s2_brief.md
//
// 登记口径（P3C-13）：
//   - 顶层 `FunDecl`（普通 `fun` 与 `#fun` 都登记，`isComptime` 只做标记）。
//   - 顶层 `MethodDecl`，但构造器与 `#` 接收者的编译期方法**不**登记。
//   - `#type` / `#const` 等非函数声明不进索引。
//
// 可见性：同模块优先（含私有）；跨模块只返回 `pub`；多个同名 `pub` ⇒ ambiguous。
// 本批只建立数据结构，Evaluator / EvalPass 的接线由后续批次承担。
// ============================================================

#pragma once
#include "../AST/Stmt.h"
#include <string>
#include <vector>

namespace Aura::ct {

struct FunInfo {
    const FunDecl* decl = nullptr;
    std::vector<std::string> typeParams;   // 显式 <...> 或签名中的 <T> 顺序
    bool isComptime = false;
    std::string modulePath;
    bool isPublic = false;
};

struct MethodInfo {
    const MethodDecl* decl = nullptr;
    std::vector<std::string> typeParams;
    std::string modulePath;
    bool isPublic = false;
};

class DeclIndex {
public:
    // 多文件：对每个模块调用一次；单文件：只调一次。
    void addModule(const std::string& modulePath, const Program& program);

    // 可见性：同模块优先；跨模块只返回 pub。
    const FunInfo* fun(const std::string& name, const std::string& currentModule,
                       bool* ambiguous = nullptr) const;
    const MethodInfo* method(const std::string& receiverType, const std::string& name,
                             const std::string& currentModule,
                             bool* ambiguous = nullptr) const;

private:
    std::vector<FunInfo> funs_;
    std::vector<MethodInfo> methods_;
};

} // namespace Aura::ct
