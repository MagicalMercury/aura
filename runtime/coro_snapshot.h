#pragma once
// ============================================================
// aura_rt/coro_snapshot.h — feature-18 P4b-1：协程逻辑栈快照（side table）
//
// 设计依据（change.md）：
//   · §3.3.1  正确语义模型（链切换）：挂起 = 移出（快照）→ 恢复 = **覆盖换回**（禁 push）
//   · §3.3.1b `resumeWithRestoreScoped`（同步唤醒路径：存 → 换入 → resume → 覆盖换回自己）
//   · §3.3.2  per-coroutine 存储 = side table `unordered_map<void*, Snapshot>` + **轻量 mutex**
//             （N6：**必须**，不是「应够」—— 跨线程 spawn 下 entry 写入线程 ≠ erase 线程）
//   · §3.3.4  `baseDepth` **随 Snapshot 传递**（裁定⑩：**不建** `g_loopBaseDepth`）
//   · §3.3.5  C-2 红线：`noexcept` awaiter 内插快照 ⇒ **必须 nothrow**（纯 memcpy 无分配；
//             表写入在 try/catch 内 ⇒ 分配失败一律「放弃」，**绝不抛**）
//   · §3.3.6  截断规则**统一为一条**：拷 `[max(0, depth-64), depth)`（丢最外、保最近）
//             + `baseDepth` 相对新起点重定位
//
// ⚠️ **R1 红线：本批不碰 `runtime/logical_stack.h`**（md5 恒为 050b6a5109619bfaceb5aec36899cd55）
//    ⇒ 本头是**独立头文件**（header-only、零新增源文件、零 CMake 改动），
//      只 **#include** `logical_stack.h` 以读 `g_lsFrames` / `g_lsDepth` /
//      `kMaxSnapshotFrames` / `Frame`（**只读**，不向其新增任何符号）。
// ⚠️ **TU 归属**（change.md §9-V14）：本头**只有 inline 函数 + inline 变量**，
//    表本体用**函数局部 static**（Meyers）承载 —— 与 `logical_stack.h:100-114` 的
//    `errStackConfig()` 同款先例；**不是** TLS、**不需**动态初始化的 TLS
//    （⇒ 不发射 `__tls_init`，不触发 bug-95 的 MinGW 多 TU 冲突）。
//    ⚠️ `g_coroBaseDepth` 是**零初始化 POD 的 TLS**（与 `logical_stack.h:52` 的
//    `g_lsDepth` 同款）⇒ 同样不发射 `__tls_init`。
// ============================================================
#include <coroutine>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "logical_stack.h"

