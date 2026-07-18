# Aura 多文件并行编译计划

> 目标：以 `aurac <入口文件.aura>` 作为唯一入口，自动收集所有依赖模块，按拓扑顺序多线程并行编译，最终链接为可执行文件。  
> 入口文件由用户通过命令行指定，不固定名称；编译器会验证该文件及其依赖树中包含唯一的 `fun main(io: Io) ...` 函数作为程序入口。  
> 本计划仅关注多文件依赖收集、并行调度与代码生成，类型检查、GC 等细节已在其他计划中覆盖。

---

## 1. 命令行接口

```
aurac <entry.aura> -o <output>
```

- `<entry.aura>`：任意有效的 Aura 源文件路径，该文件必须（直接或间接）包含 `fun main(io: Io) throws` 函数。
- `<output>`：最终生成的可执行文件名（可选，默认取入口文件名去扩展名）。
- 编译器自动查找所有被 `import` 的用户模块并编译。

---

## 2. 整体流程

```
用户输入：aurac app.aura -o myprogram
    ↓
[1] 依赖扫描与模块图构建（单线程）
    ├── 完整解析每个 .aura 文件的 AST
    ├── 从 AST 提取 import 依赖 + has_main 标记
    ├── 递归加载用户模块源文件
    └── 输出：模块依赖图（DAG），AST 常驻 ModuleInfo
    ↓
[2] 循环依赖检测（单线程）
    └── 若存在环 → 报错退出
    ↓
[3] 拓扑排序与批次划分（单线程）
    └── 将模块按依赖深度分层，同层模块无依赖，可并行编译
    ↓
[4] 入口点验证（单线程，可并行前置）
    ├── 扫描所有模块的顶层函数，检查是否存在 `fun main(io: Io) throws` 
    ├── 要求唯一入口：若多个模块含 main，报错；若均无 main，报错
    └── 默认入口点为包含 main 的那个模块
    ↓
[5] 多线程编译（按层并行）
    ├── 每层内启动多个线程，每个线程编译一个模块（语义分析 + 代码生成）
    ├── 编译产物：模块头文件(.h) + 实现文件(.cpp)
    └── 层间通过 std::barrier 或 future 同步
    ↓
[6] 最终链接（单线程）
    └── 调用系统 C++ 编译器，编译所有 .cpp 并链接 libaura_rt
```

---

## 3. 核心数据结构

### 3.1 模块信息
```cpp
struct ModuleInfo {
    std::string source_path;           // 源文件路径
    std::string ns_name;               // C++ 命名空间
    std::string header_path;           // 要生成的头文件
    std::string impl_path;             // 要生成的实现文件
    std::vector<std::string> deps;     // 依赖的模块源路径
    int layer = -1;                    // 拓扑层号
    bool is_builtin = false;           // 是否为内置模块
    bool has_main = false;             // 是否定义了 main 函数
    // AST 常驻内存（一次解析，编译阶段直接使用）
    std::unique_ptr<Aura::Program> ast;
};
```

### 3.2 依赖图
```cpp
class DependencyGraph {
    std::unordered_map<std::string, ModuleInfo> modules;
public:
    void add_module(const std::string& path);
    void add_dependency(const std::string& from, const std::string& to);
    bool has_cycle() const;
    std::vector<std::vector<ModuleInfo*>> topological_layers();
};
```

---

## 4. 详细步骤

### 4.1 依赖扫描与图构建

> **设计修正**：不做两道解析。一次完整解析每个模块的 AST，从 AST 中同时提取 `import` 依赖和 `has_main` 标记。
> 两道解析会增加 I/O 和时间开销，而 AST 本身就很轻量，可以直接在内存中传递。

- 从命令行给定的入口文件开始。
- `parse_module(path)` → **完整解析** AST：
  - 遍历顶层声明，提取所有 `ImportDecl` 节点获取依赖。
  - 同时检查是否有 `FunDecl { name == "main" }` 标记 `has_main`。
  - 返回 `ParsedModule { AST, deps, has_main }`。
- 遇到 `import "path"` → 定位用户源文件，递归解析。
- 遇到 `import name`（无引号） → 标记为内置模块，不加载源文件。
- 在 `ModuleInfo` 中记录依赖关系，**AST 保持在 `ModuleInfo` 中**供后续编译阶段直接使用。
- 最终构建出完整的模块依赖图（包含入口文件）。

### 4.2 循环检测

- 对图运行 Tarjan 算法求强连通分量。
- 若存在节点数 >1 的强连通分量 → 输出循环路径并终止编译。

### 4.3 拓扑分层

