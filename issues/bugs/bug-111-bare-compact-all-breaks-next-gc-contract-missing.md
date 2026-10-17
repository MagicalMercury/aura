---
type: bug_report
module: runtime/gc
sub_module: GcHeap::compact() 入口缺「所有 TLAB 已 flush」不变量自检——未 flush 的 TLAB 对象不在对象表、却随 rebuildPageList(All) 整页释放 ⇒ 根/字段悬垂，下一轮 GC 崩溃（帧随堆布局漂移）
status:
  - fixed
severity:
  - medium
discover_date: 2026-10-17
discovered_by: Hermes（bug-89 修复批的三步对照 E 系列实验实测暴露；主 Agent 复核）
related_issues:
  - bug-89（同一批实验的副产物；bug-89 已修）
  - "[[bug-89-globalroots-update-missing-isgcaddress-guard]]"
tags:
  - gc
  - compact
  - contract
  - testability
---

# [x] 【裸调 `compact(CompactScope::All)`（不经 mark/sweep）会破坏后续 GC —— 契约缺失】

**状态**：`[x] 已修复`（2026-10-17，见 §8）
> **⚠️ 归因订正（2026-10-17，只读调研批 + 主 Agent 复跑）**：本笔记原判断「`compact()` 必须在标记/清扫之后调用」**不成立**。真正的缺失不变量是「**所有 TLAB 已 flush**」；`safepoint.cpp:177-186` 本身就是**合法**的 compact-only 生产路径（注释「compact 不依赖 marked 标志」，安全性来自同函数 `:127` 的 `flushTlab()`）。详见 §6。
**严重度**：**medium**
**发现场景**：bug-89 修复批里，新用例最初直接调 `GcHeap::instance().compact(CompactScope::All)`（不先跑一轮 GC）⇒ 用例**自身**能过，但**全量跑到中间** `rc=139`，崩溃在 `updateObjectAllFields`。

---

## 0. 一句话摘要

`GcHeap::compact(CompactScope)`（`runtime/gc/compact.cpp:63`）隐含一条**未写进契约的调用前提**：必须在**标记/清扫之后**（本轮 forwarding 信息由该次 GC 建立）调用。裸调 `compact(All)`（不经 `markPhase`）时：
- 本次 `compact()` **能正常返回**（用例自身绿）；
- 但**下一轮 GC** 会在 `updateObjectAllFields` 崩溃（E7 稳定复现 `rc=139`，全量必现）；
- 加一次 `gc_force_major()` 前置后全量绿（E8）。

⇒ 当前只能靠调用方自律（测试代码尤其容易踩），`compact()` 没有契约断言/自检。

## 1. 现场（实测对照，E 系列逐层切变量）

| 编号 | 变量 | 结果 | 结论 |
| :-: | :--- | :--- | :--- |
| E1 | 去掉 bug-89 守卫 + 新测试登记 | 全量 rc=139（3/3） | 与守卫无关 |
| E2 | 有守卫 + **移除**新测试 TU | 1583/1583 rc=0 | 新 TU 是触发项 |
| E4 | 有守卫 + 测试体换成**空用例** | 1584/1584 rc=0 | 与加 TU/链接顺序/堆布局无关，是**测试体行为** |
| E5 | 只留「脏值根」用例 | 全量 rc=139 | — |
| E6 | 只留「有效根」用例 | 全量 rc=139 | **与脏值无关** |
| **E7** | **只裸调 `compact(All)`**（无句柄、无脏值） | 全量 rc=139 | **根因 = 裸调 compact** |
| **E8** | `gc_force_major()` + `compact(All)` | 1584/1584 rc=0 | 前置真实 GC 即安全 |

**崩溃点**：下一轮 GC 的 `updateObjectAllFields`（E7 稳定复现）。本次 `compact()` 返回正常，问题在**后续**。

## 2. 影响与触发条件

- **触发面**：所有直接调用 `compact()` 而不先走一轮标记/清扫的代码——**首先是测试**（本批被迫改成「先 `gc_force_major()` 再 `compact(All)`」才获得稳定全量）；
- **表现**：延迟崩溃（本轮绿、下轮红）⇒ 极难定位，容易被误判成"新加的用例破坏了 GC"；
- **现状**：`runtime/gc/compact.cpp:63` 的 `compact()` 入口没有前置状态断言，也没有注释级契约说明（`shouldCompact()` 只管"要不要压"，不管"现在能不能压"）。

