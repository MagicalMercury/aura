// src/CodeGen/MetaCollect.h —— feature-18 P3
#pragma once
#include <cstdint>
#include <set>
#include <string>
#include <vector>
#include <unordered_map>

namespace Aura {

struct Decl;            // 前向（AST/Stmt.h）
class  SemType;         // 前向

// 一条「收集记录」：编译期中间形态（**不含 C++ 语法，纯数据**）
struct MetaSymbolRec {
    std::string name;        // Aura 限定名（裁定 ②）
    std::string file;        // 真实源文件路径
    uint32_t    defLine = 0;
    uint8_t     kind = 0;    // 0=Fn 1=Method 2=Ctor 3=BuiltinFn（对齐 meta::SymbolKind）
    uint32_t    flags = 0;   // 🔴 O14 修订：**`meta::SymbolFlags` 是本档在 `runtime/meta.h` 新定义的枚举**
                             //   （**不是**既有符号；`MetaSymbolRec` 与枚举同头，归属无歧义）
    std::string ownerName;   // 方法所属类型的 Aura 名（函数为空）
    std::string nsName;      // 该符号所在的 C++ 命名空间（用于 extern 限定名；裁定见 §3.3）
    // ---- 🔴 N2/N3 修订（GLM 二审，2026-10-01）：以下三字段是「thunk 名单源化」的落点 ----
    std::string moduleKey;        // 所属模块（拓扑序稳定键 = path）—— **per-module 分组的键**
    uint32_t    seqInModule = 0;  // 模块内序号（`collectSymbol` 时由 **per-module 计数器**当场分配）
    std::string thunkName;        // "_aura_mat_" + nsStem + "_" + seqInModule —— **collectSymbol 时一次定型**，
                                  // 此后 thunk 定义侧与 emit 侧**只读**（**不再有 finalizeThunkNames 第二步**）
    uint32_t    index = 0xFFFFFFFFu;   // **全局索引** —— 仅由 `MetaMerger::finalize()`（主线程汇总段）写；⚠️ **仅用于表序**，
                                       // **不参与 thunk 命名**（记录未分配时 = kUnassigned）
    bool        isFrame = false;   // 是否进帧表（P4 用：函数/方法 = true）
};

// ============================================================
// 🔵 feature-18 P4b-1 B5：**匿名帧记录**（change.md 裁定⑧「两级帧区」）
//
// 背景（§3.1.2 缺口 C / §9-N4）：协程上下文 lambda（spawn 块 / spawn 调用 / sync-for /
// 闭包）**不在符号收集面**（`collectSymbol` 只挂 FunDecl/MethodDecl 钩子）⇒ **无
// `seqInModule`**。P4a 因此把整组移到 P4b（R1 修正）——**不得退回**。
//
// 方案（裁定⑧）：帧表分两级 ——
//   `[0, kSymbolCount)`        平行区（与符号表逐条平行，O12 的非帧项占位保留）
//   `[kSymbolCount, kFrameCount)` **匿名帧区**（本结构逐条渲染）
// ⇒ 匿名 lambda 的注入编号 = `kSymbolCount + kAnonFrameBase[moduleIdx] + seqInAnon`
//   （`runtime/meta.h` 的 `anonFrameIndexAt`；**不是** `symbolIndexAt`）。
// ⚠️ `kAnonFrameBase[]` 与 `kModuleBase[]` **同语义**（模块在**本区**内的起始下标、前缀和），
//    但**不同区** ⇒ 两张表、两个访问器（多模块下匿名帧也按 orderedModules 序连续排布）。
// ============================================================
struct MetaAnonFrameRec {
    std::string name;        // 可读帧名（渲染成 `FrameDesc.name`，如 `<spawn@12>`）
    std::string file;        // 真实源文件路径（与 Error.file / 符号表 file 同源）
    uint32_t    defLine = 0; // 语句/闭包定义行
};

struct MetaTypeRec {
    std::string name;        // Aura 类型名
    uint32_t    kind = 0;    // meta::TypeKindBits 位组合
    std::string nsName;      // _desc 所在命名空间
    std::string descSymbol;  // 🔴 O11 修订：`_desc` 的 **C++ 限定名**，**基础投影阶段恒为空串**
                             // ⚠️ 实测 `_desc` 是**类静态成员**：
                             //   · `runtime/types.h:268` 的 `static const TypeDescriptor _desc;` 是 **Error 类自己的**（runtime 侧同型实例）
                             //   · **用户 record 的 `_desc` 在生成侧**：声明 `DeclGen.cpp:147`、定义 `TypeMap.cpp:661+`
                             //   🔴 O38 修订（终审）：原只引 `types.h:268`（那是 Error 的）⇒ 证据行须补生成侧两处
                             //    裸 `"Player_desc"` **不是**合法 C++ 符号名（须 `User::_desc`，且定义在别处）
                             //    ⇒ 原示例失实。本阶段（基础投影）**不解析**该列，预留字段、写空串。
    uint32_t    index = 0;
};

// ============================================================
// 🔴 O1 修订（三审：GLM 主线 X1 × 盲审 O1 **双盲收敛**，2026-10-01）
//
// **问题**：本仓多文件 CodeGen 是**真线程并行** —— `main.cpp:453-463` 在 `cgN>=2` 时对
// **每个模块**发 `std::async(std::launch::async, [&, mod]{ return runCgModule(mod); })`，
// 且 `cgTasks` 在 `:443-450` 是**跨层摊平**的（不止同层）。
// 而本仓既有设计原则明文写着「**任务线程零共享写，无锁**」（`main.cpp:277-281`：
// 「每模块独立 SemAnalyzer + 独立 DiagnosticEngine（任务线程零共享写，无锁）」；
// 「runSemaModule 返回 unique_ptr：moduleSemas 的 map 写入只在主线程进行（修复并行 data race）」）。
//
// **原设计之错**：全局共享一个 `MetaCollector` + `setCurrentModule` 上下文 ⇒
//   ① STL 容器（vector/unordered_map）并发写 = **UB**；
//   ② `setCurrentModule` 交错 ⇒ **记录归错模块**；
//   ③ `seqCounter_[key]++` 竞争丢更新 ⇒ 同模块 `seqInModule` 重号 ⇒ **thunk 重名**。
//   ⚠️ §3.2.1 原只论证了「per-module 计数器 ⇒ 并行下序号**逻辑**确定」——
//      那是**逻辑确定性，不是线程安全**（三审指出的认知盲区）。
//
// **修订**：拆成两个类 ——
//   ① `MetaCollector`：**per-module 本地实例**（在 `runCgModule` 内构造 ⇒ **线程私有，零共享，无需锁**）
//   ② `MetaMerger`：**主线程单线程**按 `orderedModules` 序合并（与既有 merge 范式完全同构）
// ============================================================
class MetaCollector {
public:
    // 🔴 O1：模块身份**随构造传入** —— 上下文随实例天然隔离 ⇒ **不再需要** setCurrentModule
    //   （单文件模式同样用它：`MetaCollector(opts.inputPath, Aura::ModuleManager::sanitizeId(moduleName))` ⇒ O7 一并消解）
    MetaCollector(std::string moduleKey, std::string nsStem)
        : moduleKey_(std::move(moduleKey)), nsStem_(std::move(nsStem)) {}

