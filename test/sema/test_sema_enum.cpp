// ============================================================
// test_sema_enum.cpp — Sema 面 enum type 单元测试（feature-20 批 2/4）
//
// 覆盖 change.md §4.7.3 的用例清单：
//   Step 5（§4.4-④）类型系统/注册/成员访问/AST 标记（E-1/F-1）
//   Step 6（§4.4-⑤）约束校验 + 编译期常量 + I-1 的 `enum | None` 拦截
//   Step 7（§4.4-⑥）深只读
//   Step 8（§4.4-⑦）`==` / `!=` 身份判等
//   Step 9（§4.4-⑧）match 穷尽性 + 裸名/限定名定向 + 无法定向报错
//
// ⚠️ 用例源【不带 payload 变体语法】（附八 D-3：v1 不支持 `{ Some(T) }`）
// ⚠️ 形态 3 用例源用 `enum type Option<T> = { Some }`（Parser 收得下、Sema 报「v1 未支持」）
// ============================================================
#include "framework/test_framework.h"
#include "framework/test_helpers.h"

#include <filesystem>
#include <fstream>
#include <string>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace aura_test;

namespace {

// 创建唯一临时目录（与 test_sema_modules.cpp 一致）
std::string enumTempDir() {
    auto base = std::filesystem::temp_directory_path();
    auto dir = base / ("aura_sema_enum_" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir.string();
}

std::string writeModAura(const std::string& dir, const std::string& name,
                         const std::string& content) {
    std::string path = (std::filesystem::path(dir) / name).string();
    std::ofstream f(path);
    f << content;
    return path;
}

// 在顶层 fun 的体块内按名取第一个 let 绑定的 initializer
// （本文件用例源形态可控：FunDecl → BlockStmt → LetDecl）
const Aura::ASTNode* findLetInitializer(const Aura::Program& p, const std::string& varName) {
    for (auto& d : p.decls) {
        auto* f = dynamic_cast<const Aura::FunDecl*>(d.get());
        if (!f || !f->body) continue;
        for (auto& s : f->body->stmts) {
            if (auto* ld = dynamic_cast<const Aura::LetDecl*>(s.get())) {
                if (ld->name == varName) return ld->initializer.get();
            }
        }
    }
    return nullptr;
}

// 取 let 初始值里的 MemberAccessExpr（E-1 的 AST 标记断言用）
const Aura::MemberAccessExpr* findLetInitializerMemberAccess(const Aura::Program& p,
                                                             const std::string& varName) {
    const Aura::ASTNode* init = findLetInitializer(p, varName);
    if (!init) return nullptr;
    return dynamic_cast<const Aura::MemberAccessExpr*>(init);
}

} // namespace

// ============================================================
// Step 5：正例（值访问 / 同名 variant / 类型标注 / 空 enum / 方法）
// ============================================================
TEST(SemaEnum, EnumPlainValueAccess) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "enum type Color = { Red, Green }"
        " fun main(io: Io) { let c = Color.Red }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaEnum, EnumSameNameVariant) {
    // 裁定 A 验收：variant 与类型同名（variant 挂在类型内部成员表，不进符号表）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "enum type Foo = { Foo }"
        " fun main(io: Io) { let x = Foo.Foo }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaEnum, EnumValueIsNotGC_AssignToTypedVar) {
    // 类型标注可用 ⇒ resolveNamedType 的 TypeAlias 通路（deep-clone variants）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "enum type Color = { Red, Green }"
        " fun main(io: Io) { let c: Color = Color.Red; let d: Color = Color.Green }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaEnum, EnumEmptyIsLegal) {
    // 空 enum（0 variant）合法：Sema 不报错（不可构造值的面在 CodeGen 批断言）
    Aura::DiagnosticEngine diag;
    analyzeSource("enum type Never = { } fun main(io: Io) { }", diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaEnum, EnumMethodDefinedAndCalled) {
    // recordTypeKey 通路验证：enum 方法进 typeMethods_、调用点命中
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "enum type Color = { Red, Green }"
        " fun (self Color) hex() -> int { return 0 }"
        " fun main(io: Io) { let c = Color.Red; let h = c.hex() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// Step 5：E-1 —— Sema 定夺的 AST 标记 enumVariantAccess
// ============================================================
TEST(SemaEnum, EnumVariantAccessFlagSet) {
    // 正例：类型名 + 命中 variant ⇒ 标记置位（CodeGen 只读该标记）
    Aura::DiagnosticEngine diag;
    auto prog = analyzeSource(
        "enum type Color = { Red }"
        " fun main(io: Io) { let c = Color.Red }",
        diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(diag.hasErrors());
    auto* ma = findLetInitializerMemberAccess(*prog, "c");
    ASSERT_TRUE(ma != nullptr);
    EXPECT_TRUE(ma->enumVariantAccess);
}

TEST(SemaEnum, EnumVariantAccessShadowedByVariable) {
    // E-1 负例守护：内层 `let Color = <record 值>` 遮蔽类型名后，`Color.x` 必须按
    // 【普通成员访问】处理 —— ①不得报 `enum type 'Color' has no variant`；
    // ②该 MemberAccessExpr 的 enumVariantAccess == false；③普通成员访问错误文案不含 enum 前缀
    Aura::DiagnosticEngine diag;
    auto prog = analyzeSource(
        "enum type Color = { Red }"
        " type Box = { x: int }"
        " fun main(io: Io) { let Color = Box{x=1}; let y = Color.x }",
        diag);
    ASSERT_TRUE(prog != nullptr);
    EXPECT_FALSE(hasErrorContaining(diag, "enum type 'Color' has no variant"));
    auto* ma = findLetInitializerMemberAccess(*prog, "y");
    ASSERT_TRUE(ma != nullptr);
    EXPECT_FALSE(ma->enumVariantAccess);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaEnum, EnumVariantAccessShadowedByVariableBadMember) {
    // E-1 负例（带错字段）：只报普通成员访问类错误，文案不含 `enum type` 前缀
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "enum type Color = { Red }"
        " type Box = { x: int }"
        " fun main(io: Io) { let Color = Box{x=1}; let y = Color.zz }",
        diag);
    EXPECT_TRUE(hasErrorContaining(diag, "has no field 'zz'"));
    EXPECT_FALSE(hasErrorContaining(diag, "enum type"));
}

// ============================================================
// Step 5/6：结构类负例（未知 variant / E013）
// ============================================================
TEST(SemaEnum, EnumUnknownVariantAccess) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "enum type Color = { Red }"
        " fun main(io: Io) { let c = Color.Purple }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorContaining(diag, "has no variant 'Purple'"));
}

TEST(SemaEnum, EnumMethodMissingE013) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "enum type Color = { Red, Green }"
        " fun main(io: Io) { let c = Color.Red; let h = c.nope() }",
        diag);
    EXPECT_TRUE(hasErrorCode(diag, Aura::DiagCode::E013_MethodNotFound));
    EXPECT_TRUE(hasErrorContaining(diag, "enum type 'Color' has no method 'nope'"));
}

