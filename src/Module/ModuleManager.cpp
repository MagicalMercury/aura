// ============================================================
// ModuleManager.cpp — Aura 多文件模块管理器实现
// ============================================================

#include "ModuleManager.h"
#include "../AST/AstWalk.h"   // feature-21：stampOriginFile 借 forEachExprChild 递归表达式子槽
#include "../Lexer.h"
#include "../Parser.h"
#include "../Sema/BuiltinRegistry.h"

#include <algorithm>
#include <functional>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_set>

#ifdef _WIN32
// bug-109：窄字节 ↔ 宽字符路径转换需 Win32 API（MultiByteToWideChar / WideCharToMultiByte）。
// NOMINMAX / WIN32_LEAN_AND_MEAN 防止 <windows.h> 的 min/max 宏与 <algorithm> 打架。
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace Aura {

// ============================================================
// bug-109：非 ASCII 路径的安全桥
//
// 现象：`aurac "<中文目录>/输入.aura"` ⇒ 未捕获
//   `std::filesystem::filesystem_error: Cannot convert character sequence: Illegal byte sequence`
//   ⇒ terminate（rc=127）；仅 `--cpp` 输出路径含中文则正常。
//
// 根因（本机实测，见 scripts/enum_impl_b9_report.md §侦查）：
//   · 主机原生子进程的 `argv` 是**本地 ANSI 代码页**（中文 Windows = CP936/GBK）字节；
//     `std::ifstream`/`std::ofstream` 亦按该页解释窄文件名 ⇒ 二者自洽、可正常读写；
//   · 但 libstdc++(MinGW) 的 `std::filesystem::path(const std::string&)` 与 `.string()`
//     走的是 **C locale 的 codecvt**（UCRT64 下无法 `std::locale("")`），凡字节 ≥ 0x80
//     即 EILSEQ ⇒ 抛 `filesystem_error`（"Illegal byte sequence"）。该异常无 catch ⇒ terminate。
//   · 崩/不崩的差异由此而定：单文件模式下 `stemOf(inputPath)`（`fs::path(...).stem()`）
//     在**读入侧**最先触发；而 case B 走 `-S` 在触碰 `fs::path(cppPath)` 之前就 `return`，
//     故仅输出路径含中文不崩（见报告 §侦查⑤）。
//
// 修法：把「窄 ↔ path」的转换显式钉死在**进程 ANSI 代码页**（与 argv / ifstream / ofstream /
//   std::system 同口径），不再借用 C locale codecvt。等价于入口侧的「宽字符路径」思路——
//   每个路径都先物化成 `std::wstring` 再进 `std::filesystem::path`，杜绝窄字符串的隐式转换。
//   ※ 未改 main 形态 / CMake：argv 实测即 CP_ACP，故在 ACP 口径上落地是**精确**解，
//     无需 `wmain`/`-municode`（详见报告 §红线段）。
// ============================================================
std::filesystem::path fsPath(const std::string& s) {
#ifdef _WIN32
    if (s.empty()) return std::filesystem::path();
    const int n = MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) {
        // 非法字节序列（理论上不会出现，argv/文件系统交互本就用同一页）：
        // 退回原生构造，让上层 main 的兜底 catch 转成干净诊断，绝不 terminate。
        return std::filesystem::path(s);
    }
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), w.data(), n);
    return std::filesystem::path(w);
#else
    return std::filesystem::path(s);
#endif
}

// bug-109 case ④：import 名常来自 **UTF-8 源码内容**（本仓 .aura 均为 UTF-8），
//   与 argv 的 CP_ACP 不同 ⇒ 提供一个 UTF-8 口径的构造器，供 resolveImportPath
//   对同一 import 名分别按 ACP / UTF-8 试解析。非法 UTF-8 ⇒ 返回空 path（调用方跳过）。
std::filesystem::path fsPathUtf8(const std::string& s) {
#ifdef _WIN32
    if (s.empty()) return std::filesystem::path();
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                      s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::filesystem::path();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), w.data(), n);
    return std::filesystem::path(w);
#else
    return std::filesystem::path(s);
#endif
}

std::string pathString(const std::filesystem::path& p) {
#ifdef _WIN32
    // 注意：libstdc++ 在 Windows 上 `.string()` 产出的是 **UTF-8**（与 argv 的 CP_ACP 不一致）
    // ⇒ 一旦混用会出现「读取路径写对了却打不开」。此处统一取 `.native()`（宽）再转 CP_ACP。
    const std::wstring& w = p.native();
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_ACP, 0, w.data(), (int)w.size(),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_ACP, 0, w.data(), (int)w.size(), s.data(), n,
                        nullptr, nullptr);
    return s;
#else
    return p.string();
#endif
}

// ============================================================
// ModuleManager 构造
// ============================================================
ModuleManager::ModuleManager(DiagnosticEngine& diag) : diag_(diag) {}

// ============================================================
// 文件工具
// ============================================================
std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream oss;
    oss << f.rdbuf();
    return oss.str();
}

void writeFile(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        std::cerr << "Error: cannot write '" << path << "'\n";
        std::exit(1);
    }
    f << content;
}

std::string stemOf(const std::string& path) {
    namespace fs = std::filesystem;
    return pathString(fsPath(path).stem());   // bug-109：不经 C locale codecvt
}

