// src/CodeGen/MetaEmit.cpp —— feature-18 P3
//
// 职责：把 `MetaMerger`（主线程合并后的记录）渲染成 C++ 文本。
//   (A) emitTablesInline —— 单文件 Inline：表定义段（自含 include + thunk 前置声明）
//   (B) emitMetaHeader   —— 多文件 External：aura.meta.h（extern 声明，不含定义）
//   (C) emitMetaImpl     —— 多文件 External：aura.meta.cpp（唯一定义 + thunk 前置声明段）
//   (D) thunkSignature   —— thunk 的 C++ 签名文本（供 CodeGenerator 在 generate() 内拼体内）
//
// ⚠️ O2：生成码的 include 一律写 `#include "meta.h"`（**不带 `runtime/` 前缀**）——
//    两种模式的编译命令都已有 `-I runtime`，写全路径会去找 `runtime/runtime/meta.h`。
// ⚠️ E1：`file` 字面量必须转义（Windows 路径含 '\'，不转义会被 GCC 丢弃）。
// ⚠️ O13：表**定义**须落在 `namespace aura_rt::meta` 内（与 meta.h 的 extern 声明同命名空间）。
#include "MetaEmit.h"

#include "MetaCollect.h"   // MetaMerger / MetaSymbolRec / MetaTypeRec
#include "CodeGen.h"       // feature-18 P3（批 2）：CodeGenerator::escapeStringLiteral（O40-(a)：已移入 public）

#include <sstream>
#include <string>
#include <vector>

