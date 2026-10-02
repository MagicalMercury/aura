// ============================================================
// runtime/meta.h —— feature-18 P3：符号元数据表结构（含 feature-10 §9.1 的 file/defLine）
//
// 设计约束（feature-18 §7.6 两条红线）：
//   ① **表定义落在独立 TU**（多文件模式 = aura.meta.cpp），本头文件**只放结构 + extern 声明**；
//      ⚠️ 不得把表定义写成本头里的 inline 变量 —— MinGW 多 TU 包含会 multiple definition
//      （实测事故：g_syncStack 的 TLS init function 冲突，见 bug-86 排查记录）。
//   ② 索引由**汇总段统一分配**（main.cpp），各模块不得自行编号。
//
// 性能/体积：全部 POD（编译期常量初始化 ⇒ 落在 .rodata，无静态构造）；「列」均为可选投影
//   （params/returnType/materialize/offset/fields/methods 可为 nullptr + 计数 0）⇒ 将来裁剪（R4）不改结构。
// ============================================================
#pragma once

#include <cstdint>

namespace aura_rt { struct TypeDescriptor; class CallableErased; struct CallArg; }

namespace aura_rt::meta {

enum class SymbolKind : uint8_t { Fn, Method, Ctor, BuiltinFn };

enum class TypeKindBits : uint32_t {
    Record      = 1u << 0,
    Prim        = 1u << 1,
    Str         = 1u << 2,
    Array       = 1u << 3,
    Optional    = 1u << 4,
    Union       = 1u << 5,
    Iterator    = 1u << 6,
    GenericInst = 1u << 7,
    Interface   = 1u << 8,
};

// ---- SymbolInfo.flags 位定义（change.md §3.1 注：本档新增，写入 meta.h）----
// 与 SymbolInfo::flags（uint32_t）配合；生成码在 `namespace aura_rt::meta` 内以裸名引用
// （如 `kSymThrows`，见 change.md §3.3(A)）。
enum SymbolFlags : uint32_t {
    kSymThrows    = 1u << 0,
    kSymCoroutine = 1u << 1,
    kSymGeneric   = 1u << 2,
    kSymStatic    = 1u << 3,
    kSymVariadic  = 1u << 4,
};

struct TypeInfo;
struct SymbolInfo;

// feature-18 帧表（全量）：P4 的逻辑栈帧注入引用它；本阶段只产出表本身。
struct FrameDesc { const char* name; const char* file; uint32_t defLine; };

struct ParamInfo { const char* name; const TypeInfo* type; bool is_gc; };
struct FieldInfo { const char* name; const TypeInfo* type; uint32_t offset; bool is_gc_pointer; };

struct SymbolInfo {
    const char*      name;        // Aura 限定名（函数 = "heal"；方法 = "Player.heal"）—— 裁定 ②
    const char*      file;        // feature-10 §9.1（真实源文件路径，与 Error.file 同源）
    uint32_t         defLine;     // feature-10 §9.1
    SymbolKind       kind;
    const TypeInfo*  owner;       // 方法所属类型；函数 = nullptr
    const ParamInfo* params;      uint32_t paramCount;     // 可空（nullptr + 0）
    const TypeInfo*  returnType;  // 可空
    uint32_t         flags;       // bit0 throws | bit1 coroutine | bit2 generic | bit3 static | bit4 variadic
    // 物化配方（feature-10 §3.2 C1：存 factory，不存裸函数指针）。
    // ⚠️ recvOrNull == nullptr ⇒ 函数/静态/构造器；非空 ⇒ **方法接收者放在 recvOrNull[0]**（本档 §0.4 裁定 ③）
    CallableErased* (*materialize)(const CallArg* recvOrNull);
};

struct TypeInfo {
    const char*            name;
    uint32_t               kind;        // TypeKindBits 位组合
    const TypeDescriptor*  desc;        // GC desc（可空）
    const FieldInfo*       fields;  uint32_t fieldCount;    // 初步投影可为空（裁定 ⑥）
    const SymbolInfo*      methods; uint32_t methodCount;
    const TypeInfo* const* typeParams; uint32_t typeParamCount;
};

// ---- 索引访问（下标化；索引由汇总段统一分配）----
extern const FrameDesc   kFrameTable[];
// 帧表条数（P4 用）—— 🔵 **feature-18 P4b-1 B5 订正（裁定⑧ 落地）**：
//   `kFrameCount == kSymbolCount` 的旧式**已作废**（它使匿名帧区恒空，与 §0.5 裁定⑧ 矛盾，
//   见 change.md 🟢-1）。现式 = `kSymbolCount + kAnonFrameCount`
//   ⇒ 帧表 = `[0,kSymbolCount)` 平行区（与符号表逐条平行，非帧项占位）+ 匿名帧区。
//   ⚠️ 无协程上下文 lambda 时二者**仍然相等**（既有 `fc == sc` 断言不受影响）。
extern const uint32_t    kFrameCount;
extern const SymbolInfo  kSymbolTable[];
extern const uint32_t    kSymbolCount;
extern const TypeInfo    kTypeTable[];
extern const uint32_t    kTypeCount;

// ---- 🔴 feature-18 P4a 批 1（A5，change.md §3.1.4 + §10）：模块起始下标表 ----
// `symbolIdx = kModuleBase[moduleIdx] + seqInModule` —— 与 `MetaMerger::finalize()` 分配的全局
// `index` **恒等**（前提：收集面无序号空洞，见 change.md §3.1.2 的两处缺口修复与 §3.1.5）。
// 用途：P4a 的帧注入在**编译期**拿不到全局 index（它晚于注入点），故注入 `(moduleIdx, seqInModule)`
// 两个编译期常量，运行期用本函数换算。
// ⚠️ **TU 归属红线（本文件 `:5-6`，bug-86）**：**只在本头里 `extern` 声明**，定义落独立 TU
//   （Inline 模式 = 生成的 TU；External 模式 = `aura.meta.cpp`）——**不得**写成头内 inline 变量定义
//   （MinGW 多 TU 包含 ⇒ multiple definition）。下面只有 **inline 函数**（函数不触发该问题）。
// ⚠️ `kModuleBase[m]` **不做下标检查**（热路径、且 `m` 是编译器生成的编译期常量；越界只会来自
//   编译器自身的 bug ⇒ 不为此付运行期开销）。
extern const uint32_t    kModuleBase[];
extern const uint32_t    kModuleCount;     // 模块数（= `kModuleBase` 条数；T9 判断是否有第 2 个模块）

inline uint32_t symbolIndexAt(uint32_t moduleIdx, uint32_t seqInModule) {
    return kModuleBase[moduleIdx] + seqInModule;
}

// ============================================================
// 🔵 feature-18 P4b-1 B5（裁定⑧「两级帧区」）—— **匿名帧区**访问器
//
//   用途：协程上下文 lambda（spawn 块 / spawn 调用 / sync-for / 闭包）**不在符号收集面**
//   ⇒ 无 `seqInModule`、**不能**走 `symbolIndexAt`（走它会命中平行区里的**另一个符号**
//   —— 静默错 trace，不崩不报）。它们由**匿名帧区**编号：
//
//       frameIdx = kSymbolCount + kAnonFrameBase[moduleIdx] + seqInAnon
//
//   `kAnonFrameBase[]` 与 `kModuleBase[]` **同语义**（模块在**本区**内的起始下标、前缀和），
//   但**不同区** ⇒ 两张表；`kFrameCount = kSymbolCount + kAnonFrameCount`。
//   ⚠️ **`kAnonFrameCount` = 匿名帧总条数**（**不是** `kAnonFrameBase` 的条数 —— 后者
//      == `kModuleCount`，每模块一项含零匿名帧的模块）。
//   ⚠️ 与 `kModuleBase` 同样**不做下标检查**（热路径；实参是编译器生成的编译期常量，
//      越界只会来自编译器自身的 bug ⇒ 不为此付运行期开销）。
// ============================================================
extern const uint32_t    kAnonFrameBase[];
extern const uint32_t    kAnonFrameCount;

inline uint32_t anonFrameIndexAt(uint32_t moduleIdx, uint32_t seqInAnon) {
    return kSymbolCount + kAnonFrameBase[moduleIdx] + seqInAnon;
}

// 便利查询（O(n) 线性扫，仅诊断路径使用；n 为编译期常量、规模小）
const FrameDesc*  frameByIndex(uint32_t idx);
const SymbolInfo* symbolByIndex(uint32_t idx);

} // namespace aura_rt::meta
