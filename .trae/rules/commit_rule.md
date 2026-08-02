---
alwaysApply: false
description: Aura 仓库 commit message 编写规范，模板源自 387e11b（字符串拼接性能优化 + 分代分页 GC）
---
# Commit Message 规范（Agent Rule）

## 1. 标题格式
```
type(scope): 中文标题
```
- **type 白名单**：`feat` / `fix` / `perf` / `refactor` / `docs` / `test` / `chore`
- **scope**：受影响的子系统（gc / codegen / sema / parser / string / array / sync / builtin / runtime / compile …）
- **标题全中文**，概括本轮全部改动；涉及多个独立主题时用 ` + ` 连接：
  `perf(gc): 字符串拼接性能优化 + 分代分页 GC 阶段 2/3 + 4 项 bug 修复`
- 大改动不必把 scope 堆在标题里（如 `feat(sema,codegen,runtime)` 尽量少用），
  子系统归属用正文条目的 `type(scope)` 分别标注。

## 2. 正文结构（模板示例，源自 387e11b）
```
perf(gc): 字符串拼接性能优化 + 分代分页 GC 阶段 2/3 + 4 项 bug 修复

字符串拼接性能优化（解决 1000 万次 s = s + "a" 奇慢）：
- fix(codegen): append 返回值赋回 targetBase.get()，修复容量不足时新对象丢失
- perf(gc): GcRootHandle 改用 thread_local 侵入式链表，消除 1.4 亿次 mutex 操作
- perf(string): intern_string 新增 64 槽 LRU 线程局部缓存，热点字面量无锁命中

分代分页 GC 阶段 2/3（generational_paged_gc）：
- feat(gc): 新增 pages.h 集中页类型（Page/MediumPage/LargePage/PageClass）
- feat(gc): 中页/大页分配路由 + 中页滑动窗口 compact + 大页 mark-sweep
- feat(gc): Minor/Mixed/Major GC 三级触发 + freeMediumPages 高水位归还

配套 bug 修复（bugfix-report.md）：
- fix(string): intern L1 缓存在 compaction 后悬垂，新增 clear_intern_cache()
- fix(string): append(const GcString*) 过早 flatten + rope 扩容失效（O(n²)→O(n)）

涉及文件：
- src/CodeGen/ExprGen.cpp: append 返回值赋回
- runtime/gc/gc.h: GcRootHandleBase + ThreadRootList + GC 阈值
- runtime/builtin/string.h, string.cpp: intern LRU + append/slice GC 保护
```

## 3. 正文规则
- **按主题分段**：每个独立主题一个段落（空行分隔），段首写清问题背景/目标（一句话）。
- **条目格式**：`- type(scope): 具体改动`，每条说明"改了什么 + 为什么/解决了什么"。
- **配套修复单独分段**：使用"配套 bug 修复"标题，逐条 `- fix(scope): ...`。
- **结尾必须"涉及文件："**：列出关键文件路径与对应作用，按子系统分组。
- **全中文**（代码标识符/路径除外）；不使用 emoji。

## 4. 禁止事项
- 禁止笼统标题（如 `fix bugs`、`update code`）——标题必须能看出主题。
- 禁止正文只堆文件名、不写作用。
- 禁止把无关改动混入同一 commit（一个 commit 一个逻辑主题）。
- 禁止在 message 中写行号/内部临时代号（如 T1-3）而不解释含义。