## 3. 修法建议（待裁定，三种取向）

1. **契约断言（首选，dev/test 构建）**：`compact()` 入口自检「本轮 forwarding 信息已由 mark 建立」（例如记录 `lastMarkEpoch_` 与当前 epoch，或检查 `compactEntries_` 的来源状态），不满足时**明确报错/abort 并打印调用栈**，而不是让下一轮 GC 神秘崩溃。
2. **自动前置**：`compact()` 内部检测到未标记就自动跑 `markPhase()`（改变语义/开销，需评估；不推荐默认启用）。
3. **纯文档化**：在 `compact()` 头注释写死契约 + `REQUIRES` 注释，并在测试 helper 统一封装（成本最低，但不防未来调用方）。

> 取向需主人裁定；本笔记只登记"契约缺失"这一事实与证据。

## 4. 验证建议

- **复现脚本**：E7 形态（裸调 `compact(All)` 后连续跑两轮 `gc_force_major()`）⇒ 现版本第二轮 rc=139。
- **修后**：断言路径下裸调应在 `compact()` 入口即报错（exit code 明确、信息含"compact requires preceding mark/sweep"），全量 0 failed + Examples 17/17。

## 5. 关联

- **bug-89**：同一批实验的副产物；bug-89 已按其范围只修守卫（本项明确列为"范围外观察"）。
- 测试侧现状：`test/rt/test_enum_gc.cpp:44` 的 `forceCompactAll()` **已**先 `gc_force_major()`（安全）；`test/rt/test_gc_bug89.cpp` 亦同。

## 6. 归因订正与实测（2026-10-17）

### 6.1 原归因为什么不成立
| 原表述 | 实测反证 |
| :--- | :--- |
| 「必须在标记/清扫之后调用」 | `safepoint.cpp:177-186`（`needCompactOnly`）**生产路径**专门跳过 mark-sweep 直接 compact，注释明写「compact 不依赖 marked 标志，可独立执行」 |
| 「加 `gc_force_major()` 前置就安全（E8）」 | `forceGc()` **首行**就是 `flushTlab()`（`safepoint.cpp:304-308`）⇒ 起作用的是 flush，不是 mark |
| 「稳定崩在 `updateObjectAllFields`」 | 本批稳定复现的是 `mark_sweep.cpp:131`（全局根槽）与 `mark_sweep.cpp:486`（sweep finalizer 循环）⇒ **帧取决于哪条悬垂指针先被踩到**，不宜写死 |

### 6.2 真因链条（`文件:行号`）
1. `compact()` 入口（`compact.cpp:63-65`）直接 `computeForwardingAddresses(scope)`——**不 flush TLAB、无状态断言**；
2. `computeForwardingAddresses`（`:87-101`）的候选集**只**取 `youngObjects_` + `oldObjects_`；
3. TLAB 快路径对象只进 `tlab->localYoung`（`alloc.cpp:96-97`），**不进** `youngObjects_`，但所在页已链进 `headPage_`（`tlab.cpp:24-28`）；
4. ⇒ 未 flush 对象不在 `compactEntries_`（`:197`）⇒ `setForwardingPtr` 从未调用 ⇒ `updateAllReferences` 对它无操作；
5. `rebuildPageList(All)`（`:232-255`）**无条件**遍历 `headPage_` 全链 `VirtualFree(..., MEM_RELEASE)`（`:241-250`）；
6. ⇒ 第 4 步漏掉的对象内存被解除映射 ⇒ 指向它的根槽/字段/`oomError_` 立刻悬垂；
7. 下一轮 GC 在 `mark_sweep.cpp:131`（缺 `isGCAddress`，见 bug-110）或 `sweepPhaseAll` finalizer 循环（`:486`）解引用 ⇒ **AV**。

### 6.3 实测矩阵（主 Agent 独立复跑，与调研批一致）
| mode | 序列 | rc（N=5） |
| :-: | :--- | :-: |
| 0 | `registerThread` → 400 对象 → **裸调 compact(All)** → `gc_force_major()` | **139**（帧 `mark_sweep.cpp:131`） |
| 1 | 同上，**compact 前只加 `flushTlab()`**（不跑 mark） | **0** ← 决定性对照 |
| 2 | 同上，compact 前 `gc_force_major()`（E8 形态） | 0 |
| 3 | mode 0 + compact 后再分配 200 | 139 |
| 4 | 只分配 + 两次 GC（不做 compact，基线） | 0 |
| 5 | mode 0 + compact 后再分配 3000 | 139（帧 `mark_sweep.cpp:486`） |
| 机制隔离 | 单对象 + 单全局根：**flush 后** compact ⇒ 根槽被改写（对象真搬）；**不 flush** ⇒ 根槽指向已释放页 | 0 / 139 |

