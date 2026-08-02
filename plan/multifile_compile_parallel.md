# 4. Plan：多文件编译链路并行化 + 生命周期 bug 修复 + pub 可见性完善 + 类型退化修复

## 4.1 标题与元数据

- **Plan 标题**：多文件编译链路优化（并行化）+ 悬垂指针崩溃修复 + pub 模块级导出策略 + 类型退化修复
- **作者**：Agent / 提出者：用户
- **日期**：2026-08-01
- **关联模块**：
  - `src/main.cpp`（compileMultiFile 主流程 L210-389、CliOptions L41-50、parseArgs L52-75）
  - `src/Diag/Diagnostic.h`（L79-86）、`src/Diag/DiagnosticEngine.{h,cpp}`
  - `src/Sema/Symbol.h`（L29-53）
  - `src/Sema/SemAnalyzer.h`（L38 importExports、L78-80 collectGenericMapping）
  - `src/Sema/SemAnalyzer.cpp`（L85-101 elemTypeOf、L189-199 Generator/Optional 分支、L682-718 import 注入、L736-760 extractExports）
  - `src/Sema/Checker/DeclChecker.cpp`（L60-169 declareDecl）
  - `src/Sema/Checker/ExprInfer.cpp`（L248-297 inferMethodCall builtin 分支）
  - `src/Module/ModuleManager.h`（只读使用：ModuleInfo L55-76、topologicalLayers L99）
  - `src/CodeGen/CodeGen.h`（只读使用：generate 签名 L102-105）
  - `READMEs/12-modules.md`（文档补充 12.4）
- **关联 Issue**：TODO.txt 待新增（本 plan 审查通过后写入）

## 4.2 目标

1. **P0**：修复多文件模式下模块级 `SemAnalyzer` 生命周期短于 AST 节点 `inferredType` 引用导致的悬垂指针崩溃（`isHeapSemType` dynamic_cast 0xC0000005）。
2. **P1**：Sema 同层并行 + CodeGen 全模块并行，加速多文件编译；`-j N` 可调，模块数 <2 自动串行。
3. **P1**：多文件错误输出带模块文件名；错误合并不截断（maxErrors 每模块防级联，总数守恒）。
4. **P2**：pub 模块级导出策略（全文件无 pub → 全导出；有 pub → 仅导出带 pub 的）；import 不透传；`pub import` 报错。
5. **P2**：修复两处"未知类型退化成 int"（Generator 返回元素硬编码 int；Optional 元素提取失败静默假装 int → 改为显式报错引导类型标注）。

## 4.3 现状摘要

### 多文件编译流程（compileMultiFile，main.cpp:210-389）

1. `mgr.loadBuiltinAurai()` 加载 io.aurai（串行，main.cpp:215）
2. `mgr.loadAll(entryPath)` 串行递归解析（main.cpp:218）
3. `mgr.hasCycle()` 三色 DFS（main.cpp:225）
4. `mgr.topologicalLayers()` Kahn BFS 分层（main.cpp:231；ModuleManager.h:98 注释"同层可并行编译"）
5. **Sema 按层串行**（main.cpp:240-266）：`Aura::SemAnalyzer sema(diag)` 循环内局部变量 → 注入依赖 exports（L248-259）→ analyze（L261）→ extractExports（L264）
6. **CodeGen 按层串行**（main.cpp:284-343）：每模块 `CodeGenerator cg(diag)` + `cg.generate`（L310）→ 写 .h/.cpp（L319-337）→ 收集 allCppPaths（L339）→ "compiled:" 输出（L341）
7. g++ 链接（main.cpp:358-373）

### 关键问题（已定位）

