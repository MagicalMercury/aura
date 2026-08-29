#include "Sema/SemAnalyzer.h"

namespace Aura {

// ============================================================
// P5：match 常量模式辅助
// ============================================================

// 常量字面量 → 语义类型（int/float/bool/string/None；其他 → nullptr）
static std::unique_ptr<SemType> literalSemType(const ASTNode& lit) {
    if (dynamic_cast<const IntLiteral*>(&lit))    return intType();
    if (dynamic_cast<const FloatLiteral*>(&lit))  return floatType();
    if (dynamic_cast<const BoolLiteral*>(&lit))   return boolType();
    if (dynamic_cast<const StringLiteral*>(&lit)) return stringType();
    if (dynamic_cast<const NoneLiteral*>(&lit))   return NoneSemType::make();
    return nullptr;
}

// 常量字面量 → 去重键（重复常量检测）
static std::string literalKey(const ASTNode& lit) {
    if (auto* i = dynamic_cast<const IntLiteral*>(&lit))    return "i:" + std::to_string(i->value);
    if (auto* f = dynamic_cast<const FloatLiteral*>(&lit))  return "f:" + std::to_string(f->value);
    if (auto* b = dynamic_cast<const BoolLiteral*>(&lit))   return std::string("b:") + (b->value ? "1" : "0");
    if (auto* s = dynamic_cast<const StringLiteral*>(&lit)) return "s:" + s->value;
    if (dynamic_cast<const NoneLiteral*>(&lit))             return "none";
    return "";
}

// 字面量类型 → C++ 名（显式 Optional 元素常量判定用；未识别 → 空串）
static std::string literalCppName(const SemType& litTy) {
    if (auto* p = dynamic_cast<const PrimSemType*>(&litTy)) {
        switch (p->kind) {
            case PrimSemType::Int:    return "int32_t";
            case PrimSemType::Float:  return "double";
            case PrimSemType::Bool:   return "bool";
            case PrimSemType::String: return "aura_rt::GcString*";
        }
    }
    return "";
}

// 常量类型是否与 matchedType 兼容（非联合 equals / 联合存在变体 / Optional 元素或 None）
static bool constCompatibleWith(const SemType& matched, const SemType& litTy) {
    if (auto* u = dynamic_cast<const UnionSemType*>(&matched)) {
        for (auto& v : u->variants)
            if (v && v->equals(litTy)) return true;
        return false;
    }
    if (auto* o = dynamic_cast<const OptionalSemType*>(&matched)) {
        if (dynamic_cast<const NoneSemType*>(&litTy)) return true;
        return o->elementType && o->elementType->equals(litTy);
    }
    // 显式 `Optional<T>` 注解（GenericSemType{name=="Optional"}，resolvedName 如
    // "aura_rt::Optional<int32_t>"）：与 OptionalSemType 分支对称——None 恒兼容；
    // 元素常量从 resolvedName 提取元素 C++ 名与字面量 C++ 名比较（Optional<int> 写 5=>）
    if (auto* gs = dynamic_cast<const GenericSemType*>(&matched)) {
        if (gs->name == "Optional") {
            if (dynamic_cast<const NoneSemType*>(&litTy)) return true;
            std::string want = literalCppName(litTy);
            if (!want.empty() && !gs->resolvedName.empty()) {
                auto lt = gs->resolvedName.find('<');
                auto rt = gs->resolvedName.rfind('>');
                if (lt != std::string::npos && rt != std::string::npos && rt > lt) {
                    std::string elem = gs->resolvedName.substr(lt + 1, rt - lt - 1);
                    if (elem == want) return true;
                }
            }
            return false;
        }
    }
    return matched.equals(litTy);
}

void SemAnalyzer::checkReturnStmt(const ReturnStmt& stmt) {
    if (stmt.expr) {
        // 传当前函数返回类型作为期望类型：支持 return fun(msg){...} 从返回类型反推闭包参数
        auto retType = inferExpr(*stmt.expr,
                                 currentReturnType_ ? currentReturnType_.get() : nullptr);
        // 存储推断类型到 typeStore_，标注表达式 AST 节点供 CodeGen 使用
        typeStore_.push_back(std::move(retType));
        const_cast<ReturnStmt&>(stmt).inferredType = typeStore_.back().get();
        // 标注 return 表达式自身
        if (auto* rec = dynamic_cast<const RecordExpr*>(stmt.expr.get())) {
            const_cast<RecordExpr*>(rec)->inferredType = typeStore_.back().get();
        } else {
            const_cast<ASTNode*>(stmt.expr.get())->inferredType = typeStore_.back().get();
        }
        bool mismatch = currentReturnType_
                     && !isAssignable(*currentReturnType_, *typeStore_.back());
        if (mismatch) {
            error(*stmt.expr, "return type mismatch: expected '" + currentReturnType_->toString() + "', got '" + typeStore_.back()->toString() + "'");
        } else if (!diag_.hasErrors() && containsErrorElement(typeStore_.back().get())) {
            // 返回表达式元素类型不可解析（[] / none() 无上下文，且无法从返回类型反推）
            // 或错误类型被 isAssignable 静默放行（Error 元素）→ 干净报错，避免 error_type 泄漏到 CodeGen
            // （!diag_.hasErrors() 守卫：返回表达式自身已报错时不再叠加）
            error(*stmt.expr, "cannot infer return value type; add explicit return type annotation (e.g. -> [int])");
        } else if (!diag_.hasErrors() && currentReturnType_ && stmt.expr) {
            // #10：return 上下文字段值 canonical 下钻（对齐 checkLetDecl 的
            // propagateCanonicalName 调用）：否则返回 record 字面量的字段值
            // （如 p: Point|None 字段的 {x=1,y=2}）canonicalName 不传播，
            // genRecordExpr 退化为 designated initializer、genUnionBoxing 无法定位变体。
            // 注意：currentReturnType_ 是函数级临时（FnCtxGuard 持有），函数检查结束即
            // 释放；必须 clone 进 typeStore_ 保活再传，否则 inferredType 悬垂（CodeGen
            // 读到垃圾类型）。checkLetDecl 对应路径用 sym->type（symbol 表保活）同理。
            typeStore_.push_back(currentReturnType_->clone());
            propagateCanonicalName(*stmt.expr, typeStore_.back().get());
        }
    }
    // L3: lock 块内禁止 return 跨出
    if (inLockBlock_) {
        error(stmt, "cannot return out of lock block");
    }
    // sync/spawn 块内禁止 return 跨出（跳过 when_all / waitGroup）
    if (!syncBoundaryStack_.empty()) {
        error(stmt, "cannot return out of " + syncBoundaryStack_.back().kind + " block");
    }
    // 无表达式的 return 允许（void 等价）
}

void SemAnalyzer::checkThrowStmt(const ThrowStmt& stmt) {
    if (!currentFunctionThrows_ && insideTry_ == 0) {
        error(stmt, "'throw' used in non-throwing function");
    }
    if (stmt.expr) {
        // throw { kind=..., message=... }：结构化异常 record——CodeGen 直接提取
        // kind/message 生成 aura_rt::Error（不依赖 canonicalName / 类型匹配），
        // throw 自身即上下文 → 不触发决策 A 的无上下文 record 报错；仅推断字段值
        // 供 GcRootHandle 包装判断。
        if (auto* rec = dynamic_cast<const RecordExpr*>(stmt.expr.get())) {
            for (auto& f : rec->fields)
                if (f.value) (void)inferExpr(*f.value);
        } else {
            (void)inferExpr(*stmt.expr);
        }
    }
}

void SemAnalyzer::checkIfStmt(const IfStmt& stmt) {
    if (stmt.condition) {
        auto condType = inferExpr(*stmt.condition);
        if (!isAssignable(*boolType(), *condType)) {
            error(*stmt.condition, "if condition must be bool, got '" + condType->toString() + "'");
        }
    } else {
        // 尽力模式防御：`if { ... }`（parseExpr 遇不可解析 token 返回 null）→
        // 干净报错而非 inferExpr(*stmt.condition) 空指针崩溃（同 checkReturnStmt 防御）
        error(stmt, "expected expression after 'if'");
    }
    if (stmt.thenBranch) checkBlock(*stmt.thenBranch);
    for (auto& ei : stmt.elseIfs) {
        if (ei.condition) {
            auto eic = inferExpr(*ei.condition);
            if (!isAssignable(*boolType(), *eic)) {
                error(*ei.condition, "'else if' condition must be bool, got '" + eic->toString() + "'");
            }
        } else {
            error(stmt, "'else if' missing condition expression");
        }
        if (ei.body) checkBlock(*ei.body);
    }
    if (stmt.elseBranch) checkBlock(*stmt.elseBranch);
}

void SemAnalyzer::checkWhileStmt(const WhileStmt& stmt) {
    if (stmt.condition) {
        auto condType = inferExpr(*stmt.condition);
        if (!isAssignable(*boolType(), *condType)) {
            error(*stmt.condition, "while condition must be bool, got '" + condType->toString() + "'");
        }
    } else {
        // 尽力模式防御：`while { ... }`（空 condition）→ 干净报错而非崩溃
        error(stmt, "expected expression after 'while'");
    }
    ScopedValue<int> guard(loopDepth_, loopDepth_ + 1);
    if (stmt.body) checkBlock(*stmt.body);
}

void SemAnalyzer::checkForStmt(const ForStmt& stmt) {
    if (!stmt.iterable) {
        // 尽力模式防御：`for x in { ... }`（parseExpr 返回 null）→ 干净报错而非崩溃；
        // itemName 元素类型依赖 iterType 无法推导，跳过 body 检查
        error(stmt, "expected expression after 'for'");
        return;
    }
    auto iterType = inferExpr(*stmt.iterable);
    // A4：无标注 channel（元素不可知）的 for-in 与 receive/send 对齐报错——
    // 裸 GenericSemType{channel/sync.Channel} 无 resolvedName（未标注 <T>），
    // for 元素类型无从推导，须显式标注
    if (auto* g = dynamic_cast<const GenericSemType*>(iterType.get())) {
        if ((g->name == "channel" || g->name == "sync.Channel") && g->resolvedName.empty()) {
            error(stmt, "cannot infer element type of '" + g->name
                  + "'; add explicit type annotation (e.g. " + g->name + "<int>)");
        }
    }
    // 迭代类型默认合法（运行时检查），这里只确保表达式无错误
    ScopedValue<int> loopGuard(loopDepth_, loopDepth_ + 1);
    symtab_.enterScope();
    Symbol sym;
    sym.kind = SymKind::Variable;
    sym.name = stmt.itemName;
    // 从列表/迭代器/泛型通道类型推导元素类型（elemTypeOf 统一处理）
    sym.type = elemTypeOf(iterType.get());
    symtab_.define(std::move(sym));
    if (stmt.body) checkBlock(*stmt.body);
    symtab_.exitScope();
}

void SemAnalyzer::checkLoopStmt(const LoopStmt& stmt) {
    ScopedValue<int> guard(loopDepth_, loopDepth_ + 1);
    if (stmt.body) checkBlock(*stmt.body);
}

void SemAnalyzer::checkMatchStmt(const MatchStmt& stmt) {
    // 尽力模式防御：`match { ... }` / `(x match {...})` 残留的 `match {`（parseMatchStmt
    // 不检查 parseExpr 返回值，产出 expr=null 的 MatchStmt）→ 干净报错而非
    // inferExpr(*stmt.expr) 空指针崩溃（checkReturnStmt/checkThrowStmt 同款防御）
    if (!stmt.expr) {
        error(stmt, "expected expression after 'match'");
        return;
    }
    auto matchedType = inferExpr(*stmt.expr);

    // P5：重复常量检测 + 每个 case 首个字面量记录（供可达性检查）
    std::set<std::string> seenConstants;
    std::vector<const ASTNode*> caseLiterals;  // 与 stmt.cases 并行；无常量 case → nullptr

    for (size_t ci = 0; ci < stmt.cases.size(); ++ci) {
        auto& c = stmt.cases[ci];

        // 收集当前 case 的所有常量字面量（单常量或 `|` 分组）
        std::vector<const ASTNode*> lits;
        if (auto* cp = dynamic_cast<const ConstantPattern*>(c.pattern.get())) {
            if (cp->value) lits.push_back(cp->value.get());
        } else if (auto* gp = dynamic_cast<const GroupPattern*>(c.pattern.get())) {
            for (auto& a : gp->alts)
                if (auto* ap = dynamic_cast<const ConstantPattern*>(a.get()))
                    if (ap->value) lits.push_back(ap->value.get());
        }
        caseLiterals.push_back(lits.empty() ? nullptr : lits[0]);

        for (auto* lit : lits) {
            // 类型兼容：字面量类型须与 matchedType 匹配（非联合 equals / 联合存在变体）
            auto litTy = literalSemType(*lit);
            if (matchedType && litTy && !constCompatibleWith(*matchedType, *litTy)) {
                error(*lit, "match constant type '" + litTy->toString() +
                            "' does not match the matched type '" +
                            matchedType->toString() + "'");
            }
            // 重复常量：同一 match 内相同字面量出现多次 → 编译错（对齐 C++ duplicate case）
            std::string key = literalKey(*lit);
            if (!key.empty() && !seenConstants.insert(key).second) {
                error(*lit, "duplicate match constant; each value may appear only once");
            }
        }

        // TypePattern 中引入变量
        if (auto* tp = dynamic_cast<const TypePattern*>(c.pattern.get())) {
            symtab_.enterScope();
            if (!tp->varName.empty()) {
                Symbol sym;
                sym.kind = SymKind::Variable;
                sym.name = tp->varName;
                sym.type = resolveNamedType(tp->typeName);
                symtab_.define(std::move(sym));
            }
        }
        // 检查 body
        if (c.body) {
            // body 可能是 BlockStmt 或表达式
            if (auto* b = dynamic_cast<const BlockStmt*>(c.body.get())) {
                checkBlock(*b);
            } else {
                auto _ = inferExpr(*c.body);
            }
        }
        if (auto* tp = dynamic_cast<const TypePattern*>(c.pattern.get())) {
            if (!tp->varName.empty()) symtab_.exitScope();
        }
    }

    // P5：可达性警告——常量在其对应类型的 TypePattern 之后（该变体已被全量吞掉）
    if (matchedType) {
        for (size_t ci = 0; ci < stmt.cases.size(); ++ci) {
            const ASTNode* lit = caseLiterals[ci];
            if (!lit) continue;
            auto litTy = literalSemType(*lit);
            if (!litTy) continue;
            bool shadowed = false;
            for (size_t j = 0; j < ci; ++j) {
                if (auto* tp = dynamic_cast<const TypePattern*>(stmt.cases[j].pattern.get())) {
                    auto resolved = resolveNamedType(tp->typeName);
                    if (resolved && resolved->equals(*litTy)) { shadowed = true; break; }
                }
            }
            if (shadowed) {
                diag_.warn(lit->line, lit->col,
                           "match constant is unreachable: variant already fully covered by a preceding type pattern");
            }
        }
    }

    if (matchedType && !isMatchExhaustive(*matchedType, stmt.cases)) {
        error(stmt, DiagCode::E014_MatchNotExhaustive,
              "match is not exhaustive: not all variants are covered");
    }
}

void SemAnalyzer::checkTryCatchStmt(const TryCatchStmt& stmt) {
    insideTry_++;
    if (stmt.tryBody) checkBlock(*stmt.tryBody);
    insideTry_--;
    symtab_.enterScope();
    Symbol sym;
    sym.kind = SymKind::Variable;
    sym.name = stmt.catchVar;
    sym.type = ErrorSemType::make(); // Error 类型（后续改为具体 Error 类型）
    symtab_.define(std::move(sym));
    if (stmt.catchBody) checkBlock(*stmt.catchBody);
    symtab_.exitScope();
}

} // namespace Aura
