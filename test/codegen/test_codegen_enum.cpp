// ============================================================
// test/codegen/test_codegen_enum.cpp — CodeGen 输出单元测试：enum（feature-20 批 3/4）
//
// 覆盖面（change.md §4.7.4 + §4.4-⑨⑩⑪）：
//   · 形态 1「enum class 外套壳」五件套（`_Tag` / `_tag` / 静态变体单例 / operator==/!= / 壳内方法声明）
//   · 变体访问 = 静态单例（Sema 置位的 AST 标记 `enumVariantAccess`）
//   · match 生成：裸名变体 ⇒ tag 比较、限定名 ⇒ tag 比较、枚举类型模式 ⇒ 恒真兜底
//   · 值接收者四项硬约束 (a)(b)(c)(d)：入口按值拷贝 / ` const` / `access = '.'`
//   · 形态 2 批 A：形式 A 关联值（直调 intern_string，**不留静态裸缓存**）
//   · 壳保留名冲突 ⇒ 生成期 error（R-2 的 6 类清单，两负例）
//
// ⚠️ 与 change.md 的逐条偏差（实施批 3 报红/偏差，详见 scripts/enum_impl_b3_report.md）：
//   · `CodeGenEnumMethodGeneric` 的文档源 `fun (self Color) pick<T>(x: T) -> T` **不可解析**
//     （`MethodDecl` 无方法级 `typeParams`；Parser 在 `pick<` 处报 `expected '(' after method name`）
//     ⇒ 改用仓内既有等价泛型方法形态 `apply(f: fun(U) -> U)`（bug-07 先例，同为模板方法 +
//     类内 `template<...>` 前缀 + 定义落 header），验收目标（模板前缀/落点/` const` 只能被 g++ 抓）不变。
//   · match 用例源在 match 之后补 `return 0`（Sema 要求「所有路径显式返回」）。
// ============================================================
#include "framework/test_framework.h"
#include "framework/test_helpers.h"

#include "CodeGen/MetaCollect.h"   // 批 8：MetaCollector / MetadataSink（本文件自带 thunk 用例注入）

#include <cstddef>
#include <memory>
#include <string>
#include <utility>

using namespace aura_test;

namespace {

// 单文件 Inline 路径（注入 collector）—— 复刻 test_codegen_frames_p4b1.cpp:57-82
struct InlineOut {
    Aura::CompileUnit unit;
    bool               ok = false;
};

InlineOut genMetaEnum(const std::string& src, Aura::DiagnosticEngine& diag,
                      const std::string& sourcePath = std::string(),
                      const std::string& moduleName = "main") {
    InlineOut out;
    diag.setSourceView(src);
    diag.reset();
    Aura::ModuleManager mgr(diag);
    mgr.loadBuiltinAurai();
    Aura::Lexer lexer(src);
    auto tokens = lexer.scanAll();
    Aura::Parser parser(std::move(tokens), diag);
    auto program = parser.parse();
    if (!program) return out;
    loadImportedBuiltins(mgr, *program);
    Aura::SemAnalyzer sema(diag);        // 必须活到 generate 返回
    (void)sema.analyze(*program);
    if (diag.hasErrors()) return out;
    Aura::CodeGenConfig cfg;             // 默认 Inline
    Aura::MetaCollector meta(sourcePath, Aura::ModuleManager::sanitizeId(moduleName));
    Aura::CodeGenerator cg(diag);
    out.unit = cg.generate(*program, moduleName, {}, std::string(), cfg, {}, {},
                           sourcePath, Aura::MetadataSink::collect(meta));
    out.ok = true;
    return out;
}

// 取 `kTypeTable` 中 name 那一行的 kind 片段：`"<name>", static_cast<uint32_t>(<kind 文本>)`
std::string typeRowOf(const std::string& meta, const std::string& name) {
    const std::string key = "\"" + name + "\", static_cast<uint32_t>(";
    const size_t p = meta.find(key);
    if (p == std::string::npos) return std::string();
    const size_t e = meta.find(')', p);
    if (e == std::string::npos) return std::string();
    return meta.substr(p, e - p + 1);
}

// 按**函数签名**切片，返回 `{ ... }` 的完整函数体文本（形态照 typeRowOf：找不到返回空串）
//   E7：反向断言（「不含 GcRootHandle」）必须收窄到目标函数体——整个 impl 里 io / 闭包 /
//   驱动代码可能**合法**出现 GcRootHandle，全 impl 断言会假红。
//   限制：花括号按字符计数，含 `{` / `}` 的字符串字面量会错位 ⇒ 只用于不含花括号字面量的
//   访问器体（本用例 `Color::Red` 满足）。返回空串 ⇒ 调用方必须 ASSERT，防「切不到 ⇒ 断言空转」。
std::string funcBodyOf(const std::string& impl, const std::string& sig) {
    const size_t p = impl.find(sig);
    if (p == std::string::npos) return std::string();
    const size_t b = impl.find('{', p + sig.size());
    if (b == std::string::npos) return std::string();
    int depth = 0;
    for (size_t i = b; i < impl.size(); ++i) {
        if (impl[i] == '{') ++depth;
        else if (impl[i] == '}') {
            if (--depth == 0) return impl.substr(b, i - b + 1);
        }
    }
    return std::string();
}

} // namespace

// ============================================================
// 形态 1：壳五件套
// ============================================================
TEST(CodeGenEnum, Plain) {
    // 形态 1 壳：struct + 内嵌 `_Tag` + `_tag` + 静态变体单例 + operator==/!= + 单例 out-of-class 定义
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "enum type Color = { Red, Green, Blue }"
        " fun main(io: Io) { let c = Color.Red }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "struct Color");
    EXPECT_CONTAINS(unit.header, "enum class _Tag : uint8_t { Red = 0, Green = 1, Blue = 2 };");
    EXPECT_CONTAINS(unit.header, "_Tag _tag = _Tag::Red;");
    EXPECT_CONTAINS(unit.header, "static const Color Red;");
    EXPECT_CONTAINS(unit.header, "static const Color Green;");
    EXPECT_CONTAINS(unit.header, "bool operator==");
    EXPECT_CONTAINS(unit.impl, "const Color Color::Red{Color::_Tag::Red};");
    // 变体访问 = 静态单例（壳的静态常量，不是临时构造）
    EXPECT_CONTAINS(unit.impl, "Color::Red");
    // isHeap 口径守护：enum 是值类型 ⇒ 全档不出现 `Color*`
    EXPECT_NOT_CONTAINS(unit.header, "Color*");
    EXPECT_NOT_CONTAINS(unit.impl, "Color*");
    // 值类型非 GC 对象 ⇒ 不生成 `_desc`
    EXPECT_NOT_CONTAINS(unit.header, "Color::_desc");
    EXPECT_NOT_CONTAINS(unit.impl, "Color::_desc");
    // §4.4-⑨ 改动 5-③：产物不含 `/* unknown_semtype */` / `/* unknown_enum */` 占位
    EXPECT_NOT_CONTAINS(unit.header, "/* unknown_");
    EXPECT_NOT_CONTAINS(unit.impl, "/* unknown_");
}

