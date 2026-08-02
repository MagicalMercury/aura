# Plan: int() / float() / str() 字符串转换函数 + 默认参数支持（详细实施方案）

## 4.1 Title & Metadata

- **标题**：Python 风格 int()/float()/str() 转换函数 + 通用默认参数支持
- **作者**：Agent（Aura 编译器）
- **日期**：2026-08-02（工作流程 3 详细实施方案，替换原草案）
- **关联模块**：src/AST/Stmt.h、src/Parser/TypeParser.cpp、src/Parser/Parser.cpp、src/Sema/Symbol.h、src/Sema/SemAnalyzer.cpp、src/Sema/Checker/ExprInfer.cpp、src/Sema/Checker/DeclChecker.cpp、src/Sema/BuiltinRegistry.h、src/CodeGen/CodeGen.h、src/CodeGen/ExprGen.cpp、src/CodeGen/DeclGen.cpp、src/Module/ModuleManager.cpp、src/main.cpp、runtime/builtin/string.h、runtime/builtin/string.cpp、runtime/builtin/error.h、builtins/builtin.aurai、builtins/channel.aurai、builtins/mutex.aurai、READMEs/
- **来源**：TODO.txt [一]（原 plan11.md:41-42 两个 P0，用户确认强化为 Python 风格 + 默认参数纳入）

## 4.2 Objectives

补齐 Aura 语言层的类型转换三件套 `int(s)` / `float(s)` / `str(x)`（语义对齐 Python 3），并将"默认参数"作为通用语言特性落地（Parser/AST/Sema/CodeGen 全链路，函数/方法/构造函数统一生效），同时修复 Sema 对 throws 内置函数调用缺失的 `checkThrowsContext` 检查（含全局函数与内置方法两处）。

## 4.3 Current State Summary（本次源码精读验证结论）

以下行号均于 2026-08-02 源码精读确认：

1. **无任何字符串→数值解析能力**：runtime 仅有 `GcString::from(int32/int64/double/bool/GcString*)`（string.h:43-51，`from(GcString*)` 为透传）；`BuiltinRegistry.functions_`（BuiltinRegistry.h:301-320）只有 range×3（L302-304）/channel（L307）/sync.*（L309-316）/gc_force（L318）/gc_stats（L319）。
2. **全局内置函数无通用 CodeGen 映射**：gc_force/gc_stats/channel 是 genCallExpr 开头硬编码 special-case（ExprGen.cpp:539-552）；range 仅在 genForStmt 展开；其余全局函数走 `safeName(callee)` 裸调用。**safeName（CodeGen.cpp:220+）关键字集合含 "int"/"float"/"bool"**，会把 `int` 变量转义为 `int_`——`int(...)` 调用必须经显式映射表在 safeName 前拦截，否则生成非法 C++。
3. **throws 检查缺口（两处）**：
   - inferCall 内置全局函数分支（ExprInfer.cpp:168-175）：`findFunction` 命中后直接返回类型，**无 checkThrowsContext**（符号表函数分支 L179-186 有）。
   - inferMethodCall 内置模块函数分支（L237-247）与内置类型方法分支（L264-285）：均无 checkThrowsContext。BuiltinMethod 已有 throws 字段（BuiltinRegistry.h:70），仅未在调用点检查。
4. **不支持默认参数**：AST `Param`（Stmt.h:15-18）仅 name+type；`cloneParam`（L20-25）只拷 type；`parseParam`（TypeParser.cpp:208-217）无 `=` 解析（parseParams L219-225 / parseAurai Parser.cpp:127-138 均复用 parseParam，一处修改全链路生效）；`checkCallArgs`（SemAnalyzer.cpp:450-478）L458 严格 `args.size() != formalTypes.size()`；`SymParam`（Symbol.h:24-27）无默认值字段。
5. **findFunction/findMethod 严格匹配**：BuiltinRegistry.h:151-157（`f.name == name && (int)f.params.size() == argCount`）与 L104-113（方法同）；`BuiltinGlobalFn`（L58-63）/`BuiltinMethod`（L65-72）均无 defaultCount 字段。
6. **aurai 加载链路**：`loadBuiltinAurai`（ModuleManager.cpp:226-229）当前仅 io.aurai，main.cpp:110（单文件）/main.cpp:239（多文件）在 Sema 前调用；`loadAuraiFile`（L207-224）经 Parser::parseAurai（Parser.cpp:127-138，noBody_ 模式）→ `tryLoadAurai`（BuiltinRegistry.h:165-171）→ `doLoadAurai`（L177-214，FunDecl 分支 L203-211 / MethodDecl L190-202 / TypeDecl L180-189）。isKnownBuiltin 白名单（ModuleManager.cpp:97-104）仅 "path"。
7. **跨模块导出/导入链路（默认参数传递通道）**：`extractExports`（SemAnalyzer.cpp:830-855）→ `buildFuncExport`（L816-828，深拷贝 params/returnType/throws）→ ModuleInfo.exports（main.cpp:284 写入）→ Sema 阶段 `importExports`/`importFuncSymbol`（SemAnalyzer.cpp:790-813/772-788，符号名 qualified 为 `alias.name`）→ CodeGen 阶段 `cg.generate(*mod->ast, mod->moduleName, cgImports, mod->nsName)`（main.cpp:371）。**CodeGen 不访问符号表/exports**——跨模块调用以 `math.foo(...)` MethodCallExpr 形式出现，genMethodCall 中经 importNsNames_ 判定为命名空间调用（ExprGen.cpp:738-781）。
8. **`-> None` 显式返回类型等价**：semTypeFromBuiltinReturn Named 分支（SemAnalyzer.cpp:258-262）→ semTypeFromAuraName("None") → types_["None"]（BuiltinRegistry.h:227）→ NoneSemType，与 Kind::None（L256-257）同值。gc_force 写 `-> None` 安全。
9. **方法/ctor 签名生成**：genMethodDecl 签名在 DeclGen.cpp:442-455（`retType recv::method(params)`）；genRecordStruct 内嵌方法声明 L104-109；genConstructor L488+。调用点补齐策略下**签名均不生成默认参数，无需改动**。
10. **GcString 接口**：`view()`（string.h:74）/`data()`/`size()` 可用于解析；`make_value_error(const char*/GcString*)`（error.h:29-34）可直接复用。