// ============================================================
// Step 6：约束校验（形态 1）
// ============================================================
TEST(SemaEnum, EnumDuplicateVariantName) {
    Aura::DiagnosticEngine diag;
    analyzeSource("enum type Color = { Red, Red } fun main(io: Io) { }", diag);
    EXPECT_TRUE(hasErrorContaining(diag, "duplicate enum variant"));
}

TEST(SemaEnum, EnumDuplicateExplicitValue) {
    Aura::DiagnosticEngine diag;
    analyzeSource("enum type Color = { Red = 1, Green = 1 } fun main(io: Io) { }", diag);
    EXPECT_TRUE(hasErrorContaining(diag, "duplicate explicit enum value"));
}

TEST(SemaEnum, EnumNonIntegerExplicitValue) {
    Aura::DiagnosticEngine diag;
    analyzeSource("enum type Color = { Red = \"x\" } fun main(io: Io) { }", diag);
    EXPECT_TRUE(hasErrorContaining(diag, "must be an integer constant"));
}

TEST(SemaEnum, EnumNegativeExplicitValue) {
    // ⚠️ 与 change.md §4.7.3 的「must be non-negative」不同：`-1` 被 Parser 解析为
    //    UnaryExpr（不是 IntLiteral）⇒ 落到「非整数常量」分支 ⇒ `i->value < 0` 不可达。
    //    实测行为如下（见报告 §报红 B2-4）。
    Aura::DiagnosticEngine diag;
    analyzeSource("enum type Color = { Red = -1 } fun main(io: Io) { }", diag);
    EXPECT_TRUE(hasErrorContaining(diag, "must be an integer constant"));
    EXPECT_FALSE(hasErrorContaining(diag, "must be non-negative"));
}

