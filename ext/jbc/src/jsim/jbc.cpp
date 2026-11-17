#include "jsim/jbc.hpp"

#include <unordered_map>
#include <vector>

#include "jbc/core/cp_resolve.hpp"

namespace jsim::jbcbridge {

bool retTypeOfDesc(std::string_view desc, ValType& out, bool& hasReturn) {
    hasReturn = true;
    const usize close = desc.find(')');
    if (close == std::string_view::npos || close + 1 >= desc.size()) return false;
    const char c = desc[close + 1];
    if (c == 'V') {
        hasReturn = false;
        return true;
    }
    switch (c) {
    case 'I': case 'Z': case 'B': case 'C': case 'S': out = ValType::I32; return true;
    case 'J': out = ValType::I64; return true;
    case 'F': out = ValType::F32; return true;
    case 'D': out = ValType::F64; return true;
    case 'L': case '[': out = ValType::Ref; return true;
    default: return false;
    }
}

bool fieldTypeOfDesc(std::string_view desc, ValType& out) {
    if (desc.empty()) return false;
    switch (desc.front()) {
    case 'I': case 'Z': case 'B': case 'C': case 'S': out = ValType::I32; return true;
    case 'J': out = ValType::I64; return true;
    case 'F': out = ValType::F32; return true;
    case 'D': out = ValType::F64; return true;
    case 'L': case '[': out = ValType::Ref; return true;
    default: return false;
    }
}

u64 insnKey(jbc::ir::InsnId id) {
    return (static_cast<u64>(id.slot) << 32) | static_cast<u64>(id.gen);
}

u64 labelKey(jbc::ir::LabelId id) {
    return (static_cast<u64>(id.slot) << 32) | static_cast<u64>(id.gen);
}

bool isLdcFamily(jbc::core::Op op) {
    return op == jbc::core::Op::LDC || op == jbc::core::Op::LDC_W
           || op == jbc::core::Op::LDC2_W;
}

bool ldcStringOf(const jbc::ir::ClassSession& s, const jbc::ir::Insn& in,
                        std::string& out) {
    if (!isLdcFamily(in.op)) return false;
    jbc::ir::ClassSession::ConstSem cst;
    if (!s.constOf(in, cst)) return false;
    if (cst.kind != jbc::ir::ClassSession::ConstSem::Kind::String) return false;
    out = std::string(cst.str);
    return true;
}

bool setLdcString(jbc::ir::ClassSession& s, jbc::ir::CodeDom& dom,
                         jbc::ir::InsnId id, std::string_view text) {
    auto got = dom.get(id);
    if (got.is_err()) return false;
    jbc::ir::Insn* in = got.unwrap();
    if (in == nullptr || !isLdcFamily(in->op)) return false;
    return s.setString(*in, text);
}

Result<Method, BridgeError> toSimMethod(jbc::ir::ClassSession& s,
                                               ca::usize methodIdx) {
    const auto fail = [](BridgeError::Stage stage, std::string detail) {
        return Result<Method, BridgeError>::err({stage, std::move(detail)});
    };

    if (!jbc::ir::hasCode(s, methodIdx)) {
        return fail(BridgeError::Stage::NoCode, "方法无 Code 属性");
    }
    auto promoted = jbc::ir::promoteMethod(s, methodIdx);
    if (promoted.is_err()) {
        return fail(BridgeError::Stage::PromoteFailed, "promoteMethod 失败");
    }
    jbc::ir::CodeUnitRef cu = promoted.unwrap();
    if (!cu) return fail(BridgeError::Stage::PromoteFailed, "promote 产物为空");

    const jbc::ir::ClassUnit& unit = s.unit();
    if (methodIdx >= unit.methods.size()) {
        return fail(BridgeError::Stage::BadDom, "方法下标越界");
    }
    const jbc::ir::MethodUnit& mu = unit.methods[methodIdx];
    const std::vector<jbc::ir::InsnId>& order = cu->dom.order();

    // 指令句柄 → pc 下标。
    std::unordered_map<u64, usize> insnPc;
    insnPc.reserve(order.size() * 2);
    for (usize i = 0; i < order.size(); ++i) insnPc[insnKey(order[i])] = i;

    // 标签 → pc：resolveLabel 折成下标；方法末尾锚折成 code.size()。
    std::unordered_map<u64, usize> labelPc;
    const auto resolveLabelPc = [&](jbc::ir::LabelId label, u32& pc) -> bool {
        const u64 key = labelKey(label);
        auto cached = labelPc.find(key);
        if (cached != labelPc.end()) {
            pc = static_cast<u32>(cached->second);
            return true;
        }
        auto resolved = cu->dom.resolveLabel(label);
        if (resolved.is_err()) return false;
        const jbc::ir::LabelTarget target = resolved.unwrap();
        usize at = order.size();
        if (!target.isEnd()) {
            auto found = insnPc.find(insnKey(target.insn));
            if (found == insnPc.end()) return false;
            at = found->second;
        }
        labelPc.emplace(key, at);
        pc = static_cast<u32>(at);
        return true;
    };

    Method out;
    out.name = std::string(unit.symbols().str(mu.name));
    out.desc = std::string(unit.symbols().str(mu.desc));
    out.maxLocals = cu->maxLocals;
    out.code.reserve(order.size());

    // ldc 族的 backing CP 索引 → jsim 紧凑表下标。
    std::unordered_map<ca::u32, i32> cpToLocal;

    for (usize i = 0; i < order.size(); ++i) {
        const jbc::ir::Insn* in = cu->dom.peek(order[i]);
        if (in == nullptr) return fail(BridgeError::Stage::BadDom, "指令句柄悬垂");

        Insn ji;
        ji.opcode = static_cast<u8>(in->op);

        if (isLdcFamily(in->op)) {
            auto mapped = cpToLocal.find(in->cp);
            if (mapped == cpToLocal.end()) {
                jbc::ir::ClassSession::ConstSem cst;
                if (!s.constOf(*in, cst)) {
                    return fail(BridgeError::Stage::BadConst, "ldc 常量解析失败");
                }
                CpEntry entry;
                using ConstKind = jbc::ir::ClassSession::ConstSem::Kind;
                switch (cst.kind) {
                case ConstKind::Int:
                    entry.kind = CpEntry::Kind::Int;
                    entry.intValue = static_cast<i32>(cst.num);
                    break;
                case ConstKind::Long:
                    entry.kind = CpEntry::Kind::Long;
                    entry.longValue = cst.num;
                    break;
                case ConstKind::Float:
                    // 位型原样（jsim F32 载荷按位型，见 value.hpp）。
                    entry.kind = CpEntry::Kind::Float;
                    entry.intValue = static_cast<i32>(static_cast<u32>(cst.bits));
                    break;
                case ConstKind::Double:
                    entry.kind = CpEntry::Kind::Double;
                    entry.longValue = static_cast<i64>(cst.bits);
                    break;
                case ConstKind::String:
                    entry.kind = CpEntry::Kind::Str;
                    entry.strValue = std::string(cst.str);
                    break;
                case ConstKind::Class:
                    entry.kind = CpEntry::Kind::Class;
                    entry.className = std::string(cst.str);
                    break;
                default:
                    return fail(BridgeError::Stage::BadConst,
                                "ldc 常量种类未桥接（MethodType/MethodHandle/Dynamic）");
                }
                const auto local = static_cast<i32>(out.cp.size());
                out.cp.push_back(std::move(entry));
                mapped = cpToLocal.emplace(in->cp, local).first;
            }
            ji.a = mapped->second;
            out.code.push_back(ji);
            continue;
        }

        switch (in->op) {
        case jbc::core::Op::INVOKEVIRTUAL:
        case jbc::core::Op::INVOKESPECIAL:
        case jbc::core::Op::INVOKESTATIC:
        case jbc::core::Op::INVOKEINTERFACE: {
            jbc::ir::ClassSession::MemberSem mem;
            if (!s.memberOf(*in, mem)) {
                return fail(BridgeError::Stage::BadMemberRef, "调用点引用解析失败");
            }
            CallRef call;
            call.owner = std::string(mem.owner);
            call.name = std::string(mem.name);
            call.desc = std::string(mem.desc);
            bool hasRet = true;
            if (!retTypeOfDesc(call.desc, call.retType, hasRet)) {
                return fail(BridgeError::Stage::BadDesc, "调用点描述符不可解析");
            }
            call.hasReturn = hasRet;
            call.isStatic = in->op == jbc::core::Op::INVOKESTATIC;
            ji.a = static_cast<i32>(out.calls.size());
            out.calls.push_back(std::move(call));
            break;
        }
        case jbc::core::Op::GETSTATIC:
        case jbc::core::Op::PUTSTATIC:
        case jbc::core::Op::GETFIELD:
        case jbc::core::Op::PUTFIELD: {
            jbc::ir::ClassSession::MemberSem mem;
            if (!s.memberOf(*in, mem)) {
                return fail(BridgeError::Stage::BadMemberRef, "字段引用解析失败");
            }
            FieldRef field;
            field.owner = std::string(mem.owner);
            field.name = std::string(mem.name);
            field.desc = std::string(mem.desc);
            if (!fieldTypeOfDesc(field.desc, field.type)) {
                return fail(BridgeError::Stage::BadDesc, "字段类型不支持");
            }
            ji.a = static_cast<i32>(out.fields.size());
            out.fields.push_back(std::move(field));
            break;
        }
        case jbc::core::Op::NEW: {
            std::string_view cls = s.classOf(*in);
            if (cls.empty()) return fail(BridgeError::Stage::BadClass, "类常量解析失败");
            ji.a = static_cast<i32>(out.allocTypes.size());
            out.allocTypes.emplace_back(cls);
            break;
        }
        case jbc::core::Op::ANEWARRAY: {
            // ANEWARRAY 的 CP 类常量是**分量**类型：对象分量为内部名
            //（java/lang/String），数组分量已是描述符形（[Ljava/lang/String;）。
            // engine 堆侧 className 约定描述符形（MULTIANEWARRAY 路径同款：
            // N×'[' + 描述符 elemType）——对象分量在此装 L…;，否则 isRefArray
            // 不识别 → arraylength 恒 0 → 界检探针走错分支。
            std::string_view cls = s.classOf(*in);
            if (cls.empty()) return fail(BridgeError::Stage::BadClass, "类常量解析失败");
            ji.a = static_cast<i32>(out.allocTypes.size());
            out.allocTypes.emplace_back(cls[0] == '[' ? std::string(cls)
                                                      : "L" + std::string(cls) + ";");
            break;
        }
        case jbc::core::Op::MULTIANEWARRAY: {
            // libjbc：cp = 类常量（含全部 '['），a = 维数；jsim 侧要元素类型
            //（剥掉 dims 个 '['）+ b = 维数。
            std::string_view cls = s.classOf(*in);
            const auto dims = static_cast<usize>(in->a);
            if (cls.empty() || cls.size() < dims) {
                return fail(BridgeError::Stage::BadClass, "multianewarray 类名与维数不符");
            }
            for (usize k = 0; k < dims; ++k) {
                if (cls[k] != '[') {
                    return fail(BridgeError::Stage::BadClass, "multianewarray 类名与维数不符");
                }
            }
            ji.a = static_cast<i32>(out.allocTypes.size());
            out.allocTypes.emplace_back(cls.substr(dims));
            ji.b = in->a;
            break;
        }
        case jbc::core::Op::CHECKCAST:
        case jbc::core::Op::INSTANCEOF:
            // jsim 默认语义：checkcast 恒等 / instanceof 压 Unknown，操作数无需落位。
            break;
        case jbc::core::Op::NEWARRAY:
            // libjbc 把 atype 放在 a；jsim 的 newarray 读 b（MethodBuilder 同口径）。
            ji.a = 0;
            ji.b = in->a;
            break;
        case jbc::core::Op::GOTO:
        case jbc::core::Op::IFEQ:
        case jbc::core::Op::IFNE:
        case jbc::core::Op::IFLT:
        case jbc::core::Op::IFGE:
        case jbc::core::Op::IFGT:
        case jbc::core::Op::IFLE:
        case jbc::core::Op::IF_ICMPEQ:
        case jbc::core::Op::IF_ICMPNE:
        case jbc::core::Op::IF_ICMPLT:
        case jbc::core::Op::IF_ICMPGE:
        case jbc::core::Op::IF_ICMPGT:
        case jbc::core::Op::IF_ICMPLE:
        case jbc::core::Op::IF_ACMPEQ:
        case jbc::core::Op::IF_ACMPNE:
        case jbc::core::Op::IFNULL:
        case jbc::core::Op::IFNONNULL: {
            u32 target = 0;
            if (!resolveLabelPc(in->target, target)) {
                return fail(BridgeError::Stage::BadLabel, "分支目标解析失败");
            }
            ji.a = static_cast<i32>(target);
            break;
        }
        case jbc::core::Op::TABLESWITCH: {
            // libjbc：target = default，targets[i] 对应 key = a + i。
            SwitchTable table;
            table.min = in->a;
            table.max = in->a + static_cast<i32>(in->targets.size()) - 1;
            table.targets.reserve(in->targets.size());
            for (const jbc::ir::LabelId label : in->targets) {
                u32 target = 0;
                if (!resolveLabelPc(label, target)) {
                    return fail(BridgeError::Stage::BadLabel, "tableswitch 目标解析失败");
                }
                table.targets.push_back(target);
            }
            if (!resolveLabelPc(in->target, table.defaultPc)) {
                return fail(BridgeError::Stage::BadLabel, "tableswitch default 解析失败");
            }
            ji.a = static_cast<i32>(out.switchTables.size());
            out.switchTables.push_back(std::move(table));
            break;
        }
        case jbc::core::Op::LOOKUPSWITCH: {
            LookupTable table;
            if (in->keys.size() != in->targets.size()) {
                return fail(BridgeError::Stage::BadDom, "lookupswitch 键值表长度不符");
            }
            table.pairs.reserve(in->keys.size());
            for (usize k = 0; k < in->keys.size(); ++k) {
                u32 target = 0;
                if (!resolveLabelPc(in->targets[k], target)) {
                    return fail(BridgeError::Stage::BadLabel, "lookupswitch 目标解析失败");
                }
                table.pairs.emplace_back(in->keys[k], target);
            }
            if (!resolveLabelPc(in->target, table.defaultPc)) {
                return fail(BridgeError::Stage::BadLabel, "lookupswitch default 解析失败");
            }
            ji.a = static_cast<i32>(out.lookupTables.size());
            out.lookupTables.push_back(std::move(table));
            break;
        }
        case jbc::core::Op::INVOKEDYNAMIC: {
            // indy 桥接为普通调用点：CallRef 承 NAT name + 调用点描述符（无
            // owner，indy 栈面没有接收者，全是实参）。bootstrap 链不搬——
            // 「调用点指向谁」属 jsim-indy 组合域（indy.hpp 跑 BSM 折叠）或
            // 消费方接管面（callMocksAtPc 按调用点注册）；桥只保证方法载得
            // 进来、调用点可寻址，语义缺接管时引擎照旧 Bail（fail-closed）。
            jbc::ir::ClassSession::MemberSem mem;
            if (!s.memberOf(*in, mem)
                || mem.kind != jbc::ir::ClassSession::MemberSem::Kind::InvokeDynamic) {
                return fail(BridgeError::Stage::BadMemberRef, "indy 调用点解析失败");
            }
            CallRef call;
            call.name = std::string(mem.name);
            call.desc = std::string(mem.desc);
            bool indyHasRet = true;
            if (!retTypeOfDesc(call.desc, call.retType, indyHasRet)) {
                return fail(BridgeError::Stage::BadDesc, "indy 描述符不可解析");
            }
            call.hasReturn = indyHasRet;
            call.isStatic = true;
            ji.a = static_cast<i32>(out.calls.size());
            out.calls.push_back(std::move(call));
            break;
        }
        case jbc::core::Op::ATHROW:
            // athrow 无操作数；异常表（cu->exc）在指令循环后统一搬运成
            // Method.handlers，派发语义在引擎（engine AThrow case）。
            break;
        case jbc::core::Op::JSR:
        case jbc::core::Op::JSR_W:
        case jbc::core::Op::RET:
            // 子例程模型不进 jsim（旧字节码形态，现代编译器不再产出）。
            return fail(BridgeError::Stage::UnsupportedOp, "jsr/ret 子例程未桥接");
        default:
            // 常量/装载存储/算术/转换/栈操作/iinc/monitor：a/b 原样搬运。
            ji.a = in->a;
            ji.b = in->b;
            break;
        }

        out.code.push_back(ji);
    }

    // 异常表搬运：LabelId 边界折成 pc（end 绑方法末尾标签时折成 code.size()，
    // 与 JVMS end_pc == code_length 同表达）；catchCp 是 CP Class 索引，
    // 0 = any（catch-all / finally），非 0 经 core::resolveClassName 落内部名。
    // 表序即 JVM 派发序（JVMS 4.10.4：第一个匹配的表项胜出），原样保留。
    out.handlers.reserve(cu->exc.size());
    for (const jbc::ir::CodeUnit::Exc& ex : cu->exc) {
        ExRange range;
        if (!resolveLabelPc(ex.start, range.startPc)
            || !resolveLabelPc(ex.end, range.endPc)
            || !resolveLabelPc(ex.handler, range.handlerPc)) {
            return fail(BridgeError::Stage::BadLabel, "异常表边界解析失败");
        }
        if (ex.catchCp != 0) {
            std::string_view cls = jbc::core::resolveClassName(s.backing().cp, ex.catchCp);
            if (cls.empty()) {
                return fail(BridgeError::Stage::BadClass, "异常表 catch 类型解析失败");
            }
            range.catchType = std::string(cls);
        }
        out.handlers.push_back(std::move(range));
    }
    return Result<Method, BridgeError>::ok(std::move(out));
}

} // namespace jsim::jbcbridge
