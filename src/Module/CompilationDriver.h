#pragma once

// ============================================================
// feature-21 Phase 2 · S5：CompilationDriver（`#const` 展开的 epoch 驱动 + 单文件入口）
//
// 权威：change.md §3.5（epoch 驱动 / DriverConfig / 单文件模式 R-4/R-8）、§3.7（调用点）、
//       §3.4.2（E-8 位置语义）、§3.6（ModuleManager 接线）、§11（R-5 / R-10 / N-6 / N-8 / N-25）。
//
// 职责边界：
//   · 本类只做「驱动」——轮次、闭包、拓扑序、建 working、调用 S4 的 pass、回写 AST；
//   · 展开语义（建表 / 替换 / 残留 / 删除 / 规模 / unquote）全部在 ExpandConst.*（S4）；
//   · 模块加载 / originFile 打戳 / 扫描表刷新在 ModuleManager（§3.6）。
//
// 硬约束（brief §2）：
//   · `closure` 只返回 `.aura` 源文件；内置根只参与边收集，不得进 files/pristine（N-6）。
//   · `maxEpochs` / `maxFiles` 是**绝对上限**（R-5），不随 files.size() 变化。
//   · `seenEdges` 的键 = 规范化**边集**（R-10），不是新增文件列表。
//   · 单文件宏生成用户 import ⇒ `macro-generated import requires multi-file compilation`
//     （先于 resolve，N-8）。
// ============================================================

#include "../Diag/DiagnosticEngine.h"

#include "ModuleManager.h"

#include <cstddef>
#include <string>

namespace Aura {

// §3.5：R-13 注入式配置（测试可注入 maxNodes / maxEpochs 制造负例）
struct DriverConfig {
    std::size_t maxFiles  = 1000;
    std::size_t maxEpochs = 4096;     // 绝对上限（R-5：不再用随 files 移动的界）
    std::size_t maxNodes  = 1000000;
};

class CompilationDriver {
public:
    explicit CompilationDriver(DiagnosticEngine& diag, DriverConfig cfg = DriverConfig());

    // 多文件：在 `loadAllScanned()` 之后、`topologicalLayersOn()` 之前调用（§3.7.2）。
    // 成功 ⇒ modules_ 里的 AST 已展开、scanUnits_ 已按展开后的 AST 刷新。
    bool run(ModuleManager& mgr);

    // 单文件（R-4）：在 Parser 解析成功之后、内置 .aurai 扫描 / Sema 之前调用（§3.7.1）。
    // 成功 ⇒ `program.decls` 已就地替换为展开后的声明。
    bool expandSingleFile(Program& program, const std::string& path, ModuleManager& mgr);

private:
    DiagnosticEngine& diag_;
    DriverConfig      cfg_;
};

} // namespace Aura
