#pragma once
// ============================================================
// aura_rt/logical_stack.h — feature-18：逻辑调用栈（thread_local）+ 档位 + 热路径降级
//
// 设计依据：
//   plan §4.3（逻辑栈指针机制，D1 帧行号语义 = 本帧当前执行行）
//   plan §4.1/§4.1.1（AURA_ERR_STACK 档位 + 按抛出点计数的热路径降级，E3 阈值 256）
//   探针报告 scripts/_f18/{P0_probe1_report.md, P0_probe3_report.md}
//
// ⚠️ 线程局部变量用 inline thread_local（对齐既有 runtime/builtin/sync_context.h:170
//    的 g_syncStack 形态）⇒ header-only、零新增源文件、零 CMake 改动。
//    若 MinGW 上出现 TLS init 冲突，**只能**改成「零初始化的 TLS 指针 + 惰性创建」
//    （同 sync_context.h 的 g_syncStackPtr / syncStack()，bug-95 修复，实测有效）；
//    完整根因链与禁用方案见 issues/bugs/bug-95-tls-dynamic-init-gsyncstack-multiple-definition-on-windows.md。
//    ⚠️ 原「回退方案见 change.md §6.2」**已失效**（该章节随 change.md 多轮覆盖而消失，属悬空引用，2026-10-01 修正）。
//    ⚠️ 本头的 g_lsFrames / g_lsDepth / g_throwCounts 都是**零初始化 POD** ⇒ 不发射 __tls_init，无此问题。
//
// ⚠️ 本头文件**不得** include 任何 runtime 重量头（types.h / array.h / gc.h）——
//    它是被 types.h 与生成代码共同引用的低层设施；只允许 <cstdint>/<cstddef>/<cstdlib>/<cstring>。
// ============================================================
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace aura_rt {

// Array<T> 前向声明（captureLogicalStack 返回类型；实现见 types.cpp）
template <typename T> struct Array;

// ------------------------------------------------------------
// 容量常量
// ------------------------------------------------------------
inline constexpr uint32_t kMaxLogicalDepth   = 256;   // 逻辑栈容量（溢出策略：丢弃更深帧，不覆盖）
inline constexpr uint32_t kMaxStackFrames    = 32;    // 解构上限（主人裁定 32 帧）
inline constexpr uint32_t kMaxSnapshotFrames = 64;    // 协程快照容量（P4 使用；>= kMaxStackFrames）
inline constexpr uint32_t kMaxThrowSites     = 4096;  // 降级计数表容量

// 特殊 throwSite
inline constexpr uint32_t kThrowSiteUnknown = 0xFFFFFFFFu;  // 未标注：不参与降级（永远 L0）
inline constexpr uint32_t kThrowSiteNoStack = 0xFFFFFFFEu;  // 强制无栈：永不解构（OOM 路径专用）

// ------------------------------------------------------------
// 帧（8B）与逻辑栈本体
// ------------------------------------------------------------
struct Frame {
    uint32_t symbolIdx;   // 指向 FrameDesc[]（P3 落地；P1 阶段为调用方自定编号）
    uint32_t line;        // 本帧当前执行行（D1 语义）
};

inline thread_local Frame    g_lsFrames[kMaxLogicalDepth];
inline thread_local uint32_t g_lsDepth = 0;

// ⚠️ 返回 bool：true = 本次 push 成功（`FrameGuard` 据此决定是否 pop）
//    必须返回成功的判据 —— 否则「溢出时 push 为 no-op + 析构无条件 pop」会**错误扣减** depth：
//    G-3（GLM 审查 2026-09-28）：上限 256、递归 300 帧 ⇒ 帧 257..300 的 push 全部 no-op，
//    但它们析构时各 pop 一次 ⇒ depth 从 256 被扣到 212 ⇒ **递归进行中栈顶已错位** ⇒
//    此后采集到的栈错乱，且单测「结束后 depth 回 0」会虚假通过。
inline bool pushFrame(uint32_t symbolIdx, uint32_t line) noexcept {
    if (g_lsDepth >= kMaxLogicalDepth) return false;   // 容量满：丢弃更深帧（不覆盖既有帧）
    g_lsFrames[g_lsDepth].symbolIdx = symbolIdx;
    g_lsFrames[g_lsDepth].line      = line;
    ++g_lsDepth;
    return true;
}

inline void popFrame() noexcept {
    if (g_lsDepth) --g_lsDepth;
}

// 更新**栈顶帧**的行号（调用点 / 抛出点注入；D1：该帧内最近一次调用/抛出点）
inline void setFrameLine(uint32_t line) noexcept {
    if (g_lsDepth) g_lsFrames[g_lsDepth - 1].line = line;
}

// RAII：函数入口 push、出口（含异常路径）pop —— 探针 1 已验异常路径亦正确配平
//   ⚠️ G-3 修正：记录 pushed_，仅当真正 push 过才 pop（溢出帧不得参与 pop）
struct FrameGuard {
    bool pushed_;
    FrameGuard(uint32_t symbolIdx, uint32_t line) noexcept
        : pushed_(pushFrame(symbolIdx, line)) {}
    ~FrameGuard() noexcept { if (pushed_) popFrame(); }
    FrameGuard(const FrameGuard&)            = delete;
    FrameGuard& operator=(const FrameGuard&) = delete;
};

// ------------------------------------------------------------
// 档位：AURA_ERR_STACK
//   （未设置）            = on，threshold=256
//   on                    = 同上
//   on:threshold=N        = 自定义阈值（0 = 永不降级）
//   off                   = 全程无栈（capture=false）
//   nostack=Kind1,Kind2   = 按 kind 白名单首次即无栈（可选）
// ------------------------------------------------------------
struct ErrStackConfig {
    bool     capture   = true;       // off ⇒ false
    uint32_t threshold = 256;        // 降级阈值（0 ⇒ 永不降级）
};

inline const ErrStackConfig& errStackConfig() noexcept {
    static const ErrStackConfig cfg = []() noexcept -> ErrStackConfig {
        ErrStackConfig c;
        const char* env = std::getenv("AURA_ERR_STACK");
        if (!env || !*env) return c;                     // 未设置 ⇒ 默认 on/256
        if (std::strcmp(env, "off") == 0) { c.capture = false; return c; }   // G-7 小项：精确匹配（"offline" 不再被误判为 off）
        const char* th = std::strstr(env, "threshold=");
        if (th) {
            long v = std::strtol(th + 10, nullptr, 10);
            if (v >= 0 && v <= 65535) c.threshold = static_cast<uint32_t>(v);
        }
        return c;
    }();
    return cfg;
}

// 降级计数表（每线程独立；单向不回升）
inline thread_local uint16_t g_throwCounts[kMaxThrowSites];

// 判定是否采集栈（**有副作用：会自增该 site 的计数**）
//   语义：site 抛出次数 < threshold ⇒ 采栈（L0）；>= threshold ⇒ 不采（L1，零分配）
inline bool shouldCaptureStack(uint32_t throwSite) noexcept {
    if (throwSite == kThrowSiteNoStack) return false;
    const ErrStackConfig& c = errStackConfig();
    if (!c.capture) return false;
    if (throwSite == kThrowSiteUnknown || throwSite >= kMaxThrowSites) return true;  // 保守：不降级
    if (c.threshold == 0) return true;
    uint16_t& n = g_throwCounts[throwSite];
    // ⚠️ **F-D1 修正（2026-09-29，批 D1 全量单测暴露）**：必须「**饱和自增、再比较**」——
    //    原写法 `if (n < c.threshold) { ++n; return true; }` 等于「前 threshold 次都有栈」，
    //    与**探针 3 B4e 的实测语义**（threshold=64：`throw#63 counter=63 stack=有` /
    //    `throw#64 counter=64 stack=无`）**差 1**；也与 §4.1 的 `DegradeBoundary255Then256`
    //    用例（期望第 256 次无栈）矛盾 ⇒ 实现与测试二者必有一错，实测数据为权威。
    //    饱和自增（先判 `n < threshold` 再 ++）同时避免 threshold=65535 时 uint16_t 回绕 ⇒ 降级失效。
    if (n < c.threshold) ++n;
    return n < c.threshold;
}

// 单测辅助：清零计数表
inline void resetThrowCounters() noexcept {
    std::memset(g_throwCounts, 0, sizeof(g_throwCounts));
}

// 采集紧凑帧（E1）：返回 Array<uint64_t>*，每条 = (symbolIdx << 32) | line
//   上限 kMaxStackFrames；无帧 / 分配失败 ⇒ nullptr。**实现在 types.cpp**（需要 Array<T> 完整定义）
Array<uint64_t>* captureLogicalStack() noexcept;

} // namespace aura_rt
