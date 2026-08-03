#include "CodeGen.h"
#include "../Sema/SemAnalyzer.h"
#include <filesystem>
#include <sstream>
#include <unordered_set>

namespace Aura {

// ============================================================
// CodeGenConfig
// ============================================================

void CodeGenConfig::setConfig(const SemAnalyzer& sema) {
    ioSync = sema.getIoSync();
}

// ============================================================
// 构造 & 主入口
// ============================================================

CodeGenerator::CodeGenerator(DiagnosticEngine& diag) : diag_(diag) {
}

CompileUnit CodeGenerator::generate(const Program& program,
                                     const std::string& moduleName,
                                     const std::vector<CodeGenImport>& imports,
                                     const std::string& nsName,
                                     const CodeGenConfig& config,
                                     const CrossModuleDefaults& crossDefaults) {
    ioSync_ = config.ioSync;
    crossDefaults_ = crossDefaults;   // C5.4: 跨模块函数默认参数表
    CompileUnit unit;
    unit.moduleName = moduleName;
    unit.nsName     = nsName;

    std::ostringstream header, impl;
    headerStream_ = &header;
    implStream_   = &impl;

    // 公共头
    header << "#include \"aura_rt.h\"\n";

    // 生成 import 对应的 #include（头文件中包含依赖模块的 .h）
     for (auto& imp : imports) {
         if (imp.isBuiltin) {
             // 内置模块已通过 aura_rt.h 引入，此处生成注释说明
             header << "// using builtin: " << imp.path << "\n";
         } else {
            // 用户模块：相对路径 #include
            std::string depStem = std::filesystem::path(imp.path).stem().string();
            header << "#include \"" << depStem << ".aura.h\"\n";
        }
    }
    header << "\n";

    // 生成翻译单元级别的命名空间别名
     // 有 alias 时只生成别名，屏蔽原名
     for (auto& imp : imports) {
         bool hasAlias = !imp.alias.empty() && imp.alias != imp.modName;
         if (!hasAlias) {
             // 无别名：用模块名
             if (imp.isBuiltin) {
                 impl << "namespace " << imp.modName
                      << " = aura_rt::" << imp.path << ";\n";
             } else {
                 impl << "namespace " << imp.modName << " = " << imp.nsName << ";\n";
             }
             importNsNames_.insert(imp.modName);
         }
         if (!imp.alias.empty()) {
             // 有别名：生成别名（别名 ≠ 原名时）
             impl << "namespace " << imp.alias
                  << " = " << (imp.isBuiltin ? std::string("aura_rt::") + imp.path : imp.nsName) << ";\n";
             importNsNames_.insert(imp.alias);
         }
     }
     if (!imports.empty()) impl << "\n";

    // 打开命名空间（若有）— header 和 impl 都需要
    if (!nsName.empty()) {
        header << "namespace " << nsName << " {\n\n";
        impl << "namespace " << nsName << " {\n\n";
    }

    // 第一遍：注册所有类型名和接口名
    for (auto& d : program.decls) {
        if (!d) continue;
        if (auto* t = dynamic_cast<const TypeDecl*>(d.get()))
            registerTypeName(t->name, true);
        if (auto* i = dynamic_cast<const InterfaceDecl*>(d.get()))
            interfaceNames_.insert(i->name);
        if (auto* f = dynamic_cast<const FunDecl*>(d.get()))
            registerTypeName(f->name, false);
        if (auto* m = dynamic_cast<const MethodDecl*>(d.get())) {
            if (m->isConstructor)
                registerTypeName(m->receiverType, true);
            // 适配器默认方法转发判定数据：record 非构造方法名集合
            if (!m->isConstructor && !m->receiverType.empty())
                recordMethods_[m->receiverType].insert(m->name);
            // 显式 impl 组合收集：record 方法声明 → (receiverType, 接口名 → 类型实参)
            // 接口实现必须显式 impl（无结构匹配）；只收集非泛型 record
            // （receiverTypeArgs 为空）——泛型 record 的 C++ 类型名是 "Stack<T>"（含模板参数），
            // 适配器类型名无法对应。v1 泛型 record 接接口 → Sema 报错
            if (!m->implInterface.empty() && m->receiverTypeArgs.empty()) {
                std::vector<std::string> argCpp;
                for (auto& ta : m->implTypeArgs)
                    argCpp.push_back(ta ? mapType(*ta) : "???");
                interfaceImplementations_[m->receiverType][m->implInterface] = std::move(argCpp);
            }
        }
    }

    // 适配器生成需遍历的接口集合 = 用户接口 + 内置接口（interfaces.aurai：Stringer/Comparable/Iterator）
    std::vector<const InterfaceDecl*> allIfaces;
    for (auto& d : program.decls) {
        if (auto* i = dynamic_cast<const InterfaceDecl*>(d.get()))
            allIfaces.push_back(i);
    }
    for (auto& i : BuiltinRegistry::get().auraiInterfaces())
        allIfaces.push_back(i.get());

    // 第二遍：协程判定（提前一轮，函数体内需要知道自己的协程状态）
    for (auto& d : program.decls) {
        if (!d) continue;
        if (auto* f = dynamic_cast<const FunDecl*>(d.get())) {
            if (decideCoro(*f) == CoroDecision::Coroutine)
                coroutineFunctions_.insert(f->name);
        }
        if (auto* m = dynamic_cast<const MethodDecl*>(d.get())) {
            if (decideCoro(*m) == CoroDecision::Coroutine)
                coroutineFunctions_.insert(m->name);
        }
    }

    // 第三遍：生成代码
    // 先收集方法信息（供 genRecordStruct 嵌入声明）
    pendingMethods_.clear();
    for (auto& d : program.decls) {
        if (auto* m = dynamic_cast<const MethodDecl*>(d.get())) {
            if (!m->isConstructor) {
                PendingMethod pm;
                pm.receiverType  = m->receiverType;
                pm.methodName    = m->name;
                pm.returnTypeStr = m->returnType ? mapType(*m->returnType) : "void";
                for (auto& p : m->params) {
                    pm.paramTypes.push_back(p.type ? mapType(*p.type) : "auto");
                    pm.paramNames.push_back(p.name);
                }
                pendingMethods_.push_back(std::move(pm));
            }
        }
    }

    // 第三遍 A：先生成所有声明（避免前向引用问题）
    for (auto& d : program.decls) {
        if (!d) continue;
        if (diag_.errorCount() > 10) break;  // 错误过多，停止生成
        genDecl(header, impl, *d, unit, true);
    }

    // 接口收尾（第三遍 A 之后）：内置接口基类（interfaces.aurai）不在 program.decls 中，
    // 统一在此生成；随后为所有 record × 接口组合生成适配器——
    // 此时所有 record struct 已完整定义，适配器内联方法体可安全解引用 record 方法
    for (auto& i : BuiltinRegistry::get().auraiInterfaces())
        genInterfaceDecl(header, *i);
    for (auto* i : allIfaces) {
        for (auto& [rec, ifaces] : interfaceImplementations_) {
            if (!ifaces.count(i->name)) continue;
            std::string key = rec + i->name;
            if (ifaceAdapterCache_.count(key)) continue;
            ifaceAdapterCache_.insert(key);
            genIfaceAdapter(header, rec, *i);
        }
    }

    // 第三遍 B：再生成所有定义
    if (diag_.errorCount() <= 10) {
        for (auto& d : program.decls) {
            if (!d) continue;
            if (diag_.errorCount() > 10) break;
            genDecl(header, impl, *d, unit, false);
        }
    }

    // 关闭命名空间（若有）
    if (!nsName.empty()) {
        header << "} // namespace " << nsName << "\n";
        impl << "} // namespace " << nsName << "\n";
    }

    // 检测 main 函数并生成入口（必须在命名空间之外）
    for (auto& d : program.decls) {
        if (auto* f = dynamic_cast<const FunDecl*>(d.get())) {
            if (f->name == "main") {
                unit.hasMain = true;
                std::ostringstream footerStream;
                genMainEntry(footerStream, *f, nsName);
                unit.footer = footerStream.str();
                break;
            }
        }
    }

    unit.header = header.str();
    unit.impl   = impl.str();
    return unit;
}

// ============================================================
// 调度
// ============================================================

void CodeGenerator::genDecl(std::ostream& h, std::ostream& cpp,
                             const Decl& decl, CompileUnit& /*unit*/,
                             bool declarationsOnly) {
    if (auto* t = dynamic_cast<const TypeDecl*>(&decl)) {
        // 类型声明在 A 遍生成（B 遍跳过，避免重复）
        if (declarationsOnly)
            { genTypeDecl(h, cpp, *t); return; }
        return;
    }
    if (auto* i = dynamic_cast<const InterfaceDecl*>(&decl)) {
        // 接口声明在 A 遍生成；适配器在第三遍 A 之后统一生成
        // （需 record struct 已完整定义，见 genDecl 后的 iface 收尾循环）
        if (declarationsOnly)
            { genInterfaceDecl(h, *i); return; }
        return;
    }
    if (auto* f = dynamic_cast<const FunDecl*>(&decl)) {
        genFunDecl(h, cpp, *f, declarationsOnly);
        return;
    }
    if (auto* m = dynamic_cast<const MethodDecl*>(&decl)) {
        genMethodDecl(h, cpp, *m, declarationsOnly);
        return;
    }
}

// ============================================================
// 输出辅助
// ============================================================

void CodeGenerator::newline(std::ostream& os) { os << '\n'; }

void CodeGenerator::indent(std::ostream& os) {
    for (int i = 0; i < indentLevel_; ++i) os << "    ";
}

void CodeGenerator::dedent(std::ostream&) { /* no-op */ }

void CodeGenerator::writeLine(std::ostream& os, const std::string& line) {
    indent(os);
    os << line << '\n';
}

std::string CodeGenerator::indentStr() const {
    return std::string(static_cast<size_t>(indentLevel_) * 4, ' ');
}

std::string CodeGenerator::safeName(const std::string& name) const {
    // C++ 关键字/保留字冲突保护 — 编译器遇到冲突变量名时加后缀 _
    static const std::unordered_set<std::string> cppKeywords = {
        "class", "template", "typename", "auto", "const", "new",
        "delete", "virtual", "override", "default", "static", "enum",
        "double", "float", "int", "bool", "char", "short", "long",
        "void", "switch", "case", "break", "continue", "return",
        "if", "else", "for", "while", "do", "goto", "try", "catch",
        "throw", "namespace", "using", "public", "private", "protected",
        "struct", "union", "operator", "sizeof", "this", "true", "false",
        "nullptr", "noexcept", "mutable", "explicit", "export",
        "volatile", "register", "extern", "inline", "typedef",
        "friend", "constexpr", "consteval", "constinit", "decltype",
        "concept", "requires", "co_await", "co_return", "co_yield",
        "alignas", "alignof", "and", "and_eq", "bitand", "bitor",
        "compl", "not", "not_eq", "or", "or_eq", "xor", "xor_eq"
    };
    if (cppKeywords.count(name)) return name + "_";
    return name;
}

// ============================================================
// 错误
// ============================================================

void CodeGenerator::error(const ASTNode& node, const std::string& msg) {
    diag_.error(node, "codegen: " + msg);
}

} // namespace Aura
