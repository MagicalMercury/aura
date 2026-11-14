#pragma once

// ============================================================
// feature-21 Phase 2 · S4：`#const` 展开（ConstTable + pass + 残留/删除/规模 + unquote）
//
// 权威：change.md §3.3（表与作用域）、§3.4.1–§3.4.6（pass / 残留 / 删除 / unquote）、
//       §3.6.1（stampOriginFile 的调用契约）、§8 附录 A（AST 遍历覆盖清单）、
//       §11（N-1/N-3/N-4/N-5/N-9/N-10/N-13/N-14/N-15/N-16/N-21/N-25/N-28）。
//
// 本文件只做「展开语义」；结构遍历骨架（三张表 + §8.4 嵌套载体）在 ExpandConst.cpp
// 内部实现（因为 AstWalk.h 只有 forEachExprChild 一个入口，不能改 S2 的产物）。
//
// 红线（见 scripts/f21_p2_s4_brief.md §2）：
//   1. 只新增本批两个文件；未改 Parser / AST / ModuleManager / main / 测试 / example。
//   2. 不得改 src/AST/AstWalk.h（缺能力就回报，不扩接口）。
//   3. 替换源 = `c.decl->value->clone()`（N-1：当前 AST），**绝不**用建表快照 entry.value。
//      entry.value 只在 applyUnquotes（删除后）使用，且必须是 refreshEntryValues 刷新过的。
//   4. 插入克隆后不复扫（R5 终止性）；自引用/已在替换点报错的节点用 reportedRecursive 去重。
// ============================================================

