#include "SymbolTable.h"

namespace Aura {

// ============================================================
// Scope
// ============================================================

bool Scope::define(Symbol sym) {
    std::string name = sym.name;
    if (sym.kind == SymKind::Function || sym.kind == SymKind::Method) {
        auto it = symbols_.find(name);
        if (it != symbols_.end()) {
            if (it->second.kind == sym.kind) {
                return false;  // 同种类 → 不覆盖
            }
            // 构造函数 Method 与 TypeAlias 同名 → 存到 TypeAlias 符号上
            if (sym.kind == SymKind::Method && it->second.kind == SymKind::TypeAlias
                && sym.name == name) {
                it->second.ctorParams = std::move(sym.params);
                it->second.ctorReturnType = std::move(sym.type);
                return true;
            }
            // 其他不同种类 → 不覆盖
            return false;
        }
    } else {
        if (symbols_.contains(name)) return false;
    }
    symbols_[name] = std::move(sym);
    return true;
}

Symbol* Scope::lookupLocal(const std::string& name) const {
    auto it = symbols_.find(name);
    if (it != symbols_.end()) return const_cast<Symbol*>(&it->second);
    return nullptr;
}

Symbol* Scope::lookup(const std::string& name) const {
    const Scope* scope = this;
    while (scope) {
        auto* sym = scope->lookupLocal(name);
        if (sym) return sym;
        scope = scope->parent();
    }
    return nullptr;
}

// ============================================================
// SymbolTable
// ============================================================

SymbolTable::SymbolTable() {
    // 创建全局作用域
    auto globalScope = std::make_unique<Scope>(ScopeKind::Global, nullptr);
    current_ = globalScope.get();
    scopes_.push_back(std::move(globalScope));
}

void SymbolTable::enterScope(ScopeKind kind) {
    auto newScope = std::make_unique<Scope>(kind, current_);
    current_ = newScope.get();
    scopes_.push_back(std::move(newScope));
}

void SymbolTable::exitScope() {
    // 不允许退出全局作用域
    if (current_ && current_->kind() == ScopeKind::Global) {
        return;
    }
    if (current_) {
        current_ = current_->parent();
    }
    // 注意：不删除 scope 对象本身，保留在 scopes_ 向量中供调试
}

bool SymbolTable::define(Symbol sym) {
    return current_->define(std::move(sym));
}

bool SymbolTable::defineGlobal(Symbol sym) {
    // 找全局作用域
    for (auto& scope : scopes_) {
        if (scope->kind() == ScopeKind::Global) {
            return scope->define(std::move(sym));
        }
    }
    return false;
}

Symbol* SymbolTable::lookup(const std::string& name) const {
    return current_->lookup(name);
}

Symbol* SymbolTable::lookupGlobal(const std::string& name) const {
    for (auto& scope : scopes_) {
        if (scope->kind() == ScopeKind::Global) {
            return scope->lookupLocal(name);
        }
    }
    return nullptr;
}

} // namespace Aura
