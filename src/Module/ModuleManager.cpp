// ============================================================
// ModuleManager.cpp — Aura 多文件模块管理器实现
// ============================================================

#include "ModuleManager.h"
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

namespace Aura {

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
    return fs::path(path).stem().string();
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

std::string ModuleManager::pathToNs(const std::string& path) {
    namespace fs = std::filesystem;
    // 去掉扩展名，路径分隔符 → _
    std::string stem = fs::path(path).stem().string();
    // 将完整路径转为标识符
    std::string absPath = fs::absolute(path).string();
    // 取父目录路径 + 文件名（不含扩展名）
    std::string parent = fs::absolute(path).parent_path().string();
    std::string combined = parent + "/" + stem;
    // 统一分隔符
    std::replace(combined.begin(), combined.end(), '\\', '/');
    // 只保留相对于工作目录的路径，简化命名空间
    // 策略：用文件路径的唯一标识
    return "aura_mod_" + sanitizeId(stem);
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
    info.sourcePath = std::filesystem::absolute(sourcePath).string();
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

    // 提取 import 依赖 和 has_main
    for (auto& d : info.ast->decls) {
        if (!d) continue;

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
                std::string importerDir = std::filesystem::path(sourcePath).parent_path().string();
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
// 路径解析
// ============================================================
std::string ModuleManager::resolveImportPath(const std::string& importPath,
                                              const std::string& importerDir) const {
    namespace fs = std::filesystem;

    // 先尝试相对路径
    fs::path rel = fs::path(importerDir) / importPath;
    if (fs::exists(rel)) return fs::absolute(rel).string();

    // 自动补 .aura 扩展名
    rel += ".aura";
    if (fs::exists(rel)) return fs::absolute(rel).string();

    // 再尝试作为绝对路径
    fs::path abs = importPath;
    if (fs::exists(abs)) return fs::absolute(abs).string();
    abs += ".aura";
    if (fs::exists(abs)) return fs::absolute(abs).string();

    return {};
}

// ============================================================
// 加载 .aurai 内置接口声明
//   io.aurai   → 始终加载（语言级内置能力）
//   path.aurai → 按需加载（遇到 import path 时）
// ============================================================
void ModuleManager::loadAuraiFile(const std::string& baseName) {
    namespace fs = std::filesystem;
    fs::path auraiPath = fs::current_path() / "builtins" / baseName;
    if (!fs::exists(auraiPath)) return;
    std::string src = readFile(auraiPath.string());
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
    std::string absEntry = std::filesystem::absolute(entryPath).string();
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
    // 计算入度（只计用户模块依赖）
    std::unordered_map<std::string, int> inDegree;
    for (auto& [path, info] : modules_) {
        if (!inDegree.count(path)) inDegree[path] = 0;
        for (auto& dep : info.deps) {
            if (modules_.count(dep)) inDegree[dep]++;  // dep 被 path 依赖 → dep 的入度+1
            if (!inDegree.count(path)) inDegree[path] = 0;
        }
    }

    // 修正入度计算方向：如果 A depends on B，B 应先编译，A 的入度+1
    // 重新计算
    inDegree.clear();
    for (auto& [path, _] : modules_) inDegree[path] = 0;
    for (auto& [path, info] : modules_) {
        for (auto& dep : info.deps) {
            if (modules_.count(dep)) {
                inDegree[path]++; // path 依赖 dep → path 的入度+1
            }
        }
    }

    // BFS 分层
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

            // path 被编译后，依赖 path 的模块入度-1
            for (auto& [otherPath, otherInfo] : modules_) {
                if (processed.count(otherPath)) continue;
                bool dependsOnPath = false;
                for (auto& dep : otherInfo.deps) {
                    if (dep == path) { dependsOnPath = true; break; }
                }
                if (dependsOnPath) {
                    inDegree[otherPath]--;
                    if (inDegree[otherPath] == 0 && !processed.count(otherPath)) {
                        queue.push_back(otherPath);
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
