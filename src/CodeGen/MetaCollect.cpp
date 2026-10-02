// src/CodeGen/MetaCollect.cpp —— feature-18 P3
//
// 🔴 O1：职责拆分（架构级，三审双盲收敛）
//   ① MetaCollector = **per-module 本地实例** —— 在其所属模块的 CodeGen 线程内收集，
//      线程私有、零共享 ⇒ **无需任何锁**（类内**绝无** static/全局可变状态）。
//   ② MetaMerger   = **主线程单线程**合并器 —— 汇总段统一分配全局索引（红线②）。
#include "MetaCollect.h"

#include <string>
#include <utility>

namespace Aura {

// ============================================================
// MetaCollector —— 本模块收集（调用线程 = 本模块的 CodeGen 线程）
// ============================================================

uint32_t MetaCollector::collectSymbol(MetaSymbolRec rec) {
    // 🔴 N2 单源化：thunkName 在此**一次定型**（"_aura_mat_" + nsStem_ + "_" + seqSymbol_），
    //    此后 thunk 定义侧（CodeGen）与 emit 侧**只读**；不再有 finalizeThunkNames 第二步。
    //    seqSymbol_ 是 per-module 计数器（线程私有 ⇒ 并行安全，无竞争丢更新）。
    // 🔴 feature-18 P4a 批 1（A3）：返回本次分配的 `seqInModule`（change.md §3.1.3 的 (c) 取法）。
    //    ⚠️ 调用侧不得依赖自己那份按值传入的 `rec`（那是另一份副本：seq 仍为 0、name 已被 move）。
    rec.moduleKey   = moduleKey_;
    rec.seqInModule = seqSymbol_;
    rec.thunkName   = "_aura_mat_" + nsStem_ + "_" + std::to_string(seqSymbol_);
    const uint32_t assignedSeq = seqSymbol_;   // = 写入 rec.seqInModule 的同一个值
    ++seqSymbol_;
    symbols_.push_back(std::move(rec));
    return assignedSeq;
}

void MetaCollector::collectType(MetaTypeRec rec) {
    // 🔴 O8：类型也走本模块计数（seqType_）。
    // ⚠️ 实测差异（见实施批回报 §6）：change.md §3.2 的 `MetaTypeRec` **没有**「本模块序号」
    //    字段（只有全局 `index`）⇒ 本模块次序号**无落点可写**；类型在表内的次序由 `types_`
    //    的追加次序隐含（与 `MetaMerger` 的 allTypes 次序 = addModule 序一致）。
    ++seqType_;
    types_.push_back(std::move(rec));
}

// ============================================================
// 🔵 feature-18 P4b-1 B5：匿名帧收集（change.md 裁定⑧ / §3.1.2 缺口 C / §8.2 B0b）
//   · 与 `collectSymbol` **完全同款的 per-module 计数器纪律**（线程私有 ⇒ 无锁；
//     序号单调、不回退 ⇒ 本模块已发射的匿名帧不会重号）。
//   · **不**写 `thunkName`（匿名帧**无物化配方**：帧表尾段的 `FrameDesc` 只有
//     name/file/defLine ⇒ 天然无 `materialize` 列，不存在 A1b 那种「空名 ⇒ 坏 C++」问题）。
//   · **不**参与 `pruneUnmaterialized`（匿名帧没有 thunk 可被剪；它也不占符号序号 ⇒
//     与「恒等式」无交互 —— 这正是把匿名帧放进**独立区**的收益）。
// ============================================================
uint32_t MetaCollector::collectAnonFrame(MetaAnonFrameRec rec) {
    const uint32_t assigned = seqAnon_;
    ++seqAnon_;
    anonFrames_.push_back(std::move(rec));
    return assigned;
}

void MetaCollector::pruneUnmaterialized(const std::set<std::string>& emittedThunkNames) {
    // 🔴 O41-(g)（批 3）：后置剪枝 —— 保留「本模块**实际生成了 thunk**」的记录。
    //   · 判据是**实际产物**（`CodeGen.cpp` 的 thunk 生成循环写进集合的名字），
    //     而非「再算一遍可物化判定」⇒ **不可能**与 thunk 侧分叉。
    //   · 调用点是 `generate()` 内、thunk 循环之后、`finalize()` 之前（全局 index 未分配）。
    //   · 只动本实例（per-module 线程私有）。
    //
    // ============================================================
    // 🔴 feature-18 P4a 批 1（A1，change.md §3.1.2 修法A / §3.1.5）：**`isFrame` 记录「降级不删」**
    //
    // ① **为什么必须降级不删**：本函数原为**删除**（`symbols_.swap(kept)` 只留命中的），
    //    而 `seqSymbol_` **不回退、不复用**（见下方原注释）⇒ 删一条就留一个**序号空洞**。
    //    但 P4a 的帧编号走运行时换算 `symbolIdx = kModuleBase[moduleIdx] + seqInModule`
    //    （编译期拿不到全局 index，见 change.md §3.1.1），而该换算**恒等于** `finalize()` 对
    //    **剪后** `symbols_` 连续分配的全局 index 的**前提**正是「seq 无洞」。
    //    时序：A 遍 collectSymbol 分配 seq → B 遍注入 FrameGuard（用**剪前** seq）
    //          → thunk 循环 → **本函数**（剪）→ addModule（剪后集合）→ finalize（连续分配 index）。
    //    ⇒ 剪 1 条，其后所有符号错位 1，且**错得静默**（栈里显示成另一个函数名，不崩不报）；
    //      被剪函数本身还会「有帧无表项」（symbolIdx 悬空）。
    //    ⇒ 故对 `isFrame == true` 的记录：**保留记录与 `seqInModule`**，仅把物化列**降级为占位**
    //      （`thunkName` 置空 ⇒ emit 侧渲染裸 `nullptr`，见 A1b；记录上**没有** `materialize` 字段，
    //        该列是 emit 时按 `thunkName` 合成的）⇒ **与 O12 的「非帧项占位」完全同构**，序号无洞。
    //
    // ② **本函数当前实际「恒空转」（等效禁用）—— 这是有意为之**：当前收集面**全部 `isFrame = true`**
    //    （`CodeGen.cpp:714`（函数）/`:735`（方法）），且收集面 = `FunDecl`（**含 main**，A2 起）
    //    + `MethodDecl`（非构造器）⇒ 下面「丢弃」分支**永不命中** ⇒ 本函数对**任何**记录都不删。
    //    这是**有意为之**：O41-(g) 原先「删记录防悬空」的使命，已由 **A1b 的空名渲染**
    //    （`thunkName` 空 ⇒ 表项渲染 `nullptr`、且不发 thunk 前置声明）**承接** —— 靠「渲染层判空」
    //    而非「收集层删记录」来保证产物合法（产物不悬空 + 编号不错位，两者兼得）。
    //    ⚠️ **隐含前提（红线）**：恒等式成立**依赖「收集面全部记录 `isFrame = true`」**。
    //       若将来收集面扩展出 `isFrame == false` 的记录（例如按 change.md 缺口 D 收集构造器时
    //       忘了置 `isFrame`）⇒ 本函数**会删它** ⇒ **序号洞回归**、误差静默。
    // ============================================================
    std::vector<MetaSymbolRec> kept;
    kept.reserve(symbols_.size());
    for (MetaSymbolRec& rec : symbols_) {
        if (emittedThunkNames.count(rec.thunkName)) {
            kept.push_back(std::move(rec));
        } else if (rec.isFrame) {
            // A1：**降级不删** —— 保留 name/file/defLine/kind/flags/`seqInModule`，只清 `thunkName`
            //   （物化列 ⇒ emit 渲染 `nullptr`）。清空必须在 `std::move` **之前**。
            rec.thunkName.clear();
            kept.push_back(std::move(rec));
        }
        // else：非帧记录仍按 O41-(g) 原语义**丢弃**（当前收集面零此类记录 ⇒ 恒不命中）。
    }
    symbols_.swap(kept);
    // ⚠️ **`seqSymbol_` 不回退、不复用**：模块内序号保持单调（残留的序号空洞无害；
    //    若回退并复用已发射过的序号 ⇒ thunk **重名** ⇒ 重定义）。
    //    ⚠️ A1 之后本函数的**删除**语义已退化为空转（见上②）⇒ 序号**必然**无洞
    //       （原「空洞」与本函数同源，现只剩本函数可制造 ⇒ 已封）。
}

// ============================================================
// MetaMerger —— 主线程按 orderedModules 序合并（合并序 ⇒ 全局 index 序，红线②）
// ============================================================

void MetaMerger::addModule(const MetaCollector& mc) {
    // 按调用序把该模块记录追加进全局序（主线程单线程 ⇒ 无锁）。
    // 🔴 feature-18 P4a 批 1（A4，change.md §3.1.4）：本模块的**起始下标**必须在 append **之前**取。
    //   ⚠️ **绝不能用 `nextSymbolIndex_`** —— 它只在 `finalize()` 里被写（见本文件 `finalize()`），
    //      在 `addModule` 时**恒为初始值 0** ⇒ `kModuleBase` 会全 0（GLM 🟡-4 的原始错法）。
    //   `symbols_.size()` 就是「已合并的符号数」= 本模块首个符号的全局 index（因为 addModule 的
    //   调用序**就是** `finalize()` 的 index 分配序 ⇒ 前缀和恒等式）。
    moduleBase_.push_back(static_cast<uint32_t>(symbols_.size()));
    for (const MetaSymbolRec& rec : mc.symbols()) symbols_.push_back(rec);
    for (const MetaTypeRec&   rec : mc.types())   types_.push_back(rec);
    // 🔵 feature-18 P4b-1 B5（裁定⑧）：匿名帧区 —— 与上方 `moduleBase_` **同款铁律**：
    //   本模块在匿名帧区内的起始下标必须在 append **之前**取（`anonFrames_.size()`），
    //   否则前缀和错位、注入的 `anonFrameIndexAt` 会指向别条匿名帧（静默错 trace）。
    anonBase_.push_back(static_cast<uint32_t>(anonFrames_.size()));
    for (const MetaAnonFrameRec& rec : mc.anonFrames()) anonFrames_.push_back(rec);
}

void MetaMerger::finalize() {
    // 合并完成后一次性写回全局 index + 建「限定名 → index」映射（单线程，无竞争）。
    // 幂等：重复调用结果一致（同一输入连续生成两次 ⇒ 表文本逐字节一致，§5 验证步 6）。
    symbolIndexOf_.clear();
    nextSymbolIndex_ = 0;
    nextTypeIndex_   = 0;
    for (MetaSymbolRec& rec : symbols_) {
        rec.index = nextSymbolIndex_++;
        symbolIndexOf_[rec.name] = rec.index;
    }
    for (MetaTypeRec& rec : types_) {
        rec.index = nextTypeIndex_++;
    }
}

} // namespace Aura