TEST(SemaEnum, EnumVariantNameReservedForShell) {
    // §4.4-⑤ (a2)：壳保留名 `_Tag` / `_tag`（§4.9.4-L15）
    Aura::DiagnosticEngine diag;
    analyzeSource("enum type E = { _Tag, _tag } fun main(io: Io) { }", diag);
    EXPECT_TRUE(hasErrorContaining(diag, "is reserved for the generated enum shell"));
}

// ============================================================
// Step 6：约束校验（形态 2）+ 形态 3 拦截
// ============================================================
TEST(SemaEnum, EnumBoundVariantMissingValue) {
    Aura::DiagnosticEngine diag;
    analyzeSource("enum type C<string> = { a } fun main(io: Io) { }", diag);
    EXPECT_TRUE(hasErrorContaining(diag, "must have an explicit value"));
}

TEST(SemaEnum, EnumBoundValueTypeMismatch) {
    Aura::DiagnosticEngine diag;
    analyzeSource("enum type C<string> = { a = 1 } fun main(io: Io) { }", diag);
    EXPECT_TRUE(hasErrorContaining(diag, "does not match enum binding type"));
}

TEST(SemaEnum, EnumBoundValueNotCompileTimeConstant) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "enum type C<string> = { a = string_concat(\"x\") }"
        " fun main(io: Io) { }",
        diag);
    EXPECT_TRUE(hasErrorContaining(diag, "must be a compile-time constant"));
}

TEST(SemaEnum, EnumBoundPlainWorks) {
    // 形态 2 正例：绑定 string + 关联值访问 + 同 enum 比较 + 穷尽 match
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "enum type ErrorCode<string> = { not_found = \"NOT_FOUND\", permission = \"PERMISSION\" }"
        " fun main(io: Io) {"
        "   let e = ErrorCode.not_found;"
        "   let b = (e == ErrorCode.permission);"
        "   match e { not_found => { } permission => { } } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaEnum, EnumGenericBindingRejected) {
    // 形态 3（enum 级 `<T>` 绑定）：Sema 明确报「v1 未支持」（源不带 payload，D-3 口径）
    Aura::DiagnosticEngine diag;
    analyzeSource("enum type Option<T> = { Some } fun main(io: Io) { }", diag);
    EXPECT_TRUE(hasErrorContaining(diag, "not supported in v1"));
}

// ============================================================
// Step 6：I-1 —— `enum | None` 显式报错（不再静默折叠为 Optional）
// ============================================================
TEST(SemaEnum, EnumInUnionWithNoneRejected) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "enum type Color = { Red }"
        " fun main(io: Io) { let c: Color | None = none() }",
        diag);
    EXPECT_TRUE(hasErrorContaining(diag, "cannot form a union with 'None'"));
}

// ============================================================
// Step 7：深只读（三负例 + 两正例守护）
// ============================================================
TEST(SemaEnum, EnumDeepReadonlyMemberWrite) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "enum type Color = { Red }"
        " fun main(io: Io) { let p = Color.Red; p.hp = 50 }",
        diag);
    EXPECT_TRUE(hasErrorContaining(diag, "read-only value derived from an enum variant"));
}

TEST(SemaEnum, EnumDeepReadonlyIndexWrite) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "enum type Color = { Red }"
        " fun main(io: Io) { let p = Color.Red; p.inventory[0] = 1 }",
        diag);
    EXPECT_TRUE(hasErrorContaining(diag, "read-only value derived from an enum variant"));
}

TEST(SemaEnum, EnumDeepReadonlyAliasWrite) {
    // 别名逃逸：`let q = p.inventory` 后 `q[0] = w`（readOnly 沿根 Identifier 传播）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "enum type Color = { Red }"
        " fun main(io: Io) { let p = Color.Red; let q = p.inventory; q[0] = 1 }",
        diag);
    EXPECT_TRUE(hasErrorContaining(diag, "read-only value derived from an enum variant"));
}