- **P0 崩溃**：main.cpp:246 `SemAnalyzer sema` 为循环局部对象，循环结束（L266）即销毁；其 `typeStore_`（ExprInfer.cpp:37 中 `inferredType` 指向对象的持有者）随之释放，但模块 AST 节点 `inferredType` 仍指向其中对象。第 6 步 CodeGen（main.cpp:310 `cg.generate(*mod->ast, ...)`）访问悬垂 `inferredType` → `isHeapSemType` 的 dynamic_cast（ExprGen.cpp:17 等）RTTI 崩溃。单文件模式（main.cpp:122 sema 作用域覆盖 CodeGen L144）无此问题。
- **并行障碍**：DiagnosticEngine（DiagnosticEngine.cpp:10-26）diags_/errorMessages_/counts 无锁；fileName_ 单值无法表达多模块错误归属。**已审计的并行安全项**：CodeGenerator 可变状态全为实例成员；SemAnalyzer 每模块独立实例；BuiltinRegistry 编译期只读（tryLoadAurai 仅在 loadAll 阶段调用）；写文件路径互异；`std::cerr` 集中在主线程。
- **pub**：import 注入符号 `isPublic` 默认 true（Symbol.h:52）→ 透传 re-export；extractExports（SemAnalyzer.cpp:741）一律只导出 pub，无模块级策略；`pub import` 被 parser 接受（DeclParser.cpp:10 统一前缀）但无处消费。
- **类型退化（ErrorSemType 静默产生的全貌，D1-D8）**：
  - **D1（有标注也退化）**：协程 `channel.receive()` 走 `Generic("channel")` 分支**直接 return ErrorSemType，不看 resolvedName**（SemAnalyzer.cpp:220-224）→ 即使标注 `channel<int>` 也退化。比 Optional 分支更严重。
  - **D2**：sync.Channel 无标注 `receive()` → Optional 分支 elemTypeOf 失败 fallback int（SemAnalyzer.cpp:197）。
  - **D3**：空列表无标注 `let a = []` → `ListSemType{elementType=Error}`（ExprInfer.cpp:73-78）→ CodeGen 生成 `Array</* error_type */>*` 非法 C++。测试代码空列表全带标注（`let arr: [int] = []`），掩盖了它。
  - **D4**：泛型实参缺省 `Pair<int>`（少给 B）→ applyTypeArgs 用 min 截断不报错（SemAnalyzer.cpp:477）→ B 残留 `GenericSemType("B")` 无 resolvedName → mapSemType → `"auto"`（TypeMap.cpp:236）→ `Array<auto>*` / `Optional<auto>*` 非法 C++。
  - **D5**：嵌套空列表 `[[]]` → 内层 Error 传染到外层 `List<List<Error>>`。
  - **D6**：用户自定义类型（RecordSemType）方法调用 `obj.method()` → inferMethodCall 只给 Prim/List/内置 Generic 设 typeKey（ExprInfer.cpp:253-261），record 无 typeKey → L295-296 放行 Error → `let v = obj.method()` 生成 `auto`（侥幸可编译），但 `isHeapSemType(Error)=false` → 返回 GC 指针时不包 GcRootHandle → GC 悬垂。**报错不可行**（所有 record 方法调用都触发）；归入 newIssue 第 7 条（sema 反推/方法返回类型传播），本 plan 不做。
  - **D7**：泛型函数调用返回 `GenericSemType("T")` 未实例化（inferCall 只 clone 不替换）→ `auto` 兜底；泛型函数 CodeGen 实例化支持度待审计，暂列已知限制。
  - **D8**：`T | None` 联合中 None → resolveType(None)=Error 变体（SemAnalyzer.cpp:59）→ `UnionSemType{Error}`；mapSemType 无 Union 分支 → `"/* unknown_semtype */"`（TypeMap.cpp:238）。当前 inferExpr 无产生 UnionSemType 的路径，仅显式标注走 mapType（variant 映射），理论风险，暂列已知限制。
- **CodeGen 对 ErrorSemType 的硬约束**（C7 决策依据）：`mapSemType(Optional<Error>)` = `aura_rt::Optional</* error_type */>*` 非法 C++（TypeMap.cpp:209-213）；`isHeapSemType(ErrorSemType)=false`（ExprGen.cpp:17）→ 整体 Error 时 `let v = ch.receive()` 失去 GcRootHandle 根注册 → GC 悬垂。**故 fallback 不得改为 ErrorSemType**；D1-D5 一律改为 Sema 兜底**提前报错**（用户决策：只要 CodeGen 会出现 bug 的都应在 Sema 报错），CodeGen 不运行 → 防御值安全。

## 4.4 变更方案

### C1（P0）修复 SemAnalyzer 生命周期崩溃 — main.cpp

- **位置**：compileMultiFile 第 5 步（main.cpp:240-266），函数顶部声明容器。
- **接口契约**：
  - 新增局部容器：`std::map<std::string, std::unique_ptr<Aura::SemAnalyzer>> moduleSemas;`（键 = mod->path）
  - 每模块 `auto sema = std::make_unique<Aura::SemAnalyzer>(modDiag);`，调用改 `sema->importExports(...)` / `sema->analyze(...)` / `sema->extractExports()`，末尾 `moduleSemas[mod->path] = std::move(sema);`
- **Why**：保住各模块 `typeStore_` 生命周期至第 6 步 CodeGen 之后（本函数作用域），使 `inferredType` 有效。
- **include**：main.cpp 顶部补充 `<map>`、`<memory>`（当前仅有 cstdlib/filesystem/iostream/sstream/string/string_view/vector，main.cpp:30-36）。
- **注意**：单文件模式（compileSingleFile L122）不改。