#include "../AST/AstWalk.h"
#include "../AST/Stmt.h"
#include "../Diag/DiagnosticEngine.h"
#include "ModuleManager.h"

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Aura {

// ============================================================
// §3.3 ConstEntry / ConstTable（字段与 change.md 逐字一致）
// ============================================================
struct ConstEntry {
    std::string name;
    bool        isPublic = false;
    std::string originFile;                  // 定义处（绝对路径优先；未 stamp 时回落当前文件）
    HashConstDecl* decl = nullptr;           // 本轮 working 内的声明节点（身份/定位；删除前有效）
    std::unique_ptr<ASTNode> value;           // 值的 owned 克隆（删除后仍存活，避免悬垂）
    const BlockStmt* scopeBlock = nullptr;    // 指向本轮 working；nullptr = 模块级
    int line = 0, col = 0;                    // 定义位置（块级可见性 = 从定义点到块末）
    std::size_t order = 0;                    // 全局顺序下标（文件序 + 文件内定义序）
};

struct ConstTable {
    // file（绝对路径）→ 条目（按源码顺序追加；指向元素的指针在「建表完成后」才稳定）
    std::map<std::string, std::vector<ConstEntry>> entries;
    // file → 名字 → 模块级 pub 条目
    std::map<std::string, std::map<std::string, const ConstEntry*>> pubExports;
    // file → 名字 → 本模块可见的跨模块 pub 条目；值 == nullptr 表示**歧义**（同名来自两个模块）
    std::map<std::string, std::map<std::string, const ConstEntry*>> moduleVisible;
};

// 本轮 working AST 集合：文件绝对路径 → Program（§3.5 的 working）。
// 用别名固定下来，S5 的 CompilationDriver 直接复用同一类型。
using WorkingMap = std::unordered_map<std::string, std::unique_ptr<Program>>;

// ============================================================
// §3.3 建表 / 解析
// ============================================================

// 在**本轮 working AST** 上建表（R-1：不能建在 pristine 上，否则块指针跨树比较恒假）。
// order = 拓扑序文件列表（被依赖者先）；文件内按源码顺序。返回 false ⇒ 已有错误诊断。
bool buildConstTable(const WorkingMap& working, const std::vector<std::string>& order,
                     ModuleManager& mgr, ConstTable& out, DiagnosticEngine& diag);

// 查找顺序（§3.3）：块内（由内向外，定义点到块末）→ 本模块模块级 → 跨模块 pub。
// 返回 nullptr 且 *ambiguous == true ⇒ 同名跨模块歧义；返回 nullptr 且 !*ambiguous ⇒ 未定义。
const ConstEntry* resolveConst(const ConstTable& table, const std::string& file,
                               const std::vector<const BlockStmt*>& blocks,
                               const std::string& name, int line, int col,
                               bool* ambiguous = nullptr);

// ============================================================
// §3.4.1 重写上下文与五个类型化入口
// ============================================================
struct RewriteCtx {
    const ConstEntry* target = nullptr;                     // 当前正在跑的条目
    const ConstTable* table = nullptr;                      // 解析用（偏差 D-1：Change.md 未列，但 resolveConst 必需）
    std::string file;                                       // 当前文件
    std::vector<const BlockStmt*> blocks;                   // 当前块栈（working 内）
    bool inTargetDecl = false;                              // 正处在 target 自己的初值里
    std::unordered_set<const ASTNode*>* reportedRecursive = nullptr;  // N-25：驱动器持有，跨 ctx 共享
    ModuleManager* mgr = nullptr;                           // 预留给 S5（本批未消费）
    DiagnosticEngine* diag = nullptr;
};

void rewriteExpr(std::unique_ptr<ASTNode>& slot, RewriteCtx& ctx);   // 先判 MetaValueRef，再递归子槽
void rewriteStmt(std::unique_ptr<Stmt>& slot, RewriteCtx& ctx);
void rewriteBlock(std::unique_ptr<BlockStmt>& slot, RewriteCtx& ctx);  // N-3：块槽专用
void rewriteDecl(std::unique_ptr<Decl>& slot, RewriteCtx& ctx);
void rewriteTypeSlot(std::unique_ptr<TypeExpr>& slot, RewriteCtx& ctx);

// ============================================================
// §3.4.2 单遍扫描驱动
// ============================================================
// 每个 entry × 每个文件跑一遍 rewrite（替换源 = c.decl->value->clone()）。
// reportedRecursive 由**驱动器**持有并跨条目共享（N-25）；本函数只往里写。
bool runConstPasses(WorkingMap& working, const std::vector<std::string>& order,
                    ConstTable& table, ModuleManager& mgr, DiagnosticEngine& diag,
                    std::unordered_set<const ASTNode*>& reportedRecursive);

// ============================================================
// §3.4.3 残留检查（删除前）/ 值刷新
// ============================================================
// 在删除声明**之前**遍历所有表达式位的 MetaValueRefExpr：
//   可解析 ⇒ 已报过 recursive 的跳过（N-9），否则 `was not expanded`；不可解析 ⇒ `is not defined`；
//   歧义 ⇒ `ambiguous '#const' reference`。返回 false ⇒ 有残留（已报错）。
bool checkResiduals(WorkingMap& working, const ConstTable& table,
                    const std::unordered_set<const ASTNode*>& reportedRecursive,
                    DiagnosticEngine& diag);

// D6'：pass 在**定义处**把引述体里的 `#N` 换掉了，改的是 decl->value 而非建表快照 ⇒ 刷新。
void refreshEntryValues(ConstTable& table);

// §3.4.4 从 Program::decls 与所有 BlockStmt::stmts（含 spawn 体）移除 HashConstDecl。
void removeConstDecls(WorkingMap& working);

// §3.4.4 统计所有模块 AST 节点总数，> maxNodes ⇒ `AST node count exceeds limit (<maxNodes>)`。
bool checkNodeBudget(const WorkingMap& working, std::size_t maxNodes, DiagnosticEngine& diag);

// ============================================================
// §3.4.5 unquote 分类（D1/D3）
// ============================================================
// singleFile：单文件模式下宏生成 import 直接报 `macro-generated import requires
// multi-file compilation`（N-8：限制先于 resolve）。S5 的 expandSingleFile 传 true。
bool applyUnquotes(WorkingMap& working, const ConstTable& table, ModuleManager& mgr,
                   DiagnosticEngine& diag, bool singleFile = false);

} // namespace Aura
