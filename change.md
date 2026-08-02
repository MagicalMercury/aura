# change.md — 多文件编译链路并行化 + 生命周期修复 + pub 可见性 + 类型退化修复（实现清单）

> 对应 plan：`plan/multifile_compile_parallel.md`（2026-08-01 详细实施方案，已审查批准）
> 涉及文件：`src/main.cpp`、`src/Diag/Diagnostic.h`、`src/Diag/DiagnosticEngine.{h,cpp}`、
> `src/Sema/Symbol.h`、`src/Sema/SemAnalyzer.{h,cpp}`、`src/Sema/Checker/DeclChecker.cpp`、
> `src/Sema/Checker/ExprInfer.cpp`、`src/Sema/Checker/StmtChecker.cpp`、
> `READMEs/12-modules.md`、`READMEs/11-concurrency.md`、`READMEs/04-variables.md`、`READMEs/06-generics.md`
>
> 实施顺序（4.8）：C1 → C2 → C5 → C3 → C4 → C6 → C7 → README。每步独立编译验证。

---

## C1（P0）修复 SemAnalyzer 生命周期崩溃 — src/main.cpp

### C1.1 include 补充

位置：main.cpp L30-36。

```cpp
#include <cstdlib>
#include <filesystem>
#include <future>     // 新增：std::async / std::future
#include <iostream>
#include <map>        // 新增：moduleSemas / moduleDiags / cgResults
#include <memory>     // 新增：std::unique_ptr
#include <sstream>
#include <string>
#include <string_view>
#include <thread>     // 新增：hardware_concurrency
#include <vector>
```

### C1.2 compileMultiFile 第 5 步重构（Sema 层并行 + moduleSemas 持有）

位置：main.cpp L240-271（原"5. 语义分析各模块"整块替换）。

**修改前**（要点）：`Aura::SemAnalyzer sema(diag)` 为循环局部对象（L246），循环结束即销毁，`typeStore_` 释放而 AST 节点 `inferredType` 仍指向其中对象 → CodeGen 阶段悬垂崩溃。

**修改后**：

```cpp
    // 5. 语义分析各模块（按拓扑层；同层并行，层间串行）
    //    - 每模块独立 SemAnalyzer + 独立 DiagnosticEngine（任务线程零共享写，无锁）
    //    - 主线程按层内顺序 merge 诊断（错误输出顺序确定）
    //    - moduleSemas 持有各模块 SemAnalyzer 直至本函数结束（P0 修复）：
    //      保证模块 AST 节点 inferredType 引用的 typeStore_ 对象存活到 CodeGen 阶段
    std::map<std::string, std::unique_ptr<Aura::DiagnosticEngine>> moduleDiags;  // key = mod->sourcePath
    std::map<std::string, std::unique_ptr<Aura::SemAnalyzer>>     moduleSemas;

    // 单个模块的 Sema 任务（依赖 exports 来自前序层，层间 join 保证可见）
    // 返回 unique_ptr<SemAnalyzer>：moduleSemas 的 map 写入只在主线程进行（修复并行 data race）
    auto runSemaModule = [&](Aura::ModuleInfo* mod, Aura::DiagnosticEngine& modDiag)
        -> std::unique_ptr<Aura::SemAnalyzer> {
        auto sema = std::make_unique<Aura::SemAnalyzer>(modDiag);
        // 注入依赖模块的导出表
        for (auto& depPath : mod->deps) {
            auto depIt = mgr.modules().find(depPath);
            if (depIt == mgr.modules().end()) continue;
            std::string alias;
            for (auto& imp : mod->imports) {
                if (imp.path == depPath) { alias = imp.alias; break; }
            }
            sema->importExports(alias, depIt->second.exports);
        }
        (void)sema->analyze(*mod->ast);
        mod->exports = sema->extractExports();
        return sema;   // 交回主线程统一存入 moduleSemas
    };

    for (auto& layer : layers) {
        // 收集本层任务（非 builtin 且 AST 非空）；每模块独立 diag 并绑定源码视图
        std::vector<Aura::ModuleInfo*> tasks;
        for (auto* mod : layer) {
            if (mod->isBuiltin) continue;
            if (!mod->ast) continue;
            tasks.push_back(mod);
            auto modDiag = std::make_unique<Aura::DiagnosticEngine>();
            modDiag->setSourceView(Aura::readFile(mod->sourcePath));
            modDiag->setFileName(mod->sourcePath);
            moduleDiags[mod->sourcePath] = std::move(modDiag);
        }

        int n = parallelJobs(opts, tasks.size());   // C5，见下
        if (n < 2) {
            // 串行路径（等价于原逻辑）；moduleSemas 写入在主线程
            for (auto* mod : tasks)
                moduleSemas[mod->sourcePath] = runSemaModule(mod, *moduleDiags[mod->sourcePath]);
        } else {
            std::vector<std::future<std::unique_ptr<Aura::SemAnalyzer>>> futs;
            for (auto* mod : tasks) {
                // 主线程先绑定 diag 引用，任务体内不再触碰 moduleDiags（map 只读也避免）
                Aura::DiagnosticEngine& modDiag = *moduleDiags[mod->sourcePath];
                futs.push_back(std::async(std::launch::async,
                    [&, mod, &modDiag] { return runSemaModule(mod, modDiag); }));
            }
            // 主线程按任务序取结果写入 moduleSemas（map 写仅发生在主线程，无 data race）
            for (size_t i = 0; i < futs.size(); ++i)
                moduleSemas[tasks[i]->sourcePath] = futs[i].get();   // 异常在此重抛（由 main 捕获）
        }
        // 主线程按层内顺序合并诊断（总数守恒，不截断）
        for (auto* mod : tasks) diag.mergeFrom(*moduleDiags[mod->sourcePath]);
    }
    if (diag.hasErrors()) {
        std::cerr << "Compilation failed with " << diag.errorCount() << " error(s):\n";
        diag.print(std::cerr);
        return 1;
    }
```

