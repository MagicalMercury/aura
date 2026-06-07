// ============================================================
// aura_rt/builtin/io.cpp ─ Io 能力对象实现
// ============================================================

#include "io.h"
#include "../gc.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace aura_rt {

// ============================================================
// 终端 I/O
// ============================================================

task<void> Io::println(GcString* value) {
    if (value && value->data) {
        std::cout << std::string_view(value->data, value->length) << '\n';
    } else {
        std::cout << '\n';
    }
    co_return;
}

void Io::println_sync(GcString* value) {
    if (value && value->data) {
        std::cout << std::string_view(value->data, value->length) << '\n';
    } else {
        std::cout << '\n';
    }
}

task<GcString*> Io::readln() {
    std::string line;
    if (!std::getline(std::cin, line)) {
        throw Error(make_string("io_error"),
                    make_string("failed to read from stdin"));
        co_return nullptr;
    }
    co_return make_string(line);
}

// ============================================================
// 文件 I/O
// ============================================================

task<GcString*> Io::read_file(const Path& path) {
    std::ifstream file(path.native(), std::ios::binary);
    if (!file.is_open()) {
        throw Error(make_string("io_error"),
                    make_string("cannot open file: " + path.to_string()));
        co_return nullptr;
    }
    std::ostringstream oss;
    oss << file.rdbuf();
    file.close();
    co_return make_string(oss.str());
}

task<void> Io::write_file(const Path& path, const std::string& content) {
    std::ofstream file(path.native(), std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        throw Error(make_string("io_error"),
                    make_string("cannot write file: " + path.to_string()));
        co_return;
    }
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!file) {
        throw Error(make_string("io_error"),
                    make_string("write failed: " + path.to_string()));
    }
    file.close();
    co_return;
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
                    make_string("cannot create directory: " + path.to_string() + " - " + ec.message()));
    }
    co_return;
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
                    make_string("cannot remove: " + path.to_string() + " - " + ec.message()));
    }
    co_return;
}

task<Array<Path>*> Io::list_dir(const Path& path) {
    std::error_code ec;
    std::vector<Path> entries;
    for (auto& entry : std::filesystem::directory_iterator(path.native(), ec)) {
        if (ec) {
            throw Error(make_string("io_error"),
                        make_string("cannot list directory: " + path.to_string() + " - " + ec.message()));
            co_return nullptr;
        }
        entries.push_back(Path(entry.path()));
    }

    // 构造 Array<Path>，统一走 GC 分配
    auto* arr = Array<Path>::make(static_cast<int32_t>(entries.size()));
    arr->length = static_cast<int32_t>(entries.size());
    std::copy(entries.begin(), entries.end(), arr->elements);
    co_return arr;
}

} // namespace aura_rt