    // ---- 收集（**本模块内串行**调用 ⇒ 无锁安全）----
    // `thunkName` 在这里**一次定型**（N2 单源化）："_aura_mat_" + nsStem_ + "_" + seqSymbol_
    // 🔴 feature-18 P4a 批 1（A3）：返回值改为**本模块内的 seqInModule**（= 分配后写入 `rec.seqInModule`
    //   的同一个值）。依据 change.md §3.1.3（A-2 传法取 (c)）：「`collectSymbol` 返回分配值（一行改动），
    //   调用侧（A 遍钩子 `CodeGen.cpp:699/724`）存进 `CodeGenerator::frameSeqOf_`」。
    // ⚠️ 调用侧**必须**用本返回值填 `frameSeqOf_`，**不得**读调用方自己那份 `rec`（按值传入 ⇒
    //   调用方持有的是**另一份**：`seqInModule` 仍是默认 0，且 `name` 已被 `std::move` 掏空）。
    // 返回值 = 分配前的计数器值（与写入 rec.seqInModule 的值恒等）。
    uint32_t collectSymbol(MetaSymbolRec rec);   // ⚠️ **按值收**（内部补 moduleKey/seqInModule/thunkName）
    void collectType  (MetaTypeRec   rec);   // 🔴 O8：类型也走本模块计数（seqType_）