> 注意：`moduleDiags` / `moduleSemas` 声明在 compileMultiFile 函数体顶部（原 L240 之前），供第 7 步 CodeGen 后析构（作用域覆盖整个函数）。

---

## C2（P1）Diagnostic 多模块错误归属 + 不截断合并

### C2.0 DiagnosticEngine::source_ 从 string_view 改 std::string（修复 setSourceView 悬垂）

**背景**：C1.2 / C4.1 以 `setSourceView(Aura::readFile(...))` 传入**临时 std::string**，
而 setSourceView 存储的是 `std::string_view` → 临时对象语句结束即析构，source_ 悬垂，
print → getSourceLine 读未定义内存（UB）。根因修复：source_ 按值持有源码（拷贝语义），
任何入参生命周期（临时 / 局部 / 常量）均安全。

位置：DiagnosticEngine.h（`<string>` 已由 Diagnostic.h include 传递提供，无需新增；仅改成员类型；setSourceView 签名与实现不变）：

```cpp
    void setSourceView(std::string_view source) { source_ = source; }   // 不变：现为 string 拷贝赋值
    ...
private:
    std::string source_;    // 修改前：std::string_view source_;（悬垂风险）
```

位置：DiagnosticEngine.cpp getSourceLine（L61-77）substr 段微调
（std::string::substr 返回 std::string，无 remove_suffix；先转 string_view 再处理）：

```cpp
// 修改前
            auto s = source_.substr(start, i - start);
            // 去尾 \r
            if (!s.empty() && s.back() == '\r') s.remove_suffix(1);
            return std::string(s);

// 修改后
            auto sv = std::string_view(source_).substr(start, i - start);
            // 去尾 \r
            if (!sv.empty() && sv.back() == '\r') sv.remove_suffix(1);
            return std::string(sv);
```

> 影响：现有安全调用点（main.cpp L95 / L423 传长寿命局部 string）行为不变；
> C1.2 / C4.1 的临时 string 入参也安全。每模块多一次源码拷贝，编译器场景可忽略。

### C2.1 Diagnostic.h 增加 file 字段

位置：Diagnostic.h L79-86（`file` 追加在结构体**末尾**，避免破坏现有聚合初始化）。

```cpp
struct Diagnostic {
    DiagSeverity severity;
    DiagCode     code = DiagCode::None;
    SourceRange  range;
    std::string  message;
    std::string  fixHint;         // 可选的修复建议
    std::vector<Diagnostic> notes; // 附注（暂未使用，预留）
    std::string  file;            // 所属文件（report 时快照 fileName_；多模块并行诊断归属）
};
```

### C2.2 DiagnosticEngine.cpp report 快照 fileName_

位置：DiagnosticEngine.cpp L17 之后。

```cpp
    diags_.push_back(diag);
    diags_.back().file = fileName_;   // 快照所属文件（并行任务 merge 后能定位模块）
```

