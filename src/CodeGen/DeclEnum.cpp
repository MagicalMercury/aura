// ============================================================
// src/CodeGen/DeclEnum.cpp — enum 声明生成（change.md §4.4-⑨⑩）
//
// 形态 1（int 支撑）与形态 2（绑定类型）**共用同一个生成器**（A-1 撤销后的主人裁定）：
//   「`enum class` 外套壳」—— `enum class` 本身挂不了成员函数，但我们不往它身上挂，
//   挂的是**壳 struct**（`<Recv>::<method>` 对 struct 合法，逐字依据 change.md §4.3.6）。
//
// 唯一实质差别 = 关联值访问器（形态 2 的 `_val_*`，本批只做批 A：标量/字符串 ⇒ 形式 A）。
// 统一形态（逐字对齐 change.md §4.4-⑨ 改动 4 / §4.4-⑩ 的模板）：
//
//   struct Color {
//       enum class _Tag : uint8_t { Red = 0, Green = 1, Blue = 2 };  // 子名逐个过 safeName
//       _Tag _tag = _Tag::Red;                                       // 身份字段（值语义）
//       static const Color Red;                                      // 变体单例
//       int hex() const;                                             // 壳内方法声明（值语义 ⇒ const）
//       bool operator==(const Color& o) const { return _tag == o._tag; }
//       bool operator!=(const Color& o) const { return _tag != o._tag; }
//   };
//   // cpp：const Color Color::Red{Color::_Tag::Red};
//
// 三条「定死」（实施期不得各自发明）：
//   ① 变体值访问 ⇒ 静态单例 `Color::Red`（§4.4-⑪ 改动 1）；② match ⇒ tag 比较；
//   ③ 壳内成员函数一律 ` const`（否则在 `static const Color Red;` 上调方法 ⇒
//      "passing 'const Color' as 'this' argument discards qualifiers"）。
// ============================================================
#include "CodeGen.h"
#include "../Sema/SemType.h"

#include <cstdint>
#include <map>
#include <string>

