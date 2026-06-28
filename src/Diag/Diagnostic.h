#pragma once
// ============================================================
// Diagnostic — 统一诊断数据结构
//
// 替代各组件独立的 errors_ vector，提供：
//   - 严重级别（Error / Warning / Note）
//   - 错误码（E001-E020）
//   - 源码范围（行号 + 起止列）
//   - 可选的修复提示
// ============================================================

#include <string>
#include <vector>

namespace Aura {

// ============================================================
// DiagSeverity — 诊断严重级别
// ============================================================
enum class DiagSeverity {
    Error,    // 阻塞编译
    Warning,  // 不阻塞，但应修复
    Note,     // 附加信息，附属于某个 Error/Warning
};

// ============================================================
// DiagCode — 错误码
// ============================================================
enum class DiagCode {
    None = 0,
    // 语法错误 (E001-E009)
    E001_ExpectedToken,      // 期望的 token 缺失
    E002_UnexpectedToken,    // 不期望的 token
    E003_BracketMismatch,    // 括号不匹配
    // 语义错误 (E010-E019)
    E010_UndefinedIdent,     // 未定义标识符
    E011_TypeMismatch,       // 类型不匹配
    E012_UndefinedGeneric,   // 未定义的泛型参数
    E013_MethodNotFound,     // 内置类型方法不存在
    E014_MatchNotExhaustive, // match 非穷尽
    E015_ConstReassign,      // const 重新赋值
    E016_ThrowsViolation,    // throws 违规
    E017_NoneStandalone,     // None 作为独立类型
    E018_SpawnOutsideSync,   // spawn 在 sync 块外
    E019_ImplMismatch,       // impl 接口不匹配
};

inline const char* diagCodeStr(DiagCode code) {
    switch (code) {
        case DiagCode::E001_ExpectedToken:      return "E001";
        case DiagCode::E002_UnexpectedToken:    return "E002";
        case DiagCode::E003_BracketMismatch:    return "E003";
        case DiagCode::E010_UndefinedIdent:     return "E010";
        case DiagCode::E011_TypeMismatch:       return "E011";
        case DiagCode::E012_UndefinedGeneric:   return "E012";
        case DiagCode::E013_MethodNotFound:     return "E013";
        case DiagCode::E014_MatchNotExhaustive: return "E014";
        case DiagCode::E015_ConstReassign:      return "E015";
        case DiagCode::E016_ThrowsViolation:    return "E016";
        case DiagCode::E017_NoneStandalone:     return "E017";
        case DiagCode::E018_SpawnOutsideSync:   return "E018";
        case DiagCode::E019_ImplMismatch:       return "E019";
        default: return "";
    }
}

// ============================================================
// SourceRange — 源码位置范围
// ============================================================
struct SourceRange {
    int line     = 0;
    int colStart = 0;  // 1-based，起始列
    int colEnd   = 0;  // 1-based，结束列（含）
};

// ============================================================
// Diagnostic — 一条诊断信息
// ============================================================
struct Diagnostic {
    DiagSeverity severity;
    DiagCode     code = DiagCode::None;
    SourceRange  range;
    std::string  message;
    std::string  fixHint;         // 可选的修复建议
    std::vector<Diagnostic> notes; // 附注（暂未使用，预留）
};

} // namespace Aura
