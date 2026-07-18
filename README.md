# Aura 语言参考手册 v0.7

> 对应编译器版本：Aura v0.7
> 日期：2026-06-21

---

## 目录

- [1. 简介](READMEs/01-introduction.md)
- [2. 词法基础](READMEs/02-lexical.md)
- [3. 类型系统](READMEs/03-types.md)
- [4. 变量与常量](READMEs/04-variables.md)
- [5. 函数](READMEs/05-functions.md)
- [6. 泛型](READMEs/06-generics.md)
- [7. 方法与接口](READMEs/07-methods-interfaces.md)
- [8. 控制流](READMEs/08-control-flow.md)
- [9. 模式匹配](READMEs/09-pattern-matching.md)
- [10. 错误处理](READMEs/10-error-handling.md)
- [11. 并发](READMEs/11-concurrency.md)
- [12. 模块与导入](READMEs/12-modules.md)
- [13. `Io` 能力对象 API](READMEs/13-io-api.md)
- [14. `path` 内置模块](READMEs/14-path-module.md)
- [15. 完整示例](READMEs/15-example.md)
- [附录 A：`fun` 关键字的三种用法](READMEs/appendix-a-fun-usage.md)
- [附录 B：速查表](READMEs/appendix-b-cheatsheet.md)
- [附录 C：待开发特性](README.md#L31)

---

## 附录 C：待开发特性

> 最后更新：2026-07-18

### C.1 语言 / 运行时特性

| 特性 | 状态 | 计划文档 / 说明 |
|------|:---:|------|
| **`range()` 内置函数**（`for i in range(n)`） | ✅ 已实现 | `Iter<T>` 泛型迭代器类型 |
| **`sync(max=N)` 有界并发** | ✅ 已实现 | [concurrency_lang_spec.md](plan/concurrency_lang_spec.md) — `bounded_sync` |
| **`channel<T>` 协程通道** | ✅ 已实现 | [concurrency_lang_spec.md](plan/concurrency_lang_spec.md) — send/receive/close/for-in |
| **`sync for` 并行迭代器** | ✅ 已实现 | [concurrency_lang_spec.md](plan/concurrency_lang_spec.md) — 语法糖展开 |
| **闭包参数类型推断**（`let op: fun(int,int)->int = fun(a,b){...}`） | 🔴 未实现 | 规范 §5.3 已定义 |
| **异步 I/O**（io_uring / OVERLAPPED / epoll） | 🔴 未实现 | 当前所有 I/O 均为纯阻塞实现 |
| **接口类型擦除**（`interface` 多态派发） | 🔴 未实现 | — |

### C.2 跨模块可见性

| 特性 | 状态 | 说明 |
|------|:---:|------|
| **跨模块 Sema 类型可见性**（Phase A） | ✅ | `ModuleExports` + `importExports` + 拓扑注入 |
| **`pub` 关键字可见性控制**（Phase B） | ✅ | 前缀修饰符，默认私有 |
| **`.aurai` 内置接口声明** | ✅ | `builtins/io.aurai` + `builtins/path.aurai` |
| **多文件模式 Sema** | ✅ | `compileMultiFile` 中每模块运行 SemAnalyzer |
| **统一诊断引擎**（源码上下文 + fix-hint + 错误码） | ✅ | `DiagnosticEngine` |
