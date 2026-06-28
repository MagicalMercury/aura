#include "DiagnosticEngine.h"
#include "../AST/ASTNode.h"
#include <algorithm>
#include <sstream>

namespace Aura {

// ============================================================
// report — 核心入口
// ============================================================
void DiagnosticEngine::report(const Diagnostic& diag) {
    if (diag.severity == DiagSeverity::Error) {
        if (errorCount_ >= maxErrors_) return;
        errorCount_++;
    } else if (diag.severity == DiagSeverity::Warning) {
        warningCount_++;
    }
    diags_.push_back(diag);

    // 维护兼容旧 API 的纯文本消息列表
    std::ostringstream oss;
    oss << "[line " << diag.range.line << ":" << diag.range.colStart << "] ";
    if (diag.code != DiagCode::None) oss << diagCodeStr(diag.code) << " ";
    if (diag.severity == DiagSeverity::Warning) oss << "warning: ";
    oss << diag.message;
    errorMessages_.push_back(oss.str());
}

// ============================================================
// 便捷方法（带错误码）
// ============================================================
void DiagnosticEngine::error(int line, int col, DiagCode code, const std::string& msg, const std::string& hint) {
    report(Diagnostic{DiagSeverity::Error, code, SourceRange{line, col, col}, msg, hint, {}});
}

void DiagnosticEngine::error(const ASTNode& node, DiagCode code, const std::string& msg, const std::string& hint) {
    error(node.line, node.col, code, msg, hint);
}

// ============================================================
// 兼容旧 API（无错误码，默认 None）
// ============================================================
void DiagnosticEngine::error(int line, int col, const std::string& msg, const std::string& hint) {
    report(Diagnostic{DiagSeverity::Error, DiagCode::None, SourceRange{line, col, col}, msg, hint, {}});
}

void DiagnosticEngine::error(const ASTNode& node, const std::string& msg, const std::string& hint) {
    error(node.line, node.col, msg, hint);
}

void DiagnosticEngine::warn(int line, int col, const std::string& msg) {
    report(Diagnostic{DiagSeverity::Warning, DiagCode::None, SourceRange{line, col, col}, msg, {}, {}});
}

void DiagnosticEngine::note(int line, int col, const std::string& msg) {
    report(Diagnostic{DiagSeverity::Note, DiagCode::None, SourceRange{line, col, col}, msg, {}, {}});
}

// ============================================================
// getSourceLine — 从 source_ 视图提取第 N 行
// ============================================================
std::string DiagnosticEngine::getSourceLine(int line) const {
    if (source_.empty() || line < 1) return {};
    int current = 1;
    size_t start = 0;
    for (size_t i = 0; i < source_.size(); ++i) {
        if (current == line) {
            start = i;
            while (i < source_.size() && source_[i] != '\n') ++i;
            auto s = source_.substr(start, i - start);
            // 去尾 \r
            if (!s.empty() && s.back() == '\r') s.remove_suffix(1);
            return std::string(s);
        }
        if (source_[i] == '\n') ++current;
    }
    return {};
}

// ============================================================
// print — 格式化输出（含源码上下文 + fix-hint）
// ============================================================
void DiagnosticEngine::print(std::ostream& os) const {
    for (auto& diag : diags_) {
        // --- 错误码 + 消息 ---
        os << "error";
        if (diag.code != DiagCode::None) os << "[" << diagCodeStr(diag.code) << "]";
        os << ": " << diag.message << "\n";

        // --- 文件位置 ---
        if (!fileName_.empty())
            os << "  --> " << fileName_ << ":" << diag.range.line << ":" << diag.range.colStart << "\n";
        else
            os << "  --> line " << diag.range.line << ":" << diag.range.colStart << "\n";

        // --- 源码行 ---
        auto srcLine = getSourceLine(diag.range.line);
        if (!srcLine.empty()) {
            os << "   |\n";
            os << diag.range.line << " | " << srcLine << "\n";
            os << "   | ";
            // caret 指向错误位置
            for (int i = 1; i < diag.range.colStart; ++i) {
                if (i <= (int)srcLine.size() && srcLine[i - 1] == '\t')
                    os << '\t';
                else
                    os << ' ';
            }
            os << "^\n";
        }

        // --- fix-hint ---
        if (!diag.fixHint.empty()) {
            os << "   = help: " << diag.fixHint << "\n";
        }

        os << "\n";
    }
}

// ============================================================
// reset — 清除所有诊断
// ============================================================
void DiagnosticEngine::reset() {
    diags_.clear();
    errorMessages_.clear();
    errorCount_ = 0;
    warningCount_ = 0;
}

} // namespace Aura