## 4.4 Proposed Changes（详细实施方案）

### C1 — AST：Param 支持默认值表达式

- **变更点**：`struct Param`（Stmt.h:15-18）增加
  ```cpp
  std::unique_ptr<ASTNode> defaultExpr;  // nullptr = 无默认值
  ```
  `cloneParam`（Stmt.h:20-25）同步深拷贝：
  ```cpp
  if (p.defaultExpr) r.defaultExpr.reset(p.defaultExpr->clone().release());
  ```
- **接口契约**：`ASTNode::clone()` 需对 defaultExpr 可能出现的节点类型（IntLiteral/FloatLiteral/BoolLiteral/StringLiteral/NoneLiteral/UnaryExpr 负号/Identifier 全局引用等）可用——clone 虚函数已在 AST 体系普遍实现，若某节点缺 clone 实施时补。
- **影响面**：FunDecl（Stmt.h:327-345）/FunExpr（L507-523）/MethodDecl 全部复用 `std::vector<Param>`，一处修改全链路生效。**无破坏性**（新增字段默认 nullptr）。

### C2 — Parser：解析 `name: type = expr`

- **变更点**：`parseParam`（TypeParser.cpp:213-216）在类型解析后追加：
  ```cpp
  if (match(TokType::Assign)) {
      p.defaultExpr = parseExpression();  // 入口优先级与函数体内表达式一致
  }
  ```
  （`TokType::Assign` 已存在，用于变量赋值；`match` 消费成功返回 true）
- **验证点**：`TokType::Assign` 是否已被 parseParam 上下文的前置词法逻辑消耗（`=` 在参数列表内无歧义，需确认 parser 中 `(` 内 `=` 不会与 lambda/默认参数以外的语法冲突——lambda 在 Aura 是 `(params) => expr` 或 `fun`，`=` 不冲突）。
- **影响面**：parseParams（TypeParser.cpp:219-225）、parseAurai（Parser.cpp:127-138）、DeclParser/StmtParser/ExprParser 的 parseParam 调用点零改动，aurai 声明默认参数自动支持。

### C3 — Sema：声明规则 + SymParam + 调用检查放宽 + throws 补缺口

- **C3.1 声明规则检查**（落点：`checkFunBody`/`checkMethodBody` 开头，SemAnalyzer.cpp:723-727）：
  对 decl.params 尾部扫描：
  1. **尾部连续性**：首个 defaultExpr 非空后，其后所有参数必须也有默认值；否则报错（消息如 `default argument must be trailing: parameter '<name>'`）。检查放 `checkFunBody` 开头（声明注册后），负向用例能定位到具体参数。
  2. **默认值表达式求值检查**：对每个 defaultExpr `inferExpr`（声明作用域）：
     - 返回 ErrorSemType → 报错（未定义标识符/类型错误自然覆盖）；
     - `isAssignable(参数类型, 默认值类型)` 失败 → 报错（如 `fun f(a: int = "x")`）；
     - **v1 限制**：默认值表达式引用其他参数名 → 声明处无绑定，inferExpr 报未定义，错误消息已可读，无需特判（调用点求值语义下此限制记入 README）。
  3. **泛型参数限制**：参数类型含 `GenericTypeRef`（`fun f<T>(x: T = 5)`）→ 报错 `default argument not supported on generic parameter '<name>'`（isAssignable 对未绑定 T 语义未定义，v1 显式拒绝）。
  4. 检查函数收敛：抽一个 helper `checkDefaultArgRules(const std::vector<Param>&, const ASTNode& declNode)`（~30 行），checkFunBody/checkMethodBody 复用。
- **C3.2 SymParam 传递**：`SymParam`（Symbol.h:24-27）增加
  ```cpp
  std::unique_ptr<ASTNode> defaultExpr;  // 供 Sema 调用检查统计 + 跨模块导出
  bool hasDefault = false;
  ```
  构建函数/方法符号处（DeclChecker.cpp declareDecl FunDecl 分支 L139-162 / MethodDecl L163-175）从 decl.params 拷贝 defaultExpr 到 sym.params。**CodeGen 不依赖符号表**（见 C5.1/C5.3，直接读本模块 AST 与导出表），SymParam 仅为 Sema 调用检查 + 跨模块导出服务。