// ============================================================
// feature-21 Phase 2 §3.6.1：originFile 递归写入的遍历骨架
//
// Parser 不知道文件路径 ⇒ `HashConstDecl::originFile` 只能由 ModuleManager 在解析
// 成功后统一递归写入（含块内 / 函数体 / 引述体）。本组函数只做「找 HashConstDecl 并
// 打戳」，不做任何改写；遍历范围与 ExpandConst.cpp 的 TreeWalker 一致（表达式子槽
// 借 AstWalk.h 的 forEachExprChild，语句/声明子槽在此手工分派）。
// ============================================================
namespace {

void stampStmts(const std::string& file, std::vector<std::unique_ptr<Stmt>>& v);

// 表达式：引述体与块槽都要下钻（引述体内允许声明与 import，见 §3.2.4）
void stampExpr(const std::string& file, ASTNode& e) {
    if (auto* q = dynamic_cast<QuoteExpr*>(&e)) {
        if (q->block) stampStmts(file, q->block->stmts);
        return;
    }
    if (auto* b = dynamic_cast<BlockStmt*>(&e)) {   // AstWalk 的 Block 代理（EvalBlock/FunExpr/Quote）
        stampStmts(file, b->stmts);
        return;
    }
    forEachExprChild(e, [&](std::unique_ptr<ASTNode>& child) {
        if (child) stampExpr(file, *child);
    });
}

void stampDecl(const std::string& file, Decl& d) {
    if (auto* hc = dynamic_cast<HashConstDecl*>(&d)) {
        hc->originFile = file;
        if (hc->value) stampExpr(file, *hc->value);
        return;
    }
    if (auto* f = dynamic_cast<FunDecl*>(&d)) {
        for (auto& p : f->params) if (p.defaultExpr) stampExpr(file, *p.defaultExpr);
        if (f->body) stampStmts(file, f->body->stmts);
        return;
    }
    if (auto* m = dynamic_cast<MethodDecl*>(&d)) {
        for (auto& p : m->params) if (p.defaultExpr) stampExpr(file, *p.defaultExpr);
        if (m->body) stampStmts(file, m->body->stmts);
        return;
    }
    if (auto* i = dynamic_cast<InterfaceDecl*>(&d)) {          // N-15：默认体也是块
        for (auto& sig : i->methods) {
            for (auto& p : sig.params) if (p.defaultExpr) stampExpr(file, *p.defaultExpr);
            if (sig.defaultBody) stampStmts(file, sig.defaultBody->stmts);
        }
        return;
    }
    if (auto* l = dynamic_cast<LetDecl*>(&d)) {
        if (l->initializer) stampExpr(file, *l->initializer);
        return;
    }
    if (auto* c = dynamic_cast<ConstDecl*>(&d)) {
        if (c->initializer) stampExpr(file, *c->initializer);
        return;
    }
    if (auto* mt = dynamic_cast<MetaTypeDecl*>(&d)) {
        for (auto& p : mt->fields) if (p.defaultExpr) stampExpr(file, *p.defaultExpr);
        return;
    }
    // UnquoteItem / UnquoteGroup / ImportDecl / ModuleDecl：无 AST 子槽
}

void stampStmt(const std::string& file, Stmt& s) {
    if (auto* b = dynamic_cast<BlockStmt*>(&s)) { stampStmts(file, b->stmts); return; }
    if (auto* d = dynamic_cast<Decl*>(&s))     { stampDecl(file, *d); return; }   // Decl : Stmt
    if (auto* e = dynamic_cast<ExprStmt*>(&s))  { if (e->expr) stampExpr(file, *e->expr); return; }
    if (auto* e = dynamic_cast<ReturnStmt*>(&s)) { if (e->expr) stampExpr(file, *e->expr); return; }
    if (auto* e = dynamic_cast<ThrowStmt*>(&s))  { if (e->expr) stampExpr(file, *e->expr); return; }
    if (auto* i = dynamic_cast<IfStmt*>(&s)) {
        if (i->condition) stampExpr(file, *i->condition);
        if (i->thenBranch) stampStmts(file, i->thenBranch->stmts);
        for (auto& ei : i->elseIfs) {
            if (ei.condition) stampExpr(file, *ei.condition);
            if (ei.body) stampStmts(file, ei.body->stmts);
        }
        if (i->elseBranch) stampStmts(file, i->elseBranch->stmts);
        return;
    }
    if (auto* w = dynamic_cast<WhileStmt*>(&s)) {
        if (w->condition) stampExpr(file, *w->condition);
        if (w->body) stampStmts(file, w->body->stmts);
        return;
    }
    if (auto* l = dynamic_cast<LoopStmt*>(&s))     { if (l->body) stampStmts(file, l->body->stmts); return; }
    if (auto* f = dynamic_cast<ForStmt*>(&s)) {
        if (f->iterable) stampExpr(file, *f->iterable);
        if (f->body) stampStmts(file, f->body->stmts);
        return;
    }
    if (auto* t = dynamic_cast<TryCatchStmt*>(&s)) {
        if (t->tryBody) stampStmts(file, t->tryBody->stmts);
        if (t->catchBody) stampStmts(file, t->catchBody->stmts);
        return;
    }
    if (auto* y = dynamic_cast<SyncStmt*>(&s)) {
        if (y->maxExpr) stampExpr(file, *y->maxExpr);
        if (y->body) stampStmts(file, y->body->stmts);
        return;
    }
    if (auto* k = dynamic_cast<LockStmt*>(&s)) {
        for (auto& x : k->lockExprs) if (x) stampExpr(file, *x);
        if (k->body) stampStmts(file, k->body->stmts);
        return;
    }
    if (auto* sp = dynamic_cast<SpawnStmt*>(&s)) {   // §3.4.4 的「spawn 体」同一遍历
        for (auto& p : sp->params) if (p.defaultExpr) stampExpr(file, *p.defaultExpr);
        for (auto& a : sp->args) if (a) stampExpr(file, *a);
        if (sp->callExpr) stampExpr(file, *sp->callExpr);
        stampStmts(file, sp->body);
        return;
    }
    if (auto* sf = dynamic_cast<SyncForStmt*>(&s)) {
        if (sf->maxExpr) stampExpr(file, *sf->maxExpr);
        if (sf->iterable) stampExpr(file, *sf->iterable);
        if (sf->body) stampStmts(file, sf->body->stmts);
        return;
    }
    if (auto* m = dynamic_cast<MatchStmt*>(&s)) {
        if (m->expr) stampExpr(file, *m->expr);
        for (auto& c : m->cases) {
            if (auto* cp = dynamic_cast<ConstantPattern*>(c.pattern.get()))
                if (cp->value) stampExpr(file, *cp->value);
            if (!c.body) continue;
            if (auto* b = dynamic_cast<BlockStmt*>(c.body.get())) stampStmts(file, b->stmts);
            else stampExpr(file, *c.body);
        }
        return;
    }
    // BreakStmt / ContinueStmt：无子节点
}

void stampStmts(const std::string& file, std::vector<std::unique_ptr<Stmt>>& v) {
    for (auto& s : v) if (s) stampStmt(file, *s);
}

} // namespace