### C2.3 DiagnosticEngine.cpp print 用 diag.file 优先

位置：DiagnosticEngine.cpp L90-93 替换。

```cpp
        // --- 文件位置 ---
        const std::string& f = !diag.file.empty() ? diag.file : fileName_;
        if (!f.empty())
            os << "  --> " << f << ":" << diag.range.line << ":" << diag.range.colStart << "\n";
        else
            os << "  --> line " << diag.range.line << ":" << diag.range.colStart << "\n";
```

### C2.4 DiagnosticEngine.h 新增 mergeFrom 声明

位置：DiagnosticEngine.h L36（`print` 声明之后）。

```cpp
    void print(std::ostream& os) const;
    // 将 other 的诊断并入本引擎（多线程任务结果汇总；不截断，保留全部已记录错误）
    void mergeFrom(const DiagnosticEngine& other);
    void reset();
```

### C2.5 DiagnosticEngine.cpp 新增 mergeFrom 实现

位置：reset（L123-128）之前。

```cpp
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
```

---

## C5（P1）`-j N` 开关与并行度计算 — src/main.cpp

### C5.1 CliOptions 加字段

位置：main.cpp L41-50。

```cpp
    bool        sizeOptimize = false; // -s：-Os 体积优化
    int         jobs         = 0;     // -j N：并行任务数（0 = 自动）
};
```

### C5.2 parseArgs 加解析

位置：main.cpp L52-75，`-s` 分支之后。

```cpp
        } else if (arg == "-s") {
            opts.sizeOptimize = true;
        } else if (arg == "-j" && i + 1 < args.size()) {
            opts.jobs = std::atoi(args[++i].data());
        } else if (!arg.empty() && arg[0] != '-') {
```

### C5.3 新增 parallelJobs 辅助

位置：gccFlags（L80-84）之后。

```cpp
// ============================================================
// 并行度计算：-j 指定 / 硬件并发，与任务数取 min；任务数 <2 → 1（串行）
// ============================================================
static int parallelJobs(const CliOptions& opts, size_t taskCount) {
    int n = opts.jobs > 0 ? opts.jobs
            : (int)std::thread::hardware_concurrency();
    if (n < 1) n = 1;
    if ((size_t)n > taskCount) n = (int)taskCount;
    return n;
}
```

---

## C4（P1）CodeGen 全模块并行 — src/main.cpp

### C4.1 第 7 步重构（CodeGen 并行 + 主线程按层序汇总）

位置：main.cpp L281-343（原"7. 收集所有生成的 .cpp 文件路径" + "7. 按层编译各模块"整块替换）。

