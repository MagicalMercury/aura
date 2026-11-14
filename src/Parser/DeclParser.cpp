#include "../Parser.h"
#include <algorithm>

namespace Aura {

namespace {
// feature-21 Phase 2（§3.2.3 白名单）：`#X{...}` / `#X(...)` 前缀的挂载目标。
// 裸名（MetaValueRefExpr）不在此判定——它的挂载/拼接/织入分类延后到展开期（D3）。
static bool isMountableDecl(const Decl& d) {
    if (dynamic_cast<const FunDecl*>(&d)) return !static_cast<const FunDecl&>(d).isComptime;
    return dynamic_cast<const MethodDecl*>(&d) != nullptr
        || dynamic_cast<const TypeDecl*>(&d) != nullptr
        || dynamic_cast<const InterfaceDecl*>(&d) != nullptr;
}
} // namespace

// ============================================================
// 声明解析
// ============================================================

std::unique_ptr<Decl> Parser::parseDecl() {
    bool isPublic = match(TokType::Pub);

    std::unique_ptr<Decl> decl;

    // feature-21：挂载前缀 `#X{ ... }` / `#X( ... )` 可出现在 `pub` 之前或之后。
    // 前缀只吃「名字 + { 或 (」形态；`#type`/`#fun`/`#const`/`#where`/`#name`/`#name.member`
    // 一律不当前缀（见 parseMountPrefixes 的 break 条件）。
    auto prefixes = parseMountPrefixes();
    if (!prefixes.empty()) isPublic = match(TokType::Pub) || isPublic;

    // feature-21 Phase 1（S4）：声明起点 token —— 挂载类错误必须报在这里
    // （parseDecl 尾部的 error() 只看 peek()，会把 `#A{...}\nlet y = 1` 报到下一行）。
    Token declStart = peek();

    // feature-13 C0：`module <ident>` 软关键字分支。
    // 必须在最前（module 是文件首行声明）；用软关键字判定（非 TokType::Module），
    // 故 `let module = 1` 等既有用法不受影响。
    if (peekSoftKeyword("module")) {
        decl = parseModuleDecl();
    }
    // enum 软关键字（A4，2026-10-14）：判定沿用既有 peekSoftKeyword（Identifier + 文本比对），
    // 再加【两 token 前瞻】——必须看到紧随的硬关键字 `type` 才算 enum 声明。
    // 先例：`lock` 的两 token 前瞻（src/Parser/StmtParser.cpp:35–36）。
    // 🔴 与 check(TokType::Type) 分支**不存在**争抢：软关键字的首 token 类型是 Identifier，
    //    而 `type` 分支判的是 TokType::Type ⇒ 两者互斥
    //    （只有硬关键字方案才需要「必须排在 Type 之前」）。
    else if (peekSoftKeyword("enum") && peekNext().type == TokType::Type) {
        decl = parseEnumDecl();
    }
    else if (check(TokType::Hash)) {
        decl = parseHashDecl();
    }
    else if (check(TokType::Fun)) {
        if (peekNext().type == TokType::LParen) {
            decl = parseMethodDecl();
        } else {
            decl = parseFunDecl();
        }
    }
    else if (check(TokType::Let))      decl = parseLetDecl();
    else if (check(TokType::Const))    decl = parseConstDecl();
    else if (check(TokType::Type))     decl = parseTypeDecl();
    else if (check(TokType::Interface)) decl = parseInterfaceDecl();
    else if (check(TokType::Import))   { seenImport_ = true; decl = parseImportDecl(); }
    else {
        // feature-21 Phase 2（D2/D3）：裸名前缀 + 后面没有任何声明 ⇒ 独立 unquote 组。
        // 判定延后：裸名绑定的值可能是 QuoteExpr（拼接）或 MetaConstruct（挂载），
        // 解析期不知道 ⇒ 只有「全部是裸名」才降级为 UnquoteGroup，混合形态仍报错。
        bool allBare = !prefixes.empty()
            && std::all_of(prefixes.begin(), prefixes.end(),
                 [](auto& p){ return dynamic_cast<MetaValueRefExpr*>(p.get()) != nullptr; });
        if (allBare) {
            auto g = std::make_unique<UnquoteGroup>();
            setNodePos(g.get(), declStart);
            for (auto& p : prefixes) {
                auto* r = static_cast<MetaValueRefExpr*>(p.get());
                auto u = std::make_unique<UnquoteItem>();
                // 注意：change.md §3.2.3 写作 setNodePos(u.get(), r)，第二个形参是
                // const Token& ⇒ 与 ASTNode* 类型不符（不可编译）；此处显式拷 line/col。
                u->line = r->line;
                u->col  = r->col;
                u->name = r->name;
                g->items.push_back(std::move(u));
            }
            return g;
        }
        // feature-21：前缀已消费但后面不是任何声明 ⇒ 报错（§3.5）。
        // 必须先于 enum 定向诊断：`#A{ x = 1 } enum` 的真实问题是「后面没声明」。
        if (!prefixes.empty()) {
            errorAt(declStart, "metadata construct must be followed by a declaration");
            return nullptr;
        }
        // enum 定向诊断（软关键字的代价面）：`enum` 后不是 `type` 时给可操作文案。
        // 声明位置的裸 Identifier 本就非法（本分支**不改变任何合法源码**的语义）；
        // 但 `enum Color = { Red }`（漏写 `type`）是最可能的笔误 ⇒ 单独提示。
        if (check(TokType::Identifier) && peek().lexeme == "enum") {
            error("expected 'type' after 'enum'; Aura enums are declared as "
                  "`enum type Name = { Variant, ... }`");
            return nullptr;
        }
        error("expected declaration");
        return nullptr;
    }

    if (decl) {
        decl->isPublic = isPublic;
        // N-28：`#const` 不接受任何前缀（`#Q #const N = 1` 无语义，且会让 pass 漏处理前缀）。
        // 前缀丢弃、声明保留（错误已记录），不再走下面的收集循环。
        if (dynamic_cast<HashConstDecl*>(decl.get()) && !prefixes.empty()) {
            errorAt(declStart, "metadata prefixes cannot be attached to '#const'");
            return decl;
        }
        // B-01 白名单（Phase 1）：只有「已知形态」（构造 / 编译期调用）当场校验挂载目标；
        // 裸名（MetaValueRefExpr）只收集——挂载/拼接/织入的判定延后到展开期（D3）。
        for (auto& p : prefixes) {
            bool known = dynamic_cast<MetaConstructExpr*>(p.get()) != nullptr
                      || dynamic_cast<MetaCallExpr*>(p.get()) != nullptr;
            if (known && !isMountableDecl(*decl)) {
                // 用白名单而非黑名单：`#X{...} let` / `#X{...} import` 同样必须被拒。
                errorAt(declStart, "metadata cannot be mounted on this declaration; "
                      "mount targets are 'fun'/'method'/'type'/'interface'");
                continue;
            }
            decl->metaPrefixes.push_back(std::move(p));
        }
    }
    return decl;
}

// feature-21 Phase 1：解析声明前的挂载前缀序列（current 为 `#` 或非 `#`）。
// §0.4 硬约定：每条路径要么 break（不动 token）、要么已经消费 `#`。
std::vector<std::unique_ptr<ASTNode>> Parser::parseMountPrefixes() {
    std::vector<std::unique_ptr<ASTNode>> out;
    while (check(TokType::Hash)) {
        TokType t1 = peekAt(1).type;
        // #type / #fun / #const 是声明，不是前缀
        if (t1 == TokType::Type || t1 == TokType::Fun || t1 == TokType::Const) break;
        if (!(t1 == TokType::Identifier || isKeywordIdent(t1))) break;
        // `#where {` 不是构造
        if (t1 == TokType::Identifier && peekAt(1).lexeme == "where") break;
        TokType t2 = peekAt(2).type;
        if (t2 == TokType::LBrace)      out.push_back(parseMetaConstructAfterHash());
        else if (t2 == TokType::LParen) out.push_back(parseHashCallAfterHash());
        // feature-21 Phase 3（L1）：`#f<int>(...)` —— '<' 同样按编译期调用入口解析；
        // 类型实参前瞻不成立时 parseHashCallAfterHash 按裸名返回且不消费 '<'
        //（与下方裸名分支同结果，`#x < 5` 行为不变）。
        else if (t2 == TokType::Less)   out.push_back(parseHashCallAfterHash());
        else if (t2 != TokType::Dot)    out.push_back(parseMetaValueRefAfterHash()); // 裸名（D2）
        else break;   // #name.member：交给 parseHashDecl 报错
    }
    return out;
}

// feature-21 Phase 2（S3，D2）：裸 `#name`（进入时 current = `#`）。
// 供 parseMountPrefixes（声明位裸名前缀）复用；语句位裸名在 parseHashStmt 内联解析。
// §0.3：入口先 advance() 吃 `#` ⇒ 所有 error 分支均已消费 ≥1 token。
std::unique_ptr<ASTNode> Parser::parseMetaValueRefAfterHash() {
    auto hashTok = advance();       // '#'
    if (!(check(TokType::Identifier) || isKeywordIdent(peek().type))) {
        error("expected name after '#'");
        return nullptr;
    }
    auto& nameTok = advance();      // 名字（关键字标识符同上放行，与 parseHashExpr 同口径）
    auto n = std::make_unique<MetaValueRefExpr>();
    setNodePos(n.get(), hashTok);
    n->name = nameTok.lexeme;
    return n;
}

// feature-21 Phase 1：`#X{ k = v, ... }`（进入时 current = `#`）。
std::unique_ptr<ASTNode> Parser::parseMetaConstructAfterHash() {
    auto hashTok = advance();              // '#'
    auto& nameTok = advance();             // 类型名
    auto n = std::make_unique<MetaConstructExpr>();
    setNodePos(n.get(), hashTok);
    n->typeName = nameTok.lexeme;
    consume(TokType::LBrace, "expected '{' after metadata type name");
    if (!check(TokType::RBrace)) {
        do {
            RecordField f;
            f.name = consume(TokType::Identifier, "expected field name in metadata construct").lexeme;
            consume(TokType::Assign, "expected '=' in metadata field");
            f.value = parseExpr();
            n->fields.push_back(std::move(f));
        } while (match(TokType::Comma));
    }
    consume(TokType::RBrace, "expected '}' after metadata fields");
    return n;
}

// feature-21 Phase 1：`#f(args...)`（进入时 current = `#`）。
// feature-21 Phase 3（L1）：`#f<int>(args...)` —— `#` + 名字后可选显式类型实参。
std::unique_ptr<ASTNode> Parser::parseHashCallAfterHash() {
    auto hashTok = advance();              // '#'
    auto& nameTok = advance();             // 函数名
    auto ref = std::make_unique<MetaValueRefExpr>();
    setNodePos(ref.get(), nameTok);
    ref->name = nameTok.lexeme;
    auto call = std::make_unique<MetaCallExpr>();
    setNodePos(call.get(), hashTok);
    call->callee = std::move(ref);
    // L1：仅当 '<' 后确为类型列表、以 '>' 收尾且紧跟 '(' 时才按显式类型实参解析
    // （复用 N2 的 lookaheadTypeArgsBeforeCall，不新造判定）；否则 '<' 属比较运算
    // （`#x < 5`），本函数按裸名返回、不消费 '<' —— 与 L1 前行为一致。
    if (check(TokType::Less)) {
        if (!lookaheadTypeArgsBeforeCall()) {
            auto bare = std::make_unique<MetaValueRefExpr>();
            setNodePos(bare.get(), nameTok);
            bare->name = nameTok.lexeme;
            return bare;
        }
        advance(); // <
        do {
            call->typeArgs.push_back(parseType());
        } while (match(TokType::Comma));
        consume(TokType::Greater, "expected '>' after type arguments");
    }
    consume(TokType::LParen, "expected '(' after compile-time function name");
    if (!check(TokType::RParen)) {
        do {
            if (auto a = parseExpr()) call->args.push_back(std::move(a));
        } while (match(TokType::Comma));
    }
    consume(TokType::RParen, "expected ')' after compile-time call arguments");
    return call;
}

// feature-21 Phase 1（S4）：`#` 声明入口（进入时 current = `#`）。
// §0.4：入口先 advance() 吃 `#` ⇒ 所有 error 分支均已消费 ≥1 token。
std::unique_ptr<Decl> Parser::parseHashDecl() {
    advance(); // '#'
    if (atEnd()) { error("expected construct after '#'"); return nullptr; }

    if (check(TokType::Type))  return parseMetaTypeDecl();
    if (check(TokType::Fun))   return parseMetaFunDecl();
    if (check(TokType::Const)) return parseHashConstDecl();

    if (check(TokType::LParen) || check(TokType::LBrace)) {
        error("compile-time value is not a declaration");
        return nullptr;
    }

    if (check(TokType::Identifier) && peek().lexeme == "where"
        && peekNext().type == TokType::LBrace) {
        error("'#where' must follow a declaration");
        return nullptr;
    }

    if (check(TokType::Identifier) || isKeywordIdent(peek().type)) {
        if (peekNext().type == TokType::Dot) {
            std::string ns = advance().lexeme;   // 命名空间
            advance();                           // '.'
            // §3.6 原文写 consume(TokType::Identifier, ...)，但 `#io.sync` 的 key 是
            // 硬关键字（TokType::Sync，见 src/TokType.cpp:109）⇒ 照抄会让
            // ParserDecl.ConfigDirectiveRemoved 与探针 s4_cfg 拿不到迁移诊断。
            // 故 key 位置放行关键字标识符（与 consumeMemberName 的 `.type` 同一口径）。
            if (!(check(TokType::Identifier) || isKeywordIdent(peek().type))) {
                error("expected key after '.'");
                return nullptr;
            }
            std::string key = advance().lexeme;
            if (check(TokType::Assign)) {
                error("config directives were removed; use '#" + ns + "{ " + key +
                      " = ... }' metadata instead");
                return nullptr;
            }
            error("'#" + ns + "." + key + "' is not a declaration");
            return nullptr;
        }
        error("'#" + peek().lexeme + "' is not a declaration; use '#type', '#fun', "
              "'#const', or mount metadata with '#X{ ... }'");
        return nullptr;
    }
    error("expected construct after '#'");
    return nullptr;
}

// feature-21 Phase 1（S4）：`#type Name<T> where {...} #where {...} = { field: T, ... }`
// §0.4：入口 advance() 吃 `type`。
std::unique_ptr<MetaTypeDecl> Parser::parseMetaTypeDecl() {
    auto tok = advance(); // type
    auto decl = std::make_unique<MetaTypeDecl>();
    setNodePos(decl.get(), tok);

    auto& nameTok = consume(TokType::Identifier, "expected '#type' name");
    decl->name = nameTok.lexeme;

    if (match(TokType::Less)) {
        do {
            decl->typeParams.push_back(
                consume(TokType::Identifier, "expected type parameter name").lexeme);
        } while (match(TokType::Comma));
        consume(TokType::Greater, "expected '>' after type parameters");
    }

    // where 在前
    // ⚠️ change.md §3.7 原文此处直接调 parseWhereClause(false)，但该函数入口是
    //    consume(LBrace)（§3.9）⇒ 必须先把软关键字 `where` 吃掉（与下方 `#where`
    //    分支的 advance(); advance() 同口径）。照抄原文会在 `where` 处报
    //    `expected '{' in 'where' clause`。
    if (peekSoftKeyword("where") && peekNext().type == TokType::LBrace) {
        advance(); // 'where'
        decl->where = parseWhereClause(false);
    }

    // #where 在后（允许单独出现）
    if (check(TokType::Hash) && peekAt(1).type == TokType::Identifier
        && peekAt(1).lexeme == "where" && peekAt(2).type == TokType::LBrace) {
        advance(); // '#'
        advance(); // 'where'
        decl->mountWhere = parseWhereClause(true);
        if (peekSoftKeyword("where"))
            error("'#where' must follow 'where'");
    }

    consume(TokType::Assign, "expected '=' in '#type' declaration");
    consume(TokType::LBrace, "expected '{' in '#type' declaration");

    if (!check(TokType::RBrace)) {
        do {
            Param f;
            f.name = consume(TokType::Identifier, "expected field name in '#type'").lexeme;
            consume(TokType::Colon, "expected ':' after field name in '#type'");
            f.type = parseType();
            if (match(TokType::Assign)) f.defaultExpr = parseExpr();
            decl->fields.push_back(std::move(f));
        } while (match(TokType::Comma));
    }
    consume(TokType::RBrace, "expected '}' after '#type' fields");
    match(TokType::Semicolon);
    return decl;
}

// feature-21 Phase 1（S5）：`#fun Name(params) where? throws? -> T? #where? body`
// §0.4：入口 advance() 吃 `fun` ⇒ 所有 error 分支均已消费 ≥1 token。
std::unique_ptr<FunDecl> Parser::parseMetaFunDecl() {
    auto tok = advance(); // fun
    auto decl = std::make_unique<FunDecl>();
    decl->isComptime = true;
    setNodePos(decl.get(), tok);

    decl->name = consume(TokType::Identifier, "expected '#fun' name").lexeme;
    // feature-21 Phase 3（L1）：`#fun g<T>(...)` —— 与普通 fun 同款可选泛型表
    parseOptionalTypeParams(decl->typeParams);
    TypeParamScope typeScope(*this, decl->typeParams);
    consume(TokType::LParen, "expected '(' after '#fun' name");
    if (!check(TokType::RParen)) decl->params = parseParams();
    consume(TokType::RParen, "expected ')' after '#fun' parameters");

    // `where` 可出现在 `)` 之后（throws 之前）或 `-> T` 之后——与普通 `fun` 一致（终审 🟢1）
    if (peekSoftKeyword("where") && peekNext().type == TokType::LBrace) {
        advance(); // 'where'（parseWhereClause 入口是 consume(LBrace)）
        decl->where = parseWhereClause(false);
    }

    if (match(TokType::Throws)) decl->throws = true;
    if (match(TokType::Arrow)) decl->returnType = parseType();

    if (!decl->where && peekSoftKeyword("where") && peekNext().type == TokType::LBrace) {
        advance(); // 'where'
        decl->where = parseWhereClause(false);
    }
    if (check(TokType::Hash) && peekAt(1).type == TokType::Identifier
        && peekAt(1).lexeme == "where" && peekAt(2).type == TokType::LBrace) {
        advance(); advance();
        decl->mountWhere = parseWhereClause(true);
    }

    if (match(TokType::Ellipsis)) {
        decl->hasCppImpl = true;
    } else if (scanOnly_) {
        decl->bodySkippedByScan = true;
        skipBlockTokens();
    } else if (!noBody_) {
        decl->body = parseBlock();
    }
    return decl;
}

// feature-21 Phase 1（S5）：`#const Name = expr`（宏名，无类型标注）
// §0.4：入口 advance() 吃 `const` ⇒ 所有 error 分支均已消费 ≥1 token。
std::unique_ptr<HashConstDecl> Parser::parseHashConstDecl() {
    auto tok = advance(); // const
    auto decl = std::make_unique<HashConstDecl>();
    setNodePos(decl.get(), tok);

    decl->name = consume(TokType::Identifier, "expected '#const' name").lexeme;

    if (check(TokType::Colon)) {
        error("'#' const cannot have a type annotation");
        advance(); // ':'
        if (!check(TokType::Assign) && !atEnd()) parseType();   // 跳过类型，防级联
    }

    consume(TokType::Assign, "expected '=' in '#const' declaration");
    decl->value = parseExpr();
    match(TokType::Semicolon);
    return decl;
}

// feature-21 Phase 2（S3，D5）：`Aura` 之后的 '{' 起 —— 引述体。
// 允许：`import` / `type` / `enum type` / `interface` / `pub` 普通声明 / `fun`
//       （方法给明确诊断）+ 普通语句。
// 禁止：`#const` / `#type` / `#fun`（含 `pub` 前缀）。R-7 / N-2：
//   禁止项必须【完整解析后丢弃】，不得「跳到第一个 '}'」——那会把 `#type` 体的 '}'
//   当成引述结束，导致外层 token 错位（花括号失配）。
// N-31：入口用 QuoteDepthGuard 维护 quoteDepth_，使嵌套 `#const`（函数体内）也被拒。
std::unique_ptr<BlockStmt> Parser::parseQuoteBody() {
    QuoteDepthGuard guard(quoteDepth_);
    auto tok = consume(TokType::LBrace, "expected '{' after 'Aura'");
    auto blk = std::make_unique<BlockStmt>();
    setNodePos(blk.get(), tok);
    // consume 失败（当前 token 不是 '{'）→ 返回空块，不进入循环（与 parseBlock 同款，
    // 避免把后续所有声明吞进引述体）。
    if (tok.type != TokType::LBrace) return blk;

    while (!check(TokType::RBrace) && !atEnd()) {
        // R-7：编译期声明 —— 先完整解析（逐 token 消费，保证花括号配平）再报错丢弃。
        if (check(TokType::Hash)
            && (peekAt(1).type == TokType::Const || peekAt(1).type == TokType::Type
                || peekAt(1).type == TokType::Fun)) {
            Token badTok = peek();
            auto bad = parseHashDecl();
            (void)bad;                            // 丢弃（错误已记录）
            errorAt(badTok, "'#const'/'#type'/'#fun' are not allowed inside 'Aura{...}'");
            continue;
        }
        if (check(TokType::Import)) {
            if (auto d = parseImportDecl()) blk->stmts.push_back(std::move(d));
            continue;
        }
        if (check(TokType::Type)) {
            if (auto d = parseTypeDecl()) blk->stmts.push_back(std::move(d));
            continue;
        }
        // `enum type Name = { ... }`（软关键字，与 parseDecl 的判据一致：两 token 前瞻）
        if (check(TokType::Identifier) && peek().lexeme == "enum"
            && peekAt(1).type == TokType::Type) {
            if (auto d = parseDecl()) blk->stmts.push_back(std::move(d));
            continue;
        }
        if (check(TokType::Interface)) {
            if (auto d = parseInterfaceDecl()) blk->stmts.push_back(std::move(d));
            continue;
        }
        // N-2：`pub #const/#type/#fun` 同属禁止项 —— 否则下面宽泛的 `pub` 分支会把
        // 编译期声明塞进引述体。先完整解析（`pub` + 编译期声明）再报错丢弃。
        if (check(TokType::Pub) && peekAt(1).type == TokType::Hash
            && (peekAt(2).type == TokType::Const || peekAt(2).type == TokType::Type
                || peekAt(2).type == TokType::Fun)) {
            Token badTok = peek();
            auto bad = parseDecl();
            (void)bad;
            errorAt(badTok, "'#const'/'#type'/'#fun' are not allowed inside 'Aura{...}'");
            continue;
        }
        if (check(TokType::Pub)) {
            if (auto d = parseDecl()) blk->stmts.push_back(std::move(d));
            continue;
        }
        if (check(TokType::Fun)) {
            Token funTok = peek();
            auto d = parseDecl();                 // 完整解析（含 `fun (self T)` 方法形态）
            if (dynamic_cast<MethodDecl*>(d.get()))
                errorAt(funTok,
                        "method declarations are not allowed directly inside 'Aura{...}'");
            else if (d) blk->stmts.push_back(std::move(d));
            continue;
        }
        auto s = parseStmt();
        if (s) blk->stmts.push_back(std::move(s));
        else synchronize();   // §0.3 兜底：nullptr ⇒ 同步一次（与 parseBlock 同款），防原地死循环
    }
    consume(TokType::RBrace, "expected '}' after 'Aura' quote");
    return blk;
}

// feature-21 Phase 1（S4）：单个约束（`impl I` / `throws` / `by #f` / `{ ... }` 集合）。
// §5.3：`where` 只允许 impl / by / 集合三种界；`throws` 仅属 §6.3 的 `#where`。
// §0.4：每个 match 分支都已消费；末路径（error）不消费 ⇒ 由调用方 continue 推进。
Bound Parser::parseBound(bool allowThrows) {
    Bound b;
    if (match(TokType::Impl)) {
        b.kind = Bound::Kind::Impl;
        b.name = consume(TokType::Identifier, "expected interface name after 'impl'").lexeme;
        if (match(TokType::Less)) {           // impl Iterator<int>
            parseType();
            while (match(TokType::Comma)) parseType();
            consume(TokType::Greater, "expected '>' after interface type arguments");
        }
        return b;
    }
    if (match(TokType::Throws)) {
        if (!allowThrows)
            error("'throws' is only allowed in '#where' bounds");
        b.kind = Bound::Kind::Throws;
        return b;
    }
    if (check(TokType::Identifier) && peek().lexeme == "by") {
        advance();                            // by
        b.kind = Bound::Kind::By;
        match(TokType::Hash);                 // by #f
        b.name = consume(TokType::Identifier, "expected '#fun' name after 'by'").lexeme;
        return b;
    }
    if (match(TokType::LBrace)) {
        b.kind = Bound::Kind::Pack;
        do { b.pack.push_back(parseBound(allowThrows)); } while (match(TokType::Comma));
        consume(TokType::RBrace, "expected '}' after bound set");
        return b;
    }
    error("expected bound after ':' in 'where' clause");
    return b;
}

// feature-21 Phase 1（S4）：`where { T: impl X, U, fun }` / `#where { fun, method }`。
// §0.4：入口 consume(LBrace) 已消费。
std::unique_ptr<WhereClause> Parser::parseWhereClause(bool isMount) {
    auto wc = std::make_unique<WhereClause>();
    consume(TokType::LBrace, isMount ? "expected '{' in '#where' clause"
                                     : "expected '{' in 'where' clause");
    if (!check(TokType::RBrace)) {
        do {
            WhereItem it;
            if (!(check(TokType::Identifier) || isKeywordIdent(peek().type))) {
                error("expected constraint target in 'where' clause");
                break;
            }
            it.target = advance().lexeme;
            if (match(TokType::Colon)) {
                it.bound = parseBound(isMount);
                it.hasBound = true;
            } else if (check(TokType::Impl) || check(TokType::Throws) || (check(TokType::Identifier) && peek().lexeme == "by")
                       || check(TokType::LBrace)) {
                it.bound = parseBound(isMount);
                it.hasBound = true;
            }
            wc->items.push_back(std::move(it));
        } while (match(TokType::Comma));
    }
    consume(TokType::RBrace, "expected '}' after 'where' clause");
    return wc;
}

// feature-13 C0（2026-09-17）：软关键字判定 —— 当前 token 是 Identifier 且文本匹配。
// 与既有 `as` 的处理同款（DeclParser.cpp 的 parseImportDecl 内）。
bool Parser::peekSoftKeyword(const char* kw) {
    if (atEnd()) return false;
    const Token& t = peek();
    return t.type == TokType::Identifier && t.lexeme == kw;
}

// feature-13 C0：`module <ident>` 声明解析。
// 约束（§2.1）：文件首行、首个 import 之前；重复声明 → 错误。
// 缺失时回落文件 stem（由 ModuleManager 侧兜底，Parser 不负责）。
std::unique_ptr<ModuleDecl> Parser::parseModuleDecl() {
    auto tok = advance();   // 'module'（Identifier）
    auto decl = std::make_unique<ModuleDecl>();
    setNodePos(decl.get(), tok);

    // 位置约束：必须在任何 import/声明之前（seenImport_ 由 parseDecl 的 Import 分支置位；
    // 此处只需保证 module 自身不重复出现——重复由 ModuleManager 侧或本函数计数判定）
    if (seenImport_) {
        error("'module' declaration must appear before any import statement");
    }
    if (sawModuleDecl_) {
        error("duplicate 'module' declaration");
    }
    sawModuleDecl_ = true;

    if (!check(TokType::Identifier)) {
        error("expected module name after 'module'");
        return decl;
    }
    decl->name = advance().lexeme;
    match(TokType::Semicolon);
    return decl;
}

// feature-21 Phase 3（L1）：`fun f<T, U>` / `#fun g<T>` 名字后的可选泛型形参表。
// 非 '<' 起始 ⇒ 返回 false 且不消费 token（旧式声明零行为变化）。
bool Parser::parseOptionalTypeParams(std::vector<std::string>& out) {
    if (!check(TokType::Less)) return false;
    advance(); // <
    do {
        out.push_back(consume(TokType::Identifier,
                              "expected type parameter name").lexeme);
    } while (match(TokType::Comma));
    consume(TokType::Greater, "expected '>' after type parameters");
    return true;
}

std::unique_ptr<FunDecl> Parser::parseFunDecl() {
    auto tok = advance(); // fun
    auto decl = std::make_unique<FunDecl>();
    setNodePos(decl.get(), tok);

    auto& nameTok = consume(TokType::Identifier, "expected function name");
    decl->name = nameTok.lexeme;

    // .aurai 语法：fun path.new(...) / fun json.parse(...)
    if (match(TokType::Dot)) {
        auto& subName = consume(TokType::Identifier, "expected function name after '.'");
        decl->name = decl->name + "." + subName.lexeme;
    }

    // feature-21 Phase 3（L1）：可选显式泛型表 `fun id<T, U>(...)`。
    // 作用域覆盖参数/返回类型与函数体的解析（体内标注 T 同样解析为 GenericTypeRef）。
    parseOptionalTypeParams(decl->typeParams);
    TypeParamScope typeScope(*this, decl->typeParams);

    consume(TokType::LParen, "expected '(' after function name");
    if (!check(TokType::RParen)) {
        decl->params = parseParams();
    }
    consume(TokType::RParen, "expected ')' after parameters");
    if (peekSoftKeyword("where") && peekNext().type == TokType::LBrace) {
        advance(); // 'where'
        decl->where = parseWhereClause(false);
    }

    if (match(TokType::Throws)) decl->throws = true;

    if (match(TokType::Arrow)) {
        decl->returnType = parseType();
    }
    if (!decl->where && peekSoftKeyword("where") && peekNext().type == TokType::LBrace) {
        advance(); // 'where'
        decl->where = parseWhereClause(false);
    }

    // '...'：C++ 桥接标记（.aurai 声明文件用，aura 无实现 c++ 有实现）
    if (match(TokType::Ellipsis)) {
        decl->hasCppImpl = true;
    } else if (scanOnly_) {
        // feature-13 C2：声明级扫描模式 —— 跳过整个 body（消费 token，不建节点）。
        // 与 noBody_ 的区别：noBody_ 之后 token 停在 body 的 { 处（.aurai 路径）；
        // 此处必须把 body 消费掉，否则后续 parseDecl 从 body 内部继续 → 必然错位。
        // feature-13 C2：注册「本声明有实体 body」
        // （body 节点不建，但 FuncSkeleton.hasBody 需要这个信息）。
        decl->bodySkippedByScan = true;
        skipBlockTokens();
    } else if (!noBody_) {
        decl->body = parseBlock();
    }
    return decl;
}

std::unique_ptr<LetDecl> Parser::parseLetDecl() {
    return parseLetOrConstDeclBody<LetDecl>(advance(), "let");
}

std::unique_ptr<ConstDecl> Parser::parseConstDecl() {
    return parseLetOrConstDeclBody<ConstDecl>(advance(), "const");
}

std::unique_ptr<TypeDecl> Parser::parseTypeDecl() {
    auto tok = advance(); // type
    auto decl = std::make_unique<TypeDecl>();
    setNodePos(decl.get(), tok);

    auto& nameTok = consume(TokType::Identifier, "expected type name after 'type'");
    decl->name = nameTok.lexeme;

    // 泛型参数：type Name<T> 或 type Name<A, B>
    if (match(TokType::Less)) {
        do {
            auto& tp = consume(TokType::Identifier, "expected type parameter name");
            decl->typeParams.push_back(tp.lexeme);
        } while (match(TokType::Comma));
        consume(TokType::Greater, "expected '>' after type parameters");
    }
    if (peekSoftKeyword("where") && peekNext().type == TokType::LBrace) {
        advance(); // 'where'
        decl->where = parseWhereClause(false);
    }

    // .aurai 模式下：type Name 可作为前向声明（无 = TypeExpr）
    //
    // feature-13 C2（第二轮修正）：扫描态（scanOnly_）同样允许「无 = 的 type」。
    // 理由：声明级扫描面向的是「一个模块文件的声明骨架」，而
    // .aurai 声明文件（如 builtins/path.aurai 的 type Path）同样是模块文件。
    // 扫描若不接受这一形态，就会在 .aurai 上报错，而 C3 合并阶段
    // 正是要把 .aura 与 .aurai 放到同一套骨架里比对。
    // 注意：这里只放宽「无 = 也合法」，不改完整解析路径（noBody_）的行为。
    if (noBody_ || scanOnly_) {
        if (match(TokType::Assign)) {
            decl->type = parseType();
        }
        // 否则只是前向声明，不赋值 type 字段
        match(TokType::Semicolon);
        return decl;
    }

    consume(TokType::Assign, "expected '=' in type declaration");
    decl->type = parseType();
    match(TokType::Semicolon);
    return decl;
}

std::unique_ptr<TypeDecl> Parser::parseEnumDecl() {
    // 入口前提（由 parseDecl 的软关键字分支保证）：当前 token = Identifier("enum")，
    // 下一个 token = TokType::Type。下方 advance() 吃掉的是【标识符 enum】（不是关键字 token）。
    auto tok = advance(); // enum（Identifier；软关键字，A4）
    consume(TokType::Type, "expected 'type' after 'enum'");   // 防御性复查（正常恒成立）

    auto decl = std::make_unique<TypeDecl>();
    setNodePos(decl.get(), tok);

    auto& nameTok = consume(TokType::Identifier, "expected enum type name");
    decl->name = nameTok.lexeme;

    auto body = std::make_unique<EnumTypeExpr>();
    setNodePos(body.get(), nameTok);

    // [<绑定类型>]：统一用 parseType()（形态 2 的具体类型与形态 3 的 <T> 语法同形，语义留给 Sema）
    // ⚠️ 不填 decl->typeParams（enum 不参与泛型注册，见 §4.4-④）
    if (match(TokType::Less)) {
        do {
            body->bindingTypes.push_back(parseType());
        } while (match(TokType::Comma));
        consume(TokType::Greater, "expected '>' after enum type binding");
    }

    // .aurai（noBody_）/ 声明级扫描（scanOnly_）：允许 `enum type X` 前向声明（无 '='），
    // 与 parseTypeDecl 的既有放宽（src/Parser/DeclParser.cpp:152–159）同口径
    if ((noBody_ || scanOnly_) && !check(TokType::Assign)) {
        decl->type = std::move(body);
        match(TokType::Semicolon);
        return decl;
    }

    consume(TokType::Assign, "expected '=' in enum declaration");
    consume(TokType::LBrace, "expected '{' after '=' in enum declaration");

    // ⚠️ 不要照抄 TypeParser.cpp:57–66 的 do{...}while(match(Comma))——那里尾随逗号会报错；
    //    本处：',' 必需、尾随逗号允许、缺逗号 ⇒ break 后由 consume(RBrace) 报干净错误
    while (!check(TokType::RBrace) && !atEnd()) {
        EnumVariantDecl v;
        auto& vTok = consume(TokType::Identifier, "expected variant name in enum");
        v.name = vTok.lexeme;
        v.line = vTok.line;
        v.col  = vTok.col;
        if (match(TokType::Assign)) v.value = parseExpr();   // 值可以是任意表达式，语义由 Sema 拦截
        body->variants.push_back(std::move(v));
        if (!match(TokType::Comma)) break;
    }

    consume(TokType::RBrace, "expected '}' after enum body");
    decl->type = std::move(body);
    match(TokType::Semicolon);
    return decl;
}

std::unique_ptr<InterfaceDecl> Parser::parseInterfaceDecl() {
    auto tok = advance(); // interface
    auto decl = std::make_unique<InterfaceDecl>();
    setNodePos(decl.get(), tok);

    auto& nameTok = consume(TokType::Identifier, "expected interface name");
    decl->name = nameTok.lexeme;

    // 泛型参数：interface Iterator<T> / interface Comparable<T>
    if (match(TokType::Less)) {
        do {
            auto& tp = consume(TokType::Identifier, "expected type parameter in interface");
            decl->typeParams.push_back(tp.lexeme);
        } while (match(TokType::Comma));
        consume(TokType::Greater, "expected '>' after interface type parameters");
    }
    if (peekSoftKeyword("where") && peekNext().type == TokType::LBrace) {
        advance(); // 'where'
        decl->where = parseWhereClause(false);
    }

    consume(TokType::LBrace, "expected '{' after interface name");

    while (!check(TokType::RBrace) && !atEnd()) {
        // 错误恢复：当前 token 不是方法名（Identifier）时，跳过到下一个方法名或 '}'，
        // 避免 consume 报错不前进导致的死循环
        if (!check(TokType::Identifier)) {
            error("expected method name in interface");
            while (!atEnd() && !check(TokType::RBrace) && !check(TokType::Identifier)) {
                advance();
            }
            continue;
        }
        decl->methods.push_back(parseInterfaceMethodSig());
    }

    consume(TokType::RBrace, "expected '}' after interface body");
    return decl;
}

std::unique_ptr<MethodDecl> Parser::parseMethodDecl() {
    auto tok = advance(); // fun
    auto decl = std::make_unique<MethodDecl>();
    setNodePos(decl.get(), tok);

    consume(TokType::LParen, "expected '(' for receiver");
    auto& recvName = consume(TokType::Identifier, "expected receiver name");
    decl->receiverName = recvName.lexeme;
    // feature-21 Phase 1（S5）：编译期方法 `fun (self #Command) run()`（§3.12）
    bool hashRecv = match(TokType::Hash);
    auto& recvType = consume(TokType::Identifier,
                             hashRecv ? "expected type name after '#' in receiver"
                                      : "expected receiver type");
    decl->receiverType = (hashRecv ? "#" : "") + recvType.lexeme;

    // 泛型接收者类型参数：fun (self Stack<T>) 中的 <T>
    if (match(TokType::Less)) {
        do {
            auto& tp = consume(TokType::Identifier, "expected type parameter name");
            decl->receiverTypeArgs.push_back(tp.lexeme);
        } while (match(TokType::Comma));
        consume(TokType::Greater, "expected '>' after type parameters");
    }

    // 接口实现声明：fun (self User impl Greetable) greet() -> string
    // impl 位于接收者类型之后、')' 之前；泛型接口可带类型实参：
    // fun (self Point impl Comparable<Point>) cmp(other: Point) -> int
    if (match(TokType::Impl)) {
        auto& implTok = consume(TokType::Identifier, "expected interface name after 'impl'");
        decl->implInterface = implTok.lexeme;
        if (match(TokType::Less)) {
            do {
                decl->implTypeArgs.push_back(parseType());
            } while (match(TokType::Comma));
            consume(TokType::Greater, "expected '>' after interface type arguments");
        }
    }

    consume(TokType::RParen, "expected ')' after receiver");

    auto& nameTok = consume(TokType::Identifier, "expected method name");
    decl->name = nameTok.lexeme;

    // 构造函数：方法名与接收者类型名相同
    if (decl->name == decl->receiverType) {
        decl->isConstructor = true;
    }

    consume(TokType::LParen, "expected '(' after method name");
    if (!check(TokType::RParen)) {
        decl->params = parseParams();
    }
    consume(TokType::RParen, "expected ')' after parameters");
    if (peekSoftKeyword("where") && peekNext().type == TokType::LBrace) {
        advance(); // 'where'
        decl->where = parseWhereClause(false);
    }

    if (match(TokType::Throws)) decl->throws = true;

    if (match(TokType::Arrow)) {
        decl->returnType = parseType();
    }
    if (!decl->where && peekSoftKeyword("where") && peekNext().type == TokType::LBrace) {
        advance(); // 'where'
        decl->where = parseWhereClause(false);
    }

    // '...'：C++ 桥接标记（.aurai 声明文件用，aura 无实现 c++ 有实现）
    if (match(TokType::Ellipsis)) {
        decl->hasCppImpl = true;
    } else if (scanOnly_) {
        // feature-13 C2：声明级扫描模式 —— 跳过整个方法体（同 parseFunDecl）
        // feature-13 C2：同 parseFunDecl，记录实体 body 存在。
        decl->bodySkippedByScan = true;
        skipBlockTokens();
    } else if (!noBody_) {
        decl->body = parseBlock();
    }
    return decl;
}

std::unique_ptr<ImportDecl> Parser::parseImportDecl() {
    auto tok = advance(); // import
    auto decl = std::make_unique<ImportDecl>();
    setNodePos(decl.get(), tok);

    if (check(TokType::StringLiteral)) {
        // import "path/to/module.aura"  或  import "path" as alias
        auto& strTok = consume(TokType::StringLiteral, "expected string literal after 'import'");
        if (auto* s = std::get_if<std::string>(&strTok.literal)) {
            decl->path = *s;
        } else {
            decl->path = strTok.lexeme;
        }
        decl->isBuiltin = false;
    } else if (check(TokType::Identifier)) {
        // import path 或 import json as j  (内置模块/外部包)
        decl->path = advance().lexeme;
        decl->isBuiltin = true;
    } else {
        error("expected string literal or identifier after 'import'");
        return decl;
    }

    // 可选的 as 别名
    if (check(TokType::Identifier) && peek().lexeme == "as") {
        advance(); // skip "as"
        if (check(TokType::Identifier)) {
            decl->alias = advance().lexeme;
        } else {
            error("expected identifier after 'as'");
        }
    }

    match(TokType::Semicolon);
    return decl;
}

} // namespace Aura
