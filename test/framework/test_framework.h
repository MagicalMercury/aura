// ============================================================
// test_framework.h — Aura 编译器轻量级单元测试框架
//
// 零外部依赖（不依赖 GoogleTest/Catch2），便于离线构建与 CMake/CTest 集成。
// 用法：
//   TEST(SuiteName, CaseName) { EXPECT_TRUE(...); EXPECT_EQ(a, b); }
// 断言失败会记录文件/行号/信息，全部跑完后汇总并返回非零退出码。
// ============================================================
#pragma once

#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace aura_test {

// ============================================================
// 注册表
// ============================================================
struct TestCase {
    std::string suite;
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

inline bool registerTest(const char* suite, const char* name,
                         std::function<void()> fn) {
    registry().push_back(TestCase{suite, name, std::move(fn)});
    return true;
}

// ============================================================
// 断言失败记录
// ============================================================
struct Failure {
    std::string file;
    int line = 0;
    std::string msg;
};

inline std::vector<Failure>& failures() {
    static std::vector<Failure> f;
    return f;
}

inline void fail(const char* file, int line, const std::string& msg) {
    failures().push_back(Failure{file, line, msg});
}

// 通用比较：把值转成字符串再比较（要求类型可 << 到 ostream）
template <typename A, typename B>
std::string formatValue(const A& a, const B& b) {
    std::ostringstream os;
    os << "left=" << a << " right=" << b;
    return os.str();
}

// ============================================================
// 断言宏
// ============================================================
#define EXPECT_TRUE(cond)                                                     \
    do {                                                                      \
        if (!(cond)) {                                                        \
            aura_test::fail(__FILE__, __LINE__,                               \
                std::string("EXPECT_TRUE failed: ") + #cond);                 \
        }                                                                     \
    } while (0)

// ASSERT 系列：失败时立即 return，中止当前用例（避免空指针级联崩溃）
#define ASSERT_TRUE(cond)                                                     \
    do {                                                                      \
        if (!(cond)) {                                                        \
            aura_test::fail(__FILE__, __LINE__,                               \
                std::string("ASSERT_TRUE failed: ") + #cond);                 \
            return;                                                           \
        }                                                                     \
    } while (0)

#define ASSERT_FALSE(cond)                                                    \
    do {                                                                      \
        if (cond) {                                                           \
            aura_test::fail(__FILE__, __LINE__,                               \
                std::string("ASSERT_FALSE failed: ") + #cond);                \
            return;                                                           \
        }                                                                     \
    } while (0)

#define ASSERT_EQ(a, b)                                                       \
    do {                                                                      \
        auto va = (a); auto vb = (b);                                         \
        if (!(va == vb)) {                                                    \
            aura_test::fail(__FILE__, __LINE__,                               \
                std::string("ASSERT_EQ failed: ") + #a + " vs " + #b + " -> " \
                + aura_test::formatValue(va, vb));                            \
            return;                                                           \
        }                                                                     \
    } while (0)

#define ASSERT_NE(a, b)                                                       \
    do {                                                                      \
        auto va = (a); auto vb = (b);                                         \
        if (va == vb) {                                                       \
            aura_test::fail(__FILE__, __LINE__,                               \
                std::string("ASSERT_NE failed: ") + #a + " vs " + #b + " -> " \
                + aura_test::formatValue(va, vb));                            \
            return;                                                           \
        }                                                                     \
    } while (0)

#define EXPECT_FALSE(cond)                                                    \
    do {                                                                      \
        if (cond) {                                                           \
            aura_test::fail(__FILE__, __LINE__,                               \
                std::string("EXPECT_FALSE failed: ") + #cond);                \
        }                                                                     \
    } while (0)

#define EXPECT_EQ(a, b)                                                       \
    do {                                                                      \
        auto va = (a); auto vb = (b);                                         \
        if (!(va == vb)) {                                                    \
            aura_test::fail(__FILE__, __LINE__,                               \
                std::string("EXPECT_EQ failed: ") + #a + " vs " + #b + " -> " \
                + aura_test::formatValue(va, vb));                            \
        }                                                                     \
    } while (0)

#define EXPECT_NE(a, b)                                                       \
    do {                                                                      \
        auto va = (a); auto vb = (b);                                         \
        if (va == vb) {                                                       \
            aura_test::fail(__FILE__, __LINE__,                               \
                std::string("EXPECT_NE failed: ") + #a + " vs " + #b + " -> " \
                + aura_test::formatValue(va, vb));                            \
        }                                                                     \
    } while (0)

#define EXPECT_GE(a, b)                                                       \
    do {                                                                      \
        auto va = (a); auto vb = (b);                                         \
        if (!(va >= vb)) {                                                    \
            aura_test::fail(__FILE__, __LINE__,                               \
                std::string("EXPECT_GE failed: ") + #a + " >= " + #b + " -> " \
                + aura_test::formatValue(va, vb));                            \
        }                                                                     \
    } while (0)

#define EXPECT_LE(a, b)                                                       \
    do {                                                                      \
        auto va = (a); auto vb = (b);                                         \
        if (!(va <= vb)) {                                                    \
            aura_test::fail(__FILE__, __LINE__,                               \
                std::string("EXPECT_LE failed: ") + #a + " <= " + #b + " -> " \
                + aura_test::formatValue(va, vb));                            \
        }                                                                     \
    } while (0)

// 字符串包含断言
#define EXPECT_CONTAINS(haystack, needle)                                     \
    do {                                                                      \
        const std::string& h_ = (haystack);                                   \
        const std::string& n_ = (needle);                                     \
        if (h_.find(n_) == std::string::npos) {                               \
            aura_test::fail(__FILE__, __LINE__,                               \
                std::string("EXPECT_CONTAINS failed: \"") + n_ +              \
                "\" not found in: \"" + h_ + "\"");                           \
        }                                                                     \
    } while (0)

#define EXPECT_NOT_CONTAINS(haystack, needle)                                 \
    do {                                                                      \
        const std::string& h_ = (haystack);                                   \
        const std::string& n_ = (needle);                                     \
        if (h_.find(n_) != std::string::npos) {                               \
            aura_test::fail(__FILE__, __LINE__,                               \
                std::string("EXPECT_NOT_CONTAINS failed: \"") + n_ +          \
                "\" found in: \"" + h_ + "\"");                               \
        }                                                                     \
    } while (0)

// 测试用例注册宏
#define TEST(suite, name)                                                     \
    static void suite##_##name();                                             \
    static const bool suite##_##name##_registered =                           \
        aura_test::registerTest(#suite, #name, suite##_##name);               \
    static void suite##_##name()

} // namespace aura_test
