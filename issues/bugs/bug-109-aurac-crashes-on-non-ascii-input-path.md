---
type: bug_report
module: Parser
sub_module: 编译器入口 / std::filesystem 路径转换（非 ASCII 输入路径）
status:
  - fixed
severity:
  - high
discover_date: 2026-10-15
related_issues: []
tags:
  - crash
  - windows
  - unicode-path
  - filesystem
---

# 【编译器 / 路径】[x] **主标题：`aurac` 读取含非 ASCII 字符的**输入**路径时抛未捕获 `std::filesystem::filesystem_error` 直接终止（`Illegal byte sequence`）**

> **一句话摘要**：把 `.aura` 源文件放在含非 ASCII 字符（如中文）的目录下并作为**输入**传给 `aurac` 时，编译器抛 `std::filesystem::filesystem_error: Cannot convert character sequence: Illegal byte sequence` 并 `terminate`（`aurac_rc=127`）；**输出**路径（`--cpp`）含非 ASCII 则**正常**（主 Agent 对照实测）⇒ 缺口在「**输入路径 → `std::filesystem::path`**」的窄字符转换，且异常未被捕获。

## 1. 调研背景与发现
- **发现时间**：`2026-10-15`（enum 实施**批 6** 遗留⑤ 提出；**主 Agent 独立复现并做输出路径对照后登记**）。
- **触发场景**：Windows 上把源码放在中文/日文等非 ASCII 目录（对中文用户是常见情形），例如 `.../中文目录测试/输入.aura`。
- **影响**：进程级终止（未捕获异常），无诊断、无友好提示；用户的工程只要路径含非 ASCII 即**完全无法编译**。

## 2. 根因分析（Root Cause Analysis）
### 2.1 复现原文（主 Agent 亲跑，两条对照）
```
# A. 输入路径含中文 ⇒ 崩溃
$ cp scripts/b3_cases/c1_enum_method.aura "$T/中文目录测试/输入.aura"
$ ./build/aurac.exe "$T/中文目录测试/输入.aura" --cpp "$T/中文目录测试/out2.cpp" -S
aurac_rc=127
terminate called after throwing an instance of 'std::filesystem::__cxx11::filesystem_error'
  what():  filesystem error: Cannot convert character sequence: Illegal byte sequence

# B. 输出路径含中文（输入 ASCII）⇒ 正常
$ ./build/aurac.exe scripts/b3_cases/c1_enum_method.aura --cpp "$T/中文目录测试/x.cpp" -S
aurac_rc=0        # 且 x.cpp 正常生成
```
### 2.2 关键逻辑细节
- **A 崩 / B 正常** ⇒ 问题不在「写输出」，而在**读输入时**把窄字符串（本地编码字节）交给 `std::filesystem::path` 的转换路径（libstdc++ 在 Windows 上按当前 locale 解释窄字符串 ⇒ `Illegal byte sequence`），且该异常**没有 try/catch**，直接 `terminate`。
- **待定位（下一轮）**：`src/main.cpp` 的入口参数处理（argv → path）、`ModuleManager` 的加载路径拼接（`ModuleManager.cpp` 多处 `std::filesystem` 调用）、以及 `#include`/`--cpp` 输出目录的 path 构造点；须**逐处实读**后给行号（本批只确认现象与对照，未获授权定位）。
- **规避（当前）**：把源码与输出放在纯 ASCII 路径下。

## 3. 影响范围（Scope）
- **结论**：任何「**输入路径含非 ASCII**」的调用（含被其它工具/脚本以中文路径调用）⇒ 编译器崩溃。Windows 中文用户的常见目录（`C:\Users\xxx\桌面\...`、中文工程名）全部命中。
- **不受影响**：输入 ASCII + 输出非 ASCII（实测正常）；文件**内容**含中文（源码内容编码与路径无关）。
- **严重性判断**：`high` —— 进程级崩溃 + 零诊断（非"报错"而是"终止"），且对中文用户是**首次使用即撞**的门槛。

