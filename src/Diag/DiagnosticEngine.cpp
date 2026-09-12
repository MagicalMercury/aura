#include "DiagnosticEngine.h"
#include "../AST/ASTNode.h"
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
    diags_.back().file = fileName_;   // 快照所属文件（并行任务 merge 后能定位模块）

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
// buildLineOffsets — 构建每行起始偏移缓存（惰性、只构建一次）
// lineOffsets_[k] = 第 k+1 行（1-based）在 source_ 中的起始下标
// ============================================================
void DiagnosticEngine::buildLineOffsets() const {
    if (lineOffsetsBuilt_) return;
    lineOffsets_.clear();
    lineOffsets_.push_back(0);
    for (size_t i = 0; i < source_.size(); ++i) {
        if (source_[i] == '\n' && i + 1 < source_.size()) {
            lineOffsets_.push_back(i + 1);
        }
    }
    lineOffsetsBuilt_ = true;
}

// ============================================================
// getSourceLine — 从 source_ 视图提取第 N 行
// 利用行偏移缓存 O(1) 定位，避免每次线性扫描源码
// ============================================================
std::string DiagnosticEngine::getSourceLine(int line) const {
    if (source_.empty() || line < 1) return {};
    buildLineOffsets();
    if (line >= 1 && line <= (int)lineOffsets_.size()) {
        size_t start = lineOffsets_[line - 1];
        size_t end = (line < (int)lineOffsets_.size()) ? lineOffsets_[line] - 1
                                                        : source_.size();
        auto sv = std::string_view(source_).substr(start, end - start);
        // 去尾 \r
        if (!sv.empty() && sv.back() == '\r') sv.remove_suffix(1);
        return std::string(sv);
    }
    return {};
}

// ============================================================
// print — 格式化输出（含源码上下文 + fix-hint）
// ============================================================
void DiagnosticEngine::print(std::ostream& os) const {
    for (auto& diag : diags_) {
        // --- 错误码 + 消息 ---
        if (diag.severity == DiagSeverity::Warning) os << "warning";
        else os << "error";
        if (diag.code != DiagCode::None) os << "[" << diagCodeStr(diag.code) << "]";
        os << ": " << diag.message << "\n";

        // --- 文件位置 ---
        const std::string& f = !diag.file.empty() ? diag.file : fileName_;
        if (!f.empty())
            os << "  --> " << f << ":" << diag.range.line << ":" << diag.range.colStart << "\n";
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
// mergeFrom — 合并另一引擎的诊断（多线程任务结果汇总）
// 不检查 maxErrors_：每模块 diag 上限仅防单模块级联刷屏，汇总时全部保留
// ============================================================
void DiagnosticEngine::mergeFrom(const DiagnosticEngine& other) {
    diags_.insert(diags_.end(), other.diags_.begin(), other.diags_.end());
    errorMessages_.insert(errorMessages_.end(),
                          other.errorMessages_.begin(), other.errorMessages_.end());
    errorCount_   += other.errorCount_;
    warningCount_ += other.warningCount_;
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