TEST(SemaEnum, EnumRebindAllowed) {
    // 二分规则守护：readOnly 不拦【重新绑定】（let 语义不变；非 const）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "enum type Color = { Red, Green }"
        " fun main(io: Io) { let c = Color.Red; c = Color.Green }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaEnum, PlainRecordMemberWriteStillOK) {
    // 不误伤守护：普通 record 值写字段仍合法
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "type Box = { x: int }"
        " fun main(io: Io) { let p = Box{x=1}; p.x = 5 }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaEnum, EnumDeepReadonlyAliasWriteDiagOrderFirst) {
    // L-4（T5-C2）：别名写场景下**首条**错误必须是 member access 的精准错误（根因），
    //   而非 `let` 侧 `cannot infer element type ...` 级联。
    //   语句顺序：`let q = p.inventory`（成员错误，先）→ `q[0] = 1`（写错误，后）
    //   ⇒ 两者不可能同为首条；按 §6 Step 8 / §8 T5 裁定：首条 = C2 的成员错误，
    //     写错误仍必须存在（不许被 C2 吞掉）。
    //   现有 hasErrorContaining 遍历全部错误、抓不到顺序（test_helpers.h:210-215）⇒ 本用例直接取 [0]。
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "enum type Color = { Red }"
        " fun main(io: Io) { let p = Color.Red; let q = p.inventory; q[0] = 1 }",
        diag);
    ASSERT_TRUE(diag.hasErrors());
    const auto& msgs = diag.errorMessages();
    ASSERT_TRUE(!msgs.empty());
    // 首条 = C2 的精准成员错误（根因，先于写错误）
    EXPECT_TRUE(msgs[0].find("has no member") != std::string::npos);
    // 写错误仍必须存在（不许被 C2 吞掉）
    EXPECT_TRUE(hasErrorContaining(
        diag, "cannot modify a read-only value derived from an enum variant"));
}

TEST(SemaEnum, EnumMethodCallNotMaskedByMemberDiagnostic) {
    // 不误伤守护（C2）：enum **方法调用**走 MethodCallExpr / inferMethodCall，不经
    //   inferMemberAccess 的 enum 兜底 ⇒ 方法调用必须仍编译通过。
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "enum type Color = { Red }"
        " fun (self Color) describe() -> string { return \"x\" }"
        " fun main(io: Io) { let s = Color.Red.describe() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// Step 8：`==` / `!=` 按 variant 身份（只做 ==，hash 不做）
// ============================================================
TEST(SemaEnum, EnumEqSameVariant) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "enum type Color = { Red }"
        " fun main(io: Io) { let b = (Color.Red == Color.Red) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaEnum, EnumEqDifferentVariantNoError) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "enum type Color = { Red, Green }"
        " fun main(io: Io) { let b = (Color.Red != Color.Green) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaEnum, EnumEqCrossEnumRejected) {
    // N5 取向守护：同名但不同类型的两个 enum 不得静默等价
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "enum type A = { X }"
        " enum type B = { X }"
        " fun main(io: Io) { let b = (A.X == B.X) }",
        diag);
    EXPECT_TRUE(hasErrorContaining(diag, "cannot compare enum value of type"));
}

TEST(SemaEnum, EnumEqWithIntRejected) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "enum type A = { X }"
        " fun main(io: Io) { let b = (A.X == 5) }",
        diag);
    EXPECT_TRUE(hasErrorContaining(diag, "cannot compare enum value of type"));
}

TEST(SemaEnum, EnumOrderedCompareRejected) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "enum type A = { X, Y }"
        " fun main(io: Io) { let b = (A.X < A.Y) }",
        diag);
    EXPECT_TRUE(hasErrorContaining(diag, "only '==' and '!='"));
}

// ============================================================
// Step 8：F-1 —— 跨模块同名 enum 不静默等价（declModule 判等）
// ============================================================
TEST(SemaEnum, EnumEqCrossModuleSameNameRejected) {
    auto dir = enumTempDir();
    writeModAura(dir, "a.aura",
        "pub enum type Color = { X }\n"
        "pub fun mk() -> Color { return Color.X }\n");
    std::string entry = writeModAura(dir, "main.aura",
        "import \"a.aura\" as a\n"
        "enum type Color = { X }\n"
        "fun main(io: Io) {\n"
        "    let y: a.Color = a.mk()\n"
        "    let b = (Color.X == y)\n"
        "}\n");
    Aura::DiagnosticEngine diag;
    bool ok = analyzeFile(entry, diag);
    EXPECT_FALSE(ok);
    EXPECT_TRUE(hasErrorContaining(diag, "cannot compare enum value of type"));
    std::filesystem::remove_all(dir);
}