TEST(CodeGenEnum, ExplicitHexDecimal) {
    // 显式 0x 值：Sema 定死 intValue，CodeGen 出十进制（可读性丢失，登记已知限制）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "enum type WebColor = { Red = 0xFF0000, Green = 0x00FF00 }"
        " fun main(io: Io) { let c = WebColor.Red }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "Red = 16711680,");
}

TEST(CodeGenEnum, EmptyNotConstructible) {
    // 空 enum：`_Tag` 空、**零个单例**、`_tag` 无默认初值 ⇒ 壳不可构造值
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "enum type Never = { }"
        " fun main(io: Io) { let x = 1 }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "struct Never");
    EXPECT_CONTAINS(unit.header, "enum class _Tag : uint8_t { };");
    EXPECT_NOT_CONTAINS(unit.header, "static const Never ");
    EXPECT_NOT_CONTAINS(unit.header, "= _Tag::");
}

TEST(CodeGenEnum, TypedVarNoPointer) {
    // 类型标注 `let c: Color = Color.Red`（isHeap=false 口径守护，N2）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "enum type Color = { Red, Green, Blue }"
        " fun main(io: Io) { let c: Color = Color.Red }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "struct Color");
    EXPECT_NOT_CONTAINS(unit.impl, "Color*");
    EXPECT_NOT_CONTAINS(unit.header, "Color*");
}

TEST(CodeGenEnum, NoDesc) {
    // 值类型非 GC 对象 ⇒ 无 `_desc`（§4.6.1「无 _desc 的 GC 分配」）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "enum type Color = { Red, Green }"
        " fun main(io: Io) { let c = Color.Green }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_NOT_CONTAINS(unit.impl, "Color::_desc");
    EXPECT_NOT_CONTAINS(unit.header, "TypeDescriptor Color::_desc");
}

// ============================================================
// 形态 2 批 A：tag struct + 形式 A 关联值
// ============================================================
TEST(CodeGenEnum, BoundTag) {
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "enum type ErrorCode<string> = { not_found = \"NOT_FOUND\" }"
        " fun main(io: Io) { let e = ErrorCode.not_found }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "struct ErrorCode");
    EXPECT_CONTAINS(unit.header, "enum class _Tag : uint8_t { not_found = 0 };");
    EXPECT_CONTAINS(unit.header, "_Tag _tag = _Tag::not_found;");
    EXPECT_CONTAINS(unit.header, "static const ErrorCode not_found;");
    EXPECT_CONTAINS(unit.header, "bool operator==");
    EXPECT_CONTAINS(unit.impl, "const ErrorCode ErrorCode::not_found{ErrorCode::_Tag::not_found};");
    // 形式 A：直调 intern_string（**不得**留静态裸缓存 —— B-1/C-1/G-1 回归守护）
    EXPECT_CONTAINS(unit.impl, "aura_rt::intern_string(\"NOT_FOUND\")");
    // ⚠️ 逐字断言只对 `impl`（文档的 `source`）成立：`header` 里的 `_val_*` **声明**本身
    //    就以 `static const aura_rt::GcString* ` 开头（那是形式 A 的访问器签名，不是缓存），
    //    故此处用「缓存变量形态」（`_v;` / `_v = `）分辨 ⇒ 见报告 §偏差 D-B3-1。
    EXPECT_NOT_CONTAINS(unit.impl, "static const aura_rt::GcString* _v");
    EXPECT_NOT_CONTAINS(unit.header, "GcString* _v;");
    EXPECT_NOT_CONTAINS(unit.header, "GcString* _v =");
    // 形式 A 的访问器本体 = 单行 return（无 `static` 局部、无 guard）
    EXPECT_CONTAINS(unit.impl, "return aura_rt::intern_string(\"NOT_FOUND\");");
    EXPECT_NOT_CONTAINS(unit.impl, "static aura_rt::GcString*");
}

TEST(CodeGenEnum, BoundTagNameSafeName) {
    // H-1：variant 名撞 C++ 保留字 ⇒ 逐个过 safeName（否则 `{ class = 0 }` 坏 C++）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "enum type ErrorCode<string> = { class = \"X\" }"
        " fun main(io: Io) { let e = ErrorCode.class }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "class_");
    EXPECT_NOT_CONTAINS(unit.header, "{ class = ");
}

// ============================================================
// match 生成（§4.4-⑪ 改动 2/3/4）
// ============================================================
TEST(CodeGenEnum, MatchBare) {
    // 裸名变体 ⇒ tag 比较（定死唯一写法）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "enum type Color = { Red, Green, Blue }"
        " fun (self Color) f() -> int {"
        "   match self { Red => 1, _ => 0 }"
        "   return 0 }"
        " fun main(io: Io) { let c = Color.Red let n = c.f() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "_match_val._tag == Color::_Tag::Red");
}

TEST(CodeGenEnum, MatchQualified) {
    // 限定名变体（常量模式路径）⇒ 同款 tag 比较
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "enum type Color = { Red, Green, Blue }"
        " fun (self Color) f() -> int {"
        "   match self { Color.Red => 1, _ => 0 }"
        "   return 0 }"
        " fun main(io: Io) { let c = Color.Red let n = c.f() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "_match_val._tag == Color::_Tag::Red");
}

TEST(CodeGenEnum, MatchEnumTypePattern) {
    // 枚举类型模式（`Color v`）⇒ 恒真兜底 + 绑定（与普通类型分支同款）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "enum type Color = { Red, Green, Blue }"
        " fun (self Color) f() -> int {"
        "   match self { Color v => 1 }"
        "   return 0 }"
        " fun main(io: Io) { let c = Color.Red let n = c.f() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "if (true)");
    EXPECT_CONTAINS(unit.impl, "auto&& v = _match_val;");
}