namespace Aura {

// ============================================================
// 底层宽度（Sema 定、CodeGen 消费）—— change.md §4.4-⑤ 表
//   n == 0（空 enum）                    → uint8_t
//   maxVal ≤ 255 且 n ≤ 256              → uint8_t
//   maxVal ≤ 65535                       → uint16_t
//   maxVal ≤ 4294967295                  → uint32_t
//   否则                                 → uint64_t
//   形态 2 的 `_Tag` 取值 = 变体下标 i ⇒ 与形态 1 同一规则（maxVal = n-1）。
//   显式值 > 2^64-1 由 Lexer/strtoll 层面即失败（Sema 报错），此处不再判。
// ============================================================
std::string CodeGenerator::enumUnderlyingType(const EnumSemType& en) const {
    uint64_t maxVal = 0;
    for (size_t i = 0; i < en.variants.size(); ++i) {
        // 形态 1 = 显式/自增 intValue（非负，Sema 已校验）；形态 2 = 变体下标
        uint64_t x = en.plainInt ? static_cast<uint64_t>(en.variants[i].intValue)
                                 : static_cast<uint64_t>(i);
        if (x > maxVal) maxVal = x;
    }
    const size_t n = en.variants.size();
    if (n == 0) return "uint8_t";                       // 空 enum：合法、不可构造值
    if (maxVal <= 255ULL && n <= 256) return "uint8_t";
    if (maxVal <= 65535ULL) return "uint16_t";
    if (maxVal <= 4294967295ULL) return "uint32_t";
    return "uint64_t";
}

// TypeDecl 的 enum SemType 真源（§4.4-⑨「SemType 获取约定」）。
//   ⚠️ 批 3 实施口径（报红 B3-1，见报告）：CodeGen 侧**没有** SemAnalyzer / symtab 通路
//      （`generate()` 形参被 feature-18 锁定、不含 SemAnalyzer；`symtab_` 是 SemAnalyzer 的
//      private 成员）⇒ 「从 `Symbol::type` 装填」落地为「Sema 把同一个 `EnumSemType`
//      回填到 `TypeDecl::inferredType`，CodeGen 第一遍据此装填 `enumTypes_`」——
//      即 §4.4-⑨「SemType 获取约定」(a) 的主路径，对象身份与 `Symbol::type` 逐字相同。
const EnumSemType* CodeGenerator::enumTypeOf(const TypeDecl& decl) const {
    if (!decl.inferredType) return nullptr;
    return dynamic_cast<const EnumSemType*>(decl.inferredType);
}

// ============================================================
// 批 6（B2）跨模块（导入）enum 的装填（change.md §4.9.4-L18）
//
// 背景（实读，见报告 §侦查）：
//   · 导入模块的 `TypeDecl` **不在**本单元的 `program.decls` 里 —— `importExports`
//     （SemAnalyzer.cpp:249-257）只把类型注入**符号表**（`symtab_.defineGlobal`），
//     依赖模块的 AST 归它自己的编译单元 ⇒ CodeGen 第一遍扫 `TypeDecl` 时**看不到**导入 enum；
//   · ⇒ `enumTypes_` / `declaredEnumNames_` 只含本模块声明的 enum（原文口径如此），
//     本单元里所有「该名字是不是 enum ⇒ 值类型」的判定对它恒为 false。
// 真源：Sema 把 `EnumSemType`（符号表持有的同一对象）回填到**类型标注节点**
//   `NamedType::inferredType`（TypeResolver.cpp 的 resolveType enum 回填）——
//   与 `TypeDecl::inferredType`（DeclChecker.cpp:517-529）**同款一跳**，CodeGen 只读 AST。
// 键 = **源码限定名**（`a.Color`）：`n->namespacePrefix` 以 `.` 连接 + `n->name`，
//   正是本单元 C++ 侧写作 `a::Color` 的那个 Aura 写法的逐字形态。
// ⚠️ 只装填**带命名空间前缀**的标注（= 跨模块形态）；本地 enum 由第一遍的 `TypeDecl`
//   扫描装填（同名裸名不得被本函数二次登记，否则会污染 `kTypeTable` 的「只含用户声明类型」不变量）。
// ============================================================
std::string codegenQualifiedAuraTypeName(const NamedType& n) {
    std::string q;
    for (auto& p : n.namespacePrefix) q += p + ".";
    q += n.name;
    return q;
}

void CodeGenerator::registerImportedEnumAnnotation(const NamedType& n, const EnumSemType& en) {
    if (n.namespacePrefix.empty()) return;              // 本地/裸名：第一遍 TypeDecl 已登记
    const std::string qual = codegenQualifiedAuraTypeName(n);
    importedEnumNames_.insert(qual);
    // `declaredEnumNames_` 的 R-1 判据只看 `decl.receiverType`（裸名，无 `.`）⇒ 限定名登记无副作用；
    // 用途 = 本单元里该标注已经**声明级**可认出「是 enum」（与本地 enum 同一形态进入两张表）。
    declaredEnumNames_.insert(qual);
    enumTypes_.emplace(qual, &en);                      // 幂等：不覆盖本模块同名键
}

bool CodeGenerator::isImportedEnumTypeName(const NamedType& n) const {
    if (n.namespacePrefix.empty()) return false;
    return importedEnumNames_.count(codegenQualifiedAuraTypeName(n)) != 0;
}

void CodeGenerator::registerImportedEnumAnnotations(const TypeExpr* t) {
    if (!t) return;
    if (auto* n = dynamic_cast<const NamedType*>(t)) {
        // 🔴 就在此时认出「这标注是 enum」——同一节点也是 mapType 的消费点
        //   （TypeMap.cpp 的命名空间前缀分支据此不补 `*`）。
        if (auto* en = dynamic_cast<const EnumSemType*>(n->inferredType))
            registerImportedEnumAnnotation(*n, *en);
        for (auto& a : n->typeArgs) registerImportedEnumAnnotations(a.get());
        return;
    }
    if (auto* l = dynamic_cast<const ListType*>(t)) {
        registerImportedEnumAnnotations(l->elementType.get());
        return;
    }
    if (auto* r = dynamic_cast<const RecordType*>(t)) {
        for (auto& f : r->fields) registerImportedEnumAnnotations(f.type.get());
        return;
    }
    if (auto* tp = dynamic_cast<const TupleTypeExpr*>(t)) {
        for (auto& e : tp->elementTypes) registerImportedEnumAnnotations(e.get());
        return;
    }
    if (auto* u = dynamic_cast<const UnionType*>(t)) {
        for (auto& v : u->types) registerImportedEnumAnnotations(v.get());
        return;
    }
    if (auto* fn = dynamic_cast<const FunctionType*>(t)) {
        for (auto& p : fn->paramTypes) registerImportedEnumAnnotations(p.get());
        registerImportedEnumAnnotations(fn->returnType.get());
        return;
    }
}

// ============================================================
// 批 5（T1）变体单例 C++ 成员名 —— **唯一改名真源**
//
// 背景（`scripts/enum_verify_report.md` 的唯一 FAIL · 探针 p09）：
//   · Aura 侧 `enum type Foo = { Foo }`（同名 variant）**合法**（change.md §4.6.1 裁定 A）；
//   · C++ 侧**禁止静态数据成员与所在类同名**（成员声明与注入类名冲突）：
//       error: 'Foo::Foo' has the same name as the class in which it is declared
//     ⇒ 只改 C++ 生成侧（Aura 侧写法 `Foo.Foo` 逐字不变）。
//
// 规则（固定，不得各自发明）：
//   n = safeName(variantName)
//   if (n == safeName(enumRawName)) n += "_v";        // 固定后缀 `_v`
//   return n;
//
// 三处**必须**同源调用本函数（漏一处 = 坏 C++，只有 `g++` 能抓）：
//   ① 壳内声明      DeclEnum.cpp  `static const <X> <n>;`
//   ② 类外定义      DeclEnum.cpp  `const <X> <X>::<n>{ <X>::_Tag::<variant-src> };`
//   ③ 变体访问产点  ExprAccess.cpp `X.Variant` ⇒ `X::<n>`
//
// ⚠️ 不改 `_Tag` 枚举项名（仍 = safeName(variantName)）：它是 `_Tag` 的**枚举量**，
//    作用域在 `_Tag` 内（`Foo::_Tag::Foo` 合法，g++ 实测），且 StmtMatch 的 tag 比较
//    （`_match_val._tag == X::_Tag::<V>`）与 `_tag` 默认初值都按此名引用 ⇒ 改名会引入
//    第二套真源。`operator==`/`!=` 比的是 `_tag` 字段，不含单例名 ⇒ 无需改。
// ============================================================
std::string CodeGenerator::enumVariantCppName(const std::string& enumRawName,
                                              const std::string& variantName) const {
    const std::string shell = safeName(enumRawName);
    std::string n = safeName(variantName);
    if (n == shell) n += "_v";        // 撞壳名 ⇒ 固定后缀 `_v`（批 5 报告已登记）
    return n;
}

// ============================================================
// 形态 2：关联值访问器（形式 A + 批 B 的形式 B / 零根直返）
//
// 🔴 B-1/C-1/G-1（本批核心口径）：**不留静态裸缓存**
//   · 原「函数局部 static const GcString* _v = intern_string("…")」在 compact
//     （CompactScope::All）搬运 intern 对象后**悬垂**——compact 只回写**已注册根槽**
//     （globalRoots_ 元素 + 对象内 ptr_ref_ 槽），无从知晓别处另存的裸副本；
//   · 且 magic-static guard 内可能 alloc ⇒ 与 GC safepoint 交互有死锁面（C-1）。
//   ⇒ 形式 A = **每次调用直接 `intern_string(...)`**：intern 自带 L1 缓存（有代次校验）
//      + 池内 Global 根 ⇒ 零额外分配、地址恒为当前有效地址。
//
// 三分类（change.md §4.7，判据顺序固定）：
//   (i)   字符串字面量 / 标量字面量（int/float/bool）⇒ **形式 A**（本文件上方 4 个
//         dynamic_cast 分支，形态与 v1 逐字一致）。
//   (ii)  映射后 C++ 类型**不以 '*' 结尾**的值（另一 enum 壳 / None）⇒ **零根直返**
//         `return <值表达式>;`（无 GcRootHandle、无 Global 根）。
//   (iii) 映射后**以 '*' 结尾**（record / 元组，防御性覆盖 List / Optional）⇒ **形式 B**：
//         函数局部 `static aura_rt::GcRootHandle<T> _h{…, Global}` + `return _h.get();`
//         （模板 ① / ② 见 §4.8；模板 ② 不消除「表达式内部第二次分配」窗口）。
//
//   批 B **已落地**（不再是「本批不生成访问器」）：第 (ii) 档零根、第 (iii) 档 Global 根。
//   不留半实现的假访问器：映射不出 C++ 类型 / 匿名 record（空 canonicalName 的非元组
//   record）一律生成期 error，绝不把 `/* unknown_… */` 或擦除后的 `GcObject*` 写进产物。
// ============================================================
void CodeGenerator::emitBoundValueAccessors(std::ostream& h, std::ostream& cpp,
                                            const TypeDecl& decl, const EnumSemType& en) {
    const std::string shell = safeName(en.canonicalName.empty() ? en.name : en.canonicalName);
    for (auto& v : en.variants) {
        const std::string accName = "_val_" + safeName(v.name);
        const ASTNode* ve = v.valueExpr;
        if (!ve) continue;                       // 形态 2 无值 ⇒ Sema 已报错，此处不产

        // ---- 字符串字面量：形式 A（直调 intern_string）----
        if (auto* sl = dynamic_cast<const StringLiteral*>(ve)) {
            h << "    static const aura_rt::GcString* " << accName << "();\n";
            cpp << "const aura_rt::GcString* " << shell << "::" << accName << "() {\n"
                << "    return " << genStringLiteral(*sl) << ";\n"
                << "}\n";
            continue;
        }
        // ---- 标量字面量：形式 A（直接返回字面量）----
        if (auto* il = dynamic_cast<const IntLiteral*>(ve)) {
            const std::string ty = (il->value > 2147483647LL) ? "int64_t" : "int32_t";
            h << "    static " << ty << " " << accName << "();\n";
            cpp << ty << " " << shell << "::" << accName << "() {\n"
                << "    return " << std::to_string(il->value) << ";\n"
                << "}\n";
            continue;
        }
        if (auto* fl = dynamic_cast<const FloatLiteral*>(ve)) {
            h << "    static double " << accName << "();\n";
            cpp << "double " << shell << "::" << accName << "() {\n"
                << "    return " << genFloatLiteral(*fl) << ";\n"
                << "}\n";
            continue;
        }
        if (auto* bl = dynamic_cast<const BoolLiteral*>(ve)) {
            h << "    static bool " << accName << "();\n";
            cpp << "bool " << shell << "::" << accName << "() {\n"
                << "    return " << genBoolLiteral(*bl) << ";\n"
                << "}\n";
            continue;
        }
        // ---- 批 B：三分类（enum v1.1 L-1-b；判据顺序固定，不得改序）----
        //   (ii) 映射后 C++ 类型**不以 '*' 结尾**（非 GC 值：另一 enum 壳 / None）⇒ 零根直返
        //   (iii) 映射后**以 '*' 结尾**（record / 元组）⇒ 形式 B（Global 根）
        //         ⚠️ 数组 / Optional 亦落本档，但属**防御性分支：现行 Sema 不可达**
        //            （白名单只放 RecordExpr，DeclChecker.cpp:561-563；无数组字面量 / some(...)）
        //   ⚠️ 不得用 `isHeapSemType` 单判：它对 `PrimSemType::String` 返回 true（ExprGen.cpp:36-37），
        //      而字符串走形式 A（上方字符串分支已 continue）；也不得用「AST 节点种类」判（valueExpr
        //      为 RecordExpr 只说明书写形态，不说明元素堆性）。
        const SemType* vt = v.valueType.get();
        if (!vt) {
            diag_.error(decl.line, decl.col,
                        "codegen: enum variant '" + v.name + "' has no SemType for its bound value "
                        "(internal: Sema did not fill EnumVariantSem::valueType; see change.md §4.8 Step 0)");
            continue;
        }
        const std::string cppTy = mapSemType(*vt);
        // ---- 匿名 record 守卫（E1 替代修法）----
        //   mapSemType(RecordSemType) 三分支：元组现场合成 TupleN<...>*（TypeMap.cpp:572-579）/
        //   具名 canonicalName + "*"（:581-582）/ 兜底 "aura_rt::GcObject*"（:584）
        //   ⇒ 未具名的非元组 record 会被**静默擦除**成 GcObject*（字段全丢、文本看似合法，
        //      只有 g++ 在使用点才可能报错）⇒ 生成期必须硬失败。
        //   ⚠️ **不要**写 `cppTy == "*"` 或 `cppTy.front() == '*'` 守卫：该产物在本仓不可能出现
        //      （兜底是 GcObject*），属死代码，只会误导后来者。
        if (auto* rs = dynamic_cast<const RecordSemType*>(vt)) {
            if (!rs->isTuple && rs->canonicalName.empty()) {
                diag_.error(decl.line, decl.col,
                            "codegen: enum variant '" + v.name + "' has an anonymous record bound value "
                            "(RecordSemType.canonicalName is empty); name the record type (see change.md §4.7 E1)");
                continue;
            }
        }
        if (cppTy.empty() || cppTy.rfind("/* unknown_", 0) == 0) {
            // mapSemType 的兜底产物是注释形态（TypeMap.cpp:678）⇒ 绝不落到生成文本里
            diag_.error(decl.line, decl.col,
                        "codegen: enum variant '" + v.name + "' has unmappable bound value type '"
                        + vt->toString() + "' (mapSemType produced no C++ type)");
            continue;
        }
        h << "    static " << cppTy << " " << accName << "();\n";
        cpp << cppTy << " " << shell << "::" << accName << "() {\n";
        if (cppTy.back() != '*') {
            // 第 (ii) 档：值语义（另一 enum 壳的裸壳名 / aura_rt::NoneType）
            //   ⇒ 直接返回值表达式：无 GcRootHandle、无 GcRootScope::Global（判别力用例守护）
            cpp << "    return " << genExpr(*ve, false) << ";\n";
        } else {
            // 第 (iii) 档：形式 B。模板 ①/② 按 §4.8 选择条件**静态**判定
            //   （等价于分配点计数：record 本体 1 次，每个堆字段再 ≥1 次）：
            //     · record 纯标量字段 ⇒ 恰好 1 次分配 ⇒ **模板 ①**（ctor-init）
            //       （实测：`P2 { x = 1 }` = 1，gc_alloc<P2> 一处）
            //     · record 含 ≥1 个堆字段 ⇒ ≥2 次分配 ⇒ **模板 ②**（nullptr 占位 + set）
            //       （实测：`Point { x = 1, name = "ON" }` = 2，gc_alloc<Point> + intern_string("ON")）
            //     · tuple / List / Optional ⇒ 防御性落 **模板 ②**（本体 + 元素/装箱 ≥2；
            //       现行语言层不可达，见 §4.7 E3）
            //   ⚠️ ② 只保证「根槽先存在」，**不消除**表达式内部第二次分配导致的移位窗口
            //      （该窗口由 genExpr 产物自身的中间根化承担，本档不承诺；§4.8 / §7 R3）。
            bool multiAlloc = true;                       // 默认（tuple / List / Optional）防御取 ②
            if (auto* rs = dynamic_cast<const RecordSemType*>(vt)) {
                if (!rs->isTuple) {
                    multiAlloc = false;                   // 具名 record：按字段堆性判定
                    for (auto& f : rs->fields) {
                        if (isHeapSemType(f.type.get())) { multiAlloc = true; break; }
                    }
                }
            }
            if (multiAlloc) {
                cpp << "    static aura_rt::GcRootHandle<" << cppTy << "> _h{nullptr, aura_rt::GcRootScope::Global};\n"
                    << "    _h.set(" << genExpr(*ve, false) << ");\n"
                    << "    return _h.get();\n";
            } else {
                cpp << "    static aura_rt::GcRootHandle<" << cppTy << "> _h{" << genExpr(*ve, false)
                    << ", aura_rt::GcRootScope::Global};\n"
                    << "    return _h.get();\n";
            }
        }
        cpp << "}\n";
    }
}

// ============================================================
// 壳保留成员名冲突检查（§4.4-⑪ 改动 5-④；R-2 的 6 类清单，逐字列全）
//   壳实际自带的成员名：① `_tag` ② `_Tag` ③ `operator==` ④ `operator!=`
//   ⑤ 全部变体**单例名**（safeName 后）⑥（形态 2）全部 `_val_*` 关联值访问器名
//   ③④ 是 Aura 标识符写不出来的名字（今语法下永不命中）——**仍必须列**：清单要表达
//   「壳占了哪些名字」，将来语法放宽时不会漏。
//   ⚠️ 不能用 `_fun` 改名规避：调用点（ExprMethodCall）不参与改名 ⇒ 声明侧改名、调用侧不改
//      ⇒ 编译/链接错。⇒ **直接生成期 error**（对齐仓内既有做法：ExprClosureOldPath.cpp:78–81）。
// ============================================================
void CodeGenerator::checkShellMemberNameConflicts(const TypeDecl& decl, const EnumSemType& en) {
    std::set<std::string> reserved = {
        "_tag",          // ①
        "_Tag",          // ②
        "operator==",    // ③
        "operator!=",    // ④
    };
    // ⑤ 变体单例名 —— 批 5（T1）：**必须**走 `enumVariantCppName`（与壳内声明/类外定义/
    //    变体访问三处同源），否则撞名改名后的实际成员名（`Foo_v`）不在清单里 ⇒ 方法名
    //    撞上去也查不出来（静默坏 C++）。
    //    同时兜底：改名映射在退化角落里**非单射**（`enum type Foo = { Foo, Foo_v }`
    //    ⇒ 两个变体都落到 `Foo_v`）⇒ 生成期 error（本文件既有口径「不产坏 C++」，
    //    见下方方法冲突同款处理），不静默产重名成员。
    std::map<std::string, std::string> singletonOwner;   // C++ 成员名 → Aura 侧变体名
    for (auto& v : en.variants) {
        const std::string cn = enumVariantCppName(decl.name, v.name);
        auto ins = singletonOwner.emplace(cn, v.name);
        if (!ins.second && ins.first->second != v.name) {
            diag_.error(decl.line, decl.col,
                        "codegen: enum '" + decl.name + "' variants '" + ins.first->second
                        + "' and '" + v.name + "' both map to the C++ singleton name '" + cn
                        + "' (same-name-variant rename suffix '_v'); rename one of the variants");
        }
        reserved.insert(cn);                                            // ⑤ 变体单例名
        if (!en.plainInt)                                               // ⑥ 仅形态 2 有关联值访问器
            reserved.insert("_val_" + safeName(v.name));                //    （名字加工必须与
    }                                                                   //     emitBoundValueAccessors 同源）
    for (auto& md : pendingMethods_) {
        if (md.receiverType != decl.name) continue;
        std::string cn = safeName(md.methodName);
        if (reserved.count(cn)) {
            // 批 5（T3）：诊断位置 —— 此前恒为 `0:0`（无行列）。位置真源优先级：
            //   ① 方法声明自身（`fun` 关键字处，PendingMethod::declLine/declCol，Parser
            //      setNodePos 给出）；② 缺失（0）时回退到宿主 enum 的 TypeDecl 行列。
            int line = md.declLine, col = md.declCol;
            if (line <= 0) { line = decl.line; col = decl.col; }
            diag_.error(line, col,
                "codegen: enum method '" + md.methodName
                + "' collides with the generated enum shell member '" + cn
                + "' (reserved: _tag / _Tag / operator== / operator!= / variant singleton names"
                  " / bound-value accessors _val_*); rename the method");
        }
    }
}

// ============================================================
// genEnumDecl —— 形态 1/2 统一生成器（change.md §4.4-⑨ 改动 4 / §4.4-⑩）
// ============================================================
void CodeGenerator::genEnumDecl(std::ostream& h, std::ostream& cpp,
                                const TypeDecl& decl, const EnumTypeExpr& body) {
    // enum 类型真源（§4.4-⑨「SemType 获取约定」）：variant 表（含 intValue / 底层宽度 /
    //   形态 2 的值）**取自 Sema 写入的 EnumSemType**，而不是重新遍历 AST 复算
    //   （复算会与 Sema 的约束校验分叉 ⇒ 两处真源）。
    const EnumSemType* en = enumTypeOf(decl);
    if (!en) {
        // 防御（Step 0 准入 gate 已在前置拦截；此处兜底不产坏 C++）
        diag_.error(decl.line, decl.col,
                    "codegen: internal: enum '" + decl.name + "' has no EnumSemType "
                    "(enumTypes_ not filled before generation; see change.md §4.8 Step 0)");
        return;
    }
    const std::string under = enumUnderlyingType(*en);
    const std::string shell = safeName(decl.name);
    (void)body;   // variant 真源 = Sema 的 EnumSemType（不复算 AST，避免双真源）

    // ---- 1. 壳头 + 内嵌 `_Tag`（形态 1/2 同款）----
    h << "struct " << shell << " {\n";
    // `_Tag` 体先拼串再发射：非空 ⇒ `{ Red = 0, … }`；空 ⇒ `{ }`（两处与 change.md 的
    //   正文模板 / §4.6.1 的产物断言逐字一致）。
    //   · 形态 1 = 显式 intValue（自增/显式都由 Sema 定死，逐条显式写出 ⇒ 跨编译单元稳定）；
    //   · 形态 2 = 变体下标 i；
    //   · 子名**逐个过 safeName**（否则 `class`/`enum`/`static` 等变体名会产
    //     `enum class _Tag : uint8_t { class = 0 };` ⇒ 坏 C++）。
    std::string tagBody;
    for (size_t i = 0; i < en->variants.size(); ++i) {
        if (i > 0) tagBody += ", ";
        tagBody += safeName(en->variants[i].name) + " = "
                 + std::to_string(en->plainInt ? en->variants[i].intValue
                                               : static_cast<int64_t>(i));
    }
    h << "    enum class _Tag : " << under << " {";
    if (tagBody.empty()) h << " ";              // 空 enum ⇒ `{ }`（无变体）
    else                 h << " " << tagBody << " ";
    h << "};\n";
    // 身份字段：非空 enum 默认取首个变体；空 enum **不给**默认初值（无变体可默认）
    h << "    _Tag _tag";
    if (!en->variants.empty())
        h << " = _Tag::" << safeName(en->variants[0].name);
    h << ";\n";

    // ---- 2. 变体单例（形态 1 也有：两形态统一，值语义 = _tag）----
    //   批 5（T1）：成员名走 `enumVariantCppName`（撞壳名 ⇒ `_v`），与类外定义（下方 6）
    //   及变体访问产点（ExprAccess.cpp）**同源**。`_Tag` 项名仍 = safeName(v.name)。
    for (auto& v : en->variants)
        h << "    static const " << shell << " " << enumVariantCppName(decl.name, v.name) << ";\n";

    // ---- 3. 形态 2 专属：关联值访问器（批 A 形式 A + 批 B 形式 B / 零根直返）----
    if (!en->plainInt) {
        h << "\n";
        emitBoundValueAccessors(h, cpp, decl, *en);
    }

    // ---- 4. operator== / !=（比 _tag；裁定 7「按 variant 身份」）----
    //   注：`enum class` 的「免费 ==」优势随壳构造消失 ⇒ 必须显式给（两形态同款）。
    h << "    bool operator==(const " << shell << "& o) const { return _tag == o._tag; }\n";
    h << "    bool operator!=(const " << shell << "& o) const { return _tag != o._tag; }\n";

    // ---- 5. 保留名冲突检查 + 壳内方法声明（共用 helper；值语义 ⇒ 追加 const）----
    checkShellMemberNameConflicts(decl, *en);          // 生成期 error（不产坏 C++）
    emitPendingMethodDecls(h, decl.name, {});          // fieldNames 传空：壳无「字段」概念

    h << "};\n\n";

    // ---- 6. 单例 out-of-class 定义（与 §4.4-⑩ 同一写法）----
    //   批 5（T1）：成员名**同源** `enumVariantCppName`（漏改 ⇒ `'const Foo Foo::Foo' is not a
    //   static data member of 'struct Foo'`）；初始化器里的 `_Tag::<v>` 用**原**变体名。
    for (auto& v : en->variants) {
        cpp << "const " << shell << " " << shell << "::" << enumVariantCppName(decl.name, v.name)
            << "{" << shell << "::_Tag::" << safeName(v.name) << "};\n";
    }
    cpp << "\n";
}

} // namespace Aura
