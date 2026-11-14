# 审查报告：`plan/编译期缺陷修复-bug120-124.md`

- **审查对象**：`plan/编译期缺陷修复-bug120-124.md`（352 行，2026-11-14 17:49 版）
- **审查方式**：全文逐条核 + 源码直接实证（本报告引用的 `文件:行号` 均为本轮实读，不复用记忆）
- **总裁决**：**需修改（Changes Requested）** —— 3 项 🔴 必须改完再放行（其中 1 项需主人裁定）；9 项 🟡

---

## 一、已实证成立、可直接沿用的部分

1. **A1/A2/A3 架构事实成立**：分派在 `src/CompileTime/Evaluator.cpp:128`（`evalExpr`），`:143 evalBinary / :158 evalCall / :159 evalMethod / :160 evalMetaCall`；块作用域 `Env local`（`:399`）；`CallGuard` 在 `Evaluator.h:118-130`，上限 `maxCallDepth=256`（`Evaluator.h:43`）。
2. **bug-120 定性成立**：深度检查在调用入口，C++ 栈每层消耗多帧，深递归先 `0xC00000FD`。
3. **bug-121 前提成立**：运行期 list/Optional 是堆指针（`src/CodeGen/TypeMap.cpp:529-535`；`runtime/builtin/optional.h:28 struct Optional : GcObject`）；Sema 对 `==/!=` 不做类型限制（`src/Sema/Checker/ExprInfer.cpp:591-602`）；编译期 `Value::equals` 纯结构递归（`src/CompileTime/Value.h:62-83`，字段 `:34-45` 无身份位）→ 漂移真实存在。
4. **bug-122 AST 事实成立**：`src/Parser/ExprParser.cpp:500-505` + `parseMetaSuffix`（`:521-539`）产出 `MetaCallExpr(callee=MetaMemberExpr(...))`；`evalMetaCall` 在 `Evaluator.cpp:529-530` 只接受 `MetaValueRefExpr`；`callBuiltinMethod` 只消费 `call.args` 与 `fail(call,...)`，不碰 `call.object`（`Evaluator.h:93-94`）→ 抽 `callMethodOn` 的方案可行。
5. **bug-124 前提成立**：`Kind::Union` 无构造点（`Value.h:73/111` 等引用点与 plan D1 一致），"标注 + payload 回写"（`EvalPass.cpp:535-542`）属实。
6. **A19 运行期变量拦截真实存在**：`Evaluator.cpp:139-140`（引用）、`:283-284`（赋值）——这恰好推翻 §7.1 的部分用例构造（见 🔴-2）。

---

## 二、🔴 阻塞问题

### 🔴-1 `task.h` 骨架不具备「续体 + 对称转移 + 异常穿透」，核心承诺在骨架下不成立

计划 §4.3 骨架（plan:204-222）只有 `std::suspend_always final_suspend()`、占位 `struct Awaiter { ... };`，`wait()` 描述为 `while(!h.done()) h.resume()`（plan:197）。缺三件关键件：

1. **无续体（continuation）**：promise 里没有保存父协程句柄的字段，子协程 `final_suspend` 无法把控制权交回父协程；
2. **无对称转移**：`Awaiter::await_suspend` 必须返回 `std::coroutine_handle<>`（子句柄），让当前 resume 帧退栈后再恢复子协程。否则两条路都是错的：
   - 若在 `await_suspend` 里同步 `child.h.resume()` ⇒ C++ 栈仍为 O(depth)，**bug-120 原样未修**；
   - 若"挂起父 + 无人恢复" ⇒ `while(!h.done()) h.resume()` 会对一个悬挂在 `co_await` 的协程再次 `resume`，属 UB（崩溃）。
3. **`await_resume` 未要求重抛子协程异常**：骨架只在 `wait()` 层提重抛。父协程 `co_await` 失败子协程时，若 `await_resume` 不重抛 `err`，会拿默认构造的 `Value` 继续跑 ⇒ **错误被静默吞掉**（可能产出错误的编译结果而非诊断），比崩溃更糟；违背 A5 的传播契约。

最小补齐骨架（实施批可直接用作基线，仍应在 change.md 中定稿）：