TEST(CodeGenEnum, MatchEnumNameSafeName) {
    // R-4 正例：枚举名是 C++ 保留字（`enum type class`）⇒ 判据必须「原始名 vs 原始名」，
    // safeName 只落在 C++ 产出串上：类型模式走恒真兜底、裸名变体走 `class_::_Tag::Red`
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "enum type class = { Red, Green }"
        " fun (self class) f() -> int {"
        "   match self { Red => 1, class v => 2 }"
        "   return 0 }"
        " fun main(io: Io) { let c = class.Red let n = c.f() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "struct class_");
    EXPECT_CONTAINS(unit.impl, "_match_val._tag == class_::_Tag::Red");
    EXPECT_CONTAINS(unit.impl, "if (true)");
    EXPECT_CONTAINS(unit.impl, "auto&& v = _match_val;");
    // 旧判据（原始名 vs safeName）会把 `class v` 误判成裸名变体 ⇒ 坏产物 `class_::_Tag::class`
    EXPECT_NOT_CONTAINS(unit.impl, "class_::_Tag::class");
    // 未过 safeName 的裸保留字（壳名 / 类外定义）⇒ 坏 C++
    EXPECT_NOT_CONTAINS(unit.impl, "class::");
}

// ============================================================
// 方法通路：值接收者四项硬约束 (a)(b)(c)(d)
// ============================================================
TEST(CodeGenEnum, Method) {
    // (d)：壳内 ` const` 声明 + 类外 ` const` 定义；不含自由函数形态 `Color_hex(`
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "enum type Color = { Red, Green }"
        " fun (self Color) hex() -> int { return 1 }"
        " fun main(io: Io) { let c = Color.Red let h = c.hex() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "int32_t hex() const;");
    EXPECT_CONTAINS(unit.impl, "int32_t Color::hex() const {");
    EXPECT_NOT_CONTAINS(unit.impl, "Color_hex(");
    EXPECT_NOT_CONTAINS(unit.impl, "int32_t Color::hex() {");
    // (a)：值接收者跳过 `_this` GC 句柄（错误根守护）
    EXPECT_NOT_CONTAINS(unit.impl, "GcRootHandle<Color*>");
    // 调用点不做任何改写（A-1 撤销）
    EXPECT_CONTAINS(unit.impl, "c.hex()");
}

TEST(CodeGenEnum, MethodSelfRef) {
    // (b)：方法体内 `self` = 入口按值拷贝（`const Color self = *this;`），体内用副本比较
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "enum type Color = { Red, Green }"
        " fun (self Color) is_red() -> bool { return self == Color.Red }"
        " fun main(io: Io) { let c = Color.Red let b = c.is_red() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "bool Color::is_red() const");
    EXPECT_CONTAINS(unit.impl, "const Color self = *this;");
    EXPECT_CONTAINS(unit.impl, "self == Color::Red");
    // 裸 `this` 用法不得出现（`*this` 形态合法）
    EXPECT_NOT_CONTAINS(unit.impl, "return this");
    EXPECT_NOT_CONTAINS(unit.impl, "this ==");
    EXPECT_NOT_CONTAINS(unit.impl, "this.");
    EXPECT_NOT_CONTAINS(unit.impl, "this->");
}

TEST(CodeGenEnum, MethodCoroSelfCopy) {
    // 协程 enum 方法：入口拷贝落**协程帧**内（跨挂起安全）；体内不得出现裸 `this` 用法
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "enum type Color = { Red, Green }"
        " fun (self Color) slow_is_red(io: Io) -> bool {"
        "   sync { io.println(\"x\") }"
        "   return self == Color.Red }"
        " fun main(io: Io) { let c = Color.Red sync { let b = c.slow_is_red(io) } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "aura_rt::task<bool> Color::slow_is_red(aura_rt::Io io) const");
    EXPECT_CONTAINS(unit.impl, "const Color self = *this;");
    EXPECT_CONTAINS(unit.impl, "self == Color::Red");
    EXPECT_NOT_CONTAINS(unit.impl, "return this");
    EXPECT_NOT_CONTAINS(unit.impl, "this ==");
    EXPECT_NOT_CONTAINS(unit.impl, "this.");
    EXPECT_NOT_CONTAINS(unit.impl, "this->");
}

TEST(CodeGenEnum, MethodGeneric) {
    // 泛型方法（模板前缀 + 定义落 header）——文档源 `pick<T>(...)` 不可解析（报红 B3-3），
    // 改用仓内既有等价形态 `apply(f: fun(U) -> U)`（bug-07 先例）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "enum type Color = { Red, Green }"
        " fun (self Color) apply(f: fun(U) -> U) -> int { return f(42) }"
        " fun main(io: Io) throws { let c = Color.Red"
        "   io.println(str(c.apply(fun(x: int) -> int { return x + 1 }))) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "template<typename U>");
    EXPECT_CONTAINS(unit.header, "int32_t apply(aura_rt::CallableObj<U, U>* f) const;");
    // 模板方法定义落 header（既有放置取舍）
    EXPECT_CONTAINS(unit.header, "int32_t Color::apply(aura_rt::CallableObj<U, U>* f_raw) const");
    // `_fun` 改名不适用于壳
    EXPECT_NOT_CONTAINS(unit.impl, "apply_fun");
}

TEST(CodeGenEnum, ShellSingletonMethodCall) {
    // (c) 形态 1：变体单例上调方法 ⇒ `.`（不含 `->`）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "enum type Color = { Red }"
        " fun (self Color) hex() -> int { return 1 }"
        " fun main(io: Io) { let h = Color.Red.hex() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "Color::Red.hex()");
    EXPECT_NOT_CONTAINS(unit.impl, "->hex()");
    EXPECT_NOT_CONTAINS(unit.impl, "Color::Red->hex()");
}

TEST(CodeGenEnum, BoundSingletonMethodCall) {
    // (c) 形态 2：关联值单例上调方法 ⇒ `.`（不含 `->`）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "enum type ErrorCode<string> = { not_found = \"NOT_FOUND\" }"
        " fun (self ErrorCode) describe() -> int { return 2 }"
        " fun main(io: Io) { let d = ErrorCode.not_found.describe() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "ErrorCode::not_found.describe()");
    EXPECT_NOT_CONTAINS(unit.impl, "->describe()");
}

TEST(CodeGenEnum, EqOperator) {
    // `==` 由生成的 operator==/!= 承担（enum class 的「免费 ==」随壳构造消失）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "enum type Color = { Red, Green }"
        " fun main(io: Io) { let b = (Color.Red == Color.Green) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "bool operator==(const Color& o) const { return _tag == o._tag; }");
    EXPECT_CONTAINS(unit.header, "bool operator!=(const Color& o) const { return _tag != o._tag; }");
    EXPECT_CONTAINS(unit.impl, "Color::Red == Color::Green");
}