### C2（P1）Diagnostic 多模块错误归属 + 不截断合并 — Diagnostic.{h,cpp} / DiagnosticEngine.{h,cpp}

- **线程安全方案**：**每模块独立 DiagnosticEngine（任务线程内局部）+ 主线程按层序 merge**（不加锁）。任务线程零共享写。
- **C2-1** Diagnostic.h:79-86 结构体增加字段：
  ```cpp
  std::string file;  // 所属文件（report 时快照 fileName_）
  ```
- **C2-2** DiagnosticEngine::report（DiagnosticEngine.cpp:10-26）：`diags_.push_back(diag); diags_.back().file = fileName_;`（快照，避免整对象拷贝）。
- **C2-3** print（DiagnosticEngine.cpp:82-118）：位置行优先 `diag.file`，其次 `fileName_`（向后兼容单文件）：
  ```cpp
  const std::string& f = !diag.file.empty() ? diag.file : fileName_;
  if (!f.empty()) os << "  --> " << f << ":" << ... ;
  ```
- **C2-4** DiagnosticEngine.h:36 附近新增：
  ```cpp
  // 将 other 的诊断并入本引擎（多线程任务结果汇总；不截断，保留全部已记录错误）
  void mergeFrom(const DiagnosticEngine& other);
  ```
  实现（DiagnosticEngine.cpp）：`diags_` / `errorMessages_` 整体 insert；`errorCount_` / `warningCount_` 累加。**不检查 maxErrors_**（每模块 diag 上限仅防单模块级联刷屏，诊断完整）。
- **语义**：每模块 diag 默认 `maxErrors_=20`（与现状单引擎语义一致，DiagnosticEngine.cpp:12 达到上限丢弃该模块后续错误）；merge 后主 diag 保留全部 → 多模块错误总数 = 各模块之和，**只增不减**（优于现状全局 20 丢弃第 21+ 个）。

### C3（P1）Sema 同层并行 — main.cpp

- **位置**：compileMultiFile 第 5 步（main.cpp:240-266）整体重构。
- **数据结构**：
  - `std::map<std::string, std::unique_ptr<Aura::DiagnosticEngine>> moduleDiags;`（键 = mod->path，每模块独立 diag）
  - `moduleSemas`（C1）
  - `std::vector<std::future<void>>`（C++20 std::async）
- **模块 Sema 任务 lambda**（捕获 &mgr / &moduleSemas / &moduleDiags）：
  ```cpp
  auto runSemaModule = [&](ModuleInfo* mod, Aura::DiagnosticEngine& modDiag) {
      auto sema = std::make_unique<Aura::SemAnalyzer>(modDiag);
      for (auto& depPath : mod->deps) {
          auto depIt = mgr.modules().find(depPath);
          if (depIt == mgr.modules().end()) continue;
          std::string alias;
          for (auto& imp : mod->imports)
              if (imp.path == depPath) { alias = imp.alias; break; }
          sema->importExports(alias, depIt->second.exports);
      }
      (void)sema->analyze(*mod->ast);
      mod->exports = sema->extractExports();
      moduleSemas[mod->path] = std::move(sema);
  };
  ```
- **逐层执行**：
  - 层内任务 = 非 builtin 且 ast 非空模块
  - 并行度 `n = parallelJobs(opts, tasks.size())`（C5）；`n < 2` → 串行 for 循环；否则 `std::async(std::launch::async, ...)` 每任务 + `f.get()` 等待全部完成。
  - 层内完成 → **主线程按层内顺序** `mainDiag.mergeFrom(*moduleDiags[path])`。
- **Why**：同层模块互不依赖；依赖 exports 来自前序层（层间串行 + join 的 happens-before 保证）；`mod->exports` 每模块独立写无竞争。

### C4（P1）CodeGen 全模块并行 — main.cpp

- **位置**：compileMultiFile 第 7 步（main.cpp:284-343）重构。
- **任务返回值结构**：
  ```cpp
  struct CgResult {
      std::string cppPath;
      std::unique_ptr<Aura::DiagnosticEngine> diag;
  };
  ```
- **模块 CodeGen 任务 lambda**（捕获 &mgr / outDir）：
  1. 构建 cgImports（main.cpp:290-306 原逻辑，只读 mgr.modules()，并发读安全）
  2. `Aura::CodeGenerator cg(*modDiag)` + `cg.generate(*mod->ast, mod->moduleName, cgImports, mod->nsName)`
  3. 写 .h/.cpp（main.cpp:319-337 原逻辑）
  4. 返回 {cppPath, modDiag}
