#pragma once
#include "Diagnostic.h"
#include <ostream>
#include <string_view>
#include <vector>

namespace Aura {

struct ASTNode;

class DiagnosticEngine {
public:
    DiagnosticEngine() = default;

    void report(const Diagnostic& diag);

    void error(int line, int col, DiagCode code, const std::string& msg, const std::string& hint = "");
    void error(const ASTNode& node, DiagCode code, const std::string& msg, const std::string& hint = "");
    // 兼容旧 API（无代码）
    void error(int line, int col, const std::string& msg, const std::string& hint = "");
    void error(const ASTNode& node, const std::string& msg, const std::string& hint = "");
    void warn(int line, int col, const std::string& msg);
    void note(int line, int col, const std::string& msg);

    [[nodiscard]] bool hasErrors() const { return errorCount_ > 0; }
    [[nodiscard]] bool hasWarnings() const { return warningCount_ > 0; }
    [[nodiscard]] int  errorCount() const { return errorCount_; }
    [[nodiscard]] int  warningCount() const { return warningCount_; }
    [[nodiscard]] const std::vector<Diagnostic>& diagnostics() const { return diags_; }
    [[nodiscard]] const std::vector<std::string>& errorMessages() const { return errorMessages_; }

    void setMaxErrors(int n) { maxErrors_ = n; }
    void setSourceView(std::string_view source) { source_ = source; }
    void setFileName(const std::string& name) { fileName_ = name; }

    void print(std::ostream& os) const;
    void reset();

private:
    std::vector<Diagnostic> diags_;
    std::vector<std::string> errorMessages_;
    std::string_view source_;
    std::string fileName_;
    int maxErrors_   = 20;
    int errorCount_  = 0;
    int warningCount_ = 0;

    // 获取源码第 N 行（1-based）
    std::string getSourceLine(int line) const;
};

} // namespace Aura