    // ---- 🔵 feature-18 P4b-1 B5：匿名帧收集（**本模块内串行**，线程私有 ⇒ 无锁）----
    //   返回值 = 本次分配的**本模块匿名帧序号**（`seqAnon_` 自增前的值），供注入侧生成
    //   `anonFrameIndexAt(moduleIdx, <seq>)` 的第二个实参（编译期常量）。
    //   ⚠️ 序号**单调不回退**（同 `seqSymbol_`：回退并复用 ⇒ 与本模块已发射的匿名帧重号）。
    uint32_t collectAnonFrame(MetaAnonFrameRec rec);

    // ============================================================
    // 🔴 O41-(g) 修订（批 3，必炸点修复）：**后置剪枝（prune）**
    //
    // **问题**：`materialize` 的 thunk 需要签名 `FuncSemType*`，而 Sema **不**把「形参 TypeExpr」
    //   的解析结果写回 `TypeExpr::inferredType`（全仓只有**返回类型**写回：
    //   `BodyChecker.cpp:144`/`:225`、`ExprInferMisc.cpp:313`）⇒ CodeGen 侧对**非** `int/float/bool`
    //   的形参类型**拿不到 SemType** ⇒ 该符号的 thunk 被**跳过**；但收集侧（A 遍 `genDecl`）
    //   **已经**把表项登记了（`materialize = &_aura_mat_x_y`）⇒ **表项指向不存在的符号** ⇒
    //   链接期 `undefined reference`（单文件同样炸，多文件一次性炸）。
    //
    // **裁定（主 Agent）**：**不做**「两处同一判定函数」（两侧输入可得性不同 ⇒ 必然再次分叉；
    //   本档已吃过 4 次同族亏：O2 / O24 / O40-(a) / O41-(a)）。改为**由实际产物驱动**：
    //   thunk 生成循环把**实际生成了的** thunk 名收进集合，循环之后调用本函数过滤 `symbols_`。
    //   ⇒ **不变量：`symbols_` 里每条记录的 `thunkName`，本 TU 内都有真实定义。**
    //   ⚠️ **feature-18 P4a 批 1（A1）之后本不变量已放宽**：`isFrame == true` 的记录**降级不删**
    //      （`thunkName` 置空 + 物化列渲染 `nullptr`）⇒ 现不变量 = 「有 `thunkName` ⇒ 本 TU 内有定义；
    //      无 `thunkName` ⇒ 表项渲染 `nullptr`」。见 `MetaCollect.cpp` 的 `pruneUnmaterialized` 注释与简报 A1b。
    //
    // ⚠️ **必须是本模块内、`finalize()` 之前**的调用（全局 index 尚未分配 ⇒ 不影响索引确定性）；
    //    本函数**只动本实例**（per-module 线程私有 —— 红线：不得引入跨模块共享）。
    // ============================================================
    void pruneUnmaterialized(const std::set<std::string>& emittedThunkNames);

    // ---- 本模块只读视图（随 CgResult 带回主线程）----
    const std::vector<MetaSymbolRec>& symbols() const { return symbols_; }
    const std::vector<MetaTypeRec>&   types()   const { return types_; }
    const std::vector<MetaAnonFrameRec>& anonFrames() const { return anonFrames_; }
    const std::string& moduleKey() const { return moduleKey_; }
    const std::string& nsStem()    const { return nsStem_; }

private:
    std::string moduleKey_;   // 拓扑序稳定键（= `mod->sourcePath`）
    std::string nsStem_;      // `sanitizeId(stem)`（`ModuleManager.cpp:90-94`）⇒ 供 thunk 命名
    std::vector<MetaSymbolRec> symbols_;   // **本模块**记录（模块内声明序）
    std::vector<MetaTypeRec>   types_;
    std::vector<MetaAnonFrameRec> anonFrames_;   // 🔵 P4b-1 B5：本模块匿名帧（注入序）
    uint32_t seqSymbol_ = 0;  // per-module 计数器（**线程私有 ⇒ 无需原子/锁**）
    uint32_t seqType_   = 0;
    uint32_t seqAnon_   = 0;  // 🔵 P4b-1 B5：匿名帧 per-module 计数器（**只增不回退**）
    friend class MetaMerger;
};

// ============================================================
// MetaMerger —— 🔴 O1：**主线程单线程**使用的合并器（红线②「汇总段统一分配」的落点）
//   与既有 `runSemaModule → unique_ptr → 主线程落 map` 完全同构；**无需任何锁**。
//   ⚠️ 调用顺序即索引顺序：主线程按 `orderedModules`（层序 × 同层字典序）依次 addModule ⇒
//      **合并序天然就是全局 index 序**（无需二次排序、无 O4 的临时 index 问题）。
// ============================================================
class MetaMerger {
public:
    static constexpr uint32_t kUnassigned = 0xFFFFFFFFu;