void ModuleManager::stampOriginFile(Program& program, const std::string& filePath) {
    // 与 modules_ / working 的键同一口径：绝对路径（D7 的「定义处目录」基准）
    const std::string abs = pathString(std::filesystem::absolute(fsPath(filePath)));
    for (auto& d : program.decls) if (d) stampDecl(abs, *d);
}

// ============================================================
// feature-21 Phase 2 §3.6：DeclUnit 的 AST 刷新（imports / deps / hasMain）
//
// N-7 + R-11：`u.imports[].path` 与 `u.deps[]` 同一契约 —— 一律绝对路径。
// 相对 import 在此解析；解析失败记诊断并跳过（与 parseModule 同语义）。
// ============================================================
void ModuleManager::rebuildDeclUnit(const std::string& absPath, Program& ast, DeclUnit& u) {
    u.sourcePath = absPath;
    u.imports.clear();
    u.deps.clear();
    u.hasMain = false;

    for (auto& d : ast.decls) {
        if (!d) continue;
        if (isComptimeDecl(*d)) continue;   // feature-21：`#fun main` 不算入口

        if (auto* imp = dynamic_cast<ImportDecl*>(d.get())) {
            ImportInfo ii;
            ii.path      = imp->path;
            ii.alias     = imp->alias;
            ii.isBuiltin = imp->isBuiltin;
            if (!ii.isBuiltin && isKnownBuiltin(ii.path)) ii.isBuiltin = true;
            if (!ii.isBuiltin) {
                std::string resolved = imp->path;
                if (!fsPath(resolved).is_absolute())
                    resolved = resolveImportPath(imp->path,
                                                 pathString(fsPath(absPath).parent_path()));
                if (resolved.empty()) continue;   // 解析失败：parseModule 阶段已记诊断
                ii.path = resolved;
                u.deps.push_back(resolved);
            }
            u.imports.push_back(ii);
        } else if (auto* fn = dynamic_cast<FunDecl*>(d.get())) {
            if (!fn->isComptime && fn->name == "main") u.hasMain = true;
        }
    }
}

// ============================================================
// 路径 → 标识符
// ============================================================
std::string ModuleManager::sanitizeId(const std::string& path) {
    std::string result;
    for (char c : path) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            result += c;
        } else {
            result += '_';
        }
    }
    // 去掉连续下划线
    std::string dedup;
    bool lastUnderscore = false;
    for (char c : result) {
        if (c == '_') {
            if (!lastUnderscore) dedup += c;
            lastUnderscore = true;
        } else {
            dedup += c;
            lastUnderscore = false;
        }
    }
    return dedup;
}

// feature-13 C0（2026-09-17）：pathToNs（§1.5 痛点② 实测更正）。
//   - **隐式回落**（无 module 声明）→ stem 派生（**现状行为，保持不变以守向后兼容红线**）。
//   - **显式 module** → 不走本函数（parseModule 内直接 `aura_mod_<sanitize(name)>`，无哈希）。
//
// ⚠️ **本次只清理死码，不改行为**：原实现内 `absPath`/`parent`/`combined`/`replace`
//     四行算了不用（父目录被丢弃），返回值恒为 `"aura_mod_" + sanitizeId(stem)`。
//     已移除死码，**返回值逐字不变** → 存量用例产物不变（红线）。
//
// ⚠️ **同名冲突（实测：不同目录同名模块撞 namespace + 产物互相覆盖）留待 C3 处理**：
//     加「目录哈希」会改变**所有**隐式路径的 namespace → **直接破坏向后兼容红线**，
//     故**不能无条件加**。正确做法是在 C3 的汇总段做**全局同名检测**，仅对真冲突者
//     追加哈希（或按 §2.1 报错引导用户补显式 module 声明）。
std::string ModuleManager::pathToNs(const std::string& path) {
    namespace fs = std::filesystem;
    std::string stem = pathString(fsPath(path).stem());
    return "aura_mod_" + sanitizeId(stem);
}

// ============================================================
// bug-94：多文件模块身份键（产物命名 / thunk 前缀 / 依赖 include 名）
//
// 背景：`stem` 丢失目录信息 ⇒ `a/util.aura` 与 `b/util.aura` 撞同一对产物
//   （`util.aura.h/.cpp`）与同一 thunk 前缀（`_aura_mat_util_<seq>`）。
// 策略：默认键 = 裸 stem（向后兼容红线）；仅撞车组改「相对入口目录 slug」。
// ============================================================
std::string ModuleManager::identityKeyOf(const std::string& baseDir,
                                         const std::string& sourcePath) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path absSrc = fs::absolute(fsPath(sourcePath), ec);
    if (ec) absSrc = fsPath(sourcePath);
    fs::path absBase = fs::absolute(fsPath(baseDir), ec);
    if (ec) absBase = fsPath(baseDir);

    fs::path rel = fs::relative(absSrc, absBase, ec);
    const std::string relStr = rel.generic_string();            // 统一 '/'
    if (ec || rel.empty() || relStr.rfind("..", 0) == 0)        // 越界/失败 ⇒ 退路
        rel = absSrc.filename();

    std::string noExt = rel.replace_extension().generic_string();  // 去扩展名
    return sanitizeId(noExt);   // '/'→'_'；非 [A-Za-z0-9_] → '_'；折叠连续 '_'
}