- **执行**：所有模块一次性 `std::async` 并行（无层间依赖；CodeGen 只读自身 AST + 依赖模块命名空间信息）。
- **主线程汇总**：按"层序 + 层内序"收集 cppPath（保持 allCppPaths 顺序确定）与 merge diag；`std::cerr << "  compiled: ..."`（原 L341）统一主线程按序打印；错误判断从任务内 `if (diag.hasErrors()) return 1`（原 L312-316）改为并行完成后统一 `mainDiag.hasErrors()` 判断。

### C5（P1）`-j N` 开关与并行度计算 — main.cpp

- **位置**：CliOptions（main.cpp:41-50）、parseArgs（main.cpp:52-75）。
- **接口**：
  - CliOptions 加 `int jobs = 0;`（0 = 默认自动）
  - parseArgs：`else if (arg == "-j" && i + 1 < args.size()) opts.jobs = std::atoi(args[++i].data());`
  - 静态辅助：
    ```cpp
    static int parallelJobs(const CliOptions& opts, size_t taskCount) {
        int n = opts.jobs > 0 ? opts.jobs
                : (int)std::thread::hardware_concurrency();
        if (n < 1) n = 1;
        if ((size_t)n > taskCount) n = (int)taskCount;   // 任务数 <2 → 1（串行）
        return n;
    }
    ```
- **include**：`<thread>`、`<future>`、`<cstdlib>`（atoi，已有）。

### C6（P2）pub 模块级导出策略 — Symbol.h / DeclChecker.cpp / SemAnalyzer.{h,cpp}

- **C6-1 import 不透传**：
  - Symbol.h:52 附近新增 `bool isImported = false;`
  - SemAnalyzer.cpp:697 importExports：types 循环注入符号置 `sym.isImported = true;`；L682 importFuncSymbol：`sym.isImported = true;`
  - SemAnalyzer.cpp:736-760 extractExports 开头：`if (sym.isImported) return;`（**永远不透传**，与模块级策略无关）
- **C6-2 模块级策略**：
  - SemAnalyzer.h 成员区加 `bool hasAnyPub_ = false;`
  - DeclChecker.cpp:60-61 declareDecl 入口：
    ```cpp
    if (decl.isPublic && dynamic_cast<const ImportDecl*>(&decl)) {
        error(decl, "pub cannot be applied to import declarations");   // C6-3
    }
    if (decl.isPublic && !dynamic_cast<const ImportDecl*>(&decl)
        && !dynamic_cast<const ConfigDecl*>(&decl))
        hasAnyPub_ = true;   // config 语法待定，不参与
    ```
  - extractExports 过滤：`if (sym.isImported) return; if (hasAnyPub_ && !sym.isPublic) return;`
  - Symbol.h:52 isPublic 默认保留 `true`，注释更新为"实际由 DeclChecker 显式赋值；Global 自有符号默认公开，模块级策略在 extractExports 判定"
- **C6-4**：`pub #config` 不报错（config 语法整体待定），不置 hasAnyPub_。`.aurai` 内置文件 pub 为常态，parseDecl 统一解析不受影响。

### C7（P2）类型退化修复 — Sema 兜底提前报错（覆盖 D1-D5；D6/D7/D8 见 4.3/4.9）

用户决策：**只要 CodeGen 会出现 bug 的静默退化，一律在 Sema 提前报错**；channel 元素类型**维持变量标注**（`let ch: channel<T> = ...`，不引入构造点标注新语法）。

- **C7-1 Generator 返回元素类型**（SemAnalyzer.cpp:189-190）：
  ```cpp
  case ReturnTypeInfo::Kind::Generator:
      return IterSemType::make(semTypeFromAuraName(ret.typeName));
  ```
  semTypeFromAuraName 未命中返回 ErrorSemType（诚实标记）。range 注册为 Generator("int")（BuiltinRegistry.h:302-304）→ 行为不变。
