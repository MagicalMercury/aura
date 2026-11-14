#pragma once

#include "AST/ASTNode.h"
#include "AST/Stmt.h"
#include "AST/Type.h"
#include "Token.h"
#include "Diag/DiagnosticEngine.h"
#include <memory>
#include <vector>
#include <algorithm>
#include <string>

namespace Aura {

// feature-21 Phase 2（N-31）：`Aura{...}` 引述体深度守卫。
// RAII：parseQuoteBody 入口 ++，离开（含提前 return）--；嵌套引述按层累加。
struct QuoteDepthGuard {
    int& depth;
    explicit QuoteDepthGuard(int& d) : depth(d) { ++depth; }
    ~QuoteDepthGuard() { --depth; }
    QuoteDepthGuard(const QuoteDepthGuard&) = delete;
    QuoteDepthGuard& operator=(const QuoteDepthGuard&) = delete;
};

class Parser {
public:
    explicit Parser(std::vector<Token> tokens, DiagnosticEngine& diag);

    // 解析完整程序
    [[nodiscard]] std::unique_ptr<Program> parse();

    // 解析 .aurai 接口声明文件（跳过函数体，禁止实现语句）
    [[nodiscard]] std::unique_ptr<Program> parseAurai();

    // 获取词法错误列表
    const std::vector<std::string>& errors() const { return diag_.errorMessages(); }

    // feature-13 C2（2026-09-17）：声明级扫描入口 —— 产出「只含声明骨架、无函数体」的 AST。
    // 语义：与 parse() 结果逐节点同构，唯一差异是 FunDecl/MethodDecl/InterfaceMethodSig 的
    //       body/defaultBody 一律为 nullptr（函数体 token 被【跳过】而非【不消费】）。
    // 用途：为 DeclUnit 提供「扫描产物 == 完整解析的声明部分」的可比对基准。
    // ⚠️ 独立性：不复用 noBody_（.aurai 语义是「不消费 body token」，与「跳配对」不同，
    //       混用会造成两种语义纠缠）——使用独立成员 scanOnly_，见其声明处说明。
    [[nodiscard]] std::unique_ptr<Program> parseDeclarationsOnly();

private:
    bool noBody_ = false;  // .aurai 模式：跳过函数体
    // feature-13 C2（2026-09-17）：声明级扫描模式。
    // 与 noBody_ 【并列但不复用】——两者语义不同：
    //   noBody_  = 「不解析 body」：token 停在 body 的 '{' 处（.aurai 专用，配合 '...'）
    //   scanOnly_= 「跳过 body」 : 数 { } 配对把整个 body 消费掉，token 停在下一个声明起始。
    // 混用一个标志会让 parseAurai 的既有行为（token 停在 '{'）与扫描行为纠缠，
    // 故独立成员；既有路径（parse/parseAurai）不读本标志 → 行为逐字不变。
    bool scanOnly_ = false;
    // feature-21 Phase 2（N-31）：引述体嵌套深度（>0 ⇒ 位于 `Aura{...}` 内）。
    // 由 parseQuoteBody 的 QuoteDepthGuard 维护；parseHashStmt 的 `#const` 分支读取。
    int quoteDepth_ = 0;
    // feature-13 C0（2026-09-17）：「module」声明的软关键字支持 + 位置约束。
    // module 是【软关键字】（非 TokType::Module）——保持 `let module = 1` 可用
    //（向后兼容红线，探针 1 实测）。识别方式与既有 `as` 同款：Identifier + 文本比对。
    bool seenImport_ = false;   // 已见 import/声明 → 迟到的 module 声明报错
    bool sawModuleDecl_ = false; // 已见 module 声明 → 重复声明报错
    // --- 辅助 ---
    Token& advance();
    Token& peek();
    Token& peekNext();
    // feature-21 Phase 1：任意前瞻 token（只读不消费；越界回落到末位 Eof）
    Token& peekAt(size_t k);
    bool check(TokType type);
    bool match(TokType type);
    Token& consume(TokType type, const std::string& msg);
    // feature-21 Phase 1：成员名位置放行硬关键字 `type`（`reflect.type` / `#reflect.type`）。
    // 本批只定义；S6 已把成员访问调用点统一替换为 consumeMemberName()。
    Token& consumeMemberName();
    bool atEnd() const;
    void error(const std::string& msg);
    // feature-21 Phase 1（S4）：把诊断定位到指定 token（挂载错误必须报在 `declStart`
    // 而不是当前 `peek()`，否则 `#A{...}\nlet y = 1` 会被报到下一行声明上）。
    void errorAt(const Token& tok, const std::string& msg);
    void setNodePos(ASTNode* node, const Token& tok);
    static bool isKeywordIdent(TokType t) { return t >= TokType::Fun && t <= TokType::None; }
    // 软关键字判定：当前 token 是 Identifier 且文本等于 kw
    bool peekSoftKeyword(const char* kw);