```cpp
template <class T>
struct Task {
    struct promise_type {
        T value{}; std::exception_ptr err;
        std::coroutine_handle<> continuation = std::noop_coroutine(); // 父协程
        Task get_return_object() {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_never initial_suspend() noexcept { return {}; }
        struct FinalAwaiter {
            bool await_ready() noexcept { return false; }
            std::coroutine_handle<> await_suspend(
                std::coroutine_handle<promise_type> h) noexcept {
                return h.promise().continuation;      // 对称转移回父协程
            }
            void await_resume() noexcept {}
        };
        FinalAwaiter final_suspend() noexcept { return {}; }
        void return_value(T v) { value = std::move(v); }
        void unhandled_exception() { err = std::current_exception(); }
    };
    struct Awaiter {
        std::coroutine_handle<promise_type> h;
        bool await_ready() const noexcept { return !h || h.done(); }
        std::coroutine_handle<> await_suspend(std::coroutine_handle<> parent) noexcept {
            h.promise().continuation = parent;        // 登记续体
            return h;                                 // 对称转移到子协程
        }
        T await_resume() {
            if (h.promise().err) std::rethrow_exception(h.promise().err);
            return std::move(h.promise().value);
        }
    };
    // operator co_await / move-only / 析构 destroy / wait()（root 级重抛 err）
};
```

**必须补的验收用例**：异常穿透——`#{ return #boom(1) + 1 }`（`#boom` 内 `fail`）必须报原诊断，且不得以默认值继续。

### 🔴-2 §7.1 / §3.6 的 `==` 用例构造错误：会先撞 A19，期望值根本到不了

- plan:289-290 `EqListIdentity`：`let a=[1,2]; let b=[1,2]; #(a==b)` → 期望 false
- plan:290 `EqListSameBinding`：`let a=[1,2]; #(a==a)` → 期望 true
- plan:111 §3.6：`let a = [1,2]; let b = a; a == b` → 编译期 true

模块级 `let` 是**运行期声明**；EvalPass 求值顶层 `#(...)` 时 env 无 `a/b`，`runtimeNames_` 命中 ⇒ 报 `cannot access runtime variable 'a' in compile-time block`（`Evaluator.cpp:139-140`）。`EqRecordContent` 若同法引用运行期绑定同理。

正确写法（断言可达）：

- `#{ let a = [1,2]; let b = [1,2]; return a == b }` → **false**
- `#{ let a = [1,2]; return a == a }` → **true**
- `#{ let a = [1,2]; let b = a; return a == b }` → **true**
- record 同理：构造与绑定都放进 `#{}` 内返回。
- `#(none() === none())` 这类无自由变量的用例可保留顶层 `#(...)`。

### 🔴-3 `===` 是**新增语言语法**，不属缺陷修复；§10 漏了这项裁定

plan:15 / plan:256（自认「⚠️ 新语法」）/ plan:171-178（语义判定表「写入规范与手册」）均表明这是语言面扩展。但 §10（plan:347-352）只问「协程是否接受 / record 分支 / 是否要 `!==`」，**没有问「是否接受引入 `===` 本身」**——这正是最该先裁定的。建议二选一：

- **拆案**：bug-121 只做「编译期 `==` 向运行期对齐（list/Optional 身份）」，`===` 另立 feature issue；
- **或**本 plan §10 增列该裁定项，获明确放行后再纳入（并同步 v0.4.1 规范、`READMEs/17-compile-time.md`）。

---

## 三、🟡 重要问题（不阻塞放行，但须改）