- **C7-2（扩展）inferMethodCall builtin 分支统一兜底报错**（ExprInfer.cpp:269-272）——覆盖所有**返回形状依赖元素类型**的方法（D1/D2/unwrap），不再局限于 Optional：
  - 前置：`elemTypeOf` 增加 `OptionalSemType` 分支（SemAnalyzer.cpp:85-101）：
    ```cpp
    if (auto* os = dynamic_cast<const OptionalSemType*>(iterType))
        return os->elementType ? os->elementType->clone() : ErrorSemType::make();
    ```
  - 报错逻辑（插入 ExprInfer.cpp:269 的 `findMethod` 命中分支）：
    ```cpp
    if (auto* entry = BuiltinRegistry::get().findMethod(typeKey, e.method, (int)e.args.size())) {
        auto& ret = entry->returns;
        // 返回形状依赖元素类型的调用（Optional<T> / Generic("channel") / Generic("T")）：
        // 元素类型不可知 → 报错引导标注（不改变返回 fallback，仅诊断）
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
  - 判据说明：`Generic("[T]")`（slice/front/back/pop）返回 objType 本身、`Generic("string")`（concat）返回 string，均**不依赖**元素提取 → 不检查；`Optional("T")`（sync.Channel.receive）、`Generic("channel")`（channel.receive）依赖 → 检查。`Generic("T")`（Optional.unwrap）保留在判据中但**实际不触发**：unwrap 的 objType 是 `OptionalSemType` → typeKey 为空 → 不进入此 builtin 查表分支（维持现状 auto 兜底，与 D6 同类，归入反推 issue）。
  - 触发场景（原静默退化）：无标注 channel 的 receive → 明确报错；有标注 channel（resolvedName 已填）→ elemTypeOf 成功，不报错。
- **C7-3 修复 Generic("channel") 分支直接 return ErrorSemType**（SemAnalyzer.cpp:220-224，D1 根因）：
  ```cpp
  // fallback == "channel" → 返回 channel 的元素类型
  if (ret.typeName == "channel") {
      return elemTypeOf(objType);   // 原实现直接 return ErrorSemType（从不提取 resolvedName）
  }
  ```
  有标注时正确提取元素；无标注时由 C7-2 报错兜底。**此修复保证"有标注也退化"的 D1 不再发生**。
- **C7-4 空列表 / 列表元素为 Error 的兜底报错**（D3/D5；StmtChecker.cpp:53/80 checkLetDecl/checkConstDecl）：
  - **限定范围**：仅当 inferredType 是 `ListSemType` 且**元素类型链上含 ErrorSemType**（List.elementType 递归，含嵌套 `[[]]`）时触发。**不检查整体 Error / Optional**（避免误伤 D6 record 方法调用与 C7-2 已报错的 fallback 场景）：
    ```cpp
    // SemAnalyzer 新增辅助：List 元素链递归检测 ErrorSemType
    static bool listContainsError(const SemType* t) {
        auto* l = dynamic_cast<const ListSemType*>(t);
        if (!l) return false;
        if (!l->elementType) return true;
        return dynamic_cast<const ErrorSemType*>(l->elementType.get())
            || listContainsError(l->elementType.get());
    }
    // checkLetDecl / checkConstDecl：decl.type 为空且 listContainsError(inferredType)
    //   → error(decl, "cannot infer element type from initializer; add explicit type annotation (e.g. let x: [int] = [])")
    ```
  - 覆盖：`let a = []`（D3）、`let a = [[]]`（D5）、`let x = [f()]`（f 返回 Error 的传染）。
  - 有标注 `let a: [int] = []` → StmtChecker L54-60 用 declaredType 覆盖推断 → 不触发。
- **C7-5 泛型实参数量不匹配报错**（D4；DeclChecker.cpp:194-199 resolveType NamedType 用户泛型分支）：
  ```cpp
  if (sym && sym->kind == SymKind::TypeAlias && !sym->typeParams.empty()) {
      if (n->typeArgs.size() != sym->typeParams.size()) {
          error(*n, "type '" + n->name + "' expects " + std::to_string(sym->typeParams.size())
                + " type argument(s), got " + std::to_string(n->typeArgs.size()));
      }
      result = applyTypeArgs(std::move(result), *sym, n->typeArgs);
      ...
  }
  ```
  覆盖缺省与多余两种情况；内置泛型（channel<int> 等）不经过 applyTypeArgs，不受影响。
- **C7-6 channel for-in 无标注兜底报错**（checkForStmt，StmtChecker.cpp:161-174）：
  - `iterType` 为 GenericSemType（channel 类）且 `elemTypeOf` 返回 ErrorSemType → `error(*stmt.iterable, "cannot infer element type of channel; add explicit type annotation (e.g. let ch: channel<int> = channel(10))")`。
  - 与 C7-2 报错语义一致，覆盖 `for val in ch`（无标注）路径。

## 4.5 影响分析

- **受影响组件**：main.cpp、Diagnostic.{h,cpp}、DiagnosticEngine.{h,cpp}、Symbol.h、SemAnalyzer.{h,cpp}、DeclChecker.cpp、ExprInfer.cpp、READMEs/12-modules.md。ModuleManager / CodeGen 其余部分只读使用，不改。
- **接口变更**：
  - DiagnosticEngine 新增 `mergeFrom`（新增方法，无破坏）
  - Diagnostic 新增字段 `file`（内部结构，无外部 ABI 依赖）
  - Symbol 新增字段 `isImported`（内部）
  - SemAnalyzer 新增成员 `hasAnyPub_`（内部）
- **行为变化**：
  - 多文件错误输出：新增模块文件名（`--> file:line:col`）。✓ 非 BREAKING
  - maxErrors：每模块 20（防级联）+ merge 不截断 → 多模块错误总数只增不减。✓
  - pub 导出策略：used/ 测试模块均带 pub → 有 pub 模式行为不变；无 pub 模块从"不导出"变"全导出"。✓ 改进
  - import 不透传：仅影响"经中间模块间接取符号"的非法用法（现状错误地可用）。
  - `pub import`：从"被接受（透传碰巧发生）"变"编译报错"。
  - **C7 报错集（新报错路径，均从"静默退化/非法 C++"变"编译报错"）**：
    - 无标注 channel + `receive()`/`unwrap()`：静默 fallback → 报错引导标注。
    - **有标注 channel + `receive()`（协程）**：从"仍退化 Error"（D1）→ 修复为正确提取元素类型。✓ 纯修复
    - 空列表无标注 `let a = []`：从"生成 `Array</* error_type */>*` 非法 C++"（D3）→ 报错引导标注。
    - 泛型实参缺省/多余 `Pair<int>`：从"残留 `Generic("auto")` 非法 C++"（D4）→ 报错。
    - channel 无标注 `for val in ch`：静默 → 报错引导标注。
  - **不影响**（范围外，列为已知限制）：D6（record 方法返回类型不传播 → GC root 缺失，归入 newIssue 反推 issue）、D7（泛型函数实例化）、D8（Union 推断不产生 UnionSemType）。
  - "compiled:" 日志顺序由层序决定（与现状一致）。
- **⚠️ BREAKING**：无。单文件模式（compileSingleFile）完全不改动；多文件 `-j1` 除上述改进外与现状一致。

## 4.6 边界条件处理策略

| 边界条件 | 当前处理 | 计划处理 | 测试策略 |
|---|---|---|---|
| 单模块项目（无用户 import） | compileSingleFile（main.cpp:89），不涉及并行 | 不变 | test.aura K1-K29 回归 |
| 模块数 <2 的多文件 | 串行 | parallelJobs → 1，自动串行 | used/ 单文件模块回归 |
| 循环依赖 | hasCycle 拦截（main.cpp:225） | 不变（并行前已拦截） | 构造环导入负向测试 |
| 多个 main / 无 main | validateEntry 拦截（main.cpp:234） | 不变 | 负向测试 |
| 并行中某模块 Sema 报错 | 级联 E010 | 各模块独立 diag 汇总后统一判断；exports 缺失模块下游自然报 E010 | 故意制造 Sema 错误模块，验证无崩溃、错误完整、含文件名 |
| 某模块 CodeGen 报错 | L312 立即 return 1 | 并行完成后统一判断（可能多生成几个 .cpp，无副作用） | 负向测试 |
| 内置模块 | isBuiltin 跳过（main.cpp:242,287） | 不变 | math_utils/test_import |
| 空 AST | mod->ast 为 null 跳过 | 不变 | — |
| 输出目录不存在 | create_directories（main.cpp:279） | 不变（并行前串行执行） | — |
| 同名模块写文件冲突 | 既有问题 | 不处理（与并行无关，现状存在） | 注明 |
| -j1 / 线程数=1 | — | 串行等价路径 | -j1 与 -jN 输出一致性对比 |
| std::async 异常 | — | f.get() 重新抛出 → main 捕获返回 1 | 人为制造异常 |
| 内存（并行驻留） | 串行驻留 | 并行多模块同时驻留 | 大项目实测可接受 |
| B import A → C import B | A 符号被透传 | isImported → 不透传 | C 访问 A → E010 负测 |
| 模块无 pub | 全部不导出 | 全部导出 | 无 pub 模块 import 后访问 → 正测 |
| 模块有 pub | 仅导出 pub | 仅导出 pub（不变） | math_utils/test_import 回归 |
| `pub import` | 被接受 | 报错 | 负向测试 |
| 无 pub 声明跨模块访问（有 pub 模块） | E010 | 不变 | 负向测试 |
| `pub #config` | 被接受、语义未定义 | 暂不处理（config 语法待定） | 注明 |
| 单模块错误数 >20 | 全局第 21+ 丢弃 | 每模块 20（防级联）+ merge 全保留 | 构造 25 错误模块验证报满 20 即停 |
| channel 无类型标注 + receive() | 静默 fallback int | 报错引导标注 | 负向测试 |
| channel 有标注（协程）+ receive() | **仍退化 Error**（D1：Generic("channel") 不看 resolvedName） | 修复为提取元素类型 | K 系列 + test_channel 回归 |
| 空列表无标注 `let a = []` | `Array</* error_type */>*` 非法 C++ | 报错引导标注 | 负向测试 |
| 泛型实参缺省/多余 `Pair<int>` | 残留 Generic → `Array<auto>*` 非法 C++ | 报错 | 负向测试 |
| channel 无标注 `for val in ch` | 静默 | 报错引导标注 | 负向测试 |
| 空列表有标注 `let a: [int] = []` | declaredType 覆盖推断 | 不变（合法） | used/ 回归（4.aura 等） |
| range（Generator("int")） | 硬编码 int | 按 typeName 解析（结果同为 int） | K 系列回归 |

