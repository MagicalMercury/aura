#include "SemAnalyzer.h"

namespace Aura {

// ============================================================
// canonicalName 传播（RecordSemType → 嵌套 RecordExpr AST）
// ============================================================

bool SemAnalyzer::isRecordLiteralArg(const ASTNode& arg) {
    // 决策 A：表达式上下文（函数/方法/接口实参、赋值、条件分支等）中，匿名 record
    // 字面量需要期望类型才能解析。判断表达式是否含匿名 record（RecordExpr /
    // some(RecordExpr) / 列表元素含 record）——propagateCanonicalName 的 ListExpr
    // 分支会逐元素下钻，故列表实参（take_list([{..}])）也需命中。
    if (dynamic_cast<const RecordExpr*>(&arg)) return true;
    if (auto* call = dynamic_cast<const CallExpr*>(&arg)) {
        // #6：some(ListExpr) 也需命中——列表实参（take_list([{..}])）本身已由下方
        // ListExpr 分支命中；但 some([{..}]) 的 ListExpr 在 CallExpr 内部，必须先在此
        // 解出再按列表元素判定（含空列表：仍返回 true 以便带期望推断元素类型）。
        if (call->args.size() == 1 && call->args[0]
            && (dynamic_cast<const RecordExpr*>(call->args[0].get())
                || (dynamic_cast<const ListExpr*>(call->args[0].get())))) {
            if (auto* id = dynamic_cast<const Identifier*>(call->callee.get()))
                return id->name == "some";
        }
        return false;
    }
    if (auto* list = dynamic_cast<const ListExpr*>(&arg)) {
        for (auto& el : list->elements)
            if (el && isRecordLiteralArg(*el)) return true;
    }
    return false;
}

std::unique_ptr<SemType> SemAnalyzer::semTypeFromBuiltinParam(
    const std::string& typeName, const SemType* objType) {
    // 内置方法形参类型名 → SemType：
    //   - "T"：元素类型占位（如 [T].append(T)），从 objType 元素类型提取
    //   - 基础/内置类型名（"string"/"int" 等）：直接映射
    //   - 其余（不可解析）：返回 nullptr（调用点不传期望）
    if (typeName == "T") {
        if (!objType) return nullptr;
        auto elem = elemTypeOf(objType);
        if (dynamic_cast<const ErrorSemType*>(elem.get())) return nullptr;
        return elem;
    }
    auto named = semTypeFromAuraName(typeName);
    if (dynamic_cast<const ErrorSemType*>(named.get())) return nullptr;
    return named;
}

// ============================================================
// propagateCanonicalName — 期望类型 canonicalName 下钻
// ============================================================

namespace {
// Phase 2-③：判断 some(...) 实参是否可下钻（canonicalName 传播）。
// 除 RecordExpr/ListExpr 外，嵌套 some(...) 调用同样可下钻——否则
// `Optional<Optional<Point>> = some(some({..}))` 的内层 record canonicalName 缺失，
// genRecordExpr 退化为 designated initializer（坏 C++）。递归下钻天然支持任意深度。
static bool isSomeDescendArg(const ASTNode* arg) {
    if (!arg) return false;
    if (dynamic_cast<const RecordExpr*>(arg) || dynamic_cast<const ListExpr*>(arg))
        return true;
    if (auto* call = dynamic_cast<const CallExpr*>(arg)) {
        auto* cid = dynamic_cast<const Identifier*>(call->callee.get());
        return cid && cid->name == "some";
    }
    return false;
}
} // namespace

void SemAnalyzer::propagateCanonicalName(const ASTNode& expr, const SemType* type) {
    if (!type) return;

    // GenericSemType：通过符号表解析回具体类型（如 Tree → RecordSemType）
    if (auto* gs = dynamic_cast<const GenericSemType*>(type)) {
        // 已解析的自引用（如 Tree<int32_t>）：用模板体逐字段传播到嵌套 RecordExpr
        if (!gs->resolvedName.empty()) {
            if (auto* recExpr = dynamic_cast<const RecordExpr*>(&expr)) {
                auto* sym = symtab_.lookup(gs->name);
                if (sym && sym->kind == SymKind::TypeAlias && sym->type) {
                    auto resolved = sym->type->clone();
                    // 对模板副本做 sealSelfRefs，用 resolvedName 标注所有自引用
                    sealSelfRefs(resolved, gs->name, gs->resolvedName);
                    if (auto* rs = dynamic_cast<RecordSemType*>(resolved.get())) {
                        rs->canonicalName = gs->resolvedName;
                        const_cast<RecordExpr*>(recExpr)->inferredType = rs;
                        for (auto& f : recExpr->fields) {
                            for (auto& ft : rs->fields) {
                                if (f.name == ft.name && f.value && ft.type) {
                                    propagateCanonicalName(*f.value, ft.type.get());
                                    break;
                                }
                            }
                        }
                        typeStore_.push_back(std::move(resolved));
                        return;
                    }
                }
            }
            // #1：显式 Optional<X> 注解的 some(record) 实参下钻（与 OptionalSemType
            // 分支对齐）：否则 some({...}) 的 record 字面量实参 canonicalName
            // 不传播，genRecordExpr 退化为 designated initializer。从 resolvedName
            // 提取元素类型（RecordSemType/GenericSemType 均可经符号表解析）下钻。
            // #6：some(ListExpr) 同样下钻（elemTy 为 ListSemType，复用 ListExpr 分支
            // 逐元素传播），否则 `Optional<[Point]> = some([{..}])` 列表元素 canonicalName
            // 缺失。
            // Phase 2-③：some(some(...)) 嵌套同样下钻（isSomeDescendArg）——否则内层
            // record canonicalName 缺失，genRecordExpr 退化为 designated init。
            if (gs->name == "Optional") {
                if (auto* call = dynamic_cast<const CallExpr*>(&expr);
                    call && call->args.size() == 1 && call->args[0]
                    && isSomeDescendArg(call->args[0].get())) {
                    if (auto* id = dynamic_cast<const Identifier*>(call->callee.get());
                        id && id->name == "some") {
                        auto elemTy = elemTypeOf(type);
                        if (!dynamic_cast<const ErrorSemType*>(elemTy.get())) {
                            // 保活后下钻：elemTy 是局部对象，其内部的递归传播
                            // （叶节点/ListExpr 分支）会把 type 指针裸存到 inferredType，
                            // 不保活则悬垂（2026-08-26 unwrap 修复后 RecordSemType 元素
                            // 使该路径暴露）
                            typeStore_.push_back(elemTy->clone());
                            propagateCanonicalName(*call->args[0], typeStore_.back().get());
                        }
                    }
                }
                // G1 现象 B：裸 ListExpr 直接下钻（显式 `Optional<[Point]>` 目标的列表
                // 实参/初始化器，如 take_opt_list([{..}]) / let o: Optional<[Point]> =
                // [{..}]）——否则 ListExpr.inferredType 被改写为 GenericSemType{Optional}，
                // genListExpr 读不到 ListSemType、列表元素 canonicalName 缺失退化为
                // designated init。与 OptionalSemType 分支（L1237-1240）对齐：把
                // ListExpr 下钻到元素类型（ListSemType，其 ListExpr 分支逐元素传播）。
                if (auto* listExpr = dynamic_cast<const ListExpr*>(&expr)) {
                    auto elemTy = elemTypeOf(type);
                    if (!dynamic_cast<const ErrorSemType*>(elemTy.get())) {
                        typeStore_.push_back(elemTy->clone());
                        propagateCanonicalName(*listExpr, typeStore_.back().get());
                    }
                    return;
                }
            }
            // #1：Identifier 保留自身类型（符号表类型），不被 GenericSemType{Optional}
            // 覆盖——否则 `Optional<X> = p`（p 为 Point）与 `Optional<X> = x`
            // （x 为 Optional 变量）的 p/x 推断类型被改写成相同的 GenericSemType{Optional}，
            // CodeGen 无法区分「record→view/裸值装箱」与「已是 Optional 值防二次装箱」。
            // 与 OptionalSemType 分支（L986-987）/ 叶节点（L1023）的 Identifier 保留一致。
            // IndexExpr / MemberAccessExpr 同样保留真实元素/字段类型（inferIndexExpr /
            // inferMemberAccess 结果，即 CodeGen 求值结果 C++ 类型）——被 Optional 目标
            // 改写会掩盖「a[i] / h.opt 求值结果已是 Optional 或仍是裸 X」的事实，使
            // CodeGen isAlreadyOptionalValue 对非 Optional 元素下标误判"已是 Optional"
            // 直通裸 X*（防二次装箱判定依赖该真实类型）。
            if (!dynamic_cast<const Identifier*>(&expr)
                && !dynamic_cast<const IndexExpr*>(&expr)
                && !dynamic_cast<const MemberAccessExpr*>(&expr))
                const_cast<ASTNode&>(expr).inferredType = type;
            return;
        }
        auto* sym = symtab_.lookup(gs->name);
        if (sym && sym->kind == SymKind::TypeAlias && sym->type) {
            propagateCanonicalName(expr, sym->type.get());
            return;
        }
        const_cast<ASTNode&>(expr).inferredType = type;
        return;
    }

    // UnionSemType：试每个变体，取第一个形态可匹配的（如 children: [Tree<T>] | T）
    // 只传播与表达式形态匹配的变体：RecordExpr 匹配 record/泛型变体，ListExpr 匹配 list 变体；
    // 其他表达式（int/string 字面量等）不改写 inferredType——否则 int | [int] 中 [1,2]
    // 会被错误标注为 int 变体，导致 CodeGen 隐式装箱选错变体索引
    if (auto* us = dynamic_cast<const UnionSemType*>(type)) {
        // #4：联合含 None 变体时，none()（CallExpr）→ 标注 NoneSemType——否则字段
        // none() 推断保持 Optional<error>，CodeGen genUnionBoxing 的
        // mapSemType(OptionalSemType{Error}) 报 error_type（形态 B：Node|None /
        // channel<int>|None 等不折叠 UnionSemType 字段）。与 isAssignable 对
        // Union+None 的放行（SemAnalyzer.cpp:589-595）对齐。
        if (auto* call = dynamic_cast<const CallExpr*>(&expr);
            call && call->args.empty()) {
            if (auto* id = dynamic_cast<const Identifier*>(call->callee.get());
                id && id->name == "none") {
                for (auto& v : us->variants) {
                    if (v && dynamic_cast<const NoneSemType*>(v.get())) {
                        const_cast<ASTNode&>(expr).inferredType = v.get();
                        return;
                    }
                }
            }
        }
        for (auto& v : us->variants) {
            if (!v) continue;
            bool shapeMatch =
                (dynamic_cast<const RecordExpr*>(&expr) &&
                 (dynamic_cast<const RecordSemType*>(v.get()) ||
                  dynamic_cast<const GenericSemType*>(v.get())))
                || (dynamic_cast<const ListExpr*>(&expr) &&
                    dynamic_cast<const ListSemType*>(v.get()));
            if (shapeMatch) { propagateCanonicalName(expr, v.get()); return; }
        }
        return;
    }

    // OptionalSemType（T | None 折叠 / Optional<T> 上下文）：record 字面量下钻
    // elementType 传播 canonicalName（否则被覆盖为 OptionalSemType，genRecordExpr
    // 取不到具体类型退化为 designated initializer）；some(record) 实参同样下钻；
    // Identifier 保持原样（类型来自符号表），其余表达式保持 OptionalSemType 标注
    if (auto* os = dynamic_cast<const OptionalSemType*>(type)) {
        if (os->elementType) {
            if (dynamic_cast<const RecordExpr*>(&expr)) {
                propagateCanonicalName(expr, os->elementType.get());
                return;
            }
            // #6：some(record)/some(list) 实参下钻——some(ListExpr) 时 os->elementType
            // 为 ListSemType，复用 ListExpr 分支逐元素传播
            // Phase 2-③：some(some(...)) 嵌套同样下钻（isSomeDescendArg）
            if (auto* call = dynamic_cast<const CallExpr*>(&expr);
                call && call->args.size() == 1 && call->args[0]
                && isSomeDescendArg(call->args[0].get())) {
                if (auto* id = dynamic_cast<const Identifier*>(call->callee.get());
                    id && id->name == "some") {
                    propagateCanonicalName(*call->args[0], os->elementType.get());
                }
            }
            // #6（C10/C13）：`[Point]|None = [{..},{..}]` 折叠为 Optional<[Point]>，
            // ListExpr 直接（非 some 包裹）下钻到列表逐元素；否则 inferredType 被
            // 覆盖为 OptionalSemType、元素 canonicalName 缺失 → genListExpr 读不到
            // 列表类型退化为 designated init。
            if (auto* listExpr = dynamic_cast<const ListExpr*>(&expr)) {
                propagateCanonicalName(*listExpr, os->elementType.get());
                return;
            }
        }
        // IndexExpr / MemberAccessExpr 保留真实元素/字段类型（同 GenericSemType 分支），
        // 不被 Optional 目标改写——否则 `o: Point|None = a[i]`（a: [Point]）的 a[i] 被
        // 改写为 OptionalSemType，CodeGen 误判"已是 Optional"直通裸 Point*。
        if (!dynamic_cast<const Identifier*>(&expr)
            && !dynamic_cast<const IndexExpr*>(&expr)
            && !dynamic_cast<const MemberAccessExpr*>(&expr))
            const_cast<ASTNode&>(expr).inferredType = type;
        return;
    }

    // ConditionalExpr：分支下钻（#1，决策 A）——`let p: Point = flag ? {..} : {..}`
    // 等场景：期望类型传播到两分支的匿名 record（inferConditional 推断期已带期望，
    // 此处是提交点/下钻路径再次写入 canonicalName 的兜底），并把整体标注为期望类型
    if (auto* cond = dynamic_cast<const ConditionalExpr*>(&expr)) {
        if (cond->thenBranch) propagateCanonicalName(*cond->thenBranch, type);
        if (cond->elseBranch) propagateCanonicalName(*cond->elseBranch, type);
        const_cast<ASTNode&>(expr).inferredType = type;
        return;
    }

    // RecordExpr 匹配 RecordSemType ↔ 标注 + 传播到字段
    if (auto* recExpr = dynamic_cast<const RecordExpr*>(&expr)) {
        if (auto* rs = dynamic_cast<const RecordSemType*>(type)) {
            if (!rs->canonicalName.empty()) {
                // 保活后再写入 inferredType：type 可能来自调用方局部对象
                // （如 elemTypeOf 的返回值，仅临时持有），裸存会悬垂
                typeStore_.push_back(rs->clone());
                const_cast<RecordExpr*>(recExpr)->inferredType = typeStore_.back().get();
            }
            for (auto& f : recExpr->fields) {
                for (auto& ft : rs->fields) {
                    if (f.name == ft.name && f.value) {
                        if (ft.type)
                            propagateCanonicalName(*f.value, ft.type.get());
                        break;
                    }
                }
            }
        }
        return;
    }

    // ListExpr 匹配 ListSemType ↔ 传播到每个元素
    if (auto* listExpr = dynamic_cast<const ListExpr*>(&expr)) {
        const_cast<ListExpr*>(listExpr)->inferredType = type;
        if (auto* ls = dynamic_cast<const ListSemType*>(type)) {
            for (auto& elem : listExpr->elements) {
                if (elem) propagateCanonicalName(*elem, ls->elementType.get());
            }
        }
        return;
    }

    // 叶节点：仅标注 inferredType（Identifier 除外——其类型来自符号表，
    // 若被声明类型改写会丢失 record 具体类型；CodeGen 接口视图绑定
    // （genLetStmt §3.10）依赖 initializer 保持 RecordSemType 以识别 record 指针）
    if (dynamic_cast<const Identifier*>(&expr)) return;
    const_cast<ASTNode&>(expr).inferredType = type;
}

} // namespace Aura