```cpp
    // 7. 收集所有生成的 .cpp 文件路径
    std::vector<std::string> allCppPaths;

    // 7. 并行生成各模块 C++（无层间依赖；CodeGen 只读自身 AST + 依赖命名空间信息）
    struct CgResult {
        std::string cppPath;
        std::unique_ptr<Aura::DiagnosticEngine> diag;
    };

    // 单个模块的 CodeGen 任务（含写 .h/.cpp；路径互异，无写竞争）
    auto runCgModule = [&](Aura::ModuleInfo* mod) -> CgResult {
        CgResult res;
        auto modDiag = std::make_unique<Aura::DiagnosticEngine>();
        modDiag->setSourceView(Aura::readFile(mod->sourcePath));
        modDiag->setFileName(mod->sourcePath);

        // 构建 import 信息列表（只读 mgr.modules()，并发读安全）
        std::vector<Aura::CodeGenImport> cgImports;
        for (auto& imp : mod->imports) {
            Aura::CodeGenImport ci;
            ci.path      = imp.path;
            ci.alias     = imp.alias;
            ci.isBuiltin = imp.isBuiltin;
            if (!imp.isBuiltin) {
                auto it = mgr.modules().find(imp.path);
                if (it != mgr.modules().end()) {
                    ci.nsName   = it->second.nsName;
                    ci.modName  = it->second.moduleName;
                }
            } else {
                ci.modName = imp.path;  // 内置模块名即命名空间键
            }
            cgImports.push_back(ci);
        }

        // 代码生成
        Aura::CodeGenerator cg(*modDiag);
        auto unit = cg.generate(*mod->ast, mod->moduleName, cgImports, mod->nsName);

        // 写出头文件
        std::string hdrPath = outDir + "/" + mod->moduleName + ".aura.h";
        {
            std::ostringstream hdr;
            hdr << unit.header;
            Aura::writeFile(hdrPath, hdr.str());
        }
        // 写出实现文件
        std::string cppPath = outDir + "/" + mod->moduleName + ".aura.cpp";
        {
            std::ostringstream implCpp;
            implCpp << "#include \"" << mod->moduleName << ".aura.h\"\n";
            implCpp << unit.impl;
            if (!unit.footer.empty()) implCpp << "\n" << unit.footer;
            Aura::writeFile(cppPath, implCpp.str());
        }
        res.cppPath = cppPath;
        res.diag = std::move(modDiag);
        return res;
    };

    // 收集所有非内置模块任务
    std::vector<Aura::ModuleInfo*> cgTasks;
    for (auto& layer : layers)
        for (auto* mod : layer)
            if (!mod->isBuiltin) cgTasks.push_back(mod);

    std::map<std::string, CgResult> cgResults;  // key = mod->sourcePath
    int cgN = parallelJobs(opts, cgTasks.size());
    if (cgN < 2) {
        for (auto* mod : cgTasks) cgResults[mod->sourcePath] = runCgModule(mod);
    } else {
        std::vector<std::future<CgResult>> futs;
        for (auto* mod : cgTasks)
            futs.push_back(std::async(std::launch::async,
                [&, mod] { return runCgModule(mod); }));
        for (size_t i = 0; i < futs.size(); ++i)
            cgResults[cgTasks[i]->sourcePath] = futs[i].get();
    }

    // 主线程按"层序 + 层内序"收集 cppPath + 汇总诊断 + 打印（顺序确定）
    for (auto& layer : layers) {
        for (auto* mod : layer) {
            if (mod->isBuiltin) continue;
            auto& res = cgResults[mod->sourcePath];
            diag.mergeFrom(*res.diag);
            if (!res.cppPath.empty()) allCppPaths.push_back(res.cppPath);
            std::cerr << "  compiled: " << mod->moduleName << ".aura\n";
        }
    }
    if (diag.hasErrors()) {
        std::cerr << "CodeGen errors:\n";
        diag.print(std::cerr);
        return 1;
    }
```

> 行为变化：原"CodeGen 错误立即 return 1"改为并行完成后统一判断（可能多生成几个 .cpp，无副作用）。原 L312-316 的 `if (diag.hasErrors()) { ... return 1; }` 删除。

---

## C6（P2）pub 模块级导出策略

### C6.1 Symbol.h 加 isImported 字段

位置：Symbol.h L50-53。

```cpp
    // ——— 导入符号 ———
    std::string belongsToModule;  // 非空 = 来自此模块的导入
    bool isImported = false;      // 来自 import 注入（永远不透传 re-export）
    bool isPublic = true;  // 实际由 DeclChecker 显式赋值；Global 自有符号默认公开，模块级策略在 extractExports 判定
```

### C6.2 SemAnalyzer.h 加 hasAnyPub_ 成员

位置：SemAnalyzer.h L263-264（ioSync_ 之后）。

```cpp
    // ============ #config 配置 ============
    bool ioSync_ = false;       // #io.sync = true → 同步模式
    bool hasAnyPub_ = false;    // 模块级 pub 策略：文件中出现任一 pub 声明 → 仅导出带 pub 的
```

### C6.3 SemAnalyzer.cpp importFuncSymbol 标记 isImported

位置：SemAnalyzer.cpp L692-694。

```cpp
    sym.type   = f.returnType ? f.returnType->clone() : nullptr;
    sym.throws = f.throws;
    sym.isImported = true;   // 导入符号：永远不透传（C6-1）
    symtab_.defineGlobal(std::move(sym));
```

### C6.4 SemAnalyzer.cpp importExports types 循环标记 isImported

位置：SemAnalyzer.cpp L698-704。

```cpp
    for (auto& [name, type] : exports.types) {
        Symbol sym;
        sym.kind = SymKind::TypeAlias;
        sym.name = alias.empty() ? name : (alias + "." + name);
        sym.type = type->clone();
        sym.isImported = true;   // 导入符号：永远不透传（C6-1）
        symtab_.defineGlobal(std::move(sym));
    }
```

### C6.5 SemAnalyzer.cpp extractExports 双重过滤

位置：SemAnalyzer.cpp L740-741。