// ============================================================
// 批 6（B2）：跨模块（导入）enum 的【方法】通路（change.md §4.9.4-L18）
//   方法表键：`importExports` 建键 = `alias + "::" + canonicalName`（`a::Color`），
//   而 F-1 让 enum 的 `canonicalName` 不带模块前缀 ⇒ 调用点必须用
//   `declModule + "::" + canonicalName` 才能命中（与 record 侧靠 canonicalName 自带前缀等价）。
// ============================================================
TEST(SemaEnum, EnumCrossModuleMethodCallResolved) {
    auto dir = enumTempDir();
    writeModAura(dir, "a.aura",
        "pub enum type Color = { X, Y }\n"
        "pub fun (self Color) hex() -> int { return 1 }\n"
        "pub fun mk() -> Color { return Color.X }\n");
    std::string entry = writeModAura(dir, "main.aura",
        "import \"a.aura\" as a\n"
        "fun main(io: Io) {\n"
        "    let y: a.Color = a.mk()\n"
        "    let h = y.hex()\n"
        "}\n");
    Aura::DiagnosticEngine diag;
    bool ok = analyzeFile(entry, diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_TRUE(ok);
    std::filesystem::remove_all(dir);
}

TEST(SemaEnum, EnumCrossModuleUnknownMethodE013) {
    auto dir = enumTempDir();
    writeModAura(dir, "a.aura",
        "pub enum type Color = { X, Y }\n"
        "pub fun (self Color) hex() -> int { return 1 }\n"
        "pub fun mk() -> Color { return Color.X }\n");
    std::string entry = writeModAura(dir, "main.aura",
        "import \"a.aura\" as a\n"
        "fun main(io: Io) {\n"
        "    let y: a.Color = a.mk()\n"
        "    let h = y.nope()\n"
        "}\n");
    Aura::DiagnosticEngine diag;
    bool ok = analyzeFile(entry, diag);
    EXPECT_FALSE(ok);
    EXPECT_TRUE(hasErrorCode(diag, Aura::DiagCode::E013_MethodNotFound));
    EXPECT_TRUE(hasErrorContaining(diag, "has no method 'nope'"));
    // 提示里必须列出**导入** enum 的可用方法（证明方法表已解析到该导入 enum）
    EXPECT_TRUE(hasErrorHintContaining(diag, "valid methods: hex"));
    std::filesystem::remove_all(dir);
}

// ============================================================
// Step 9：match 穷尽性 + 裸名/限定名定向 + 无法定向报错
// ============================================================
TEST(SemaEnum, EnumMatchBareNamesExhaustive) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "enum type Color = { Red, Green }"
        " fun main(io: Io) { let c = Color.Red;"
        " match c { Red => { } Green => { } } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaEnum, EnumMatchQualifiedExhaustive) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "enum type Color = { Red, Green }"
        " fun main(io: Io) { let c = Color.Red;"
        " match c { Color.Red => { } Color.Green => { } } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaEnum, EnumMatchMissingVariant) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "enum type Color = { Red, Green, Blue }"
        " fun main(io: Io) { let c = Color.Red; match c { Red => { } } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorCode(diag, Aura::DiagCode::E014_MatchNotExhaustive));
}

TEST(SemaEnum, EnumMatchWildcardCovers) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "enum type Color = { Red, Green }"
        " fun main(io: Io) { let c = Color.Red; match c { Red => { } _ => { } } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaEnum, EnumMatchGroupExhaustive) {
    // ⚠️ 与 change.md §4.7.3 的裸名分组（`Red | Green`）不同：Parser 只允许【常量模式】
    //    分组，裸名是 TypePattern ⇒ 报 `type pattern cannot be grouped`（见下方
    //    EnumMatchBareNameGroupRejectedByParser）。故本用例改用【限定名】分组
    //    （实测可解析且穷尽），见报告 §报红 B2-5。
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "enum type Color = { Red, Green, Blue }"
        " fun main(io: Io) { let c = Color.Red;"
        " match c { Color.Red | Color.Green => { } Blue => { } } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaEnum, EnumMatchBareNameGroupRejectedByParser) {
    // 现状记录（非本批范围）：裸名分组在 Parser 阶段即被拒（枚举值分组 v1 不支持）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "enum type Color = { Red, Green, Blue }"
        " fun main(io: Io) { let c = Color.Red;"
        " match c { Red | Green => { } Blue => { } } }",
        diag);
    EXPECT_TRUE(hasErrorContaining(diag, "type pattern cannot be grouped"));
}

TEST(SemaEnum, EnumMatchDuplicateVariant) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "enum type Color = { Red }"
        " fun main(io: Io) { let c = Color.Red;"
        " match c { Red => { } Red => { } _ => { } } }",
        diag);
    EXPECT_TRUE(hasErrorContaining(diag, "duplicate match variant"));
}