std::unordered_map<std::string, std::string>
ModuleManager::computeModuleKeys(const std::string& entryPath) const {
    namespace fs = std::filesystem;
    const std::string baseDir = pathString(fs::absolute(fsPath(entryPath)).parent_path());

    // ① 收集参与 CodeGen 的非内置模块（有 sourcePath），按 stem 分桶
    std::vector<std::string> paths;
    std::unordered_map<std::string, std::vector<std::string>> byStem;
    for (const auto& [key, info] : modules_) {
        (void)key;
        if (info.isBuiltin || info.sourcePath.empty()) continue;
        paths.push_back(info.sourcePath);
        byStem[stemOf(info.sourcePath)].push_back(info.sourcePath);
    }
    std::sort(paths.begin(), paths.end());                      // 确定性（与调度序无关）
    for (auto& [stem, group] : byStem) std::sort(group.begin(), group.end());

    // ② 默认键 = 裸 stem（非撞车工程产物名逐字节不变）
    std::unordered_map<std::string, std::string> result;
    for (const auto& p : paths) result[p] = stemOf(p);

    // ③ 仅撞车组（同 stem ≥ 2 文件）改派生 slug
    for (const auto& [stem, group] : byStem) {
        if (group.size() < 2) continue;
        std::unordered_map<std::string, std::vector<std::string>> bySlug;
        for (const auto& p : group) bySlug[identityKeyOf(baseDir, p)].push_back(p);
        for (auto& [slug, dups] : bySlug) {
            if (dups.size() == 1) { result[dups[0]] = slug; continue; }
            // ④ 派生 slug 仍撞车（如 `a-b/x.aura` 与 `a/b/x.aura` 均归一成 `a_b_x`）
            //    ⇒ 按路径字典序追加 `_<n>`（确定性；对排序后下标）
            for (size_t i = 0; i < dups.size(); ++i)
                result[dups[i]] = slug + "_" + std::to_string(i);
        }
    }
    return result;
}

// ============================================================
// 内置模块白名单
// ============================================================
bool ModuleManager::isKnownBuiltin(const std::string& name) const {
    // 初始内置模块：path（运行时已提供 aura_rt::path）
    // 未来可扩展：json, http, ...
    static const std::unordered_set<std::string> builtins = {
        "path",
        "math"
    };
    return builtins.count(name) > 0;
}

// ============================================================
// 解析模块
// ============================================================
ModuleInfo ModuleManager::parseModule(const std::string& sourcePath) {
    ModuleInfo info;
    info.sourcePath = pathString(std::filesystem::absolute(fsPath(sourcePath)));
    // 先按「隐式回落 stem」派生（向后兼容：无 module 声明时即为最终值）
    info.moduleName = stemOf(sourcePath);
    info.nsName      = pathToNs(sourcePath);

    std::string source = readFile(sourcePath);
    if (source.empty()) {
        diag_.error(0, 0, sourcePath + ": cannot read file");
        return info;
    }

    // 词法 + 语法分析（一次完整解析，AST 常驻）
    Lexer lexer(source);
    auto tokens = lexer.scanAll();

    Parser parser(std::move(tokens), diag_);
    info.ast = parser.parse();

    if (!info.ast) {
        diag_.error(0, 0, sourcePath + ": failed to parse");
        return info;
    }

    // feature-21 Phase 2 §3.6.1：Parser 不知道文件路径 ⇒ 解析成功后立刻递归写 originFile
    //（必须先于任何建表 / 展开；单文件入口由 CompilationDriver 自行调用）。
    stampOriginFile(*info.ast, info.sourcePath);

    // feature-13 C0（2026-09-17）：解析 `module <ident>` 声明 → 覆盖 moduleName/nsName。
    // 语义（D12）：显式 module 名 = 模块逻辑名（符号前缀 + 产物 namespace 来源），
    // 与文件路径解耦；同 module 多名文件 → **统一 namespace（无哈希）**；
    // 缺声明 → 保持上面的 stem 派生（现状行为，向后兼容）。
    for (auto& d : info.ast->decls) {
        if (auto* md = dynamic_cast<ModuleDecl*>(d.get())) {
            if (!md->name.empty()) {
                info.moduleName = md->name;
                info.nsName     = "aura_mod_" + sanitizeId(md->name);
                info.hasExplicitModule = true;
            }
            break;   // 至多一条（Parser 侧已保证重复报错）
        }
    }

    // 提取 import 依赖 和 has_main
    for (auto& d : info.ast->decls) {
        if (!d) continue;
        if (isComptimeDecl(*d)) continue;   // feature-21：`#fun main` 不得把模块判成有入口

        if (auto* imp = dynamic_cast<ImportDecl*>(d.get())) {
            ImportInfo ii;
            ii.path      = imp->path;
            ii.alias     = imp->alias;
            ii.isBuiltin = imp->isBuiltin;

            if (!ii.isBuiltin) {
                // 用户模块：检查是否已知内置名
                if (isKnownBuiltin(ii.path)) {
                    ii.isBuiltin = true;
                }
            }

            if (ii.isBuiltin) {
                // 内置模块：按需加载对应 .aurai
                loadAuraiFile(ii.path + ".aurai");
                info.imports.push_back(ii);
            } else {
                // 用户模块：解析路径并记录依赖
                std::string importerDir = pathString(fsPath(sourcePath).parent_path());
                std::string resolved = resolveImportPath(ii.path, importerDir);
                if (resolved.empty()) {
                    diag_.error(0, 0, sourcePath + ": cannot resolve import '" + ii.path + "'");
                    continue;
                }
                ii.path = resolved; // 存入解析后的绝对路径
                info.imports.push_back(ii);
                info.deps.push_back(resolved);
            }
        }

        if (auto* fn = dynamic_cast<FunDecl*>(d.get())) {
            if (fn->name == "main") {
                info.hasMain = true;
            }
        }
    }

    return info;
}

// ============================================================
// feature-13 C2（2026-09-17）：声明级扫描与类型串化辅助
//
// 与 parseModule 的关系：完全独立的【新增】路径，不修改 parseModule 任何行为。
// 差异（D-A = A2 决策：扫描只出符号、AST 丢弃）：
//   1. 用 Parser::parseDeclarationsOnly()（跳过所有函数体，不建 body 节点）
//   2. 不把 AST 常驻 —— 函数返回前局部 AST 即析构
//   3. 不调用 loadAuraiFile（内置 .aurai 的按需加载是 parseModule 的职责；
//      扫描产物只记录 imports/deps，与 parseModule 的 imports/deps 语义一致）
// ⚠️ 不接入主流程（接入是 C4）。
// ============================================================

// 类型表达式 → 类型串（尽力而为；不参与任何语义判定，仅供符号骨架展示）。
// 覆盖 parsePrimaryType 能产出的全部形态：NamedType / ListType / RecordType /
// TupleTypeExpr / UnionType / FunctionType / GenericTypeRef。
static std::string typeStringOf(const TypeExpr* t);