1. **身份赋值点两处清单不一致**：plan:49 写 `:903 slice(List)`、`:911 append(List)`；plan:164 写 `/900/911`。属计划内部矛盾（不依赖实施偏移），须统一；并明确 record（`:192`）与 Enum（`:224`）是否需要 `ident`——按 §4.2 语义表它们走内容/变体比较，`ident` 无消费者，建议不加。
2. **Impact Analysis 漏「Evaluator 的直接使用者」**：plan:259 断言「仅被 EvalPass/main 使用」，但 `test/compile_time/test_eval.cpp`（27 TEST）是否直接构造 `ct::Evaluator` 未核实；若直接使用，`task<>` 签名变更会先红在测试 TU。实施前 `grep -rn "ct::Evaluator\|Evaluator eval" test/` 兜底。
3. **C++20 协程前提未验证**：`task.h` 依赖 `<coroutine>`，plan 未提 `CMakeLists` 语言标准；若工程仍 C++17，S4a 直接失败。实施前确认/提升 `CMAKE_CXX_STANDARD`。
4. **Sema 是 `===` 的唯一闸门**：CodeGen 兜底会原样输出 op（`ExprBinary.cpp:256-261`），任何绕过 `inferBinaryExpr` 的路径都会生成非法 C++ `a === b`。建议 Sema + CodeGen 双保险（CodeGen 加一行 `fail`）。
5. **`append`/`slice` 身份语义需落文档**：运行期 `Array<T>::append(T)` 返回 `void` 且原地改（`runtime/builtin/array.tcc:230`），编译期 append 产生新 Value/新 ident；`a == a.append(x)` 运行期根本写不出来（void），编译期若写为 false——不构成漂移，但须在 §3.6/§9 写明「append 的返回值不承诺与接收者同身份」。
6. **兼容性证据面太窄**：plan:263 只引 `test_eval.cpp`，未列 `example/used/`、`builtins/*.aurai`、其余 test 套件。建议落可复现命令：`grep -rn "#(" example/ builtins/ --include=*.aura` 逐条核 `==`。
7. **回归基线数字冲突**：plan:308 写 1728，仓库 `AGENTS.md` 记 1383（2026-09-29）。以实施当天全量实测为准，文档不写死。
8. **用例覆盖缺口**：无 `!=` 用例（身份语义的否定分支未钉）；`===` 跨类型/空容器只有表格行（plan:272-273）无用例名；`StrictEqRuntimeRejected` 未指明落在哪个 Sema 测试文件。
9. **「7 个函数」与清单不符**：plan:191 说 7 个，实际列 8 个 +「全部 `eval*`」；真正必须协程化的是**所有直接/间接调用 `evalExpr` 的函数**（`evalUnary/evalConditional/evalList/evalRecord/evalMatch/...`），数量远超 8，工作量估算需按实复核。好消息：漏一处即编译不过（`co_await` 只能在协程内），不会静默错。

---

## 四、对 §10 三问的意见

1. **协程化是否接受**：方向正确（唯一能"真到 256 再报错"的方案），但必须先补 🔴-1 的续体/对称转移/异常穿透，再按 S4a→S4c 小步推进。若先止血，同意 fallback B'（显式帧 + 栈水位探测），但它只能"报错不崩"，须在 progress 如实标记未完成。
2. **record 的 `==` 保持内容比较**：同意。运行期 record `==` 是 `Comparable.equal`（`runtime/builtin/interfaces.h:93-97`），编译期保持结构即一致；未 impl Comparable 在编译期不报错作为已知差异登记，合理。
3. **`!==`**：同意不引入（`not (a === b)` 即可）。但请把「是否引入 `===` 本身」补进裁定清单（🔴-3）。

---

## 五、修复顺序建议

- **P0（放行前必须改）**：🔴-1 骨架补齐 + 异常穿透验收用例；🔴-2 全部 `#(...)` → `#{}` 改写；🔴-3 §10 增列 `===` 裁定（附拆案选项）。
- **P1**：🟡-1/2/3/4（内部一致性 + 影响面 + 构建前提 + 双保险）。
- **P2**：🟡-5/6/7/8/9（文档与用例补齐）。

---

## 六、本报告结论的可复跑验证

- `rg -n "cannot access runtime variable" src/CompileTime/Evaluator.cpp` → `:139-140` / `:220-221` / `:283-284`
- `rg -n "suspend_always final_suspend|struct Awaiter" plan/编译期缺陷修复-bug120-124.md` → `:212` / `:218`
- `rg -n "EqListIdentity|EqListSameBinding" plan/编译期缺陷修复-bug120-124.md` → `:289-290`
- `rg -n "void Array<T>::append" runtime/builtin/array.tcc` → `:230`
- `rg -n "while\(!h.done" plan/编译期缺陷修复-bug120-124.md` → `:197`（wait 描述）