## 4.7 测试计划

1. **P0 验证**：`example/used/test_import.aura`（import math_utils + 泛型 ctor/zip）多文件编译 → 不再崩溃，链接运行输出正确。
2. **并行正确性**：同一项目 `-j1` 与默认并行编译，exe 运行输出一致；`-j2`/默认三档对比。
3. **回归**：
   - 单文件：example/test.aura（K1-K29）+ compile.cmd → test.exe 全通过。
   - 多文件：used/ 下 test_sync_for / test_closure / test_range / test_sync_max / los_compact 全部编译运行。
   - math_utils + test_import 完整流程。
4. **错误路径**：
   - Sema 报错模块 + 正常模块混合 → 无崩溃、错误含模块文件名、退出码 1。
   - 语法错误模块 → parseModule 报错路径不变。
   - 25 错误模块 → 报满 20 即停；多模块错误 merge 总数守恒。
5. **pub（C6）**：
   - 正测：math_utils+test_import 全 pub 回归不变。
   - 正测：无 pub 模块被 import 访问 → 正常编译运行。
   - 负测：A→B→C 普通 import，C 访问 A 符号 → E010。
   - 负测：`pub import` → 报错。
   - 负测：有 pub 模块中无 pub 声明跨模块访问 → E010。
   - 回归：builtin .aurai 加载不受影响（io.aurai 均 pub）。
