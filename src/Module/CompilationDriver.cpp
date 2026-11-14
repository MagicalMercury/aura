// ============================================================
// feature-21 Phase 2 · S5：CompilationDriver 实现
//
// 权威：change.md §3.5（epoch 驱动）、§3.7（两处调用点）、§3.6（ModuleManager 接线）、
//       §3.4.2（E-8 位置语义由 S4 的 RefRepositioner 承担）、§11（R-5 / R-10 / N-6 / N-8 / N-25）。
// 简报：scripts/f21_p2_s5_brief.md
//
// 每轮（epoch）: 两段（S5-F1）
//   ① 探测轮（局部 DiagnosticEngine，诊断一律丢弃、working 不提交）：
//      closure(roots, E) → maxFiles → 环检测 → 拓扑序 → 克隆 pristine
//      → buildConstTable → runConstPasses → checkResiduals → refreshEntryValues
//      → removeConstDecls → applyUnquotes → collectEdges
//   ② E2 == E（边集收敛）时才进收敛轮：**重新克隆 pristine**，同一顺序跑一次干净全过程，
//      诊断进主 diag_ → commit + refreshDeclUnitsFromAst。
//   ③ E2 != E ⇒ 发布本轮 import（ModuleInfo）→ 加载新目标 → 下一轮。
// ============================================================

#include "CompilationDriver.h"

#include "ExpandConst.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Aura {
namespace {

namespace fs = std::filesystem;

// 边集 E：`map<from, set<to>>` —— 两侧都有序，`==` 即「边集相等」，canonical 串天然确定
//（R-10：seenEdges 的键必须是规范化**边集**，不能依赖 unordered_map 的迭代序）。
using EdgeSet = std::map<std::string, std::set<std::string>>;

constexpr const char* kBuiltinPrefix = "builtin:";

std::string dirOfPath(const std::string& file) {
    if (file.empty()) return std::string(".");
    const fs::path parent = fsPath(file).parent_path();
    return parent.empty() ? std::string(".") : pathString(parent);
}

// N-6：只有 `.aura` 源文件能进 closure（内置伪节点以 `builtin:` 前缀区分）
bool isAuraSource(const std::string& node) {
    if (node.rfind(kBuiltinPrefix, 0) == 0) return false;
    return fsPath(node).extension() == ".aura";
}

bool isBuiltinNode(const std::string& node) {
    return node.rfind(kBuiltinPrefix, 0) == 0;
}

std::vector<std::string> sortedKeys(const WorkingMap& working) {
    std::vector<std::string> keys;
    keys.reserve(working.size());
    for (const auto& kv : working) keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());
    return keys;
}

// 顶层 import → 边。目标一律写绝对路径（N-7 同一契约）；内置模块写成 `builtin:<name>`
// 伪节点（只参与边收集，不进 files/pristine，见 §3.5 N-6）。
void collectEdgesFromProgram(const std::string& file, Program& prog,
                             ModuleManager& mgr, EdgeSet& out) {
    auto& tos = out[file];                        // 无 import 的文件也要有节点（根可为叶）
    for (auto& d : prog.decls) {
        if (!d) continue;
        if (isComptimeDecl(*d)) continue;
        auto* imp = dynamic_cast<ImportDecl*>(d.get());
        if (!imp) continue;                       // 引述体内的 import 是惰性 AST（§3.2.4）
        if (imp->isBuiltin) { tos.insert(std::string(kBuiltinPrefix) + imp->path); continue; }
        std::string target = imp->path;
        if (target.empty()) continue;
        if (!fsPath(target).is_absolute())        // 展开期已写绝对；防御相对残留
            target = mgr.resolveImportPathFrom(target, dirOfPath(file));
        if (target.empty()) continue;             // 解析失败：parseModule / applyUnquotes 已报
        tos.insert(target);
    }
}

EdgeSet collectEdgesFromModules(ModuleManager& mgr) {
    EdgeSet e;
    std::vector<std::string> keys;
    for (const auto& kv : mgr.modulesInternal()) keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());          // 确定性
    for (const auto& k : keys) {
        auto& mod = mgr.modulesInternal().at(k);
        if (!mod.ast) continue;
        collectEdgesFromProgram(k, *mod.ast, mgr, e);
    }
    return e;
}

