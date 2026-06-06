#include "CodeGen.h"
#include <sstream>

namespace Aura {

// ============================================================
// 记录类型声明（plan §4.2）
// ============================================================

void CodeGenerator::genTypeDecl(std::ostream& h, std::ostream& cpp,
                                 const TypeDecl& decl) {
    if (!decl.type) return;
    if (auto* rec = dynamic_cast<const RecordType*>(decl.type.get())) {
        std::vector<std::string> tparams;
        for (auto& f : rec->fields) {
            if (f.type && dynamic_cast<const GenericTypeRef*>(f.type.get())) {
                tparams.push_back(dynamic_cast<const GenericTypeRef*>(f.type.get())->name);
            }
        }
        genRecordStruct(h, cpp, decl.name, *rec, tparams);
    }
}

void CodeGenerator::genRecordStruct(std::ostream& h, std::ostream& cpp,
                                     const std::string& name,
                                     const RecordType& body,
                                     const std::vector<std::string>& tparams) {
    // 模板前缀
    std::string tprefix;
    if (!tparams.empty()) {
        tprefix = "template<";
        for (size_t i = 0; i < tparams.size(); ++i) {
            if (i > 0) tprefix += ", ";
            tprefix += "typename " + tparams[i];
        }
        tprefix += ">\n";
    }

    h << tprefix << "struct " << name << " : aura_rt::GcObject {\n";

    std::vector<std::string> ptrFields;

    for (auto& f : body.fields) {
        std::string cppType = f.type ? mapType(*f.type) : "???";
        h << "  " << cppType << " " << safeName(f.name) << ";\n";
        if (auto* nt = dynamic_cast<const NamedType*>(f.type.get())) {
            auto it = registeredTypes_.find(nt->name);
            if (it != registeredTypes_.end() && it->second)
                ptrFields.push_back(safeName(f.name));
        }
    }

    // 收集此类型的所有方法声明并嵌入 struct 内部
    for (auto& md : pendingMethods_) {
        if (md.receiverType == name) {
            h << "  " << md.returnTypeStr << " " << safeName(md.methodName) << "(";
            for (size_t i = 0; i < md.paramTypes.size(); ++i) {
                if (i > 0) h << ", ";
                h << md.paramTypes[i] << " " << safeName(md.paramNames[i]);
            }
            h << ");\n";
        }
    }

    h << "\n  static const aura_rt::TypeDescriptor _desc;\n";
    h << "};\n\n";

    genTypeDescriptor(cpp, name, tparams, ptrFields);
}

// ============================================================
// 接口声明（plan §4.5）
// ============================================================

void CodeGenerator::genInterfaceDecl(std::ostream& h,
                                      const InterfaceDecl& decl) {
    h << "// interface " << decl.name << " — ";
    for (size_t i = 0; i < decl.methods.size(); ++i) {
        if (i > 0) h << ", ";
        h << decl.methods[i].name << "()";
    }
    h << '\n';
    h << "// (full type-erasure wrapper TBD)\n\n";
    (void)decl;
}

// ============================================================
// 函数声明 + 实现（plan §4.6, §4.8）
// ============================================================

void CodeGenerator::genFunDecl(std::ostream& h, std::ostream& cpp,
                                const FunDecl& decl) {
    bool isCoro = coroutineFunctions_.count(decl.name);
    currentFunctionIsCoroutine_ = isCoro;

    std::vector<std::string> tparams;
    for (auto& p : decl.params) {
        if (p.type && dynamic_cast<const GenericTypeRef*>(p.type.get()))
            tparams.push_back(dynamic_cast<const GenericTypeRef*>(p.type.get())->name);
    }

    std::string tprefix;
    if (!tparams.empty()) {
        tprefix = "template<";
        for (size_t i = 0; i < tparams.size(); ++i) {
            if (i > 0) tprefix += ", ";
            tprefix += "typename " + tparams[i];
        }
        tprefix += ">\n";
    }

    std::string sig = funSignature(decl, tparams);
    h << tprefix << sig << ";\n";

    valueTypeVarNames_.clear();
    for (auto& p : decl.params) {
        if (p.type && dynamic_cast<const NamedType*>(p.type.get())) {
            auto* nt = dynamic_cast<const NamedType*>(p.type.get());
            if (registeredTypes_.count(nt->name) && !registeredTypes_[nt->name])
                valueTypeVarNames_.insert(p.name);
        }
    }

    cpp << tprefix << sig << " {\n";
    if (decl.body) genBlock(cpp, *decl.body, isCoro);
    cpp << "}\n\n";
    valueTypeVarNames_.clear();
}

std::string CodeGenerator::funSignature(const FunDecl& decl,
                                         const std::vector<std::string>& tparams) {
    bool isCoro = coroutineFunctions_.count(decl.name);
    std::ostringstream sig;

    std::string retType = decl.returnType ? mapType(*decl.returnType) : "void";
    if (!tparams.empty() && decl.returnType) {
        if (auto* nt = dynamic_cast<const NamedType*>(decl.returnType.get())) {
            if (isHeapType(nt->name)) {
                retType = nt->name + "<";
                for (size_t i = 0; i < tparams.size(); ++i) {
                    if (i > 0) retType += ", ";
                    retType += tparams[i];
                }
                retType += ">*";
            }
        }
    }

    sig << (isCoro ? "aura_rt::task<" + retType + ">" : retType);
    std::string fn = safeName(decl.name);
    if (fn == "main") fn = "aura_main";
    sig << " " << fn << "(";
    for (size_t i = 0; i < decl.params.size(); ++i) {
        if (i > 0) sig << ", ";
        sig << (decl.params[i].type ? mapType(*decl.params[i].type) : "auto")
            << " " << safeName(decl.params[i].name);
    }
    sig << ")";
    return sig.str();
}

void CodeGenerator::genMethodDecl(std::ostream& /*h*/, std::ostream& cpp,
                                   const MethodDecl& decl) {
    if (decl.isConstructor) {
        genConstructor(cpp, decl);
        return;
    }

    bool isCoro = coroutineFunctions_.count(decl.name);
    currentFunctionIsCoroutine_ = isCoro;

    valueTypeVarNames_.clear();
    // receiverName (self) is a pointer, not a value type

    std::string retType = decl.returnType ? mapType(*decl.returnType) : "void";
    std::string sig = retType + " " + decl.receiverType + "::" + safeName(decl.name) + "(";
    for (size_t i = 0; i < decl.params.size(); ++i) {
        if (i > 0) sig += ", ";
        sig += (decl.params[i].type ? mapType(*decl.params[i].type) : "auto")
             + " " + safeName(decl.params[i].name);
    }
    sig += ")";

    cpp << sig << " {\n";
    currentReceiverName_ = decl.receiverName;
    if (decl.body) genBlock(cpp, *decl.body, isCoro);
    currentReceiverName_.clear();
    cpp << "}\n\n";
    valueTypeVarNames_.clear();
}

// ============================================================
// 构造函数生成（plan §4.3）
// ============================================================

void CodeGenerator::genConstructor(std::ostream& cpp, const MethodDecl& decl) {
    // 提取泛型类型参数（从参数类型中检测 GenericTypeRef）
    std::vector<std::string> tparams;
    for (auto& p : decl.params) {
        if (p.type && dynamic_cast<const GenericTypeRef*>(p.type.get()))
            tparams.push_back(dynamic_cast<const GenericTypeRef*>(p.type.get())->name);
    }

    // 构建模板前缀
    std::string tprefix;
    if (!tparams.empty()) {
        tprefix = "template<";
        for (size_t i = 0; i < tparams.size(); ++i) {
            if (i > 0) tprefix += ", ";
            tprefix += "typename " + tparams[i];
        }
        tprefix += ">\n";
    }

    // 构建带模板参数的完整类型名（如 Pair<A, B>）
    std::string fullType = decl.receiverType;
    if (!tparams.empty()) {
        fullType += "<";
        for (size_t i = 0; i < tparams.size(); ++i) {
            if (i > 0) fullType += ", ";
            fullType += tparams[i];
        }
        fullType += ">";
    }

    std::string sig = constructorSignature(decl, tparams);
    cpp << tprefix << sig << " {\n";
    cpp << "  " << fullType << "* " << safeName(decl.receiverName) << " = aura_rt::gc_alloc<"
        << fullType << ">(&" << fullType << "::_desc);\n";
    // 构造函数翻译为自由函数，不设置 currentReceiverName_（不是 C++ 成员函数）
    if (decl.body) genBlock(cpp, *decl.body, false);
    cpp << "  return " << safeName(decl.receiverName) << ";\n";
    cpp << "}\n\n";
}

std::string CodeGenerator::constructorSignature(const MethodDecl& decl,
                                                  const std::vector<std::string>& tparams) {
    std::ostringstream sig;
    std::string fullType = decl.receiverType;
    if (!tparams.empty()) {
        fullType += "<";
        for (size_t i = 0; i < tparams.size(); ++i) {
            if (i > 0) fullType += ", ";
            fullType += tparams[i];
        }
        fullType += ">";
    }
    sig << fullType << "* " << decl.receiverType << "_ctor(";
    for (size_t i = 0; i < decl.params.size(); ++i) {
        if (i > 0) sig << ", ";
        sig << (decl.params[i].type ? mapType(*decl.params[i].type) : "auto")
            << " " << safeName(decl.params[i].name);
    }
    sig << ")";
    return sig.str();
}

// ============================================================
// 主入口（plan §5）
// ============================================================

void CodeGenerator::genMainEntry(std::ostream& cpp, const FunDecl& mainDecl) {
    cpp << "\n// ============================================================\n";
    cpp << "// Aura 程序入口\n";
    cpp << "// ============================================================\n";
    cpp << "int main(int /*argc*/, char** /*argv*/) {\n";
    cpp << "  aura_rt::Io io;\n";
    cpp << "  auto t = ::aura_main(io);\n";
    cpp << "  aura_rt::run_event_loop(t);\n";
    cpp << "  return 0;\n";
    cpp << "}\n";
    (void)mainDecl;
}

} // namespace Aura