namespace Aura::MetaEmit {
namespace {

// ------------------------------------------------------------------
// lit —— 把字符串渲染为 **C++ 字符串字面量**（带引号 + 转义）。
//
// 🔴 feature-18 P3（批 2，O40-(a) 裁定落地点）：**本处在批 1 曾逐字复刻一份转义实现**
//    （为在「`escapeStringLiteral` 仍处 `private:` 段」时解阻塞）。批 2 按裁定：
//    **删除复刻版**，改为**直接调用** `CodeGenerator::escapeStringLiteral`
//    （该声明已由批 2 移入 `CodeGen.h` 的 `public:` 段）。
//    依据：E1 的教训本质是「**转义规则只能有一份**」—— `CodeGen`（`sourceFile_`）与
//    `MetaEmit`（`kSymbolTable[].file`）各持一份**必然分叉**。
//
// ⚠️ **批 1 遗漏（本批修正，见回报 §6）**：批 1 的本函数**只做转义、不包裹引号**，
//    而 §3.3(A) 的示例是 `{ "Player.heal", "D:/you/Aura/example/player.aura", 12 }`
//    （**带引号**）；漏引号 ⇒ 产物里出现 `{ Probe.bump, C:/.../probe.aura, 4 }`
//    ⇒ 生成码**编不过**（实测：`expected primary-expression before '/' token`）。
//    ⇒ 本函数语义定为「渲染 C++ 字符串字面量（含引号）」。
// ------------------------------------------------------------------
std::string lit(const std::string& s) {
    return "\"" + CodeGenerator::escapeStringLiteral(s) + "\"";
}

// 表项对 thunk 的取址引用：nsName 非空 ⇒ 限定名 <nsName>::<thunk>；空 ⇒ 单文件全局裸符号。
std::string thunkRef(const std::string& nsName, const std::string& thunkName) {
    return nsName.empty() ? thunkName : (nsName + "::" + thunkName);
}

// MetaSymbolRec::kind（0..3 编码，对齐 meta::SymbolKind）→ 枚举常量名。
const char* symbolKindName(uint8_t k) {
    switch (k) {
        case 0:  return "SymbolKind::Fn";
        case 1:  return "SymbolKind::Method";
        case 2:  return "SymbolKind::Ctor";
        case 3:  return "SymbolKind::BuiltinFn";
        default: return "SymbolKind::Fn";
    }
}

// MetaTypeRec::kind（TypeKindBits 位组合）→ 常量表达式。
std::string typeKindExpr(uint32_t bits) {
    struct Bit { uint32_t mask; const char* name; };
    static const Bit kB[] = {
        { 1u << 0, "TypeKindBits::Record" },
        { 1u << 1, "TypeKindBits::Prim" },
        { 1u << 2, "TypeKindBits::Str" },
        { 1u << 3, "TypeKindBits::Array" },
        { 1u << 4, "TypeKindBits::Optional" },
        { 1u << 5, "TypeKindBits::Union" },
        { 1u << 6, "TypeKindBits::Iterator" },
        { 1u << 7, "TypeKindBits::GenericInst" },
        { 1u << 8, "TypeKindBits::Interface" },
    };
    std::string out;
    for (const Bit& b : kB) {
        if (bits & b.mask) {
            if (!out.empty()) out += " | ";
            out += b.name;
        }
    }
    return out.empty() ? std::string("0") : out;
}

// MetaSymbolRec::flags（meta::SymbolFlags 位组合）→ 常量表达式。
std::string symbolFlagsExpr(uint32_t bits) {
    struct Bit { uint32_t mask; const char* name; };
    static const Bit kB[] = {
        { 1u << 0, "kSymThrows" },
        { 1u << 1, "kSymCoroutine" },
        { 1u << 2, "kSymGeneric" },
        { 1u << 3, "kSymStatic" },
        { 1u << 4, "kSymVariadic" },
    };
    std::string out;
    for (const Bit& b : kB) {
        if (bits & b.mask) {
            if (!out.empty()) out += " | ";
            out += b.name;
        }
    }
    return out.empty() ? std::string("0") : out;
}

// thunk 前置声明段（M2-(2)：表项要取 thunk 地址，而 thunk 定义在下方的 impl 段内 ⇒ 先声明）。
// nsName 非空 ⇒ namespace 块包裹（多文件，§3.3C）；空 ⇒ 全局裸符号（单文件，§3.3A / O32）。
std::string renderThunkDecls(const MetaMerger& merger) {
    std::ostringstream o;
    o << "// ← M2-(2)：thunk 前置声明（表项要取它的地址，而它定义在下方的 impl 段内）\n";
    for (const MetaSymbolRec& s : merger.allSymbols()) {
        // 🔴 feature-18 P4a 批 1（A1b-①，change.md §8.1 A1b / R3）：**空 `thunkName` ⇒ 跳过**。
        //   A1 的「降级不删」会把未发射 thunk 的记录（如 main —— 恒存在）留在 `symbols_` 里并把
        //   `thunkName` 清空 ⇒ 若照常渲染，`thunkSignature(nsName, "")` 会产出
        //   `aura_rt::CallableErased* (const aura_rt::CallArg* recv);`（无名函数声明，与下方符号表
        //   的取址呼应不上）⇒ 空名记录**没有 thunk 可声明**，必须跳过。
        if (s.thunkName.empty()) continue;
        const std::string sig = thunkSignature(s.nsName, s.thunkName);
        if (s.nsName.empty())
            o << sig << ";\n";
        else
            o << "namespace " << s.nsName << " { " << sig << "; }\n";
    }
    return o.str();
}

// 表数据 + 索引函数的**定义段**（须落在 `namespace aura_rt::meta` 内，O13）。
std::string renderTables(const MetaMerger& merger) {
    const std::vector<MetaSymbolRec>& syms = merger.allSymbols();
    const std::vector<MetaTypeRec>&   tys  = merger.allTypes();

    std::ostringstream o;
    o << "namespace aura_rt::meta {\n";

    // ---- 帧表：`[0,kSymbolCount)` 与符号表**严格平行**（🔴 O12；非帧项填占位）
    //      ＋ `[kSymbolCount,kFrameCount)` **匿名帧区**（🔵 feature-18 P4b-1 B5，裁定⑧）----
    o << "const FrameDesc   kFrameTable[]  = {\n";
    for (const MetaSymbolRec& s : syms) {
        if (s.isFrame)
            o << "    { " << lit(s.name) << ", " << lit(s.file) << ", " << s.defLine << " },\n";
        else
            o << "    { nullptr, nullptr, 0 },\n";
    }
    // 🔵 匿名帧区（协程上下文 lambda：spawn 块 / spawn 调用 / sync-for / 闭包）——
    //   这些 lambda **不在符号收集面**（无 seqInModule，§3.1.2 缺口 C）⇒ 编号走
    //   `kSymbolCount + kAnonFrameBase[m] + seqInAnon`（`meta.h` 的 `anonFrameIndexAt`）。
    //   行形态与符号帧**逐字同源**（`{ name, file, defLine }`）⇒ 渲染/消费侧零分支。
    for (const MetaAnonFrameRec& a : merger.allAnonFrames()) {
        o << "    { " << lit(a.name) << ", " << lit(a.file) << ", " << a.defLine << " },\n";
    }
    o << "};\n";
    // ⚠️ 🔵 B5 订正（裁定⑧ 与 meta.h:80 / 本行的旧形态**矛盾**，change.md 🟢-1 要求同步）：
    //    旧式 `kFrameCount = syms.size()`（即 `kFrameCount == kSymbolCount`）**已作废** ——
    //    它使匿名帧区 `[kSymbolCount,kFrameCount)` **恒空**。现式 = 平行区 + 匿名区。
    //    无协程上下文 lambda 时二者**仍然相等**（既有断言 `fc == sc` 不受影响）。
    o << "const uint32_t    kFrameCount    = "
      << (syms.size() + merger.allAnonFrames().size()) << ";\n";

    // ---- 🔵 B5：匿名帧区的**每模块起始下标**（前缀和；与 `kModuleBase` 同款公式）----
    //   `anonFrameIndexAt(m, s) = kSymbolCount + kAnonFrameBase[m] + s`
    //   ⚠️ 值 = `MetaMerger::addModule` 在**追加该模块匿名帧之前**取的 `anonFrames_.size()`。
    //   ⚠️ 落点同样**只在 renderTables 这一处** ⇒ Inline / External 两模式自动覆盖。
    o << "const uint32_t    kAnonFrameBase[] = {\n";
    for (uint32_t b : merger.anonBase())
        o << "    " << b << ",\n";
    o << "};\n";
    // ⚠️ **语义钉死**（防与 `kModuleCount` 混淆 —— 后者是「模块数」）：本常量是
    //   **匿名帧总条数**（= `kFrameCount - kSymbolCount`；= `allAnonFrames().size()`），
    //   **不是** `kAnonFrameBase` 的条数。`kAnonFrameBase[]` 的条数 == `kModuleCount`
    //   （每模块一项，含「本模块零匿名帧」的占位）。
    o << "const uint32_t    kAnonFrameCount = " << merger.allAnonFrames().size() << ";\n";

    // ---- 符号表 ----
    // SymbolInfo 字段序（§3.1）：name/file/defLine/kind/owner/params/paramCount/returnType/flags/materialize
    o << "const SymbolInfo  kSymbolTable[] = {\n";
    for (const MetaSymbolRec& s : syms) {
        o << "    { " << lit(s.name) << ", " << lit(s.file) << ", " << s.defLine
          << ", " << symbolKindName(s.kind)
          << ", nullptr, nullptr, 0, nullptr, " << symbolFlagsExpr(s.flags)
          << ", ";
        // 🔴 feature-18 P4a 批 1（A1b-②，change.md §8.1 A1b / R3）：**空 `thunkName` ⇒ 渲染裸
        //   `nullptr`（不带 `&`）**。A1「降级不删」的产物：记录留着（保编号）、物化列降级为空。
        //   ⚠️ 漏了这一步就会渲染成 `&`（或 `&ns::`）⇒ **坏 C++**（`expected primary-expression`），
        //      且 `main` **恒存在**（A2 起也进收集面）⇒ 每个程序都会炸（A6 即验此点）。
        //   `materialize == nullptr` 的语义 =「该符号不可运行时物化」，与 `meta.h:10-11` 的可空契约自洽。
        if (s.thunkName.empty())
            o << "nullptr },\n";
        else
            o << "&" << thunkRef(s.nsName, s.thunkName) << " },\n";
    }
    o << "};\n";
    o << "const uint32_t    kSymbolCount   = " << syms.size() << ";\n";

    // ---- 🔴 feature-18 P4a 批 1（A4，change.md §3.1.4 + §10 🟡-8）：`kModuleBase`（模块起始下标，前缀和）----
    // 语义：`symbolIdx = kModuleBase[moduleIdx] + seqInModule`（恒等于 `finalize()` 分配的全局 index）。
    // ⚠️ 值 = `MetaMerger::addModule` 在**追加该模块记录之前**取的 `symbols_.size()`
    //    （❌ 不是 `nextSymbolIndex_` —— 它只在 `finalize()` 写，`addModule` 时恒为 0）。
    // ⚠️ 落点**只在 renderTables 这一处** ⇒ `emitTablesInline` / `emitMetaImpl` 两模式自动覆盖
    //    （本函数是二者唯一的表渲染入口）；TU 归属随之自洽：Inline 模式落在生成的 TU，
    //    External 模式落在 `aura.meta.cpp` —— **不得**写成 `meta.h` 的 inline 变量（:5-6 红线，bug-86）。
    // ⚠️ 另出 `kModuleCount`（T9 要读 `kModuleBase[1]` ⇒ 须能判断条数 ≥ 2）。
    o << "const uint32_t    kModuleBase[]  = {\n";
    for (uint32_t b : merger.moduleBase())
        o << "    " << b << ",\n";
    o << "};\n";
    o << "const uint32_t    kModuleCount   = " << merger.moduleBase().size() << ";\n";

    // ---- 类型表 ----
    // TypeInfo 字段序（§3.1）：name/kind/desc/fields/fieldCount/methods/methodCount/typeParams/typeParamCount
    // ⚠️ 见回报 §6：§3.3(A) 的示例 `{ "Player", "", TypeKindBits::Record, "", 0 }` 只有 5 个值、
    //    且与 §3.1 的 TypeInfo 9 字段类型不符 ⇒ 此处按 §3.1 的定义（9 字段）渲染。
    o << "const TypeInfo    kTypeTable[]   = {\n";
    for (const MetaTypeRec& t : tys) {
        // ⚠️ **批 2 实测缺陷修正（见回报 §6）**：`TypeInfo.kind` 是 **`uint32_t`**，而
        //    `TypeKindBits` 是 **`enum class`（scoped）** ⇒ 直接写 `TypeKindBits::Record`
        //    在聚合初始化里 **`cannot convert ... to 'uint32_t'`**（实测）。§3.3(A) 的
        //    示例本身用的就是**裸数值** `/*kind*/1u`。此处按 §3.3(A) 的语义强制转换。
        o << "    { " << lit(t.name) << ", static_cast<uint32_t>(" << typeKindExpr(t.kind)
          << "), nullptr, nullptr, 0, nullptr, 0, nullptr, 0 },\n";
    }
    o << "};\n";
    o << "const uint32_t    kTypeCount     = " << tys.size() << ";\n";

    // ---- 便利查询 ----
    o << "const FrameDesc*  frameByIndex(uint32_t i)  { return i < kFrameCount  ? &kFrameTable[i]  : nullptr; }\n";
    o << "const SymbolInfo* symbolByIndex(uint32_t i) { return i < kSymbolCount ? &kSymbolTable[i] : nullptr; }\n";
    o << "} // namespace aura_rt::meta\n";
    return o.str();
}

} // namespace

// (A) 单文件 Inline：内嵌进生成的 .cpp（拼到 unit.metaImpl）
std::string emitTablesInline(const MetaMerger& merger) {
    std::ostringstream o;
    o << "// ---- feature-18 P3：符号元数据表（单文件模式内嵌；本产物仅一个 TU）----\n";
    o << "// ⚠️ 若本 .cpp 将来被多个 TU 引用 ⇒ 必须切多文件 External 形态（aura.meta.h/.cpp）。\n";
    o << "#include \"meta.h\"\n";   // O2：不带 runtime/ 前缀（-I runtime 直接命中）；M2-(1)：header 段不能动
    o << "\n";
    o << renderThunkDecls(merger);
    o << "\n";
    o << renderTables(merger);
    return o.str();
}

// (B) 多文件：aura.meta.h（extern 声明，**不含定义** —— 红线①）
std::string emitMetaHeader(const MetaMerger& /*merger*/) {
    std::ostringstream o;
    o << "#pragma once\n";
    o << "#include \"meta.h\"\n";   // O2
    o << "\n";
    o << "namespace aura_rt::meta {\n";
    o << "extern const FrameDesc   kFrameTable[];\n";
    o << "extern const uint32_t    kFrameCount;\n";
    o << "extern const uint32_t    kAnonFrameBase[];\n";   // 🔵 feature-18 P4b-1 B5（裁定⑧）
    o << "extern const uint32_t    kAnonFrameCount;\n";    // 🔵 feature-18 P4b-1 B5（裁定⑧）
    o << "extern const SymbolInfo  kSymbolTable[];\n";
    o << "extern const uint32_t    kSymbolCount;\n";
    o << "extern const TypeInfo    kTypeTable[];\n";
    o << "extern const uint32_t    kTypeCount;\n";
    o << "} // namespace aura_rt::meta\n";
    return o.str();
}

// (C) 多文件：aura.meta.cpp（**唯一定义** + thunk 前置声明段）
std::string emitMetaImpl(const MetaMerger& merger) {
    std::ostringstream o;
    o << "#include \"aura.meta.h\"\n";
    o << "\n";
    o << renderThunkDecls(merger);
    o << "\n";
    o << renderTables(merger);
    return o.str();
}

// (D) thunk 签名文本
std::string thunkSignature(const std::string& nsName, const std::string& thunkName) {
    // §3.3D：thunk 的 C++ 签名文本（**无函数体**）。
    // ⚠️ namespace 包裹由**调用点**负责：§3.5(d) 的 splice 点在 impl 的 namespace 块内（B13）；
    //    emit 侧的声明段按 nsName 自行包裹（见 renderThunkDecls）⇒ 此处只出签名行。
    //    nsName 入参保留以维持 §3.3D/O17 的**单一拼法**（避免两处拼名不一致）。
    (void)nsName;
    return "aura_rt::CallableErased* " + thunkName + "(const aura_rt::CallArg* recv)";
}

} // namespace Aura::MetaEmit