EdgeSet collectEdgesFromWorking(const WorkingMap& working, ModuleManager& mgr) {
    EdgeSet e;
    for (const auto& k : sortedKeys(working)) {
        auto it = working.find(k);
        if (it == working.end() || !it->second) continue;
        collectEdgesFromProgram(k, *it->second, mgr, e);
    }
    return e;
}

// rootsOf：入口（hasMain）+ 内置伪节点（只参与边收集）。
// 无入口（库工程）⇒ 已加载模块全体作根，保证闭包不凭空缩小。
std::vector<std::string> rootsOf(ModuleManager& mgr, const EdgeSet& e,
                                 const WorkingMap& pristine) {
    std::set<std::string> roots;
    for (const auto& kv : mgr.modulesInternal())
        if (kv.second.hasMain) roots.insert(kv.first);
    for (const auto& kv : e)
        for (const auto& t : kv.second)
            if (isBuiltinNode(t)) roots.insert(t);
    if (roots.empty())
        for (const auto& kv : pristine) roots.insert(kv.first);
    return std::vector<std::string>(roots.begin(), roots.end());
}

// closure(roots, E)：在 E 上从根可达、且**已加载的 .aura 源文件**集合（R-5：可减）。
std::vector<std::string> closure(const std::vector<std::string>& roots, const EdgeSet& e,
                                 const WorkingMap& pristine) {
    std::set<std::string> files;
    std::set<std::string> seen(roots.begin(), roots.end());
    std::vector<std::string> stack(roots.rbegin(), roots.rend());
    while (!stack.empty()) {
        const std::string node = stack.back();
        stack.pop_back();
        if (!isBuiltinNode(node) && pristine.count(node)) files.insert(node);   // N-6
        auto it = e.find(node);
        if (it == e.end()) continue;
        for (auto rit = it->second.rbegin(); rit != it->second.rend(); ++rit)
            if (seen.insert(*rit).second) stack.push_back(*rit);
    }
    return std::vector<std::string>(files.begin(), files.end());
}

// 环检测（DFS 三色，仅在 files 子图上）：报环时带 provenance（a -> b -> a）。
bool hasCycle(const EdgeSet& e, const std::vector<std::string>& files, DiagnosticEngine& diag) {
    const std::set<std::string> fileSet(files.begin(), files.end());
    std::map<std::string, int> color;             // 0 白 / 1 灰 / 2 黑
    for (const auto& f : files) color[f] = 0;
    std::vector<std::string> stack;

    std::function<bool(const std::string&)> dfs = [&](const std::string& node) -> bool {
        color[node] = 1;
        stack.push_back(node);
        auto it = e.find(node);
        if (it != e.end()) {
            for (const auto& t : it->second) {
                if (!fileSet.count(t)) continue;  // 内置 / 未加载：不参与
                const int c = color[t];
                if (c == 1) {                     // 回边 ⇒ 环
                    std::string path;
                    auto from = std::find(stack.begin(), stack.end(), t);
                    for (auto i = from; i != stack.end(); ++i) path += *i + " -> ";
                    path += t;
                    diag.error(0, 0, "circular import detected in macro-expanded import graph: " + path);
                    return true;
                }
                if (c == 0 && dfs(t)) return true;
            }
        }
        stack.pop_back();
        color[node] = 2;
        return false;
    };

    for (const auto& f : files)
        if (color[f] == 0 && dfs(f)) return true;
    return false;
}

// 拓扑序（被依赖者先；Kahn 的确定性实现，n ≤ maxFiles）。
std::vector<std::string> topoOrder(const EdgeSet& e, const std::vector<std::string>& files) {
    const std::set<std::string> fileSet(files.begin(), files.end());
    std::map<std::string, std::set<std::string>> deps;   // node → 它依赖的 files
    for (const auto& f : files) {
        auto it = e.find(f);
        if (it == e.end()) continue;
        for (const auto& t : it->second)
            if (fileSet.count(t)) deps[f].insert(t);
    }

    std::vector<std::string> order;
    std::set<std::string> emitted;
    order.reserve(files.size());
    while (order.size() < files.size()) {
        bool progressed = false;
        for (const auto& f : files) {
            if (emitted.count(f)) continue;
            bool ready = true;
            for (const auto& d : deps[f])
                if (!emitted.count(d)) { ready = false; break; }
            if (!ready) continue;
            emitted.insert(f);
            order.push_back(f);
            progressed = true;
        }
        if (progressed) continue;
        // 理论不可达（环已在 hasCycle 拦下）：剩余按字典序补齐，保证不挂死
        for (const auto& f : files)
            if (!emitted.count(f)) { emitted.insert(f); order.push_back(f); }
    }
    return order;
}