6. **类型退化（C7）**：
   - 正测：range for-in 行为不变（K 系列）。
   - 正测：`sync.Channel<int>` receive() 推断不变、GcRootHandle 包装不变（K18-K23）。
   - 正测：**协程 `channel<int>` receive() 修复**（D1）→ `let v = ch.receive()` 推断为元素类型而非 Error（test_channel.aura，先修 StmtGen 既有 spawn 问题或构造最小用例）。
   - 负测：`let ch = sync.Channel(10)` + `ch.receive()` → 报"cannot infer element type"。
   - 负测：`let ch = channel(10)` + `ch.receive()` → 报"cannot infer element type"。
   - 负测：`let a = []` → 报"add explicit type annotation"；`let a = [[]]` 同理。
   - 负测：`let a: [int] = []` → **合法不报错**（回归）。
   - 负测：`Pair<int>`（少 B）→ 报"expects 2 type argument(s), got 1"。
   - 负测：无标注 channel + `for val in ch` → 报错。
   - 审计：临时注册 Generator("string") 验证推断为 string 而非 int；Optional.unwrap() 正常路径（K21 `j.unwrap()`）不误报。

## 4.8 实施步骤（按序，依赖链明确）

| 步骤 | 内容 | 验证 |
|---|---|---|
| 1 | 前置分析（已完成） | 本 Plan 审查通过 |
| 2 | **C1**：moduleSemas 持有 SemAnalyzer（串行版修复崩溃） | `cmake --build build` 无 error；test_import.aura 编译不再崩溃并运行正确 |
| 3 | **C2**：Diagnostic 加 file + report 快照 + print 用 diag.file + mergeFrom | 编译通过；多文件错误输出带文件名 |
| 4 | **C5**：`-j` 开关 + parallelJobs（独立小改） | 编译通过；-j1/-j2/默认三档可用 |
| 5 | **C3**：Sema 同层并行（任务 lambda + std::async + 层内 merge） | 编译通过；test_import 回归；Sema 报错模块负测 |
| 6 | **C4**：CodeGen 全并行 + 主线程顺序收集/汇总/打印 | 编译通过；多文件回归；-j1/-jN 输出一致性 |
| 7 | **C6**：pub（isImported 不透传 + hasAnyPub_ 模块级策略 + pub import 报错） | 编译通过；4.7 第 5 项 pub 正/负测全过 |
| 8 | **C7**：类型退化（C7-1 Generator 按 typeName → C7-3 Generic("channel") 提取 → C7-2 统一兜底报错 → C7-6 for-in → C7-4 空列表 → C7-5 泛型实参） | 编译通过；4.7 第 6 项验证 |
| 9 | **README**：12-modules.md 新增 12.4（按 4.10 草案） | 文档审查 |
| 10 | 全量回归：K1-K29 + used/ 多文件 + 负向测试 + 一致性对比 | 全部通过 |
| 11 | 清理临时文件；TODO.txt 标记完成；生成 commit message | 审查 |
| 回滚 | 各步骤独立 commit；失败可 revert 该 commit（C1-C7 相互独立，改动集中在 main.cpp/Diag/Sema/README） | — |