- **C3.3 checkCallArgs 数量放宽**（SemAnalyzer.cpp:450-462）：
  签名增加参数 `size_t defaultCount = 0`（默认 0，既有调用零改动）。数量检查改为：
  ```cpp
  size_t total = formalTypes.size();
  size_t min   = total - defaultCount;   // 默认参数保证尾部连续（C3.1），min 即 total-defaultCount
  if (args.size() < min || args.size() > total) {
      error(callNode, role + " '" + calleeName + "' expects " +
          (min == total ? std::to_string(total) : std::to_string(min) + "~" + std::to_string(total)) +
          " arguments, got " + std::to_string(args.size()));
  }
  ```
  类型检查（L463-477）仅遍历已传入实参，缺失的默认参数不参与泛型映射收集（保持现有循环 `i < args.size() && i < formalTypes.size()`）。
  调用方传递 defaultCount：
  - inferCall 符号表函数分支（ExprInfer.cpp:179-186）：`从 sym->params 统计尾部 hasDefault 数`；
  - inferCall 函数类型变量分支（L195-204）：`fst->paramTypes` 对应 FuncSemType——v1 闭包默认参数标记为阶段 3/4，此分支暂传 0（`FuncSemType::paramTypes` 无 hasDefault 字段，不引入）；
  - inferCall 内置函数分支（L170-172）：`fn->defaultCount`（C4 新增字段）；
  - inferMethodCall 内置方法分支（L264-285）：`entry->defaultCount`；
  - inferMethodCall import 命名空间函数分支（L212-233）：`imported->params` 统计（importFuncSymbol 已携带 defaultExpr）。
- **C3.4 throws 检查补缺口（三处）**：
  - ExprInfer.cpp:170-172：`findFunction` 命中后补 `checkThrowsContext(e, callee->name, fn->throws)`；
  - ExprInfer.cpp:237-247（内置模块函数）：补 `checkThrowsContext(e, fqName, fn->throws)`；
  - ExprInfer.cpp:264-285（内置类型方法）：`findMethod` 命中后补 `checkThrowsContext(e, e.method, entry->throws)`。
  - **顺带修复**：内置分支对 args 缺 `inferExpr`（对比 L242-244 有）：在 L170-172 分支补
    ```cpp
    for (auto& arg : e.args) if (arg) (void)inferExpr(*arg);
    ```
    使 CodeGen 的 `args[i]->inferredType` 有效（genGcRootedArgs 依赖，ExprGen.cpp:633）。此改动同时修正 `int(io.readLine()!)` 等参数表达式的 GcRootHandle 保护。
- **Where**：Symbol.h、SemAnalyzer.cpp、ExprInfer.cpp、DeclChecker.cpp。

### C4 — BuiltinRegistry：defaultCount 与匹配放宽

- **变更点**：`BuiltinGlobalFn`（BuiltinRegistry.h:58-63）与 `BuiltinMethod`（L65-72）增加 `int defaultCount = 0;`。
- **findFunction**（L151-157）改为：
  ```cpp
  if (f.name == name
      && (int)f.params.size() - f.defaultCount <= argCount
      && argCount <= (int)f.params.size())
      return &f;
  ```
  findMethod（L104-113）同理。**注意 str×4 重载**：4 个 `str(x)` 均 defaultCount=0、params.size()=1，`str("abc")`/`str(3.14)`/`str(true)` 都命中首个 `str(x:int)`——返回类型均为 Named("string")，类型推断一致，无实际影响（Sema 内置分支不检查参数类型，与 path.join 现状一致，C++ 侧 string_of 重载解析正确）。记入风险 2。
- **defaultCount 来源**：
  - 硬编码注册表：int/float/str 直接在 functions_ 初始化列表填（C7.1 builtin.aurai 已承载，硬编码侧无需填）；
  - aurai 注册（doLoadAurai FunDecl 分支 L203-211）：从 `fn->params` 尾部统计 defaultExpr 非空数：
    ```cpp
    int dc = 0;
    for (auto it = fn->params.rbegin(); it != fn->params.rend() && it->defaultExpr; ++it) ++dc;
    gf.defaultCount = dc;
    ```
    （依赖 C2 Parser 先行——aurai 中 `base: int = 10` 需先能解析）。
- **Where**：src/Sema/BuiltinRegistry.h。

### C5 — CodeGen：调用点补默认实参 + 全局函数映射 + 跨模块默认参数