// ============================================================
// §4.4-⑪ 改动 5-④：壳保留成员名冲突 ⇒ 生成期 error（R-2 的 6 类清单）
// ============================================================
TEST(CodeGenEnum, ShellReservedMethodNameRejected) {
    // 负例 ①：方法名 = `_tag`（清单第 ① 类）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "enum type Color = { Red, Green }"
        " fun (self Color) _tag() -> int { return 0 }"
        " fun main(io: Io) { let c = Color.Red }",
        diag);
    EXPECT_TRUE(hasErrorContaining(diag, "collides with the generated enum shell member"));
    (void)unit;
}

TEST(CodeGenEnum, ShellReservedMethodNameRejectedVariant) {
    // 负例 ②（R-2 新增）：方法名 = 变体单例名（清单第 ⑤ 类）——**不得**退化成 `_fun` 改名
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "enum type Color = { Red }"
        " fun (self Color) Red() -> int { return 0 }"
        " fun main(io: Io) { let c = Color.Red }",
        diag);
    EXPECT_TRUE(hasErrorContaining(diag, "collides with the generated enum shell member"));
    EXPECT_NOT_CONTAINS(unit.impl, "Red_fun");
}

// ============================================================
// 批 5（T1/T2）：同名 variant —— `enum type Foo = { Foo }`（§4.6.1 裁定 A 合法）
// C++ 禁止静态数据成员与所在类同名（成员名与注入类名冲突）：
//   error: 'Foo::Foo' has the same name as the class in which it is declared
// ⇒ 只改 C++ 生成侧：单例成员名撞壳名时固定追加后缀 `_v`（唯一真源
//   `CodeGenerator::enumVariantCppName`）。Aura 侧写法 `Foo.Foo` 逐字不变。
// 三处必须同源（漏一处 = 坏 C++）：① 壳内声明 ② 类外定义 ③ 变体访问产点。
// ============================================================
TEST(CodeGenEnum, SameNameVariant) {
    // 最小复现（scripts/_v1_probe/p09_same_name_variant.aura）的产物断言：
    // 此前产物 = `static const Foo Foo;` / `const Foo Foo::Foo{Foo::_Tag::Foo};` ⇒ g++ rc=1。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "enum type Foo = { Foo }"
        " fun main(io: Io) { let x = Foo.Foo  let b = (x == Foo.Foo) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // ① 壳内声明：改名后（且**无**同名裸声明）
    EXPECT_CONTAINS(unit.header, "static const Foo Foo_v;");
    EXPECT_NOT_CONTAINS(unit.header, "static const Foo Foo;");
    // ② 类外定义：成员名改名后；初始化器仍用 `_Tag` 的**原**枚举项名
    EXPECT_CONTAINS(unit.impl, "const Foo Foo::Foo_v{Foo::_Tag::Foo};");
    EXPECT_NOT_CONTAINS(unit.impl, "const Foo Foo::Foo{");
    // ③ 变体访问产点：Aura 侧 `Foo.Foo` ⇒ C++ `Foo::Foo_v`
    EXPECT_CONTAINS(unit.impl, "auto x = Foo::Foo_v;");
    EXPECT_CONTAINS(unit.impl, "x == Foo::Foo_v");
    // `_Tag` 枚举项名**不改**（tag 比较真源，与 StmtMatch 同源）：`Foo::_Tag::Foo` 仍合法
    EXPECT_CONTAINS(unit.header, "enum class _Tag : uint8_t { Foo = 0 };");
    // 值类型 ⇒ 无 `Foo*` / 无 `_desc`
    EXPECT_NOT_CONTAINS(unit.header, "Foo*");
    EXPECT_NOT_CONTAINS(unit.impl, "Foo*");
    EXPECT_NOT_CONTAINS(unit.header, "Foo::_desc");
}

TEST(CodeGenEnum, SameNameVariantMethod) {
    // 同名 variant + 壳方法：单例上调方法（③ 的产物）与 `==`（operator== 走 `_tag`，无需改名）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "enum type Foo = { Foo }"
        " fun (self Foo) hex() -> int { return 1 }"
        " fun (self Foo) is_foo() -> bool { return self == Foo.Foo }"
        " fun main(io: Io) { let h = Foo.Foo.hex() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "Foo::Foo_v.hex()");
    EXPECT_NOT_CONTAINS(unit.impl, "Foo::Foo_v->hex()");
    EXPECT_CONTAINS(unit.impl, "self == Foo::Foo_v");
    EXPECT_CONTAINS(unit.header, "bool operator==(const Foo& o) const { return _tag == o._tag; }");
}

TEST(CodeGenEnum, SameNameVariantMatch) {
    // 同名 variant + match：tag 比较用 `_Tag` 枚举项名（**不改名**）⇒ 与单例改名正交。
    // ⚠️ 必须用**限定名** `Foo.Foo`（裸名 `Foo` 会被 Parser/Sema 判为类型模式「整型兜底」
    //    ⇒ 产物 `if (true)`；见 scripts/enum_impl_b5_report.md「偏差/报红 B5-1」——
    //    Parser/Sema 侧口径，超出本批允许改动面，仅登记不改）。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "enum type Foo = { Foo, Bar }"
        " fun (self Foo) pick() -> int {"
        "   match self { Foo.Foo => 1, Foo.Bar => 2, _ => 0 }"
        "   return 0"
        " }"
        " fun main(io: Io) { let n = Foo.Foo.pick() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "_match_val._tag == Foo::_Tag::Foo");
    EXPECT_CONTAINS(unit.impl, "_match_val._tag == Foo::_Tag::Bar");
    EXPECT_CONTAINS(unit.impl, "Foo::Foo_v.pick()");
    EXPECT_NOT_CONTAINS(unit.impl, "Foo::_Tag::Foo_v");
}

TEST(CodeGenEnum, SameNameVariantBareMatch) {
    // 🐛 bug-107（2026-10-16 修复）：裸名臂**先查变体表** ⇒ 同名变体必须走 tag 比较。
    // 修复前：裸名 `Foo` 被 Sema 判为类型模式（`tp->typeName == enumRawName`）⇒ 产物 `if (true)`，
    //   后续 `Bar` 臂死代码，且 aurac 零诊断（静默错码，见 issues/bugs/bug-107-*.md）。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "enum type Foo = { Foo, Bar }"
        " fun (self Foo) pick2() -> int {"
        "   match self { Foo => 1, Bar => 2, _ => 0 }"
        "   return 0"
        " }"
        " fun main(io: Io) { let n = Foo.Bar.pick2() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    // 同名变体裸名 ⇒ tag 比较（与限定名写法逐字同款）
    EXPECT_CONTAINS(unit.impl, "_match_val._tag == Foo::_Tag::Foo");
    EXPECT_CONTAINS(unit.impl, "_match_val._tag == Foo::_Tag::Bar");
    // 🐛 回归判据：此 match **不得**出现恒真分支（曾为 `if (true)`）
    EXPECT_NOT_CONTAINS(unit.impl, "if (true)");
    // tag 枚举项名不改名（`Foo` 而非 `Foo_v`；与单例改名正交）
    EXPECT_NOT_CONTAINS(unit.impl, "Foo::_Tag::Foo_v");
}

