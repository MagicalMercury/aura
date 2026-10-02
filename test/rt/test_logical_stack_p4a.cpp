// ============================================================
// test/rt/test_logical_stack_p4a.cpp — feature-18 P4a 批 4：逻辑栈运行期用例（T1–T3）
//
// 依据：change.md §6.1（T1/T2/T3） + §2-R4「倒退 G-3」红线（T2 的核心判据）。
// 被测实现：runtime/logical_stack.h（**本批不改** —— R1 红线）。
//
// ⚠️ 与既有 test/rt/test_logical_stack.cpp（feature-18 P1）的分工：
//   P1 已覆盖 push/pop 配平、setFrameLine 只改栈顶、溢出截断、Guard 异常路径。
//   本文件按 §6.1 的**用例名**重述 T1/T3，并补 P1 **未做**的两件事：
//     · T2：**真递归 300 帧**（R7 —— 不许 mock/循环摊平绕过），且断言「活帧对齐不变量」
//       （P1 的 `GuardOverflowKeepsDepthBalanced` 用的是 std::vector<unique_ptr<FrameGuard>>
//        的**摊平**形态，只能验「析构后归零」，**抓不到 G-3 的中途错位**）。
//     · T3：在**多帧**下逐帧核对，并在 pop 之后再次 setFrameLine（P1 只验 2 帧、无 pop）。
// ============================================================
#include "logical_stack.h"
#include "framework/test_framework.h"

using namespace aura_rt;

// ============================================================
// T2 的递归探针（文件级静态：避免栈上大对象；单测串行执行）
// ============================================================
namespace {

constexpr int kT2Depth = 300;                 // R7：必须真递归 300 帧

int  g_t2DepthAfterCall[kT2Depth + 1];        // 第 n 层：子层返回后、本层析构前观察到的 depth
bool g_t2Pushed[kT2Depth + 1];                // 第 n 层 FrameGuard 是否真正 push 过
int  g_t2MaxDepth = 0;

// 每层一个 FrameGuard（RAII），层层真递归到 300
int t2Recurse(int n) {
    FrameGuard g(static_cast<uint32_t>(n), static_cast<uint32_t>(n));
    g_t2Pushed[n] = g.pushed_;
    if (static_cast<int>(g_lsDepth) > g_t2MaxDepth)
        g_t2MaxDepth = static_cast<int>(g_lsDepth);
    int r = 0;
    if (n < kT2Depth) r = t2Recurse(n + 1);
    // 本层 guard 仍在作用域内 ⇒ 此处 depth 应等于「本层在活帧中的序号 + 1」
    g_t2DepthAfterCall[n] = static_cast<int>(g_lsDepth);
    return r;
}

} // namespace

// ============================================================
// T1 LogicalStack.FrameGuardPushesAndPops
//   构造/析构配对；结束时 g_lsDepth **回到原值**（含非零基线、含嵌套）
// ============================================================
TEST(LogicalStack, FrameGuardPushesAndPops) {
    g_lsDepth = 0;
    pushFrame(1u, 10u);
    pushFrame(2u, 20u);
    EXPECT_EQ(g_lsDepth, 2u);                       // 非零基线（不是「恰好为 0 才通过」的假判据）

    {
        FrameGuard g(7u, 70u);
        EXPECT_TRUE(g.pushed_);
        EXPECT_EQ(g_lsDepth, 3u);                   // 构造 ⇒ push
        {
            FrameGuard g2(8u, 80u);
            EXPECT_TRUE(g2.pushed_);
            EXPECT_EQ(g_lsDepth, 4u);               // 嵌套构造
        }
        EXPECT_EQ(g_lsDepth, 3u);                   // 内层析构 ⇒ pop
    }
    EXPECT_EQ(g_lsDepth, 2u);                       // 外层析构 ⇒ 回到**原值**（不是 0）
    EXPECT_EQ(g_lsFrames[1].symbolIdx, 2u);         // 基线帧未被破坏

    popFrame();
    popFrame();
    EXPECT_EQ(g_lsDepth, 0u);
}

