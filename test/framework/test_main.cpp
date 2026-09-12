// ============================================================
// test_main.cpp — Aura 编译器单元测试入口
//
// 运行所有 TEST() 用例，输出汇总结果，失败时返回非零退出码（供 CTest 使用）。
// 启动时切换到项目根目录（builtins/ 所在目录），保证内置模块可被加载。
// ============================================================
#include "test_framework.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <direct.h>
#define chdir _chdir
#else
#include <unistd.h>
#endif

#ifndef AURA_PROJECT_ROOT
#define AURA_PROJECT_ROOT "."
#endif

int main(int argc, char* argv[]) {
    // 切换到项目根目录，使 ModuleManager 能找到 builtins/*.aurai
    if (chdir(AURA_PROJECT_ROOT) != 0) {
        std::fprintf(stderr, "[test] WARNING: cannot chdir to %s\n",
                     AURA_PROJECT_ROOT);
    }

    // 支持 --filter=SuiteName 过滤
    std::string filter;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--filter=", 0) == 0) filter = a.substr(9);
    }

    auto& tests = aura_test::registry();
    int passed = 0;
    int failed = 0;
    std::string currentSuite;

    for (auto& t : tests) {
        if (!filter.empty() && t.suite != filter) continue;
        aura_test::failures().clear();
        try {
            t.fn();
        } catch (const std::exception& e) {
            aura_test::fail(__FILE__, __LINE__,
                std::string("uncaught exception: ") + e.what());
        } catch (...) {
            aura_test::fail(__FILE__, __LINE__, "uncaught unknown exception");
        }
        if (aura_test::failures().empty()) {
            ++passed;
            if (t.suite != currentSuite) {
                currentSuite = t.suite;
                std::printf("\n[----------] %s\n", t.suite.c_str());
            }
            std::printf("[  PASSED  ] %s.%s\n", t.suite.c_str(), t.name.c_str());
        } else {
            ++failed;
            if (t.suite != currentSuite) {
                currentSuite = t.suite;
                std::printf("\n[----------] %s\n", t.suite.c_str());
            }
            std::printf("[  FAILED  ] %s.%s\n", t.suite.c_str(), t.name.c_str());
            for (auto& f : aura_test::failures()) {
                std::printf("    %s:%d: %s\n", f.file.c_str(), f.line,
                            f.msg.c_str());
            }
        }
    }

    std::printf("\n[==========] %d tests, %d passed, %d failed\n",
                (int)tests.size(), passed, failed);
    return failed == 0 ? 0 : 1;
}