static std::string typeStringsImpl(const std::vector<std::unique_ptr<TypeExpr>>& ts) {
    std::string out;
    for (size_t i = 0; i < ts.size(); ++i) {
        if (i > 0) out += ", ";
        out += typeStringOf(ts[i].get());
    }
    return out;
}

static std::string typeStringOf(const TypeExpr* t) {
    if (!t) return std::string();

    if (auto* n = dynamic_cast<const NamedType*>(t)) {
        std::string s;
        for (auto& p : n->namespacePrefix) { s += p; s += "."; }
        s += n->name;
        if (!n->typeArgs.empty()) {
            s += "<";
            for (size_t i = 0; i < n->typeArgs.size(); ++i) {
                if (i > 0) s += ", ";
                s += typeStringOf(n->typeArgs[i].get());
            }
            s += ">";
        }
        return s;
    }
    if (auto* g = dynamic_cast<const GenericTypeRef*>(t)) {
        return "<" + g->name + ">";
    }
    if (auto* l = dynamic_cast<const ListType*>(t)) {
        return "[" + typeStringOf(l->elementType.get()) + "]";
    }
    // enum（方案 B）：骨架比对用稳定串；variant 名进串 ⇒ 增删变体会被 C3 骨架比对捕获
    if (auto* en = dynamic_cast<const EnumTypeExpr*>(t)) {
        std::string s = "enum";
        if (!en->bindingTypes.empty())
            s += "<" + typeStringOf(en->bindingTypes[0].get()) + ">";
        s += " {";
        for (size_t i = 0; i < en->variants.size(); ++i) {
            if (i > 0) s += ", ";
            s += en->variants[i].name;
            if (en->variants[i].value) s += " = ?";   // 值不参与骨架串（表达式形态不稳定）
        }
        return s + "}";
    }
    if (auto* r = dynamic_cast<const RecordType*>(t)) {
        std::string s = "{";
        for (size_t i = 0; i < r->fields.size(); ++i) {
            if (i > 0) s += ", ";
            s += r->fields[i].name + ": " + typeStringOf(r->fields[i].type.get());
        }
        return s + "}";
    }
    if (auto* tp = dynamic_cast<const TupleTypeExpr*>(t)) {
        std::string s = "(";
        for (size_t i = 0; i < tp->elementTypes.size(); ++i) {
            if (i > 0) s += ", ";
            s += typeStringOf(tp->elementTypes[i].get());
        }
        return s + ")";
    }
    if (auto* u = dynamic_cast<const UnionType*>(t)) {
        std::string s;
        for (size_t i = 0; i < u->types.size(); ++i) {
            if (i > 0) s += " | ";
            s += typeStringOf(u->types[i].get());
        }
        return s;
    }
    if (auto* f = dynamic_cast<const FunctionType*>(t)) {
        std::string s = "fun(" + typeStringsImpl(f->paramTypes) + ")";
        if (f->throws) s += " throws";
        if (f->returnType) s += " -> " + typeStringOf(f->returnType.get());
        return s;
    }
    return std::string();   // 未知形态：留空（尽力而为）
}

// 参数表 → 类型串列表（尽力而为：无类型标注的参数留空串）
static std::vector<std::string> typeStringsOf(const std::vector<Param>& params) {
    std::vector<std::string> out;
    out.reserve(params.size());
    for (auto& p : params) out.push_back(typeStringOf(p.type.get()));
    return out;
}