TEST(CodeGenEnum, SameNameVariantBareMatchExhaustiveNoWildcard) {
    // bug-107 配套：无 `_` 的穷尽裸名 match（同名变体）⇒ 仍无诊断 + 两臂 tag 比较
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "enum type Foo = { Foo, Bar }"
        " fun (self Foo) pick3() -> int {"
        "   match self { Foo => 1, Bar => 2 }"
        "   return 0"
        " }"
        " fun main(io: Io) { let n = Foo.Foo.pick3() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "_match_val._tag == Foo::_Tag::Foo");
    EXPECT_CONTAINS(unit.impl, "_match_val._tag == Foo::_Tag::Bar");
    EXPECT_NOT_CONTAINS(unit.impl, "if (true)");
}

TEST(CodeGenEnum, SameNameVariantTypePatternStillAll) {
    // bug-107 不误伤守护：`Foo v =>`（varName 非空）**仍**按类型模式 ⇒ 恒真兜底 + 绑定
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "enum type Foo = { Foo, Bar }"
        " fun (self Foo) pick4() -> int {"
        "   match self { Foo v => 1 }"
        "   return 0"
        " }"
        " fun main(io: Io) { let n = Foo.Foo.pick4() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.impl, "if (true)");
    EXPECT_CONTAINS(unit.impl, "auto&& v = _match_val;");
}

TEST(CodeGenEnum, SameNameVariantRenamedMemberReserved) {
    // ⑤ 清单同源守护：改名后的成员名 `Foo_v` 也在保留名清单里 ⇒ 方法名撞它必须报错
    //（若清单仍用未改名的 `Foo`，此处会静默产 `static const Foo Foo_v;` + 方法 `Foo_v()` 重名）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "enum type Foo = { Foo }"
        " fun (self Foo) Foo_v() -> int { return 0 }"
        " fun main(io: Io) { let x = Foo.Foo }",
        diag);
    EXPECT_TRUE(hasErrorContaining(diag, "collides with the generated enum shell member"));
    (void)unit;
}

TEST(CodeGenEnum, SameNameVariantSuffixClashRejected) {
    // 退化角落（改名映射非单射）：变体名恰为 `<Enum>_v` ⇒ `Foo`→`Foo_v` 与既有 `Foo_v` 撞名。
    // 仅凭 (enumRawName, variantName) 无法区分 ⇒ 按本文件既有口径「不产坏 C++」生成期报错
    //（此前形态 = 静默产两个同名静态成员 ⇒ g++ 坏 C++）。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "enum type Foo = { Foo, Foo_v }"
        " fun main(io: Io) { let x = Foo.Foo }",
        diag);
    EXPECT_TRUE(hasErrorContaining(diag, "both map to the C++ singleton name"));
    (void)unit;
}

// ============================================================
// feature-20 批 8（bug-108）：enum 进【顶层函数签名】⇒ 物化 thunk 走值语义
// ------------------------------------------------------------
// 缺陷（issues/bugs/bug-108-*.md）：物化 thunk（`_aura_mat_*`）的 erased 边界
//   装/拆箱把 enum 当 **堆对象指针** ——
//     形参 `static_cast<Color>(__v[0].v.p)`（`__v[i].v.p` = 指针成员）
//     返回 `static_cast<aura_rt::GcObject*>(__t->invoke(__t))`
//   ⇒ 生成的 C++ 编不过，而 `aurac` 零诊断（静默坏产物）。
//
// 修复：erased 边界的「值 / 指针」判据一律走 `isEnumValueSemType(sem)`（与批 3 落地的
//   enum 值语义链同源），enum 值经 **I64 槽** 承载壳的身份字段 `_tag`
//   （零 runtime 改动：`CallArg` 只有 I64/F64/Ptr/Nonev 四槽）：
//     装箱 `(int64_t)((EXPR)._tag)`；拆箱 `Color{static_cast<Color::_Tag>(slot.v.i)}`
//   （聚合初始化写法与 DeclEnum.cpp 的单例定义 `const Color Color::Red{…};` 同款）。
//
// ⚠️ 本节自带 `enumThunkGen`：本目录的 `compileSource` **不注入 collector**
//    （见 `test/framework/test_helpers.h` 的显式注释）⇒ **不发射任何 thunk**
//    ⇒ thunk 面断言在 `compileSource` 下**恒不可能成立**。此处复刻
//    `test_codegen_meta.cpp` 的 `genWithMeta` 最小形态（同一个 `cg.generate` 9 参调用）。
// ============================================================
namespace {

struct EnumThunkOut {
    Aura::CompileUnit                   unit;
    std::unique_ptr<Aura::MetaCollector> meta;
    bool                                ok = false;
};

EnumThunkOut enumThunkGen(const std::string& src, Aura::DiagnosticEngine& diag) {
    EnumThunkOut out;
    diag.setSourceView(src);
    diag.reset();
    Aura::ModuleManager mgr(diag);
    mgr.loadBuiltinAurai();
    Aura::Lexer lexer(src);
    auto tokens = lexer.scanAll();
    Aura::Parser parser(std::move(tokens), diag);
    auto program = parser.parse();
    if (!program) return out;
    loadImportedBuiltins(mgr, *program);
    // ⚠️ SemAnalyzer 必须存活到 generate 返回（AST inferredType 指向它的 typeStore_）。
    Aura::SemAnalyzer sema(diag);
    (void)sema.analyze(*program);
    if (diag.hasErrors()) return out;
    Aura::CodeGenConfig cfg;
    out.meta = std::make_unique<Aura::MetaCollector>(
        "", Aura::ModuleManager::sanitizeId("main"));
    Aura::CodeGenerator cg(diag);
    out.unit = cg.generate(*program, "main", {}, "", cfg, {}, {}, "",
                           Aura::MetadataSink::of(out.meta.get()));
    out.ok = true;
    return out;
}

}   // namespace

