// ============================================================
// aura_rt/builtin/io.cpp ─ Io 能力对象实现
// ============================================================

#include "io.h"
#include "array.h"
#include "../gc.h"
#include "string.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#ifdef _WIN32
#include "../win_iocp.h"
#include "../event_loop.h"
#endif
#include <future>

namespace aura_rt {

// 终端输出锁：多线程 println/println_sync 并发时保证整行原子输出
namespace {
    std::mutex g_coutM;
}

// ============================================================
// 终端 I/O
// ============================================================

task<void> Io::println(GcString* value) const {
    std::lock_guard<std::mutex> lk(g_coutM);
    if (value && value->data()) {
        std::cout << std::string_view(value->data(), value->length) << '\n';
    } else {
        std::cout << '\n';
    }
    co_return;
}

void Io::println_sync(GcString* value) const {
    std::lock_guard<std::mutex> lk(g_coutM);
    if (value && value->data()) {
        std::cout << std::string_view(value->data(), value->length) << '\n';
    } else {
        std::cout << '\n';
    }
}

task<GcString*> Io::readln() {
    // 控制台不支持 OVERLAPPED，用独立线程 + FutureAwaiter 避免阻塞事件循环
    auto promise = std::make_shared<std::promise<GcString*>>();
    auto future  = promise->get_future();

    std::thread([promise = std::move(promise)]() {
        std::string line;
        if (!std::getline(std::cin, line)) {
            try {
                throw Error(make_string("io_error"),
                            make_string("failed to read from stdin"));
            } catch (...) {
                promise->set_exception(std::current_exception());
                return;
            }
        }
        promise->set_value(make_string(line));
    }).detach();

    struct FutureAwaiter {
        std::shared_ptr<std::promise<GcString*>> promise;
        std::future<GcString*> future;

        bool await_ready() const noexcept {
            return future.wait_for(std::chrono::seconds(0))
                   == std::future_status::ready;
        }
        void await_suspend(std::coroutine_handle<> cont) {
            std::thread([this, cont]() mutable {
                try { future.wait(); } catch (...) {}
                EventLoop::instance().schedule(cont);
            }).detach();
        }
        GcString* await_resume() { return future.get(); }
    };

    co_return co_await FutureAwaiter{std::move(promise), std::move(future)};
}

GcString* Io::readln_sync() {
    std::string line;
    if (!std::getline(std::cin, line)) {
        throw Error(make_string("io_error"),
                    make_string("failed to read from stdin"));
    }
    return make_string(line);
}

// ============================================================
// 文件 I/O
// ============================================================

task<GcString*> Io::read_file(const Path& path) {
#ifdef _WIN32
    // ── IOCP 真异步路径 ──
    HANDLE hFile = CreateFileW(path.native().c_str(),
                               GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        throw Error(make_string("io_error"),
                    make_string("cannot open file: " + path.native().string()));
        co_return nullptr;
    }

    IoCompletionPort::instance().associate(hFile, 0);

    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFile, &fileSize)) {
        CloseHandle(hFile);
        throw Error(make_string("io_error"), make_string("cannot get file size"));
        co_return nullptr;
    }

    size_t totalSize = static_cast<size_t>(fileSize.QuadPart);
    if (totalSize == 0) {
        CloseHandle(hFile);
        co_return GcString::empty();
    }

    // 分配 GC 字符串（未初始化容量，ReadFile 填充）
    GcString* result = GcString::make_with_capacity(totalSize, totalSize);

    EventLoop::instance().incPending();
    DWORD bytesRead = co_await IoAwaitable{hFile, result->data(),
                                            static_cast<DWORD>(totalSize)};

    result->length = static_cast<int32_t>(bytesRead);
    result->data()[bytesRead] = '\0';
    CloseHandle(hFile);
    co_return result;
#else
    // ── 非 Windows 阻塞回退 ──
    std::ifstream file(path.native(), std::ios::binary);
    if (!file.is_open()) {
        throw Error(make_string("io_error"),
                    make_string("cannot open file: " + path.native().string()));
        co_return nullptr;
    }
    std::ostringstream oss;
    oss << file.rdbuf();
    file.close();
    co_return make_string(oss.str());
