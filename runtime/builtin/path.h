#pragma once
// ============================================================
// aura_rt/builtin/path.h — `path` 内置模块 + Path 类型
//
// README §11.4: path 提供纯函数路径操作，无副作用
// README §13:   Path 方法列表
//
// 使用方式：
//   import "path"
//   let p = path.join(path.new("/home"), "docs", "readme.md")
//
// C++ 映射：
//   path.new(s)        → path::new_(s)
//   path.join(a,b,...) → path::join({a, b, ...})
//
// Path 是值类型（非 GC 托管），可自由拷贝。
// ============================================================

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace aura_rt {

// ============================================================
// Path — 跨平台路径类型
// ============================================================
class Path {
public:
    Path() = default;
    explicit Path(std::filesystem::path p) : path_(std::move(p)) {}

    // --- 方法（README §13.2）---

    // parent() → Path — 返回父目录路径
    Path parent() const { return Path(path_.parent_path()); }

    // file_name() → string — 返回文件名（含扩展名）
    std::string file_name() const { return path_.filename().string(); }

    // extension() → string — 返回扩展名（含 .），无则 ""
    std::string extension() const { return path_.extension().string(); }

    // is_absolute() → bool — 是否为绝对路径
    bool is_absolute() const { return path_.is_absolute(); }

    // to_string() → string — 转为字符串表示
    std::string to_string() const { return path_.string(); }

    // --- 运算符 ---
    bool operator==(const Path& other) const { return path_ == other.path_; }
    bool operator!=(const Path& other) const { return !(*this == other); }

    // 内部访问（供 path 模块 / Io 实现使用）
    const std::filesystem::path& native() const { return path_; }

    // join 操作符（Path / Path 拼接）
    Path operator/(const Path& other) const { return Path(path_ / other.path_); }
    Path operator/(const std::string& segment) const { return Path(path_ / segment); }

private:
    std::filesystem::path path_;
};

// ============================================================
// `path` 模块顶层函数（README §13.1）
//
// 通过 import "path" 导入，C++ 中映射为 path::xxx()
// 纯函数，无副作用。
// ============================================================
namespace path {

// new(s: string) → Path — 从字符串创建路径
inline Path new_(const std::string& s) { return Path(std::filesystem::path(s)); }

// join(parts...) → Path — 拼接多个路径
// 参数可以是 Path 或 string
inline Path join(const Path& first) { return first; }
inline Path join(const Path& first, const Path& second) { return first / second; }
inline Path join(const Path& first, const std::string& second) { return first / second; }

template <typename... Rest>
Path join(const Path& first, const Path& second, Rest&&... rest) {
    return join(first / second, std::forward<Rest>(rest)...);
}

template <typename... Rest>
Path join(const Path& first, const std::string& second, Rest&&... rest) {
    return join(first / second, std::forward<Rest>(rest)...);
}

} // namespace path

} // namespace aura_rt
