# 重构 Plan: genFunExpr 抽 4 个内联 walker 到 ASTWalker.h

> 来源：[TODO.txt L147-152](file:///d:/you/Aura/TODO.txt#L147)
> 目标：消除 [ExprGen.cpp:527-974](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L527) `genFunExpr` 中 4 个内联 walker 结构体
> 策略：**纯结构重构，行为零变化** — 每个 visit 方法逐字符复制
> 归属选择：放 [ASTWalker.h](file:///d:/you/Aura/src/ASTWalker.h)（而非 CodeGen.h）— 这些 walker 是纯 AST 分析工具，不依赖 CodeGenerator 任何成员，可被 CodeGen/CoroDecide/Sema 等任意模块复用
> 日期：2026-07-18

---

## 1. 现状分析

### 1.1 genFunExpr 实际规模（已核实）

| 区段 | 行范围 | 行数 | 说明 |
|:---|:---|:---:|:---|
| 函数总体 | [L527-974](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L527) | 448 | TODO 中"309 行"偏低，实际更大 |
| 内联 IoDetector | [L533-560](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L533) | 28 | Stmt-only，不递归到 Expr |
| 内联 AssignTargetCollector | [L667-696](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L667) | 30 | Stmt-only，不递归到 Expr |
| 内联 CallTargetScanner | [L709-761](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L709) | 53 | Stmt+Expr，完整递归 |
| 内联 CaptureArgScanner | [L773-818](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L773) | 46 | Stmt+Expr，完整递归 |
| **小计：4 个 walker** |  | **157** |  |

### 1.2 关键行为差异（必须严格保留）

四个 walker **不是同构的**，分两类：

| Walker | 遍历范围 | `IfStmt.condition` | `ExprStmt.expr` |
|:---|:---|:---|:---|
| IoDetector | Stmt-only | **不扫描** | 仅检查是否为 `MethodCallExpr`（不递归） |
| AssignTargetCollector | Stmt-only | **不扫描** | 仅检查是否为 `AssignExpr`（不递归） |
| CallTargetScanner | Stmt+Expr | **扫描** | 递归 `scanExpr` |
| CaptureArgScanner | Stmt+Expr | **扫描** | 递归 `scanExpr` |

**重构陷阱**：若把 IoDetector/AssignTargetCollector 也加上 Expr 递归（"统一"），会导致行为变化：
- 性能：扫描更多节点
- 正确性：可能误报（如条件表达式里的赋值被算作 mutable 触发）

**结论**：必须**逐字符复制**每个 visit 方法，不可"顺手统一"。

### 1.3 已有基础设施

[ASTWalker.h](file:///d:/you/Aura/src/ASTWalker.h) 已定义 `StmtWalker<V>` / `ExprWalker<V>` 模板框架。[CodeGen.h:122-193](file:///d:/you/Aura/src/CodeGen/CodeGen.h#L122) 中 `IdRefCollector` / `DeclaredCollector` 是基于该框架的范式参考（但作为 CodeGenerator 嵌套类，未来可考虑也迁出）。

**插入点**：[ASTWalker.h L125](file:///d:/you/Aura/src/ASTWalker.h#L125) `ExprWalker` 模板结束 `};` 之后，L127 `} // namespace Aura` 之前。

**归属选择理由**：
- ✅ 4 个 walker 都是纯 AST 遍历工具，不依赖 CodeGenerator 任何成员
- ✅ ASTWalker.h 是"基于 AST 的遍历工具集合"的天然归属
- ✅ CoroScanner 已在 [CoroDecide.cpp](file:///d:/you/Aura/src/CodeGen/CoroDecide.cpp) 独立实现，证明这类 walker 本就跨文件复用
- ✅ 让 CodeGen.h 更聚焦于"代码生成"，不塞各种 walker
- ⚠️ IoDetector 含 `id->name == "io"` 业务关键字检测，但仍是 AST 形态分析，放 ASTWalker.h 可接受

---

## 2. 提议改动

### Step 0: 准备 — 补充 ASTWalker.h 头文件 + 不可变前缀

**0.1 补充 include**：在 [ASTWalker.h L18-19](file:///d:/you/Aura/src/ASTWalker.h#L18) 现有 `#include "AST/Expr.h"` / `#include "AST/Stmt.h"` 之后追加：

```cpp
#include <set>
#include <string>
#include <vector>
```

理由：4 个 walker 使用 `std::set<std::string>` / `std::vector<std::string>` / `std::string`，当前 ASTWalker.h 未包含这些标准头。

**0.2 分隔注释**：在 ASTWalker.h 中 `ExprWalker` 模板之后、`} // namespace Aura` 之前插入分隔注释：

```cpp
// ============================================================
// 闭包分析 Walker（基于 ASTWalker 模板）
// 用于 genFunExpr 中的捕获/协程/mutable 推导
//
// 这些 walker 是纯 AST 遍历工具，不依赖 CodeGenerator 状态，
// 可被 CodeGen/CoroDecide/Sema 等任意模块复用。
// ============================================================
```

### Step 1: 新增 IoDetector 到 ASTWalker.h

**位置**：[ASTWalker.h L125](file:///d:/you/Aura/src/ASTWalker.h#L125) 之后（`ExprWalker` 模板结束 `};` 之后，`} // namespace Aura` 之前）

**代码**（逐字符复制 [ExprGen.cpp:533-560](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L533) 的内联 struct，仅删除 `struct` 改为 `class`、移除缩进、加 `public:` 前缀、加 `static scan` 入口）：

```cpp
// IoDetector — 检测闭包体内是否包含 io.xxx 方法调用
// 用于判定闭包是否需要协程化（genFunExpr 中 isCoroutine && !ioSync_ 场景）
//
// 注意：Stmt-only，不递归到 Expr。IfStmt 不扫条件（与原内联实现一致）。
class IoDetector {
public:
    bool found = false;
    bool scanStmt(const Stmt& stmt) { return StmtWalker<IoDetector>::walk(stmt, *this); }
    // 便捷入口：扫描整个闭包体
    static bool scan(const BlockStmt& body) {
        IoDetector d;
        for (auto& s : body.stmts)
            if (s && d.scanStmt(*s)) { d.found = true; break; }
        return d.found;
    }
    bool visit(const MethodCallExpr& n, IoDetector&) {
        if (n.object) {
            if (auto* id = dynamic_cast<const Identifier*>(n.object.get()))
                if (id->name == "io") { found = true; return true; }
        }
        return false;
    }
    bool visit(const BlockStmt& n, IoDetector& self) { for (auto& ss : n.stmts) if (ss && self.scanStmt(*ss)) return true; return false; }
    bool visit(const IfStmt& n, IoDetector& self) { if (n.thenBranch && self.scanStmt(*n.thenBranch)) return true; for (auto& ei : n.elseIfs) if (ei.body && self.scanStmt(*ei.body)) return true; if (n.elseBranch && self.scanStmt(*n.elseBranch)) return true; return false; }
    bool visit(const WhileStmt& n, IoDetector& self) { return n.body && self.scanStmt(*n.body); }
    bool visit(const ForStmt& n, IoDetector& self) { return n.body && self.scanStmt(*n.body); }
    bool visit(const LoopStmt& n, IoDetector& self) { return n.body && self.scanStmt(*n.body); }
    bool visit(const TryCatchStmt& n, IoDetector& self) { if (n.tryBody && self.scanStmt(*n.tryBody)) return true; return n.catchBody && self.scanStmt(*n.catchBody); }
    bool visit(const MatchStmt& n, IoDetector& self) { for (auto& c : n.cases) if (c.body) { if (auto* cb = dynamic_cast<const BlockStmt*>(c.body.get())) { if (self.scanStmt(*cb)) return true; } } return false; }
    bool visit(const ExprStmt& n, IoDetector& self) { if (n.expr) { if (auto* mc = dynamic_cast<const MethodCallExpr*>(n.expr.get())) return self.visit(*mc, self); } return false; }
    bool visit(const ReturnStmt&, IoDetector&) { return false; }
    bool visit(const ThrowStmt&, IoDetector&) { return false; }
    bool visit(const LetDecl&, IoDetector&) { return false; }
    bool visit(const ConstDecl&, IoDetector&) { return false; }
    bool visit(const BreakStmt&, IoDetector&) { return false; }
    bool visit(const ContinueStmt&, IoDetector&) { return false; }
    bool visit(const SyncStmt&, IoDetector&) { return false; }
    bool visit(const SyncForStmt&, IoDetector&) { return false; }
    bool visit(const SpawnStmt&, IoDetector&) { return false; }
};
```

**验证关键**：
- ✅ `IfStmt` 不调用 `scanExpr(*n.condition)`（与原一致）
- ✅ `ExprStmt` 仅 `dynamic_cast<const MethodCallExpr*>`，不递归（与原一致）
- ✅ `MatchStmt` 仅处理 `BlockStmt` case body，不处理 AssignExpr case body（与原一致）

### Step 2: 新增 AssignTargetCollector 到 ASTWalker.h

**位置**：IoDetector 之后

**代码**（逐字符复制 [ExprGen.cpp:667-696](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L667)）：

```cpp
// AssignTargetCollector — 检测指定名称的捕获变量是否在赋值表达式左侧出现
// 用于决定闭包是否需要 mutable 关键字
//
// 注意：Stmt-only，不递归到 Expr。IfStmt 不扫条件（与原内联实现一致）。
class AssignTargetCollector {
public:
    std::string targetName;
    bool found = false;
    bool collectStmt(const Stmt& stmt) {
        return StmtWalker<AssignTargetCollector>::walk(stmt, *this);
    }
    // 便捷入口：captures 列表中任一被赋值则返回 true
    static bool anyMatch(const BlockStmt& body, const std::vector<std::string>& captures) {
        for (auto& cap : captures) {
            AssignTargetCollector c;
            c.targetName = cap;
            for (auto& s : body.stmts)
                if (s && c.collectStmt(*s)) return true;
        }
        return false;
    }
    bool visit(const AssignExpr& n, AssignTargetCollector& /*self*/) {
        if (auto* id = dynamic_cast<const Identifier*>(n.target.get())) {
            if (id->name == targetName) { found = true; return true; }
        }
        return false;
    }
    bool visit(const BlockStmt& n, AssignTargetCollector& self) { for (auto& ss : n.stmts) if (ss && self.collectStmt(*ss)) return true; return false; }
    bool visit(const IfStmt& n, AssignTargetCollector& self) { if (n.thenBranch && self.collectStmt(*n.thenBranch)) return true; if (n.elseBranch && self.collectStmt(*n.elseBranch)) return true; for (auto& ei : n.elseIfs) if (ei.body && self.collectStmt(*ei.body)) return true; return false; }
    bool visit(const WhileStmt& n, AssignTargetCollector& self) { return n.body && self.collectStmt(*n.body); }
    bool visit(const ForStmt& n, AssignTargetCollector& self) { return n.body && self.collectStmt(*n.body); }
    bool visit(const LoopStmt& n, AssignTargetCollector& self) { return n.body && self.collectStmt(*n.body); }
    bool visit(const TryCatchStmt& n, AssignTargetCollector& self) { if (n.tryBody && self.collectStmt(*n.tryBody)) return true; return n.catchBody && self.collectStmt(*n.catchBody); }
    bool visit(const SyncStmt& n, AssignTargetCollector& self) { return n.body && self.collectStmt(*n.body); }
    bool visit(const SpawnStmt& n, AssignTargetCollector& self) { for (auto& sb : n.body) if (sb && self.collectStmt(*sb)) return true; return false; }
    bool visit(const MatchStmt& n, AssignTargetCollector& self) { for (auto& c : n.cases) if (c.body) { if (auto* cb = dynamic_cast<const BlockStmt*>(c.body.get())) { if (self.collectStmt(*cb)) return true; } else { if (auto* ae = dynamic_cast<const AssignExpr*>(c.body.get())) return self.visit(*ae, self); } } return false; }
    bool visit(const ExprStmt& n, AssignTargetCollector& self) { if (auto* ae = dynamic_cast<const AssignExpr*>(n.expr.get())) return self.visit(*ae, self); return false; }
    bool visit(const ReturnStmt&, AssignTargetCollector&) { return false; }
    bool visit(const ThrowStmt&, AssignTargetCollector&) { return false; }
    bool visit(const LetDecl&, AssignTargetCollector&) { return false; }
    bool visit(const ConstDecl&, AssignTargetCollector&) { return false; }
    bool visit(const BreakStmt&, AssignTargetCollector&) { return false; }
    bool visit(const ContinueStmt&, AssignTargetCollector&) { return false; }
    bool visit(const SyncForStmt&, AssignTargetCollector&) { return false; }
};
```

**验证关键**：
- ✅ `MatchStmt` 处理两种 case body：`BlockStmt` 和 `AssignExpr`（与原一致）
- ✅ `ExprStmt` 仅检查 `AssignExpr`（与原一致）

### Step 3: 新增 CallTargetScanner 到 ASTWalker.h

**位置**：AssignTargetCollector 之后

**代码**（逐字符复制 [ExprGen.cpp:709-761](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L709)）：

```cpp
// CallTargetScanner — 检测捕获变量是否作为调用目标（被调用）
// 用于决定闭包是否需要 mutable 关键字
//
// 注意：Stmt+Expr 完整递归。IfStmt 扫描条件（与原内联实现一致）。
class CallTargetScanner {
public:
    std::string targetName;
    bool found = false;
    bool scanStmt(const Stmt& stmt) {
        return StmtWalker<CallTargetScanner>::walk(stmt, *this);
    }
    bool scanExpr(const ASTNode& node) {
        return ExprWalker<CallTargetScanner>::walk(node, *this);
    }
    static bool anyMatch(const BlockStmt& body, const std::vector<std::string>& captures) {
        for (auto& cap : captures) {
            CallTargetScanner s;
            s.targetName = cap;
            for (auto& stmt : body.stmts)
                if (stmt && s.scanStmt(*stmt)) return true;
        }
        return false;
    }
    bool visit(const CallExpr& n, CallTargetScanner& /*self*/) {
        if (auto* id = dynamic_cast<const Identifier*>(n.callee.get()))
            if (id->name == targetName) { found = true; return true; }
        for (auto& a : n.args) if (a && scanExpr(*a)) return true;
        return false;
    }
    // Stmt visitors
    bool visit(const BlockStmt& n, CallTargetScanner& self) { for (auto& ss : n.stmts) if (ss && self.scanStmt(*ss)) return true; return false; }
    bool visit(const IfStmt& n, CallTargetScanner& self) { if (n.condition && self.scanExpr(*n.condition)) return true; if (n.thenBranch && self.scanStmt(*n.thenBranch)) return true; for (auto& ei : n.elseIfs) { if (ei.condition && self.scanExpr(*ei.condition)) return true; if (ei.body && self.scanStmt(*ei.body)) return true; } if (n.elseBranch && self.scanStmt(*n.elseBranch)) return true; return false; }
    bool visit(const WhileStmt& n, CallTargetScanner& self) { if (n.condition && self.scanExpr(*n.condition)) return true; return n.body && self.scanStmt(*n.body); }
    bool visit(const ForStmt& n, CallTargetScanner& self) { if (n.iterable && self.scanExpr(*n.iterable)) return true; return n.body && self.scanStmt(*n.body); }
    bool visit(const LoopStmt& n, CallTargetScanner& self) { return n.body && self.scanStmt(*n.body); }
    bool visit(const TryCatchStmt& n, CallTargetScanner& self) { if (n.tryBody && self.scanStmt(*n.tryBody)) return true; return n.catchBody && self.scanStmt(*n.catchBody); }
    bool visit(const SyncStmt& n, CallTargetScanner& self) { return n.body && self.scanStmt(*n.body); }
    bool visit(const SyncForStmt& n, CallTargetScanner& self) { return n.body && self.scanStmt(*n.body); }
    bool visit(const SpawnStmt& n, CallTargetScanner& self) { for (auto& sb : n.body) if (sb && self.scanStmt(*sb)) return true; return false; }
    bool visit(const MatchStmt& n, CallTargetScanner& self) { if (n.expr && self.scanExpr(*n.expr)) return true; for (auto& c : n.cases) if (c.body) { if (auto* cb = dynamic_cast<const BlockStmt*>(c.body.get())) { if (self.scanStmt(*cb)) return true; } else if (self.scanExpr(*c.body)) return true; } return false; }
    bool visit(const ExprStmt& n, CallTargetScanner& self) { return n.expr && self.scanExpr(*n.expr); }
    bool visit(const ReturnStmt& n, CallTargetScanner& self) { return n.expr && self.scanExpr(*n.expr); }
    bool visit(const ThrowStmt& n, CallTargetScanner& self) { return n.expr && self.scanExpr(*n.expr); }
    bool visit(const LetDecl& n, CallTargetScanner& self) { return n.initializer && self.scanExpr(*n.initializer); }
    bool visit(const ConstDecl& n, CallTargetScanner& self) { return n.initializer && self.scanExpr(*n.initializer); }
    bool visit(const BreakStmt&, CallTargetScanner&) { return false; }
    bool visit(const ContinueStmt&, CallTargetScanner&) { return false; }
    // Expr visitors
    bool visit(const BinaryExpr& n, CallTargetScanner& self) { return (n.left && self.scanExpr(*n.left)) || (n.right && self.scanExpr(*n.right)); }
    bool visit(const MethodCallExpr& n, CallTargetScanner& self) { if (n.object && self.scanExpr(*n.object)) return true; for (auto& a : n.args) if (a && self.scanExpr(*a)) return true; return false; }
    bool visit(const UnaryExpr& n, CallTargetScanner& self) { return n.operand && self.scanExpr(*n.operand); }
    bool visit(const MemberAccessExpr& n, CallTargetScanner& self) { return n.object && self.scanExpr(*n.object); }
    bool visit(const IndexExpr& n, CallTargetScanner& self) { return (n.object && self.scanExpr(*n.object)) || (n.index && self.scanExpr(*n.index)); }
    bool visit(const AssignExpr& n, CallTargetScanner& self) { return (n.target && self.scanExpr(*n.target)) || (n.value && self.scanExpr(*n.value)); }
    bool visit(const ErrorPropagationExpr& n, CallTargetScanner& self) { return n.expr && self.scanExpr(*n.expr); }
    bool visit(const PipeExpr& n, CallTargetScanner& self) { return (n.left && self.scanExpr(*n.left)) || (n.right && self.scanExpr(*n.right)); }
    bool visit(const RecordExpr& n, CallTargetScanner& self) { for (auto& f : n.fields) if (f.value && self.scanExpr(*f.value)) return true; return false; }
    bool visit(const ListExpr& n, CallTargetScanner& self) { for (auto& e : n.elements) if (e && self.scanExpr(*e)) return true; return false; }
    bool visit(const FunExpr& n, CallTargetScanner& self) { return n.body && self.scanStmt(*n.body); }
    bool visit(const IntLiteral&, CallTargetScanner&) { return false; }
    bool visit(const FloatLiteral&, CallTargetScanner&) { return false; }
    bool visit(const StringLiteral&, CallTargetScanner&) { return false; }
    bool visit(const BoolLiteral&, CallTargetScanner&) { return false; }
    bool visit(const NoneLiteral&, CallTargetScanner&) { return false; }
    bool visit(const Identifier&, CallTargetScanner&) { return false; }
};
```

**验证关键**：
- ✅ `IfStmt` 扫描 `n.condition`（与原一致）
- ✅ 完整 Expr visitors 列表（与原一致）
- ✅ `Identifier` visitor 返回 false（与原一致，避免误报）

### Step 4: 新增 CaptureArgScanner 到 ASTWalker.h

**位置**：CallTargetScanner 之后

**代码**（逐字符复制 [ExprGen.cpp:773-818](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L773)）：

```cpp
// CaptureArgScanner — 收集 captures 中被调用（作为 callee 或参数）的变量名
// 用于 returnOnlyGenerics 推导（如 make_tree_mapper 闭包中的 U）
//
// 注意：Stmt+Expr 完整递归。Identifier 直接命中即返回 true（与原一致）。
class CaptureArgScanner {
public:
    std::string name;
    bool foundArg = false;
    bool scanStmt(const Stmt& stmt) { return StmtWalker<CaptureArgScanner>::walk(stmt, *this); }
    bool scanExpr(const ASTNode& node) { return ExprWalker<CaptureArgScanner>::walk(node, *this); }
    static std::set<std::string> collectMatched(const BlockStmt& body,
                                                 const std::vector<std::string>& captures) {
        std::set<std::string> result;
        for (auto& cap : captures) {
            CaptureArgScanner s;
            s.name = cap;
            for (auto& stmt : body.stmts) {
                if (stmt && s.scanStmt(*stmt)) { result.insert(cap); break; }
            }
        }
        return result;
    }
    bool visit(const CallExpr& n, CaptureArgScanner& self) {
        if (auto* id = dynamic_cast<const Identifier*>(n.callee.get()))
            if (id->name == name) { foundArg = true; return true; }
        for (auto& a : n.args) if (a && self.scanExpr(*a)) return true;
        return false;
    }
    bool visit(const Identifier& n, CaptureArgScanner&) { if (n.name == name) { foundArg = true; return true; } return false; }
    bool visit(const BlockStmt& n, CaptureArgScanner& self) { for (auto& ss : n.stmts) if (ss && self.scanStmt(*ss)) return true; return false; }
    bool visit(const ReturnStmt& n, CaptureArgScanner& self) { return n.expr && self.scanExpr(*n.expr); }
    bool visit(const ExprStmt& n, CaptureArgScanner& self) { return n.expr && self.scanExpr(*n.expr); }
    bool visit(const IfStmt& n, CaptureArgScanner& self) { if (n.condition && self.scanExpr(*n.condition)) return true; if (n.thenBranch && self.scanStmt(*n.thenBranch)) return true; for (auto& ei : n.elseIfs) { if (ei.condition && self.scanExpr(*ei.condition)) return true; if (ei.body && self.scanStmt(*ei.body)) return true; } if (n.elseBranch && self.scanStmt(*n.elseBranch)) return true; return false; }
    bool visit(const WhileStmt& n, CaptureArgScanner& self) { if (n.condition && self.scanExpr(*n.condition)) return true; return n.body && self.scanStmt(*n.body); }
    bool visit(const ForStmt& n, CaptureArgScanner& self) { if (n.iterable && self.scanExpr(*n.iterable)) return true; return n.body && self.scanStmt(*n.body); }
    bool visit(const LoopStmt& n, CaptureArgScanner& self) { return n.body && self.scanStmt(*n.body); }
    bool visit(const TryCatchStmt& n, CaptureArgScanner& self) { if (n.tryBody && self.scanStmt(*n.tryBody)) return true; return n.catchBody && self.scanStmt(*n.catchBody); }
    bool visit(const SyncStmt& n, CaptureArgScanner& self) { return n.body && self.scanStmt(*n.body); }
    bool visit(const SyncForStmt& n, CaptureArgScanner& self) { return n.body && self.scanStmt(*n.body); }
    bool visit(const SpawnStmt& n, CaptureArgScanner& self) { for (auto& sb : n.body) if (sb && self.scanStmt(*sb)) return true; return false; }
    bool visit(const MatchStmt& n, CaptureArgScanner& self) { for (auto& c : n.cases) if (c.body) { if (auto* cb = dynamic_cast<const BlockStmt*>(c.body.get())) { if (self.scanStmt(*cb)) return true; } else if (self.scanExpr(*c.body)) return true; } return false; }
    bool visit(const LetDecl& n, CaptureArgScanner& self) { return n.initializer && self.scanExpr(*n.initializer); }
    bool visit(const ConstDecl& n, CaptureArgScanner& self) { return n.initializer && self.scanExpr(*n.initializer); }
    bool visit(const BinaryExpr& n, CaptureArgScanner& self) { return (n.left && self.scanExpr(*n.left)) || (n.right && self.scanExpr(*n.right)); }
    bool visit(const UnaryExpr& n, CaptureArgScanner& self) { return n.operand && self.scanExpr(*n.operand); }
    bool visit(const MethodCallExpr& n, CaptureArgScanner& self) { if (n.object && self.scanExpr(*n.object)) return true; for (auto& a : n.args) if (a && self.scanExpr(*a)) return true; return false; }
    bool visit(const MemberAccessExpr& n, CaptureArgScanner& self) { return n.object && self.scanExpr(*n.object); }
    bool visit(const IndexExpr& n, CaptureArgScanner& self) { return (n.object && self.scanExpr(*n.object)) || (n.index && self.scanExpr(*n.index)); }
    bool visit(const AssignExpr& n, CaptureArgScanner& self) { return (n.target && self.scanExpr(*n.target)) || (n.value && self.scanExpr(*n.value)); }
    bool visit(const ErrorPropagationExpr& n, CaptureArgScanner& self) { return n.expr && self.scanExpr(*n.expr); }
    bool visit(const PipeExpr& n, CaptureArgScanner& self) { return (n.left && self.scanExpr(*n.left)) || (n.right && self.scanExpr(*n.right)); }
    bool visit(const RecordExpr& n, CaptureArgScanner& self) { for (auto& f : n.fields) if (f.value && self.scanExpr(*f.value)) return true; return false; }
    bool visit(const ListExpr& n, CaptureArgScanner& self) { for (auto& e : n.elements) if (e && self.scanExpr(*e)) return true; return false; }
    bool visit(const FunExpr& n, CaptureArgScanner& self) { return n.body && self.scanStmt(*n.body); }
    bool visit(const IntLiteral&, CaptureArgScanner&) { return false; }
    bool visit(const FloatLiteral&, CaptureArgScanner&) { return false; }
    bool visit(const StringLiteral&, CaptureArgScanner&) { return false; }
    bool visit(const BoolLiteral&, CaptureArgScanner&) { return false; }
    bool visit(const NoneLiteral&, CaptureArgScanner&) { return false; }
    bool visit(const ThrowStmt&, CaptureArgScanner&) { return false; }
    bool visit(const BreakStmt&, CaptureArgScanner&) { return false; }
    bool visit(const ContinueStmt&, CaptureArgScanner&) { return false; }
};
```

**验证关键**：
- ✅ `MatchStmt` **不**扫描 `n.expr`（与原一致，注意原代码 L796 没有 `if (n.expr && self.scanExpr(*n.expr))`）
- ✅ `Identifier` visitor 命中即返回 true（与原一致，与 CallTargetScanner 不同）

### Step 5: 替换 genFunExpr 中的 4 个内联块

**文件**：[src/CodeGen/ExprGen.cpp](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp)

#### 5.1 替换 IoDetector 区段（L530-564）

**原代码** [L530-565](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L530)：

```cpp
    // 检测闭包体内是否包含 io.xxx 调用 — 若有则在协程上下文中生成协程 lambda
    bool closureHasIo = false;
    if (isCoroutine && !ioSync_) {
        struct IoDetector { ... };  // L533-560
        IoDetector detector;
        for (auto& s : e.body->stmts)
            if (s && detector.scanStmt(*s)) { closureHasIo = true; break; }
    }
    bool closureIsCoro = closureHasIo;
```

**替换为**：

```cpp
    // 检测闭包体内是否包含 io.xxx 调用 — 若有则在协程上下文中生成协程 lambda
    bool closureHasIo = isCoroutine && !ioSync_ && IoDetector::scan(*e.body);
    bool closureIsCoro = closureHasIo;
```

**减少行数**：~35 → 2 行

#### 5.2 替换 AssignTargetCollector 区段（L661-702）

**原代码** [L661-702](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L661)：

```cpp
    // === 3. 检测是否修改捕获变量（决定 mutable 关键字） ===
    bool needsMutable = false;
    // 扫描：赋值左侧是捕获变量 → 直接 mutable
    for (auto& cap : captures) {
        for (auto& s : e.body->stmts) {
            if (!s) continue;
            struct AssignTargetCollector { ... };  // L667-696
            AssignTargetCollector collector;
            collector.targetName = cap;
            if (collector.collectStmt(*s)) { needsMutable = true; break; }
        }
        if (needsMutable) break;
    }
```

**替换为**：

```cpp
    // === 3. 检测是否修改捕获变量（决定 mutable 关键字） ===
    bool needsMutable = !captures.empty() && AssignTargetCollector::anyMatch(*e.body, captures);
```

**减少行数**：~42 → 2 行

#### 5.3 替换 CallTargetScanner 区段（L704-768）

**原代码** [L704-768](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L704)：

```cpp
    // 扫描：捕获变量被用作调用目标 → 按值捕获的 lambda operator() 为 const，需 mutable
    if (!needsMutable) {
        for (auto& cap : captures) {
            for (auto& s : e.body->stmts) {
                if (!s) continue;
                struct CallTargetScanner { ... };  // L709-761
                CallTargetScanner scanner;
                scanner.targetName = cap;
                if (scanner.scanStmt(*s)) { needsMutable = true; break; }
            }
            if (needsMutable) break;
        }
    }
```

**替换为**：

```cpp
    // 扫描：捕获变量被用作调用目标 → 按值捕获的 lambda operator() 为 const，需 mutable
    if (!needsMutable && !captures.empty()) {
        needsMutable = CallTargetScanner::anyMatch(*e.body, captures);
    }
```

**减少行数**：~65 → 4 行

#### 5.4 替换 CaptureArgScanner 区段（L770-827）

**原代码** [L770-827](file:///d:/you/Aura/src/CodeGen/ExprGen.cpp#L770)：

```cpp
    // 收集在闭包体内被引用（作为调用参数或直接调用）的捕获变量名
    std::set<std::string> calledCaptures;
    {
        struct CaptureArgScanner { ... };  // L773-818
        for (auto& cap : captures) {
            for (auto& s : e.body->stmts) {
                if (!s) continue;
                CaptureArgScanner argScanner;
                argScanner.name = cap;
                if (argScanner.scanStmt(*s)) { calledCaptures.insert(cap); break; }
            }
        }
    }
```

**替换为**：

```cpp
    // 收集在闭包体内被引用（作为调用参数或直接调用）的捕获变量名
    std::set<std::string> calledCaptures = CaptureArgScanner::collectMatched(*e.body, captures);
```

**减少行数**：~58 → 2 行

### Step 6: genFunExpr 重构后的完整结构

**重构后 genFunExpr（L527-约 815）**：

```cpp
std::string CodeGenerator::genFunExpr(const FunExpr& e, bool isCoroutine) {
    if (!e.body) return "[]{}";

    // === 1. 协程判定（原 L530-565，压缩为 2 行） ===
    bool closureIsCoro = isCoroutine && !ioSync_ && IoDetector::scan(*e.body);

    // === 2. 捕获分析（原 L567-596，完全不变） ===
    std::set<std::string> allRefs;
    IdRefCollector idCol(allRefs);
    for (auto& s : e.body->stmts)
        if (s) idCol.collectStmt(*s);

    std::set<std::string> declared;
    DeclaredCollector declCol(declared);
    for (auto& s : e.body->stmts)
        if (s) declCol.collectStmt(*s);

    std::set<std::string> paramNames;
    for (auto& p : e.params) paramNames.insert(p.name);

    std::set<std::string> builtins = {"_tasks"};
    std::vector<std::string> captures;
    for (auto& name : allRefs) {
        if (declared.count(name))     continue;
        if (paramNames.count(name))   continue;
        if (builtins.count(name))     continue;
        if (registeredTypes_.count(name)) continue;

        auto it = registeredTypes_.find(name);
        if (it != registeredTypes_.end() && it->second) {
            error(e, "cannot capture heap-allocated variable '" + name +
                  "' in closure (not yet supported)");
            continue;
        }
        captures.push_back(name);
    }

    // === 3. 泛型分析（原 L598-659，完全不变） ===
    // collectTParams / callableParamIndices / callableResultGenerics / returnOnlyGenerics
    // ... 约 60 行原样保留

    // === 4. mutable 检测（原 L661-768，压缩为 5 行） ===
    bool needsMutable = !captures.empty() && AssignTargetCollector::anyMatch(*e.body, captures);
    if (!needsMutable && !captures.empty())
        needsMutable = CallTargetScanner::anyMatch(*e.body, captures);

    // === 5. 被调用的捕获变量（原 L770-827，压缩为 2 行） ===
    std::set<std::string> calledCaptures = CaptureArgScanner::collectMatched(*e.body, captures);

    // === 6. 生成 C++ lambda（原 L829-974，完全不变） ===
    // ... 约 145 行原样保留
    // oss 构造 / 模板参数 / 参数列表 / 返回类型 / invoke_result_t 声明 / 函数体
}
```

---

## 3. 关键假设与决策

### 3.1 关键假设

1. **行为完全不变**：4 个 walker 的 visit 方法逐字符复制，唯一改动是 `struct` → `class` + `public:` 前缀 + 加 `static scan/anyMatch/collectMatched` 入口
2. **4 个 walker 行为差异保留**：
   - IoDetector / AssignTargetCollector：Stmt-only，`IfStmt` 不扫条件
   - CallTargetScanner / CaptureArgScanner：Stmt+Expr，`IfStmt` 扫条件
   - CaptureArgScanner 的 `Identifier` 直接命中（CallTargetScanner 不命中）
3. **`captures.empty()` 短路**：原代码中 `for (auto& cap : captures)` 在 captures 为空时自然不执行。新代码显式加 `!captures.empty()` 前置条件，行为等价但更清晰
4. **`ioSync_` 是 CodeGenerator 成员**：已确认 [CodeGen.h](file:///d:/you/Aura/src/CodeGen/CodeGen.h) 中存在（CoroDecide.cpp 使用过）

### 3.2 关键决策

1. **不修改 genFunExpr 中其他部分**：泛型分析（L598-659）、lambda 生成（L829-974）完全不动
2. **不修改 public API**：`genFunExpr` 签名不变
3. **4 个 walker 作为 Aura 命名空间独立类放到 ASTWalker.h**：纯 AST 分析工具，不依赖 CodeGenerator 状态，可跨模块复用
4. **不引入新依赖**：ASTWalker.h 已 include AST/Expr.h + AST/Stmt.h，已含所需所有 AST 节点类型；`<set>` / `<vector>` / `<string>` 需在 ASTWalker.h 顶部补充 include（当前未包含）

---

## 4. 风险与缓解

| 风险 | 严重度 | 缓解 |
|:---|:---:|:---|
| visit 方法复制时打错字 | 🔴 高 | 每 step 编译 + 跑一个闭包示例验证 |
| 漏复制某个 visit 方法导致 ASTWalker 编译失败 | 🟡 中 | 编译器会立即报错（ASTWalker 模板要求所有节点类型都有 visit） |
| `static scan` 入口的 `for` 循环与原代码行为不一致 | 🟡 中 | 原代码就是 `for (auto& s : body.stmts) if (s && detector.scanStmt(*s)) { ...; break; }`，static 入口完全等价 |
| `anyMatch` 的短路行为与原嵌套循环不一致 | 🟢 低 | 原代码外层 `for (cap)` 找到即 `break`，内层 `for (s)` 找到即 `break`，等价于 `anyMatch` 立即返回 true |

---

## 5. 验证步骤

### 5.1 每 step 完成后的最小验证

```cmd
cd d:\you\Aura
build.cmd
```

期望：编译无错误、无新增警告。

### 5.2 行为不变性专项验证

准备测试文件 `samples/closure_test.aura`：

```aura
fun main(io: Io) {
    // 1. 普通闭包（覆盖捕获分析）
    let add = fun(x: int) -> int { return x + 1 }
    io.println(add(10))  // 期望 11

    // 2. 修改捕获变量（覆盖 AssignTargetCollector → mutable）
    let counter = 0
    let inc = fun() -> int { counter = counter + 1; return counter }
    io.println(inc())  // 期望 1
    io.println(inc())  // 期望 2

    // 3. 调用捕获变量（覆盖 CallTargetScanner → mutable）
    let f = fun(x: int) -> int { return x * 2 }
    let caller = fun(n: int) -> int { return f(n) }
    io.println(caller(5))  // 期望 10

    // 4. 闭包内 io 调用（覆盖 IoDetector → 协程化）
    let logger = fun(msg: string) {
        io.println(msg)
    }
    logger("hello")
}
```

```cmd
aura.exe samples/closure_test.aura
```

期望输出：
```
11
1
2
10
hello
```

### 5.3 高阶函数验证（覆盖 CaptureArgScanner → returnOnlyGenerics）

```aura
type Tree<T> = { value: T, children: [Tree<T>] }

fun make_mapper() -> fun(Tree<int>) -> int {
    return fun(root: Tree<int>) -> int {
        return root.value
    }
}

fun main(io: Io) {
    let t = Tree { value: 42, children: [] }
    let m = make_mapper()
    io.println(m(t))  // 期望 42
}
```

### 5.4 全部完成后的最终验证

1. `build.cmd` 编译成功
2. 跑通 READMEs/15-example.md 中所有闭包相关示例
3. 对比重构前后生成的 .cpp 文件，应当**字符完全一致**（可用 `fc` 或 `diff`）

### 5.5 一致性终极验证

```cmd
:: 重构前先生成一份 .cpp
aura.exe samples/closure_test.aura > before.txt

:: 重构后再生成
aura.exe samples/closure_test.aura > after.txt

fc before.txt after.txt
```

期望：`FC: 找不到差异`。若有差异，立即定位是哪个 walker 行为变化。

---

## 6. 预期收益

| 指标 | 重构前 | 重构后 | 改善 |
|:---|:---:|:---:|:---:|
| genFunExpr 行数 | 448 | ~290 | -158 行（-35%） |
| 内联 struct 数量 | 4 | 0 | -4 |
| cyclomatic complexity（估算） | 90 | ~20 | -78% |
| ASTWalker.h 新增行数 | 0 | +157 | +157（但可复用） |
| walker 复用潜力 | 0 | 4 个独立类 | 可被 CoroScanner / Sema / 其他模块复用 |

---

## 7. 执行顺序

按风险从低到高（每步独立编译验证）：

1. **Step 0**: 补充 ASTWalker.h 头文件 + 分隔注释（编译验证 — 无功能改动）
2. **Step 1**: 新增 IoDetector 到 ASTWalker.h（编译验证 — 此时 ExprGen.cpp 还在用内联 struct，应能通过）
3. **Step 2**: 新增 AssignTargetCollector（同上）
4. **Step 3**: 新增 CallTargetScanner（同上）
5. **Step 4**: 新增 CaptureArgScanner（同上）
6. **Step 5.1**: 替换 IoDetector 使用点（编译 + 跑闭包测试）
7. **Step 5.2**: 替换 AssignTargetCollector 使用点（编译 + 跑 mutable 测试）
8. **Step 5.3**: 替换 CallTargetScanner 使用点（编译 + 跑 mutable 测试）
9. **Step 5.4**: 替换 CaptureArgScanner 使用点（编译 + 跑高阶函数测试）

每步完成后单独 commit，便于回溯。

---

## 8. 不在本 plan 范围内（明确排除）

- ❌ genFunExpr 中泛型分析段（L598-659）的任何改动
- ❌ genFunExpr 中 lambda 生成段（L829-974）的任何改动
- ❌ Res.md §九 其他 4 项（inferCall 复制粘贴 / matchFuncSig / resolveType 拆分 / parseLetDecl 合并）
- ❌ 任何性能优化（如缓存 walker 结果）
- ❌ 任何 API 签名变更
- ❌ 测试用例新增（除手动验证外）

如需上述任何项，请单独 plan。