```cpp
        scope->forEach([&](const std::string& name, const Symbol& sym) {
            if (sym.isImported) return;               // import 不透传（C6-1）
            if (hasAnyPub_ && !sym.isPublic) return;  // 模块级策略：有 pub 仅导出带 pub 的（C6-2）
            switch (sym.kind) {
```

### C6.6 DeclChecker.cpp declareDecl 入口：pub import 报错 + 置 hasAnyPub_

位置：DeclChecker.cpp L60-61（declareDecl 函数开头）。

```cpp
void SemAnalyzer::declareDecl(const Decl& decl) {
    // 模块级 pub 策略：pub 仅可修饰声明（type/fun/方法/构造函数）
    // pub import 为错误（C6-3）；config 语法待定，不参与策略（C6-4）
    if (decl.isPublic && dynamic_cast<const ImportDecl*>(&decl)) {
        error(decl, "pub cannot be applied to import declarations");
    } else if (decl.isPublic && !dynamic_cast<const ConfigDecl*>(&decl)) {
        hasAnyPub_ = true;
    }
    if (auto* t = dynamic_cast<const TypeDecl*>(&decl)) {
```

---

## C7（P2）类型退化修复 — Sema 兜底提前报错（覆盖 D1-D5）

### C7-1 Generator 按 typeName 解析返回元素类型

位置：SemAnalyzer.cpp L189-190。

**修改前**：

```cpp
        case ReturnTypeInfo::Kind::Generator:
            return IterSemType::make(intType());   // 硬编码 int，忽略 ret.typeName
```

**修改后**：

```cpp
        case ReturnTypeInfo::Kind::Generator:
            return IterSemType::make(semTypeFromAuraName(ret.typeName));
```

> range 注册为 Generator("int")（BuiltinRegistry.h:302-304）→ 行为不变；其他 Generator 诚实解析（未命中 → ErrorSemType）。

### C7-2a elemTypeOf 增加 OptionalSemType 分支

位置：SemAnalyzer.cpp L91（GenericSemType 分支之前插入）。

```cpp
    if (auto* os = dynamic_cast<const OptionalSemType*>(iterType))
        return os->elementType ? os->elementType->clone() : ErrorSemType::make();
```

### C7-2b inferMethodCall builtin 分支统一兜底报错

位置：ExprInfer.cpp L269-272。

**修改前**：

```cpp
        if (auto* entry = BuiltinRegistry::get().findMethod(typeKey, e.method, (int)e.args.size())) {
            auto& ret = entry->returns;
            return semTypeFromBuiltinReturn(ret, objType.get());
        }
```

**修改后**：

```cpp
        if (auto* entry = BuiltinRegistry::get().findMethod(typeKey, e.method, (int)e.args.size())) {
            auto& ret = entry->returns;
            // 返回形状依赖元素类型的调用（Optional<T> / Generic("channel")）：
            // 元素类型不可知 → 报错引导显式类型标注（不改返回 fallback，仅诊断）
            bool needsElem = (ret.kind == ReturnTypeInfo::Kind::Optional)
                || (ret.kind == ReturnTypeInfo::Kind::Generic
                    && ret.typeName != "[T]" && ret.typeName != "string");
            if (needsElem) {
                auto elem = elemTypeOf(objType.get());
                if (dynamic_cast<const ErrorSemType*>(elem.get())) {
                    error(e, "cannot infer element type of '" + typeKey
                           + "'; add explicit type annotation (e.g. " + typeKey + "<int>)");
                }
            }
            return semTypeFromBuiltinReturn(ret, objType.get());
        }
```

> 判据：`Generic("[T]")`（slice/front/back/pop）与 `Generic("string")`（concat）不依赖元素提取，排除；`Optional("T")`（sync.Channel.receive）、`Generic("channel")`（channel.receive）检查。`Generic("T")`（Optional.unwrap）保留在判据但实际不触发（objType 为 OptionalSemType → typeKey 空，不进此分支）。

### C7-3 修复 Generic("channel") 分支直接 return ErrorSemType（D1 根因）

位置：SemAnalyzer.cpp L219-224。

**修改前**：

```cpp
            // fallback == "channel" → 返回 channel 的元素类型
            if (ret.typeName == "channel") {
                // channel<T> 的元素类型：从 objType 提取
                // GenericSemType("channel") 无元素类型信息，回退到 error
                return ErrorSemType::make();
            }
```

**修改后**：