DeclUnit ModuleManager::scanDeclarations(const std::string& sourcePath) {
    DeclUnit unit;
    unit.sourcePath = pathString(std::filesystem::absolute(fsPath(sourcePath)));
    // 先按「隐式回落 stem」派生（与 parseModule 同款，C0 逻辑）
    unit.moduleName = stemOf(sourcePath);

    std::string source = readFile(sourcePath);
    if (source.empty()) {
        diag_.error(0, 0, sourcePath + ": cannot read file");
        return unit;
    }

    // 词法 + 声明级语法分析（AST 仅局部持有，不做常驻）
    Lexer lexer(source);
    auto tokens = lexer.scanAll();

    Parser parser(std::move(tokens), diag_);
    auto ast = parser.parseDeclarationsOnly();
    if (!ast) {
        diag_.error(0, 0, sourcePath + ": failed to parse");
        return unit;
    }

    // 显式 module 声明 → 覆盖 moduleName（与 parseModule 同语义）
    for (auto& d : ast->decls) {
        if (auto* md = dynamic_cast<ModuleDecl*>(d.get())) {
            if (!md->name.empty()) {
                unit.moduleName = md->name;
                unit.hasExplicitModule = true;
            }
            break;   // 至多一条（Parser 侧已保证重复报错）
        }
    }

    // 逐声明收集骨架
    for (auto& d : ast->decls) {
        if (!d) continue;
        if (isComptimeDecl(*d)) continue;   // feature-21：编译期声明不进跨模块函数骨架

        // ---- import：与 parseModule 的 imports/deps 语义逐条对齐 ----
        if (auto* imp = dynamic_cast<ImportDecl*>(d.get())) {
            ImportInfo ii;
            ii.path      = imp->path;
            ii.alias     = imp->alias;
            ii.isBuiltin = imp->isBuiltin;

            if (!ii.isBuiltin && isKnownBuiltin(ii.path)) {
                ii.isBuiltin = true;
            }

            if (ii.isBuiltin) {
                // 内置模块：只进 imports、不进 deps（与 parseModule 一致）
                // 注意：此处不 loadAuraiFile（那是 parseModule 的职责）
                unit.imports.push_back(ii);
            } else {
                std::string importerDir =
                    pathString(fsPath(sourcePath).parent_path());
                std::string resolved = resolveImportPath(ii.path, importerDir);
                if (resolved.empty()) {
                    diag_.error(0, 0, sourcePath + ": cannot resolve import '" + ii.path + "'");
                    continue;
                }
                ii.path = resolved;   // 存入解析后的绝对路径
                unit.imports.push_back(ii);
                unit.deps.push_back(resolved);
            }
            continue;
        }

        // ---- type 声明 ----
        if (auto* td = dynamic_cast<TypeDecl*>(d.get())) {
            DeclSkeleton sk;
            sk.name        = td->name;
            sk.typeParams  = td->typeParams;
            sk.isInterface = false;
            sk.isPublic    = td->isPublic;
            sk.hasBody     = (td->type != nullptr);
            unit.types.push_back(std::move(sk));
            continue;
        }

        // ---- interface 声明 ----
        if (auto* id = dynamic_cast<InterfaceDecl*>(d.get())) {
            DeclSkeleton sk;
            sk.name        = id->name;
            sk.typeParams  = id->typeParams;
            sk.isInterface = true;
            sk.isPublic    = id->isPublic;
            sk.hasBody     = !id->methods.empty();
            unit.types.push_back(std::move(sk));
            continue;
        }

        // ---- 普通函数 ----
        if (auto* fn = dynamic_cast<FunDecl*>(d.get())) {
            if (fn->name == "main") unit.hasMain = true;
            FuncSkeleton sk;
            sk.name         = fn->name;
            sk.paramTypes   = typeStringsOf(fn->params);
            sk.returnType   = typeStringOf(fn->returnType.get());
            sk.throws       = fn->throws;
            sk.isPublic     = fn->isPublic;
            // feature-13 C2：扫描态下 body 节点不建，由 bodySkippedByScan 标记「源码有实体 body」。
            sk.hasBody      = (fn->body != nullptr) || fn->bodySkippedByScan;
            sk.hasCppImpl   = fn->hasCppImpl;
            unit.funcs.push_back(std::move(sk));
            continue;
        }

        // ---- 方法（fun (self T) name(...)） ----
        if (auto* md = dynamic_cast<MethodDecl*>(d.get())) {
            FuncSkeleton sk;
            sk.name         = md->name;
            sk.paramTypes   = typeStringsOf(md->params);
            sk.returnType   = typeStringOf(md->returnType.get());
            sk.throws       = md->throws;
            sk.isPublic     = md->isPublic;
            // feature-13 C2：同上（扫描态标记）。
            sk.hasBody      = (md->body != nullptr) || md->bodySkippedByScan;
            sk.hasCppImpl   = md->hasCppImpl;
            sk.receiverType = md->receiverType;
            unit.funcs.push_back(std::move(sk));
            continue;
        }
    }

    return unit;
}
// ============================================================
// feature-13 C3（2026-09-17）：汇总段——声明级扫描表 + 环/拓扑/入口/冲突
//
// 与既有 loadAll/hasCycle/topologicalLayers/validateEntry 并行存在：
//   - 既有 = 完整解析后再判（现状主流程，保持不变）
//   - 本组 = 只用声明级扫描表判定（f13 真增量：依赖图/入口/冲突在完整解析前可用）
// C4 主流程切换到本组；既有方法暂留，收口由后续批次决定（本批不删）。
// ============================================================

// ---- 声明级递归发现（替代 loadAll 的「完整解析 + 递归」中的递归发现部分）----
bool ModuleManager::scanAll(const std::string& entryPath) {
    scanUnits_.units.clear();

    std::vector<std::string> pending;
    std::unordered_set<std::string> visited;

    std::string absEntry = pathString(std::filesystem::absolute(fsPath(entryPath)));
    pending.push_back(absEntry);

    while (!pending.empty()) {
        std::string path = pending.back();
        pending.pop_back();
        if (visited.count(path)) continue;
        visited.insert(path);

        DeclUnit u = scanDeclarations(path);
        // scanDeclarations 已把不可解析 import 等错误记入 diag_；继续收集其余依赖
        for (auto& dep : u.deps) {
            if (!visited.count(dep)) pending.push_back(dep);
        }
        scanUnits_.units[path] = std::move(u);
    }

    return !diag_.hasErrors();
}

// ---- module 名冲突判定（D12 两分，2026-09-16 主人裁定）----
// 显式同 module（多文件均带 module <name>） → 共享 namespace，合法；
// 含任一隐式（无 module 声明回落 stem）同 moduleName        → 冲突（无共享意图）。
bool ModuleManager::checkModuleConflicts() const {
    return checkModuleConflicts(diag_);
}

// feature-21 Phase 2 §3.6：驱动器在每轮 ensureModuleLoaded 后用它复查
// （宏生成模块可能同名/同 ns，只查一次会漏检）。
bool ModuleManager::checkModuleConflicts(DiagnosticEngine& diag) const {
    // moduleName → [(sourcePath, hasExplicitModule)]
    std::unordered_map<std::string, std::vector<std::pair<std::string, bool>>> byName;
    for (auto& [path, u] : scanUnits_.units)
        byName[u.moduleName].push_back({path, u.hasExplicitModule});

    for (auto& [name, items] : byName) {
        if (items.size() < 2) continue;
        bool anyImplicit = false;
        for (auto& [_, ex] : items) if (!ex) { anyImplicit = true; break; }
        if (!anyImplicit) continue;   // 全显式同名：共享 namespace，合法

        std::string msg = "module name conflict: '" + name
                        + "' resolves to multiple files without an explicit `module` declaration:\n";
        for (auto& [p, _] : items) msg += "  " + p + "\n";
        msg += "  → 请在上述文件首行添加显式 `module <name>` 声明（显式同名 = 共享命名空间，合法）；"
               "或给模块起不同的名字。";
        diag.error(0, 0, msg);
        return true;
    }
    return false;
}

// ---- 环检测（DFS 三色，读扫描表；逻辑与 hasCycle 一致）----
bool ModuleManager::hasCycleOn() const {
    enum Color { White, Gray, Black };
    std::unordered_map<std::string, Color> color;
    for (auto& [path, _] : scanUnits_.units) color[path] = White;

    std::function<bool(const std::string&)> dfs = [&](const std::string& path) -> bool {
        color[path] = Gray;
        auto it = scanUnits_.units.find(path);
        if (it == scanUnits_.units.end()) { color[path] = Black; return false; }
        for (auto& dep : it->second.deps) {
            auto ci = color.find(dep);
            if (ci == color.end()) continue;   // 非扫描单元（内置等）不参与
            if (ci->second == Gray) return true;
            if (ci->second == White && dfs(dep)) return true;
        }
        color[path] = Black;
        return false;
    };

    for (auto& [path, _] : scanUnits_.units) {
        if (color[path] == White && dfs(path)) {
            diag_.error(0, 0, "circular dependency detected involving: " + path);
            return true;
        }
    }
    return false;
}

