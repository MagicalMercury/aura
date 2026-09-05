// ============================================================
// test_examples.cpp — example 目录 aura 代码回归测试
//
// 编译 example/ 下全部 .aura 示例（含用户模块 import 依赖），
// 验证它们都能通过词法/语法/语义分析，防止编译器改动导致示例回归。
//
// 说明：
//   - 使用 analyzeFile 走完整多文件模块分析流程（与 main.cpp 一致）
//   - 文件缺失时跳过并记录失败（防止 CI 环境路径差异误报）
// ============================================================
#include "framework/test_framework.h"
#include "framework/test_helpers.h"

#include <filesystem>
#include <string>
#include <vector>

using namespace aura_test;

#ifndef AURA_PROJECT_ROOT
#define AURA_PROJECT_ROOT "."
#endif

namespace {

// 拼接 example 目录下的文件路径
std::string examplePath(const std::string& rel) {
    return std::string(AURA_PROJECT_ROOT) + "/example/" + rel;
}

// 编译单个 example 文件，断言无错误
void expectExampleCompiles(const char* testName, const std::string& rel) {
    std::string path = examplePath(rel);
    if (!std::filesystem::exists(path)) {
        aura_test::fail(__FILE__, __LINE__,
            "example file missing: " + path);
        return;
    }
    Aura::DiagnosticEngine diag;
    bool ok = analyzeFile(path, diag);
    if (!ok) {
        aura_test::fail(__FILE__, __LINE__,
            "example failed to compile: " + rel + "\n" +
            joinErrorMessages(diag));
    }
}

} // namespace

// ============================================================
// 顶层示例
// ============================================================
TEST(Examples, TestAura) {
    // 元组/三元/复合赋值/数值提升/math/Iterator 联合/并发 GC 综合用例
    expectExampleCompiles("TestAura", "test.aura");
}

TEST(Examples, MathUtils) {
    // 泛型记录 Pair + 构造函数 + 泛型函数
    expectExampleCompiles("MathUtils", "math_utils.aura");
}

TEST(Examples, TestGcMutex) {
    // Mutex GC 稳定性
    expectExampleCompiles("TestGcMutex", "used/test_gc_mutex.aura");
}

// ============================================================
// used/ 目录示例
// ============================================================
TEST(Examples, Used1ComplexClosure) {
    // 复杂闭包：泛型管道、重试、接口、树映射、可变状态、条件组合
    expectExampleCompiles("Used1ComplexClosure", "used/1.aura");
}

TEST(Examples, Used2Closures) {
    // 大量闭包使用场景
    expectExampleCompiles("Used2Closures", "used/2.aura");
}

TEST(Examples, Used3RecordsGenerics) {
    // 记录/泛型/栈
    expectExampleCompiles("Used3RecordsGenerics", "used/3.aura");
}

TEST(Examples, Used4Los) {
    // LOS 大对象空间测试
    expectExampleCompiles("Used4Los", "used/4.aura");
}

TEST(Examples, Used5ArrayView) {
    // ArrayView 零拷贝视图测试
    expectExampleCompiles("Used5ArrayView", "used/5.aura");
}

TEST(Examples, Used6Conversions) {
    // int()/float()/str() 转换 + 默认参数
    expectExampleCompiles("Used6Conversions", "used/6.aura");
}

TEST(Examples, UsedMathUtils) {
    // 泛型记录 Pair（模块形态）
    expectExampleCompiles("UsedMathUtils", "used/math_utils.aura");
}

TEST(Examples, UsedTestChannel) {
    // channel 测试
    expectExampleCompiles("UsedTestChannel", "used/test_channel.aura");
}

TEST(Examples, UsedTestClosure) {
    // 闭包测试
    expectExampleCompiles("UsedTestClosure", "used/test_closure.aura");
}

TEST(Examples, UsedTestImport) {
    // 用户模块 import（import "math_utils.aura" as math）
    expectExampleCompiles("UsedTestImport", "used/test_import.aura");
}

TEST(Examples, UsedTestRange) {
    // range 迭代器测试
    expectExampleCompiles("UsedTestRange", "used/test_range.aura");
}

TEST(Examples, UsedTestSyncFor) {
    // sync for 并行迭代
    expectExampleCompiles("UsedTestSyncFor", "used/test_sync_for.aura");
}

TEST(Examples, UsedTestSyncMax) {
    // sync(max=N) 并发
    expectExampleCompiles("UsedTestSyncMax", "used/test_sync_max.aura");
}

TEST(Examples, UsedLosCompact) {
    // LOS compact 测试
    expectExampleCompiles("UsedLosCompact", "used/los_compact_test.aura");
}