- **C5.1 同模块函数默认参数补齐**：
  - 新成员（CodeGen.h，CodeGenerator private 区）：
    ```cpp
    // 函数名 → 尾部默认值表达式指针列表（顺序 = 形参位置），仅记录有默认值的参数
    std::map<std::string, std::vector<const ASTNode*>> fnDefaultArgs_;
    ```
  - 收集点：`genFunDecl`（DeclGen.cpp:176）非 declarationsOnly 路径，遍历 decl.params 尾部 defaultExpr 非空的参数，`fnDefaultArgs_[decl.name]` 按形参顺序 push_back（与形参索引对齐，调用点按"缺失个数"取尾部 N 个）。
  - 补齐点：`genCallExpr`（ExprGen.cpp:534）argExprs 构造（L594-617）**之前**：
    ```cpp
    // 同模块函数默认参数补齐（callee 是本模块 FunDecl）
    auto fit = fnDefaultArgs_.find(calleeName);
    if (fit != fnDefaultArgs_.end()) {
        size_t missing = fnDefaultArgs_total_(fit->second 所在函数的形参总数) - e.args.size();
        // 实际：fnDefaultArgs_ 需同时记录"函数形参总数"，见接口契约
        for (size_t k = e.args.size(); k < 形参总数; ++k)
            argExprs.push_back(genExpr(*fit->second[k - 最小参数数], isCoroutine));
    }
    ```
    **接口契约修正**：仅存尾部默认值列表无法映射到"从第几个形参开始补齐"。改为存 `std::pair<size_t /*形参总数*/, std::vector<const ASTNode*>>` 或直接存 `std::vector<const ASTNode*>` 长度=形参总数（无默认值为 nullptr）。**推荐后者**（长度=形参总数，nullptr=无默认值），调用点：`missing = 形参总数 - e.args.size()`，对 `k = 形参总数 - missing; k < 形参总数; ++k` 若 `defaults[k]` 非空则 append `genExpr(*defaults[k])`。
  - 补齐后统一走现有 genGcRootedArgs（L630-635）：默认值表达式若返回 GC 指针（string 字面量 intern_string 等），`isHeapSemType` 自动 GcRootHandle 保护。**前提是 e.args[i]->inferredType 有效**——默认值表达式的 inferredType 由 genExpr 内部设置（genExpr 各分支会写 inferredType？需确认 genExpr 仅读不写——若默认值表达式未设置 inferredType，genGcRootedArgs 对补齐参数传 `e.args` 之外的指针时需自行 inferExpr 或视为堆类型保守保护。实施细节：补齐参数的 inferredType 直接取 defaultExpr 求值类型，若不可得则保守按堆类型包装——默认值多为字面量/字符串，保守包装安全）。
- **C5.2 全局内置函数映射**（genCallExpr 开头，L539-552 special-case 区域后、L554 isCtor 判断前）：
  ```cpp
  // 全局函数名 → runtime 映射（int/float/str 是 C++ 关键字，必须拦截）
  static const std::map<std::string, std::string> kGlobalFnMap = {
      {"int",   "aura_rt::string_to_int"},
      {"float", "aura_rt::string_to_float"},
      {"str",   "aura_rt::string_of"},
  };
  auto gmap = kGlobalFnMap.find(calleeName);
  if (gmap != kGlobalFnMap.end()) {
      // 内置默认参数补齐：int 的 base=10
      std::vector<std::string> argExprs;
      for (size_t i = 0; i < e.args.size(); ++i)
          argExprs.push_back(genExpr(*e.args[i], isCoroutine));
      if (auto* fn = BuiltinRegistry::get().findFunction(calleeName, (int)e.args.size())) {
          while (argExprs.size() < fn->params.size())
              argExprs.push_back("10");   // base=10 字面量（v1 仅 int 有默认参数，映射表值可扩展）
      }
      std::string callExpr = gmap->second + "(";
      for (size_t i = 0; i < argExprs.size(); ++i) { if (i > 0) callExpr += ", "; callExpr += "{" + std::to_string(i) + "}"; }
      callExpr += ")";
      // 统一走 genGcRootedArgs 保护（string 参数为堆类型）
      std::vector<std::pair<std::string, const SemType*>> gcArgs;
      for (size_t i = 0; i < argExprs.size(); ++i)
          gcArgs.emplace_back(argExprs[i], i < e.args.size() ? e.args[i]->inferredType : nullptr);
      return genGcRootedArgs(gcArgs, callExpr, isCoroutine);
  }
  ```
  - **注意**：此特判必须在 safeName 前（genExpr(callee) 前），因 safeName 会把 int→int_。
  - `str(x)` 的 C++ 侧：`string_of(int32_t/int64_t/double/bool/GcString*)` 重载解析，正确。
  - 内置默认参数补齐依赖 `findFunction` 命中（Sema 已保证调用数量合法）。
- **C5.3 方法/构造函数默认参数（阶段 4）**：
  - 新成员 `std::map<std::string, std::vector<const ASTNode*>> methodDefaultArgs_;`，键 = `receiverType + "." + methodName`（与 PendingMethod 键约定一致）。收集点：genMethodDecl（DeclGen.cpp:361，非 ctor）/genConstructor（L488+）。
  - 补齐点：genMethodCall（ExprGen.cpp:648）mArgExprs 构造（L784-786）前，按 `e.object` 的类型键查表。接收者类型获取：v1 从 `e.object->inferredType`（ListSemType→"[T]"、GenericSemType→name、PrimSemType→string），与 Sema inferMethodCall 的 typeKey 推导（L253-262）对齐；取不到类型 → 不补齐（保守，C++ 编译错误提示）。
  - ctor 调用（genCallExpr isCtor 分支 L557-583）：键 = calleeName（registeredTypes_ 中的记录名），补齐同 C5.1。
  - **阶段 4 标记**：方法默认参数跨模块（对象类型定义在另一模块）v1 不支持（与跨模块函数同机制见下，方法键需携带模块信息，超范围 → 记录 TODO）。