## 4. 实测复现矩阵（Validation Matrix）
| 探针 / 命令 | 场景 | 预期（修复后） | 当前实际 | 状态 |
| :--- | :--- | :--- | :--- | :--- |
| `aurac "…/中文目录测试/输入.aura" --cpp …` | 输入路径非 ASCII | 正常编译或**干净诊断** | ❌ `rc=127` + `filesystem_error: Illegal byte sequence`（terminate） | 根因直击 |
| `aurac scripts/b3_cases/c1_enum_method.aura --cpp "…/中文目录测试/x.cpp"` | **仅输出**非 ASCII | 正常 | ✅ `rc=0` + 文件生成 | 对照（定位到"读入"侧） |
| `aurac <ASCII 路径>` | 全 ASCII | 正常 | ✅ `rc=0` | 不误伤 |

## 5. 修复方案（Fix Plan）
- **方向（择一，按仓内既有约定）**：
  1. **入口统一走宽字符/UTF-8 路径**：Windows 用 `wmain` + `std::filesystem::path(std::wstring)`（或 `std::filesystem::u8path`）构造路径，避免窄字符串 → path 的隐式转换；
  2. 至少对所有 `std::filesystem` 调用做 **try/catch**，把 `filesystem_error` 转成**干净的编译诊断**（含原始路径的十六进制/转义表示），杜绝 terminate。
- **落点侦查（下一轮首批）**：`src/main.cpp`（argv → path）、`src/Module/ModuleManager.cpp`（`std::filesystem` 调用点）、`--cpp` 输出目录构造处、`import` 相对路径解析处。
- **测试**：输入/输出路径非 ASCII 各一例（含 `import` 相对路径含非 ASCII 的模块）；断言不崩（`rc∈{0}` 且无 `terminate`）。

## 6. 回归验证清单（Regression Checklist）
- [x] 输入路径含中文 ⇒ `aurac_rc=0` + `out2.cpp` 生成（**真修复，不只是干净诊断**）
- [x] 输出路径含中文、输入输出**都**含中文 ⇒ `aurac_rc=0` + 产物生成（②③）
- [x] 非 ASCII 目录下的 `import "子模块.aura"` 相对导入 ⇒ `aurac_rc=0` + 四个模块产物 + `aura.meta.*`（④）
- [x] 纯 ASCII 路径行为与修复前一致（`b3_cases c1..c9` + `_v1_probe/p09` 两步铁律全 `0/0`）
- [x] 全量单测保持基线：**1583 / 1583**（`--filter=Examples` 17/17；`example/test.aura` md5 未变；`runtime/` 一行未动）
- [ ] `aurac` 的 CLI 用法文档（若有）补一句路径编码说明 —— **未做**（`READMEs/` 为红线禁改；待后续批次处理）

## 7. 附加资源与产物
- **复现命令**：见 §2.1；探针源 `scripts/b3_cases/c1_enum_method.aura`（内容无关，仅需路径含非 ASCII）
- **产物/日志**：`%LOCALAPPDATA%\Temp\b6verify\nonascii2.log`（A 的 stderr 原文）、`nonascii.log`（B，正常）
- **相关位置（待实读定位）**：`src/main.cpp`、`src/Module/ModuleManager.cpp`、`src/CodeGen/MetaCollect.cpp`（thunk/文件写出路径拼接）
- **来源批次**：`scripts/enum_impl_b6_report.md` §报红/遗留 ⑤

## 8. 修复记录（2026-10-16）

### 8.1 侦查：根因链（逐处实读，行号为修复前）
- **入口**：`src/main.cpp:637` `int main(int argc, char* argv[])` —— **无 `wmain`**；`argv` 由 CRT 从 Windows UTF-16 命令行按**进程 ANSI 代码页**（本机 = CP936/GBK）转成窄字节。
- **读入侧第一崩点**：`src/main.cpp:138` `Aura::stemOf(opts.inputPath)` → `src/Module/ModuleManager.cpp:46-49` `fs::path(path).stem().string()`。
- **全部窄→`std::filesystem::path` 转换点**（均按 C locale codecvt ⇒ 非 ASCII 抛异常）：
  - `src/main.cpp`：`:211`（`path(inputPath).parent_path()`）、`:235`（`path(cppPath).replace_extension`）、`:401`（`absolute(inputPath)`）、`:404`（`create_directories`）、`:251/:617/:622/:627`（`remove`）。
  - `src/Module/ModuleManager.cpp`：`:46-49`（`stemOf`）、`:92`（`pathToNs`）、`:107-110`（`identityKeyOf` 的 `absolute(...,ec)` / `path(...)`）、`:124`（`computeModuleKeys` 的 baseDir）、`:176`（`parseModule` 的 sourcePath）、`:237`（importerDir）、`:364`（`scanDeclarations` 的 sourcePath）、`:417`（importerDir）、`:505/:723`（`absolute(entryPath)`）、`:667-678`（`resolveImportPath` 四处）、`:690-692`（`loadAuraiFile` 的 `current_path()/builtins` + `.string()`）。
  - `src/CodeGen/CodeGen.cpp:200`（`path(imp.path).stem().string()`）—— **红线禁改区**，见 §8.5。
