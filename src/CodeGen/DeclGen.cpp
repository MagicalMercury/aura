#include "CodeGen.h"
#include "../Sema/BuiltinRegistry.h"
#include <sstream>

namespace Aura {

// ============================================================
// 记录类型声明（plan §4.2）
// ============================================================

void CodeGenerator::genTypeDecl(std::ostream& h, std::ostream& cpp,
                                 const TypeDecl& decl) {
    if (!decl.type) return;

    // 记录类型 → 生成 struct
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
        // 记录模板参数表（供后续 genLetStmt 等跳过模板类型的 canonicalName）
        if (!tparams.empty())
            typeAliasTemplateParams_[decl.name] = tparams;
        return;
    }

    // 其他类型（函数类型、联合类型、命名类型）→ 生成 C++ using 别名
    std::string mappedType = mapType(*decl.type);

    // 注册类型名（使后续代码生成知晓该类型的存在）
    registerTypeName(decl.name, false); // 函数/联合/命名类型默认非堆对象

    // 收集泛型参数：优先用 decl.typeParams，否则从类型内部扫描 GenericTypeRef
    std::vector<std::string> tparams = decl.typeParams;
    if (tparams.empty()) {
        std::set<std::string> tpSet;
        collectTParams(*decl.type, tpSet);
        tparams.assign(tpSet.begin(), tpSet.end());
    }

    // 记录类型别名的模板参数（供后续 funSignature 生成 Name<T,U> 形式的返回类型）
    if (!tparams.empty())
        typeAliasTemplateParams_[decl.name] = tparams;

    // 模板前缀
    if (!tparams.empty()) {
        h << "template<";
        for (size_t i = 0; i < tparams.size(); ++i) {
            if (i > 0) h << ", ";
            h << "typename " << tparams[i];
        }
        h << ">\n";
    }
    h << "using " << decl.name << " = " << mappedType << ";\n\n";
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
    std::set<std::string> fieldNames;

    for (auto& f : body.fields) {
        std::string cppType = f.type ? mapType(*f.type) : "???";
        h << "  " << cppType << " " << safeName(f.name) << ";\n";
        fieldNames.insert(f.name);

        // 检测 GC 指针字段：任何映射后以 * 结尾的 C++ 类型都是堆对象指针
        // 包括 NamedType（如 User*）、ListType（如 Array<T>*）、GcString* 等
        if (!cppType.empty() && cppType.back() == '*') {
            ptrFields.push_back(safeName(f.name));
        }
    }

    structFieldNames_[name] = fieldNames;