- **C5.4 跨模块函数默认参数**：
  - 机制：导出表携带默认值表达式 AST（C3.2 SymParam.defaultExpr → buildFuncExport 深拷贝 L816-828 → ModuleInfo.exports）→ main.cpp runCgModule 构造 → CodeGen 新参数。
  - `CodeGenerator::generate`（CodeGen.h:102-106）新增参数：
    ```cpp
    // 跨模块函数默认参数：模块名 → (函数名 → 默认值表达式指针数组，长度=形参总数，nullptr=无默认)
    using CrossModuleDefaults = std::map<std::string,
        std::map<std::string, std::vector<const ASTNode*>>>;
    ```
    存为 CodeGenerator 成员 `crossDefaults_`（generate 内赋值）。
  - main.cpp runCgModule（L344-392）：遍历 cgImports（用户模块分支 L357-362），`mgr.modules().find(imp.path)->exports.funcs`，对每个 FuncExport 尾部 defaultExpr 非空的函数构建 `crossDefaults_[imp.modName][函数名]`（AST 指针来自 ModuleInfo.exports 常驻内存，跨模块并发读安全——多文件 CodeGen 并行时**只读**，无写竞争）。
  - 补齐点：genMethodCall isNs 分支（ExprGen.cpp:738-781，`importNsNames_.count(id->name)`）mArgExprs 构造前，查 `crossDefaults_[id->name][e.method]`。
  - **降级策略**：若实施中评估改动超预期（generate 签名变化影响单文件模式 main.cpp:165 与测试驱动），降级为 Sema 报错：inferCall import 分支（L212-233）调用缺参且函数带默认参数 → 报 `default argument for imported function '<name>' is not supported; pass all arguments explicitly`，跨模块默认参数列入后续版本。**默认推荐走完整机制**（改动 ~60 行，与多文件链路正交）。
- **Where**：src/CodeGen/CodeGen.h、src/CodeGen/ExprGen.cpp、src/CodeGen/DeclGen.cpp、src/main.cpp。

### C6 — runtime：string_to_int / string_to_float / string_of

- **C6.1 `aura_rt::string_to_int(GcString* s, int32_t base = 10) -> int32_t`**（string.cpp 实现，string.h 声明）：
  - 语义（对齐 Python `int(s, base=10)`）：
    - `std::string_view v = s->view()`；跳过前导/尾部 isspace；允许 `+`/`-`。
    - base 合法值：0 或 2~36（其他 → `make_value_error`）。
    - 前缀自动检测（strtol base=0 语义）：`0x/0X`→16、`0o/0O`→8、`0b/0B`→2；base=10 时也认前缀（Python 3.11 行为）；显式 base 与前缀不一致 → ValueError。
    - 完整消费检查（endptr == v.end()）；至少一个数字位；拒绝小数/指数/内嵌空白（strtol 天然拒绝 `3.14`/`1e3`）。
    - 溢出：`errno == ERANGE` 或结果超出 int32 范围 → ValueError。
    - 空串/纯空白 → ValueError。消息 Python 风格：`invalid literal for int() with base <n>: '<s>'`。
  - 实现：裁剪空白后取子串构造 `std::string`（或直接构造 C 串）交 `strtol`；`errno = 0` 前置；错误统一 `throw make_value_error(msg)`（error.h:29-34，**勿用 std::stoi**——半解析静默 + 抛 C++ 异常）。
- **C6.2 `aura_rt::string_to_float(GcString* s) -> double`**（同位置）：
  - 空白/±/小数/e 指数；先手动识别 `inf`/`infinity`/`nan`（大小写不敏感、可带符号）→ ±INFINITY / NAN；其余 `strtod` + endptr 完整消费；ERANGE 返回 ±inf（Python 行为，不报错）；空串/非数字 → ValueError。
- **C6.3 `aura_rt::string_of(...)` 重载**（string.h 内联，直接转发 GcString::from）：
  ```cpp
  inline GcString* string_of(int32_t v) { return GcString::from(v); }
  inline GcString* string_of(int64_t v) { return GcString::from(v); }
  inline GcString* string_of(double v)  { return GcString::from(v); }
  inline GcString* string_of(bool v)    { return GcString::from(v); }
  inline GcString* string_of(GcString* s) { return s; }
  ```
  注意 `string_of(int32_t)` 与 `string_of(bool)` 不歧义（类型不同）；Aura int→int32_t、float→double 与重载精确匹配。
- **Where**：runtime/builtin/string.h、runtime/builtin/string.cpp。**无关模块**：string.cpp 归属 runtime/build，与 aurac 构建独立，需 `cmake --build runtime/build` 重编。

### C7 — 注册与文档

- **C7.1 新建 builtins/builtin.aurai（始终加载）**（用户审查指定）：
  ```aurai
  // 基础内置全局函数（始终加载；range/channel/sync.* 保留硬编码）
  fun gc_force() -> None
  fun gc_stats() -> string
  fun int(s: string, base: int = 10) throws -> int
  fun float(s: string) throws -> float
  fun str(x: int) -> string
  fun str(x: float) -> string
  fun str(x: bool) -> string
  fun str(x: string) -> string
  ```
  - `doLoadAurai` FunDecl 分支（BuiltinRegistry.h:203-211）计算 defaultCount（见 C4）；TypeDecl 分支不涉及（无 type 声明）。
  - `loadBuiltinAurai`（ModuleManager.cpp:227）追加 `loadAuraiFile("builtin.aurai");`（main.cpp:110/239 两条路径自动覆盖）。
  - 从 functions_ **移除** gc_force（L318）/gc_stats（L319），避免与 aurai 重复注册（doLoadAurai 不查重直接 push_back）；**range×3/channel/sync.* 保留硬编码**。
  - CodeGen special-case（ExprGen.cpp:539-552）与 range 展开（genForStmt）按名特判，与注册来源解耦，不受迁移影响。
  - **加载顺序**：init()（BuiltinRegistry 构造）→ tryLoadAurai 追加，无冲突。
  - **range 迁移留待后续**：aurai 返回类型语法无法表达 Generator（SemAnalyzer.cpp:279-280 IterSemType），若迁移需 `-> generator<T>` 语法扩展——记录 TODO，本次不迁移。