- **catch**：全仓 `catch (const std::filesystem::filesystem_error&)` = **0 处** ⇒ 异常直接 `terminate`（`rc=127`）。
- **A 崩 / B 不崩 的差异行**：`src/main.cpp:232` `if (opts.stopAfterCpp) return 0;`。case B（输入 ASCII + 输出中文）在**触碰 `main.cpp:235` 的 `fs::path(cppPath)` 之前就 return**（输出走 `writeFile`→`ofstream`，而 `ofstream` 与 argv 同按 CP_ACP ⇒ 正常）；case A 的**输入路径**却在 `main.cpp:138` 一进门就进了 `fs::path` ⇒ 抛。**差异不在「读文件 vs 写文件」，而在「是否走了 `std::filesystem::path` 的窄字符串构造」。**

### 8.2 探针实测（本机，`%LOCALAPPDATA%\Temp\b9probe\probe.cpp` / `probe2.cpp`）
对同一 `argv[1]`（含 `中文目录测试/输入.aura`）实测：
```
argv[1] bytes : ... 2F D6 D0 CE C4 C4 BF C2 BC B2 E2 CA D4 2F CA E4 C8 EB 2E 61 75 72 61   <- GBK(CP936)
ifstream.open : OK                       <- 窄文件名按 CP_ACP 解释，能打开
fs::path.stem : THROW filesystem error: Cannot convert character sequence: Illegal byte sequence
fs::absolute  : THROW （同上）
fs::exists    : THROW （同上）
MB2WC CP_UTF8 : INVALID (0)              <- argv 不是 UTF-8
MB2WC CP_ACP  : 57 wchars -> ... U+4E2D U+6587 U+76EE U+5F55 U+6D4B U+8BD5 ...  <- 正是「中文目录测试」
wide path.stem: OK
std::locale::global(std::locale("")) : THROW (locale name not valid)；setlocale(LC_ALL,"") 亦无效
```
**结论**：`argv`/`ifstream`/`ofstream`/`std::system` 统一在 **CP_ACP** 口径，只有 `std::filesystem::path` 的窄字符串构造（与 `.string()`）用 C locale codecvt ⇒ **口径不一致**才是根因（本机 `GetACP()=936`）。另实测 libstdc++ 在 Windows 的 `path::string()` 产出 **UTF-8**（`E4 B8 AD …`），与 `argv` 的 CP_ACP **不同口径** —— 这也解释了为何不能简单用 `.string()` 往返。

### 8.3 修法（两条都做）
**① 入口侧「宽字符路径」思路（未改 `main` 形态 / CMake）**：在 `src/Module/ModuleManager.{h,cpp}` 新增三个桥函数，把所有「窄字符串 ↔ path」边界显式钉死在正确代码页，**每个路径都先物化成 `std::wstring` 再进 `std::filesystem::path`**：
- `std::filesystem::path Aura::fsPath(const std::string&)` —— 窄字节按 **CP_ACP** → 宽（与 argv/ifstream/ofstream 同口径；非 Windows 直通）；
- `std::filesystem::path Aura::fsPathUtf8(const std::string&)` —— 窄字节按 **UTF-8** → 宽（供**源码内容里的文件名**，见 case ④；非法 UTF-8 返回空）；
- `std::string Aura::pathString(const std::filesystem::path&)` —— 宽 → **CP_ACP** 窄字节（**不用** libstdc++ 的 `.string()`，它给 UTF-8）。
并把 §8.1 列出的**全部** `path(...)` / `absolute(...)` / `.string()` / `remove` / `create_directories` 调用点改用该桥；`resolveImportPath` 对同一 import 名**先按 ACP、再按 UTF-8**各试一轮（ASCII 下两者恒等 ⇒ 零回归），使 UTF-8 源码里的中文文件名可被解析（case ④）。
  另修一处同源隐蔽口径错配：`loadAuraiFile` 原用 `auraiPath.string()`（UTF-8）再喂 `readFile`（ifstream 按 ACP）⇒ 非 ASCII cwd 下会「路径拼对了却打不开」，改为 `pathString(auraiPath)`。