// ============================================================
// T2 LogicalStack.OverflowKeepsDepthBalanced   🔴 G-3 红线（change.md §2-R4 / §6.1 T2）
//
//   真递归 300 帧（kMaxLogicalDepth = 256）：
//     · 第 0..255 层的 FrameGuard push 成功（pushed_ == true）；
//     · 第 256..300 层 push 失败（pushed_ == false）⇒ 析构时**不得 pop**；
//     · 终点 g_lsDepth 归零。
//
//   🔴 关键加强（P1 未覆盖）：**中途对齐不变量** ——
//     第 n 层（n < 256）在「子层已返回、本层仍活」时，depth 必须 == n + 1。
//     若 FrameGuard 退化为「无条件 pop」（即丢掉 pushed_ 三参机制），
//     则最深 45 个溢出层会在回升时各多 pop 一次 ⇒ 从第 255 层起 depth 逐个错位
//     （255 层读到 255 而非 256）⇒ 本断言**必定抓红**。
//     ⚠️ 而「结束后 depth == 0」这一条**抓不到**（popFrame 有下溢截断，见 logical_stack.h:67-69）
//        —— 这正是 G-3 里「单测虚假通过」的形态。
// ============================================================
TEST(LogicalStack, OverflowKeepsDepthBalanced) {
    g_lsDepth = 0;
    for (int i = 0; i <= kT2Depth; ++i) {
        g_t2DepthAfterCall[i] = -1;
        g_t2Pushed[i] = false;
    }
    g_t2MaxDepth = 0;

    (void)t2Recurse(0);                             // 真递归 300 帧（R7）

    // ① 确实抵达容量上限（证明溢出真的发生，而非递归被优化掉）
    EXPECT_EQ(g_t2MaxDepth, static_cast<int>(kMaxLogicalDepth));

    // ② pushed_ 语义：容量内的层 push 成功 / 溢出层失败
    int firstBadPushed = -1;
    for (int n = 0; n <= kT2Depth; ++n) {
        const bool want = (n < static_cast<int>(kMaxLogicalDepth));
        if (g_t2Pushed[n] != want && firstBadPushed < 0) firstBadPushed = n;
    }
    EXPECT_EQ(firstBadPushed, -1);                  // -1 ⇒ 全层符合（失败时给出首个违规层号）

    // ③ 🔴 中途对齐不变量（G-3 的真判据）
    int firstBadAlign = -1;
    for (int n = 0; n <= kT2Depth; ++n) {
        const int want = (n < static_cast<int>(kMaxLogicalDepth))
                             ? (n + 1)
                             : static_cast<int>(kMaxLogicalDepth);
        if (g_t2DepthAfterCall[n] != want && firstBadAlign < 0) firstBadAlign = n;
    }
    EXPECT_EQ(firstBadAlign, -1);                   // -1 ⇒ 全层对齐（失败时给出首个错位层号）

    // ④ 收尾归零（G-3：溢出帧不得参与 pop）
    EXPECT_EQ(g_lsDepth, 0u);
}

// ============================================================
// T3 LogicalStack.SetFrameLineUpdatesTop
//   只改**栈顶**帧的行号；pop 之后作用于新的栈顶；symbolIdx 与其它帧一律不动
// ============================================================
TEST(LogicalStack, SetFrameLineUpdatesTop) {
    g_lsDepth = 0;
    pushFrame(1u, 10u);
    pushFrame(2u, 20u);
    pushFrame(3u, 30u);

    setFrameLine(999u);
    EXPECT_EQ(g_lsFrames[0].line, 10u);             // 下层不受影响
    EXPECT_EQ(g_lsFrames[1].line, 20u);
    EXPECT_EQ(g_lsFrames[2].line, 999u);            // 仅栈顶被更新（D1 语义）
    EXPECT_EQ(g_lsFrames[2].symbolIdx, 3u);         // symbolIdx 不变

    setFrameLine(1234u);                            // 再次更新仍是同一栈顶
    EXPECT_EQ(g_lsFrames[2].line, 1234u);
    EXPECT_EQ(g_lsFrames[1].line, 20u);

    popFrame();
    setFrameLine(555u);                             // 栈顶变成第 2 帧
    EXPECT_EQ(g_lsFrames[1].line, 555u);
    EXPECT_EQ(g_lsFrames[2].line, 1234u);           // 已离栈的帧不被改写

    g_lsDepth = 0;
    setFrameLine(1u);                               // 空栈 ⇒ no-op（不得越界）
    EXPECT_EQ(g_lsDepth, 0u);
}