    // 收集此类型的所有方法声明并嵌入 struct 内部
    for (auto& md : pendingMethods_) {
        if (md.receiverType == name) {
            // 检测方法名与字段名冲突（C++ 不允许同名成员函数与成员变量）
            std::string cppMethodName = safeName(md.methodName);
            if (fieldNames.count(cppMethodName))
                cppMethodName += "_fun";
            h << "  " << md.returnTypeStr << " " << cppMethodName << "(";
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
// 接口声明 → 抽象基类 + std::function 包装器
// ============================================================

void CodeGenerator::genInterfaceDecl(std::ostream& h,
                                      const InterfaceDecl& decl) {
    std::string name = decl.name;

    // 1. 抽象基类
    h << "struct " << name << " {\n";
    h << "  virtual ~" << name << "() = default;\n";
    for (auto& m : decl.methods) {
        std::string retType = m.returnType ? mapType(*m.returnType) : "void";
        h << "  virtual " << retType << " " << m.name << "(";
        for (size_t i = 0; i < m.params.size(); ++i) {
            if (i > 0) h << ", ";
            h << (m.params[i].type ? mapType(*m.params[i].type) : "auto")
              << " " << safeName(m.params[i].name);
        }
        h << ") const = 0;\n";
    }
    h << "};\n\n";

    // 2. std::function 包装器（闭包适配器）
    if (decl.methods.size() == 1) {
        auto& m = decl.methods[0];
        std::string retType = m.returnType ? mapType(*m.returnType) : "void";
        std::string params;
        for (size_t i = 0; i < m.params.size(); ++i) {
            if (i > 0) params += ", ";
            params += m.params[i].type ? mapType(*m.params[i].type) : "auto";
        }
        h << "struct " << name << "Func : " << name << " {\n";
        h << "  std::function<" << retType << "(" << params << ")> func;\n";
        h << "  " << name << "Func(std::function<" << retType << "(" << params
          << ")> f) : func(std::move(f)) {}\n";
        h << "  " << retType << " " << m.name << "(";
        for (size_t i = 0; i < m.params.size(); ++i) {
            if (i > 0) h << ", ";
            h << (m.params[i].type ? mapType(*m.params[i].type) : "auto")
              << " " << safeName(m.params[i].name);
        }
        h << ") const override { return func(";
        for (size_t i = 0; i < m.params.size(); ++i) {
            if (i > 0) h << ", ";
            h << safeName(m.params[i].name);
        }
        h << "); }\n";
        h << "};\n\n";
    }
}

// ============================================================
// 函数声明 + 实现（plan §4.6, §4.8）
// ============================================================

void CodeGenerator::genFunDecl(std::ostream& h, std::ostream& cpp,
                                const FunDecl& decl, bool declarationsOnly) {
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

    // 注册回调类型参数（必须在 declarationsOnly return 之前，确保其他函数闭包体可见）
    fnInterfaceParams_.erase(decl.name);
    fnCallbackParams_.erase(decl.name);
    for (size_t i = 0; i < decl.params.size(); ++i) {
        if (auto* nt = decl.params[i].type
                ? dynamic_cast<const NamedType*>(decl.params[i].type.get())
                : nullptr) {
            if (interfaceNames_.count(nt->name)) {
                fnInterfaceParams_[decl.name].push_back({i, nt->name});
            }
        }
        // 仅当函数本身是模板时，才注册回调包装（需要模板参数 T, U 在作用域内）
        if (!tparams.empty() && decl.params[i].type
            && dynamic_cast<const FunctionType*>(decl.params[i].type.get())) {
            auto ft = dynamic_cast<const FunctionType*>(decl.params[i].type.get());
            fnCallbackParams_[decl.name].push_back({i, mapType(*ft)});
        }
    }

    // declarationsOnly 模式：仅输出前向声明
    if (declarationsOnly) {
        h << tprefix << sig << ";\n";
        return;
    }

    valueTypeVarNames_.clear();
    stringVarNames_.clear();
    for (auto& p : decl.params) {
        if (p.type && dynamic_cast<const NamedType*>(p.type.get())) {
            auto* nt = dynamic_cast<const NamedType*>(p.type.get());
            if ((registeredTypes_.count(nt->name) && !registeredTypes_[nt->name])
                || (BuiltinRegistry::get().findType(nt->name) != nullptr
                    && !BuiltinRegistry::get().isHeapType(nt->name)))
                valueTypeVarNames_.insert(p.name);
        }
        // 跟踪 string 类型参数 → lambda 捕获后 genBinaryExpr 用 concat
        if (p.type && mapType(*p.type).find("aura_rt::GcString*") != std::string::npos) {
            stringVarNames_.insert(p.name);
        }
        // 接口类型参数 → 值类型追踪（引用用 . 不是 ->）
        if (p.type) {
            if (auto* nt = dynamic_cast<const NamedType*>(p.type.get())) {
                if (interfaceNames_.count(nt->name))
                    valueTypeVarNames_.insert(p.name);
            }
        }
    }

    // 模板函数或 auto 返回（泛型闭包）→ 体放入头文件（跨模块可见）
    bool needsHeader = !tparams.empty();
    if (!needsHeader && decl.returnType) {
        // plan12: 泛型闭包返回 → auto → 需要 .h
        std::set<std::string> retGen;
        if (auto* ft = dynamic_cast<const FunctionType*>(decl.returnType.get())) {
            collectTParams(*ft, retGen);
            needsHeader = !retGen.empty();
        } else if (auto* nt = dynamic_cast<const NamedType*>(decl.returnType.get())) {
            needsHeader = typeAliasTemplateParams_.count(nt->name) > 0;
        }
    }
    std::ostream& out = needsHeader
        ? static_cast<std::ostream&>(h)
        : static_cast<std::ostream&>(cpp);

    out << tprefix << sig << " {\n";
    if (decl.body) genBlock(out, *decl.body, isCoro);
    // 协程函数末尾无 return 时补 co_return，确保 C++20 将其识别为协程
    if (isCoro) {
        bool lastIsReturn = decl.body && !decl.body->stmts.empty()
            && dynamic_cast<const ReturnStmt*>(decl.body->stmts.back().get());
        if (!lastIsReturn)
            out << "  co_return;\n";
    }
    out << "}\n\n";
    valueTypeVarNames_.clear();
    stringVarNames_.clear();
}

std::string CodeGenerator::funSignature(const FunDecl& decl,
                                         const std::vector<std::string>& tparams) {
    bool isCoro = coroutineFunctions_.count(decl.name);
    std::ostringstream sig;

    std::string retType = decl.returnType ? mapType(*decl.returnType) : "void";

    // plan12: 泛型闭包返回 → auto
    bool isGenClosureRet = tparams.empty() && decl.returnType;
    if (isGenClosureRet) {
        std::set<std::string> check;
        collectTParams(*decl.returnType, check);
        bool isGenericFT = (dynamic_cast<const FunctionType*>(decl.returnType.get()) && !check.empty());
        bool isAlias = false;
        if (auto* nt = dynamic_cast<const NamedType*>(decl.returnType.get()))
            isAlias = typeAliasTemplateParams_.count(nt->name) > 0;
        isGenClosureRet = isGenericFT || isAlias;
    }

    if (isGenClosureRet)
        retType = "auto";

    currentReturnCppType_ = retType;

    std::string fn = safeName(decl.name);
    // 所有协程：NoneType 返回 → void（task<void> 有 return_void()，task<NoneType> 没有）
    if (retType == "aura_rt::NoneType") retType = "void";
    if (fn == "main") fn = "aura_main";

    sig << (isCoro ? "aura_rt::task<" + retType + ">" : retType);
    sig << " " << fn << "(";
    for (size_t i = 0; i < decl.params.size(); ++i) {
        if (i > 0) sig << ", ";
        if (isGenClosureRet && decl.params[i].type) {
            std::set<std::string> pGen;
            collectTParams(*decl.params[i].type, pGen);
            if (!pGen.empty())
                sig << "auto";
            else
                sig << mapType(*decl.params[i].type);
        } else {
            sig << (decl.params[i].type ? mapParamType(*decl.params[i].type) : "auto");
        }
        sig << " " << safeName(decl.params[i].name);
    }
    sig << ")";
    return sig.str();
}

void CodeGenerator::genMethodDecl(std::ostream& h, std::ostream& cpp,
                                   const MethodDecl& decl,
                                   bool declarationsOnly) {
    if (decl.isConstructor) {
        if (!declarationsOnly)  // 构造函数体只在定义阶段生成
            genConstructor(cpp, decl);
        return;
    }
    if (declarationsOnly) return;  // 方法声明已在 struct 内部，无需重复

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

    // 跟踪方法接收者 self 的类型
    if (!decl.receiverTypeArgs.empty() || !registeredTypes_.count(decl.receiverType) || registeredTypes_[decl.receiverType])
        ; // self 通常为指针类型
    else
        valueTypeVarNames_.insert(decl.receiverName);

    for (auto& p : decl.params) {
        if (p.type) {
            auto mt = mapType(*p.type);
            if (mt.find("aura_rt::GcString*") != std::string::npos)
                stringVarNames_.insert(p.name);
            // 接口类型参数 → 值类型追踪（引用用 . 不是 ->）
            if (auto* nt = dynamic_cast<const NamedType*>(p.type.get())) {
                if (interfaceNames_.count(nt->name))
                    valueTypeVarNames_.insert(p.name);
            }
        }
    }

    std::string retType = decl.returnType ? mapType(*decl.returnType) : "void";

    // 存储 C++ 返回类型，供 genReturnStmt 生成正确 RecordExpr
    currentReturnCppType_ = retType;

    // 检测方法名与 receiver 的字段名是否冲突，冲突时加 _fun 后缀
    std::string methodCppName = safeName(decl.name);
    auto fnIt = structFieldNames_.find(decl.receiverType);
    if (fnIt != structFieldNames_.end() && fnIt->second.count(methodCppName))
        methodCppName += "_fun";

    std::string sig = retType + " " + recvFullType + "::" + methodCppName + "(";
    for (size_t i = 0; i < decl.params.size(); ++i) {
        if (i > 0) sig += ", ";
        sig += (decl.params[i].type ? mapParamType(*decl.params[i].type) : "auto")
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
    std::string callPrefix = nsName.empty() ? "::aura_main" : nsName + "::aura_main";
    if (ioSync_) {
        // 同步模式：aura_main 返回 void，直接调用
        cpp << "  " << callPrefix << "(io);\n";
        cpp << "  return 0;\n";
    } else {
        // 异步模式：aura_main 返回 task<void>，走 run_event_loop
        cpp << "  auto t = " << callPrefix << "(io);\n";
        cpp << "  aura_rt::run_event_loop(t);\n";
        cpp << "  return 0;\n";
    }
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
        // 若该名称是模板类型别名，添加其模板参数
        auto aliasIt = typeAliasTemplateParams_.find(n->name);
        if (aliasIt != typeAliasTemplateParams_.end()) {
            for (auto& tp : aliasIt->second) out.insert(tp);
            return;
        }
        // 排除内置类型（int/float/bool/string/None 不是泛型参数，无需 template<...>）
        if (n->name == "int" || n->name == "float" || n->name == "bool"
            || n->name == "string" || n->name == "None")
            return;

        // 无 typeArgs + 非注册类型 → 是泛型参数（如 Pair<A,B> 中的 A/B）
        if (n->typeArgs.empty() && !registeredTypes_.count(n->name)
            && !interfaceNames_.count(n->name)
            && !BuiltinRegistry::get().findType(n->name))
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

    // plan12 统一方案：若返回泛型闭包，外层函数不模板化，泛型由闭包自身声明
    if (decl.returnType) {
        std::set<std::string> retGen;
        if (auto* ft = dynamic_cast<const FunctionType*>(decl.returnType.get())) {
            collectTParams(*ft, retGen);
            if (!retGen.empty()) return {}; // 泛型闭包 → 不模板化
        } else if (auto* nt = dynamic_cast<const NamedType*>(decl.returnType.get())) {
            // 仅函数式类型别名（如 Pipeline<T> = fun(T)->T，非堆类型）跳过模板化
            // 堆类型（如 Tree<T>）保持模板参数
            auto it = registeredTypes_.find(nt->name);
            if (typeAliasTemplateParams_.count(nt->name)
                && it != registeredTypes_.end() && !it->second)  // registered as non-heap
                return {};
        }
    }

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
