/// @file jbc.hpp
/// @brief jsim-jbc 扩展包：libjbc 字节码模型 ↔ libjsim 模拟执行的官方桥。
///        jbc dom/instruction 面 → jsim 执行输入（Method）的形状搬运、
///        字符串/LDC 面读写（setString 自动升格口径）、消费方最小接管点。
///
/// 依赖声明：本包依赖外部包 libjbc（jbc/ir 会话面 + core Op），是扩展包
/// 位面的边界胶水（设计 §7）；核心（src/jsim）零依赖红线不动，核心对本包
/// 零反向依赖。与 jsim-str 等 JDK 语义包互不依赖，组合发生在用户侧。
///
/// 字符串口径：classfile 常量是 MUTF-8 字节。桥按家族口径**原样透传**给
/// jsim 载荷——BMP 非零码元下 MUTF-8 与 UTF-8 逐字节相同，jsim-str 按
/// UTF-8 码元读。来自 ldc 字面量的字符串带常量池 intern 身份事实，灌预填
/// 时用 SimValue::internedStr（同一 ldc 字面量在常量池里必然同一对象，
/// if_acmpeq 的相等语义依赖它）。
///
/// 消费方最小接管点：桥只做模型搬运，不注册任何语义。桥出来的 Method 配
/// 普通 jsim::Session 跑，缺的语义照 §6 接管面补——
///   - callMocks / callMocksAtPc：invoke 系（含解密器编排的跨方法接力）；
///   - allocScaffolds：new 系（String 构造两阶段走 jsim-str 的
///     str::installStringAlloc）；
///   - opcodeHandlers：表外 opcode 或默认语义不够时的整条接管；
///   - fieldMocks / branchHooks / callMockResolver：字段注入、分支三态裁决、
///     按接收者解析。
/// 三个注册表始终对消费方开放（透出），桥不包裹、不隐藏。

#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/units.hpp"

#include "jsim/engine.hpp"

namespace jsim {
namespace jbcbridge {

/// 桥接失败：结构化分 stage。桥的纪律是宁可整方法放弃，也不产出
/// 「看起来能跑」的半截方法——任何一步拿不到确定信息就整方法报错。
struct BridgeError {
    enum class Stage : u8 {
        NoCode,         ///< 方法无 Code 属性（abstract/native）
        PromoteFailed,  ///< libjbc promoteMethod 失败
        BadDom,         ///< 指令句柄悬垂 / 方法下标越界
        BadLabel,       ///< 分支或 switch 目标标签解析失败
        BadConst,       ///< ldc 族常量种类不可桥（MethodType/MethodHandle/Dynamic）
        BadMemberRef,   ///< invoke / 字段引用解析失败（CP 结构不符）
        BadDesc,        ///< 方法或字段描述符不可解析
        BadClass,       ///< new 族类常量解析失败
        UnsupportedOp,  ///< v1 明确不桥的指令族（invokedynamic / athrow / jsr / ret）
    };

    Stage stage = Stage::BadDom;
    std::string detail;
};

/// 描述符返回段 → ValType；`V` 置 hasReturn=false（<init> 等 void 调用）。
inline bool retTypeOfDesc(std::string_view desc, ValType& out, bool& hasReturn) {
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

/// 字段描述符首段 → ValType。
inline bool fieldTypeOfDesc(std::string_view desc, ValType& out) {
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

/// 指令句柄 → order() 下标的稳定散列键（owner 在单 DOM 内恒定）。
inline u64 insnKey(jbc::ir::InsnId id) {
    return (static_cast<u64>(id.slot) << 32) | static_cast<u64>(id.gen);
}

inline u64 labelKey(jbc::ir::LabelId id) {
    return (static_cast<u64>(id.slot) << 32) | static_cast<u64>(id.gen);
}

/// ldc 族判定（LDC / LDC_W / LDC2_W）。
inline bool isLdcFamily(jbc::core::Op op) {
    return op == jbc::core::Op::LDC || op == jbc::core::Op::LDC_W
           || op == jbc::core::Op::LDC2_W;
}

/// 读 ldc 族指令装载的字符串常量（MUTF-8 字节按家族口径原样透传）。
/// 非 ldc 族、非 String 常量或解析失败返回 false；命中时 out 收字节，
/// 灌预填用 SimValue::internedStr(out)（见文件头的 intern 身份口径）。
inline bool ldcStringOf(const jbc::ir::ClassSession& s, const jbc::ir::Insn& in,
                        std::string& out) {
    if (!isLdcFamily(in.op)) return false;
    jbc::ir::ClassSession::ConstSem cst;
    if (!s.constOf(in, cst)) return false;
    if (cst.kind != jbc::ir::ClassSession::ConstSem::Kind::String) return false;
    out = std::string(cst.str);
    return true;
}

/// 把模拟还原出的明文写回 ldc（解密改写的出口）。语义改写走 libjbc 的
/// setString：CP 追加去重（intern），新索引超 u1 域自动升格 LDC→LDC_W
/// ——宽度陷阱由 libjbc 挡掉，不留给调用方。非 ldc 族或改写失败返回 false。
inline bool setLdcString(jbc::ir::ClassSession& s, jbc::ir::CodeDom& dom,
                         jbc::ir::InsnId id, std::string_view text) {
    auto got = dom.get(id);
    if (got.is_err()) return false;
    jbc::ir::Insn* in = got.unwrap();
    if (in == nullptr || !isLdcFamily(in->op)) return false;
    return s.setString(*in, text);
}

/// 单方法桥接（形状搬运，无语义解释）：libjbc CodeUnit → jsim::Method。
///
/// 指令序列摊成 pc 下标、标签解析成 pc（方法末尾锚折成 code.size()）、
/// ldc 族 CP 索引重映射成 jsim 紧凑表下标（backing CP 与 jsim 表解耦，
/// 稀疏索引不搬进 jsim）。语义视图全部走 ClassSession 的 constOf /
/// memberOf / classOf——桥不手走常量池。NEWARRAY 的 atype 在 libjbc 的
/// a 槽、jsim 引擎读 b 槽，桥内显式搬运；MULTIANEWARRAY 的元素类型由
/// 类名剥掉 dims 个 '[' 得到，维数落 b。
inline Result<Method, BridgeError> toSimMethod(jbc::ir::ClassSession& s,
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
        case jbc::core::Op::NEW:
        case jbc::core::Op::ANEWARRAY: {
            std::string_view cls = s.classOf(*in);
            if (cls.empty()) return fail(BridgeError::Stage::BadClass, "类常量解析失败");
            ji.a = static_cast<i32>(out.allocTypes.size());
            out.allocTypes.emplace_back(cls);
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
        case jbc::core::Op::INVOKEDYNAMIC:
            // indy 的 bootstrap 语义属 jsim-indy 组合域（设计 §7），桥不猜。
            return fail(BridgeError::Stage::UnsupportedOp, "invokedynamic 未桥接（jsim-indy 组合域）");
        case jbc::core::Op::ATHROW:
            // 异常表派发 v1 不做（设计 §11）。
            return fail(BridgeError::Stage::UnsupportedOp, "athrow 需异常表派发（v1 不做）");
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
    return Result<Method, BridgeError>::ok(std::move(out));
}

} // namespace jbcbridge
} // namespace jsim
