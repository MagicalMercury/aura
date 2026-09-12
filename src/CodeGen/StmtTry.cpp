#include "CodeGen.h"
#include "../Sema/BuiltinRegistry.h"
#include "../Sema/SemType.h"
#include <sstream>

namespace Aura {

// ============================================================
// try / catch
// ============================================================

void CodeGenerator::genTryCatchStmt(std::ostream& cpp,
                                     const TryCatchStmt& stmt,
                                     bool isCoroutine) {
    if (!isCoroutine || !stmt.tryBody || stmt.tryBody->stmts.empty()) {
        genTryCatchRaw(cpp, stmt, isCoroutine);
        return;
    }

    // === 协程安全模式 ===
    // C++20 协程 + GCC 上 try/catch 有 bug（非 std::exception 异常类型匹配失败）
    // 改用：把 try 体中的"setup"语句包装为普通函数 IIFE，用 variant 传回错误
    //
    // 策略：分析 try 体，找到第一个 LetDecl（含 initializer）作为"抛出版本"，
    // 将其初始值表达式提取到非协程 IIFE 中，其余语句作为 continuation 分支。

    auto& stmts = stmt.tryBody->stmts;

    // 1. 找到 try 体中的第一个 LetDecl（含 initializer）
    const LetDecl* setupLet = nullptr;
    size_t letIdx = 0;
    for (size_t i = 0; i < stmts.size(); ++i) {
        if (auto* let = dynamic_cast<const LetDecl*>(stmts[i].get())) {
            if (let->initializer) { setupLet = let; letIdx = i; break; }
        }
    }

    if (!setupLet) {
        // v1.2 修复：协程模式下无 setupLet 时也用 IIFE + variant 模式
        // 原因：genTryCatchRaw 会在 catch handler 中生成 co_await，违反 C++ 标准
        // （catch handler 内禁止 co_await）
        // 策略：IIFE 执行 try 体所有语句（同步版本），返回 variant<monostate, Error>
        //       成功分支执行后续语句（无 setupLet 时通常无后续）
        //       错误分支执行 catchBody（在协程正常流程中，可含 co_await）
        if (!isCoroutine) {
            genTryCatchRaw(cpp, stmt, isCoroutine);
            return;
        }
        genTryCatchNoSetupIIFE(cpp, stmt, isCoroutine);
        return;
    }

    // 2. 推断结果类型
    // 优先用 SemType 推导（避免 decltype(initExpr) 中嵌套 lambda 在未求值上下文无法捕获变量）
    std::string initExpr = genExpr(*setupLet->initializer, false);
    std::string resultType;
    if (setupLet->type) {
        resultType = mapType(*setupLet->type);
    } else if (setupLet->initializer && setupLet->initializer->inferredType) {
        resultType = mapSemType(*setupLet->initializer->inferredType);
    }
    if (resultType.empty() || resultType == "auto") {
        // 退化：无法从 SemType 推导，用 decltype（仅在 initExpr 不含 lambda 时安全）
        resultType = "decltype(" + initExpr + ")";
    }
    std::string varName = safeName(setupLet->name);
    std::string cv = safeName(stmt.catchVar);

    cpp << indentStr() << "{\n";
    indentLevel_++;

    // 3. 安全 IIFE：在普通函数中 try/catch，返回 variant<Result, Error>
    writeLine(cpp, "auto _try = [&]() -> std::variant<" + resultType + ", aura_rt::Error> {");
    indentLevel_++;
    writeLine(cpp, "try {");
    indentLevel_++;
    writeLine(cpp, "return " + initExpr + ";");
    indentLevel_--;
    writeLine(cpp, "} catch (const aura_rt::Error& _e) {");
    indentLevel_++;
    writeLine(cpp, "return _e;");
    indentLevel_--;
    writeLine(cpp, "}");
    indentLevel_--;
    writeLine(cpp, "}();");

    // 4. 错误分支
    cpp << indentStr() << "if (std::holds_alternative<aura_rt::Error>(_try)) {\n";
    indentLevel_++;
    writeLine(cpp, "auto& " + cv + " = std::get<aura_rt::Error>(_try);");
    // GC 安全：variant 中的 Error 是值嵌入的，GC 不知道其内部结构，
    // 不会自动更新 kind/message/extra 指针。用 GcRootHandle 保护，
    // catchBody 中若有 co_await 触发 GC compact，指针会被自动更新。
    writeLine(cpp, "aura_rt::GcRootHandle<decltype(" + cv + ".kind)> _eh_kind(" + cv + ".kind);");
    writeLine(cpp, "aura_rt::GcRootHandle<decltype(" + cv + ".message)> _eh_msg(" + cv + ".message);");
    writeLine(cpp, "aura_rt::GcRootHandle<decltype(" + cv + ".extra)> _eh_extra(" + cv + ".extra);");
    valueTypeVarNames_.insert(cv);
    if (stmt.catchBody) genBlock(cpp, *stmt.catchBody, isCoroutine);
    valueTypeVarNames_.erase(cv);
    indentLevel_--;
    cpp << indentStr() << "} else {\n";
    indentLevel_++;

    // 5. 成功分支
    writeLine(cpp, "auto " + varName + " = std::get<" + resultType + ">(_try);");
    // GC 安全：resultType 为 GC 指针时用 GcRootHandle 保护（对齐错误分支 kind/message/extra 保护），
    // 后续 stmts 中若触发 GC（co_await compact），varName 指向的堆对象不会被回收/悬垂。
    if (isGcPointerType(resultType)) {
        writeLine(cpp, "aura_rt::GcRootHandle<" + resultType + "> _" + varName + "_root(" + varName + ");");
    }
    for (size_t i = letIdx + 1; i < stmts.size(); ++i) {
        if (stmts[i]) genStmt(cpp, *stmts[i], isCoroutine);
    }

    indentLevel_--;
    cpp << indentStr() << "}\n";
    indentLevel_--;
    cpp << indentStr() << "}\n";
}

// v1.2 修复：协程模式下无 setupLet 的 try/catch 用 IIFE + variant<monostate, Error>
// 避免 catch handler 内生成 co_await（C++ 标准禁止）
// IIFE 内执行 try 体所有语句（同步版本，isCoroutine=false），
// 成功返回 monostate，失败返回 Error；后续在协程正常流程中处理错误分支
void CodeGenerator::genTryCatchNoSetupIIFE(std::ostream& cpp,
                                            const TryCatchStmt& stmt,
                                            bool isCoroutine) {
    std::string cv = safeName(stmt.catchVar);

    cpp << indentStr() << "{\n";
    indentLevel_++;

    // IIFE：普通函数，执行 try 体所有语句（同步版本），返回 variant<monostate, Error>
    // feature-05：生成面联合已弃用 std::variant，此处为 try 内部 monostate|Error 机制保留
    writeLine(cpp, "auto _try = [&]() -> std::variant<std::monostate, aura_rt::Error> {");
    indentLevel_++;
    writeLine(cpp, "try {");
    indentLevel_++;
    // try 体语句：同步版本（isCoroutine=false，避免生成 co_await）
    if (stmt.tryBody) {
        for (auto& s : stmt.tryBody->stmts) {
            if (s) genStmt(cpp, *s, false);
        }
    }
    writeLine(cpp, "return std::monostate{};");
    indentLevel_--;
    writeLine(cpp, "} catch (const aura_rt::Error& _e) {");
    indentLevel_++;
    writeLine(cpp, "return _e;");
    indentLevel_--;
    writeLine(cpp, "}");
    indentLevel_--;
    writeLine(cpp, "}();");

    // 错误分支：在协程正常流程中执行 catchBody（可含 co_await）
    cpp << indentStr() << "if (std::holds_alternative<aura_rt::Error>(_try)) {\n";
    indentLevel_++;
    writeLine(cpp, "auto& " + cv + " = std::get<aura_rt::Error>(_try);");
    // GC 安全：variant 中的 Error 是值嵌入的，需 GcRootHandle 保护内部指针
    writeLine(cpp, "aura_rt::GcRootHandle<decltype(" + cv + ".kind)> _eh_kind(" + cv + ".kind);");
    writeLine(cpp, "aura_rt::GcRootHandle<decltype(" + cv + ".message)> _eh_msg(" + cv + ".message);");
    writeLine(cpp, "aura_rt::GcRootHandle<decltype(" + cv + ".extra)> _eh_extra(" + cv + ".extra);");
    valueTypeVarNames_.insert(cv);
    if (stmt.catchBody) genBlock(cpp, *stmt.catchBody, isCoroutine);
    valueTypeVarNames_.erase(cv);
    indentLevel_--;
    cpp << indentStr() << "}\n";

    indentLevel_--;
    cpp << indentStr() << "}\n";
}

void CodeGenerator::genTryCatchRaw(std::ostream& cpp,
                                    const TryCatchStmt& stmt,
                                    bool isCoroutine) {
    cpp << indentStr() << "try {\n";
    if (stmt.tryBody) genBlock(cpp, *stmt.tryBody, isCoroutine);
    std::string cv = safeName(stmt.catchVar);
    cpp << indentStr() << "} catch (aura_rt::Error& " << cv << ") {\n";
    indentLevel_++;
    // GC 安全：Error 在 C++ 异常存储区中（非 GC 堆），GC compact 不会自动更新
    // 其内部的 GcString* 指针（kind/message/extra）。用 GcRootHandle 持有这些指针的地址，
    // GC compact 时会通过 roots_ 更新它们，防止 catchBody 中访问悬垂指针。
    writeLine(cpp, "aura_rt::GcRootHandle<decltype(" + cv + ".kind)> _eh_kind(" + cv + ".kind);");
    writeLine(cpp, "aura_rt::GcRootHandle<decltype(" + cv + ".message)> _eh_msg(" + cv + ".message);");
    writeLine(cpp, "aura_rt::GcRootHandle<decltype(" + cv + ".extra)> _eh_extra(" + cv + ".extra);");
    valueTypeVarNames_.insert(cv);
    if (stmt.catchBody) genBlock(cpp, *stmt.catchBody, isCoroutine);
    valueTypeVarNames_.erase(cv);
    indentLevel_--;
    cpp << indentStr() << "}\n";
}

// (IdRefCollector / DeclaredCollector 定义已移至 CodeGen.h)

} // namespace Aura
