#pragma once
// ============================================================
// aura_rt/builtin/io.h ─ Io 能力对象
//
// README §11.3: main(io: Io) — 所有 I/O 通过 io 显式调用
// README §12:   Io API 完整列表
//
// 所有 I/O 原语返回 task<T>，在协程上下文中使用 co_await 调用。
// 路径参数使用 Path 类型（来自 path 模块）。
// ============================================================

#include "../types.h"
#include "../task.h"
#include "path.h"

#include <string>

namespace aura_rt {

// ============================================================
// Io — I/O 能力令牌
//
// 封装所有 I/O 副作用原语。
// main(io: Io) 接收运行时注入的 Io 实例。
// ============================================================
class Io {
public:
    Io() = default;

    // ========== 终端 I/O ==========

    // println(value: string) — 输出一行文本（自动换行）
    task<void> println(GcString* value) const;

    // println_sync(value: string) — 同步版本，用于 spawn/嵌套协程中避免协程嵌套
    void println_sync(GcString* value) const;

    // readln() throws -> string — 读取一行标准输入
    task<GcString*> readln();

    // ========== 文件 I/O (参数为 Path) ==========

    // read_file(path: Path) throws -> string — 读取文件内容
    task<GcString*> read_file(const Path& path);

    // write_file(path: Path, content: string) throws — 写入文件（覆盖）
    task<void> write_file(const Path& path, const std::string& content);

    // file_exists(path: Path) -> bool — 检查文件或目录是否存在
    bool file_exists(const Path& path) const;

    // mkdir(path: Path) throws — 创建目录
    task<void> mkdir(const Path& path);

    // remove(path: Path) throws — 删除文件或空目录
    task<void> remove(const Path& path);

    // list_dir(path: Path) throws -> [Path] — 列出目录内容
    task<Array<Path>*> list_dir(const Path& path);

    // ========== 路径操作 ==========

    // cwd() -> Path — 获取当前工作目录
    Path cwd() const { return Path(std::filesystem::current_path()); }
};

} // namespace aura_rt