    // --- 错误恢复 ---
    void synchronize();                // 跳到下一个安全恢复点
    void synchronizeTo(TokType type);  // 跳到特定 token 类型

    // --- 解析声明 ---
    std::unique_ptr<Decl> parseDecl();
    // feature-13 C2：跳过一对配对花括号（不建任何 AST 节点）。
    // 仅数 LBrace/RBrace 深度；字符串/注释内的花括号由 Lexer 吞掉（不产 token），
    // 故配对安全（探针 3 实测）。当前 token 不是 '{' 时原样返回（如 '...' 桥接形态）。
    void skipBlockTokens();
    std::unique_ptr<FunDecl> parseFunDecl();
    std::unique_ptr<LetDecl> parseLetDecl();
    std::unique_ptr<ConstDecl> parseConstDecl();
    std::unique_ptr<TypeDecl> parseTypeDecl();
    // 方案 B：enum 复用 TypeDecl，type = EnumTypeExpr（见 change.md §4.4-②）
    std::unique_ptr<TypeDecl> parseEnumDecl();
    std::unique_ptr<InterfaceDecl> parseInterfaceDecl();
    std::unique_ptr<MethodDecl> parseMethodDecl();
    std::unique_ptr<ImportDecl> parseImportDecl();
    // feature-13 C0：`module <ident>` 声明（文件首行、import 之前；缺失时回落文件 stem）
    std::unique_ptr<ModuleDecl> parseModuleDecl();

    // feature-21 Phase 1：`#` 元数据语法骨架（S3 基础设施）
    // （`#type` / `#fun` / `#const` 的解析属 S4/S5，本批不声明）
    std::vector<std::unique_ptr<ASTNode>> parseMountPrefixes();
    std::unique_ptr<ASTNode> parseMetaConstructAfterHash();
    std::unique_ptr<ASTNode> parseHashCallAfterHash();
    // feature-21 Phase 2（S3，D2）：裸 `#name` —— 供「裸名前缀」与「语句位裸名」复用。
    // 进入时 current = `#`；成功消费 '#' + name，失败消费 '#' 后返回 nullptr（§0.3）。
    std::unique_ptr<ASTNode> parseMetaValueRefAfterHash();

    // feature-21 Phase 1（S4）：`#` 声明与 `where` 子句
    std::unique_ptr<Decl> parseHashDecl();
    std::unique_ptr<MetaTypeDecl> parseMetaTypeDecl();
    // feature-21 Phase 1（S5）：`#fun` / `#const`
    std::unique_ptr<FunDecl> parseMetaFunDecl();
    std::unique_ptr<HashConstDecl> parseHashConstDecl();
    std::unique_ptr<WhereClause> parseWhereClause(bool isMount);
    Bound parseBound(bool allowThrows = false);
    std::unique_ptr<TypeExpr> parseMetaTypeName();

