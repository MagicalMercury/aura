// ============================================================
// test/rt/test_logical_stack.cpp — feature-18 P1：逻辑栈基础设施
//   覆盖：push/pop 配平、setFrameLine 只改栈顶、溢出截断不越界、
//         FrameGuard 在异常路径也 pop、容量边界
// ============================================================
#include "logical_stack.h"
#include "framework/test_framework.h"
#include <memory>                   // 偏差补：std::unique_ptr / std::make_unique（GuardOverflowKeepsDepthBalanced）

using namespace aura_rt;

TEST(LogicalStack, PushPopBalance) {
    g_lsDepth = 0;
    pushFrame(1, 10);
    pushFrame(2, 20);
    EXPECT_EQ(g_lsDepth, 2u);
    popFrame();
    EXPECT_EQ(g_lsDepth, 1u);
    EXPECT_EQ(g_lsFrames[0].symbolIdx, 1u);
    EXPECT_EQ(g_lsFrames[0].line, 10u);
    popFrame();
    EXPECT_EQ(g_lsDepth, 0u);
}

TEST(LogicalStack, SetFrameLineOnlyTouchesTop) {
    g_lsDepth = 0;
    pushFrame(1, 10);
    pushFrame(2, 20);
    setFrameLine(99);
    EXPECT_EQ(g_lsFrames[0].line, 10u);   // 不受影响
    EXPECT_EQ(g_lsFrames[1].line, 99u);   // 栈顶被更新（D1 语义）
    g_lsDepth = 0;
}

TEST(LogicalStack, SetFrameLineOnEmptyIsNoop) {
    g_lsDepth = 0;
    setFrameLine(123);                    // 不得越界
    EXPECT_EQ(g_lsDepth, 0u);
}

TEST(LogicalStack, OverflowDiscardsDeeperFrames) {
    g_lsDepth = 0;
    for (uint32_t i = 0; i < kMaxLogicalDepth + 20; ++i) pushFrame(i, i);
    EXPECT_EQ(g_lsDepth, kMaxLogicalDepth);            // 不越界、不覆盖
    EXPECT_EQ(g_lsFrames[0].symbolIdx, 0u);            // 最外帧保留
    EXPECT_EQ(g_lsFrames[kMaxLogicalDepth - 1].symbolIdx, kMaxLogicalDepth - 1);
    g_lsDepth = 0;
}

// ── G-3（GLM 审查）：溢出「中途错位」单测 —— 旧单测只验「结束后 depth 回 0」，
//    恰好是虚假通过的形态（配平只在结束后看似正确）─────────────────────────
TEST(LogicalStack, OverflowMidRecursionTopIsStillNewFrame) {
    g_lsDepth = 0;
    for (uint32_t i = 0; i < kMaxLogicalDepth; ++i) pushFrame(i, i);   // 恰好满
    EXPECT_EQ(g_lsDepth, kMaxLogicalDepth);
    // 递归继续：第 257 帧 push 必须失败（返回 false），且**不得覆盖**栈顶既有帧
    EXPECT_TRUE(pushFrame(9999u, 1u) == false);
    EXPECT_EQ(g_lsDepth, kMaxLogicalDepth);
    EXPECT_EQ(g_lsFrames[kMaxLogicalDepth - 1].symbolIdx, kMaxLogicalDepth - 1);  // 仍旧帧
    // 且 setFrameLine 仍作用于**当前栈顶**（不因溢出而错位）
    setFrameLine(4242u);
    EXPECT_EQ(g_lsFrames[kMaxLogicalDepth - 1].line, 4242u);
    g_lsDepth = 0;
}

// ── G-3：FrameGuard 溢出配平（仅真正 push 过的帧才 pop）────────────────────
TEST(LogicalStack, GuardOverflowKeepsDepthBalanced) {
    g_lsDepth = 0;
    {
        std::vector<std::unique_ptr<FrameGuard>> guards;
        for (uint32_t i = 0; i < kMaxLogicalDepth + 44; ++i)
            guards.push_back(std::make_unique<FrameGuard>(i, i));   // 44 个溢出（pushed_ == false）
        EXPECT_EQ(g_lsDepth, kMaxLogicalDepth);                     // 溢出帧未推进 depth
    }                                                               // 全部析构：只有成功者 pop
    EXPECT_EQ(g_lsDepth, 0u);                                       // 配平（旧实现会扣到 212）
}

TEST(LogicalStack, GuardPopsOnExceptionPath) {
    g_lsDepth = 0;
    try {
        FrameGuard g(7, 70);
        EXPECT_EQ(g_lsDepth, 1u);
        throw 1;
    } catch (...) { /* 期望：析构已 pop */ }
    EXPECT_EQ(g_lsDepth, 0u);
}

TEST(LogicalStack, CaptureReturnsNullOnEmptyStack) {
    g_lsDepth = 0;
    EXPECT_TRUE(captureLogicalStack() == nullptr);
}