// R-10：seenEdges 的键 = 规范化边集
std::string canonicalEdges(const EdgeSet& e) {
    std::string s;
    for (const auto& kv : e) {
        s += kv.first;
        s += " ->";
        for (const auto& t : kv.second) { s += ' '; s += t; }
        s += '\n';
    }
    return s;
}

// E2 中「新出现的边目标」且尚未加载者（不含内置伪节点）。
std::vector<std::string> newTargets(const EdgeSet& e2, const EdgeSet& e1, ModuleManager& mgr) {
    std::set<std::string> before;
    for (const auto& kv : e1)
        for (const auto& t : kv.second) before.insert(t);

    std::set<std::string> fresh;
    for (const auto& kv : e2) {
        for (const auto& t : kv.second) {
            if (isBuiltinNode(t)) continue;
            if (!isAuraSource(t)) continue;
            if (before.count(t)) continue;
            if (mgr.modulesInternal().count(t)) continue;   // 已加载（去重）
            fresh.insert(t);
        }
    }
    return std::vector<std::string>(fresh.begin(), fresh.end());
}

// 回写：working（展开后的 AST）→ modules_（§3.5 commit）
void commit(WorkingMap& working, ModuleManager& mgr) {
    auto& mods = mgr.modulesInternal();
    for (auto& kv : working) {
        auto it = mods.find(kv.first);
        if (it == mods.end() || !kv.second) continue;
        it->second.ast = std::move(kv.second);
    }
}

// 从 pristine 克隆本轮 working（R-1：表、块栈、use site 必须在同一棵树）。
// S5-F1：每轮要两份 —— 探测轮（诊断丢弃）与收敛轮（诊断进主 diag），各从 pristine 起。
void cloneWorking(WorkingMap& dst, const WorkingMap& pristine,
                  const std::vector<std::string>& order) {
    for (const auto& f : order) {
        auto it = pristine.find(f);
        if (it == pristine.end() || !it->second) continue;   // builtin / 未加载：不进 working
        dst[f].reset(static_cast<Program*>(it->second->clone().release()));
    }
}

// S5-F1：把本轮（已展开）working 的顶层 import 发布进 ModuleManager 的模块元信息。
//
// 为什么必须：`buildConstTable` 的 moduleVisible 取 `mgr.moduleAt(file)->imports`，
// 而**宏生成的 import 只存在于本轮 working AST**（pristine / modules_.ast 里没有）。
// 不发布 ⇒ 下一轮即使把新模块加载进来（E 已含该边），`#N`（跨模块 `pub #const`）
// 仍解析不到，收敛轮会把它误报成 `is not defined`。
// 语义与 `ModuleManager::rebuildDeclUnit` 对齐：path 一律绝对路径；内置模块只进
// imports、不进 deps；解析失败跳过（收敛轮由 applyUnquotes 报 `cannot resolve`）。
void publishWorkingImports(const WorkingMap& working, ModuleManager& mgr) {
    auto& mods = mgr.modulesInternal();
    for (const auto& k : sortedKeys(working)) {
        auto it = working.find(k);
        if (it == working.end() || !it->second) continue;
        auto mit = mods.find(k);
        if (mit == mods.end()) continue;

        std::vector<ImportInfo> imports;
        std::vector<std::string> deps;
        for (auto& d : it->second->decls) {
            if (!d || isComptimeDecl(*d)) continue;
            auto* imp = dynamic_cast<ImportDecl*>(d.get());
            if (!imp) continue;
            ImportInfo ii;
            ii.path      = imp->path;
            ii.alias     = imp->alias;
            ii.isBuiltin = imp->isBuiltin;
            if (!ii.isBuiltin) {
                std::string resolved = ii.path;
                if (!fsPath(resolved).is_absolute())
                    resolved = mgr.resolveImportPathFrom(ii.path, dirOfPath(k));
                if (resolved.empty()) continue;      // 收敛轮会报（此处只做尽力发布）
                ii.path = resolved;
                deps.push_back(resolved);
            }
            imports.push_back(ii);
        }
        mit->second.imports = std::move(imports);
        mit->second.deps    = std::move(deps);
    }
}

} // namespace