### 6.4 修法（按 准确性 > 可维护性 > 复杂性）
- **P0（首选）入口不变量自检**：遍历 `tlabList_`（`gc.h:650-651`），任一 `Tlab` 满足
  `curPage != nullptr || !localYoung.empty()` ⇒ **明确 abort/报错**（信息含 `compact requires all TLABs flushed`）。
  - **必须遍历 `tlabList_`**，只查 `tlab_` 会漏掉其它线程（E7 的真实情形）。
  - 附注：GC 自己的 mark worker 线程不注册 TLAB ⇒ 不会误报。
- **② 自动前置 `flushTlab()`**：只覆盖**调用线程**，多线程不安全 ⇒ 仅作辅助（单线程/测试友好），不能替代 ①。
- **②' 自动前置 `markPhase()`**：引入「mark 了没 sweep」的新中间态（标记位残留、`rememberedSet_` 未清）⇒ **不推荐**。
- **③ 文档改正**：把「必须紧跟 mark/sweep」改为「要求所有 TLAB 已 flush」。

### 6.5 调用点清点（生产 7 处均安全，危险面 = 外部直接调用者）
`mark_sweep.cpp:37/:527`、`parallel_mark.cpp:194`、`safepoint.cpp:183/:185/:260/:262/:333/:335` —— 全部在
`safepoint.cpp:127` 的 `flushTlab()` 之后；`test/` 两处 helper 已先 `gc_force_major()`。
⇒ 生产**无**危险裸调；风险在**外部直接调用 `GcHeap::compact()`** 的代码（测试、工具、插件、未来新增）。

---

## 8. 修复记录（2026-10-17）

### 8.1 实现（按 §6.4 的 P0 + ③，不扩面）

1. **可测判据**：`GcHeap::hasDirtyTlabs()`
   - 声明：`runtime/gc/gc.h`（紧跟 `flushTlab/refillTlab/ensureTlab/releaseTlab` 之后，只加声明）；
   - 实现：`runtime/gc/compact.cpp`（**未**改 `gc.h` 既有成员、**未**新增源文件）；
   - 判据：遍历 `tlabList_`（**持 `tlabList_m_`**），任一 `Tlab` 满足
     `curPage != nullptr || !localYoung.empty()` ⇒ `true`；
   - **必须遍历 `tlabList_`**：只查本线程 `tlab_` 会漏掉其它 mutator（§6.4 的核心结论）。
   - 命名为**非 const**方法：`tlabList_m_` 不是 `mutable`，而 brief 限定 `gc.h` 只加声明，
     故不为此改既有成员（`instance()` 返回非 const 引用，调用方无感）。
2. **入口自检**：`compact()` 第一行调用它，命中 ⇒ 复用仓内既有「报错 + `std::abort`」方式
   （与 `safepoint.cpp:210-213` 的 `STW DEADLOCK` 同款：`std::fprintf(stderr, ...)` + `std::abort()`），
   信息含 `compact requires all TLABs flushed (call flushTlab() before compact)`。
3. **契约注释改正**（`compact()` 头注释）：要求 = **所有线程**的 TLAB 已 flush
   （`safepoint.cpp:127` 即此保证；`forceGc()` 首行同样 flush）；**不要求**先跑 mark/sweep
   （`safepoint.cpp:177-186` 的 compact-only 路径合法），并把违反后的机制链条逐条写清。
4. **刻意不做**：不自动 `flushTlab()`（只覆盖调用线程 ⇒ 多线程仍不安全，属把危险面缩小而非消除）；
   不自动 `markPhase()`（引入「mark 了没 sweep」的新中间态）。与 §6.4 一致。

### 8.2 实施中新发现（§6.4 附注的补充，重要）

§6.4 只说「GC 自己的 mark worker 不注册 TLAB ⇒ 不会误报」。实测补充两点：

- **空闲的池 worker 也会持有脏 TLAB**（它上次分配后没人替它 flush）⇒ 判据为真**是正确命中**，
  不是误报：此时 direct `compact()` 确实会漏搬那些对象。
