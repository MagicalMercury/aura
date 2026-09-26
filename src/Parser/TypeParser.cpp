#include "../Parser.h"

namespace Aura {

// ============================================================
// 类型解析
// ============================================================

std::unique_ptr<TypeExpr> Parser::parseType() {
    return parseUnionType();
}

std::unique_ptr<TypeExpr> Parser::parseUnionType() {
    auto left = parsePrimaryType();

    while (match(TokType::Bar)) {
        auto un = std::make_unique<UnionType>();
        setNodePos(un.get(), peek());
        un->types.push_back(std::move(left));
        un->types.push_back(parsePrimaryType());
        while (match(TokType::Bar)) {
            un->types.push_back(parsePrimaryType());
        }
        left = std::move(un);
    }

    return left;
}

std::unique_ptr<TypeExpr> Parser::parsePrimaryType() {
    // 泛型类型引用 <T>
    if (check(TokType::Less)) {
        advance(); // <
        auto& nameTok = consume(TokType::Identifier, "expected type variable name after '<'");
        auto n = std::make_unique<GenericTypeRef>();
        setNodePos(n.get(), nameTok);
        n->name = nameTok.lexeme;
        consume(TokType::Greater, "expected '>' after type variable");
        return n;
    }

    // 列表类型 [Type]
    if (match(TokType::LBracket)) {
        auto n = std::make_unique<ListType>();
        setNodePos(n.get(), peek());
        n->elementType = parseType();
        consume(TokType::RBracket, "expected ']' after list element type");
        return n;
    }

    // 记录类型 { field: Type, ... }
    if (check(TokType::LBrace)) {
        auto n = std::make_unique<RecordType>();
        setNodePos(n.get(), peek());
        advance(); // {

        if (!check(TokType::RBrace)) {
            do {
                RecordFieldType f;
                auto& nameTok = consume(TokType::Identifier, "expected field name in record type");
                f.name = nameTok.lexeme;
                consume(TokType::Colon, "expected ':' in record field type");
                f.type = parseType();
                n->fields.push_back(std::move(f));
            } while (match(TokType::Comma));
        }

        consume(TokType::RBrace, "expected '}' after record type");
        return n;
    }

    // 函数类型: fun(params) throws? -> Ret
    if (check(TokType::Fun)) {
        advance(); // fun

        auto ft = std::make_unique<FunctionType>();
        setNodePos(ft.get(), peek());

        consume(TokType::LParen, "expected '(' after 'fun' in function type");
        if (!check(TokType::RParen)) {
            do {
                // 参数类型，可能包含 <T> 泛型标记
                ft->paramTypes.push_back(parseType());
            } while (match(TokType::Comma));
        }
        consume(TokType::RParen, "expected ')' after function type parameters");

        if (match(TokType::Throws)) ft->throws = true;

        if (match(TokType::Arrow)) {
            ft->returnType = parseType();
        }

        return ft;
    }

    // 命名类型，可能是泛型实例化: Name<T> 或 Name<A, B>
    // 也可能是命名空间限定: ns.Name<T>
    // None 也是合法的类型名
    // sync 是关键字但在类型上下文作为伪模块名（如 sync.Mutex）
    if (check(TokType::Identifier) || check(TokType::None) || check(TokType::Sync)) {
        auto& tok = advance();
        auto n = std::make_unique<NamedType>();
        setNodePos(n.get(), tok);
        // sync 关键字在类型位置作为伪模块名 "sync"
        n->name = (tok.type == TokType::Sync) ? "sync" : tok.lexeme;

        // 解析命名空间前缀: a.b.c.Name → prefix = [a, b, c], name = Name
        while (match(TokType::Dot)) {
            auto& nextTok = consume(TokType::Identifier, "expected identifier after '.' in type name");
            n->namespacePrefix.push_back(n->name);
            n->name = nextTok.lexeme;
        }

        // 泛型实例化参数: Name<T> 或 Name<A, B>
        if (match(TokType::Less)) {
            do {
                n->typeArgs.push_back(parseType());
            } while (match(TokType::Comma));
            consume(TokType::Greater, "expected '>' after type arguments");
        }

        if (match(TokType::Arrow)) {
            auto ft = std::make_unique<FunctionType>();
            setNodePos(ft.get(), tok);
            ft->paramTypes.push_back(std::move(n));
            ft->returnType = parseType();
            if (match(TokType::Throws)) ft->throws = true;
            return ft;
        }

        return n;
    }

    // 函数类型 (params) -> retType
    if (match(TokType::LParen)) {
        auto tok = peek();
        std::vector<std::unique_ptr<TypeExpr>> paramTypes;

        if (!check(TokType::RParen)) {
            do {
                paramTypes.push_back(parseType());
            } while (match(TokType::Comma));
        }

        consume(TokType::RParen, "expected ')'");

        if (match(TokType::Arrow)) {
            auto ft = std::make_unique<FunctionType>();
            setNodePos(ft.get(), tok);
            ft->paramTypes = std::move(paramTypes);
            ft->returnType = parseType();
            if (match(TokType::Throws)) ft->throws = true;
            return ft;
        }

        if (paramTypes.size() == 1) return std::move(paramTypes[0]);
        if (paramTypes.empty()) {
            error("expected type in parentheses");
            return nullptr;
        }
        // 多参数无箭头 → 元组类型 (T1, T2, ...)（匿名 record 语法糖，位置字段 _0/_1/...）
        auto tp = std::make_unique<TupleTypeExpr>();
        setNodePos(tp.get(), tok);
        tp->elementTypes = std::move(paramTypes);
        return tp;
    }

    error("expected type");
    return nullptr;
}

// ============================================================
// 模式解析
// ============================================================

std::unique_ptr<Pattern> Parser::parsePattern() {
    // P5：Rust 风格 `|` 分组——仅常量模式允许分组
    //   match x { 1 | 2 | 3 => A, _ => B }
    // 类型模式分组报错引导分开写（如 User | string）。
    auto first = parseSinglePattern();

    if (!match(TokType::Bar)) return first;

    auto group = std::make_unique<GroupPattern>();
    if (!dynamic_cast<ConstantPattern*>(first.get())) {
        error("type pattern cannot be grouped; write separate cases or use a union type");
    }
    group->alts.push_back(std::move(first));
    do {
        auto alt = parseSinglePattern();
        if (!dynamic_cast<ConstantPattern*>(alt.get())) {
            error("type pattern cannot be grouped; write separate cases or use a union type");
        }
        group->alts.push_back(std::move(alt));
    } while (match(TokType::Bar));
    return group;
}

std::unique_ptr<Pattern> Parser::parseSinglePattern() {
    // 通配符 _
    if (check(TokType::Identifier) && peek().lexeme == "_") {
        advance();
        return std::make_unique<WildcardPattern>();
    }

    // 常量模式
    if (check(TokType::None) || check(TokType::True) || check(TokType::False) ||
        check(TokType::IntLiteral) || check(TokType::FloatLiteral) ||
        check(TokType::StringLiteral)) {
        auto pat = std::make_unique<ConstantPattern>();
        pat->value = parsePrimary();
        return pat;
    }

    // 类型模式: TypeName [variableName]
    if (check(TokType::Identifier)) {
        auto pat = std::make_unique<TypePattern>();
        pat->typeName = advance().lexeme;
        if (check(TokType::Identifier)) {
            pat->varName = advance().lexeme;
        }
        return pat;
    }

    error("expected pattern");
    return std::make_unique<WildcardPattern>();
}

// ============================================================
// 参数解析
// ============================================================

Param Parser::parseParam() {
    Param p;
    auto& nameTok = consume(TokType::Identifier, "expected parameter name");
    p.name = nameTok.lexeme;

    if (match(TokType::Colon)) {
        p.type = parseType();
        // 默认参数：name: type = expr
        if (match(TokType::Assign)) {
            // feature-13 C2（第二轮修正）：默认值改用真实表达式解析，与完整解析路径同构。
            //
            // 旧做法（已废）：扫描态调 trySkipValueTokens() 跳 token。实测失败 ——
            // 该函数为防向逆吞声明，显式拒绝把 Fun 当作值的首 token，
            // 而默认值恰好可以是闭包字面量 fun() -> int { ... }，
            // 所以该迴避策略对这一形态天然不成立。
            //
            // 为何用 parseExpr：默认值的结束边界（逗号 / 右括号）是语法
            // 事实，只有真实语法分析器能全面覆盖；自写跳 token 近似实现
            // 必然在新形态上翻车。默认值属声明签名的一部分（不知道它
            // 从哪里结束就无法确定参数列表的边界），与「扫描跳过
            // body」并不矛盾：不建 body 节点依然由 parseFunDecl/
            // parseMethodDecl 的 skipBlockTokens() 分支保证。
            //
            // 不再区分 scanOnly_ 的理由：存下 defaultExpr 对扫描产物无害
            // （FuncSkeleton 只取参数类型串），且保留了两条路径对同一源
            // 码产出相同 params 这一性质。
            p.defaultExpr = parseExpr();
        }
    }
    return p;
}

std::vector<Param> Parser::parseParams() {
    std::vector<Param> params;
    do {
        params.push_back(parseParam());
    } while (match(TokType::Comma));
    return params;
}

InterfaceMethodSig Parser::parseInterfaceMethodSig() {
    InterfaceMethodSig sig;
    auto& nameTok = consume(TokType::Identifier, "expected method name in interface");
    sig.name = nameTok.lexeme;

    consume(TokType::LParen, "expected '(' after method name in interface");
    if (!check(TokType::RParen)) {
        sig.params = parseParams();
    }
    consume(TokType::RParen, "expected ')' after parameters in interface");

    if (match(TokType::Throws)) sig.throws = true;

    if (match(TokType::Arrow)) {
        sig.returnType = parseType();
    }

    // 接口方法签名后三选一：
    //   { body } → Aura 默认方法（DefaultAura）
    //   ...      → C++ 桥接方法（CppBridge，aura 无实现 c++ 有实现）
    //   无       → 纯虚（record 必须实现）
    if (check(TokType::LBrace)) {
        if (scanOnly_) {
            // feature-13 C2：声明级扫描 —— 默认方法体只跳过，不建 AST。
            // bodyKind 保持 Pure：扫描段的用途是「声明骨架」（接口名/方法名/签名），
            // 默认方法体本身不属于骨架；C3 汇总段需要 body 时走完整解析路径。
            skipBlockTokens();
        } else {
            sig.bodyKind = InterfaceMethodSig::BodyKind::DefaultAura;
            sig.defaultBody = parseBlock();
        }
    } else if (match(TokType::Ellipsis)) {
        sig.bodyKind = InterfaceMethodSig::BodyKind::CppBridge;
    }

    return sig;
}

} // namespace Aura
