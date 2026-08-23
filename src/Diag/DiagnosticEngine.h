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
    // source_ 变更后需失效行偏移缓存，下次 getSourceLine 重建
    void setSourceView(std::string_view source) { source_ = source; lineOffsetsBuilt_ = false; }
    void setFileName(const std::string& name) { fileName_ = name; }

    void print(std::ostream& os) const;
    // 将 other 的诊断并入本引擎（多线程任务结果汇总；不截断，保留全部已记录错误）
    void mergeFrom(const DiagnosticEngine& other);
    void reset();

private:
    std::vector<Diagnostic> diags_;
    std::vector<std::string> errorMessages_;
    std::string source_;    // 按值持有源码（setSourceView 任意入参生命周期均安全，避免悬垂 string_view）
    std::string fileName_;
    int maxErrors_   = 20;
    int errorCount_  = 0;
    int warningCount_ = 0;

    // 行偏移缓存：避免 getSourceLine 每次线性扫描整个源码
    // （mutable：在 const 的 getSourceLine 内惰性构建）
    mutable std::vector<size_t> lineOffsets_;
    mutable bool lineOffsetsBuilt_ = false;
    void buildLineOffsets() const;

    // 获取源码第 N 行（1-based）
    std::string getSourceLine(int line) const;
};

} // namespace Aura