    // feature-21 Phase 1（S6）：表达式/语句侧的 `#` 语法
    std::unique_ptr<ASTNode> parseHashExpr();
    std::unique_ptr<ASTNode> parseMetaSuffix(std::unique_ptr<ASTNode> base,
                                             const Token& posTok);
    std::unique_ptr<Stmt> parseHashStmt();
    // feature-21 Phase 2（D5）：`Aura{...}` 引述体 —— 允许普通声明（import / type /
    // enum type / interface / pub 声明 / fun）+ 普通语句；禁止编译期声明
    // （`#const`/`#type`/`#fun`，含 `pub` 前缀）。入口 consume '{'，用 QuoteDepthGuard 维护 quoteDepth_。
    std::unique_ptr<BlockStmt> parseQuoteBody();

    // --- feature-21 Phase 3（L1）：显式泛型 `<...>` ---
    // 当前函数声明正在解析的显式泛型名（仅声明位有效：旧式 `fun f(x: <T>)` 不设置
    // 本成员 ⇒ 类型标注解析路径逐字不变）。
    // 用途：`fun f<T>(x: T)` 的类型标注中，命中本表的裸标识符解析为 GenericTypeRef，
    // 与旧式 `<T>` 共用同一个 AST 表示（Sema 无需第二套机制）。
    std::vector<std::string> activeTypeParams_;
    bool isActiveTypeParam(const std::string& name) const {
        return std::find(activeTypeParams_.begin(), activeTypeParams_.end(), name)
               != activeTypeParams_.end();
    }
    // 活动泛型名的作用域守卫：构造时接管 names、析构时恢复原值（含异常/提前 return）。
    // 嵌套在 Parser 内以便访问 activeTypeParams_（私有）。
    struct TypeParamScope {
        Parser& p;
        std::vector<std::string> saved;
        TypeParamScope(Parser& parser, const std::vector<std::string>& names)
            : p(parser), saved(parser.activeTypeParams_) {
            p.activeTypeParams_ = names;
        }
        ~TypeParamScope() { p.activeTypeParams_ = std::move(saved); }
        TypeParamScope(const TypeParamScope&) = delete;
        TypeParamScope& operator=(const TypeParamScope&) = delete;
    };
    // `fun f<T, U>` / `#fun g<T>` 名字后的可选泛型形参表；非 '<' 起始时返回 false
    // 且不消费任何 token（'<' 起始时按声明位语义必然消费到 '>'）。
    bool parseOptionalTypeParams(std::vector<std::string>& out);

    // --- 解析语句 ---
    std::unique_ptr<Stmt> parseStmt();
    std::unique_ptr<BlockStmt> parseBlock();
    std::unique_ptr<Stmt> parseIfStmt();
    std::unique_ptr<Stmt> parseWhileStmt();
    std::unique_ptr<Stmt> parseLoopStmt();
    std::unique_ptr<Stmt> parseForStmt();
    std::unique_ptr<Stmt> parseReturnStmt();
    std::unique_ptr<Stmt> parseThrowStmt();
    std::unique_ptr<Stmt> parseTryCatchStmt();
    std::unique_ptr<Stmt> parseSyncStmt();
    std::unique_ptr<Stmt> parseSyncForStmt();
    std::unique_ptr<Stmt> parseSyncForRest(Token& syncTok, bool isThread);  // sync for / sync thread for 共享解析
    std::unique_ptr<Stmt> parseSpawnStmt();
    std::unique_ptr<Stmt> parseLockStmt();   // lock (m) { } 块语句
    std::unique_ptr<Stmt> parseMatchStmt();
    std::unique_ptr<Stmt> parseExprStmt();

    // 共用 let/const 解析（模板方法）
    template <typename DeclT>
    std::unique_ptr<DeclT> parseLetOrConstDeclBody(Token tok, const char* kw) {
        auto decl = std::make_unique<DeclT>();
        setNodePos(decl.get(), tok);
        std::string errMsg = std::string("expected name after '") + kw + "'";
        auto& nameTok = consume(TokType::Identifier, errMsg);
        decl->name = nameTok.lexeme;
        if (match(TokType::Comma)) {
            // 解构 let a, b = f()：字段类型取自初始值推断，不允许类型注解
            decl->names.push_back(nameTok.lexeme);
            do {
                auto& nTok = consume(TokType::Identifier,
                    "expected name after ',' in destructuring declaration");
                decl->names.push_back(nTok.lexeme);
            } while (match(TokType::Comma));
        } else {
            if (match(TokType::Colon)) decl->type = parseType();
        }
        consume(TokType::Assign, "expected '=' in declaration");
        decl->initializer = parseExpr();
        match(TokType::Semicolon);
        return decl;
    }