- 使用 Kahn 算法进行 BFS 分层：
  - 初始层为所有入度为 0 的模块（通常为最底层的工具库）。
  - 后续层为所有依赖模块已分配在更早层的模块。
- 同一层中的模块没有直接或间接依赖，可以安全地并行编译。

### 4.4 入口点验证

> **修正**：`has_main` 在完整解析阶段（§4.1）已通过 AST 精确判定，不是靠轻量扫描的字样匹配。

- 遍历所有模块的 `has_main` 标记（已在解析阶段从 AST 中精确获取）：
  - 若没有模块包含 `fun main(io: Io) throws` → 报错："未找到程序入口"。
  - 若多个模块包含 main 定义 → 报错："重复定义程序入口"。
- 确定入口模块（以包含 main 的模块为准，通常即用户指定的入口文件中的模块）。
- 入口模块会在链接时提供 C++ 标准的 `int main(int argc, char** argv)` 函数，负责创建 `Io` 并调用 Aura 的 `main`。

### 4.5 多线程并行编译

> **线程安全要点**：`CodeGenerator` 持有可变状态（`indentLevel_`、`registeredTypes_`、`pendingMethods_` 等），**每个线程必须创建自己的 `CodeGenerator` 实例**。
> 依赖模块的符号类型信息通过它们已生成的头文件（`#include`）或内存中的共享符号表（只读）获取。

```cpp
for (auto& layer : layers) {
    std::vector<std::future<void>> tasks;
    for (auto* mod : layer) {
        if (mod->is_builtin) continue;
        tasks.push_back(std::async(std::launch::async, [mod]() {
            compile_single_module(mod);
        }));
    }
    for (auto& t : tasks) t.get(); // 等待本层完成
}
```

`compile_single_module` 内容：
- **直接使用 §4.1 已解析的 AST**，不需要重新解析。
- 创建独立的 `SemAnalyzer` + `CodeGenerator` 实例。
- 执行语义分析、类型检查。
- 生成 C++ 代码：
  - 头文件：类型定义、**模板函数完整实现**（plan7 §4 要求）。
  - 实现文件：`TypeDescriptor` 静态成员、非模板函数体、入口 `int main()` 包装。
- 对于入口模块，额外生成 C++ 标准的 `int main()` 函数。

同步要点：
- **每线程独立 `CodeGenerator`**，无共享可变状态。
- 同层模块编译完全独立，可安全并行。
- 层间通过 future 保证依赖模块的头文件已写出。

### 4.6 最终链接

收集所有生成的 `.cpp` 文件（包括入口模块的 `main` 包装），调用系统 C++ 编译器：
```bash
g++ -std=c++20 module1.cpp module2.cpp ... entry.cpp -laura_rt -o myprogram
```
`aurac` 通过 `std::system` 执行该命令，并检查返回状态。

---

## 5. 线程管理

- 使用 `std::async` 或线程池。
- 最大并发数 = `std::thread::hardware_concurrency()`，避免过度订阅。
- 同一层内任务数多于线程数时，由线程池自动排队。
- 层间严格顺序：所有当前层任务完成后才启动下一层。

---

## 6. 错误处理

- 模块编译失败（类型错误、语法错误等）：
  - 当前层剩余任务继续执行，以收集尽可能多的错误信息。
  - 层结束后若存在任何错误，终止编译，打印所有诊断信息。
- 内置模块缺失：
  - 最终链接阶段若 `#include` 找不到相应头文件，C++ 编译器报错，`aurac` 捕获并给出友好提示（如“未找到内置模块 path，请确保运行时库已安装”）。
- 循环依赖或入口点缺失立即报错并停止。

---

## 7. 后续优化方向（不在本次计划内）

- 增量编译：通过比较源文件时间戳和元数据，只重新编译修改过的模块及其下游依赖。
- 共享符号缓存：避免多次解析已编译模块的头文件。
- 分布式编译：将模块发送到远程构建服务器。

---

## 8. 实施时间线

| 阶段 | 内容 | 时间 |
|------|------|------|
| 1 | 完整 AST 解析、import 收集、模块图构建 | 第 1 周 |
| 2 | 循环依赖检测与拓扑分层 | 第 2 周 |
| 3 | 入口点验证、单模块编译框架（语义分析 + 代码生成，每线程独立 CodeGenerator） | 第 3–4 周 |
| 4 | 多线程调度与层间同步 | 第 5 周 |
| 5 | 最终链接集成、cpp 清理与端到端测试 | 第 6 周 |

该计划聚焦于 Aura 编译器多文件并行编译的完整流水线，与类型系统、GC 后端等计划互补，共同构成编译器项目的核心骨架。