**② 兜底 catch**：`src/main.cpp` 原 `main` 改为 `static int runMain(...)`，新的 `main` 只做 `try { return runMain(...); }` + `catch (const std::filesystem::filesystem_error&)` / `catch (const std::exception&)` ⇒ 打印 `Error: filesystem operation failed: …` 并 `return 1`，**杜绝 terminate**（全仓该 catch 由 0 处 → 1 处）。
**选此法（而非 `wmain`/`-municode`）的理由**：argv 实测即 CP_ACP（§8.2），故在 ACP 口径上落地是**精确解**，无需改入口形态与 CMake（避免触发红线），改动面小、ASCII 下逐字节等价；`wmain`+全程宽字符（含 `ifstream`/`std::system` 全面宽化）是更大改造，本次不做（见 §8.5 残余）。

### 8.4 用例原文（`scripts/_b9_cases.sh`，`T=%LOCALAPPDATA%\Temp\b9verify`）
| # | 场景 | 命令（要点） | 结果 |
| :- | :--- | :--- | :--- |
| ① | 输入路径含中文 | `aurac "$T/中文目录测试/输入.aura" --cpp "$T/中文目录测试/out2.cpp" -S` | **`aurac_rc=0`**，`out2.cpp` 2878B 生成（修复前：`rc=127` + `terminate … Illegal byte sequence`） |
| ② | 仅输出路径含中文 | `aurac scripts/b3_cases/c1_enum_method.aura --cpp "$T/中文目录测试/x.cpp" -S` | **`aurac_rc=0`**，`x.cpp` 2772B 生成（不回归） |
| ③ | 输入输出都含中文 | `aurac "$T/中文目录测试/输入.aura" --cpp "$T/中文目录测试/输出.gen.cpp" -S` | **`aurac_rc=0`**，`输出.gen.cpp` 2878B 生成 |
| ④ | 非 ASCII 目录下 `import "子模块.aura"` | `aurac "$T/中文目录测试/主模块.aura" --cpp "$T/中文目录测试/out_dir" -S` | **`aurac_rc=0`**；`主模块.aura.{h,cpp}` + `子模块.aura.{h,cpp}` + `aura.meta.{h,cpp}` 全部生成 |
| ⑤ | 非 ASCII **cwd** 下运行 | `cd "$T/中文目录测试" && aurac c1.aura --cpp ./c1_rel.cpp -S` | **`aurac_rc=0`**，`c1_rel.cpp` 2644B 生成 |
| ⑥ | 纯 ASCII 回归 | `bash scripts/b3_cases/run_two_step.sh`（c1..c9） | 9 例全 **`aurac_rc=0  g++_rc=0`**，`two-step iron law overall: PASS` |
| ⑥b | 纯 ASCII 回归 | `scripts/_v1_probe/p09_same_name_variant.aura` 两步铁律 | **`aurac_rc=0  g++_rc=0`** —— `PASS` |

> ①③ 判为**真修复**（`rc=0` 且产物生成），非「干净诊断」；因此 §6 的首条按「正常编译」勾选。

