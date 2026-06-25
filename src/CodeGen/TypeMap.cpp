#include "CodeGen.h"

namespace Aura {

// ============================================================
// 类型名注册
// ============================================================

void CodeGenerator::registerTypeName(const std::string& auraName, bool isHeap) {
    registeredTypes_[auraName] = isHeap;
}

bool CodeGenerator::isValueType(const std::string& auraName) const {
    auto it = registeredTypes_.find(auraName);
    if (it == registeredTypes_.end()) return false;
    return !it->second;
}

bool CodeGenerator::isHeapType(const std::string& auraName) const {
    auto it = registeredTypes_.find(auraName);
    if (it == registeredTypes_.end()) return false;
    return it->second;
}

// ============================================================
// 类型映射（plan §4.1）
// ============================================================

std::string CodeGenerator::mapType(const TypeExpr& type) {
    if (auto* n = dynamic_cast<const NamedType*>(&type)) {
        std::string base = mapNamedType(n->name);
        if (!n->typeArgs.empty()) {
            bool hadStar = base.size() > 1 && base.back() == '*';
            if (hadStar) base.pop_back();
            base += "<";
            for (size_t i = 0; i < n->typeArgs.size(); ++i) {
                if (i > 0) base += ", ";
                base += n->typeArgs[i] ? mapType(*n->typeArgs[i]) : "???";
            }
            base += ">";
            if (hadStar) base += "*";
        }
        // 命名空间前缀：math.Pair → math::Pair
        if (!n->namespacePrefix.empty()) {
            std::string prefix;
            for (auto& ns : n->namespacePrefix)
                prefix += ns + "::";
            // 跨模块类型都是堆指针，若 base 不是以 * 结尾则追加
            if (!base.empty() && base.back() != '*')
                base += "*";
            if (base.find("aura_rt::") == 0) {
                base.insert(std::string("aura_rt::").size(), prefix);
            } else {
                base = prefix + base;
            }
        }
        return base;
    }
    if (auto* g = dynamic_cast<const GenericTypeRef*>(&type))
        return mapGenericRef(*g);
    if (auto* l = dynamic_cast<const ListType*>(&type)) {
        auto elem = l->elementType ? mapType(*l->elementType) : "???";
        return "aura_rt::Array<" + elem + ">*";
    }
    if (dynamic_cast<const RecordType*>(&type)) {
        return "/* inline record */ aura_rt::GcObject*";
    }
    if (auto* u = dynamic_cast<const UnionType*>(&type)) {
        std::string result = "std::variant<";
        for (size_t i = 0; i < u->types.size(); ++i) {
            if (i > 0) result += ", ";
            result += u->types[i] ? mapType(*u->types[i]) : "???";
        }
        result += ">";
        return result;
    }
    if (auto* f = dynamic_cast<const FunctionType*>(&type)) {
        // 函数类型 → 映射为 std::function
        std::string sig = "std::function<";
        sig += f->returnType ? mapType(*f->returnType) : "void";
        sig += "(";
        for (size_t i = 0; i < f->paramTypes.size(); ++i) {
            if (i > 0) sig += ", ";
            sig += f->paramTypes[i] ? mapType(*f->paramTypes[i]) : "???";
        }
        sig += ")>";
        return sig;
    }
    return "/* unknown_type */";
}

std::string CodeGenerator::mapNamedType(const std::string& name) {
    // 内置值类型
    if (name == "int")    return "int32_t";
    if (name == "float")  return "double";
    if (name == "bool")   return "bool";

    // string → GcString*
    if (name == "string") return "aura_rt::GcString*";

    // None 在值上下文
    if (name == "None")   return "aura_rt::NoneType";

    // 用户定义类型
    auto it = registeredTypes_.find(name);
    if (it != registeredTypes_.end()) {
        // 堆对象 → 返回指针类型
        if (it->second) {
            return name + "*";
        }
        // 运行时值类型 → 加 aura_rt:: 前缀
        if (name == "Io" || name == "Path") {
            return "aura_rt::" + name;
        }
        return name;
    }

    // 接口类型 → 保留原名（函数参数生成处做 const& 处理）
    if (interfaceNames_.contains(name)) {
        return name;
    }

    // 未知类型 → 保守返回
    return name;
}

std::string CodeGenerator::mapGenericRef(const GenericTypeRef& genericRef) {
    // <A>, <T> → 直接映射为 C++ 模板参数名（A, T）
    return genericRef.name;
}

std::string CodeGenerator::mapValueType(const TypeExpr& type) {
    // 值上下文：指针类型去掉 *
    std::string result = mapType(type);
    if (result.size() > 1 && result.back() == '*') {
        result.pop_back();
    }
    return result;
}

std::string CodeGenerator::mapParamType(const TypeExpr& type) {
    std::string result = mapType(type);
    // 接口类型 → const&（抽象类不能按值传递）
    if (auto* nt = dynamic_cast<const NamedType*>(&type)) {
        if (interfaceNames_.count(nt->name)) {
            return "const " + result + "&";
        }
    }
    return result;
}

// ============================================================
// SemType → C++ 类型映射
// ============================================================

std::string CodeGenerator::mapSemType(const SemType& semType) {
    if (auto* p = dynamic_cast<const PrimSemType*>(&semType)) {
        switch (p->kind) {
            case PrimSemType::Int:    return "int32_t";
            case PrimSemType::Float:  return "double";
            case PrimSemType::Bool:   return "bool";
            case PrimSemType::String: return "aura_rt::GcString*";
        }
    }
    if (dynamic_cast<const NoneSemType*>(&semType))
        return "aura_rt::NoneType";
    if (dynamic_cast<const ErrorSemType*>(&semType))
        return "/* error_type */";
    if (auto* l = dynamic_cast<const ListSemType*>(&semType)) {
        return "aura_rt::Array<" + mapSemType(*l->elementType) + ">*";
    }
    if (auto* r = dynamic_cast<const RecordSemType*>(&semType)) {
        if (!r->canonicalName.empty()) {
            return r->canonicalName + "*";
        }
        return "aura_rt::GcObject*";
    }
    if (auto* f = dynamic_cast<const FuncSemType*>(&semType)) {
        std::string sig = "std::function<";
        sig += f->returnType ? mapSemType(*f->returnType) : "void";
        sig += "(";
        for (size_t i = 0; i < f->paramTypes.size(); ++i) {
            if (i > 0) sig += ", ";
            sig += f->paramTypes[i] ? mapSemType(*f->paramTypes[i]) : "auto";
        }
        sig += ")>";
        return sig;
    }
    if (auto* gs = dynamic_cast<const GenericSemType*>(&semType)) {
        if (!gs->resolvedName.empty()) return gs->resolvedName + "*";
        return "auto";
    }
    return "/* unknown_semtype */";
}

// ============================================================
// 类型描述符生成（plan §4.2, §4.10）
// ============================================================

void CodeGenerator::genTypeDescriptor(std::ostream& cpp,
                                       const std::string& structName,
                                       const std::vector<std::string>& templateParams,
                                       const std::vector<std::string>& ptrFieldNames) {
    // 构建 C++ 模板前缀: template<typename A, typename B>
    std::string tprefix, tparamsStr;
    if (!templateParams.empty()) {
        tprefix = "template<";
        for (size_t i = 0; i < templateParams.size(); ++i) {
            if (i > 0) { tprefix += ", "; tparamsStr += ", "; }
            tprefix += "typename " + templateParams[i];
            tparamsStr += templateParams[i];
        }
        tprefix += ">\n";
        tparamsStr = "<" + tparamsStr + ">";
    }

    std::string fullName = structName + tparamsStr;

    // 非模板类型不需要 prefix
    bool isTemplate = !templateParams.empty();

    if (ptrFieldNames.empty()) {
        if (isTemplate) cpp << tprefix;
        cpp << "const aura_rt::TypeDescriptor " << fullName
            << "::_desc = { sizeof(" << fullName << "), 0, nullptr };\n";
    } else {
        if (isTemplate) cpp << tprefix;
        cpp << "static const size_t _" << structName << "_ptrs[] = {";
        for (size_t i = 0; i < ptrFieldNames.size(); ++i) {
            if (i > 0) cpp << ", ";
            cpp << "offsetof(" << fullName << ", " << ptrFieldNames[i] << ")";
        }
        cpp << "};\n";
        if (isTemplate) cpp << tprefix;
        cpp << "const aura_rt::TypeDescriptor " << fullName
            << "::_desc = { sizeof(" << fullName << "), "
            << ptrFieldNames.size() << ", _"
            << structName << "_ptrs" << tparamsStr << " };\n";
    }
}

} // namespace Aura