TEST(CodeGenEnum, ThunkEnumParamValueSemantics) {
    // 🐛 bug-108：enum 作**形参** ⇒ thunk 的形参拆箱必须走**值语义**（I64 槽承载 `_tag`），
    //   不得走指针成员 `__v[0].v.p`。修复前产物（实测 g++ 报
    //   `cannot convert 'aura_rt::GcObject* const' to 'Color::_Tag' in initialization`）：
    //     `return aura_rt::CallArg::of((int64_t)(__t->invoke(__t, static_cast<Color>(__v[0].v.p))));`
    Aura::DiagnosticEngine diag;
    auto r = enumThunkGen(
        "enum type Color = { Red, Green }"
        " fun take(c: Color) -> int { return 1 }"
        " fun main(io: Io) { let n = take(Color.Red) }",
        diag);
    ASSERT_TRUE(r.ok);
    const std::string& impl = r.unit.impl;
    // thunk 确实发射（前置条件——否则下列断言会「因缺内容而假绿」）。
    // 符号序：本源无方法 ⇒ `take` 是 seq 0 ⇒ thunk 名 `_aura_mat_main_0`。
    EXPECT_CONTAINS(impl, "_aura_mat_main_0(const aura_rt::CallArg* recv)");
    // ① 形参 kind 判据 = I64（与装箱表达式同源）
    EXPECT_CONTAINS(impl, "__v[0].kind != aura_rt::CallArg::Kind::I64");
    // ② 形参拆箱 = 值语义聚合构造（`_tag` 回壳）
    EXPECT_CONTAINS(impl, "Color{static_cast<Color::_Tag>(__v[0].v.i)}");
    // ③ 🔴 回归护栏：**不得**出现指针拆箱 / enum 当 GcObject* 的形态
    EXPECT_NOT_CONTAINS(impl, "__v[0].v.p");
    EXPECT_NOT_CONTAINS(impl, "static_cast<Color>(__v[0].v.p)");
    EXPECT_NOT_CONTAINS(impl, "static_cast<Color>(__v");
    // enum 值类型 ⇒ 产物中不出现 `Color*`（与既有 isHeap=false 口径一致）
    EXPECT_NOT_CONTAINS(impl, "Color*");
}

TEST(CodeGenEnum, ThunkEnumReturnValueSemantics) {
    // 🐛 bug-108：enum 作**返回** ⇒ thunk 的返回装箱必须取 `_tag`（I64），
    //   不得 `static_cast<aura_rt::GcObject*>(invoke(...))`。
    //   修复前实测 g++：`invalid 'static_cast' from type 'Color' to type 'aura_rt::GcObject*'`。
    Aura::DiagnosticEngine diag;
    auto r = enumThunkGen(
        "enum type Color = { Red, Green }"
        " fun give() -> Color { return Color.Red }"
        " fun main(io: Io) { let c = give() }",
        diag);
    ASSERT_TRUE(r.ok);
    const std::string& impl = r.unit.impl;
    // 符号序：本源无方法 ⇒ `give` 是 seq 0 ⇒ thunk 名 `_aura_mat_main_0`
    EXPECT_CONTAINS(impl, "_aura_mat_main_0(const aura_rt::CallArg* recv)");
    EXPECT_CONTAINS(impl, "aura_rt::CallArg::of((int64_t)((__t->invoke(__t))._tag))");
    // 🔴 回归护栏：不得把 enum 返回值 static_cast 成 GcObject*
    EXPECT_NOT_CONTAINS(impl, "static_cast<aura_rt::GcObject*>(__t->invoke(__t))");
    EXPECT_NOT_CONTAINS(impl, "Color*");
}

TEST(CodeGenEnum, ErasedCallSiteEnumValueSemantics) {
    // 🐛 bug-108 同源面（**erased 调用点**）：`Callable` 变量持 enum 签名函数时，
    //   实参打包与返回拆箱同样必须走值语义（同一份判据 / 同一组助手）。
    //   修复前会产 `static_cast<aura_rt::GcObject*>(Color.Green)`（编不过）。
    Aura::DiagnosticEngine diag;
    auto r = enumThunkGen(
        "enum type Color = { Red, Green }"
        " fun take(c: Color) -> int { return 1 }"
        " fun give() -> Color { return Color.Red }"
        " fun main(io: Io) {"
        "   let f: Callable = take"
        "   let n = f(Color.Green)"
        "   let g: Callable = give"
        "   let d = g()"
        " }",
        diag);
    ASSERT_TRUE(r.ok);
    const std::string& impl = r.unit.impl;
    // 实参装箱：I64 槽承载 `_tag`
    EXPECT_CONTAINS(impl, "aura_rt::CallArg::of((int64_t)((Color::Green)._tag))");
    // 返回拆箱：CallArg → enum 值
    EXPECT_CONTAINS(impl, "Color{static_cast<Color::_Tag>(__r_");
    // 🔴 回归护栏：enum 值不得被当 GC 指针（装箱 / 根化两条都不许）
    EXPECT_NOT_CONTAINS(impl, "static_cast<aura_rt::GcObject*>(Color::");
    EXPECT_NOT_CONTAINS(impl, "GcRootHandle<Color>");
}

TEST(CodeGenEnum, ThunkNonEnumReturnsKeepBranch) {
    // 🐛 bug-108 **不误伤**守护：新增的 enum 值语义分支是**窄守卫**（只认 enum）——
    //   **返回面**（返回类型是 Sema 会回填 `inferredType` 的那一侧）必须逐字保持修复前形态：
    //     `-> string`（堆）⇒ `CallArg::of(static_cast<aura_rt::GcObject*>(__t->invoke(__t)))`
    //     `-> int`（值）  ⇒ `CallArg::of((int64_t)(__t->invoke(__t)))`
    //   ⚠️ 用**返回**面而非形参面：Sema **不**把形参 TypeExpr 的解析结果写回
    //      `TypeExpr::inferredType`（除 enum 回填外，见 CodeGen.cpp 的 `semTypeOfDeclType`
    //      注释）⇒ 只有 int/float/bool 形参能构造签名，拿不到「堆形参」用于本守护。
    Aura::DiagnosticEngine diag;
    auto r = enumThunkGen(
        "fun mkstr() -> string { return \"hi\" }"
        " fun one() -> int { return 1 }"
        " fun main(io: Io) { let s = mkstr(); let n = one() }",
        diag);
    ASSERT_TRUE(r.ok);
    const std::string& impl = r.unit.impl;
    EXPECT_CONTAINS(impl, "aura_rt::CallArg::of(static_cast<aura_rt::GcObject*>(__t->invoke(__t)))");
    EXPECT_CONTAINS(impl, "aura_rt::CallArg::of((int64_t)(__t->invoke(__t)))");
}