```cpp
            // fallback == "channel" → 返回 channel 的元素类型（D1 修复：原实现直接 return ErrorSemType）
            if (ret.typeName == "channel") {
                return elemTypeOf(objType);
            }
```

> 有标注 channel（resolvedName 已填）→ elemTypeOf 正确提取；无标注 → elemTypeOf 返回 Error，由 C7-2b 报错兜底。

### C7-4 空列表 / 列表元素为 Error 兜底报错（D3/D5）

位置：StmtChecker.cpp 顶部（L3 `namespace Aura {` 之后）新增文件局部辅助：

```cpp
// 列表元素类型链递归检测 ErrorSemType（空列表 [] / 嵌套 [[]] → 元素类型不可知）
// 仅检查 List 链，不检查整体 Error / Optional（避免误伤 record 方法调用等放行路径）
static bool listContainsError(const SemType* t) {
    auto* l = dynamic_cast<const ListSemType*>(t);
    if (!l) return false;
    if (!l->elementType) return true;
    return dynamic_cast<const ErrorSemType*>(l->elementType.get())
        || listContainsError(l->elementType.get());
}
```

checkLetDecl（StmtChecker.cpp L53-60）修改：

**修改前**：

```cpp
    auto inferredType = decl.initializer ? inferExpr(*decl.initializer) : ErrorSemType::make();
    if (decl.type) {
        auto declaredType = resolveType(*decl.type);
        if (!isAssignable(*declaredType, *inferredType)) {
            error(decl, "type mismatch: cannot assign '" + inferredType->toString() + "' to '" + declaredType->toString() + "'");
        }
        inferredType = std::move(declaredType);
    }
```

**修改后**：

```cpp
    auto inferredType = decl.initializer ? inferExpr(*decl.initializer) : ErrorSemType::make();
    if (decl.type) {
        auto declaredType = resolveType(*decl.type);
        if (!isAssignable(*declaredType, *inferredType)) {
            error(decl, "type mismatch: cannot assign '" + inferredType->toString() + "' to '" + declaredType->toString() + "'");
        }
        inferredType = std::move(declaredType);
    } else if (listContainsError(inferredType.get())) {
        // 无标注 + 列表元素类型不可知（空列表 / 嵌套空列表）→ 报错引导标注（D3/D5）
        error(decl, "cannot infer element type from initializer; add explicit type annotation (e.g. let x: [int] = [])");
    }
```

checkConstDecl（StmtChecker.cpp L80-87）同样修改：

```cpp
    auto inferredType = decl.initializer ? inferExpr(*decl.initializer) : ErrorSemType::make();
    if (decl.type) {
        auto declaredType = resolveType(*decl.type);
        if (!isAssignable(*declaredType, *inferredType)) {
            error(decl, "type mismatch in const: expected '" + declaredType->toString() + "', got '" + inferredType->toString() + "'");
        }
        inferredType = std::move(declaredType);
    } else if (listContainsError(inferredType.get())) {
        error(decl, "cannot infer element type from initializer; add explicit type annotation (e.g. let x: [int] = [])");
    }
```

### C7-5 泛型实参数量不匹配报错（D4）

位置：DeclChecker.cpp L194-199（resolveType NamedType 用户泛型分支）。

**修改前**：

```cpp
        if (!n->typeArgs.empty()) {
            auto* sym = symtab_.lookup(fullName);
            if (sym && sym->kind == SymKind::TypeAlias && !sym->typeParams.empty()) {
                // 用户自定义泛型：applyTypeArgs 替换形参为实参 + materializeCanonicalName
                result = applyTypeArgs(std::move(result), *sym, n->typeArgs);
                materializeCanonicalName(result, *n);
            } else {
```

**修改后**：

```cpp
        if (!n->typeArgs.empty()) {
            auto* sym = symtab_.lookup(fullName);
            if (sym && sym->kind == SymKind::TypeAlias && !sym->typeParams.empty()) {
                // 泛型实参数必须与声明一致（缺省/多余均报错，D4）
                if (n->typeArgs.size() != sym->typeParams.size()) {
                    error(*n, "type '" + n->name + "' expects "
                          + std::to_string(sym->typeParams.size())
                          + " type argument(s), got " + std::to_string(n->typeArgs.size()));
                }
                // 用户自定义泛型：applyTypeArgs 替换形参为实参 + materializeCanonicalName
                result = applyTypeArgs(std::move(result), *sym, n->typeArgs);
                materializeCanonicalName(result, *n);
            } else {
```

