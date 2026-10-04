// ============================================================
// test/rt/test_io.cpp — bug-97：Io::read_file 的错误分支回归（非 ASAN 判据）
//
// 目的：锁住「读不存在文件 ⇒ 抛 aura_rt::Error(kind=io_error, message 含路径)」
//       这一既定语义（bug-97 的 throw 分支实测无越界；本用例防将来退化）。
// 用 read_file_sync：不经事件循环 ⇒ 单测内可直判（avoid 惰性协程实参悬垂 bug-102）。
// ============================================================
#include "builtin/io.h"
#include "builtin/path.h"
#include "framework/test_framework.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <process.h>
#define aura_getpid _getpid
#else
#include <unistd.h>
#define aura_getpid getpid
#endif

using namespace aura_rt;

namespace {
std::string sv(GcString* s) {
    return s && s->data() ? std::string(s->data(), static_cast<size_t>(s->length)) : std::string();
}
} // namespace

TEST(RtIo, ReadFileSyncMissingFileThrowsIoErrorWithPath) {
    Io io;
    // 相对短名 + 不存在的文件（CWD 由测试进程决定，不存在 ⇒ 必失败）
    Path p{std::filesystem::path(std::string("aura_bug97_no_such_file.dat"))};

    bool threw = false;
    try {
        (void)io.read_file_sync(p);
    } catch (const Error& e) {
        threw = true;
        EXPECT_EQ(sv(e.kind), "io_error");
        EXPECT_CONTAINS(sv(e.message), "cannot open file");
        EXPECT_CONTAINS(sv(e.message), "aura_bug97_no_such_file.dat");   // 路径未丢失/未失真
    } catch (...) {
        // 其它类型异常 = 违规（应为 Error）
        EXPECT_TRUE(false);
    }
    EXPECT_TRUE(threw);
}

TEST(RtIo, ReadFileSyncExistingFileReadsContent) {
    Io io;
    const std::string name = "aura_bug97_fixture.dat";
    { std::ofstream f(name, std::ios::binary | std::ios::trunc); f << "0123456789abcdef"; }
    Path p{std::filesystem::path(name)};
    GcString* s = io.read_file_sync(p);
    EXPECT_EQ(sv(s), "0123456789abcdef");
    std::error_code ec;
    std::filesystem::remove(name, ec);
}

// bug-102(B)：按值签名下，长路径 read_file（io 面）语义仍正确（真读内容）
//   注：走 sync 版（不改签名，仍 const&）作语义对比；长路径越过 SSO ⇒ 触及堆缓冲
TEST(RtIo, ReadFileByValueLongPathReadsContent) {
    Io io;
    auto dir = std::filesystem::temp_directory_path() /
               ("rtio_b102_" + std::to_string(aura_getpid()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    std::string target = (dir / "payload_bug102_long_path_padding_file.dat").string();
    while (target.size() < 48) target += "x";
    { std::ofstream f(target, std::ios::binary | std::ios::trunc); f << "rtio-b102"; }

    Path p{std::filesystem::path(target)};
    GcString* s = io.read_file_sync(p);            // sync 版语义对比（不改签名）
    EXPECT_EQ(std::string(s->data(), static_cast<size_t>(s->length)), "rtio-b102");
    std::filesystem::remove_all(dir, ec);
}