// ============================================================
// enum v1.1 L-2：元数据 kind（Enum 位 / 位表镜像判别力）
// ============================================================
TEST(CodeGenEnum, KindIsEnum) {
    // L-2：enum 行 kind = TypeKindBits::Enum；record 行仍是 Record（同表内对照）
    Aura::DiagnosticEngine diag;
    InlineOut r = genMetaEnum(
        "enum type Color = { Red, Green }"
        " type P = { x: int }"
        " fun main(io: Io) { let c = Color.Red }",
        diag, "D:/you/Aura/probe/enum_v11/kind.aura");
    ASSERT_TRUE(r.ok);
    const std::string rowEnum = typeRowOf(r.unit.metaImpl, "Color");
    const std::string rowRec  = typeRowOf(r.unit.metaImpl, "P");
    ASSERT_TRUE(!rowEnum.empty());
    ASSERT_TRUE(!rowRec.empty());
    EXPECT_TRUE(rowEnum.find("TypeKindBits::Enum") != std::string::npos);
    EXPECT_TRUE(rowRec.find("TypeKindBits::Record") != std::string::npos);
    EXPECT_TRUE(rowRec.find("TypeKindBits::Enum") == std::string::npos);
}

TEST(CodeGenEnum, MetaKindTeeth) {
    // L-2 判别力：MetaEmit 位表漏加 bit9 ⇒ typeKindExpr 走 fallback "0"（MetaEmit.cpp:82）
    //   ⇒ **该 enum 行**渲染 `static_cast<uint32_t>(0)`（静默丢 kind 的唯一可见痕迹）
    //   E4：断言**必须收窄到 `Color` 那一行**——`symbolFlagsExpr` 的 fallback 同为 "0"
    //   （MetaEmit.cpp:102），全产物断言会被 flags=0 的无关行打红
    Aura::DiagnosticEngine diag;
    InlineOut r = genMetaEnum(
        "enum type Color = { Red }"
        " fun main(io: Io) { let c = Color.Red }",
        diag, "D:/you/Aura/probe/enum_v11/teeth.aura");
    ASSERT_TRUE(r.ok);
    const std::string row = typeRowOf(r.unit.metaImpl, "Color");
    ASSERT_TRUE(!row.empty());
    EXPECT_CONTAINS(row, "TypeKindBits::Enum");
    EXPECT_NOT_CONTAINS(row, "static_cast<uint32_t>(0)");
}

// ============================================================
// enum v1.1 L-1（批 B）：关联值三分类（形式 B / 零根直返）与匿名 record 守卫
// ============================================================
TEST(CodeGenEnum, BoundGlobalRoot) {
    // L-1 第 (iii) 档：关联值为 record（含 GC 指针字段 name）⇒ 形式 B：函数局部 static + Global 根
    //   **预期模板②**（E5：源含 1 个 string ⇒ 分配点 ≥2 = gc_alloc<Point> + intern_string("ON")，
    //   按 §4.8 判据落②；Step 10 探针实测 2 已钉死）
    //   ⚠️ 下面的断言对 ①/② 两版都成立 ⇒ 模板切换不需改用例
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int, name: string }"
        " enum type Wrap<Point> = { active = Point { x = 1, name = \"ON\" } }"
        " fun main(io: Io) { let w = Wrap.active }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "static Point* _val_active();");
    EXPECT_CONTAINS(unit.impl, "aura_rt::GcRootHandle<Point*> _h{");
    EXPECT_CONTAINS(unit.impl, "aura_rt::GcRootScope::Global");
    EXPECT_CONTAINS(unit.impl, "return _h.get();");
    // 模板②的判别点（① 没有这一行 ⇒ 本行断言把「多分配窗口」钉在产物上）
    EXPECT_CONTAINS(unit.impl, "_h.set(");
    // 守护：不得回退成「静态裸副本」（B-1/C-1 教训）
    EXPECT_NOT_CONTAINS(unit.impl, "static Point* _v =");
}

TEST(CodeGenEnum, BoundScalarRecordUsesTemplate1) {
    // L-1 第 (iii) 档的模板①分支（§4.8 第 2 步「至多一次分配」）：纯标量字段 record
    //   ⇒ 只有一个分配点（gc_alloc<P2>）⇒ ctor-init 形态；纯标量字段**仍是 GC 堆对象**
    //   （record 是 GcObject）⇒ 仍必须有 Global 根，不得走「零根直返」。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type P2 = { x: int }"
        " enum type Wrap2<P2> = { one = P2 { x = 1 } }"
        " fun main(io: Io) { let w = Wrap2.one }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "static P2* _val_one();");
    // ① = ctor-init：`_h{` 后**直接**跟值表达式（此处是 record 字面量的 IIFE），无 nullptr 占位
    EXPECT_CONTAINS(unit.impl, "aura_rt::GcRootHandle<P2*> _h{[&]() -> P2* {");
    EXPECT_CONTAINS(unit.impl, "aura_rt::GcRootScope::Global");
    EXPECT_CONTAINS(unit.impl, "return _h.get();");
    // ① 的判别点：不得出现 ② 的 nullptr 占位 + set
    EXPECT_NOT_CONTAINS(unit.impl, "GcRootHandle<P2*> _h{nullptr");
    EXPECT_NOT_CONTAINS(unit.impl, "_h.set(");
}

TEST(CodeGenEnum, BoundNoRootForEnumValue) {
    // L-1 第 (ii) 档（C1 判别力）：关联值是**另一 enum 壳**（值语义）⇒ 零根直返
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "enum type Color = { Red, Green }"
        " enum type Slot<Color> = { empty = Color.Red }"
        " fun main(io: Io) { let s = Slot.empty }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    EXPECT_CONTAINS(unit.header, "static Color _val_empty();");
    // E7：反向断言**必须**收窄到 `_val_empty` 的函数体——整个 impl 里 io / 闭包 / 驱动代码
    //   可能**合法**出现 GcRootHandle（全 impl 断言 = 假红，不是判别力）
    //   ⚠️ 切片锚点 = 产物里的**实际签名** `Color Slot::_val_empty() {`（generated 侧
    //      `<返回类型> <壳>::<访问器>`，非 `Color::_val_empty`）——见报告 D-2。
    const std::string body = funcBodyOf(unit.impl, "Slot::_val_empty()");
    ASSERT_TRUE(!body.empty());                       // 切片必须命中（防「切不到 ⇒ 断言空转」）
    EXPECT_CONTAINS(body, "Color::Red");
    // 误并入形式 B 时下面两条立即失败（GcRootHandle<Color> 亦非法，ExprGen.cpp:57-63）
    EXPECT_NOT_CONTAINS(body, "GcRootHandle");
    EXPECT_NOT_CONTAINS(body, "GcRootScope::Global");
}