### C7-6 channel for-in 无标注兜底报错

位置：StmtChecker.cpp L161-174 checkForStmt，`auto iterType = inferExpr(*stmt.iterable);` 之后插入：

```cpp
    auto iterType = inferExpr(*stmt.iterable);
    // channel 类（GenericSemType）无显式类型标注 → 元素类型不可知 → 报错引导标注（C7-6）
    if (dynamic_cast<const GenericSemType*>(iterType.get())
        && dynamic_cast<const ErrorSemType*>(elemTypeOf(iterType.get()).get())) {
        error(*stmt.iterable, "cannot infer element type of channel; add explicit type annotation (e.g. let ch: channel<int> = channel(10))");
    }
    // 迭代类型默认合法（运行时检查），这里只确保表达式无错误
    ScopedValue<int> loopGuard(loopDepth_, loopDepth_ + 1);
```

---

## README 补充

### READMEs/12-modules.md 新增 12.4（追加于 12.3 之后）

````markdown
## 12.4 可见性（pub）

可见性按**模块级策略**控制导出：

- **文件中没有任何 `pub` 关键字** → 全部顶层声明默认导出。
- **文件中存在 `pub` 关键字** → 仅带 `pub` 的声明导出，其余为模块私有。

```aura
// utils.aura —— 存在 pub，半开放模式
pub type Pair<A, B> = { first: A, second: B }
pub fun zip(a: <A>, b: <B>) -> Pair<A, B> { return Pair(a, b) }
fun helper() { ... }              // 私有：仅 utils.aura 内可用

// helpers.aura —— 无 pub，全导出模式
fun helper1() { ... }             // 自动公开
fun helper2() { ... }             // 自动公开
```

**规则：**

1. 导出对象：顶层 `type`、`fun`、方法、构造函数。
2. 被导出的 `type` 的方法与字段随类型一并公开（Go 风格，无字段/方法级 `pub`）。
3. **import 不透传**：模块 B `import "a.aura"` 后，B 自身不导出 A 的符号；模块 C `import "b.aura"` 无法间接访问 A 的符号。C 若需要 A 的符号，直接 `import "a.aura"` 即可（import 按路径直达，无层级传递）。
4. `pub` 仅可修饰声明（`type` / `fun` / 方法 / 构造函数）；`pub import` 为错误。
````

### READMEs/11-concurrency.md 补充 channel 标注规则（§11.2 与 §11.7 的"创建"表格旁）

````markdown
> **元素类型必须显式标注**：`channel<T>` / `sync.Channel<T>` 的元素类型无法从构造表达式
> `channel(cap)` 推断，编译器不会猜测（v1 不做从 `send()` 反推）。
> 未标注时，`receive()` / `for val in ch` 会在编译期报错：

```aura
let ch: channel<int> = channel(10)   // 必须标注元素类型
let v = ch.receive()                 // 合法：从标注推断 v: Optional<int>
// let ch = channel(10)              // 编译错误：无法推断元素类型
```
````

### READMEs/04-variables.md 类型推断小节补充空列表规则

````markdown
**空列表必须标注元素类型**（编译器无法从 `[]` 推断）：

```aura
let a: [int] = []        // 必须标注
// let a = []            // 编译错误：无法推断元素类型
let b = [1, 2, 3]        // 合法：从元素推断
```

嵌套同理：`let m: [[int]] = [[]]`。
````

### READMEs/06-generics.md 补充泛型实参规则

````markdown
**泛型类型实参数必须与声明一致**（缺省或多余均报错）：

```aura
type Pair<A, B> = { first: A, second: B }
let p: Pair<int, string> = ...   // 合法
// let p: Pair<int> = ...        // 编译错误：expects 2 type argument(s), got 1
```
````

---

## 验证步骤

1. `cmake --build build` 无 error（aurac 编译自身）。
2. P0 验证：`.\build\aurac.exe example/used/test_import.aura --cpp example/used` → 不再崩溃；g++ 链接运行输出正确。
3. 并行一致性：同一项目 `-j1` / `-j2` / 默认三档编译，exe 输出一致。
4. 回归：`example/test.aura`（K1-K29，单文件）+ used/ 多文件全部编译运行。
5. 负向测试（4.7 第 6 项）：无标注 channel receive / 空列表 / `Pair<int>` / 无标注 channel for-in → 均报错。
6. README 文档审查。