### 8.5 统计 / 红线 / 残余
- **全量单测**：基线 **1583 / 1583** → 修复后 **1583 / 1583**（`0 failed`）；**新增单测 0 条**（理由见下）。
- `--filter=Examples`：**17 passed, 0 failed**；`example/test.aura` md5 = `5f1760a5a360f4139d176434775abddd`（未变）；`git status --short -- runtime/` **空**（`runtime/` 一行未动）。
- **只改了**：`src/main.cpp`、`src/Module/ModuleManager.h`、`src/Module/ModuleManager.cpp`（均在本批允许清单内）；未触碰 `example/`、`runtime/`、`READMEs/`、`change.md`、`src/CodeGen/**`、`src/Sema/**`、其它缺陷笔记；未做任何 git 写操作。**未动入口/CMake**（无需报红：ACP 口径已精确）。
- **新增单测 0 条的理由**：本缺陷只在**真实进程 `argv` → CRT 代码页转换**这一层可复现（进程内单测框架无法制造非 ASCII `argv`），且 `test/` 不在本批允许改动清单内；回归护栏改由 `scripts/_b9_cases.sh`（上述 6 组 CLI 用例）承载。
- **同源残留（未修，登记于此）**：
  1. `src/CodeGen/CodeGen.cpp:200` 的 `fs::path(imp.path).stem().string()` 是同类窄→path 调用，位于**红线禁改区**（`src/CodeGen/**`）。当前**不可达**：多文件路径下 `imp.artifactKey` 恒非空（`main.cpp:441` 经 `moduleKeyOfPath`/`stemOf`），故该三元 else 分支不执行；仅当模块未在 `mgr.modules()` 中命中（异常形态）才回落触发。**留待后续批次**（需主人授权改 CodeGen 或改为由 main 侧预置 artifactKey）。
  2. **诊断文本编码**：`stderr` 输出的路径是 CP_ACP 字节，在 UTF-8 终端（git-bash）下显示为乱码（cmd.exe CP936 正常）；不影响产物与 `rc`。
  3. **入口未宽化**：不可表示于 ACP 的字符（如部分 emoji）在 argv 阶段已由 CRT 替换为 `?`，信息不可恢复；彻底解决需 `wmain`/`GetCommandLineW` + 全程宽字符（含 `ifstream`/`std::system`），属更大改造，未做。
  4. §6 末条 CLI 文档说明未做（`READMEs/` 禁改）。
  5. 兜底 catch 在本机正常 `argv` 下**不可达**（CRT 已将 argv 规整为合法 ACP）；作为 defense-in-depth 保留，已验证编译通过且不改变既有行为。

---

**当前状态**：`2026-10-16` **修复完成**（`status: fixed`）—— 入口/兜底两条修法落地，6 组用例全过，全量 **1583 / 1583**，`--filter=Examples` 17/17，`example/test.aura` md5 未变，`runtime/` 未动；上报残余 5 项（含 1 处 `src/CodeGen/**` 禁改区同源潜在点）。

---

## 9. 主人裁定（2026-10-16）：三项残余 **暂不处理**

> 主 Agent 于 `2026-10-16` 把 §8.5 残余中的三项上报主人裁定；主人裁定：**直接记录于此，暂不处理**（本日不排期）。

| # | 事项 | 裁定 | 留存说明（未来若要处理，从这里接） |
| :-: | :--- | :--- | :--- |
| **D1** | **新增单测 0 条**（本缺陷只在真实进程 `argv` → CRT 代码页转换层可复现，进程内单测框架造不出非 ASCII `argv`；且 `test/` 系**主 Agent 简报**未列入允许清单 —— 白名单限制责任在主 Agent 简报） | **暂不处理** | 后续可补 2–3 条 **in-process** 单测钉住转换函数本身：`fsPath`/`pathString` 往返一致、`fsPath("中文目录测试")` **不抛**、`fsPathUtf8` 非法输入返回空 |
| **D2** | **`src/CodeGen/CodeGen.cpp:200`** 的 `fs::path(imp.path).stem().string()` 为同类窄→path 调用（位于**红线禁改区** `src/CodeGen/**`；当前**不可达**，仅模块未命中 `mgr.modules()` 的异常形态才回落触发） | **暂不处理**（不授权改 `src/CodeGen/**`） | 未来二选一：① 放宽红线改该行；② 由 `main` 侧预置 `artifactKey` 消掉该回落分支（后者不动 CodeGen） |
| **D3** | **入口未宽化**：不可表示于 ACP 的字符（如部分 emoji）在 `argv` 阶段已被 CRT 替换为 `?`，信息不可恢复；彻底解决需 `wmain`/`GetCommandLineW` + 全程宽字符（含 `ifstream`/`std::system`）。另：`stderr` 的路径为 CP_ACP 字节，在 UTF-8 终端（git-bash）下显示乱码（不影响产物与 `rc`） | **暂不处理**（属较大改造） | 与 §8.5 残余 2/3 同源；若未来做「全程宽字符」批次可一并解决 |

**状态**：本条缺陷**修复已关闭**（`status: fixed`）；上表三项为**已登记的已知残余**，`2026-10-16` 起**不排期**。