// ---- 拓扑分层（Kahn BFS，读扫描表；返回 path 分组；逻辑与 topologicalLayers 一致）----
std::vector<std::vector<std::string>> ModuleManager::topologicalLayersOn() const {
    std::unordered_map<std::string, int> inDegree;
    std::unordered_map<std::string, std::vector<std::string>> dependents;
    inDegree.reserve(scanUnits_.units.size());
    dependents.reserve(scanUnits_.units.size());
    for (auto& [path, _] : scanUnits_.units) inDegree[path] = 0;
    for (auto& [path, u] : scanUnits_.units) {
        for (auto& dep : u.deps) {
            if (scanUnits_.units.count(dep)) {
                inDegree[path]++;
                dependents[dep].push_back(path);
            }
        }
    }

    std::deque<std::string> queue;
    for (auto& [path, deg] : inDegree) {
        if (deg == 0) queue.push_back(path);
    }

    std::vector<std::vector<std::string>> layers;
    std::unordered_set<std::string> processed;
    while (!queue.empty()) {
        std::vector<std::string> currentLayer;
        size_t layerSize = queue.size();
        for (size_t i = 0; i < layerSize; ++i) {
            std::string path = queue.front();
            queue.pop_front();
            if (processed.count(path)) continue;
            processed.insert(path);
            currentLayer.push_back(path);

            auto dit = dependents.find(path);
            if (dit != dependents.end()) {
                for (auto& dependent : dit->second) {
                    if (processed.count(dependent)) continue;
                    if (--inDegree[dependent] == 0) queue.push_back(dependent);
                }
            }
        }
        layers.push_back(std::move(currentLayer));
    }
    return layers;
}

// ---- 入口验证（读扫描表 hasMain；逻辑与 validateEntry 一致）----
bool ModuleManager::validateEntryOn(std::string& outEntry) const {
    std::string entry;
    int entryCount = 0;
    for (auto& [path, u] : scanUnits_.units) {
        if (u.hasMain) { entry = path; entryCount++; }
    }
    if (entryCount == 0) {
        diag_.error(0, 0, "no entry point found: no module contains 'fun main(io: Io)'");
        return false;
    }
    if (entryCount > 1) {
        diag_.error(0, 0, "multiple entry points found: more than one module defines 'fun main(io: Io)'");
        return false;
    }
    outEntry = entry;
    return true;
}

// ---- 第二段：完整解析扫描表中的每个单元 -> modules_（A2；不递归）----
bool ModuleManager::loadAllScanned() {
    modules_.clear();
    for (auto& [path, _] : scanUnits_.units) {
        ModuleInfo info = parseModule(path);
        if (!info.ast) {
            // parseModule 已把不可解析/读文件失败记入 diag_；继续其余单元
            continue;
        }
        modules_[path] = std::move(info);
    }
    return !diag_.hasErrors();
}

// ============================================================
// feature-21 Phase 2 §3.6：宏生成 import 的按需加载 + AST 刷新
// ============================================================
bool ModuleManager::ensureModuleLoaded(const std::string& absPath) {
    const std::string abs = pathString(std::filesystem::absolute(fsPath(absPath)));
    if (modules_.count(abs)) return true;          // 去重：同一目标只加载一次

    ModuleInfo info = parseModule(abs);
    if (!info.ast) return false;                   // parseModule 已记诊断

    const std::string key = info.sourcePath.empty() ? abs : info.sourcePath;
    auto& u = scanUnits_.units[key];               // 新模块进入扫描表（骨架留空，deps 真实）
    u.moduleName        = info.moduleName;
    u.hasExplicitModule = info.hasExplicitModule;
    rebuildDeclUnit(key, *info.ast, u);

    modules_[key] = std::move(info);
    return true;
}

void ModuleManager::refreshDeclUnitsFromAst() {
    for (auto& [path, mod] : modules_) {
        if (!mod.ast) continue;
        auto& u = scanUnits_.units[path];
        u.moduleName        = mod.moduleName;
        u.hasExplicitModule = mod.hasExplicitModule;
        rebuildDeclUnit(path, *mod.ast, u);
    }
}

// ============================================================
// 路径解析
// ============================================================
std::string ModuleManager::resolveImportPath(const std::string& importPath,
                                              const std::string& importerDir) const {
    namespace fs = std::filesystem;

    // bug-109：目录来自 argv（CP_ACP）；import 名可能含非 ASCII 且来自 **UTF-8 源码内容**
    //   ⇒ 对同一名字分别按 ACP / UTF-8 各试一轮（ASCII 下两者恒等 ⇒ 零回归）。
    const fs::path impCandidates[2] = { fsPath(importPath), fsPathUtf8(importPath) };
    const fs::path dir = fsPath(importerDir);
    for (const fs::path& imp : impCandidates) {
        if (imp.empty()) continue;

        // ① 相对导入者目录
        fs::path rel = dir / imp;
        if (fs::exists(rel)) return pathString(fs::absolute(rel));
        rel += ".aura";                       // 自动补扩展名
        if (fs::exists(rel)) return pathString(fs::absolute(rel));

        // ② 作为绝对路径
        fs::path abs = imp;
        if (fs::exists(abs)) return pathString(fs::absolute(abs));
        abs += ".aura";
        if (fs::exists(abs)) return pathString(fs::absolute(abs));
    }

    return {};
}