## 4.9 风险与缓解

| 风险 | 缓解 |
|---|---|
| MinGW std::async 行为差异/线程开销 | 任务数 <2 降级串行；备选 std::jthread 池 |
| 错误输出顺序 | 每模块独立 diag + 主线程按层序 merge，顺序确定 |
| maxErrors 语义变化 | 每模块 20 = 单模块防级联（与现状单引擎一致）；merge 不截断，总数守恒 |
| f.get() 异常传播遗漏 | main 包 try/catch 统一捕获返回 1 |
| 并行期 BuiltinRegistry 被写入（只读假设被破坏） | 审计 tryLoadAurai 仅在 loadAll 阶段；并行前打断言 |
| 内存峰值 | 默认并行度受限硬件并发；-jN 可限流 |
| 写文件竞态 | 任务线程独立写（路径互异）；outDir 创建在并行前 |
| C6 模块级策略改变（无 pub 模块"不导出"→"全导出"） | used/ 均带 pub 不受影响；README 明确新策略 |
| C7 影响下游 | range/标注 channel 不变；fallback 保留 int（GC 安全），仅新增报错路径；有标注 channel receive 修复（D1）为纯修复 |
| C7 误报风险（`needsElem` 判据过宽） | 判据限定 Optional/Generic("channel")/Generic("T")；"[T]"/"string" 排除；测试覆盖 concat/slice/unwrap 不误报 |
| C7-4 空列表报错误伤（有标注空列表） | 有 decl.type 时不触发（declaredType 覆盖推断）；回归 used/ 4.aura、los_compact_test.aura |
| **D6/D7/D8 已知限制（本 plan 不处理）** | D6 record 方法返回类型不传播 → GC root 缺失，归入 newIssue 第 7 条（sema 反推）；D7 泛型函数实例化待审计；D8 Union 推断当前无产生路径，理论风险。README 不描述（避免误导），TODO.txt 记录 |
| `-j` 参数解析与现有参数冲突 | 独立参数，parseArgs 顺序无关；`-j` 后必须跟数字 |

## 4.10 README 补充草案

### 4.10.1 可见性（READMEs/12-modules.md 新增 12.4，追加于 12.3 之后）

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

### 4.10.2 类型标注规则（README 分散落点，对应 C7 报错集）

> 背景：Aura 的 `let x = expr` 采用单遍类型推断（Go 风格）。**当"单遍推断无法获得、但语法允许省略"的类型信息缺失时，编译器会在 Sema 阶段报错**（不会静默退化成错误类型）。以下情况**必须显式标注**：

**① 通道元素类型（READMEs/11-concurrency.md §11.2 / §11.7 "创建"表格旁补充）**

```aura
let ch: channel<int> = channel(10)          // 必须标注元素类型
let v = ch.receive()                        // 合法：从标注推断 v: Optional<int>
// let ch = channel(10)                     // 编译错误：无法推断元素类型
```

- `channel<T>`（§11.2）与 `sync.Channel<T>`（§11.7）同理；`receive()` / `unwrap()` / `for val in ch` 均依赖元素类型推断。
- 原因：构造表达式 `channel(cap)` 本身不含元素类型信息，编译器不会猜测（v1 不做从 `send()` 反推）。

**② 空列表（READMEs/04-variables.md 类型推断小节补充）**

```aura
let a: [int] = []        // 必须标注元素类型
// let a = []            // 编译错误：无法推断元素类型
let b = [1, 2, 3]        // 合法：从元素推断
```

- 嵌套同理：`let m: [[int]] = [[]]`。

**③ 泛型类型实参（READMEs/06-generics.md 补充）**

```aura
type Pair<A, B> = { first: A, second: B }
let p: Pair<int, string> = ...   // 实参数必须与声明一致
// let p: Pair<int> = ...        // 编译错误：expects 2 type argument(s), got 1
```

**排除项**（编译器不要求标注）：函数/方法的返回类型（符号表已有签名）、普通非空列表元素（可从元素推断）、`range` 迭代器（注册表已有类型）。

---

*本 plan 为工作流程 3（准备实现）产物，等待用户审查。审查通过后：写入 TODO.txt → 按 4.8 进入实施（change.md）。*