namespace aura_rt {

// ------------------------------------------------------------
// 快照（512B + 8B；容量 = `kMaxSnapshotFrames` 64 —— 与解构上限 `kMaxStackFrames` 32 无关）
// ------------------------------------------------------------
struct Snapshot {
    Frame    frames[kMaxSnapshotFrames];
    uint32_t depth     = 0;
    uint32_t baseDepth = 0;
};

// ------------------------------------------------------------
// 当前活跃逻辑链的**根深度**（TLS；零初始化 POD ⇒ 无 __tls_init）
//   §3.3.4 裁定⑩：baseDepth **随 Snapshot 传递**（不建 `g_loopBaseDepth`）。
//   本变量就是「传递」的载体：恢复时从快照写入、挂起时由快照读取。
// ------------------------------------------------------------
inline thread_local uint32_t g_coroBaseDepth = 0;

// ------------------------------------------------------------
// side table（键 = `coroutine_handle::address()`）+ 轻量 mutex
//   函数局部 static（Meyers）⇒ 唯一实体、首次使用时线程安全初始化、退出时析构（不泄漏）。
// ------------------------------------------------------------
inline std::unordered_map<void*, Snapshot>& coroSnapshotTable() {
    static std::unordered_map<void*, Snapshot> table;
    return table;
}

inline std::mutex& coroSnapshotMutex() {
    static std::mutex m;
    return m;
}

// ------------------------------------------------------------
// 内部：把「当前 TLS 链」按 §3.3.6 统一截断规则拷进 `dst`
//   · 拷贝区间 = [max(0, depth-64), depth)（丢最外、保最近）
//   · depth ≤ 64 ⇒ 等价 [0, depth)
//   · `baseSrc` 是**未截断坐标**下的链根深度 ⇒ 必须减 `from`（否则指向被丢弃区间）；
//     `baseSrc ≤ from` ⇒ 钳为 0
//   · 纯 memcpy（≤512B）+ 标量赋值 ⇒ 无分配、天然 nothrow
// ------------------------------------------------------------
inline void captureCurrentChain(Snapshot& dst, uint32_t baseSrc) noexcept {
    const uint32_t depth = g_lsDepth;
    const uint32_t n     = depth < kMaxSnapshotFrames ? depth : kMaxSnapshotFrames;
    const uint32_t from  = depth - n;                 // == max(0, depth - 64)
    if (n) std::memcpy(dst.frames, &g_lsFrames[from], static_cast<std::size_t>(n) * sizeof(Frame));
    dst.depth     = n;
    dst.baseDepth = (baseSrc > from) ? (baseSrc - from) : 0;   // 相对新起点重定位
}

// ------------------------------------------------------------
// 内部：**覆盖**换入 `src`（§3.3.1 红线：恢复必须是覆盖，**不得 push 回去**）
//   ⚠️ 覆盖式（非追加式）与「尚未析构的 FrameGuard」天然**幂等**：
//      写回的数据 == 挂起时的数据 ⇒ guard 的 `pushed_` 不变，后续析构逐帧 pop 照常配平。
// ------------------------------------------------------------
inline void overwriteChainWith(const Snapshot& src) noexcept {
    const uint32_t n = src.depth;
    if (n) std::memcpy(g_lsFrames, src.frames, static_cast<std::size_t>(n) * sizeof(Frame));
    g_lsDepth       = n;                 // 全量覆盖：**不是** min(depth,64) 之外的魔数
    g_coroBaseDepth = src.baseDepth;     // §3.3.6「恢复」行：baseDepth 一并换回
}

// ------------------------------------------------------------
// snapshotStack —— **挂起点**快照（挂起 = 移出）
//   baseDepth 语义（§3.3.4）：链的第一次快照时固定，之后每次挂起/恢复**复制传递**
//     · 本 key **已有存档**（尚未恢复）⇒ 沿用存档的 baseDepth（链根已固定）
//     · 无存档（已恢复/首次）    ⇒ 沿用**当前活跃链**的根深度（`g_coroBaseDepth`）
//   🔴 C-2/R4 红线：`noexcept` —— 容量/分配失败一律**放弃**（不写表），**绝不抛**。
// ------------------------------------------------------------
inline void snapshotStack(void* key) noexcept {
    if (!key) return;
    try {
        std::lock_guard<std::mutex> lk(coroSnapshotMutex());
        auto& tbl = coroSnapshotTable();
        auto  it  = tbl.find(key);
        const uint32_t baseSrc = (it != tbl.end()) ? it->second.baseDepth : g_coroBaseDepth;
        Snapshot snap;
        captureCurrentChain(snap, baseSrc);
        if (it != tbl.end()) it->second = snap;
        else                 tbl.emplace(key, snap);
    } catch (...) {
        // R4：**绝不抛**（noexcept）。分配失败 ⇒ 放弃本次快照。
    }
}

// ------------------------------------------------------------
// snapshotStackAtCreation —— **创建点**快照（§3.3.4 + 🟡-1 / change.md §8.2 B0a）
//   · 创建点在 **caller 线程** ⇒ 初始快照含 **caller 整链**（含 baseDepth 之下的外层
//     caller 帧；即「不截断到 baseDepth」——**不是**「无视 64 容量」）
//   · `baseDepth = 当前 depth`（§8.2 B0a 原文；新链此刻尚无自有帧 ⇒ 全部既有帧都是外层）
//   · 跨线程 spawn：worker 首启（P4b-2）用它把整链覆盖进 worker 的 TLS
// ------------------------------------------------------------
inline void snapshotStackAtCreation(void* key) noexcept {
    if (!key) return;
    try {
        std::lock_guard<std::mutex> lk(coroSnapshotMutex());
        auto& tbl = coroSnapshotTable();
        Snapshot snap;
        captureCurrentChain(snap, g_lsDepth);   // baseDepth = 当前 depth
        auto it = tbl.find(key);
        if (it != tbl.end()) it->second = snap;
        else                 tbl.emplace(key, snap);
    } catch (...) {
        // R4：绝不抛
    }
}

// ------------------------------------------------------------
// eraseSnapshotFor —— 清理钩子（`~task_promise_base()` 消费）
//   §3.3.2 / §9-N5：`~task_promise_base()` 是「**能析构时**的清理钩子」，
//   **不覆盖** detach 路径（`task.h` 的 run_to_completion 故意泄漏帧 ⇒ promise 永不析构
//   ⇒ entry 与已泄漏帧**同生命周期**，一致性无害，见 §9-V16）。
//   ⚠️ 链上其它承诺（不使用本表的协程）不受影响（键按 address 精确匹配）。
// ------------------------------------------------------------
inline void eraseSnapshotFor(void* key) noexcept {
    if (!key) return;
    try {
        std::lock_guard<std::mutex> lk(coroSnapshotMutex());
        coroSnapshotTable().erase(key);
    } catch (...) {
        // 析构函数隐式 noexcept ⇒ 绝不抛
    }
}

// ------------------------------------------------------------
// restoreStack —— **恢复 = 覆盖换回**（§3.3.2「恢复方读、恢复时 erase」）
//   · 有 entry ⇒ 覆盖 TLS（`g_lsDepth` / `g_coroBaseDepth` 一并换）+ **erase entry**
//   · 无 entry ⇒ **no-op**（§9-N10 裁定：Release 取 (a) 不 restore ——
//     此刻 TLS = caller 链（真正在执行且无存档），清空它会抹掉从未挂起的活跃链）
//   🔴 R3 红线：纯 memcpy 全量覆盖，**禁止任何「push 回去」的实现**
// ------------------------------------------------------------
inline void restoreStack(void* key) noexcept {
    if (!key) return;
    Snapshot snap;
    bool found = false;
    try {
        std::lock_guard<std::mutex> lk(coroSnapshotMutex());
        auto& tbl = coroSnapshotTable();
        auto  it  = tbl.find(key);
        if (it != tbl.end()) { snap = it->second; tbl.erase(it); found = true; }
    } catch (...) {
        return;   // 绝不抛 ⇒ 放弃恢复（等价 no-op）
    }
    if (found) overwriteChainWith(snap);
}

// ------------------------------------------------------------
// resumeWithRestore —— **真调度边界**用（change.md §3.3.1b 分派表）
//   `task.h:99`（final 短链，幂等）/ `task.cpp:116`（processReady）/ `task.h:305`（worker 首启）
// ------------------------------------------------------------
inline void resumeWithRestore(std::coroutine_handle<> h) {
    if (!h) return;
    restoreStack(h.address());
    h.resume();
}

// ------------------------------------------------------------
// resumeWithRestoreScoped —— **同步唤醒路径**用（§3.3.1b：`channel.h` 的 `try_flush`
//   跑在 `await_ready` / `await_resume` / `close` 的**同步路径**上，不在调度边界）
//   存当前活跃链 → 换入对方 → `h.resume()` → **覆盖换回自己**（行为零变化、递归安全）
//   ⚠️ 若 resume 抛出（`await_resume` 抛 Error ⇒ 其实会被 unhandled_exception 吃掉；
//      此处仅作严格兜底）⇒ 先换回自己再重抛，绝不把 TLS 留在别条链上。
//   ⚠️ 局部缓冲 ≈ 520B/层（§9-V17 已登记：唤醒风暴下栈压力上升，是既有行为的放大系数）
// ------------------------------------------------------------
inline void resumeWithRestoreScoped(std::coroutine_handle<> h) {
    if (!h) return;
    Snapshot own;
    captureCurrentChain(own, g_coroBaseDepth);   // ① 存当前活跃链（含自己的 baseDepth）
    restoreStack(h.address());                   // ② 换入对方（无 entry ⇒ no-op，N10(a)）
    try {
        h.resume();                              // ③
    } catch (...) {
        overwriteChainWith(own);                 // ④' 兜底换回
        throw;
    }
    overwriteChainWith(own);                     // ④ 覆盖换回自己
}

} // namespace aura_rt
