#include "CodeGen.h"
#include "../Sema/BuiltinRegistry.h"
#include "../Sema/SemType.h"
#include <sstream>

namespace Aura {

// ============================================================
// try / catch（feature-18 P2：IIFE 退役，统一原地路径）
//
// 为什么不再用 IIFE（原 `std::variant<Result, Error>` 技法）：
//   ① bug-87：sync 块生成 `co_await _ctx.wait_all()`，落进**非协程 lambda** ⇒
//      g++ `unable to find the promise type for this coroutine`；
//   ② bug-90：try 体内调用协程函数时 initExpr 是 `aura_rt::task<int>`，
//      与 SemType 推出的 `int` 不匹配 ⇒ `could not convert 'task<int>' to
//      'std::variant<int, Error>'`。
//   根因单一：**try 体被塞进非协程 lambda** ⇒ 去掉 IIFE 即可（plan §2.5 翻案结论）。
//
// 为什么 catch 只赋值、catchBody 要挪到正常流程：
//   C++ 标准**禁止 catch handler 内出现 co_await**（GCC 实测
//   `error: await expressions are not permitted in handlers`）⇒ trick：
//   handler 内只做值拷贝 + 置标志，catchBody 在 `if (_tk_err) { ... }`
//   分支里生成 —— 那里已是正常流程，`co_await` 合法。
//
// 为什么 try 体可以直接原地生成（含 co_await）：
//   值化后「同步 throw」与「协程 await_resume 抛」走**同一条 C++ 异常路径**
//   ⇒ 就地捕获即可，不需要「逐语句显式检查」（plan §4.5 末条）。
// ============================================================

void CodeGenerator::genTryCatchStmt(std::ostream& cpp,
                                     const TryCatchStmt& stmt,
                                     bool isCoroutine) {
    // P2：isCoroutine 只用于透传给 try 体/ catchBody（co_await 由下游判定）
    std::string cv = safeName(stmt.catchVar);

    cpp << indentStr() << "{\n";
    indentLevel_++;

    // ① 承装槽 + 5 个 Ref 模式根化句柄
    //    Ref 模式绑定 `_tk_hold` 的**成员地址**：compact 时 GC 原位改写这些字段，
    //    保证 catchBody（可能含 co_await ⇒ 触发 GC）中读取的 kind/message/file/stack 有效。
    //    `_tk_hold` 是非 optional 的值对象 ⇒ 成员地址始终有效 ⇒ 可安全用 Ref 模式。
    //    ⚠️ kind/file 通常为 intern_string（全局根常驻），此处一并根化 = 防将来来源变化
    //       （与既有 StmtTry/CodeGen 的注释同理由）。
    writeLine(cpp, "bool _tk_err = false;");
    writeLine(cpp, "aura_rt::Error _tk_hold{};");
    writeLine(cpp, "aura_rt::GcRootHandle<decltype(_tk_hold.kind)> _tk_kind_h(_tk_hold.kind);");
    writeLine(cpp, "aura_rt::GcRootHandle<decltype(_tk_hold.message)> _tk_msg_h(_tk_hold.message);");
    writeLine(cpp, "aura_rt::GcRootHandle<decltype(_tk_hold.extra)> _tk_extra_h(_tk_hold.extra);");
    writeLine(cpp, "aura_rt::GcRootHandle<decltype(_tk_hold.file)> _tk_file_h(_tk_hold.file);");
    writeLine(cpp, "aura_rt::GcRootHandle<decltype(_tk_hold.stack)> _tk_stack_h(_tk_hold.stack);");

    // ② try 体：**原地**生成（isCoroutine 原样透传 ⇒ try 内 co_await / sync 块合法）
    writeLine(cpp, "try {");
    indentLevel_++;
    // feature-14 U5：try 体在 `try {` 之内 → opensScope=true。
    if (stmt.tryBody) genBlock(cpp, *stmt.tryBody, isCoroutine, /*opensScope=*/true);
    indentLevel_--;

    // ③ catch handler：只做值拷贝 + 置标志（**禁止**在此生成 co_await）
    writeLine(cpp, "} catch (const aura_rt::Error& _e) {");
    indentLevel_++;
    writeLine(cpp, "_tk_hold.kind = _e.kind; _tk_hold.message = _e.message; _tk_hold.extra = _e.extra;");
    writeLine(cpp, "_tk_hold.file = _e.file; _tk_hold.line = _e.line; _tk_hold.stack = _e.stack;");
    writeLine(cpp, "_tk_err = true;");
    indentLevel_--;
    writeLine(cpp, "}");

    // ④ catchBody：在正常流程分支中生成（可含 co_await）
    cpp << indentStr() << "if (_tk_err) {\n";
    indentLevel_++;
    writeLine(cpp, "auto& " + cv + " = _tk_hold;");
    valueTypeVarNames_.insert(cv);
    if (stmt.catchBody) genBlock(cpp, *stmt.catchBody, isCoroutine, /*opensScope=*/true);
    valueTypeVarNames_.erase(cv);
    indentLevel_--;
    cpp << indentStr() << "}\n";

    indentLevel_--;
    cpp << indentStr() << "}\n";
}

// (IdRefCollector / DeclaredCollector 定义已移至 CodeGen.h)

} // namespace Aura
