# 🌸 屎山代码分析报告 🌸

## 📑 目录

- [糟糕指数](#overall-score)
- [评分指标详情](#metrics-details)
- [最屎代码排行榜](#problem-files)
- [诊断结论](#conclusion)

![Score](https://img.shields.io/badge/Score-79%25-green)

## 糟糕指数 {#overall-score}

| 指标摘要 | 评分 |
|------|-------|
| **糟糕指数** | **78.76/100** |
| 屎山等级 | 😐 微臭青年 |

> 略带清香，偶尔飘过一丝酸爽

### 📊 统计信息

| 指标 | 数值 |
|--------|-------|
| 总文件数 | 119 |
| 已跳过 | 392 |
| 耗时 | 869ms |

### 📋 项目概览

| 指标 | 数值 |
|--------|-------|
| 总代码行数 | 22962 |
| 总注释行数 | 6471 |
| 整体注释比例 | 28.2% |
| 平均文件大小 | 276 行 |
| 最大文件 | `src\CodeGen\StmtGen.cpp` (2002) |

#### 语言分布

| 语言 | 文件数 |
|:-----|------:|
| C++ | 70 |
| C | 49 |

## 评分指标详情 {#metrics-details}

| 指标摘要 | 评分 | Min | Max | Median | 状态 |
|:-----|------:|------:|------:|------:|:------:|
| 循环复杂度 | 11.41% | 0.0% | 79.2% | 0.0% | ✓✓ |
| 认知复杂度 | 10.76% | 0.0% | 74.1% | 0.0% | ✓✓ |
| 嵌套深度 | 11.77% | 0.0% | 99.7% | 0.0% | ✓✓ |
| 函数长度 | 5.23% | 0.0% | 48.4% | 0.0% | ✓✓ |
| 文件长度 | 3.62% | 0.0% | 82.6% | 0.0% | ✓✓ |
| 参数数量 | 1.53% | 0.0% | 61.7% | 0.0% | ✓✓ |
| 代码重复 | 5.03% | 0.0% | 85.5% | 0.0% | ✓✓ |
| 结构分析 | 7.01% | 0.0% | 75.0% | 0.0% | ✓✓ |
| 错误处理 | 19.75% | 0.0% | 98.8% | 0.0% | ✓✓ |
| 注释比例 | 32.69% | 0.0% | 100.0% | 15.9% | ✓ |
| 命名规范 | 17.67% | 0.0% | 100.0% | 0.0% | ✓✓ |

## 最屎代码排行榜 {#problem-files}

### 1. src\CodeGen\StmtGen.cpp

**糟糕指数: 54.82**

> 行数: 2002 总计, 1466 代码, 404 注释 | 函数: 31 | 类: 2

**问题**: 🔄 复杂度问题: 31, ⚠️ 其他问题: 7, 🏗️ 结构问题: 14, ❌ 错误处理问题: 20, 📝 注释问题: 1

#### 函数详情

| 函数 | 行范围 | 行数 | 复杂度 | 嵌套 | 参数 | 注释 |
|:-----|------:|------:|------:|------:|------:|:------:|
| `genLetStmt` | L220-564 | 345 | 80 | 8 | 2 | ✗ |
| `genMatchStmt` | L1727-1989 | 263 | 65 | 8 | 3 | ✗ |
| `genSyncForStmt` | L1202-1374 | 173 | 36 | 5 | 3 | ✗ |
| `genForStmt` | L773-926 | 154 | 24 | 5 | 3 | ✗ |
| `genSpawnStmt` | L1376-1448 | 73 | 23 | 3 | 3 | ✗ |
| `genStmt` | L23-61 | 39 | 21 | 3 | 3 | ✗ |
| `genUnionBoxingImpl` | L128-218 | 91 | 21 | 4 | 3 | ✗ |
| `genLockStmt` | L1539-1659 | 121 | 20 | 3 | 3 | ✓ |
| `genConstStmt` | L566-618 | 53 | 15 | 4 | 2 | ✗ |
| `genReturnStmt` | L624-692 | 69 | 14 | 4 | 3 | ✗ |
| `genTryCatchStmt` | L948-1055 | 108 | 14 | 3 | 3 | ✗ |
| `genSpawnCallAsThread` | L1487-1527 | 41 | 9 | 2 | 2 | ✓ |
| `genSpawnAsThread` | L1673-1721 | 49 | 9 | 2 | 2 | ✓ |
| `genThrowStmt` | L694-721 | 28 | 8 | 4 | 2 | ✗ |
| `genSpawnCallAsCoro` | L1453-1483 | 31 | 6 | 2 | 2 | ✓ |
| `genUnionBoxing` | L88-100 | 13 | 5 | 2 | 3 | ✓ |
| `genIfStmt` | L723-739 | 17 | 5 | 2 | 3 | ✗ |
| `genTryCatchNoSetupIIFE` | L1061-1106 | 46 | 5 | 3 | 3 | ✓ |
| `genSyncStmt` | L1135-1164 | 30 | 5 | 2 | 3 | ✗ |
| `genBlock` | L12-17 | 6 | 3 | 2 | 3 | ✗ |
| `isNoneCallExpr` | L69-75 | 7 | 3 | 2 | 1 | ✓ |
| `~IterVarGuard` | L767-770 | 4 | 3 | 1 | 0 | ✗ |
| `genTryCatchRaw` | L1108-1127 | 20 | 3 | 1 | 3 | ✗ |
| `genSyncThreadStmt` | L1177-1200 | 24 | 3 | 1 | 2 | ✓ |
| `genRecordToViewIIFE` | L110-126 | 17 | 2 | 1 | 3 | ✓ |
| `genWhileStmt` | L741-747 | 7 | 2 | 1 | 3 | ✗ |
| `IterVarGuard` | L759-766 | 8 | 2 | 1 | 3 | ✗ |
| `genLoopStmt` | L928-934 | 7 | 2 | 1 | 3 | ✗ |
| `genExprStmt` | L1995-1999 | 5 | 2 | 1 | 3 | ✗ |
| `genBreakStmt` | L936-938 | 3 | 1 | 0 | 1 | ✗ |
| `genContinueStmt` | L940-942 | 3 | 1 | 0 | 1 | ✗ |

**全部问题 (71)**

- 🔄 `genStmt()` L23: 复杂度: 21
- 🔄 `genUnionBoxingImpl()` L128: 复杂度: 21
- 🔄 `genLetStmt()` L220: 复杂度: 80
- 🔄 `genConstStmt()` L566: 复杂度: 15
- 🔄 `genReturnStmt()` L624: 复杂度: 14
- 🔄 `genForStmt()` L773: 复杂度: 24
- 🔄 `genTryCatchStmt()` L948: 复杂度: 14
- 🔄 `genSyncForStmt()` L1202: 复杂度: 36
- 🔄 `genSpawnStmt()` L1376: 复杂度: 23
- 🔄 `genLockStmt()` L1539: 复杂度: 20
- 🔄 `genMatchStmt()` L1727: 复杂度: 65
- 🔄 `genStmt()` L23: 认知复杂度: 27
- 🔄 `genUnionBoxingImpl()` L128: 认知复杂度: 29
- 🔄 `genLetStmt()` L220: 认知复杂度: 96
- 🔄 `genConstStmt()` L566: 认知复杂度: 23
- 🔄 `genReturnStmt()` L624: 认知复杂度: 22
- 🔄 `genThrowStmt()` L694: 认知复杂度: 16
- 🔄 `genForStmt()` L773: 认知复杂度: 34
- 🔄 `genTryCatchStmt()` L948: 认知复杂度: 20
- 🔄 `genSyncForStmt()` L1202: 认知复杂度: 46
- 🔄 `genSpawnStmt()` L1376: 认知复杂度: 29
- 🔄 `genLockStmt()` L1539: 认知复杂度: 26
- 🔄 `genMatchStmt()` L1727: 认知复杂度: 81
- 🔄 `genUnionBoxingImpl()` L128: 嵌套深度: 4
- 🔄 `genLetStmt()` L220: 嵌套深度: 8
- 🔄 `genConstStmt()` L566: 嵌套深度: 4
- 🔄 `genReturnStmt()` L624: 嵌套深度: 4
- 🔄 `genThrowStmt()` L694: 嵌套深度: 4
- 🔄 `genForStmt()` L773: 嵌套深度: 5
- 🔄 `genSyncForStmt()` L1202: 嵌套深度: 5
- 🔄 `genMatchStmt()` L1727: 嵌套深度: 8
- 📏 `genLetStmt()` L220: 345 代码量
- 📏 `genForStmt()` L773: 154 代码量
- 📏 `genTryCatchStmt()` L948: 108 代码量
- 📏 `genSyncForStmt()` L1202: 173 代码量
- 📏 `genLockStmt()` L1539: 121 代码量
- 📏 `genMatchStmt()` L1727: 263 代码量
- 🏗️ `genStmt()` L23: 中等嵌套: 3
- 🏗️ `genUnionBoxingImpl()` L128: 中等嵌套: 4
- 🏗️ `genLetStmt()` L220: 嵌套过深: 8
- 🏗️ `genConstStmt()` L566: 中等嵌套: 4
- 🏗️ `genReturnStmt()` L624: 中等嵌套: 4
- 🏗️ `genThrowStmt()` L694: 中等嵌套: 4
- 🏗️ `genForStmt()` L773: 嵌套过深: 5
- 🏗️ `genTryCatchStmt()` L948: 中等嵌套: 3
- 🏗️ `genTryCatchNoSetupIIFE()` L1061: 中等嵌套: 3
- 🏗️ `genSyncForStmt()` L1202: 嵌套过深: 5
- 🏗️ `genSpawnStmt()` L1376: 中等嵌套: 3
- 🏗️ `genLockStmt()` L1539: 中等嵌套: 3
- 🏗️ `genMatchStmt()` L1727: 嵌套过深: 8
- 🏗️ L1: 文件过大: 2002 行
- ❌ L123: 未处理的易出错调用
- ❌ L193: 未处理的易出错调用
- ❌ L206: 未处理的易出错调用
- ❌ L352: 未处理的易出错调用
- ❌ L427: 未处理的易出错调用
- ❌ L492: 未处理的易出错调用
- ❌ L677: 未处理的易出错调用
- ❌ L683: 未处理的易出错调用
- ❌ L751: 未处理的易出错调用
- ❌ L840: 未处理的易出错调用
- ❌ L849: 未处理的易出错调用
- ❌ L871: 未处理的易出错调用
- ❌ L1416: 未处理的易出错调用
- ❌ L1439: 未处理的易出错调用
- ❌ L1442: 未处理的易出错调用
- ❌ L1866: 未处理的易出错调用
- ❌ L1871: 未处理的易出错调用
- ❌ L1881: 未处理的易出错调用
- ❌ L1940: 未处理的易出错调用
- ❌ L1944: 未处理的易出错调用

**详情**:
- 循环复杂度: 平均: 13.3, 最大: 80
- 认知复杂度: 平均: 18.5, 最大: 96
- 嵌套深度: 平均: 2.6, 最大: 8
- 函数长度: 平均: 59.8 行, 最大: 345 行
- 文件长度: 1466 代码量 (2002 总计)
- 参数数量: 平均: 2.5, 最大: 3
- 代码重复: 0.0% 重复 (0/31)
- 结构分析: 14 个结构问题
- 错误处理: 20/65 个错误被忽略 (30.8%)
- 注释比例: 27.6% (404/1466)
- 命名规范: 发现 1 个违规

### 2. src\CodeGen\ExprGen.cpp

**糟糕指数: 53.65**

> 行数: 1829 总计, 1377 代码, 320 注释 | 函数: 31 | 类: 0

**问题**: 🔄 复杂度问题: 30, ⚠️ 其他问题: 8, 🏗️ 结构问题: 12, ❌ 错误处理问题: 23

#### 函数详情

| 函数 | 行范围 | 行数 | 复杂度 | 嵌套 | 参数 | 注释 |
|:-----|------:|------:|------:|------:|------:|:------:|
| `genMethodCall` | L888-1186 | 299 | 86 | 6 | 2 | ✗ |
| `genFunExpr` | L1524-1826 | 303 | 78 | 5 | 2 | ✗ |
| `genCallExpr` | L658-886 | 229 | 62 | 6 | 2 | ✗ |
| `genListExpr` | L290-400 | 111 | 31 | 7 | 2 | ✗ |
| `genUnionDispatch` | L1206-1298 | 93 | 29 | 4 | 3 | ✓ |
| `genBinaryExpr` | L504-646 | 143 | 25 | 5 | 2 | ✗ |
| `genAssignExpr` | L1364-1471 | 108 | 24 | 5 | 2 | ✗ |
| `genExpr` | L199-239 | 41 | 20 | 1 | 2 | ✗ |
| `genGcRootedArgs` | L83-193 | 111 | 18 | 4 | 3 | ✓ |
| `isHeapSemType` | L32-56 | 25 | 12 | 3 | 1 | ✓ |
| `genRecordExpr` | L402-451 | 50 | 12 | 3 | 2 | ✗ |
| `escapeStringLiteral` | L13-27 | 15 | 8 | 2 | 1 | ✓ |
| `genUnionIndexDispatch` | L1304-1347 | 44 | 7 | 2 | 3 | ✓ |
| `isIfaceView` | L63-70 | 8 | 4 | 1 | 1 | ✓ |
| `genIdentifier` | L265-284 | 20 | 4 | 1 | 1 | ✗ |
| `collectStringChain` | L457-479 | 23 | 4 | 3 | 2 | ✗ |
| `genMemberAccess` | L1188-1198 | 11 | 4 | 2 | 1 | ✗ |
| `isStringExprInChain` | L481-498 | 18 | 3 | 2 | 1 | ✗ |
| `genIndexExpr` | L1349-1358 | 10 | 3 | 2 | 2 | ✗ |
| `genBoolLiteral` | L257-259 | 3 | 2 | 0 | 1 | ✗ |
| `genUnaryExpr` | L648-652 | 5 | 2 | 1 | 2 | ✗ |
| `decomposeFieldAccess` | L1483-1492 | 10 | 2 | 1 | 1 | ✗ |
| `isUnionHeapVariant` | L75-77 | 3 | 1 | 0 | 1 | ✓ |
| `genIntLiteral` | L245-247 | 3 | 1 | 0 | 1 | ✗ |
| `genFloatLiteral` | L249-251 | 3 | 1 | 0 | 1 | ✗ |
| `genStringLiteral` | L253-255 | 3 | 1 | 0 | 1 | ✗ |
| `genNoneLiteral` | L261-263 | 3 | 1 | 0 | 0 | ✗ |
| `isGcFieldAssignment` | L1477-1481 | 5 | 1 | 0 | 1 | ✗ |
| `genErrorPropagation` | L1498-1503 | 6 | 1 | 0 | 2 | ✗ |
| `genPipeExpr` | L1505-1510 | 6 | 1 | 0 | 2 | ✗ |
| `genConditionalExpr` | L1512-1518 | 7 | 1 | 0 | 2 | ✗ |

**全部问题 (72)**

- 🔄 `isHeapSemType()` L32: 复杂度: 12
- 🔄 `genGcRootedArgs()` L83: 复杂度: 18
- 🔄 `genExpr()` L199: 复杂度: 20
- 🔄 `genListExpr()` L290: 复杂度: 31
- 🔄 `genRecordExpr()` L402: 复杂度: 12
- 🔄 `genBinaryExpr()` L504: 复杂度: 25
- 🔄 `genCallExpr()` L658: 复杂度: 62
- 🔄 `genMethodCall()` L888: 复杂度: 86
- 🔄 `genUnionDispatch()` L1206: 复杂度: 29
- 🔄 `genAssignExpr()` L1364: 复杂度: 24
- 🔄 `genFunExpr()` L1524: 复杂度: 78
- 🔄 `isHeapSemType()` L32: 认知复杂度: 18
- 🔄 `genGcRootedArgs()` L83: 认知复杂度: 26
- 🔄 `genExpr()` L199: 认知复杂度: 22
- 🔄 `genListExpr()` L290: 认知复杂度: 45
- 🔄 `genRecordExpr()` L402: 认知复杂度: 18
- 🔄 `genBinaryExpr()` L504: 认知复杂度: 35
- 🔄 `genCallExpr()` L658: 认知复杂度: 74
- 🔄 `genMethodCall()` L888: 认知复杂度: 98
- 🔄 `genUnionDispatch()` L1206: 认知复杂度: 37
- 🔄 `genAssignExpr()` L1364: 认知复杂度: 34
- 🔄 `genFunExpr()` L1524: 认知复杂度: 88
- 🔄 `genGcRootedArgs()` L83: 嵌套深度: 4
- 🔄 `genListExpr()` L290: 嵌套深度: 7
- 🔄 `genBinaryExpr()` L504: 嵌套深度: 5
- 🔄 `genCallExpr()` L658: 嵌套深度: 6
- 🔄 `genMethodCall()` L888: 嵌套深度: 6
- 🔄 `genUnionDispatch()` L1206: 嵌套深度: 4
- 🔄 `genAssignExpr()` L1364: 嵌套深度: 5
- 🔄 `genFunExpr()` L1524: 嵌套深度: 5
- 📏 `genGcRootedArgs()` L83: 111 代码量
- 📏 `genListExpr()` L290: 111 代码量
- 📏 `genBinaryExpr()` L504: 143 代码量
- 📏 `genCallExpr()` L658: 229 代码量
- 📏 `genMethodCall()` L888: 299 代码量
- 📏 `genAssignExpr()` L1364: 108 代码量
- 📏 `genFunExpr()` L1524: 303 代码量
- 🏗️ `isHeapSemType()` L32: 中等嵌套: 3
- 🏗️ `genGcRootedArgs()` L83: 中等嵌套: 4
- 🏗️ `genListExpr()` L290: 嵌套过深: 7
- 🏗️ `genRecordExpr()` L402: 中等嵌套: 3
- 🏗️ `collectStringChain()` L457: 中等嵌套: 3
- 🏗️ `genBinaryExpr()` L504: 嵌套过深: 5
- 🏗️ `genCallExpr()` L658: 嵌套过深: 6
- 🏗️ `genMethodCall()` L888: 嵌套过深: 6
- 🏗️ `genUnionDispatch()` L1206: 中等嵌套: 4
- 🏗️ `genAssignExpr()` L1364: 嵌套过深: 5
- 🏗️ `genFunExpr()` L1524: 嵌套过深: 5
- 🏗️ L1: 文件过大: 1829 行
- ❌ L150: 未处理的易出错调用
- ❌ L170: 未处理的易出错调用
- ❌ L181: 未处理的易出错调用
- ❌ L272: 未处理的易出错调用
- ❌ L277: 未处理的易出错调用
- ❌ L392: 未处理的易出错调用
- ❌ L394: 未处理的易出错调用
- ❌ L429: 未处理的易出错调用
- ❌ L432: 未处理的易出错调用
- ❌ L559: 未处理的易出错调用
- ❌ L807: 未处理的易出错调用
- ❌ L1017: 未处理的易出错调用
- ❌ L1162: 未处理的易出错调用
- ❌ L1230: 未处理的易出错调用
- ❌ L1388: 未处理的易出错调用
- ❌ L1406: 未处理的易出错调用
- ❌ L1410: 未处理的易出错调用
- ❌ L1412: 未处理的易出错调用
- ❌ L1414: 未处理的易出错调用
- ❌ L1478: 未处理的易出错调用
- ❌ L1557: 未处理的易出错调用
- ❌ L1665: 未处理的易出错调用
- ❌ L1743: 未处理的易出错调用

**详情**:
- 循环复杂度: 平均: 14.5, 最大: 86
- 认知复杂度: 平均: 18.7, 最大: 98
- 嵌套深度: 平均: 2.1, 最大: 7
- 函数长度: 平均: 55.5 行, 最大: 303 行
- 文件长度: 1377 代码量 (1829 总计)
- 参数数量: 平均: 1.6, 最大: 3
- 代码重复: 0.0% 重复 (0/31)
- 结构分析: 12 个结构问题
- 错误处理: 23/71 个错误被忽略 (32.4%)
- 注释比例: 23.2% (320/1377)
- 命名规范: 无命名违规

### 3. src\main.cpp

**糟糕指数: 49.34**

> 行数: 558 总计, 382 代码, 120 注释 | 函数: 6 | 类: 2

**问题**: 🔄 复杂度问题: 10, ⚠️ 其他问题: 3, 🏗️ 结构问题: 4, ❌ 错误处理问题: 8, 📝 注释问题: 1

#### 函数详情

| 函数 | 行范围 | 行数 | 复杂度 | 嵌套 | 参数 | 注释 |
|:-----|------:|------:|------:|------:|------:|:------:|
| `compileMultiFile` | L230-495 | 266 | 51 | 5 | 3 | ✓ |
| `compileSingleFile` | L107-224 | 118 | 18 | 3 | 2 | ✓ |
| `parseArgs` | L57-82 | 26 | 11 | 10 | 1 | ✗ |
| `main` | L500-557 | 58 | 8 | 4 | 2 | ✓ |
| `parallelJobs` | L96-102 | 7 | 4 | 1 | 2 | ✓ |
| `gccFlags` | L87-91 | 5 | 3 | 1 | 1 | ✓ |

**全部问题 (24)**

- 🔄 `parseArgs()` L57: 复杂度: 11
- 🔄 `compileSingleFile()` L107: 复杂度: 18
- 🔄 `compileMultiFile()` L230: 复杂度: 51
- 🔄 `parseArgs()` L57: 认知复杂度: 31
- 🔄 `compileSingleFile()` L107: 认知复杂度: 24
- 🔄 `compileMultiFile()` L230: 认知复杂度: 61
- 🔄 `main()` L500: 认知复杂度: 16
- 🔄 `parseArgs()` L57: 嵌套深度: 10
- 🔄 `compileMultiFile()` L230: 嵌套深度: 5
- 🔄 `main()` L500: 嵌套深度: 4
- 📏 `compileSingleFile()` L107: 118 代码量
- 📏 `compileMultiFile()` L230: 266 代码量
- 🏗️ `parseArgs()` L57: 嵌套过深: 10
- 🏗️ `compileSingleFile()` L107: 中等嵌套: 3
- 🏗️ `compileMultiFile()` L230: 嵌套过深: 5
- 🏗️ `main()` L500: 中等嵌套: 4
- ❌ L193: 未处理的易出错调用
- ❌ L215: 未处理的易出错调用
- ❌ L298: 未处理的易出错调用
- ❌ L350: 未处理的易出错调用
- ❌ L400: 未处理的易出错调用
- ❌ L409: 未处理的易出错调用
- ❌ L484: 未处理的易出错调用
- ❌ L488: 未处理的易出错调用

**详情**:
- 循环复杂度: 平均: 15.8, 最大: 51
- 认知复杂度: 平均: 23.8, 最大: 61
- 嵌套深度: 平均: 4.0, 最大: 10
- 函数长度: 平均: 80.0 行, 最大: 266 行
- 文件长度: 382 代码量 (558 总计)
- 参数数量: 平均: 1.8, 最大: 3
- 代码重复: 0.0% 重复 (0/6)
- 结构分析: 4 个结构问题
- 错误处理: 8/18 个错误被忽略 (44.4%)
- 注释比例: 31.4% (120/382)
- 命名规范: 无命名违规

### 4. src\Sema\SemAnalyzer.cpp

**糟糕指数: 47.90**

> 行数: 1111 总计, 856 代码, 185 注释 | 函数: 35 | 类: 0

**问题**: 🔄 复杂度问题: 39, ⚠️ 其他问题: 4, 🏗️ 结构问题: 15, ❌ 错误处理问题: 8

#### 函数详情

| 函数 | 行范围 | 行数 | 复杂度 | 嵌套 | 参数 | 注释 |
|:-----|------:|------:|------:|------:|------:|:------:|
| `isAssignable` | L409-562 | 154 | 44 | 5 | 2 | ✗ |
| `propagateCanonicalName` | L829-922 | 94 | 27 | 8 | 2 | ✗ |
| `checkStmt` | L983-1016 | 34 | 25 | 2 | 1 | ✗ |
| `substitute` | L564-629 | 66 | 22 | 4 | 3 | ✗ |
| `semTypeFromBuiltinReturn` | L328-407 | 80 | 18 | 3 | 2 | ✗ |
| `semTypeToCppName` | L204-231 | 28 | 15 | 2 | 1 | ✓ |
| `cppNameOfTypeExpr` | L62-99 | 38 | 14 | 3 | 1 | ✓ |
| `qualifyRecordTypes` | L144-167 | 24 | 13 | 6 | 2 | ✓ |
| `checkDecl` | L934-981 | 48 | 13 | 5 | 1 | ✗ |
| `resolveNamedType` | L262-312 | 51 | 12 | 3 | 1 | ✗ |
| `collectGenericMapping` | L691-733 | 43 | 12 | 4 | 4 | ✗ |
| `materializeCanonicalName` | L772-823 | 52 | 12 | 3 | 2 | ✗ |
| `elemTypeOf` | L242-260 | 19 | 11 | 3 | 1 | ✗ |
| `sealSelfRefs` | L739-759 | 21 | 11 | 5 | 3 | ✗ |
| `extractExports` | L1083-1109 | 27 | 11 | 4 | 0 | ✗ |
| `replaceCanonicalArg` | L102-139 | 38 | 10 | 3 | 3 | ✓ |
| `semTypeFromAuraName` | L170-187 | 18 | 8 | 2 | 1 | ✗ |
| `importExports` | L1042-1065 | 24 | 8 | 2 | 2 | ✗ |
| `checkCallArgs` | L644-676 | 33 | 7 | 2 | 7 | ✗ |
| `importFuncSymbol` | L1023-1040 | 18 | 7 | 2 | 3 | ✓ |
| `matchFuncSig` | L314-326 | 13 | 6 | 2 | 6 | ✗ |
| `buildFuncExport` | L1068-1081 | 14 | 5 | 2 | 3 | ✓ |
| `isIteratorType` | L233-240 | 8 | 4 | 1 | 1 | ✗ |
| `semTypeFromCppName` | L189-201 | 13 | 3 | 2 | 1 | ✗ |
| `checkProgram` | L928-932 | 5 | 3 | 2 | 1 | ✗ |
| `cppNameOf` | L45-49 | 5 | 2 | 1 | 1 | ✓ |
| `checkThrowsContext` | L635-642 | 8 | 2 | 1 | 3 | ✗ |
| `applyGenericMap` | L678-685 | 8 | 2 | 1 | 2 | ✗ |
| `applyTypeArgs` | L761-770 | 10 | 2 | 1 | 3 | ✗ |
| `SemAnalyzer` | L11-11 | 1 | 1 | 0 | 1 | ✗ |
| `analyze` | L13-17 | 5 | 1 | 0 | 1 | ✗ |
| `error` | L23-25 | 3 | 1 | 0 | 2 | ✗ |
| `error` | L27-29 | 3 | 1 | 0 | 4 | ✗ |
| `error` | L31-33 | 3 | 1 | 0 | 3 | ✗ |
| `error` | L35-37 | 3 | 1 | 0 | 5 | ✗ |

**全部问题 (65)**

- 🔄 `cppNameOfTypeExpr()` L62: 复杂度: 14
- 🔄 `qualifyRecordTypes()` L144: 复杂度: 13
- 🔄 `semTypeToCppName()` L204: 复杂度: 15
- 🔄 `elemTypeOf()` L242: 复杂度: 11
- 🔄 `resolveNamedType()` L262: 复杂度: 12
- 🔄 `semTypeFromBuiltinReturn()` L328: 复杂度: 18
- 🔄 `isAssignable()` L409: 复杂度: 44
- 🔄 `substitute()` L564: 复杂度: 22
- 🔄 `collectGenericMapping()` L691: 复杂度: 12
- 🔄 `sealSelfRefs()` L739: 复杂度: 11
- 🔄 `materializeCanonicalName()` L772: 复杂度: 12
- 🔄 `propagateCanonicalName()` L829: 复杂度: 27
- 🔄 `checkDecl()` L934: 复杂度: 13
- 🔄 `checkStmt()` L983: 复杂度: 25
- 🔄 `extractExports()` L1083: 复杂度: 11
- 🔄 `cppNameOfTypeExpr()` L62: 认知复杂度: 20
- 🔄 `replaceCanonicalArg()` L102: 认知复杂度: 16
- 🔄 `qualifyRecordTypes()` L144: 认知复杂度: 25
- 🔄 `semTypeToCppName()` L204: 认知复杂度: 19
- 🔄 `elemTypeOf()` L242: 认知复杂度: 17
- 🔄 `resolveNamedType()` L262: 认知复杂度: 18
- 🔄 `semTypeFromBuiltinReturn()` L328: 认知复杂度: 24
- 🔄 `isAssignable()` L409: 认知复杂度: 54
- 🔄 `substitute()` L564: 认知复杂度: 30
- 🔄 `collectGenericMapping()` L691: 认知复杂度: 20
- 🔄 `sealSelfRefs()` L739: 认知复杂度: 21
- 🔄 `materializeCanonicalName()` L772: 认知复杂度: 18
- 🔄 `propagateCanonicalName()` L829: 认知复杂度: 43
- 🔄 `checkDecl()` L934: 认知复杂度: 23
- 🔄 `checkStmt()` L983: 认知复杂度: 29
- 🔄 `extractExports()` L1083: 认知复杂度: 19
- 🔄 `qualifyRecordTypes()` L144: 嵌套深度: 6
- 🔄 `isAssignable()` L409: 嵌套深度: 5
- 🔄 `substitute()` L564: 嵌套深度: 4
- 🔄 `collectGenericMapping()` L691: 嵌套深度: 4
- 🔄 `sealSelfRefs()` L739: 嵌套深度: 5
- 🔄 `propagateCanonicalName()` L829: 嵌套深度: 8
- 🔄 `checkDecl()` L934: 嵌套深度: 5
- 🔄 `extractExports()` L1083: 嵌套深度: 4
- 📏 `isAssignable()` L409: 154 代码量
- 📏 `matchFuncSig()` L314: 6 参数数量
- 📏 `checkCallArgs()` L644: 7 参数数量
- 🏗️ `cppNameOfTypeExpr()` L62: 中等嵌套: 3
- 🏗️ `replaceCanonicalArg()` L102: 中等嵌套: 3
- 🏗️ `qualifyRecordTypes()` L144: 嵌套过深: 6
- 🏗️ `elemTypeOf()` L242: 中等嵌套: 3
- 🏗️ `resolveNamedType()` L262: 中等嵌套: 3
- 🏗️ `semTypeFromBuiltinReturn()` L328: 中等嵌套: 3
- 🏗️ `isAssignable()` L409: 嵌套过深: 5
- 🏗️ `substitute()` L564: 中等嵌套: 4
- 🏗️ `collectGenericMapping()` L691: 中等嵌套: 4
- 🏗️ `sealSelfRefs()` L739: 嵌套过深: 5
- 🏗️ `materializeCanonicalName()` L772: 中等嵌套: 3
- 🏗️ `propagateCanonicalName()` L829: 嵌套过深: 8
- 🏗️ `checkDecl()` L934: 嵌套过深: 5
- 🏗️ `extractExports()` L1083: 中等嵌套: 4
- 🏗️ L1: 文件过大: 1111 行
- ❌ L342: 未处理的易出错调用
- ❌ L491: 未处理的易出错调用
- ❌ L507: 未处理的易出错调用
- ❌ L848: 未处理的易出错调用
- ❌ L863: 未处理的易出错调用
- ❌ L897: 未处理的易出错调用
- ❌ L911: 未处理的易出错调用
- ❌ L1096: 未处理的易出错调用

**详情**:
- 循环复杂度: 平均: 9.6, 最大: 44
- 认知复杂度: 平均: 14.4, 最大: 54
- 嵌套深度: 平均: 2.4, 最大: 8
- 函数长度: 平均: 28.9 行, 最大: 154 行
- 文件长度: 856 代码量 (1111 总计)
- 参数数量: 平均: 2.3, 最大: 7
- 代码重复: 0.0% 重复 (0/35)
- 结构分析: 15 个结构问题
- 错误处理: 8/49 个错误被忽略 (16.3%)
- 注释比例: 21.6% (185/856)
- 命名规范: 无命名违规

### 5. src\Sema\Checker\ExprInfer.cpp

**糟糕指数: 43.60**

> 行数: 739 总计, 598 代码, 98 注释 | 函数: 22 | 类: 0

**问题**: 🔄 复杂度问题: 21, ⚠️ 其他问题: 2, 🏗️ 结构问题: 9, ❌ 错误处理问题: 7

#### 函数详情

| 函数 | 行范围 | 行数 | 复杂度 | 嵌套 | 参数 | 注释 |
|:-----|------:|------:|------:|------:|------:|:------:|
| `inferMethodCall` | L272-505 | 234 | 69 | 6 | 1 | ✗ |
| `inferCall` | L196-270 | 75 | 23 | 4 | 1 | ✗ |
| `inferExpr` | L10-42 | 33 | 21 | 19 | 1 | ✗ |
| `inferBinaryExpr` | L106-183 | 78 | 17 | 4 | 1 | ✗ |
| `isMatchExhaustive` | L688-737 | 50 | 16 | 6 | 2 | ✗ |
| `inferIndexExpr` | L552-581 | 30 | 15 | 4 | 1 | ✗ |
| `inferMethodCallOnVariant` | L509-535 | 27 | 12 | 4 | 2 | ✓ |
| `inferFunExpr` | L637-682 | 46 | 7 | 2 | 1 | ✗ |
| `inferListExpr` | L73-94 | 22 | 6 | 2 | 1 | ✗ |
| `inferMemberAccess` | L537-550 | 14 | 5 | 3 | 1 | ✗ |
| `inferAssign` | L583-601 | 19 | 5 | 3 | 1 | ✗ |
| `inferUnaryExpr` | L185-194 | 10 | 4 | 2 | 1 | ✗ |
| `inferConditional` | L615-631 | 17 | 4 | 1 | 1 | ✗ |
| `inferIdentifier` | L64-71 | 8 | 3 | 1 | 1 | ✗ |
| `inferRecordExpr` | L96-104 | 9 | 3 | 1 | 1 | ✗ |
| `inferErrorPropagation` | L603-608 | 6 | 2 | 1 | 1 | ✗ |
| `inferIntLiteral` | L44-46 | 3 | 1 | 0 | 1 | ✗ |
| `inferFloatLiteral` | L48-50 | 3 | 1 | 0 | 1 | ✗ |
| `inferStringLiteral` | L52-54 | 3 | 1 | 0 | 1 | ✗ |
| `inferBoolLiteral` | L56-58 | 3 | 1 | 0 | 1 | ✗ |
| `inferNoneLiteral` | L60-62 | 3 | 1 | 0 | 0 | ✗ |
| `inferPipe` | L610-613 | 4 | 1 | 0 | 1 | ✗ |

**全部问题 (38)**

- 🔄 `inferExpr()` L10: 复杂度: 21
- 🔄 `inferBinaryExpr()` L106: 复杂度: 17
- 🔄 `inferCall()` L196: 复杂度: 23
- 🔄 `inferMethodCall()` L272: 复杂度: 69
- 🔄 `inferMethodCallOnVariant()` L509: 复杂度: 12
- 🔄 `inferIndexExpr()` L552: 复杂度: 15
- 🔄 `isMatchExhaustive()` L688: 复杂度: 16
- 🔄 `inferExpr()` L10: 认知复杂度: 59
- 🔄 `inferBinaryExpr()` L106: 认知复杂度: 25
- 🔄 `inferCall()` L196: 认知复杂度: 31
- 🔄 `inferMethodCall()` L272: 认知复杂度: 81
- 🔄 `inferMethodCallOnVariant()` L509: 认知复杂度: 20
- 🔄 `inferIndexExpr()` L552: 认知复杂度: 23
- 🔄 `isMatchExhaustive()` L688: 认知复杂度: 28
- 🔄 `inferExpr()` L10: 嵌套深度: 19
- 🔄 `inferBinaryExpr()` L106: 嵌套深度: 4
- 🔄 `inferCall()` L196: 嵌套深度: 4
- 🔄 `inferMethodCall()` L272: 嵌套深度: 6
- 🔄 `inferMethodCallOnVariant()` L509: 嵌套深度: 4
- 🔄 `inferIndexExpr()` L552: 嵌套深度: 4
- 🔄 `isMatchExhaustive()` L688: 嵌套深度: 6
- 📏 `inferMethodCall()` L272: 234 代码量
- 🏗️ `inferExpr()` L10: 嵌套过深: 19
- 🏗️ `inferBinaryExpr()` L106: 中等嵌套: 4
- 🏗️ `inferCall()` L196: 中等嵌套: 4
- 🏗️ `inferMethodCall()` L272: 嵌套过深: 6
- 🏗️ `inferMethodCallOnVariant()` L509: 中等嵌套: 4
- 🏗️ `inferMemberAccess()` L537: 中等嵌套: 3
- 🏗️ `inferIndexExpr()` L552: 中等嵌套: 4
- 🏗️ `inferAssign()` L583: 中等嵌套: 3
- 🏗️ `isMatchExhaustive()` L688: 嵌套过深: 6
- ❌ L239: 未处理的易出错调用
- ❌ L250: 未处理的易出错调用
- ❌ L261: 未处理的易出错调用
- ❌ L286: 未处理的易出错调用
- ❌ L398: 未处理的易出错调用
- ❌ L454: 未处理的易出错调用
- ❌ L485: 未处理的易出错调用

**详情**:
- 循环复杂度: 平均: 9.9, 最大: 69
- 认知复杂度: 平均: 15.6, 最大: 81
- 嵌套深度: 平均: 2.9, 最大: 19
- 函数长度: 平均: 31.7 行, 最大: 234 行
- 文件长度: 598 代码量 (739 总计)
- 参数数量: 平均: 1.0, 最大: 2
- 代码重复: 0.0% 重复 (0/22)
- 结构分析: 9 个结构问题
- 错误处理: 7/56 个错误被忽略 (12.5%)
- 注释比例: 16.4% (98/598)
- 命名规范: 无命名违规

### 6. src\Sema\Checker\DeclChecker.cpp

**糟糕指数: 42.44**

> 行数: 779 总计, 591 代码, 142 注释 | 函数: 16 | 类: 0

**问题**: 🔄 复杂度问题: 21, ⚠️ 其他问题: 4, 🏗️ 结构问题: 10, ❌ 错误处理问题: 3

#### 函数详情

| 函数 | 行范围 | 行数 | 复杂度 | 嵌套 | 参数 | 注释 |
|:-----|------:|------:|------:|------:|------:|:------:|
| `resolveType` | L345-470 | 126 | 34 | 5 | 1 | ✗ |
| `checkMethodBody` | L644-776 | 133 | 28 | 7 | 1 | ✗ |
| `declareDecl` | L228-339 | 112 | 27 | 5 | 1 | ✗ |
| `verifyImplCompleteness` | L158-226 | 69 | 22 | 7 | 1 | ✓ |
| `forEachGenericRef` | L50-81 | 32 | 20 | 3 | 2 | ✓ |
| `stmtAllPathsReturn` | L563-599 | 37 | 16 | 4 | 1 | ✗ |
| `unionVariantGcUnsafe` | L29-44 | 16 | 13 | 3 | 1 | ✓ |
| `checkFunBody` | L601-642 | 42 | 10 | 2 | 1 | ✗ |
| `buildTypeMethods` | L108-128 | 21 | 8 | 4 | 1 | ✓ |
| `declareTopLevel` | L130-153 | 24 | 8 | 3 | 1 | ✗ |
| `checkDefaultArgRules` | L516-546 | 31 | 8 | 3 | 2 | ✓ |
| `declareInterface` | L475-509 | 35 | 7 | 2 | 1 | ✓ |
| `blockAllPathsReturn` | L554-561 | 8 | 4 | 2 | 1 | ✗ |
| `variantStorageUnsafe` | L11-18 | 8 | 3 | 1 | 1 | ✓ |
| `recordTypeKey` | L98-105 | 8 | 3 | 2 | 1 | ✓ |
| `registerTypeGenerics` | L84-91 | 8 | 1 | 1 | 2 | ✓ |

**全部问题 (37)**

- 🔄 `unionVariantGcUnsafe()` L29: 复杂度: 13
- 🔄 `forEachGenericRef()` L50: 复杂度: 20
- 🔄 `verifyImplCompleteness()` L158: 复杂度: 22
- 🔄 `declareDecl()` L228: 复杂度: 27
- 🔄 `resolveType()` L345: 复杂度: 34
- 🔄 `stmtAllPathsReturn()` L563: 复杂度: 16
- 🔄 `checkMethodBody()` L644: 复杂度: 28
- 🔄 `unionVariantGcUnsafe()` L29: 认知复杂度: 19
- 🔄 `forEachGenericRef()` L50: 认知复杂度: 26
- 🔄 `buildTypeMethods()` L108: 认知复杂度: 16
- 🔄 `verifyImplCompleteness()` L158: 认知复杂度: 36
- 🔄 `declareDecl()` L228: 认知复杂度: 37
- 🔄 `resolveType()` L345: 认知复杂度: 44
- 🔄 `stmtAllPathsReturn()` L563: 认知复杂度: 24
- 🔄 `checkMethodBody()` L644: 认知复杂度: 42
- 🔄 `buildTypeMethods()` L108: 嵌套深度: 4
- 🔄 `verifyImplCompleteness()` L158: 嵌套深度: 7
- 🔄 `declareDecl()` L228: 嵌套深度: 5
- 🔄 `resolveType()` L345: 嵌套深度: 5
- 🔄 `stmtAllPathsReturn()` L563: 嵌套深度: 4
- 🔄 `checkMethodBody()` L644: 嵌套深度: 7
- 📏 `declareDecl()` L228: 112 代码量
- 📏 `resolveType()` L345: 126 代码量
- 📏 `checkMethodBody()` L644: 133 代码量
- 🏗️ `unionVariantGcUnsafe()` L29: 中等嵌套: 3
- 🏗️ `forEachGenericRef()` L50: 中等嵌套: 3
- 🏗️ `buildTypeMethods()` L108: 中等嵌套: 4
- 🏗️ `declareTopLevel()` L130: 中等嵌套: 3
- 🏗️ `verifyImplCompleteness()` L158: 嵌套过深: 7
- 🏗️ `declareDecl()` L228: 嵌套过深: 5
- 🏗️ `resolveType()` L345: 嵌套过深: 5
- 🏗️ `checkDefaultArgRules()` L516: 中等嵌套: 3
- 🏗️ `stmtAllPathsReturn()` L563: 中等嵌套: 4
- 🏗️ `checkMethodBody()` L644: 嵌套过深: 7
- ❌ L133: 未处理的易出错调用
- ❌ L198: 未处理的易出错调用
- ❌ L446: 未处理的易出错调用

**详情**:
- 循环复杂度: 平均: 13.3, 最大: 34
- 认知复杂度: 平均: 20.0, 最大: 44
- 嵌套深度: 平均: 3.4, 最大: 7
- 函数长度: 平均: 44.4 行, 最大: 133 行
- 文件长度: 591 代码量 (779 总计)
- 参数数量: 平均: 1.2, 最大: 2
- 代码重复: 0.0% 重复 (0/16)
- 结构分析: 10 个结构问题
- 错误处理: 3/22 个错误被忽略 (13.6%)
- 注释比例: 24.0% (142/591)
- 命名规范: 无命名违规

### 7. src\CodeGen\DeclGen.cpp

**糟糕指数: 40.86**

> 行数: 946 总计, 721 代码, 144 注释 | 函数: 16 | 类: 0

**问题**: 🔄 复杂度问题: 24, ⚠️ 其他问题: 6, 🏗️ 结构问题: 12, ❌ 错误处理问题: 4

#### 函数详情

| 函数 | 行范围 | 行数 | 复杂度 | 嵌套 | 参数 | 注释 |
|:-----|------:|------:|------:|------:|------:|:------:|
| `genMethodDecl` | L609-757 | 149 | 38 | 5 | 4 | ✗ |
| `genInterfaceDecl` | L129-242 | 114 | 28 | 4 | 2 | ✗ |
| `genIfaceAdapter` | L248-362 | 115 | 26 | 5 | 3 | ✗ |
| `genFunDecl` | L413-535 | 123 | 26 | 3 | 4 | ✗ |
| `funSignature` | L537-607 | 71 | 21 | 4 | 2 | ✗ |
| `collectTParams` | L860-906 | 47 | 21 | 3 | 2 | ✗ |
| `genRecordStruct` | L63-123 | 61 | 14 | 4 | 5 | ✗ |
| `genTypeDecl` | L11-61 | 51 | 12 | 4 | 3 | ✗ |
| `genConstructor` | L763-806 | 44 | 9 | 3 | 2 | ✗ |
| `collectFunTParams` | L908-931 | 24 | 9 | 4 | 1 | ✗ |
| `registerParamTracking` | L377-393 | 17 | 7 | 2 | 1 | ✗ |
| `constructorSignature` | L808-828 | 21 | 7 | 3 | 2 | ✗ |
| `registerRawParamTracking` | L395-411 | 17 | 6 | 3 | 1 | ✗ |
| `collectMethodTParams` | L933-943 | 11 | 5 | 2 | 1 | ✗ |
| `genMainEntry` | L834-854 | 21 | 3 | 1 | 3 | ✗ |
| `clearVarTrackingState` | L368-375 | 8 | 1 | 0 | 0 | ✗ |

**全部问题 (44)**

- 🔄 `genTypeDecl()` L11: 复杂度: 12
- 🔄 `genRecordStruct()` L63: 复杂度: 14
- 🔄 `genInterfaceDecl()` L129: 复杂度: 28
- 🔄 `genIfaceAdapter()` L248: 复杂度: 26
- 🔄 `genFunDecl()` L413: 复杂度: 26
- 🔄 `funSignature()` L537: 复杂度: 21
- 🔄 `genMethodDecl()` L609: 复杂度: 38
- 🔄 `collectTParams()` L860: 复杂度: 21
- 🔄 `genTypeDecl()` L11: 认知复杂度: 20
- 🔄 `genRecordStruct()` L63: 认知复杂度: 22
- 🔄 `genInterfaceDecl()` L129: 认知复杂度: 36
- 🔄 `genIfaceAdapter()` L248: 认知复杂度: 36
- 🔄 `genFunDecl()` L413: 认知复杂度: 32
- 🔄 `funSignature()` L537: 认知复杂度: 29
- 🔄 `genMethodDecl()` L609: 认知复杂度: 48
- 🔄 `collectTParams()` L860: 认知复杂度: 27
- 🔄 `collectFunTParams()` L908: 认知复杂度: 17
- 🔄 `genTypeDecl()` L11: 嵌套深度: 4
- 🔄 `genRecordStruct()` L63: 嵌套深度: 4
- 🔄 `genInterfaceDecl()` L129: 嵌套深度: 4
- 🔄 `genIfaceAdapter()` L248: 嵌套深度: 5
- 🔄 `funSignature()` L537: 嵌套深度: 4
- 🔄 `genMethodDecl()` L609: 嵌套深度: 5
- 🔄 `collectFunTParams()` L908: 嵌套深度: 4
- 📏 `genInterfaceDecl()` L129: 114 代码量
- 📏 `genIfaceAdapter()` L248: 115 代码量
- 📏 `genFunDecl()` L413: 123 代码量
- 📏 `genMethodDecl()` L609: 149 代码量
- 🏗️ `genTypeDecl()` L11: 中等嵌套: 4
- 🏗️ `genRecordStruct()` L63: 中等嵌套: 4
- 🏗️ `genInterfaceDecl()` L129: 中等嵌套: 4
- 🏗️ `genIfaceAdapter()` L248: 嵌套过深: 5
- 🏗️ `registerRawParamTracking()` L395: 中等嵌套: 3
- 🏗️ `genFunDecl()` L413: 中等嵌套: 3
- 🏗️ `funSignature()` L537: 中等嵌套: 4
- 🏗️ `genMethodDecl()` L609: 嵌套过深: 5
- 🏗️ `genConstructor()` L763: 中等嵌套: 3
- 🏗️ `constructorSignature()` L808: 中等嵌套: 3
- 🏗️ `collectTParams()` L860: 中等嵌套: 3
- 🏗️ `collectFunTParams()` L908: 中等嵌套: 4
- ❌ L331: 未处理的易出错调用
- ❌ L391: 未处理的易出错调用
- ❌ L739: 未处理的易出错调用
- ❌ L882: 未处理的易出错调用

**详情**:
- 循环复杂度: 平均: 14.6, 最大: 38
- 认知复杂度: 平均: 20.8, 最大: 48
- 嵌套深度: 平均: 3.1, 最大: 5
- 函数长度: 平均: 55.9 行, 最大: 149 行
- 文件长度: 721 代码量 (946 总计)
- 参数数量: 平均: 2.3, 最大: 5
- 代码重复: 0.0% 重复 (0/16)
- 结构分析: 12 个结构问题
- 错误处理: 4/28 个错误被忽略 (14.3%)
- 注释比例: 20.0% (144/721)
- 命名规范: 无命名违规

### 8. runtime\gc\safepoint.cpp

**糟糕指数: 40.21**

> 行数: 645 总计, 428 代码, 182 注释 | 函数: 13 | 类: 0

**问题**: 🔄 复杂度问题: 8, ⚠️ 其他问题: 3, 🏗️ 结构问题: 4, 📝 注释问题: 1, 🏷️ 命名问题: 3

#### 函数详情

| 函数 | 行范围 | 行数 | 复杂度 | 嵌套 | 参数 | 注释 |
|:-----|------:|------:|------:|------:|------:|:------:|
| `safepoint` | L42-242 | 201 | 36 | 6 | 0 | ✓ |
| `waitForRootThreadsStopped` | L598-642 | 45 | 9 | 4 | 0 | ✓ |
| `logGcEvent` | L385-434 | 50 | 7 | 2 | 1 | ✗ |
| `forceGc` | L248-294 | 47 | 6 | 5 | 0 | ✗ |
| `startConcurrentGc` | L463-550 | 88 | 6 | 4 | 0 | ✓ |
| `broadcastInterrupt` | L564-589 | 26 | 5 | 2 | 0 | ✓ |
| `writeBarrier` | L23-37 | 15 | 4 | 2 | 3 | ✓ |
| `getStats` | L296-318 | 23 | 4 | 1 | 0 | ✗ |
| `appendFmt` | L343-354 | 12 | 4 | 1 | 4 | ✓ |
| `fmtBytes(size_t bytes, char* buf, size_t bufSize)` | L321-330 | 10 | 3 | 2 | 3 | ✓ |
| `recordGcEvent` | L357-383 | 27 | 3 | 1 | 8 | ✓ |
| `gcKindName(uint8_t kind)` | L335-338 | 4 | 2 | 0 | 1 | ✓ |
| `gc_stats_string()` | L436-451 | 16 | 1 | 0 | 0 | ✗ |

**全部问题 (17)**

- 🔄 `safepoint()` L42: 复杂度: 36
- 🔄 `safepoint()` L42: 认知复杂度: 48
- 🔄 `forceGc()` L248: 认知复杂度: 16
- 🔄 `waitForRootThreadsStopped()` L598: 认知复杂度: 17
- 🔄 `safepoint()` L42: 嵌套深度: 6
- 🔄 `forceGc()` L248: 嵌套深度: 5
- 🔄 `startConcurrentGc()` L463: 嵌套深度: 4
- 🔄 `waitForRootThreadsStopped()` L598: 嵌套深度: 4
- 📏 `safepoint()` L42: 201 代码量
- 📏 `recordGcEvent()` L357: 8 参数数量
- 🏗️ `safepoint()` L42: 嵌套过深: 6
- 🏗️ `forceGc()` L248: 嵌套过深: 5
- 🏗️ `startConcurrentGc()` L463: 中等嵌套: 4
- 🏗️ `waitForRootThreadsStopped()` L598: 中等嵌套: 4
- 🏷️ `fmtBytes(size_t bytes, char* buf, size_t bufSize)()` L321: "fmtBytes(size_t bytes, char* buf, size_t bufSize)" - camelCase/snake_case/PascalCase
- 🏷️ `gcKindName(uint8_t kind)()` L335: "gcKindName(uint8_t kind)" - camelCase/snake_case/PascalCase
- 🏷️ `gc_stats_string()()` L436: "gc_stats_string()" - camelCase/snake_case/PascalCase

**详情**:
- 循环复杂度: 平均: 6.9, 最大: 36
- 认知复杂度: 平均: 11.5, 最大: 48
- 嵌套深度: 平均: 2.3, 最大: 6
- 函数长度: 平均: 43.4 行, 最大: 201 行
- 文件长度: 428 代码量 (645 总计)
- 参数数量: 平均: 1.5, 最大: 8
- 代码重复: 0.0% 重复 (0/13)
- 结构分析: 4 个结构问题
- 错误处理: 未检测到易出错调用
- 注释比例: 42.5% (182/428)
- 命名规范: 发现 3 个违规

### 9. src\CodeGen\TypeMap.cpp

**糟糕指数: 39.82**

> 行数: 436 总计, 341 代码, 67 注释 | 函数: 13 | 类: 0

**问题**: 🔄 复杂度问题: 8, ⚠️ 其他问题: 3, 📋 重复问题: 1, 🏗️ 结构问题: 3, ❌ 错误处理问题: 2

#### 函数详情

| 函数 | 行范围 | 行数 | 复杂度 | 嵌套 | 参数 | 注释 |
|:-----|------:|------:|------:|------:|------:|:------:|
| `mapType` | L63-221 | 159 | 58 | 9 | 1 | ✗ |
| `mapSemType` | L284-379 | 96 | 44 | 4 | 1 | ✗ |
| `genTypeDescriptor` | L385-433 | 49 | 11 | 3 | 4 | ✗ |
| `isIfaceViewTypeName` | L45-57 | 13 | 7 | 2 | 1 | ✓ |
| `mapNamedType` | L231-258 | 28 | 6 | 2 | 1 | ✗ |
| `isGcPointerType` | L32-39 | 8 | 5 | 1 | 1 | ✗ |
| `isValueType` | L14-21 | 8 | 3 | 1 | 1 | ✗ |
| `isHeapType` | L23-30 | 8 | 3 | 1 | 1 | ✗ |
| `optionalElemOf` | L223-229 | 7 | 3 | 1 | 1 | ✗ |
| `mapValueType` | L265-272 | 8 | 2 | 1 | 1 | ✗ |
| `registerTypeName` | L10-12 | 3 | 1 | 0 | 2 | ✗ |
| `mapGenericRef` | L260-263 | 4 | 1 | 0 | 1 | ✗ |
| `mapParamType` | L274-278 | 5 | 1 | 0 | 1 | ✗ |

**全部问题 (15)**

- 🔄 `mapType()` L63: 复杂度: 58
- 🔄 `mapSemType()` L284: 复杂度: 44
- 🔄 `genTypeDescriptor()` L385: 复杂度: 11
- 🔄 `mapType()` L63: 认知复杂度: 76
- 🔄 `mapSemType()` L284: 认知复杂度: 52
- 🔄 `genTypeDescriptor()` L385: 认知复杂度: 17
- 🔄 `mapType()` L63: 嵌套深度: 9
- 🔄 `mapSemType()` L284: 嵌套深度: 4
- 📏 `mapType()` L63: 159 代码量
- 📋 `isValueType()` L14: 重复模式: isValueType, isHeapType
- 🏗️ `mapType()` L63: 嵌套过深: 9
- 🏗️ `mapSemType()` L284: 中等嵌套: 4
- 🏗️ `genTypeDescriptor()` L385: 中等嵌套: 3
- ❌ L51: 未处理的易出错调用
- ❌ L190: 未处理的易出错调用

**详情**:
- 循环复杂度: 平均: 11.2, 最大: 58
- 认知复杂度: 平均: 15.0, 最大: 76
- 嵌套深度: 平均: 1.9, 最大: 9
- 函数长度: 平均: 30.5 行, 最大: 159 行
- 文件长度: 341 代码量 (436 总计)
- 参数数量: 平均: 1.3, 最大: 4
- 代码重复: 7.7% 重复 (1/13)
- 结构分析: 3 个结构问题
- 错误处理: 2/13 个错误被忽略 (15.4%)
- 注释比例: 19.6% (67/341)
- 命名规范: 无命名违规

### 10. src\Sema\Checker\StmtChecker.cpp

**糟糕指数: 32.02**

> 行数: 610 总计, 470 代码, 87 注释 | 函数: 23 | 类: 0

**问题**: 🔄 复杂度问题: 9, ⚠️ 其他问题: 1, 🏗️ 结构问题: 4, ❌ 错误处理问题: 8

#### 函数详情

| 函数 | 行范围 | 行数 | 复杂度 | 嵌套 | 参数 | 注释 |
|:-----|------:|------:|------:|------:|------:|:------:|
| `checkMatchStmt` | L288-374 | 87 | 27 | 6 | 1 | ✗ |
| `checkLetDecl` | L87-150 | 64 | 15 | 2 | 1 | ✗ |
| `checkConstDecl` | L152-205 | 54 | 14 | 2 | 1 | ✗ |
| `checkLockStmt` | L548-607 | 60 | 12 | 3 | 1 | ✗ |
| `checkSpawnStmt` | L452-502 | 51 | 9 | 2 | 1 | ✗ |
| `literalKey` | L30-37 | 8 | 7 | 1 | 1 | ✓ |
| `checkIfStmt` | L243-257 | 15 | 7 | 2 | 1 | ✗ |
| `checkSyncStmt` | L390-413 | 24 | 7 | 2 | 1 | ✗ |
| `literalSemType` | L20-27 | 8 | 6 | 1 | 1 | ✓ |
| `constCompatibleWith` | L40-51 | 12 | 6 | 3 | 2 | ✓ |
| `checkReturnStmt` | L207-232 | 26 | 6 | 2 | 1 | ✗ |
| `checkSyncForStmt` | L415-450 | 36 | 6 | 2 | 1 | ✗ |
| `isSameLockExpr` | L527-546 | 20 | 6 | 2 | 2 | ✓ |
| `rejectStandaloneNone` | L66-77 | 12 | 4 | 3 | 2 | ✓ |
| `listContainsError` | L7-13 | 7 | 3 | 1 | 1 | ✓ |
| `checkBlock` | L57-63 | 7 | 3 | 2 | 1 | ✗ |
| `checkThrowStmt` | L234-241 | 8 | 3 | 1 | 1 | ✗ |
| `checkWhileStmt` | L259-266 | 8 | 3 | 1 | 1 | ✗ |
| `checkTryCatchStmt` | L376-388 | 13 | 3 | 1 | 1 | ✗ |
| `checkSyncMax` | L80-85 | 6 | 2 | 1 | 2 | ✓ |
| `checkForStmt` | L268-281 | 14 | 2 | 1 | 1 | ✗ |
| `checkLoopStmt` | L283-286 | 4 | 2 | 1 | 1 | ✗ |
| `checkExprStmt` | L504-508 | 5 | 2 | 1 | 1 | ✗ |

**全部问题 (21)**

- 🔄 `checkLetDecl()` L87: 复杂度: 15
- 🔄 `checkConstDecl()` L152: 复杂度: 14
- 🔄 `checkMatchStmt()` L288: 复杂度: 27
- 🔄 `checkLockStmt()` L548: 复杂度: 12
- 🔄 `checkLetDecl()` L87: 认知复杂度: 19
- 🔄 `checkConstDecl()` L152: 认知复杂度: 18
- 🔄 `checkMatchStmt()` L288: 认知复杂度: 39
- 🔄 `checkLockStmt()` L548: 认知复杂度: 18
- 🔄 `checkMatchStmt()` L288: 嵌套深度: 6
- 🏗️ `constCompatibleWith()` L40: 中等嵌套: 3
- 🏗️ `rejectStandaloneNone()` L66: 中等嵌套: 3
- 🏗️ `checkMatchStmt()` L288: 嵌套过深: 6
- 🏗️ `checkLockStmt()` L548: 中等嵌套: 3
- ❌ L12: 未处理的易出错调用
- ❌ L137: 未处理的易出错调用
- ❌ L147: 未处理的易出错调用
- ❌ L192: 未处理的易出错调用
- ❌ L301: 未处理的易出错调用
- ❌ L305: 未处理的易出错调用
- ❌ L540: 未处理的易出错调用
- ❌ L590: 未处理的易出错调用

**详情**:
- 循环复杂度: 平均: 6.7, 最大: 27
- 认知复杂度: 平均: 10.5, 最大: 39
- 嵌套深度: 平均: 1.9, 最大: 6
- 函数长度: 平均: 23.9 行, 最大: 87 行
- 文件长度: 470 代码量 (610 总计)
- 参数数量: 平均: 1.2, 最大: 2
- 代码重复: 4.3% 重复 (1/23)
- 结构分析: 4 个结构问题
- 错误处理: 8/34 个错误被忽略 (23.5%)
- 注释比例: 18.5% (87/470)
- 命名规范: 无命名违规

### 11. src\Lexer.cpp

**糟糕指数: 28.68**

> 行数: 319 总计, 259 代码, 17 注释 | 函数: 17 | 类: 0

**问题**: 🔄 复杂度问题: 9, 🏗️ 结构问题: 3, 📝 注释问题: 1

#### 函数详情

| 函数 | 行范围 | 行数 | 复杂度 | 嵌套 | 参数 | 注释 |
|:-----|------:|------:|------:|------:|------:|:------:|
| `scanOperatorOrDelimiter` | L53-127 | 75 | 38 | 2 | 1 | ✗ |
| `scanNumber` | L181-249 | 69 | 18 | 4 | 0 | ✗ |
| `scanString` | L251-278 | 28 | 11 | 3 | 0 | ✗ |
| `skipWhitespaceAndComments` | L129-148 | 20 | 7 | 6 | 0 | ✗ |
| `scanOne` | L29-51 | 23 | 4 | 1 | 0 | ✗ |
| `scanAll` | L9-27 | 19 | 3 | 2 | 0 | ✗ |
| `skipBlockComment` | L158-169 | 12 | 3 | 2 | 0 | ✗ |
| `advance` | L280-290 | 11 | 3 | 1 | 0 | ✗ |
| `skipLineComment` | L150-156 | 7 | 2 | 1 | 0 | ✗ |
| `scanIdentifierOrKeyword` | L171-179 | 9 | 2 | 1 | 0 | ✗ |
| `peek` | L292-295 | 4 | 2 | 1 | 0 | ✗ |
| `peekNext` | L297-300 | 4 | 2 | 1 | 0 | ✗ |
| `Lexer` | L7-7 | 1 | 1 | 0 | 1 | ✗ |
| `atEnd` | L302-304 | 3 | 1 | 0 | 0 | ✗ |
| `makeToken` | L306-308 | 3 | 1 | 0 | 1 | ✗ |
| `makeToken` | L310-312 | 3 | 1 | 0 | 2 | ✗ |
| `makeError` | L314-316 | 3 | 1 | 0 | 1 | ✗ |

**全部问题 (12)**

- 🔄 `scanOperatorOrDelimiter()` L53: 复杂度: 38
- 🔄 `scanNumber()` L181: 复杂度: 18
- 🔄 `scanString()` L251: 复杂度: 11
- 🔄 `scanOperatorOrDelimiter()` L53: 认知复杂度: 42
- 🔄 `skipWhitespaceAndComments()` L129: 认知复杂度: 19
- 🔄 `scanNumber()` L181: 认知复杂度: 26
- 🔄 `scanString()` L251: 认知复杂度: 17
- 🔄 `skipWhitespaceAndComments()` L129: 嵌套深度: 6
- 🔄 `scanNumber()` L181: 嵌套深度: 4
- 🏗️ `skipWhitespaceAndComments()` L129: 嵌套过深: 6
- 🏗️ `scanNumber()` L181: 中等嵌套: 4
- 🏗️ `scanString()` L251: 中等嵌套: 3

**详情**:
- 循环复杂度: 平均: 5.9, 最大: 38
- 认知复杂度: 平均: 8.8, 最大: 42
- 嵌套深度: 平均: 1.5, 最大: 6
- 函数长度: 平均: 17.3 行, 最大: 75 行
- 文件长度: 259 代码量 (319 总计)
- 参数数量: 平均: 0.4, 最大: 2
- 代码重复: 0.0% 重复 (0/17)
- 结构分析: 3 个结构问题
- 错误处理: 未检测到易出错调用
- 注释比例: 6.6% (17/259)
- 命名规范: 无命名违规

### 12. src\Parser\ExprParser.cpp

**糟糕指数: 27.22**

> 行数: 382 总计, 301 代码, 25 注释 | 函数: 15 | 类: 0

**问题**: 🔄 复杂度问题: 8, ⚠️ 其他问题: 2, 🏗️ 结构问题: 4, ❌ 错误处理问题: 23, 📝 注释问题: 1

#### 函数详情

| 函数 | 行范围 | 行数 | 复杂度 | 嵌套 | 参数 | 注释 |
|:-----|------:|------:|------:|------:|------:|:------:|
| `parsePrimary` | L240-355 | 116 | 17 | 3 | 0 | ✗ |
| `parseAssignment` | L13-57 | 45 | 11 | 5 | 0 | ✗ |
| `parseCall` | L184-238 | 55 | 10 | 6 | 0 | ✗ |
| `parseBinaryLevel` | L94-121 | 28 | 6 | 4 | 3 | ✗ |
| `parseConditional` | L59-75 | 17 | 4 | 1 | 0 | ✗ |
| `parsePipe` | L77-92 | 16 | 4 | 2 | 0 | ✗ |
| `parseUnary` | L154-182 | 29 | 4 | 1 | 0 | ✗ |
| `parseFunExpr` | L360-379 | 20 | 4 | 1 | 0 | ✓ |
| `parseExpr` | L9-11 | 3 | 1 | 0 | 0 | ✗ |
| `parseOr` | L123-126 | 4 | 1 | 0 | 0 | ✗ |
| `parseAnd` | L128-131 | 4 | 1 | 0 | 0 | ✗ |
| `parseEquality` | L133-136 | 4 | 1 | 0 | 0 | ✗ |
| `parseComparison` | L138-142 | 5 | 1 | 0 | 0 | ✗ |
| `parseAddSub` | L144-147 | 4 | 1 | 0 | 0 | ✗ |
| `parseMulDiv` | L149-152 | 4 | 1 | 0 | 0 | ✗ |

**全部问题 (36)**

- 🔄 `parseAssignment()` L13: 复杂度: 11
- 🔄 `parsePrimary()` L240: 复杂度: 17
- 🔄 `parseAssignment()` L13: 认知复杂度: 21
- 🔄 `parseCall()` L184: 认知复杂度: 22
- 🔄 `parsePrimary()` L240: 认知复杂度: 23
- 🔄 `parseAssignment()` L13: 嵌套深度: 5
- 🔄 `parseBinaryLevel()` L94: 嵌套深度: 4
- 🔄 `parseCall()` L184: 嵌套深度: 6
- 📏 `parsePrimary()` L240: 116 代码量
- 🏗️ `parseAssignment()` L13: 嵌套过深: 5
- 🏗️ `parseBinaryLevel()` L94: 中等嵌套: 4
- 🏗️ `parseCall()` L184: 嵌套过深: 6
- 🏗️ `parsePrimary()` L240: 中等嵌套: 3
- ❌ L20: 未处理的易出错调用
- ❌ L41: 未处理的易出错调用
- ❌ L50: 未处理的易出错调用
- ❌ L70: 未处理的易出错调用
- ❌ L84: 未处理的易出错调用
- ❌ L110: 未处理的易出错调用
- ❌ L157: 未处理的易出错调用
- ❌ L165: 未处理的易出错调用
- ❌ L176: 未处理的易出错调用
- ❌ L190: 未处理的易出错调用
- ❌ L206: 未处理的易出错调用
- ❌ L220: 未处理的易出错调用
- ❌ L227: 未处理的易出错调用
- ❌ L257: 未处理的易出错调用
- ❌ L267: 未处理的易出错调用
- ❌ L278: 未处理的易出错调用
- ❌ L285: 未处理的易出错调用
- ❌ L291: 未处理的易出错调用
- ❌ L298: 未处理的易出错调用
- ❌ L309: 未处理的易出错调用
- ❌ L322: 未处理的易出错调用
- ❌ L336: 未处理的易出错调用
- ❌ L363: 未处理的易出错调用

**详情**:
- 循环复杂度: 平均: 4.5, 最大: 17
- 认知复杂度: 平均: 7.5, 最大: 23
- 嵌套深度: 平均: 1.5, 最大: 6
- 函数长度: 平均: 23.6 行, 最大: 116 行
- 文件长度: 301 代码量 (382 总计)
- 参数数量: 平均: 0.2, 最大: 3
- 代码重复: 0.0% 重复 (0/15)
- 结构分析: 4 个结构问题
- 错误处理: 23/23 个错误被忽略 (100.0%)
- 注释比例: 8.3% (25/301)
- 命名规范: 无命名违规

### 13. runtime\gc\compact.cpp

**糟糕指数: 27.19**

> 行数: 968 总计, 676 代码, 179 注释 | 函数: 21 | 类: 0

**问题**: 🔄 复杂度问题: 20, ⚠️ 其他问题: 3, 🏗️ 结构问题: 10, 📝 注释问题: 1

#### 函数详情

| 函数 | 行范围 | 行数 | 复杂度 | 嵌套 | 参数 | 注释 |
|:-----|------:|------:|------:|------:|------:|:------:|
| `computeForwardingAddresses` | L75-202 | 128 | 30 | 3 | 1 | ✗ |
| `compactMediumPages` | L650-764 | 115 | 21 | 4 | 0 | ✗ |
| `updateAllReferences` | L320-414 | 95 | 19 | 3 | 1 | ✗ |
| `sweepLargePages` | L869-965 | 97 | 19 | 4 | 0 | ✗ |
| `rebuildPageList` | L225-318 | 94 | 17 | 3 | 1 | ✗ |
| `updateMediumPageReferences` | L766-839 | 74 | 14 | 3 | 0 | ✗ |
| `shouldCompact` | L30-61 | 32 | 13 | 2 | 1 | ✗ |
| `updateObjectAllFields` | L547-583 | 37 | 12 | 4 | 1 | ✓ |
| `updateInlineArrayElements` | L515-542 | 28 | 9 | 3 | 1 | ✗ |
| `shouldCompactMedium` | L630-648 | 19 | 8 | 2 | 0 | ✗ |
| `updateObjectFields` | L491-513 | 23 | 7 | 2 | 1 | ✗ |
| `isGCAddress` | L615-624 | 10 | 7 | 2 | 1 | ✗ |
| `relocateGlobalRootPtrs` | L432-461 | 30 | 6 | 3 | 0 | ✗ |
| `relocateRootsInForwardMap` | L465-489 | 25 | 5 | 4 | 1 | ✓ |
| `reclaimExcessMediumPages` | L845-857 | 13 | 4 | 2 | 0 | ✓ |
| `GcHeap::findMediumPage(GcObject* obj) const` | L589-597 | 9 | 3 | 2 | 1 | ✗ |
| `GcHeap::findLargePage(GcObject* obj) const` | L599-607 | 9 | 3 | 2 | 1 | ✗ |
| `pageClassOf` | L609-613 | 5 | 3 | 1 | 1 | ✗ |
| `compact` | L63-73 | 11 | 2 | 1 | 1 | ✗ |
| `copyObjectsToNewLocations` | L204-223 | 20 | 2 | 2 | 1 | ✗ |
| `shouldSweepLargePages` | L863-867 | 5 | 2 | 1 | 0 | ✗ |

**全部问题 (32)**

- 🔄 `shouldCompact()` L30: 复杂度: 13
- 🔄 `computeForwardingAddresses()` L75: 复杂度: 30
- 🔄 `rebuildPageList()` L225: 复杂度: 17
- 🔄 `updateAllReferences()` L320: 复杂度: 19
- 🔄 `updateObjectAllFields()` L547: 复杂度: 12
- 🔄 `compactMediumPages()` L650: 复杂度: 21
- 🔄 `updateMediumPageReferences()` L766: 复杂度: 14
- 🔄 `sweepLargePages()` L869: 复杂度: 19
- 🔄 `shouldCompact()` L30: 认知复杂度: 17
- 🔄 `computeForwardingAddresses()` L75: 认知复杂度: 36
- 🔄 `rebuildPageList()` L225: 认知复杂度: 23
- 🔄 `updateAllReferences()` L320: 认知复杂度: 25
- 🔄 `updateObjectAllFields()` L547: 认知复杂度: 20
- 🔄 `compactMediumPages()` L650: 认知复杂度: 29
- 🔄 `updateMediumPageReferences()` L766: 认知复杂度: 20
- 🔄 `sweepLargePages()` L869: 认知复杂度: 27
- 🔄 `relocateRootsInForwardMap()` L465: 嵌套深度: 4
- 🔄 `updateObjectAllFields()` L547: 嵌套深度: 4
- 🔄 `compactMediumPages()` L650: 嵌套深度: 4
- 🔄 `sweepLargePages()` L869: 嵌套深度: 4
- 📏 `computeForwardingAddresses()` L75: 128 代码量
- 📏 `compactMediumPages()` L650: 115 代码量
- 🏗️ `computeForwardingAddresses()` L75: 中等嵌套: 3
- 🏗️ `rebuildPageList()` L225: 中等嵌套: 3
- 🏗️ `updateAllReferences()` L320: 中等嵌套: 3
- 🏗️ `relocateGlobalRootPtrs()` L432: 中等嵌套: 3
- 🏗️ `relocateRootsInForwardMap()` L465: 中等嵌套: 4
- 🏗️ `updateInlineArrayElements()` L515: 中等嵌套: 3
- 🏗️ `updateObjectAllFields()` L547: 中等嵌套: 4
- 🏗️ `compactMediumPages()` L650: 中等嵌套: 4
- 🏗️ `updateMediumPageReferences()` L766: 中等嵌套: 3
- 🏗️ `sweepLargePages()` L869: 中等嵌套: 4

**详情**:
- 循环复杂度: 平均: 9.8, 最大: 30
- 认知复杂度: 平均: 14.9, 最大: 36
- 嵌套深度: 平均: 2.5, 最大: 4
- 函数长度: 平均: 41.9 行, 最大: 128 行
- 文件长度: 676 代码量 (968 总计)
- 参数数量: 平均: 0.7, 最大: 1
- 代码重复: 4.8% 重复 (1/21)
- 结构分析: 10 个结构问题
- 错误处理: 未检测到易出错调用
- 注释比例: 26.5% (179/676)
- 命名规范: 发现 2 个违规

### 14. src\Parser\DeclParser.cpp

**糟糕指数: 26.60**

> 行数: 298 总计, 217 代码, 32 注释 | 函数: 9 | 类: 0

**问题**: 🔄 复杂度问题: 4, 🏗️ 结构问题: 4, ❌ 错误处理问题: 7

#### 函数详情

| 函数 | 行范围 | 行数 | 复杂度 | 嵌套 | 参数 | 注释 |
|:-----|------:|------:|------:|------:|------:|:------:|
| `parseMethodDecl` | L147-210 | 64 | 12 | 3 | 0 | ✗ |
| `parseDecl` | L9-33 | 25 | 10 | 7 | 0 | ✗ |
| `parseConfigDecl` | L249-295 | 47 | 8 | 3 | 0 | ✗ |
| `parseFunDecl` | L35-68 | 34 | 7 | 2 | 0 | ✗ |
| `parseInterfaceDecl` | L111-145 | 35 | 6 | 3 | 0 | ✗ |
| `parseImportDecl` | L212-247 | 36 | 6 | 2 | 0 | ✗ |
| `parseTypeDecl` | L78-109 | 32 | 5 | 2 | 0 | ✗ |
| `parseLetDecl` | L70-72 | 3 | 1 | 0 | 0 | ✗ |
| `parseConstDecl` | L74-76 | 3 | 1 | 0 | 0 | ✗ |

**全部问题 (15)**

- 🔄 `parseMethodDecl()` L147: 复杂度: 12
- 🔄 `parseDecl()` L9: 认知复杂度: 24
- 🔄 `parseMethodDecl()` L147: 认知复杂度: 18
- 🔄 `parseDecl()` L9: 嵌套深度: 7
- 🏗️ `parseDecl()` L9: 嵌套过深: 7
- 🏗️ `parseInterfaceDecl()` L111: 中等嵌套: 3
- 🏗️ `parseMethodDecl()` L147: 中等嵌套: 3
- 🏗️ `parseConfigDecl()` L249: 中等嵌套: 3
- ❌ L38: 未处理的易出错调用
- ❌ L43: 未处理的易出错调用
- ❌ L81: 未处理的易出错调用
- ❌ L114: 未处理的易出错调用
- ❌ L150: 未处理的易出错调用
- ❌ L215: 未处理的易出错调用
- ❌ L252: 未处理的易出错调用

**详情**:
- 循环复杂度: 平均: 6.2, 最大: 12
- 认知复杂度: 平均: 11.1, 最大: 24
- 嵌套深度: 平均: 2.4, 最大: 7
- 函数长度: 平均: 31.0 行, 最大: 64 行
- 文件长度: 217 代码量 (298 总计)
- 参数数量: 平均: 0.0, 最大: 0
- 代码重复: 0.0% 重复 (0/9)
- 结构分析: 4 个结构问题
- 错误处理: 7/7 个错误被忽略 (100.0%)
- 注释比例: 14.7% (32/217)
- 命名规范: 无命名违规

### 15. src\CodeGen\CodeGen.cpp

**糟糕指数: 26.24**

> 行数: 290 总计, 206 代码, 50 注释 | 函数: 11 | 类: 0

**问题**: 🔄 复杂度问题: 3, ⚠️ 其他问题: 2, 🏗️ 结构问题: 1, ❌ 错误处理问题: 3

#### 函数详情

| 函数 | 行范围 | 行数 | 复杂度 | 嵌套 | 参数 | 注释 |
|:-----|------:|------:|------:|------:|------:|:------:|
| `generate` | L24-207 | 184 | 52 | 4 | 6 | ✗ |
| `genDecl` | L213-237 | 25 | 7 | 2 | 5 | ✗ |
| `indent` | L245-247 | 3 | 2 | 1 | 1 | ✗ |
| `safeName` | L260-279 | 20 | 2 | 1 | 1 | ✗ |
| `setConfig` | L13-15 | 3 | 1 | 0 | 1 | ✗ |
| `CodeGenerator` | L21-22 | 2 | 1 | 0 | 1 | ✗ |
| `newline` | L243-243 | 1 | 1 | 0 | 1 | ✗ |
| `dedent` | L249-249 | 1 | 1 | 0 | 1 | ✗ |
| `writeLine` | L251-254 | 4 | 1 | 0 | 2 | ✗ |
| `indentStr` | L256-258 | 3 | 1 | 0 | 0 | ✗ |
| `error` | L285-287 | 3 | 1 | 0 | 2 | ✗ |

**全部问题 (9)**

- 🔄 `generate()` L24: 复杂度: 52
- 🔄 `generate()` L24: 认知复杂度: 60
- 🔄 `generate()` L24: 嵌套深度: 4
- 📏 `generate()` L24: 184 代码量
- 📏 `generate()` L24: 6 参数数量
- 🏗️ `generate()` L24: 中等嵌套: 4
- ❌ L119: 未处理的易出错调用
- ❌ L120: 未处理的易出错调用
- ❌ L164: 未处理的易出错调用

**详情**:
- 循环复杂度: 平均: 6.4, 最大: 52
- 认知复杂度: 平均: 7.8, 最大: 60
- 嵌套深度: 平均: 0.7, 最大: 4
- 函数长度: 平均: 22.6 行, 最大: 184 行
- 文件长度: 206 代码量 (290 总计)
- 参数数量: 平均: 1.9, 最大: 6
- 代码重复: 0.0% 重复 (0/11)
- 结构分析: 1 个结构问题
- 错误处理: 3/12 个错误被忽略 (25.0%)
- 注释比例: 24.3% (50/206)
- 命名规范: 无命名违规

### 16. runtime\gc\mark_sweep.cpp

**糟糕指数: 23.59**

> 行数: 609 总计, 420 代码, 130 注释 | 函数: 13 | 类: 0

**问题**: 🔄 复杂度问题: 15, ⚠️ 其他问题: 2, 🏗️ 结构问题: 7, 📝 注释问题: 1

#### 函数详情

| 函数 | 行范围 | 行数 | 复杂度 | 嵌套 | 参数 | 注释 |
|:-----|------:|------:|------:|------:|------:|:------:|
| `sweepPhaseAll` | L379-519 | 141 | 23 | 4 | 0 | ✗ |
| `scanRootsOnly` | L70-156 | 87 | 18 | 4 | 1 | ✓ |
| `promoteToOld` | L323-373 | 51 | 14 | 4 | 1 | ✗ |
| `scanStackCandidate` | L160-204 | 45 | 13 | 3 | 1 | ✓ |
| `sweepPhaseYoung` | L263-321 | 59 | 12 | 3 | 0 | ✗ |
| `compactAndReclaim` | L521-583 | 63 | 11 | 3 | 0 | ✗ |
| `markInlineArrayFields` | L233-257 | 25 | 7 | 3 | 1 | ✗ |
| `markFields` | L216-231 | 16 | 5 | 2 | 1 | ✗ |
| `minorGc` | L26-40 | 15 | 3 | 2 | 0 | ✓ |
| `markObject` | L206-214 | 9 | 3 | 1 | 1 | ✗ |
| `mixedGc` | L588-606 | 19 | 3 | 2 | 0 | ✓ |
| `majorGc` | L45-57 | 13 | 1 | 0 | 0 | ✓ |
| `markPhase` | L63-67 | 5 | 1 | 0 | 1 | ✗ |

**全部问题 (23)**

- 🔄 `scanRootsOnly()` L70: 复杂度: 18
- 🔄 `scanStackCandidate()` L160: 复杂度: 13
- 🔄 `sweepPhaseYoung()` L263: 复杂度: 12
- 🔄 `promoteToOld()` L323: 复杂度: 14
- 🔄 `sweepPhaseAll()` L379: 复杂度: 23
- 🔄 `compactAndReclaim()` L521: 复杂度: 11
- 🔄 `scanRootsOnly()` L70: 认知复杂度: 26
- 🔄 `scanStackCandidate()` L160: 认知复杂度: 19
- 🔄 `sweepPhaseYoung()` L263: 认知复杂度: 18
- 🔄 `promoteToOld()` L323: 认知复杂度: 22
- 🔄 `sweepPhaseAll()` L379: 认知复杂度: 31
- 🔄 `compactAndReclaim()` L521: 认知复杂度: 17
- 🔄 `scanRootsOnly()` L70: 嵌套深度: 4
- 🔄 `promoteToOld()` L323: 嵌套深度: 4
- 🔄 `sweepPhaseAll()` L379: 嵌套深度: 4
- 📏 `sweepPhaseAll()` L379: 141 代码量
- 🏗️ `scanRootsOnly()` L70: 中等嵌套: 4
- 🏗️ `scanStackCandidate()` L160: 中等嵌套: 3
- 🏗️ `markInlineArrayFields()` L233: 中等嵌套: 3
- 🏗️ `sweepPhaseYoung()` L263: 中等嵌套: 3
- 🏗️ `promoteToOld()` L323: 中等嵌套: 4
- 🏗️ `sweepPhaseAll()` L379: 中等嵌套: 4
- 🏗️ `compactAndReclaim()` L521: 中等嵌套: 3

**详情**:
- 循环复杂度: 平均: 8.8, 最大: 23
- 认知复杂度: 平均: 13.5, 最大: 31
- 嵌套深度: 平均: 2.4, 最大: 4
- 函数长度: 平均: 42.2 行, 最大: 141 行
- 文件长度: 420 代码量 (609 总计)
- 参数数量: 平均: 0.5, 最大: 1
- 代码重复: 0.0% 重复 (0/13)
- 结构分析: 7 个结构问题
- 错误处理: 0/2 个错误被忽略 (0.0%)
- 注释比例: 31.0% (130/420)
- 命名规范: 无命名违规

### 17. src\Parser\StmtParser.cpp

**糟糕指数: 23.07**

> 行数: 430 总计, 311 代码, 60 注释 | 函数: 16 | 类: 0

**问题**: 🔄 复杂度问题: 5, ⚠️ 其他问题: 1, 📋 重复问题: 2, 🏗️ 结构问题: 3, ❌ 错误处理问题: 24

#### 函数详情

| 函数 | 行范围 | 行数 | 复杂度 | 嵌套 | 参数 | 注释 |
|:-----|------:|------:|------:|------:|------:|:------:|
| `parseStmt` | L9-59 | 51 | 21 | 2 | 0 | ✗ |
| `parseSpawnStmt` | L287-339 | 53 | 12 | 4 | 0 | ✗ |
| `parseSyncStmt` | L197-235 | 39 | 5 | 2 | 0 | ✗ |
| `parseSyncForRest` | L246-285 | 40 | 5 | 2 | 2 | ✓ |
| `parseBlock` | L61-84 | 24 | 4 | 2 | 0 | ✗ |
| `parseReturnStmt` | L143-168 | 26 | 4 | 3 | 0 | ✗ |
| `parseMatchStmt` | L362-393 | 32 | 4 | 3 | 0 | ✗ |
| `parseIfStmt` | L86-107 | 22 | 3 | 2 | 0 | ✗ |
| `parseExprStmt` | L395-427 | 33 | 3 | 1 | 0 | ✗ |
| `parseLockStmt` | L346-360 | 15 | 2 | 1 | 0 | ✓ |
| `parseWhileStmt` | L109-117 | 9 | 1 | 0 | 0 | ✗ |
| `parseLoopStmt` | L119-126 | 8 | 1 | 0 | 0 | ✗ |
| `parseForStmt` | L128-141 | 14 | 1 | 0 | 0 | ✗ |
| `parseThrowStmt` | L170-178 | 9 | 1 | 0 | 0 | ✗ |
| `parseTryCatchStmt` | L180-195 | 16 | 1 | 0 | 0 | ✗ |
| `parseSyncForStmt` | L237-241 | 5 | 1 | 0 | 0 | ✗ |

**全部问题 (34)**

- 🔄 `parseStmt()` L9: 复杂度: 21
- 🔄 `parseSpawnStmt()` L287: 复杂度: 12
- 🔄 `parseStmt()` L9: 认知复杂度: 25
- 🔄 `parseSpawnStmt()` L287: 认知复杂度: 20
- 🔄 `parseSpawnStmt()` L287: 嵌套深度: 4
- 📋 `parseLoopStmt()` L119: 重复模式: parseLoopStmt, parseThrowStmt
- 📋 `parseForStmt()` L128: 重复模式: parseForStmt, parseTryCatchStmt
- 🏗️ `parseReturnStmt()` L143: 中等嵌套: 3
- 🏗️ `parseSpawnStmt()` L287: 中等嵌套: 4
- 🏗️ `parseMatchStmt()` L362: 中等嵌套: 3
- ❌ L43: 未处理的易出错调用
- ❌ L50: 未处理的易出错调用
- ❌ L64: 未处理的易出错调用
- ❌ L89: 未处理的易出错调用
- ❌ L112: 未处理的易出错调用
- ❌ L122: 未处理的易出错调用
- ❌ L131: 未处理的易出错调用
- ❌ L146: 未处理的易出错调用
- ❌ L153: 未处理的易出错调用
- ❌ L173: 未处理的易出错调用
- ❌ L183: 未处理的易出错调用
- ❌ L209: 未处理的易出错调用
- ❌ L225: 未处理的易出错调用
- ❌ L248: 未处理的易出错调用
- ❌ L272: 未处理的易出错调用
- ❌ L273: 未处理的易出错调用
- ❌ L290: 未处理的易出错调用
- ❌ L332: 未处理的易出错调用
- ❌ L333: 未处理的易出错调用
- ❌ L349: 未处理的易出错调用
- ❌ L365: 未处理的易出错调用
- ❌ L402: 未处理的易出错调用
- ❌ L406: 未处理的易出错调用
- ❌ L412: 未处理的易出错调用

**详情**:
- 循环复杂度: 平均: 4.3, 最大: 21
- 认知复杂度: 平均: 7.1, 最大: 25
- 嵌套深度: 平均: 1.4, 最大: 4
- 函数长度: 平均: 24.8 行, 最大: 53 行
- 文件长度: 311 代码量 (430 总计)
- 参数数量: 平均: 0.1, 最大: 2
- 代码重复: 12.5% 重复 (2/16)
- 结构分析: 3 个结构问题
- 错误处理: 24/24 个错误被忽略 (100.0%)
- 注释比例: 19.3% (60/311)
- 命名规范: 无命名违规

### 18. src\Parser\TypeParser.cpp

**糟糕指数: 20.10**

> 行数: 289 总计, 203 代码, 38 注释 | 函数: 8 | 类: 0

**问题**: 🔄 复杂度问题: 2, ⚠️ 其他问题: 1, 🏗️ 结构问题: 1, ❌ 错误处理问题: 11

#### 函数详情

| 函数 | 行范围 | 行数 | 复杂度 | 嵌套 | 参数 | 注释 |
|:-----|------:|------:|------:|------:|------:|:------:|
| `parsePrimaryType` | L30-171 | 142 | 25 | 3 | 0 | ✗ |
| `parseInterfaceMethodSig` | L257-286 | 30 | 6 | 2 | 0 | ✗ |
| `parsePattern` | L177-198 | 22 | 5 | 2 | 0 | ✗ |
| `parseSinglePattern` | L200-228 | 29 | 5 | 2 | 0 | ✗ |
| `parseUnionType` | L13-28 | 16 | 3 | 2 | 0 | ✗ |
| `parseParam` | L234-247 | 14 | 3 | 2 | 0 | ✗ |
| `parseParams` | L249-255 | 7 | 2 | 1 | 0 | ✗ |
| `parseType` | L9-11 | 3 | 1 | 0 | 0 | ✗ |

**全部问题 (15)**

- 🔄 `parsePrimaryType()` L30: 复杂度: 25
- 🔄 `parsePrimaryType()` L30: 认知复杂度: 31
- 📏 `parsePrimaryType()` L30: 142 代码量
- 🏗️ `parsePrimaryType()` L30: 中等嵌套: 3
- ❌ L18: 未处理的易出错调用
- ❌ L36: 未处理的易出错调用
- ❌ L45: 未处理的易出错调用
- ❌ L54: 未处理的易出错调用
- ❌ L77: 未处理的易出错调用
- ❌ L104: 未处理的易出错调用
- ❌ L125: 未处理的易出错调用
- ❌ L150: 未处理的易出错调用
- ❌ L164: 未处理的易出错调用
- ❌ L186: 未处理的易出错调用
- ❌ L192: 未处理的易出错调用

**详情**:
- 循环复杂度: 平均: 6.3, 最大: 25
- 认知复杂度: 平均: 9.8, 最大: 31
- 嵌套深度: 平均: 1.8, 最大: 3
- 函数长度: 平均: 32.9 行, 最大: 142 行
- 文件长度: 203 代码量 (289 总计)
- 参数数量: 平均: 0.0, 最大: 0
- 代码重复: 0.0% 重复 (0/8)
- 结构分析: 1 个结构问题
- 错误处理: 11/11 个错误被忽略 (100.0%)
- 注释比例: 18.7% (38/203)
- 命名规范: 无命名违规

### 19. runtime\gc\gc.cpp

**糟糕指数: 18.91**

> 行数: 167 总计, 99 代码, 48 注释 | 函数: 12 | 类: 0

**问题**: 🔄 复杂度问题: 3, 🏗️ 结构问题: 1, 📝 注释问题: 1, 🏷️ 命名问题: 3

#### 函数详情

| 函数 | 行范围 | 行数 | 复杂度 | 嵌套 | 参数 | 注释 |
|:-----|------:|------:|------:|------:|------:|:------:|
| `parseGcLogEnv` | L100-128 | 29 | 14 | 5 | 0 | ✗ |
| `parseGcLogLevel` | L90-98 | 9 | 7 | 1 | 1 | ✗ |
| `parallelFor` | L52-73 | 22 | 6 | 2 | 3 | ✓ |
| `GcHeap` | L130-144 | 15 | 3 | 2 | 0 | ✗ |
| `notifyIdleWakeups` | L42-45 | 4 | 2 | 1 | 0 | ✗ |
| `trimWs` | L83-88 | 6 | 2 | 1 | 1 | ✓ |
| `GcCompactSuspendGuard` | L27-29 | 3 | 1 | 0 | 0 | ✓ |
| `~GcCompactSuspendGuard` | L30-32 | 3 | 1 | 0 | 0 | ✗ |
| `registerIdleWakeup` | L37-40 | 4 | 1 | 0 | 1 | ✓ |
| `& GcHeap::instance()` | L146-148 | 3 | 1 | 0 | 0 | ✗ |
| `noteCoroutineFrameImpl` | L152-154 | 3 | 1 | 0 | 2 | ✓ |
| `~GcHeap` | L156-164 | 9 | 1 | 0 | 0 | ✗ |

**全部问题 (7)**

- 🔄 `parseGcLogEnv()` L100: 复杂度: 14
- 🔄 `parseGcLogEnv()` L100: 认知复杂度: 24
- 🔄 `parseGcLogEnv()` L100: 嵌套深度: 5
- 🏗️ `parseGcLogEnv()` L100: 嵌套过深: 5
- 🏷️ `~GcCompactSuspendGuard()` L30: "~GcCompactSuspendGuard" - camelCase/snake_case/PascalCase
- 🏷️ `& GcHeap::instance()()` L146: "& GcHeap::instance()" - camelCase/snake_case/PascalCase
- 🏷️ `~GcHeap()` L156: "~GcHeap" - camelCase/snake_case/PascalCase

**详情**:
- 循环复杂度: 平均: 3.3, 最大: 14
- 认知复杂度: 平均: 5.3, 最大: 24
- 嵌套深度: 平均: 1.0, 最大: 5
- 函数长度: 平均: 9.2 行, 最大: 29 行
- 文件长度: 99 代码量 (167 总计)
- 参数数量: 平均: 0.7, 最大: 3
- 代码重复: 0.0% 重复 (0/12)
- 结构分析: 1 个结构问题
- 错误处理: 未检测到易出错调用
- 注释比例: 48.5% (48/99)
- 命名规范: 发现 3 个违规

### 20. src\Module\ModuleManager.cpp

**糟糕指数: 18.90**

> 行数: 393 总计, 265 代码, 74 注释 | 函数: 15 | 类: 1

**问题**: 🔄 复杂度问题: 6, 🏗️ 结构问题: 5, 📝 注释问题: 1

#### 函数详情

| 函数 | 行范围 | 行数 | 复杂度 | 嵌套 | 参数 | 注释 |
|:-----|------:|------:|------:|------:|------:|:------:|
| `topologicalLayers` | L306-363 | 58 | 15 | 5 | 0 | ✓ |
| `parseModule` | L110-177 | 68 | 12 | 4 | 1 | ✓ |
| `hasCycle` | L275-301 | 27 | 9 | 3 | 0 | ✓ |
| `sanitizeId` | L54-76 | 23 | 6 | 3 | 1 | ✓ |
| `loadAll` | L237-270 | 34 | 6 | 3 | 1 | ✓ |
| `resolveImportPath` | L182-201 | 20 | 5 | 1 | 2 | ✓ |
| `validateEntry` | L368-390 | 23 | 5 | 2 | 1 | ✓ |
| `loadAuraiFile` | L208-225 | 18 | 4 | 1 | 1 | ✓ |
| `readFile` | L29-35 | 7 | 2 | 1 | 1 | ✓ |
| `writeFile` | L37-44 | 8 | 2 | 1 | 2 | ✗ |
| `ModuleManager` | L24-24 | 1 | 1 | 0 | 1 | ✓ |
| `stemOf` | L46-49 | 4 | 1 | 0 | 1 | ✗ |
| `pathToNs` | L78-92 | 15 | 1 | 0 | 1 | ✗ |
| `isKnownBuiltin` | L97-105 | 9 | 1 | 0 | 1 | ✓ |
| `loadBuiltinAurai` | L227-232 | 6 | 1 | 0 | 0 | ✗ |

**全部问题 (11)**

- 🔄 `parseModule()` L110: 复杂度: 12
- 🔄 `topologicalLayers()` L306: 复杂度: 15
- 🔄 `parseModule()` L110: 认知复杂度: 20
- 🔄 `topologicalLayers()` L306: 认知复杂度: 25
- 🔄 `parseModule()` L110: 嵌套深度: 4
- 🔄 `topologicalLayers()` L306: 嵌套深度: 5
- 🏗️ `sanitizeId()` L54: 中等嵌套: 3
- 🏗️ `parseModule()` L110: 中等嵌套: 4
- 🏗️ `loadAll()` L237: 中等嵌套: 3
- 🏗️ `hasCycle()` L275: 中等嵌套: 3
- 🏗️ `topologicalLayers()` L306: 嵌套过深: 5

**详情**:
- 循环复杂度: 平均: 4.7, 最大: 15
- 认知复杂度: 平均: 7.9, 最大: 25
- 嵌套深度: 平均: 1.6, 最大: 5
- 函数长度: 平均: 21.4 行, 最大: 68 行
- 文件长度: 265 代码量 (393 总计)
- 参数数量: 平均: 0.9, 最大: 2
- 代码重复: 0.0% 重复 (0/15)
- 结构分析: 5 个结构问题
- 错误处理: 0/8 个错误被忽略 (0.0%)
- 注释比例: 27.9% (74/265)
- 命名规范: 无命名违规

## 最差函数 Top 10

| 函数 | 文件 | 复杂度 | 嵌套 | 行数 |
|:-----|:-----|------:|------:|------:|
| `genMethodCall` | src\CodeGen\ExprGen.cpp | 86 | 6 | 299 |
| `genLetStmt` | src\CodeGen\StmtGen.cpp | 80 | 8 | 345 |
| `genFunExpr` | src\CodeGen\ExprGen.cpp | 78 | 5 | 303 |
| `inferMethodCall` | src\Sema\Checker\ExprInfer.cpp | 69 | 6 | 234 |
| `genMatchStmt` | src\CodeGen\StmtGen.cpp | 65 | 8 | 263 |
| `genCallExpr` | src\CodeGen\ExprGen.cpp | 62 | 6 | 229 |
| `mapType` | src\CodeGen\TypeMap.cpp | 58 | 9 | 159 |
| `generate` | src\CodeGen\CodeGen.cpp | 52 | 4 | 184 |
| `compileMultiFile` | src\main.cpp | 51 | 5 | 266 |
| `isAssignable` | src\Sema\SemAnalyzer.cpp | 44 | 5 | 154 |

## 诊断结论 {#conclusion}

🌸 **微臭青年** - 略有异味，建议适量通风

👍 继续保持，你是编码界的一股清流，代码洁癖者的骄傲

---

*由 [fuck-u-code](https://github.com/Done-0/fuck-u-code) 生成*