- **C7.2 README 更新**：
  - **新建 READMEs/16-builtins.md**（目录 01-15 + 2 附录，16 号无冲突）：
    1. **.aurai 文件机制**：位置 builtins/；加载规则（loadBuiltinAurai 始终 io.aurai + builtin.aurai；import path 按需 path.aurai；loadedAurai_ 幂等；channel.aurai/mutex.aurai 为文档性不加载）；文件格式（type 前向声明 / `fun (self T) method` / `fun mod.fn` / `fun fn` / throws）；doLoadAurai 三类声明解析。
    2. **完整内置清单**（来源标注：C++ 硬编码 / aurai / 文档）：
       - 类型：int/float/bool/string/None/Io/Path/channel<T>/Mutex/RWMutex/Once/sync.Channel<T>/Optional<T>（+ 虚拟 RWMutexReadView/RWMutexWriteView）
       - string 方法：len/concat/append×4/slice
       - [T] 方法：len/size/empty/capacity/front/back/append/pop×2/remove/insert/clear/reserve/slice
       - channel 方法：send/receive/close；sync.Channel 方法：send/receive/close/is_done
       - Optional 方法：is_none/unwrap；RWMutex 方法：r/w
       - Io（io.aurai 始终加载）9 方法；Path（path.aurai 按需）5 方法 + path.new/join×2
       - 全局函数：range×3/channel/sync.Mutex/sync.RWMutex/sync.Once/sync.Channel×2（硬编码）+ gc_force/gc_stats/int/float/str（builtin.aurai）+ path.*（path.aurai）
    3. **转换函数章节**：int/float/str 语义、throws 与 `!`/try/catch、ValueError。
  - **READMEs/05-functions.md**：默认参数小节——语法 `fun f(a: int, b: int = 10)`；规则（尾部连续、类型可赋值、非常量表达式调用点求值、v1 限制：不支持泛型参数/引用其他参数/跨模块方法）；**默认值调用点求值（区别于 Python 定义时求值）**。
  - **README.md**：目录追加 16-builtins.md。
- **C7.3 aurai 文档文件**（用户审查指定）：
  - **builtins/channel.aurai 追加 sync.Channel 声明**（文档性，权威注册 BuiltinRegistry.h:285-291）：
    ```aurai
    // sync.Channel<T>：sync thread 跨线程通信通道（C++ Runtime: ThreadChannel<T>）
    type sync.Channel<T>
    fun sync.Channel(cap: int) -> sync.Channel<T>
    fun sync.Channel() -> sync.Channel<T>
    fun (self sync.Channel<T>) send(value: T)
    fun (self sync.Channel<T>) receive() -> Optional<T>
    fun (self sync.Channel<T>) close()
    fun (self sync.Channel<T>) is_done() -> bool
    ```
  - **新建 builtins/mutex.aurai**（文档性，权威注册 BuiltinRegistry.h:232-243/309-312）：
    ```aurai
    // 锁原语（文档性质，注册硬编码）
    type Mutex
    type RWMutex
    type Once
    type RWMutexReadView
    type RWMutexWriteView
    fun sync.Mutex() -> Mutex
    fun sync.RWMutex() -> RWMutex
    fun sync.Once() -> Once
    fun (self RWMutex) r() -> RWMutexReadView
    fun (self RWMutex) w() -> RWMutexWriteView
    ```
  - **语义**：不加载（与 channel.aurai 现状一致），仅作内置 API 对照；READMEs/16-builtins.md 清单与之保持一致。若未来实际加载需先解决 `sync.Channel` 类型名含点、`Optional<T>` 返回类型表达等问题（记录 TODO，超本 plan 范围）。

## 4.5 Impact Analysis

| 组件 | 影响 | 破坏性 |
|------|------|--------|
| src/AST/Stmt.h | Param 加 defaultExpr；cloneParam 同步 | 无（新增字段默认 nullptr） |
| src/Parser/TypeParser.cpp | parseParam 加 `=` 分支 | 无（向后兼容旧语法） |
| src/Sema（Symbol/checkCallArgs/DeclChecker/ExprInfer） | 声明规则 + 数量放宽 + throws 补缺口 | ⚠️ throws 内置裸调用从"放行"变"报错"（行为修正，符合语言规范）；顺带 inferCall 内置分支补 inferExpr（无害修正） |
| src/Sema/BuiltinRegistry.h | defaultCount 字段 + 匹配放宽 + gc_force/gc_stats 迁出 | 低：range 等既有重载 defaultCount=0 行为不变；str×4 重载首个命中但返回类型一致 |
| src/Module/ModuleManager.cpp | loadBuiltinAurai 追加 builtin.aurai | 无 |
| src/main.cpp | runCgModule 构造 crossDefaults；generate 新参数 | 低：单文件模式（L165）传空表 |
| builtins/ | 新建 builtin.aurai（加载）、mutex.aurai（文档）；channel.aurai 追加 | ⚠️ builtin.aurai 实际参与注册，其余文档 |
| src/CodeGen（ExprGen/DeclGen/CodeGen.h） | 调用点补实参 + 映射表 + crossDefaults | 低：gc_force/channel/range special-case 不动 |
| runtime/builtin/string | 新增 string_to_int/string_to_float/string_of | 无（纯新增，需重编 runtime） |
| 语言层 | `int`/`float` 函数名与类型名双义 | ⚠️ 文档级：CallExpr vs NamedType 天然分离；`let int = 5` 时 Sema lookup 优先命中变量，遮蔽生效，CodeGen safeName 转义 int_，行为一致 |