CompilationDriver::CompilationDriver(DiagnosticEngine& diag, DriverConfig cfg)
    : diag_(diag), cfg_(cfg) {}

// ============================================================
// 多文件：§3.5 epoch 不动点
// ============================================================
bool CompilationDriver::run(ModuleManager& mgr) {
    // pristine：本轮「原始 AST」快照（modules_ 的克隆）。每轮从它重新克隆 working（R-1）。
    WorkingMap pristine;
    {
        std::vector<std::string> keys;
        for (const auto& kv : mgr.modulesInternal())
            if (kv.second.ast) keys.push_back(kv.first);
        std::sort(keys.begin(), keys.end());
        for (const auto& k : keys) {
            auto& mod = mgr.modulesInternal().at(k);
            pristine[k].reset(static_cast<Program*>(mod.ast->clone().release()));
        }
    }
    if (pristine.empty()) return !diag_.hasErrors();   // 空工程：无事可做

    EdgeSet E = collectEdgesFromModules(mgr);
    const std::vector<std::string> roots = rootsOf(mgr, E, pristine);
    std::set<std::string> seenEdges;                   // R-10：键 = 规范化边集

    for (std::size_t epoch = 0; epoch < cfg_.maxEpochs; ++epoch) {
        (void)epoch;
        // N-6：closure 只返回 .aura 源文件（内置根只参与边收集）
        const std::vector<std::string> files = closure(roots, E, pristine);
        if (files.size() > cfg_.maxFiles) {
            diag_.error(0, 0, "too many source files in compilation closure (> " +
                              std::to_string(cfg_.maxFiles) + ")");
            return false;
        }
        if (hasCycle(E, files, diag_)) return false;
        const std::vector<std::string> order = topoOrder(E, files);

        // ------------------------------------------------------------
        // ① 探测轮（S5-F1）：**局部 DiagnosticEngine** —— 边集未收敛时的
        //    `is not defined` / `cannot resolve` 只是「本轮还没加载到那些模块」的
        //    中间态，不得污染主 diag；本轮 working 也不提交。
        // ------------------------------------------------------------
        WorkingMap probe;
        cloneWorking(probe, pristine, order);
        EdgeSet E2;
        {
            DiagnosticEngine probeDiag;                    // 全部丢弃（含语法级错误，收敛轮会再报）
            ConstTable probeTable;
            std::unordered_set<const ASTNode*> probeReported;
            if (buildConstTable(probe, order, mgr, probeTable, probeDiag)) {
                (void)runConstPasses(probe, order, probeTable, mgr, probeDiag, probeReported);
                (void)checkResiduals(probe, probeTable, probeReported, probeDiag);
                refreshEntryValues(probeTable);            // D6'
                removeConstDecls(probe);
                (void)applyUnquotes(probe, probeTable, mgr, probeDiag, /*singleFile=*/false);
            }
            E2 = collectEdgesFromWorking(probe, mgr);      // 边集取探测轮的展开结果
        }

        if (E2 != E) {                                   // 未收敛 ⇒ 加载新目标、下一轮
            const std::string key = canonicalEdges(E2);   // R-10
            if (!seenEdges.insert(key).second) {
                diag_.error(0, 0, "macro-generated imports did not converge (oscillation)");
                return false;
            }
            // S5-F1：宏生成的 import 必须发布进模块元信息，否则下一轮 moduleVisible
            // 看不到新模块的 `pub #const`（见 publishWorkingImports 注释）。
            publishWorkingImports(probe, mgr);

            // ⚠️ 顺序修正（报告 D-1）：必须**先**算差集再覆盖 E，否则 newTargets(E2, E) 恒空。
            const std::vector<std::string> fresh = newTargets(E2, E, mgr);
            E = E2;
            for (const auto& target : fresh) {
                if (!mgr.ensureModuleLoaded(target)) return false;
                if (mgr.checkModuleConflicts(diag_)) return false;   // 宏生成模块可能同名/同 ns
                auto& mods = mgr.modulesInternal();
                auto it = mods.find(target);
                if (it == mods.end() || !it->second.ast) {
                    diag_.error(0, 0, "cannot load module '" + target + "' generated by macro import");
                    return false;
                }
                pristine[target].reset(static_cast<Program*>(it->second.ast->clone().release()));
            }
            continue;
        }

        // ------------------------------------------------------------
        // ② 收敛轮（S5-F1）：E2 == E ⇒ 从 pristine **重新克隆**，按正确顺序跑一次
        //    干净的全过程，诊断进主 diag_（探测轮的 working 丢弃）。
        // ------------------------------------------------------------
        WorkingMap working;
        cloneWorking(working, pristine, order);

        ConstTable table;
        if (!buildConstTable(working, order, mgr, table, diag_)) return false;

        std::unordered_set<const ASTNode*> reportedRecursive;      // N-25：驱动器持有
        (void)runConstPasses(working, order, table, mgr, diag_, reportedRecursive);
        if (diag_.hasErrors()) return false;     // 展开期已报错（recursive / 引述位置）：不动用户 AST

        if (!checkResiduals(working, table, reportedRecursive, diag_)) return false;   // 删除前
        refreshEntryValues(table);               // D6'：pass 在定义处改的是 decl->value
        removeConstDecls(working);
        if (!applyUnquotes(working, table, mgr, diag_, /*singleFile=*/false)) return false;
        if (!checkNodeBudget(working, cfg_.maxNodes, diag_)) return false;

        commit(working, mgr);
        mgr.refreshDeclUnitsFromAst();            // R-11：deps / imports 一律绝对路径
        return !diag_.hasErrors();
    }

    diag_.error(0, 0, "macro-generated import expansion exceeded maxEpochs (" +
                      std::to_string(cfg_.maxEpochs) + ")");
    return false;
}

