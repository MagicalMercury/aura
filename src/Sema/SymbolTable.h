#pragma once

#include "Symbol.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Aura {

// ============================================================
// Scope — 一个作用域层
// ============================================================

enum class ScopeKind {
    Global,   // 文件顶层
    Function, // 函数体
    Block,    // { ... } / if / while / loop / for / match / sync / spawn / try/catch
};

class Scope {
public:
    explicit Scope(ScopeKind kind = ScopeKind::Block, Scope* parent = nullptr)
        : kind_(kind), parent_(parent) {}

    ScopeKind kind() const { return kind_; }
    Scope*   parent() const { return parent_; }

    // 在本层作用域定义符号。如果已存在同名符号，返回 false
    bool define(Symbol sym);

    // 在本层查找（不向上搜索）
    Symbol* lookupLocal(const std::string& name) const;

    // 向上搜索所有父作用域
    Symbol* lookup(const std::string& name) const;

    // 遍历本层所有符号
    template<typename Fn>
    void forEach(Fn&& fn) const {
        for (auto& [name, sym] : symbols_) {
            fn(name, sym);
        }
    }

private:
    ScopeKind kind_;
    Scope* parent_ = nullptr;
    std::unordered_map<std::string, Symbol> symbols_;
};

// ============================================================
// SymbolTable — 多层作用域管理器
// ============================================================

class SymbolTable {
public:
    SymbolTable();

    // 进入新作用域
    void enterScope(ScopeKind kind = ScopeKind::Block);

    // 退出当前作用域（不允许退出全局作用域）
    void exitScope();

    // 在当前作用域定义符号
    bool define(Symbol sym);

    // 在全局作用域定义符号
    bool defineGlobal(Symbol sym);

    // 从当前作用域向上搜索
    Symbol* lookup(const std::string& name) const;

    // 只在全局作用域查找
    Symbol* lookupGlobal(const std::string& name) const;

    // 当前作用域指针
    Scope* currentScope() { return current_; }
    const Scope* currentScope() const { return current_; }

    // 所有作用域（用于调试）
    const std::vector<std::unique_ptr<Scope>>& allScopes() const { return scopes_; }

private:
    std::vector<std::unique_ptr<Scope>> scopes_;
    Scope* current_ = nullptr;
};

} // namespace Aura
