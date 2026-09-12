#include "SemAnalyzer.h"
#include <ranges>

namespace Aura {

bool SemAnalyzer::isAssignable(const SemType& target, const SemType& source) const {
    // 静默传播 error 类型
    if (dynamic_cast<const ErrorSemType*>(&target) || dynamic_cast<const ErrorSemType*>(&source))
        return true;

    // 泛型形参（未解析，resolvedName 为空，如泛型函数体内的 T）——模板体/构造体内
    // 字段类型为未绑定 T，实例化时再检查；但容器型 source（Optional/物化 Optional<T>/
    // Union）在任意实例化下均 ≠ T（Aura 无隐式解箱/解包）→ 恒非法，直接拒绝，
    // 防泄漏坏 C++（原无条件放行吞掉错配，#42/#53，见下方 gt->resolvedName.empty() 分支）
    // 已解析的泛型类型（如 Iterator<string>）与同为泛型的 source 必须精确比较
    // （P0.5：equals 比较 resolvedName，防止 Iterator<int> ≡ Iterator<string> 混淆）；
    // source 为结构化推断类型（如 some() 的 OptionalSemType）时保持旧放行语义——
    // 类型标注 Optional<int> 物化为 GenericSemType 而工厂推断为 OptionalSemType，
    // 二者同义，直接比较会误报（历史表示不一致，不在本次修复范围）
    if (auto* gt = dynamic_cast<const GenericSemType*>(&target)) {
        if (gt->resolvedName.empty()) {
            // #42/#53：模板体（含 ctor 体）内 self.field 的字段类型为未绑定 T。容器型
            // source（Optional / 物化 Optional<T> / Union）在任意实例化下均 ≠ T（Aura
            // 无隐式解箱/解包）→ 干净报错，防泄漏坏 C++（原无条件放行吞掉错配）。
            if (dynamic_cast<const OptionalSemType*>(&source)) return false;
            if (auto* sg = dynamic_cast<const GenericSemType*>(&source))
                if (sg->name == "Optional" && !sg->resolvedName.empty()) return false;
            if (dynamic_cast<const UnionSemType*>(&source)) return false;
            return true;   // 纯 T→T、具体类型→T 等模板体合法形态原样放行
        }
        if (auto* gs = dynamic_cast<const GenericSemType*>(&source)) {
            if (gt->equals(*gs)) return true;
            // gap7 兜底：同名容器泛型（Optional/Iterator/用户泛型实例）两侧 resolvedName
            // 仅因 record 元素 '*' 物化差异而字符串不等时（impl 声明侧 cppNameOfTypeExpr
            // 对 record 缺 '*' vs 接口侧 substitute/semTypeToCppName 补 '*'），做元素级
            // 语义比较消除 '*' 差异。elemTypeOf 提取 <...> 内元素 + semTypeFromCppName
            // 还原（"Point*" 与 "Point" 还原为同一 RecordSemType 结构）。仅两侧 name
            // 相同且元素均可提取时生效；任一步失败维持原 equals 结果（false）。
            if (gt->name == gs->name && !gs->resolvedName.empty()) {
                auto tElem = elemTypeOf(gt);
                auto sElem = elemTypeOf(gs);
                if (!dynamic_cast<const ErrorSemType*>(tElem.get())
                    && !dynamic_cast<const ErrorSemType*>(sElem.get()))
                    return isAssignable(*tElem, *sElem);
            }
            return false;
        }
        // source 非 GenericSemType（Record/Prim/List/结构化 Optional/None 等）：
        // 原实现 L680 一律 return true（G1 遗留-1 主根因）——已物化 GenericSemType
        // 目标（resolvedName 非空，如显式 `Optional<[Point]>` 注解）对任意非泛型
        // source 不检查元素语义，record 直赋 Optional<[Point]> 等语义不成立形态被
        // 放行 → 坏 C++。按目标泛型语义分层校验（以不误伤已合法形态为最高优先）：
        //   - OptionalSemType source：GenericSemType{Optional}（注解物化）与
        //     OptionalSemType（some()/union 折叠结构化推断）同义的历史表示不一致，
        //     保留旧放行（some([{..}]) → Optional<[Point]>、let o: Optional<[Point]> =
        //     some([{..}]) 等 #6 形态）；对非 Optional 目标同为旧行为，保守不误伤。
        //   - name=="Optional"：允许隐式装箱（裸值 make_optional 进元素类型），
        //     递归 isAssignable(elementTypeOf(target), source)——record→Optional<Point>
        //     合法（元素 Point 结构匹配）、record→Optional<[Point]> 非法（record 不可
        //     赋 [Point]）、int→Optional<int> 合法、list→Optional<[Point]> 合法
        //     （G1 return [{..}] 隐式包装）。NoneSemType 为 Optional 的 none 值放行。
        //   - name=="Iterator"：record→视图转换需 record 显式 impl Iterator
        //     （let it: Iterator<int> = fib），匿名 record 字面量无 impl → 拒绝
        //     （[Iterator<Point>]=[{..}]）；IterSemType 值直通。
        //   - channel/sync.Channel：无隐式转换，裸值拒绝（[channel<int>]=[{..}]）。
        //   - 其余已解析泛型（用户泛型 record 自引用 Tree<U> 等）：保持旧放行——
        //     结构匹配由下游 RecordSemType 分支兜底（record 字面量经
        //     recordTypeFromExpected 从类型别名解出的字段可能含未替换的泛型形参，
        //     直接递归比较会误报，如 used/1.aura Tree<T> children 自引用）。
        if (dynamic_cast<const OptionalSemType*>(&source)) {
            // 窄拦截（problem.txt「匿名 record → 接口视图」条目）：Optional 视图中
            // some({..}) 匿名 record 元素对视图目标不应放行——放行会使 CodeGen 生成
            // designated initializer 坏 C++（expected primary-expression / deduced
            // type 'void'）。仅当 target 是 Optional 泛型且 source 为 OptionalSemType
            //（some() 结构化推断）时，同步剥 target/source 的 Optional 层（防层数
            // 不对称误判），检查最内层是否为「接口视图 tElem + 匿名 record sInner」
            // 非法组合。其余形态（元素为 list / record 变量 / 具体 record）维持旧
            // 放行，不误伤 #6 形态 Optional<[Point]>=some([{..}]) 与 record→Optional。
            if (gt->name == "Optional" && !gt->resolvedName.empty()) {
                std::vector<std::unique_ptr<SemType>> ownedT, ownedS;
                const SemType* tCur = gt;
                const SemType* sCur = &source;
                // 同步剥层：target 经 elemTypeOf 从 resolvedName 提取 <...> 内元素，
                // source 取 OptionalSemType.elementType；任一侧不再是 Optional 即停止。
                // source 侧 Optional 层除 OptionalSemType（some()/折叠推断）外，还可能是
                // GenericSemType{name=="Optional", resolvedName 非空}——显式 Optional<X>
                // 注解变量引用经 materialize 物化（bug-12 扩展：repro_var_carried_2to1，
                // some(Optional<Person> 变量) 内层为 GenericSemType 而非 OptionalSemType）。
                while (true) {
                    auto* tg = dynamic_cast<const GenericSemType*>(tCur);
                    auto* so = dynamic_cast<const OptionalSemType*>(sCur);
                    auto* sg = dynamic_cast<const GenericSemType*>(sCur);
                    bool sIsOptLayer = (so && so->elementType)
                        || (sg && sg->name == "Optional" && !sg->resolvedName.empty());
                    if (!tg || tg->name != "Optional" || tg->resolvedName.empty()
                        || !sIsOptLayer)
                        break;
                    ownedT.push_back(elemTypeOf(tCur));
                    tCur = ownedT.back().get();
                    ownedS.push_back(so ? so->elementType->clone() : elemTypeOf(sCur));
                    sCur = ownedS.back().get();
                }
                // bug-12 Optional 层数校验：source（some 结构化推断）层数 > target
                // （显式 Optional<X> 注解）层数时放行会令 CodeGen 把内层 some 结果当
                // 值装箱 → 坏 C++（Optional<Greetable> = some(some(p)) 2 层 > 1 层）。
                // 同步剥层循环两侧各剥一层（循环次数 = min(两侧层数)）；循环终止后
                // source 若仍残留 Optional 层（OptionalSemType 或 GenericSemType{Optional}
                // 且元素可提取），说明 source 层数 > target 层数 → 拒绝（调用方报干净
                // type mismatch）。相等或更少则继续走下方 P4-7 窄拦截，不误伤。
                if (auto* restSo = dynamic_cast<const OptionalSemType*>(sCur))
                    if (restSo->elementType) return false;
                if (auto* restSg = dynamic_cast<const GenericSemType*>(sCur))
                    if (restSg->name == "Optional" && !restSg->resolvedName.empty())
                        return false;
                // 最内层 target 元素须为接口视图（InterfaceSemType，或泛型接口物化
                // 占位 GenericSemType{name="Cmp<Point*>"}——semTypeFromCppName 对
                // "Cmp<Point*>" 反解失败落入占位，基名 Cmp 查符号表为 Interface）
                bool tIsIface = dynamic_cast<const InterfaceSemType*>(tCur) != nullptr;
                if (!tIsIface) {
                    if (auto* tg = dynamic_cast<const GenericSemType*>(tCur)) {
                        std::string base = tg->name;
                        auto lt = base.find('<');
                        if (lt != std::string::npos) base = base.substr(0, lt);
                        auto* isym = symtab_.lookup(base);
                        tIsIface = isym && isym->kind == SymKind::Interface;
                    }
                }
                if (tIsIface) {
                    // source 若仍有多余 Optional 层，剥到最内层元素
                    const SemType* sInner = sCur;
                    std::vector<std::unique_ptr<SemType>> ownedRest;
                    while (auto* so = dynamic_cast<const OptionalSemType*>(sInner)) {
                        if (!so->elementType) break;
                        ownedRest.push_back(so->elementType->clone());
                        sInner = ownedRest.back().get();
                    }
                    // 匿名 record（canonicalName 空，recordImplIfaces_ 查不到）→ 拦截
                    if (auto* sr = dynamic_cast<const RecordSemType*>(sInner))
                        if (sr->canonicalName.empty()) return false;
                }
            }
            return true;
        }
        if (gt->name == "Optional") {
            if (dynamic_cast<const NoneSemType*>(&source)) return true;
            auto elem = elemTypeOf(gt);
            if (dynamic_cast<const ErrorSemType*>(elem.get()))
                return true;   // 元素类型解析失败：保持旧放行（不误伤）
            return isAssignable(*elem, source);
        }
        if (gt->name == "Iterator") {
            if (dynamic_cast<const IterSemType*>(&source)) return true;
            if (auto* srcRec = dynamic_cast<const RecordSemType*>(&source)) {
                auto it = recordImplIfaces_.find(srcRec->canonicalName);
                if (it != recordImplIfaces_.end() && it->second.count("Iterator"))
                    return true;
            }
            return false;
        }
        if (gt->name == "channel" || gt->name == "sync.Channel") return false;
        // L720 兜底放行前展开结构比较（problem.txt「自引用 record isAssignable 深层
        // 匹配」条目，一并对泛型 Tree<int> 与非泛型 Node 生效）：resolvedName 非空且
        // 符号表查得 TypeAlias→RecordSemType（用户泛型 record 自引用）时，反解并实例化
        // 原始定义做深层结构比较——否则 children 自引用字段保留为 GenericSemType 时
        // 不进入下方 RecordSemType 分支，children 深层字段类型（value: int vs string）
        // 从不比较 → 坏 C++。非 record（Iterator/channel 等内置泛型/接口）返回
        // nullptr → 保持旧放行。递归终止：target 展开为实例化 RecordSemType、source
        // 有限、空列表元素为 Error/None 时 isAssignable 开头/列表分支放行。
        if (auto inst = instantiateUserRecordFromCppName(gt->resolvedName))
            return isAssignable(*inst, source);
        return true;
    }

    // 泛型参数作为 source：查类型别名获取实际类型再做兼容检查
    // 处理递归类型引用（如 Tree<T> 内 children: [Tree<T>]，自引用产生 GenericSemType("Tree")）
    // 仅未解析的泛型形参（resolvedName 空）走别名解析；
    // 已解析泛型（如 Iterator<int32_t>，resolvedName 非空）继续向下走
    // UnionSemType 变体匹配 / equals——否则接口名与泛型同名时（如内置 Iterator
    // 接口经符号表注册为 Interface 符号）会在下方 UnionSemType 分支前被误拦截
    if (auto* gs = dynamic_cast<const GenericSemType*>(&source)) {
        // 仅未解析的泛型形参（resolvedName 空）查类型别名；
        // 已解析泛型（如 Iterator<int32_t>）不在此 return，继续向下走
        // UnionSemType 变体匹配 / equals——否则接口名与泛型同名时
        // （内置 Iterator 接口经符号表注册为 Interface 符号）会在下方被误拦截
        if (gs->resolvedName.empty()) {
            auto* sym = symtab_.lookup(gs->name);
            if (sym && sym->kind == SymKind::TypeAlias && sym->type) {
                return isAssignable(target, *sym->type);
            }
            return false;
        }
    }

    // 联合类型：source 匹配任一变体即为可赋值
    if (auto* u = dynamic_cast<const UnionSemType*>(&target)) {
        // P1-3：source 本身为联合 → 子集判定（源联合 ⊆ 目标联合）。
        // 要求 source 的每个变体都能匹配 target 的某个变体（isAssignable(targetVariant,
        // sourceVariant)），None/Optional 变体经各自分支自然处理。
        // 例：int|None 自赋值通过；int|string 赋 int|None 时 string 无对应变体 → 拒绝。
        // 必须放在单变体匹配之前：否则整源联合作为非 Generic 落入 GenericSemType
        // target 放行洞（420-425，后续独立项）被误放行。
        if (auto* us = dynamic_cast<const UnionSemType*>(&source)) {
            for (auto& sv : us->variants) {
                if (!sv) continue;
                bool matched = false;
                for (auto& tv : u->variants) {
                    if (tv && isAssignable(*tv, *sv)) {
                        matched = true;
                        break;
                    }
                }
                if (!matched) return false;
            }
            return true;
        }
        for (auto& v : u->variants) {
            if (v && isAssignable(*v, source))
                return true;
        }
        // none() 占位（Optional<error>，元素类型未知）→ 联合含 None 变体时视为 None 赋值放行
        if (auto* os = dynamic_cast<const OptionalSemType*>(&source)) {
            if (dynamic_cast<const ErrorSemType*>(os->elementType.get())) {
                for (auto& v : u->variants)
                    if (v && dynamic_cast<const NoneSemType*>(v.get())) return true;
            }
        }
        return false;
    }

    // 列表类型：元素类型兼容即兼容
    if (auto* lt = dynamic_cast<const ListSemType*>(&target)) {
        if (auto* ls = dynamic_cast<const ListSemType*>(&source)) {
            // 元素类型未知（null）时放行，避免空指针解引用（编译期类型未知，运行时验证）
            if (!lt->elementType || !ls->elementType) return true;
            return isAssignable(*lt->elementType, *ls->elementType);
        }
        return false;
    }

    // Optional<T>：双方元素类型需可赋值；Error 元素（none() 占位/未知类型）静默兼容
    // 非 Optional 源：None 值允许；T 值允许隐式包装为 some(T)（T | None → Optional<T> 语法糖）
    if (auto* oa = dynamic_cast<const OptionalSemType*>(&target)) {
        if (auto* ob = dynamic_cast<const OptionalSemType*>(&source)) {
            if (dynamic_cast<const ErrorSemType*>(oa->elementType.get())
                || dynamic_cast<const ErrorSemType*>(ob->elementType.get()))
                return true;
            return isAssignable(*oa->elementType, *ob->elementType);
        }
        if (dynamic_cast<const NoneSemType*>(&source)) return true;
        if (dynamic_cast<const ErrorSemType*>(oa->elementType.get())) return true;
        return isAssignable(*oa->elementType, source);
    }

    // feature-06（阶段 C）：Callable 赋值规则（收窄=编译期报错，v2.1 决策①）
    // 目标为裸 Callable（CallableSemType，erased 或带 origins）：
    //   (a) FuncSemType → Callable：widening ✅（origins 由传播点维护，此处只判可赋性）
    //   (b) Callable → Callable：✅（引用语义拷贝；并集语义 origins 在传播点 join）
    //   (d) record（functor 协议）→ Callable：record 声明 invoke 方法 → ✅
    //       （调用侧 adapt 经 invoke 方法转发）
    //   （其余 source → Callable：int/string 等不可赋）
    if (dynamic_cast<const CallableSemType*>(&target)) {
        if (dynamic_cast<const FuncSemType*>(&source)) return true;
        if (dynamic_cast<const CallableSemType*>(&source)) return true;
        if (auto* rs = dynamic_cast<const RecordSemType*>(&source)) {
            // functor 协议：record 有 invoke 方法（typeMethods_/importedMethods_，
            // 泛型 record 按基名回退——与 inferMethodCall 方法查找同源）
            return findRecordMethod(rs->canonicalName, "invoke") != nullptr;
        }
        return false;
    }
    // 收窄（source=CallableSemType, target=FuncSemType）：`let f: fun(int)->int = all[0]`
    // origins 中每一签名都必须与 target 兼容（matchFuncSig 语义——经 isAssignable 递归）；
    // erased（无溯源）收窄 → 干净拒绝（引导 match 判别/保留 Callable 标注）。
    if (auto* cs = dynamic_cast<const CallableSemType*>(&source)) {
        if (auto* ft = dynamic_cast<const FuncSemType*>(&target)) {
            if (cs->erased()) return false;
            for (auto& o : cs->origins)
                if (o && !isAssignable(*ft, *o)) return false;
            return true;
        }
    }

    // 函数类型：逐参数检查（支持泛型参数）
    if (auto* ft = dynamic_cast<const FuncSemType*>(&target)) {
        if (auto* fs = dynamic_cast<const FuncSemType*>(&source)) {
            return matchFuncSig(ft->paramTypes, ft->returnType.get(), ft->throws,
                               fs->paramTypes, fs->returnType.get(), fs->throws);
        }
        return false;
    }

    // 接口类型：单方法接口可由函数类型（闭包）满足；具体 record 结构匹配（结构类型系统）
    if (auto* iface = dynamic_cast<const InterfaceSemType*>(&target)) {
        // 0. 接口视图 → 接口：同名接口视图值可直接透传（P1 视图按值传参，
        //    视图 { 方法Fn, self } 已由 let/参数构造，直接转发给同接口参数）
        if (auto* si = dynamic_cast<const InterfaceSemType*>(&source))
            return iface->name == si->name;
        // 1. 闭包 → 单方法接口（现有路径保留）
        if (auto* func = dynamic_cast<const FuncSemType*>(&source)) {
            if (iface->methods.size() == 1) {
                auto& m = iface->methods[0];
                return matchFuncSig(m.paramTypes, m.returnType.get(), m.throws,
                                   func->paramTypes, func->returnType.get(), func->throws);
            }
            return false;
        }
        // 2. 具体 record → 必须显式 impl 该接口（无结构匹配）
        //    （impl 方法签名一致性由 checkMethodBody 验证；完整性由 verifyImplCompleteness 验证）
        if (auto* rec = dynamic_cast<const RecordSemType*>(&source)) {
            auto it = recordImplIfaces_.find(rec->canonicalName);   // 空 = 匿名 record
            if (it == recordImplIfaces_.end()) return false;
            return it->second.count(iface->name) > 0;
        }
        return false; // 其他类型不能满足接口
    }

    // 记录类型：结构匹配，用 isAssignable 而非 equals（支持 ErrorSemType / GenericSemType 容错）
    if (auto* rt = dynamic_cast<const RecordSemType*>(&target)) {
        if (auto* rs = dynamic_cast<const RecordSemType*>(&source)) {
            if (rt->fields.size() != rs->fields.size()) return false;
            for (auto& tf : rt->fields) {
                auto it = std::ranges::find_if(rs->fields.begin(), rs->fields.end(),
                    [&](const RecordFieldSem& sf) { return sf.name == tf.name; });
                if (it == rs->fields.end()) return false;
                if (!tf.type || !it->type) return true;  // 字段类型未知：放行
                if (!isAssignable(*tf.type, *it->type)) return false;
            }
            return true;
        }
        // 泛型 record 自引用（Tree<T> 定义内 children 元素为 GenericSemType("Tree", "Tree<T>")）：
        // source 指回类型别名原始定义 → 解析别名做结构比较，否则 Record target 对
        // Generic source 一律拒绝——泛型函数体内递归传参（for child in node.children
        // 再传回 Tree<T> 形参）会误报 type mismatch。
        // 仅 TypeAlias→RecordSemType 生效（用户泛型 record）；Interface 符号（如内置
        // Iterator）不受影响，维持原有 Union 变体匹配路径。
        if (auto* gs = dynamic_cast<const GenericSemType*>(&source)) {
            if (!gs->resolvedName.empty()) {
                auto* sym = symtab_.lookup(gs->name);
                if (sym && sym->kind == SymKind::TypeAlias && sym->type
                    && dynamic_cast<const RecordSemType*>(sym->type.get())) {
                    return isAssignable(target, *sym->type);
                }
            }
        }
        return false;
    }

    // 数值提升（Java 赋值转换语义）：float 接受 int（加宽）；int 不接受 float（收窄）
    // bool/string 基元严格相等（equals），不受影响
    if (auto* tp = dynamic_cast<const PrimSemType*>(&target)) {
        if (auto* sp = dynamic_cast<const PrimSemType*>(&source)) {
            if (tp->kind == PrimSemType::Float && sp->kind == PrimSemType::Int)
                return true;
            return target.equals(source);
        }
    }
    return target.equals(source);
}

} // namespace Aura