## 4.6 Boundary Condition Handling Strategy

| 边界条件 | 现状 | 计划处理 | 测试策略 |
|---|---|---|---|
| `int("")` / `int("   ")` | 无此函数 | ValueError | 负向 |
| `int("abc")` / `int("12abc")` | — | ValueError（endptr 完整消费） | 负向 |
| `int("3.14")` / `int("1e3")` | — | ValueError（strtol 天然拒绝） | 负向 |
| `int("12", 1/37/-5)` | — | ValueError（base 仅 0 或 2~36） | 负向 |
| `int("0x10", 10)` / `int("0b101", 10)` | — | ValueError（前缀与显式 base 不一致） | 负向 |
| `int("2147483648")` | — | ValueError（int32 溢出） | 负向 |
| `int("  +17")` / `int("-17  ")` | — | ± 与前后空白 | 正向 |
| `int("0xff")` / `int("ff",16)` / `int("0b101",0)` / `int("12",5)` | — | 前缀自动判定 + 显式 base | 正向 |
| `float("")` / `float("abc")` / `float("1 2")` | — | ValueError | 负向 |
| `float("1e999")` | — | ±inf（ERANGE 不报错） | 正向 |
| `float("inf")` / `float("Infinity")` / `float("-nan")` | — | 大小写不敏感特殊 token | 正向 |
| `str(true)` | — | Aura 小写 "true"（不照搬 Python True） | 正向 |
| `f(1)` / `f(1,2)` / `f()` | 严格数量报错 | [min,total] 内合法；少于 min 报错 | 正/负向 |
| `fun f(a: int = 1, b: int)` | — | C3.1 声明期报错（非尾部） | 负向 |
| `fun f(a: int = "x")` | — | C3.1 类型不匹配报错 | 负向 |
| `fun f(x: T = 5)`（泛型参数默认值） | — | C3.1 显式拒绝 | 负向 |
| 默认值引用其他参数/未定义标识符 | — | C3.1 inferExpr 声明处报未定义 | 负向 |
| `int("x")` 裸调用 | 不报错（缺口） | C3.4 E016_ThrowsViolation | 负向 |
| `int("x")!` / try/catch | — | 通过 | 正向 |
| 默认值是 GC 对象（string 字面量） | — | C5.1 补实参走 genGcRootedArgs 保护 | 正向（GC 回归） |
| 默认值非常量表达式（同模块） | — | 调用点内联求值（如 range(10)） | 正向 |
| 跨模块函数带默认参数缺参调用 | — | C5.4 crossDefaults 补齐；降级路径 Sema 报错 | 正向/负向（按实施选择） |
| 方法/ctor 默认参数（阶段 4） | — | C5.3；跨模块方法 v1 不支持（TODO） | 按阶段测试 |
| 内置重载匹配稳定性（range 3 重载） | 严格相等 | 放宽后仍需正确命中 | 回归 K 系列 |

## 4.7 Test Plan

- **单元测试（example/test.aura，按项目约定 compile.cmd 非 ASAN + test.exe 运行）**：
  - K-int1: `int("42")`→42；`int("  -17")`→-17；`int("+17")`→17；`int(" 0xff ")`→255
  - K-int2: `int("ff", 16)`→255；`int("0b101", 0)`→5；`int("12", 5)`→7；`int("0x10")`→16
  - K-int3（负向，try/catch 捕获 ValueError）：`int("")`、`int("abc")`、`int("3.14")`、`int("12abc")`、`int("12", 1)`、`int("12", 37)`、`int("0x10", 10)`、`int("2147483648")`
  - K-float1: `float("3.14")`；`float("1e3")`→1000；`float("-2.5e-2")`；`float(" 42.5 ")`
  - K-float2: `float("inf")`、`float("Infinity")`、`float("nan")`、`float("1e999")`→inf
  - K-float3（负向）：`float("")`、`float("abc")`、`float("1 2")`
  - K-str1: `str(42)`→"42"；`str(3.14)`；`str(true)`→"true"；`str("abc")`→"abc"
  - K-def1: `fun add(a: int, b: int = 10) -> int`：`add(1)`→11、`add(1,2)`→3
  - K-def2（负向）：`add()` 数量不足；`fun bad(a: int = 1, b: int)` 声明期报错；`fun bad2(a: int = "x")` 类型报错
  - K-def3: `fun greet(n: string = "world") -> string` + `greet()`，GC 安全
  - K-def4（阶段 4）：record 方法带默认参数调用
  - K-throws1（负向）：`let x = int("42")` → E016；`let x = int("42")!` 通过
  - K-shadow（兼容性）：`let str = 5` + `str(5)` → Sema 遮蔽行为回归
- **集成**：`.\build\aurac.exe example/test.aura --cpp example/test -o example/test.exe` 编译运行全绿；used/ 系列回归；多文件并行三档（-j1/-j2/默认）产物逐字节一致。
- **边界映射**：4.6 每行对应一个用例。
- **回归风险**：findFunction/findMethod 放宽影响既有内置调用（range/channel/io/path）→ 全量 K + used/ 回归守护；runtime 重编后需重链。