// ============================================================
// 加载 .aurai 内置接口声明
//   io.aurai   → 始终加载（语言级内置能力）
//   path.aurai → 按需加载（遇到 import path 时）
// ============================================================
void ModuleManager::loadAuraiFile(const std::string& baseName) {
    namespace fs = std::filesystem;
    // bug-109：cwd 可能含非 ASCII；`.string()` 在 Windows 返回 UTF-8（≠ argv 的 CP_ACP）
    // ⇒ 用 pathString 保持与 readFile(ifstream) 同口径。
    fs::path auraiPath = fs::current_path() / "builtins" / fsPath(baseName);
    if (!fs::exists(auraiPath)) return;
    std::string src = readFile(pathString(auraiPath));
    if (src.empty()) return;

    Lexer lexer(src);
    auto tokens = lexer.scanAll();

    DiagnosticEngine dummyDiag;  // .aurai 解析错误不影响主流程
    Parser parser(std::move(tokens), dummyDiag);
    auto ast = parser.parseAurai();
    if (ast) {
        auto& reg = BuiltinRegistry::get();
        reg.tryLoadAurai(baseName, *ast);
    }
}

void ModuleManager::loadBuiltinAurai() {
    loadAuraiFile("io.aurai");
    loadAuraiFile("builtin.aurai");  // 基础内置全局函数（int/float/str/gc_*）
    loadAuraiFile("interfaces.aurai");  // 内置接口（Stringer/Comparable/Iterator）
    // path.aurai 不在此加载——由 import path 时按需加载
}

// ============================================================
// 递归加载
// ============================================================
bool ModuleManager::loadAll(const std::string& entryPath) {
    // 用栈模拟递归，避免深层递归爆栈
    std::vector<std::string> pending;
    std::unordered_set<std::string> visited;

    // 解析入口文件为绝对路径
    std::string absEntry = pathString(std::filesystem::absolute(fsPath(entryPath)));
    pending.push_back(absEntry);

    while (!pending.empty()) {
        std::string path = pending.back();
        pending.pop_back();

        if (visited.count(path)) continue;
        visited.insert(path);

        ModuleInfo info = parseModule(path);
        if (!info.ast) {
            diag_.error(0, 0, "failed to parse: " + path);
            return false;
        }

        // 将未访问的依赖加入待处理队列
        for (auto& dep : info.deps) {
            if (!visited.count(dep)) {
                pending.push_back(dep);
            }
        }

        modules_[path] = std::move(info);
    }

    return !diag_.hasErrors();
}

// ============================================================
// 循环检测（DFS 三色标记）
// ============================================================
bool ModuleManager::hasCycle() {
    enum Color { White, Gray, Black };
    std::unordered_map<std::string, Color> color;
    for (auto& [path, info] : modules_) color[path] = White;

    std::function<bool(const std::string&)> dfs = [&](const std::string& path) -> bool {
        color[path] = Gray;
        auto it = modules_.find(path);
        if (it == modules_.end()) { color[path] = Black; return false; }
        for (auto& dep : it->second.deps) {
            auto ci = color.find(dep);
            if (ci == color.end()) continue; // 内置模块不参与检测
            if (ci->second == Gray) return true;  // 回边 → 环
            if (ci->second == White && dfs(dep)) return true;
        }
        color[path] = Black;
        return false;
    };

    for (auto& [path, _] : modules_) {
        if (color[path] == White && dfs(path)) {
            diag_.error(0, 0, "circular dependency detected involving: " + path);
            return true;
        }
    }
    return false;
}

// ============================================================
// 拓扑分层（Kahn BFS）
// ============================================================
std::vector<std::vector<ModuleInfo*>> ModuleManager::topologicalLayers() {
    // 入度：A depends on B → B 应先编译，A 的入度+1（修正方向：被依赖者先编译，
    // 依赖者入度 +1，入度为 0 者无未编译依赖，可入队）
    std::unordered_map<std::string, int> inDegree;
    inDegree.reserve(modules_.size());
    // 反向邻接（dependents）：dep → 依赖 dep 的模块列表，供 O(1) 后继查找，
    // 替代原先对每个 path 全量扫描 modules_ 找依赖者的 O(n^2) 实现
    std::unordered_map<std::string, std::vector<std::string>> dependents;
    dependents.reserve(modules_.size());
    for (auto& [path, _] : modules_) inDegree[path] = 0;
    for (auto& [path, info] : modules_) {
        for (auto& dep : info.deps) {
            if (modules_.count(dep)) {
                inDegree[path]++;                 // path 依赖 dep → path 入度+1
                dependents[dep].push_back(path);  // dep 的依赖者集合含 path
            }
        }
    }

    // Kahn BFS 分层：入度为 0 的模块入队，处理后将依赖者入度-1
    std::deque<std::string> queue;
    for (auto& [path, deg] : inDegree) {
        if (deg == 0) queue.push_back(path);
    }

    std::vector<std::vector<ModuleInfo*>> layers;
    std::unordered_set<std::string> processed;

    while (!queue.empty()) {
        std::vector<ModuleInfo*> currentLayer;
        size_t layerSize = queue.size();
        for (size_t i = 0; i < layerSize; ++i) {
            std::string path = queue.front();
            queue.pop_front();
            if (processed.count(path)) continue;
            processed.insert(path);

            auto it = modules_.find(path);
            if (it == modules_.end()) continue;
            currentLayer.push_back(&it->second);
            it->second.layer = static_cast<int>(layers.size());

            // path 编译后，依赖 path 的模块入度-1；用 dependents 反向表 O(1) 查后继
            auto dit = dependents.find(path);
            if (dit != dependents.end()) {
                for (auto& dependent : dit->second) {
                    if (processed.count(dependent)) continue;
                    if (--inDegree[dependent] == 0) {
                        queue.push_back(dependent);
                    }
                }
            }
        }
        layers.push_back(std::move(currentLayer));
    }

    return layers;
}

// ============================================================
// 入口点验证
// ============================================================
bool ModuleManager::validateEntry(std::string& outEntryModule) {
    std::string entry;
    int entryCount = 0;

    for (auto& [path, info] : modules_) {
        if (info.hasMain) {
            entry = path;
            entryCount++;
        }
    }

    if (entryCount == 0) {
        diag_.error(0, 0, "no entry point found: no module contains 'fun main(io: Io)'");
        return false;
    }
    if (entryCount > 1) {
        diag_.error(0, 0, "multiple entry points found: more than one module defines 'fun main(io: Io)'");
        return false;
    }

    outEntryModule = entry;
    return true;
}

} // namespace Aura
