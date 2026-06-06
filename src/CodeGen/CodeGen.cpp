#include "CodeGen.h"
#include <sstream>

namespace Aura {

// ============================================================
// 构造 & 主入口
// ============================================================

CodeGenerator::CodeGenerator() {
    // 注册内置值类型
    registeredTypes_["int"]    = false;
    registeredTypes_["float"]  = false;
    registeredTypes_["bool"]   = false;
    registeredTypes_["string"] = true; // GcString* 是堆指针
    registeredTypes_["Io"]     = false; // value type — qualified as aura_rt::Io
    registeredTypes_["Path"]   = false; // value type — qualified as aura_rt::Path
}

CompileUnit CodeGenerator::generate(const Program& program,
                                     const std::string& moduleName) {
    CompileUnit unit;
    unit.moduleName = moduleName;

    std::ostringstream header, impl;
    headerStream_ = &header;
    implStream_   = &impl;

    // 公共头
    header << "#include \"aura_rt.h\"\n\n";

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
        }
    }

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

    for (auto& d : program.decls) {
        if (!d) continue;
        genDecl(header, impl, *d, unit);
    }

    // 检测 main 函数并生成入口
    for (auto& d : program.decls) {
        if (auto* f = dynamic_cast<const FunDecl*>(d.get())) {
            if (f->name == "main") {
                unit.hasMain = true;
                genMainEntry(impl, *f);
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
                             const Decl& decl, CompileUnit& /*unit*/) {
    if (auto* t = dynamic_cast<const TypeDecl*>(&decl))
        { genTypeDecl(h, cpp, *t); return; }
    if (auto* i = dynamic_cast<const InterfaceDecl*>(&decl))
        { genInterfaceDecl(h, *i); return; }
    if (auto* f = dynamic_cast<const FunDecl*>(&decl))
        { genFunDecl(h, cpp, *f); return; }
    if (auto* m = dynamic_cast<const MethodDecl*>(&decl))
        { genMethodDecl(h, cpp, *m); return; }
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
    // C++ 关键字冲突保护
    if (name == "class" || name == "template" || name == "typename" ||
        name == "auto" || name == "const" || name == "new" ||
        name == "delete" || name == "virtual" || name == "override")
        return name + "_";
    return name;
}

// ============================================================
// 错误
// ============================================================

void CodeGenerator::error(const ASTNode& node, const std::string& msg) {
    errors_.push_back("[line " + std::to_string(node.line) + ":" +
                      std::to_string(node.col) + "] codegen: " + msg);
}

} // namespace Aura
