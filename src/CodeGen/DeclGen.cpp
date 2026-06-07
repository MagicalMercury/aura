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
        // 优先使用 TypeDecl 显式声明的泛型参数，否则从字段中扫描 GenericTypeRef
        std::vector<std::string> tparams = decl.typeParams;
        if (tparams.empty()) {
            for (auto& f : rec->fields) {
                if (f.type && dynamic_cast<const GenericTypeRef*>(f.type.get())) {
                    tparams.push_back(dynamic_cast<const GenericTypeRef*>(f.type.get())->name);
                }
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

        // 检测 GC 指针字段：任何映射后以 * 结尾的 C++ 类型都是堆对象指针
        // 包括 NamedType（如 User*）、ListType（如 Array<T>*）、GcString* 等
        if (!cppType.empty() && cppType.back() == '*') {
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

    // 模板类型的 _desc 必须在头文件中实例化（跨模块链接需要）
    genTypeDescriptor(tparams.empty() ? cpp : h, name, tparams, ptrFields);
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

    std::vector<std::string> tparams = collectFunTParams(decl);
    currentTParams_ = tparams;

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
    stringVarNames_.clear();
    for (auto& p : decl.params) {
        if (p.type && dynamic_cast<const NamedType*>(p.type.get())) {
            auto* nt = dynamic_cast<const NamedType*>(p.type.get());
            if (registeredTypes_.count(nt->name) && !registeredTypes_[nt->name])
                valueTypeVarNames_.insert(p.name);
        }
    }

    // 模板函数：体放入头文件（跨模块可见）
    std::ostream& out = tparams.empty()
        ? static_cast<std::ostream&>(cpp)
        : static_cast<std::ostream&>(h);

    out << tprefix << sig << " {\n";
    if (decl.body) genBlock(out, *decl.body, isCoro);
    out << "}\n\n";
    valueTypeVarNames_.clear();
    stringVarNames_.clear();
}

std::string CodeGenerator::funSignature(const FunDecl& decl,
                                         const std::vector<std::string>& tparams) {
    bool isCoro = coroutineFunctions_.count(decl.name);
    std::ostringstream sig;

    std::string retType = decl.returnType ? mapType(*decl.returnType) : "void";
    (void)tparams; // mapType 已正确解析 typeArgs 顺序（含泛型参数名）

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

void CodeGenerator::genMethodDecl(std::ostream& h, std::ostream& cpp,
                                   const MethodDecl& decl) {
    if (decl.isConstructor) {
        genConstructor(cpp, decl);
        return;
    }

    bool isCoro = coroutineFunctions_.count(decl.name);
    currentFunctionIsCoroutine_ = isCoro;

    std::vector<std::string> tparams = collectMethodTParams(decl);
    currentTParams_ = tparams;

    std::string tprefix;
    if (!tparams.empty()) {
        tprefix = "template<";
        for (size_t i = 0; i < tparams.size(); ++i) {
            if (i > 0) tprefix += ", ";
            tprefix += "typename " + tparams[i];
        }
        tprefix += ">\n";
    }

    // 构建带模板参数的接收者类型名
    std::string recvFullType = decl.receiverType;
    if (!tparams.empty()) {
        recvFullType += "<";
        for (size_t i = 0; i < tparams.size(); ++i) {
            if (i > 0) recvFullType += ", ";
            recvFullType += tparams[i];
        }
        recvFullType += ">";
    }

    valueTypeVarNames_.clear();
    stringVarNames_.clear();

    std::string retType = decl.returnType ? mapType(*decl.returnType) : "void";
    std::string sig = retType + " " + recvFullType + "::" + safeName(decl.name) + "(";
    for (size_t i = 0; i < decl.params.size(); ++i) {
        if (i > 0) sig += ", ";
        sig += (decl.params[i].type ? mapType(*decl.params[i].type) : "auto")
             + " " + safeName(decl.params[i].name);
    }
    sig += ")";

    // 模板方法：体放入头文件（跨模块可见）
    std::ostream& out = tparams.empty()
        ? static_cast<std::ostream&>(cpp)
        : static_cast<std::ostream&>(h);

    out << tprefix << sig << " {\n";
    currentReceiverName_ = decl.receiverName;
    if (decl.body) genBlock(out, *decl.body, isCoro);
    currentReceiverName_.clear();
    out << "}\n\n";
    valueTypeVarNames_.clear();
    stringVarNames_.clear();
}

// ============================================================
// 构造函数生成（plan §4.3）
// ============================================================

void CodeGenerator::genConstructor(std::ostream& cpp, const MethodDecl& decl) {
    // 提取泛型类型参数（统一用 collectMethodTParams）
    std::vector<std::string> tparams = collectMethodTParams(decl);
    currentTParams_ = tparams;

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

    // 模板构造函数：体放入头文件（跨模块可见）
    std::ostream& out = tparams.empty()
        ? static_cast<std::ostream&>(cpp)
        : *headerStream_;

    out << tprefix << sig << " {\n";
    out << "  " << fullType << "* " << safeName(decl.receiverName) << " = aura_rt::gc_alloc<"
        << fullType << ">(&" << fullType << "::_desc);\n";
    if (decl.body) genBlock(out, *decl.body, false);
    out << "  return " << safeName(decl.receiverName) << ";\n";
    out << "}\n\n";
    valueTypeVarNames_.clear();
    stringVarNames_.clear();
    currentTParams_.clear();
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

void CodeGenerator::genMainEntry(std::ostream& cpp, const FunDecl& mainDecl,
                                    const std::string& nsName) {
    cpp << "\n// ============================================================\n";
    cpp << "// Aura 程序入口\n";
    cpp << "// ============================================================\n";
    cpp << "int main(int /*argc*/, char** /*argv*/) {\n";
    cpp << "  aura_rt::Io io;\n";
    if (nsName.empty()) {
        cpp << "  auto t = ::aura_main(io);\n";
    } else {
        cpp << "  auto t = " << nsName << "::aura_main(io);\n";
    }
    cpp << "  aura_rt::run_event_loop(t);\n";
    cpp << "  return 0;\n";
    cpp << "}\n";
    (void)mainDecl;
}

// ============================================================
// 泛型模板参数收集
// ============================================================

void CodeGenerator::collectTParams(const TypeExpr& type, std::set<std::string>& out) const {
    if (auto* g = dynamic_cast<const GenericTypeRef*>(&type)) {
        out.insert(g->name);
        return;
    }
    if (auto* n = dynamic_cast<const NamedType*>(&type)) {
        for (auto& a : n->typeArgs)
            if (a) collectTParams(*a, out);
        // 无 typeArgs + 非注册类型 → 是泛型参数（如 Pair<A,B> 中的 A/B）
        if (n->typeArgs.empty() && !registeredTypes_.count(n->name)
            && !interfaceNames_.count(n->name))
            out.insert(n->name);
        return;
    }
    if (auto* l = dynamic_cast<const ListType*>(&type)) {
        if (l->elementType) collectTParams(*l->elementType, out);
        return;
    }
    if (auto* r = dynamic_cast<const RecordType*>(&type)) {
        for (auto& f : r->fields)
            if (f.type) collectTParams(*f.type, out);
        return;
    }
    if (auto* u = dynamic_cast<const UnionType*>(&type)) {
        for (auto& v : u->types)
            if (v) collectTParams(*v, out);
        return;
    }
    if (auto* fn = dynamic_cast<const FunctionType*>(&type)) {
        for (auto& p : fn->paramTypes)
            if (p) collectTParams(*p, out);
        if (fn->returnType) collectTParams(*fn->returnType, out);
        return;
    }
}

std::vector<std::string> CodeGenerator::collectFunTParams(const FunDecl& decl) const {
    std::set<std::string> names;
    for (auto& p : decl.params)
        if (p.type) collectTParams(*p.type, names);
    if (decl.returnType) collectTParams(*decl.returnType, names);
    return {names.begin(), names.end()};
}

std::vector<std::string> CodeGenerator::collectMethodTParams(const MethodDecl& decl) const {
    std::set<std::string> names;
    // 优先从 receiverTypeArgs（如 Stack<T> 中的 T）
    for (auto& ta : decl.receiverTypeArgs)
        names.insert(ta);
    // 从参数类型中收集
    for (auto& p : decl.params)
        if (p.type) collectTParams(*p.type, names);
    if (decl.returnType) collectTParams(*decl.returnType, names);
    return {names.begin(), names.end()};
}

} // namespace Aura
