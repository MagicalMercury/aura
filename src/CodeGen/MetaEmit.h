// src/CodeGen/MetaEmit.h
#pragma once
#include <string>
namespace Aura { class MetaMerger; }
// 🔴 O28 修订（终审）：三个 emit 函数的入参已**全部改收 `const MetaMerger&`**（O1 配套）⇒ 原前置声明
//   缺 `MetaMerger` **编不过**；且旧的三声明（`MetaCollector` / `CompileUnit` / `SemType`）在 O1 后
//   **已无消费者** ⇒ 一并清理。

namespace Aura::MetaEmit {

// (A) 单文件模式：内嵌进生成的 .cpp（返回「表定义段」文本，由 CodeGen 拼到 unit.metaImpl）
//     ⚠️ O13 修订：表定义须在 **`namespace aura_rt::meta` 内、模块命名空间之外** ——
//        meta.h 的 extern 声明在 `aura_rt::meta` 里（不是「全局的」）；定义与声明**必须同命名空间**，否则 undefined reference。
std::string emitTablesInline(const MetaMerger& merger);

// (B) 多文件模式：aura.meta.h（extern 声明，**不含定义** —— 红线①）
//     🔴 O1：入参为 MetaMerger（主线程合并后的产物；MetaCollector 是线程私有的）
std::string emitMetaHeader(const MetaMerger& merger);

// (C) 多文件模式：aura.meta.cpp（**唯一定义** + thunk 的前置声明段 —— 见 §3.3 M2-(c)）
std::string emitMetaImpl(const MetaMerger& merger);

// (D) thunk 签名文本（**必须由 CodeGenerator 在 generate() 内生成** —— 见 §3.5-(d)；
//     本函数只提供「thunk 的 C++ 签名文本」供 CodeGen 拼体内）
// 🔴 N1 修订：收 thunkName（**不再收 index**）—— 名字在 collectSymbol 时已定型（N2 单源化）
std::string thunkSignature(const std::string& nsName, const std::string& thunkName);

} // namespace Aura::MetaEmit