TEST(SemaEnum, EnumMatchUnresolvablePattern) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "enum type Color = { Red }"
        " fun main(io: Io) { let c = Color.Red;"
        " match c { Bogus => { } _ => { } } }",
        diag);
    EXPECT_TRUE(hasErrorContaining(diag, "cannot be resolved"));
}

TEST(SemaEnum, EnumMatchEnumTypePatternCoversAll) {
    // `Color v`：整型兜底（吞掉全部变体）⇒ 穷尽
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "enum type Color = { Red, Green }"
        " fun main(io: Io) { let c = Color.Red; match c { Color v => { } } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaEnum, EnumMatchNonEnumUnionUnchanged) {
    // 回归守护：既有 union 的 match 穷尽语义不受 enum 分支影响
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun f(x: int | string) { match x { int => { } string => { } } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

// ============================================================
// 🐛 bug-107（2026-10-16 修复）：裸名与枚举类型名【同名】时的定向解析
//   `enum type Foo = { Foo, Bar }` + 裸名臂 `Foo`（裁定 ⑦ 允许省略限定名）
//   修复前：被当【类型模式】⇒ 覆盖判定 `<all>`（恒真）+ 穷尽性判全覆盖
//   ⇒ 产物 `if (true)`、`Bar` 臂死代码、aurac 零诊断（静默错码）。
//   修法：纯裸名**先查变体表**（resolveBareEnumPattern，src/Sema/SemType.h）。
// ============================================================
TEST(SemaEnum, EnumSameNameVariantBareMatchExhaustive) {
    // 正例：同名变体 + 裸名两臂齐全 ⇒ 无诊断（且 `Foo` 臂按变体覆盖，不再吞掉 `Bar`）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "enum type Foo = { Foo, Bar }"
        " fun main(io: Io) { let x = Foo.Foo;"
        " match x { Foo => { } Bar => { } } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaEnum, EnumSameNameVariantBareMatchMissingVariant) {
    // 负例（修复的核心判据）：同名变体 + 裸名 + 缺 `Bar` 臂 ⇒ 必须报穷尽性（修复前静默）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "enum type Foo = { Foo, Bar }"
        " fun main(io: Io) { let x = Foo.Foo;"
        " match x { Foo => { } } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
    EXPECT_TRUE(hasErrorCode(diag, Aura::DiagCode::E014_MatchNotExhaustive));
    EXPECT_TRUE(hasErrorContaining(diag, "not all variants are covered"));
}

TEST(SemaEnum, EnumSameNameVariantBareDuplicateVariant) {
    // 同源证据：裸名 `Foo` 若仍被判为类型模式（`<all>`），重复检测会**跳过**它 ⇒ 不报重复；
    // 修复后 `Foo` 解为变体常量模式 ⇒ 参与重复检测 ⇒ 干净报 duplicate match variant。
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "enum type Foo = { Foo, Bar }"
        " fun main(io: Io) { let x = Foo.Foo;"
        " match x { Foo => { } Foo => { } _ => { } } }",
        diag);
    EXPECT_TRUE(hasErrorContaining(diag, "duplicate match variant"));
}

TEST(SemaEnum, EnumSameNameVariantQualifiedUnchanged) {
    // 不误伤：限定名写法与修复前一致（穷尽且无诊断）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "enum type Foo = { Foo, Bar }"
        " fun main(io: Io) { let x = Foo.Foo;"
        " match x { Foo.Foo => { } Foo.Bar => { } } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaEnum, EnumSameNameVariantTypePatternStillAll) {
    // 不误伤：`Foo v =>`（varName 非空）仍按类型模式 ⇒ 整型兜底吞掉全部变体（穷尽）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "enum type Foo = { Foo, Bar }"
        " fun main(io: Io) { let x = Foo.Foo;"
        " match x { Foo v => { } } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaEnum, EnumSameNameVariantBareEnumNameStillAll) {
    // 不误伤：纯裸【枚举名】（无绑定名，非变体名冲突之外的既有形态）仍按类型模式 ⇒ 穷尽
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "enum type Color = { Red, Green }"
        " fun main(io: Io) { let c = Color.Red; match c { Color => { } } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaEnum, EnumPlainBareMissingVariantUnchanged) {
    // 不误伤：无同名变体的枚举 + 缺臂 ⇒ 与修复前一致地报穷尽性
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "enum type Color = { Red, Green, Blue }"
        " fun main(io: Io) { let c = Color.Red; match c { Red => { } } }",
        diag);
    EXPECT_TRUE(hasErrorCode(diag, Aura::DiagCode::E014_MatchNotExhaustive));
}