    // --- 解析表达式 ---
    using ParseFn = std::unique_ptr<ASTNode> (Parser::*)();
    std::unique_ptr<ASTNode> parseBinaryLevel(ParseFn next, const TokType* ops, size_t opCount);
    std::unique_ptr<ASTNode> parseExpr();
    std::unique_ptr<ASTNode> parseAssignment();
    std::unique_ptr<ASTNode> parseConditional();
    std::unique_ptr<ASTNode> parsePipe();
    std::unique_ptr<ASTNode> parseOr();
    std::unique_ptr<ASTNode> parseAnd();
    std::unique_ptr<ASTNode> parseEquality();
    std::unique_ptr<ASTNode> parseComparison();
    std::unique_ptr<ASTNode> parseAddSub();
    std::unique_ptr<ASTNode> parseMulDiv();
    std::unique_ptr<ASTNode> parseUnary();
    std::unique_ptr<ASTNode> parseCall();
    std::unique_ptr<ASTNode> parsePrimary();

    // N2：调用点显式类型实参 `B<int>(...)` 与比较运算 `a < b` 的语法消歧。
    // lookaheadTypeArgsBeforeCall：仅当当前 '<' 后是合法类型序列、以 '>' 收尾且紧跟
    // '(' 时才视为显式类型实参（只读不消费 token）；skipTypeTokens：从 tokens_[i] 跳过
    // 一个类型（Identifier / ns.Ident / Ident<T1,T2>，含内置类型名 int/string 等），
    // 成功推进 i 返回 true。参照 C++17 模板实参优先规则。
    bool lookaheadTypeArgsBeforeCall();
    bool skipTypeTokens(size_t& i) const;
    // bug-51：record 字面量显式类型实参 `Box<int> { value = 7 }` 的前瞻判型——
    // 与 N2 的 lookaheadTypeArgsBeforeCall（只认 `>` 后 `(`）分工：本函数认 `>` 后
    // `{ Ident =` / `{}`（record 字面量体）。语句头抑制（suppressNamedRecordLiteral_）
    // 下恒 false（`{` 属语句体，与既有具名 record 分支判据一致）。只读不消费 token。
    bool lookaheadTypeArgsBeforeRecord();

    // --- 解析闭包 ---
    std::unique_ptr<ASTNode> parseFunExpr();

    // --- 解析类型 ---
    std::unique_ptr<TypeExpr> parseType();
    std::unique_ptr<TypeExpr> parseUnionType();
    std::unique_ptr<TypeExpr> parsePrimaryType();

    // --- 解析模式 ---
    std::unique_ptr<Pattern> parsePattern();
    // P5：单个模式（parsePattern 的 `|` 分组内单元）
    std::unique_ptr<Pattern> parseSinglePattern();

    // --- 解析参数 ---
    Param parseParam();
    std::vector<Param> parseParams();
    InterfaceMethodSig parseInterfaceMethodSig();

    // #5：具名 record 字面量 `Point { x = 1, y = 2 }` 的语句头抑制标志。
    // parseIfStmt/parseWhileStmt/parseForStmt/parseSyncForRest 的语句头表达式
    // （condition/iterable）位置置位，防止 `Ident { Ident =` 与语句体同形
    // （used/5.aura `for v in ch26 { v26 = v }` 回归红线）被 parseCall 误吞为具名
    // record 字面量。解析语句头表达式前后保存/置位/恢复（防嵌套语句头污染）。
    bool suppressNamedRecordLiteral_ = false;

    // --- 数据 ---
    std::vector<Token> tokens_;
    size_t currentIdx_ = 0;
    DiagnosticEngine& diag_;
};

} // namespace Aura
