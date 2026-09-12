#include "CodeGen.h"
#include "../Sema/BuiltinRegistry.h"
#include <algorithm>
#include <set>
#include <sstream>
#include <utility>

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
    std::vector<std::string> deferredPtrFields;   // #54：未绑定泛型字段（val: T），desc 延迟判定
    std::set<std::string> fieldNames;

    for (auto& f : body.fields) {
        std::string cppType = f.type ? mapType(*f.type) : "???";
        h << "  " << cppType << " " << safeName(f.name) << ";\n";
        fieldNames.insert(f.name);

        // 检测 GC 指针字段：任何映射后以 * 结尾的 C++ 类型都是堆对象指针
        // 包括 NamedType（如 User*）、ListType（如 Array<T>*）、GcString* 等
        if (!cppType.empty() && cppType.back() == '*') {
            ptrFields.push_back(safeName(f.name));
        } else if (isIfaceViewTypeName(cppType)) {
            // 接口视图字段（值类型，非 * 结尾）：注册 self 子偏移
            // 格式 "field+ViewType"，genTypeDescriptor 展开为
            // offsetof(Self, field) + offsetof(ViewType, self)
            ptrFields.push_back(safeName(f.name) + "+" + cppType);
        } else if (std::find(tparams.begin(), tparams.end(), cppType) != tparams.end()) {
            // #54：未绑定泛型字段（val: T / val: <T>）→ mapType 产物为裸名（genRecordStruct
            // 上下文 ifaceTypeMap_/defaultArgMaterializedTypes_ 为空，mapType 对泛型字段
            // 无论 NamedType 裸 T 还是 GenericTypeRef <T> 均返回裸名 T）。判定用
            // 「cppType ∈ tparams」而非 dynamic_cast<GenericTypeRef*>——TypeParser.cpp:29-39
            // 实证仅 `<T>` 语法产 GenericTypeRef，裸 T 是 NamedType，AST 判定恒 false。
            // 此类字段实例化后可能是 GC 指针（string/record/列表）——desc 生成时必须
            // if constexpr 延迟判定（per-instantiation），否则 Box<T>::_desc ptrFieldCount=0
            // → GC mark 不追踪/compact 不更新 → 悬垂 0xC0000005。条目格式 "name|cppType"。
            deferredPtrFields.push_back(safeName(f.name) + "|" + cppType);
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
            // bug-07：方法自身裸泛型（非 receiver 泛型，如 apply(f: fun(U)->U) 的 U）
            // → 类内函数模板声明 template<...> 前缀（与定义侧 genMethodDecl 对齐）；
            // receiver 泛型（Box<T> 的 T）已在 struct 模板作用域内，无需重复声明。
            if (!md.templateParams.empty()) {
                h << "  template<";
                for (size_t tp = 0; tp < md.templateParams.size(); ++tp) {
                    if (tp > 0) h << ", ";
                    h << "typename " << md.templateParams[tp];
                }
                h << ">\n";
            }
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
    genTypeDescriptor(tparams.empty() ? cpp : h, name, tparams, ptrFields, deferredPtrFields);
}

// ============================================================
// 接口声明 → 值视图结构体 + XFunc GC 化（P1 全接口去虚化）
// ============================================================

bool CodeGenerator::ifaceMethodHasFreeGeneric(const InterfaceDecl& iface,
                                              const InterfaceMethodSig& m) const {
    std::set<std::string> sigGen;
    for (auto& p : m.params)
        if (p.type) collectTParams(*p.type, sigGen);
    if (m.returnType) collectTParams(*m.returnType, sigGen);
    for (auto& g : sigGen) {
        // 接口泛型形参（如 Comparable<T> 的 T）在模板视图/适配器 tmap 中可表达
        if (std::find(iface.typeParams.begin(), iface.typeParams.end(), g)
            != iface.typeParams.end())
            continue;
        // 自由裸泛型（apply(f: fun(U)->U) 的 U，非接口泛型）→ 具体类中 U 未定义
        return true;
    }
    return false;
}

void CodeGenerator::genInterfaceDecl(std::ostream& h,
                                      const InterfaceDecl& decl) {
    std::string name = decl.name;

    // G1：注册接口方法形参 C++ 类型名（键 = "接口名.methodName"，与 genMethodCall 的
    // recvTypeKey 机制一致；接口视图调用 s.put(u) 时实参 Optional/Union 装箱）。
    // 必须在生成视图结构体前注册（A 遍，调用点可能先于接口声明生成）。
    for (auto& m : decl.methods) {
        std::vector<std::string> ptys;
        ptys.reserve(m.params.size());
        for (auto& p : m.params)
            ptys.push_back(p.type ? mapType(*p.type) : "");
        methodParamCppTypes_[name + "." + m.name] = std::move(ptys);
        // G3：注册接口方法接口参数（键 = "接口名.methodName"，同键机制；接口视图
        // 调用 g.use(u) 的 record 实参直传接口视图形参 → genMethodCall 做
        // record→view）。注册条件与 #8 一致（用户接口 + 内置接口）。
        methodInterfaceParams_.erase(name + "." + m.name);
        for (size_t i = 0; i < m.params.size(); ++i) {
            auto* nt = m.params[i].type
                ? dynamic_cast<const NamedType*>(m.params[i].type.get())
                : nullptr;
            if (!nt) continue;
            bool builtinIface = false;
            for (auto& ai : BuiltinRegistry::get().auraiInterfaces())
                if (ai->name == nt->name) { builtinIface = true; break; }
            if (interfaceNames_.count(nt->name) || builtinIface)
                methodInterfaceParams_[name + "." + m.name].push_back({i, nt->name});
        }
    }

    // 内置 Iterator：C++ 形态来自 runtime/builtin/iterator.h（aura_rt::Iterator<T>），
    // 不在此生成视图（避免与 runtime 的 Iterator<T> 重复/冲突）。
    // interfaces.aurai 中的声明仅供 Sema（方法签名），record impl 适配器走 genIfaceAdapter 特判。
    if (name == "Iterator") return;

    // 接口视图结构体引用用户 record（Wrapper* / Transform<T>*），而 record struct 完整
    // 定义在第三遍 B 才生成 → 此处输出 C++ 前向声明（problem.txt「接口声明中引用后置
    // 类型」CodeGen 侧：接口方法签名引用后置 record 时视图结构体引用未声明类型）。
    emitIfaceRecordForwardDecls(h, decl);

    // 泛型接口 → 模板视图（template<typename T> struct Comparable { ... }）
    // 方法签名中的泛型引用（GenericTypeRef → "T"）在模板作用域内有效
    std::string tprefix;
    if (!decl.typeParams.empty()) {
        tprefix = "template<";
        for (size_t i = 0; i < decl.typeParams.size(); ++i) {
            if (i > 0) tprefix += ", ";
            tprefix += "typename " + decl.typeParams[i];
        }
        tprefix += ">\n";
    }

    // 1. 值视图结构体（B+W 统一对象模型）：
    //    - 每个纯虚方法 → 无捕获函数指针字段（名 = 方法名 + "Fn"，避免与成员函数名冲突）
    //      + 转发成员函数（内部调用函数指针，首参 self）
    //    - 默认方法 → 视图内普通成员函数，体内经 currentReceiverName_="self" 映射 this，
    //      转发调用本视图的纯虚转发成员（完全复用现有 genBlock 翻译）
    //    - CppBridge（返回类型含未绑定 U，无法在 C++ 表达）→ 不生成
    h << tprefix << "struct " << name << " {\n";
    // 1a. 函数指针字段（仅纯虚方法；默认方法不占 Fn 字段，由视图内默认方法体承接）
    for (auto& m : decl.methods) {
        // bug-07：含自由裸泛型签名（fun(U)->U 的 U 非接口泛型）→ C++ 视图无法表达
        // std::function<U(U)>（'U' was not declared），跳过生成（record 直调不受影响）
        if (m.defaultBody || m.bodyKind == InterfaceMethodSig::BodyKind::CppBridge
            || ifaceMethodHasFreeGeneric(decl, m)) continue;
        std::string retType = m.returnType ? mapType(*m.returnType) : "void";
        if (retType == "aura_rt::NoneType") retType = "void";   // #33：接口方法 None→void（对齐 genMethodDecl M1）
        h << "  " << retType << " (*" << m.name << "Fn)(aura_rt::GcObject* self";
        for (size_t i = 0; i < m.params.size(); ++i) {
            h << ", " << (m.params[i].type ? mapType(*m.params[i].type) : "auto");
        }
        h << ") = nullptr;\n";
    }
    h << "  aura_rt::GcObject* self = nullptr;\n";
    // 1b. 成员函数（纯虚转发 / 默认方法体）
    for (auto& m : decl.methods) {
        // bug-07：含自由裸泛型签名 → 无 Fn 字段，跳过成员函数（防引用不存在的 Fn）
        if (m.bodyKind == InterfaceMethodSig::BodyKind::CppBridge
            || ifaceMethodHasFreeGeneric(decl, m)) continue;
        std::string retType = m.returnType ? mapType(*m.returnType) : "void";
        if (retType == "aura_rt::NoneType") retType = "void";   // #33：接口方法 None→void（对齐 genMethodDecl M1）
        if (m.defaultBody) {
            // 默认方法：体内 Aura 代码（self.xxx(...)）经 genBlock 翻译为 this->xxx(...)
            h << "  " << retType << " " << m.name << "(";
            for (size_t i = 0; i < m.params.size(); ++i) {
                if (i > 0) h << ", ";
                h << (m.params[i].type ? mapType(*m.params[i].type) : "auto")
                  << " " << safeName(m.params[i].name);
            }
            h << ") {\n";
            currentReceiverName_ = "self";
            genBlock(h, *m.defaultBody, /*isCoroutine=*/false);
            currentReceiverName_.clear();
            // 清理方法体生成残留的变量跟踪状态（与 genMethodDecl 末尾一致）
            clearVarTrackingState();
            h << "  }\n";
        } else {
            // 纯虚方法：转发成员 → 函数指针
            h << "  " << retType << " " << m.name << "(";
            for (size_t i = 0; i < m.params.size(); ++i) {
                if (i > 0) h << ", ";
                h << (m.params[i].type ? mapType(*m.params[i].type) : "auto")
                  << " " << safeName(m.params[i].name);
            }
            h << ") { return " << m.name << "Fn(self";
            for (size_t i = 0; i < m.params.size(); ++i) {
                h << ", " << safeName(m.params[i].name);
            }
            h << "); }\n";
        }
    }
    h << "};\n\n";

    // 2. 闭包适配器（XFunc）——仅非泛型单方法接口（泛型接口无类型参数可绑定）
    // feature-06（阶段 B，B3b）：收敛为 CallableObj 派生接线。闭包实参（GC 堆
    // CallableObj 派生，捕获槽 desc 追踪）直接作视图 self——适配器仅需静态转发：
    //   <iface>Fn(GcObject* self, args) → static_cast<CallableObj<Ret,Params...>*>
    //     (self)->invoke(self, args...)
    // 捕获 GC 可见性由闭包自身 desc 保证（替代旧 std::function 成员 + desc 0 追踪
    // 盲区——feature-06 §1 痛点 3）。view() 从基指针构造视图，调用点零额外分配。
    if (decl.typeParams.empty() && decl.methods.size() == 1
        && !ifaceMethodHasFreeGeneric(decl, decl.methods[0])) {
        auto& m = decl.methods[0];
        std::string retType = m.returnType ? mapType(*m.returnType) : "void";
        if (retType == "aura_rt::NoneType") retType = "void";   // #33：接口方法 None→void（对齐 genMethodDecl M1）
        // 接口方法签名 → CallableObj<Ret, Params...>（与闭包生成路径同源映射）
        std::string base = "aura_rt::CallableObj<" + retType;
        std::string fnParams;    // 转发函数形参（含 self）
        std::string argNames;    // 转发实参名（invoke 调用）
        fnParams = "aura_rt::GcObject* self";
        for (size_t i = 0; i < m.params.size(); ++i) {
            base += ", ";
            fnParams += ", ";
            if (i > 0) argNames += ", ";
            std::string pt = m.params[i].type ? mapType(*m.params[i].type) : "auto";
            base += pt;
            fnParams += pt + " " + safeName(m.params[i].name);
            argNames += safeName(m.params[i].name);
        }
        base += ">";
        h << "struct " << name << "Func final : " << base << " {\n";
        h << "  static " << retType << " " << m.name << "Fn(" << fnParams << ") {\n";
        h << "    auto* __c = static_cast<" << base << "*>(self);\n";
        h << "    return __c->invoke(__c" << (argNames.empty() ? "" : ", " + argNames)
          << ");\n";
        h << "  }\n";
        h << "  static " << name << " view(" << base << "* o) {\n";
        h << "    return { &" << name << "Func::" << m.name << "Fn, o };\n";
        h << "  }\n";
        h << "};\n\n";
    }
}

// ============================================================
// 接口视图引用的用户 record C++ 前向声明
// ============================================================
// 接口视图结构体（genInterfaceDecl）在第三遍 A 生成，而用户 record struct 完整定义在
// 第三遍 B 才输出——接口方法签名引用后置 record（如 interface Getter { get() -> Wrapper }
// 且 Wrapper 声明在接口之后）时，视图结构体/闭包适配器的 `Wrapper*` 引用未声明类型 →
// g++ 'Wrapper' was not declared。此处递归收集签名引用的用户堆 record（registeredTypes_
// 命中且为堆；排除接口名/内置类型），输出 C++ 前向声明（指针引用只需前向声明）。
// 泛型 record（Transform<T>）输出模板前向声明，参数列表取自 typeAliasTemplateParams_。
void CodeGenerator::emitIfaceRecordForwardDecls(std::ostream& h,
                                                const InterfaceDecl& decl) {
    std::set<std::string> emitted;
    auto emitFwd = [&](const TypeExpr* t, auto&& self) -> void {
        if (!t) return;
        if (auto* n = dynamic_cast<const NamedType*>(t)) {
            // 用户堆 record 才需前向声明（registeredTypes_ true）；接口名/内置类型/
            // 非 record 别名（isHeap false）跳过；跨模块（命名空间限定）暂不处理
            auto rt = registeredTypes_.find(n->name);
            if (n->namespacePrefix.empty() && rt != registeredTypes_.end() && rt->second
                && !interfaceNames_.count(n->name) && emitted.insert(n->name).second) {
                auto tp = typeAliasTemplateParams_.find(n->name);
                if (tp != typeAliasTemplateParams_.end() && !tp->second.empty()) {
                    h << "template<";
                    for (size_t i = 0; i < tp->second.size(); ++i) {
                        if (i > 0) h << ", ";
                        h << "typename " << tp->second[i];
                    }
                    h << ">\nstruct " << n->name << ";\n";
                } else {
                    h << "struct " << n->name << ";\n";
                }
            }
            for (auto& a : n->typeArgs)
                if (a) self(a.get(), self);
            return;
        }
        if (auto* l = dynamic_cast<const ListType*>(t)) {
            self(l->elementType.get(), self);
            return;
        }
        if (auto* r = dynamic_cast<const RecordType*>(t)) {
            for (auto& f : r->fields)
                if (f.type) self(f.type.get(), self);
            return;
        }
        if (auto* u = dynamic_cast<const UnionType*>(t)) {
            for (auto& v : u->types)
                if (v) self(v.get(), self);
            return;
        }
        if (auto* fn = dynamic_cast<const FunctionType*>(t)) {
            for (auto& p : fn->paramTypes)
                if (p) self(p.get(), self);
            self(fn->returnType.get(), self);
            return;
        }
        if (auto* tp2 = dynamic_cast<const TupleTypeExpr*>(t)) {
            for (auto& e : tp2->elementTypes)
                if (e) self(e.get(), self);
        }
    };
    for (auto& m : decl.methods) {
        if (m.bodyKind == InterfaceMethodSig::BodyKind::CppBridge) continue;
        for (auto& p : m.params)
            emitFwd(p.type.get(), emitFwd);
        emitFwd(m.returnType.get(), emitFwd);
    }
}

// ============================================================
// "类型 × 接口"适配器（方案 B：record 保持不动，适配器持值持有根）
// ============================================================

void CodeGenerator::genIfaceAdapter(std::ostream& h,
                                    const std::string& recordName,
                                    const InterfaceDecl& iface) {
    std::string adapterName = safeName(recordName) + iface.name;
    // 适配器可能生成于 record 定义之前（接口 decl 先于 type decl）：
    // 前向声明 record（重复声明无害），保证 GcRootHandle<record*> 成员合法
    h << "struct " << safeName(recordName) << ";\n";
    // 泛型接口：基类实例化为 Comparable<Point>（类型实参来自 interfaceImplementations_ 收集）
    // 同时构建"泛型形参名 → 具体 C++ 类型"映射（T → Point*），
    // 适配器无模板上下文，方法签名中的泛型引用必须替换为具体类型
    std::map<std::string, std::string> tmap;
    // 内置 Iterator：C++ 形态来自 runtime（aura_rt::Iterator<T>），基类名需带命名空间
    std::string baseType = (iface.name == "Iterator") ? "aura_rt::Iterator" : iface.name;
    if (!iface.typeParams.empty()) {
        auto recIt = interfaceImplementations_.find(recordName);
        if (recIt != interfaceImplementations_.end()) {
            auto ifIt = recIt->second.find(iface.name);
            if (ifIt != recIt->second.end() && !ifIt->second.empty()) {
                baseType += "<";
                for (size_t i = 0; i < ifIt->second.size(); ++i) {
                    if (i > 0) baseType += ", ";
                    baseType += ifIt->second[i];
                    if (i < iface.typeParams.size())
                        tmap[iface.typeParams[i]] = ifIt->second[i];
                }
                baseType += ">";
            }
        }
    }
    // 接口方法签名类型映射：泛型引用（T）→ 具体实参类型；其余走 mapType
    // 注：接口参数/返回类型中的裸泛型名（如 cmp(other: T) 的 T）由 TypeParser 解析为
    // NamedType 而非 GenericTypeRef，两者都需要查 tmap
    auto mapIfaceType = [&](const TypeExpr* t) -> std::string {
        if (auto* g = dynamic_cast<const GenericTypeRef*>(t)) {
            auto it = tmap.find(g->name);
            if (it != tmap.end()) return it->second;
        }
        if (auto* n = dynamic_cast<const NamedType*>(t)) {
            auto it = tmap.find(n->name);
            if (it != tmap.end()) return it->second;
        }
        std::string r = t ? mapType(*t) : "auto";
        if (r == "aura_rt::NoneType") r = "void";   // #33：适配器签名 None→void（覆盖返回类型）
        return r;
    };
    // 将 tmap 挂到全局类型映射上下文：mapType/mapGenericRef 递归代换泛型形参名，
    // 使容器/复合类型内嵌 T（Optional<T> / [T] / Iterator<T> / Transform<T> / Box2<T>）
    // 在 getFn 签名中递归实例化（对应 Sema 侧 substitute 在 TypeExpr 层的实现）。
    // 仅本函数作用域内有效（退出前恢复），不影响其余 CodeGen 对 T 的原样输出。
    auto savedTypeMap = std::exchange(ifaceTypeMap_, tmap);
    // 内置 Iterator：适配器 GC 化（单继承 GcObject），owner 裸指针经 desc 扫描；
    // 视图 {nextFn, self} 分派到 owner->next()（self 恒为适配器对象起始）
    if (iface.name == "Iterator") {
        std::string elem = "int32_t";
        auto recIt = interfaceImplementations_.find(recordName);
        if (recIt != interfaceImplementations_.end()) {
            auto ifIt = recIt->second.find(iface.name);
            if (ifIt != recIt->second.end() && !ifIt->second.empty())
                elem = ifIt->second[0];
        }
        h << "struct " << adapterName << " final : aura_rt::GcObject {\n";
        h << "  " << recordName << "* owner;\n";
        h << "  explicit " << adapterName << "(" << recordName << "* o) : owner(o) {}\n";
        h << "  static aura_rt::Optional<" << elem << ">* nextFn(aura_rt::GcObject* self) {\n";
        h << "    return static_cast<" << adapterName << "*>(self)->owner->next();\n";
        h << "  }\n";
        h << "  static aura_rt::Iterator<" << elem << "> view(" << adapterName << "* o) {\n";
        h << "    return { &nextFn, o };\n";
        h << "  }\n";
        h << "  static const aura_rt::TypeDescriptor& desc() {\n";
        h << "    static const size_t _o[] = { offsetof(" << adapterName << ", owner) };\n";
        h << "    static const aura_rt::TypeDescriptor d = { sizeof(" << adapterName
          << "), 1, _o, 0, nullptr, nullptr };\n";
        h << "    return d;\n";
        h << "  }\n";
        h << "};\n\n";
        ifaceTypeMap_ = std::move(savedTypeMap);
        return;
    }
    // 非 Iterator 接口：适配器 GC 化（单继承 GcObject），owner 裸指针经 desc 扫描；
    // 视图 { 纯虚方法Fn..., self } 分派到 owner->方法(...)
    // 仅非默认/非 CppBridge 方法生成 static Fn（视图结构体中只有纯虚方法占用 Fn 字段）
    std::string viewType = baseType;   // 泛型接口已实例化（如 Comparable<Point*>）
    h << "struct " << adapterName << " final : aura_rt::GcObject {\n";
    h << "  " << recordName << "* owner;\n";
    h << "  explicit " << adapterName << "(" << recordName << "* o) : owner(o) {}\n";
    for (auto& m : iface.methods) {
        // bug-07：含自由裸泛型签名 → 适配器 static Fn 无法表达 std::function<U(U)>，
        // 跳过生成（接口视图 Fn 字段同样已跳过；record 直调不受影响）
        if (m.defaultBody || m.bodyKind == InterfaceMethodSig::BodyKind::CppBridge
            || ifaceMethodHasFreeGeneric(iface, m)) continue;
        std::string retType = m.returnType ? mapIfaceType(m.returnType.get()) : "void";
        h << "  static " << retType << " " << m.name << "Fn(aura_rt::GcObject* self";
        for (size_t i = 0; i < m.params.size(); ++i) {
            h << ", " << mapIfaceType(m.params[i].type.get())
              << " " << safeName(m.params[i].name);
        }
        h << ") {\n";
        h << "    return static_cast<" << adapterName << "*>(self)->owner->"
          << m.name << "(";
        for (size_t i = 0; i < m.params.size(); ++i) {
            if (i > 0) h << ", ";
            h << safeName(m.params[i].name);
        }
        h << ");\n";
        h << "  }\n";
    }
    // view()：聚合初始化，字段顺序 = 纯虚方法 Fn 声明序 + self
    h << "  static " << viewType << " view(" << adapterName << "* o) {\n";
    h << "    return {";
    std::string viewFields;
    for (auto& m : iface.methods) {
        if (m.defaultBody || m.bodyKind == InterfaceMethodSig::BodyKind::CppBridge
            || ifaceMethodHasFreeGeneric(iface, m)) continue;
        if (!viewFields.empty()) viewFields += ", ";
        viewFields += "&" + std::string(m.name) + "Fn";
    }
    h << viewFields << (viewFields.empty() ? "o" : ", o") << " };\n";
    h << "  }\n";
    h << "  static const aura_rt::TypeDescriptor& desc() {\n";
    h << "    static const size_t _o[] = { offsetof(" << adapterName << ", owner) };\n";
    h << "    static const aura_rt::TypeDescriptor d = { sizeof(" << adapterName
      << "), 1, _o, 0, nullptr, nullptr };\n";
    h << "    return d;\n";
    h << "  }\n";
    h << "};\n\n";
    ifaceTypeMap_ = std::move(savedTypeMap);
}

} // namespace Aura