## 4.8 Implementation Steps (Ordered, with Dependencies)

**阶段 1 — 默认参数基础设施（函数）**
1. C1：AST Param.defaultExpr + cloneParam。验证：`cmake --build build` 编译通过。
2. C2：parseParam 解析 `= expr`。验证：`fun f(a: int = 1)` 能解析（临时打印或直接进入后续阶段验证）。
3. C3.1/C3.2：checkFunBody/checkMethodBody 声明规则检查 + SymParam.defaultExpr/hasDefault + declareDecl 拷贝。验证：负向用例（非尾部/类型不匹配/泛型参数）报错。
4. C3.3：checkCallArgs 数量放宽（新参数 defaultCount）+ inferCall/inferMethodCall 各分支传 defaultCount。验证：`f()` 报错、`f(1)` 通过。
5. C5.1：CodeGen fnDefaultArgs_（长度=形参总数，nullptr=无默认）+ genCallExpr 补齐 + genGcRootedArgs 保护。验证：K-def1/K-def3 编译运行正确。
6. **回滚点**：若调用点补齐破坏既有调用，回退 C5.1 单独保留阶段 2。

**阶段 2 — int()/float()/str() + throws 修复 + builtin.aurai 迁移**
7. C6：runtime string_to_int/string_to_float/string_of。验证：`cmake --build runtime/build` + 临时 C++ 或 Aura 用例。
8. C4：BuiltinGlobalFn/BuiltinMethod defaultCount + findFunction/findMethod 放宽 + doLoadAurai 计算。验证：Sema 命中 `int("42")` 与 `int("42",16)`。
9. C7.1：builtins/builtin.aurai + loadBuiltinAurai 追加 + 从 functions_ 移除 gc_force/gc_stats（range 保留）。验证：`for i in range(5)` 回归；`gc_force()` 返回语义不变。
10. C3.4：三处 throws 检查补缺口 + inferCall 内置分支补 inferExpr。验证：裸调用报 E016。
11. C5.2：genCallExpr 全局函数映射表 + 内置默认参数补齐（base=10 字面量）。验证：`let x = int("ff", 16)!` = 255；`let y = int("42")!` = 42。
12. 全量测试 + 回归 + 并行一致性（-j1/-j2/默认三档产物一致）。

**阶段 3 — aurai 文档文件（纯文档）**
13. C7.3：channel.aurai 追加 sync.Channel；新建 builtins/mutex.aurai。验证：人工核对与硬编码注册一致（不加载）。

**阶段 4 — 方法/构造函数/闭包默认参数 + 跨模块 + README**
14. C5.3：methodDefaultArgs_/ctor 补齐（genMethodCall/ctor 调用点）。验证：K-def4 record 方法默认参数。
15. C5.4：crossDefaults 机制（generate 新参数 + main.cpp 构造 + genMethodCall isNs 补齐）或降级（Sema 显式报错）。验证：多文件模块带默认参数函数跨模块调用。
16. 闭包默认参数（FunExpr）：若实现复杂 → 拆独立 issue 记录 TODO，不阻塞主链路。
17. C7.2 README（16-builtins.md + 05-functions.md 默认参数小节 + README.md 目录）+ TODO.txt 标记完成。

**依赖关系**：阶段 1 → 2（aurai 默认参数解析依赖 C2，defaultCount 计算依赖 Parser）；阶段 3 无依赖；阶段 4 依赖 1。内置 int 的 base=10 依赖阶段 1 的默认参数基础设施（Sema/注册表）+ C4。

## 4.9 Risks & Mitigations

- **风险 1（中）**：findFunction/findMethod 放宽引入歧义（str×4 重载首个命中、range 匹配）。缓解：str 重载返回类型一致无影响；range/sync.Channel defaultCount=0 严格匹配不变；阶段 2 测试锁定回归。
- **风险 2（中）**：`int`/`float` 函数名与类型名双义 + 变量名遮蔽。缓解：Sema 路径分离（CallExpr lookup 优先变量）；`let int` 时 CodeGen safeName 转义 int_ 一致；README 注明；C5.2 映射表必须在 safeName 前。
- **风险 3（中）**：调用点补实参遗漏某调用路径（genCallExpr/genMethodCall/io _sync/spawn/闭包）。缓解：统一在 argExprs/mArgExprs 构造处补齐；阶段测试覆盖 io 调用与 spawn；defaultExpr 补齐参数 inferredType 缺失时保守按堆类型包装。
- **风险 4（中）**：跨模块默认参数机制（generate 签名变化 + exports AST 生命周期）。缓解：AST 指针来自 ModuleInfo.exports 常驻内存，多文件 CodeGen 并行只读安全；降级路径（Sema 报错）已备。
- **风险 5（低）**：strtol/strtod 平台差异。缓解：view() 局部拷贝 + endptr 完整消费显式判定；溢出显式检查（不依赖 errno 平台差异）。
- **风险 6（低）**：parseParam `=` 与既有语法冲突。缓解：仅在 `:` 类型解析后 match Assign；回归旧语法。
- **假设**：默认值表达式调用点求值（每次调用重新计算），区别于 Python 定义时求值——README 明确；v1 限制（泛型参数默认值、跨模块方法默认值、闭包默认值）记录 TODO。