// ============================================================
// 单文件：R-4 / R-8（复用同一套 pass，files/roots 都只有自己）
// ============================================================
bool CompilationDriver::expandSingleFile(Program& program, const std::string& path,
                                        ModuleManager& mgr) {
    const std::string abs = pathString(fs::absolute(fsPath(path)));

    // ① N-8：单文件不走 parseModule ⇒ 入口第一步必须 stamp（否则 dirOf("") 让宏 import 误报）
    ModuleManager::stampOriginFile(program, abs);

    // ② files = {该文件}，roots = {该文件}；单文件不允许宏生成新文件
    WorkingMap working;
    working[abs].reset(static_cast<Program*>(program.clone().release()));
    const std::vector<std::string> order{abs};

    ConstTable table;
    if (!buildConstTable(working, order, mgr, table, diag_)) return false;

    std::unordered_set<const ASTNode*> reportedRecursive;
    (void)runConstPasses(working, order, table, mgr, diag_, reportedRecursive);
    if (diag_.hasErrors()) return false;

    if (!checkResiduals(working, table, reportedRecursive, diag_)) return false;
    refreshEntryValues(table);
    removeConstDecls(working);
    // N-8：单文件限制（`macro-generated import requires multi-file compilation`）在
    //       applyUnquotes 的 resolve **之前**判定 —— 由 singleFile=true 分支承担。
    if (!applyUnquotes(working, table, mgr, diag_, /*singleFile=*/true)) return false;

    auto it = working.find(abs);
    if (it == working.end() || !it->second) {
        diag_.error(0, 0, "internal: single-file working AST is missing");
        return false;
    }
    // 兜底（§3.5 末条）：展开后仍残留用户 import ⇒ 单文件无法满足
    for (auto& d : it->second->decls) {
        auto* imp = dynamic_cast<ImportDecl*>(d.get());
        if (!imp || imp->isBuiltin) continue;
        diag_.error(imp->line, imp->col, "macro-generated import requires multi-file compilation");
        return false;
    }
    if (!checkNodeBudget(working, cfg_.maxNodes, diag_)) return false;

    // ③ 回写：把展开后的声明就地换回调用者的 Program（Program 只有 decls 一项语义字段）
    program.decls.swap(it->second->decls);
    return !diag_.hasErrors();
}

} // namespace Aura
