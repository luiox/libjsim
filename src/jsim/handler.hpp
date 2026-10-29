/// @file handler.hpp
/// @brief 栈效应表与指令分类——默认 handler 只保证类型传播（设计 §4）。

#pragma once

#include "jsim/method.hpp"

namespace jsim {

/// 指令类别。
/// Value：值内指令，走「全 Known 算值、否则压正确类型 Unknown」的默认语义。
/// StackOp：槽位重排，handler 直接搬槽位，与 known 无关。
/// Branch：条件分支，操作数全 Known 单边裁决，否则 Bail{NotConcrete} 或分支接管。
/// Switch：tableswitch/lookupswitch，同 Branch 纪律。
/// Alloc：new 系，只走分配脚手架注册面，未注册 Bail。
/// Array：数组指令（newarray/iaload/iastore/arraylength），JVM 指令集语义，
///        走会话堆上的 int 数组脚手架（I64/F64 数组不在 MVP 面）。
/// Field：getstatic/putstatic，走 Session.statics 键值面；未初始化字段按类型
///        传播压 Unknown。
/// BoundaryCall：invoke 系，核心不自带继续语义，只走接管面，未注册即 Bail。
/// Control：无条件 pc 转移（goto）。
/// Return：弹栈收尾，产生 Completed。
enum class InsnKind : u8 {
    Value,
    StackOp,
    Branch,
    Switch,
    Alloc,
    Array,
    Field,
    BoundaryCall,
    Control,
    Return,
};

/// 一条 opcode 的静态效应。MVP 全部 cat1；cat2 记账随 libjbc 接入补全。
struct OpInfo {
    u8 opcode = 0;
    InsnKind kind = InsnKind::Value;
    u8 popSlots = 0;
    u8 pushSlots = 0;
    ValType resultType = ValType::I32; // StackOp/Branch/Switch/Control 忽略
};

/// 表外 opcode 一律 Bail{UnsupportedOpcode}。效应表未满 202 条前这是诚实的
/// 失败，不做半猜测语义（设计 D8/D10）。
inline const OpInfo* infoOf(u8 opcode) {
    static const OpInfo kTable[] = {
        {op::Nop,          InsnKind::Value,        0, 0, ValType::I32},
        {op::AConstNull,   InsnKind::Value,        0, 1, ValType::Ref},
        {op::IConstM1,     InsnKind::Value,        0, 1, ValType::I32},
        {op::IConst0,      InsnKind::Value,        0, 1, ValType::I32},
        {op::IConst1,      InsnKind::Value,        0, 1, ValType::I32},
        {op::IConst2,      InsnKind::Value,        0, 1, ValType::I32},
        {op::IConst3,      InsnKind::Value,        0, 1, ValType::I32},
        {op::IConst4,      InsnKind::Value,        0, 1, ValType::I32},
        {op::IConst5,      InsnKind::Value,        0, 1, ValType::I32},
        {op::LConst0,      InsnKind::Value,        0, 1, ValType::I64},
        {op::LConst1,      InsnKind::Value,        0, 1, ValType::I64},
        {op::Bipush,       InsnKind::Value,        0, 1, ValType::I32},
        {op::Sipush,       InsnKind::Value,        0, 1, ValType::I32},
        // ldc/ldc_w/ldc2_w 统一走 jsim Ldc：实际类型由 cp 条目决定（Int→I32 /
        // Long→I64 / Str→Ref），不走 popAndCompute，表内 resultType 仅占位。
        {op::Ldc,          InsnKind::Value,        0, 1, ValType::I32},
        {op::ILoad,        InsnKind::Value,        0, 1, ValType::I32},
        {op::ALoad,        InsnKind::Value,        0, 1, ValType::Ref},
        {op::IALoad,       InsnKind::Array,        2, 1, ValType::I32},
        {op::IStore,       InsnKind::Value,        1, 0, ValType::I32},
        {op::AStore,       InsnKind::Value,        1, 0, ValType::Ref},
        {op::IAStore,      InsnKind::Array,        3, 0, ValType::I32},
        {op::Iinc,         InsnKind::Value,        0, 0, ValType::I32},
        {op::Pop,          InsnKind::StackOp,      1, 0, ValType::I32},
        {op::Dup,          InsnKind::StackOp,      1, 2, ValType::I32},
        {op::DupX1,        InsnKind::StackOp,      2, 3, ValType::I32},
        {op::DupX2,        InsnKind::StackOp,      3, 4, ValType::I32},
        {op::Dup2,         InsnKind::StackOp,      2, 4, ValType::I32},
        {op::Swap,         InsnKind::StackOp,      2, 2, ValType::I32},
        {op::IAdd,         InsnKind::Value,        2, 1, ValType::I32},
        {op::LAdd,         InsnKind::Value,        2, 1, ValType::I64},
        {op::ISub,         InsnKind::Value,        2, 1, ValType::I32},
        {op::IMul,         InsnKind::Value,        2, 1, ValType::I32},
        {op::INeg,         InsnKind::Value,        1, 1, ValType::I32},
        {op::IShl,         InsnKind::Value,        2, 1, ValType::I32},
        {op::LShl,         InsnKind::Value,        2, 1, ValType::I64},
        {op::IShr,         InsnKind::Value,        2, 1, ValType::I32},
        {op::LShr,         InsnKind::Value,        2, 1, ValType::I64},
        {op::IUshr,        InsnKind::Value,        2, 1, ValType::I32},
        {op::LUshr,        InsnKind::Value,        2, 1, ValType::I64},
        {op::IAnd,         InsnKind::Value,        2, 1, ValType::I32},
        {op::LAnd,         InsnKind::Value,        2, 1, ValType::I64},
        {op::Ior,          InsnKind::Value,        2, 1, ValType::I32},
        {op::Lor,          InsnKind::Value,        2, 1, ValType::I64},
        {op::IXor,         InsnKind::Value,        2, 1, ValType::I32},
        {op::LXor,         InsnKind::Value,        2, 1, ValType::I64},
        {op::I2b,          InsnKind::Value,        1, 1, ValType::I32},
        {op::I2c,          InsnKind::Value,        1, 1, ValType::I32},
        {op::I2s,          InsnKind::Value,        1, 1, ValType::I32},
        {op::I2l,          InsnKind::Value,        1, 1, ValType::I64},
        {op::L2i,          InsnKind::Value,        1, 1, ValType::I32},
        {op::IfEq,         InsnKind::Branch,       1, 0, ValType::I32},
        {op::IfNe,         InsnKind::Branch,       1, 0, ValType::I32},
        {op::IfLt,         InsnKind::Branch,       1, 0, ValType::I32},
        {op::IfGe,         InsnKind::Branch,       1, 0, ValType::I32},
        {op::IfGt,         InsnKind::Branch,       1, 0, ValType::I32},
        {op::IfLe,         InsnKind::Branch,       1, 0, ValType::I32},
        {op::IfIcmpeq,     InsnKind::Branch,       2, 0, ValType::I32},
        {op::IfIcmpne,     InsnKind::Branch,       2, 0, ValType::I32},
        {op::IfIcmplt,     InsnKind::Branch,       2, 0, ValType::I32},
        {op::IfIcmpge,     InsnKind::Branch,       2, 0, ValType::I32},
        {op::IfIcmpgt,     InsnKind::Branch,       2, 0, ValType::I32},
        {op::IfIcmple,     InsnKind::Branch,       2, 0, ValType::I32},
        {op::TableSwitch,  InsnKind::Switch,       1, 0, ValType::I32},
        {op::LookupSwitch, InsnKind::Switch,       1, 0, ValType::I32},
        {op::Goto,         InsnKind::Control,      0, 0, ValType::I32},
        {op::IReturn,      InsnKind::Return,       1, 0, ValType::I32},
        {op::LReturn,      InsnKind::Return,       1, 0, ValType::I64},
        {op::AReturn,      InsnKind::Return,       1, 0, ValType::Ref},
        {op::Return,       InsnKind::Return,       0, 0, ValType::I32},
        {op::GetStatic,    InsnKind::Field,        0, 1, ValType::I32},
        {op::PutStatic,    InsnKind::Field,        1, 0, ValType::I32},
        {op::NewArray,     InsnKind::Array,        1, 1, ValType::Ref},
        {op::ArrayLength,  InsnKind::Array,        1, 1, ValType::I32},
        // invoke 系的实际返回类型由描述符决定（引擎用 CallRef.retType），表内
        // resultType 仅占位；pushSlots 也由接管面与 hasReturn 决定。
        {op::InvokeVirtual,  InsnKind::BoundaryCall, 0, 1, ValType::I32},
        {op::InvokeSpecial,  InsnKind::BoundaryCall, 0, 0, ValType::I32},
        {op::InvokeStatic,   InsnKind::BoundaryCall, 0, 1, ValType::I32},
        {op::New,            InsnKind::Alloc,        0, 1, ValType::Ref},
    };
    for (const OpInfo& entry : kTable) {
        if (entry.opcode == opcode) return &entry;
    }
    return nullptr;
}

/// 观测面用的 opcode 助记名。
inline const char* opcodeName(u8 opcode) {
    switch (opcode) {
    case op::Nop: return "nop";
    case op::AConstNull: return "aconst_null";
    case op::IConstM1: return "iconst_m1";
    case op::IConst0: return "iconst_0";
    case op::IConst1: return "iconst_1";
    case op::IConst2: return "iconst_2";
    case op::IConst3: return "iconst_3";
    case op::IConst4: return "iconst_4";
    case op::IConst5: return "iconst_5";
    case op::LConst0: return "lconst_0";
    case op::LConst1: return "lconst_1";
    case op::Bipush: return "bipush";
    case op::Sipush: return "sipush";
    case op::Ldc: return "ldc";
    case op::ILoad: return "iload";
    case op::ALoad: return "aload";
    case op::IALoad: return "iaload";
    case op::IStore: return "istore";
    case op::AStore: return "astore";
    case op::IAStore: return "iastore";
    case op::Iinc: return "iinc";
    case op::Pop: return "pop";
    case op::Dup: return "dup";
    case op::DupX1: return "dup_x1";
    case op::DupX2: return "dup_x2";
    case op::Dup2: return "dup2";
    case op::Swap: return "swap";
    case op::IAdd: return "iadd";
    case op::LAdd: return "ladd";
    case op::ISub: return "isub";
    case op::IMul: return "imul";
    case op::INeg: return "ineg";
    case op::IShl: return "ishl";
    case op::LShl: return "lshl";
    case op::IShr: return "ishr";
    case op::LShr: return "lshr";
    case op::IUshr: return "iushr";
    case op::LUshr: return "lushr";
    case op::IAnd: return "iand";
    case op::LAnd: return "land";
    case op::Ior: return "ior";
    case op::Lor: return "lor";
    case op::IXor: return "ixor";
    case op::LXor: return "lxor";
    case op::I2b: return "i2b";
    case op::I2c: return "i2c";
    case op::I2s: return "i2s";
    case op::I2l: return "i2l";
    case op::L2i: return "l2i";
    case op::IfEq: return "ifeq";
    case op::IfNe: return "ifne";
    case op::IfLt: return "iflt";
    case op::IfGe: return "ifge";
    case op::IfGt: return "ifgt";
    case op::IfLe: return "ifle";
    case op::IfIcmpeq: return "if_icmpeq";
    case op::IfIcmpne: return "if_icmpne";
    case op::IfIcmplt: return "if_icmplt";
    case op::IfIcmpge: return "if_icmpge";
    case op::IfIcmpgt: return "if_icmpgt";
    case op::IfIcmple: return "if_icmple";
    case op::TableSwitch: return "tableswitch";
    case op::LookupSwitch: return "lookupswitch";
    case op::Goto: return "goto";
    case op::IReturn: return "ireturn";
    case op::LReturn: return "lreturn";
    case op::AReturn: return "areturn";
    case op::Return: return "return";
    case op::GetStatic: return "getstatic";
    case op::PutStatic: return "putstatic";
    case op::NewArray: return "newarray";
    case op::ArrayLength: return "arraylength";
    case op::InvokeVirtual: return "invokevirtual";
    case op::InvokeSpecial: return "invokespecial";
    case op::InvokeStatic: return "invokestatic";
    case op::New: return "new";
    default: return "<表外>";
    }
}

} // namespace jsim
