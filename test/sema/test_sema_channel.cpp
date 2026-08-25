// ============================================================
// test_sema_channel.cpp — Sema 通道语义单元测试
//
// 覆盖：channel<T> 元素类型标注、sync.Channel<T>、
//       receive/is_none/unwrap、close、for-in、未标注行为
// ============================================================
#include "framework/test_framework.h"
#include "framework/test_helpers.h"

using namespace aura_test;

// ============================================================
// channel<T>（协程通道）
// ============================================================
TEST(SemaChannel, AnnotatedSend) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let ch: channel<int> = channel(10); ch.send(1) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaChannel, AnnotatedReceive) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let ch: channel<int> = channel(10); let v = ch.receive() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaChannel, AnnotatedForIn) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let ch: channel<int> = channel(10); for v in ch { io.println(v) } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaChannel, AnnotatedClose) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let ch: channel<int> = channel(10); ch.close() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaChannel, UnannotatedReceiveErrors) {
    // 元素类型必须显式标注；未标注时 receive 报错
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let ch = channel(10); let v = ch.receive() }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
}

TEST(SemaChannel, UnannotatedSendErrors) {
    // A4（行为变更）：未标注 + send 报错（与 receive 对齐，元素类型必须显式标注）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let ch = channel(10); ch.send(1) }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
}

TEST(SemaChannel, UnannotatedForInErrors) {
    // A4（行为变更）：未标注 for-in 报错（与 receive 对齐，元素类型必须显式标注）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let ch = channel(10); for v in ch { } }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
}

TEST(SemaChannel, UnannotatedConstructErrors) {
    // A4：无标注 channel 构造报错（元素类型必须显式标注，避免裸 Channel* 坏代码）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let ch = channel(10) }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
}

// ============================================================
// sync.Channel<T>（跨线程通道）
// ============================================================
TEST(SemaChannel, SyncChannelAnnotated) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let ch: sync.Channel<int> = sync.Channel(10); ch.send(1) }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaChannel, SyncChannelUnannotatedReceiveErrors) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let ch = sync.Channel(10); let v = ch.receive() }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
}

TEST(SemaChannel, SyncChannelUnannotatedSendErrors) {
    // A4：无标注 sync.Channel 的 send 报错（元素类型必须显式标注）
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let ch = sync.Channel(10); ch.send(1) }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
}

TEST(SemaChannel, SyncChannelUnannotatedConstructErrors) {
    // A4：无标注 sync.Channel 构造报错
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) { let ch = sync.Channel(10) }",
        diag);
    EXPECT_TRUE(diag.hasErrors());
}

TEST(SemaChannel, SyncChannelReceiveOptional) {
    // receive 返回 Optional<T>：is_none / unwrap 消费
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) {"
        " let ch: sync.Channel<int> = sync.Channel(10);"
        " let v = ch.receive();"
        " if v.is_none() { } else { let x = v.unwrap() } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaChannel, SyncChannelIsDone) {
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) {"
        " let ch: sync.Channel<int> = sync.Channel(10); let b = ch.is_done() }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}

TEST(SemaChannel, SyncChannelForInInThread) {
    // sync thread 块内 for val in ch 自动展开
    Aura::DiagnosticEngine diag;
    analyzeSource(
        "fun main(io: Io) {"
        " let ch: sync.Channel<int> = sync.Channel(10);"
        " sync thread { spawn (ch: sync.Channel<int>, io: Io) { for val in ch { io.println(val) } } } }",
        diag);
    EXPECT_FALSE(diag.hasErrors());
}