#endif
}

GcString* Io::read_file_sync(const Path& path) {
    std::ifstream file(path.native(), std::ios::binary);
    if (!file.is_open()) {
        throw Error(make_string("io_error"),
                    make_string("cannot open file: " + path.native().string()));
    }
    std::ostringstream oss;
    oss << file.rdbuf();
    file.close();
    return make_string(oss.str());
}

task<void> Io::write_file(const Path& path, GcString* content) {
    std::ofstream file(path.native(), std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        throw Error(make_string("io_error"),
                    make_string("cannot write file: " + path.native().string()));
        co_return;
    }
    file.write(content->data(), static_cast<std::streamsize>(content->len()));
    if (!file) {
        throw Error(make_string("io_error"),
                    make_string("write failed: " + path.native().string()));
    }
    file.close();
    co_return;
}

void Io::write_file_sync(const Path& path, GcString* content) {
    std::ofstream file(path.native(), std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        throw Error(make_string("io_error"),
                    make_string("cannot write file: " + path.native().string()));
    }
    file.write(content->data(), static_cast<std::streamsize>(content->len()));
    if (!file) {
        throw Error(make_string("io_error"),
                    make_string("write failed: " + path.native().string()));
    }
    file.close();
}

bool Io::file_exists(const Path& path) const {
    return std::filesystem::exists(path.native());
}

// ============================================================
// 目录操作
// ============================================================

task<void> Io::mkdir(const Path& path) {
    std::error_code ec;
    std::filesystem::create_directories(path.native(), ec);
    if (ec) {
        throw Error(make_string("io_error"),
                    make_string("cannot create directory: " + path.native().string() + " - " + ec.message()));
    }
    co_return;
}

void Io::mkdir_sync(const Path& path) {
    std::error_code ec;
    std::filesystem::create_directories(path.native(), ec);
    if (ec) {
        throw Error(make_string("io_error"),
                    make_string("cannot create directory: " + path.native().string() + " - " + ec.message()));
    }
}

task<void> Io::remove(const Path& path) {
    std::error_code ec;
    if (std::filesystem::is_directory(path.native())) {
        std::filesystem::remove(path.native(), ec);
    } else {
        std::filesystem::remove(path.native(), ec);
    }
    if (ec) {
        throw Error(make_string("io_error"),
                    make_string("cannot remove: " + path.native().string() + " - " + ec.message()));
    }
    co_return;
}

void Io::remove_sync(const Path& path) {
    std::error_code ec;
    if (std::filesystem::is_directory(path.native())) {
        std::filesystem::remove(path.native(), ec);
    } else {
        std::filesystem::remove(path.native(), ec);
    }
    if (ec) {
        throw Error(make_string("io_error"),
                    make_string("cannot remove: " + path.native().string() + " - " + ec.message()));
    }
}

task<Array<Path>*> Io::list_dir(const Path& path) {
    std::error_code ec;
    std::vector<Path> entries;
    for (auto& entry : std::filesystem::directory_iterator(path.native(), ec)) {
        if (ec) {
            throw Error(make_string("io_error"),
                        make_string("cannot list directory: " + path.native().string() + " - " + ec.message()));
            co_return nullptr;
        }
        entries.push_back(Path(entry.path()));
    }

    // 构造 Array<Path>，统一走 GC 分配
    auto* arr = Array<Path>::make(0);
    for (auto& entry : entries) {
        arr->append(entry);
    }
    co_return arr;
}

Array<Path>* Io::list_dir_sync(const Path& path) {
    std::error_code ec;
    std::vector<Path> entries;
    for (auto& entry : std::filesystem::directory_iterator(path.native(), ec)) {
        if (ec) {
            throw Error(make_string("io_error"),
                        make_string("cannot list directory: " + path.native().string() + " - " + ec.message()));
        }
        entries.push_back(Path(entry.path()));
    }
    auto* arr = Array<Path>::make(0);
    for (auto& entry : entries) {
        arr->append(entry);
    }
    return arr;
}

} // namespace aura_rt