TEST(CodeGenEnum, BoundAnonymousRecordRejected) {
    // E1 负例：匿名 record（`typeName` 空、非元组）作关联值。
    //   ❌ 不可接受 = 静默擦除成 `aura_rt::GcObject*`（TypeMap.cpp:584 兜底）并生成 `_val_bad`
    //   ✅ 可接受 = ①Parser/Sema 先拒；②Sema 放行而 §4.7 守卫报
    //              "has an anonymous record bound value"
    //   现状预期落 ①（匿名 record 无期望类型 ⇒ ExprInfer.cpp:319-326 报
    //   "cannot infer type of record literal"）⇒ 本用例的**主判据是「不得静默擦除」**；
    //   守卫文案断言只在「真走到 CodeGen」时生效（无条件断言文案会把 ① 误判成红 = 假红）。
    //   守卫的可达性判断见 §4.7 Why 第 2 条（防御性分支，不得因暂不可达删除）。
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "enum type Wrap = { bad = { x = 1 } }"
        " fun main(io: Io) { let w = Wrap.bad }",
        diag);
    ASSERT_TRUE(diag.hasErrors());
    if (!unit.header.empty()) {                    // 走到 CodeGen ⇒ 必须由守卫拦下
        EXPECT_TRUE(hasErrorContaining(diag, "has an anonymous record bound value"));
    }
    EXPECT_NOT_CONTAINS(unit.header, "_val_bad");
    EXPECT_NOT_CONTAINS(unit.impl, "GcObject* _val_bad");
}

// ============================================================
// bug-113：闭包捕获 const 局部（值捕获槽类型去 const）
//   修前：槽声明 `decltype(<源>)` ⇒ 源为 `const T` 时槽成员只读，而填槽生成
//   `__o_h.get()->cap_x = x;` ⇒ g++ "assignment of read-only member" /
//   "passing 'const T' as 'this' argument discards qualifiers"（生坏 C++）。
//   修后：值捕获槽 = `std::remove_const_t<decltype(<源>)>`（拷贝语义不变）。
// ============================================================

namespace {

// 取捕获槽 `name` 的**声明行**（形如 `<类型> <name>;`）；找不到返回空串。
// 跳过成员访问形态（`__o_h.get()->cap_x` 之类的使用点），只命中 struct 内声明。
std::string slotDeclLineOf(const std::string& code, const std::string& name) {
    const std::string key = " " + name + ";";
    size_t p = code.find(key);
    while (p != std::string::npos) {
        size_t b = code.rfind('\n', p);
        b = (b == std::string::npos) ? 0 : b + 1;
        std::string line = code.substr(b, p + key.size() - b);
        if (line.find("->") == std::string::npos) return line;
        p = code.find(key, p + 1);
    }
    return std::string();
}

} // namespace

TEST(CodeGenEnum, ClosureCaptureConstReceiverSelf) {
    // 情形 A：enum **值接收者**（`self` 是 `const <壳> self = *this;`）+ 内层闭包捕获 self
    //   ⇒ 修前 aurac 抛 g++ "passing 'const Color' as 'this'"（rc=1）
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "enum type Color = { Red, Green }"
        " fun (self Color) pick() -> bool {"
        "   let g = fun() -> bool { return self == Color.Red }"
        "   return g()"
        " }"
        " fun main(io: Io) { io.println(str(Color.Red.pick())) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    const std::string code = unit.header + unit.impl;
    const std::string decl = slotDeclLineOf(code, "cap_self");
    ASSERT_TRUE(!decl.empty());                       // 切片必须命中（防断言空转）
    // 承重断言：槽声明不带 const 限定（修前该行 = `decltype(self) cap_self;`，
    // 展开即 `const Color` ⇒ 只读成员 ⇒ 填槽赋值失败）
    EXPECT_NOT_CONTAINS(decl, "const ");
    EXPECT_NOT_CONTAINS(decl, "const Color");
    EXPECT_CONTAINS(decl, "std::remove_const_t<");
    // 槽仍按值存整个 enum 值（未被改成引用/指针/放弃捕获）
    EXPECT_CONTAINS(decl, "decltype(self)");
    EXPECT_CONTAINS(unit.impl, "__o_h.get()->cap_self = self;");
    // body 内捕获访问仍走槽（未改成引用捕获）
    EXPECT_CONTAINS(unit.impl, "__c_h.get()->cap_self == Color::Red");
}

TEST(CodeGenEnum, ClosureCaptureConstLocal) {
    // 情形 B：普通函数里 `const n = 5` 被闭包捕获
    //   ⇒ 修前 aurac rc=1："assignment of read-only member '…::__closure_0::cap_n'"
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "fun main(io: Io) {"
        "  const n = 5"
        "  let g = fun() -> int { return n }"
        "  io.println(str(g()))"
        "}",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    const std::string code = unit.header + unit.impl;
    const std::string decl = slotDeclLineOf(code, "cap_n");
    ASSERT_TRUE(!decl.empty());
    EXPECT_NOT_CONTAINS(decl, "const ");
    EXPECT_CONTAINS(decl, "std::remove_const_t<");
    EXPECT_CONTAINS(unit.impl, "__o_h.get()->cap_n = n;");
    EXPECT_CONTAINS(unit.impl, "__c_h.get()->cap_n");
}

TEST(CodeGenEnum, ClosureCaptureValueReceiverControl) {
    // 情形 C（对照，防误伤）：普通 record 值接收者 + 闭包捕获 self
    //   ⇒ 修前/修后都应 rc=0；receiver 槽走 cap_recv 指针路径，本缺陷修复**不得**动它
    Aura::DiagnosticEngine diag;
    auto unit = compileSource(
        "type Point = { x: int }"
        " fun (self Point) get() -> int {"
        "   let g = fun() -> int { return self.x }"
        "   return g()"
        " }"
        " fun main(io: Io) { io.println(str(Point { x = 3 }.get())) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
    const std::string code = unit.header + unit.impl;
    // receiver 槽保持指针形态（未受 remove_const_t 影响）
    const std::string decl = slotDeclLineOf(code, "cap_recv");
    ASSERT_TRUE(!decl.empty());
    EXPECT_CONTAINS(decl, "Point*");
    EXPECT_NOT_CONTAINS(decl, "remove_const_t");
}