- 因此在本仓的**整套单测进程**里（同进程、且早前套件留下了 32 个仍注册的池 worker），
  任何**外部直接**调 `compact()` 的代码都会命中自检并终止。这不是缺陷：生产侧所有
  `compact()` 调用点都在 STW（`:127` flush）之后；它恰好证明本自检能抓到真违规。
  单测因此改为「先 `gc_force_major()` 取静止窗口（STW 让所有 mutator flush，空闲 worker 不会自行再分配），
  再验证判据转移」——与生产路径的调用次序一致。
- 并发读说明（已写进源码）：`tlabList_` 的增删都在 `tlabList_m_` 内，指针不会悬垂；
  `curPage/localYoung` 由拥有者线程无锁修改，本函数只做「是否为空」的**比较、不解引用**，
  最坏是漏报/误报、不会 UB；刻意不抢 `allocM_`（避免与 `flushTlab` 的锁序纠缠）。

### 8.3 承重测试 `test/rt/test_gc_bug111.cpp`（2 用例，已登记 `test/CMakeLists.txt`）

| 用例 | 验证 |
| :--- | :--- |
| `DirtyTlabDetectedThenClearedByFlush` | 静止窗口断言 `false` → `registerThread` + 8×`GcString::make`（不 flush）⇒ **`true`** → `flushTlab()` ⇒ **`false`** → `compact(All)` **正常返回** → 下一轮 `gc_force_major()` 不崩 + 堆可用 → 注销后现场干净 |
| `DirtyTlabOfAnotherThreadIsDetected` | 主线程自身无 TLAB，另一线程注册并分配后保持脏 ⇒ `hasDirtyTlabs() == true`（证明遍历的是 `tlabList_`，不是本线程 `tlab_`）；worker flush + 注销后 ⇒ `false` |

> 用例内**不**真触发 abort（会杀掉整套测试）；abort 这一半行为由独立探针
> `scripts/fix110111_compact_probe.cpp`（mode 0/1）验证，见 §8.4。

### 8.4 三步对照（真跑，rc 与帧）

| 步 | 操作 | 结果 |
| :-: | :--- | :--- |
| B-绿（断言在位） | `scripts/fix110111_probe.exe 0`（400 对象不 flush → 裸调 `compact(All)`） | **rc=127**（= `abort()` 的 MSYS 映射）；stderr 打出 `*** COMPACT CONTRACT VIOLATION ***: compact requires all TLABs flushed (call flushTlab() before compact).` |
| B-绿（不误报） | `... 1`（compact 前 `flushTlab()`） | **rc=0**；`hasDirtyTlabs()` 1→0、`compact` 返回、下一轮 GC 正常、`after=PROBE111_AFTER (len=14)` |
| B-红（去掉自检） | 删掉 `compact()` 入口那段 `if (hasDirtyTlabs()) { ... abort(); }`，重建 | mode 0：`compact(All) RETURNED`（本轮绿）→ **下一轮 `gc_force_major()` ⇒ rc=139**；帧 `GcObject::marked (this=0x180000)` ← `sweepPhaseAll()` lambda @ `mark_sweep.cpp:450`（悬垂对象在 `youngObjects_` 死亡清扫里先被踩到；与调研批 `:131`/`:486` 属同族、帧随堆布局漂移） |
| B-恢复 | 从备份还原 | md5 回 `9efff1a4…`；mode 0 又回到 rc=127（自检命中）、mode 1 rc=0 |

### 8.5 回归

- 双目标重建：`cmake --build runtime/build` + `cmake --build test/build` 均 0 error；
- 全量 **1591/1591 passed, 0 failed**（连跑 2 次，rc=0；基线 1585 + 本批 6 个新用例）；
- `--filter=Examples` **17/17**，rc=0；`example/test.aura` md5 `5f1760a5a360f4139d176434775abddd`（未变）；
- 未改 `src/`、`example/`、`change.md`、`READMEs/`、`plan/`；无 git 写操作。

### 8.6 遗留（仅登记，未实施）

- **单线程 fast path 的调用方**（`threadCount <= 1`）只 flush 自己；若进程里存在**未注册**的
  native 线程持有 TLAB，本判据看不见（那种线程也不会出现在 `tlabList_`）。属已知边界。
- §4 的验证建议里「信息含 *compact requires preceding mark/sweep*」已按归因订正改为
  「compact requires all TLABs flushed」——**旧文案是错误归因的残留，以本节为准**。
- 若要对外部直接调用者更友好，可另立「compact 前置 helper（STW 化）」的 feature 提案，
  本批按 brief 不实现。