    // 主线程**按 orderedModules 序**逐个调用（顺序确定 ⇒ 索引确定，红线②）
    void addModule(const MetaCollector& mc);

    // 合并完成后一次性写回全局 index / 建「限定名 → index」映射（单线程，无竞争）
    void finalize();

    // ---- 产物生成所需的只读视图（主线程）----
    const std::vector<MetaSymbolRec>& allSymbols() const { return symbols_; }
    const std::vector<MetaTypeRec>&   allTypes()   const { return types_; }
    // 🔵 feature-18 P4b-1 B5（裁定⑧「两级帧区」）：**全局匿名帧序**（= addModule 调用序）
    //   与**每模块在匿名帧区内的起始下标**（前缀和）。消费者：`MetaEmit::renderTables`
    //   渲染 `kFrameTable[]` 的 `[kSymbolCount, kFrameCount)` 尾段 + `kAnonFrameBase[]` /
    //   `kAnonFrameCount`。
    //   ⚠️ 与 `moduleBase()` 同款铁律：取值必须在 `addModule` 内**追加该模块的匿名帧之前**。
    const std::vector<MetaAnonFrameRec>& allAnonFrames() const { return anonFrames_; }
    const std::vector<uint32_t>&         anonBase()      const { return anonBase_; }
    // 🔴 feature-18 P4a 批 1（A4）：每模块在全局符号表中的**起始下标**（= 前缀和）。
    //   与 `addModule` 的调用序一一对应（第 i 项 = 模块 i 的首个符号的全局 index）。
    //   消费者：`MetaEmit::renderTables` 渲染 `kModuleBase[]` / `kModuleCount`。
    const std::vector<uint32_t>& moduleBase() const { return moduleBase_; }
    // 🔴 O36 修订（终审直核）：以下三个接口 + 私有 `moduleKeys_`/`moduleIdx_` map **本档流程零消费者** ⇒ **删除**。
    //   ⚠️ 原注释声称「moduleRecords 供 emitMetaImpl 按模块列清单」—— **不成立**：
    //      emit 侧用 `allSymbols()` 逐条取 `nsName` + `thunkName` 即可渲染（同 nsName 的连续块天然成组，
    //      且**不需要**预先分好组）；引入 moduleRecords 反而多一套索引要维护。
    //   ⚠️ 裁定：**删除**（**不是**「P4/P5 预留」—— AGENTS.md 禁止无依据的死代码；P4 真要用时再加）。
    //      保留的消费者只有：收集侧 `MetaCollector::symbols()/types()`、emit 侧 `allSymbols()/allTypes()`。

private:
    std::vector<MetaSymbolRec> symbols_;                       // **全局序**（= addModule 调用序）
    std::vector<MetaTypeRec>   types_;                         // （🔴 O36：`moduleKeys_` / `moduleIdx_` 已删 —— 零消费者）
    // 🔴 feature-18 P4a 批 1（A4）：每模块起始下标表（与 `symbols_` 同序）。
    //   ⚠️ 取值必须在 `addModule` 内 **追加该模块记录之前** 取 `symbols_.size()`
    //      —— **绝不能**用 `nextSymbolIndex_`（它只在 `finalize()` 里被写，`addModule` 时恒为 0）。
    std::vector<uint32_t> moduleBase_;
    // 🔵 feature-18 P4b-1 B5：匿名帧区（与 `symbols_` / `moduleBase_` 平行的一套）
    std::vector<MetaAnonFrameRec> anonFrames_;   // **全局匿名帧序**（= addModule 调用序）
    std::vector<uint32_t>         anonBase_;     // 每模块在**匿名帧区**内的起始下标
    std::unordered_map<std::string, uint32_t> symbolIndexOf_;
    uint32_t nextSymbolIndex_ = 0;
    uint32_t nextTypeIndex_   = 0;
};

} // namespace Aura
