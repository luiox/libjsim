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
/// Array：数组指令（newarray/anewarray/multianewarray + load/store/length），
///        JVM 指令集语义（不涉及任何 JDK 类）：按元素宽度与符号性建在会话堆上
///        （[B 有符号、[Z/[C 无符号）；multianewarray 全维分配。
/// Field：getstatic/putstatic/getfield/putfield。接管（Session.fieldMocks）优先；
///        未命中走会话存贮底座（statics / 堆 fields），缺项按类型传播压 Unknown。
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
///
/// 表是 dense 索引：一次性把行表摊进 256 槽，`infoOf` 退化成两次数组访问。
/// 线性扫行表在百万级步数下会成为热点（每步 O(行数)），不值得省这点内存。
struct OpTableView
{
    OpInfo entries[256];
    bool   present[256] = {};

    OpTableView()
    {
        // 行表顺序无关；同 opcode 后行覆盖前行（便于按族补齐后再单独改写）。
        const OpInfo rows[] = {
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
            {op::FConst0,      InsnKind::Value,        0, 1, ValType::F32},
            {op::FConst1,      InsnKind::Value,        0, 1, ValType::F32},
            {op::FConst2,      InsnKind::Value,        0, 1, ValType::F32},
            {op::DConst0,      InsnKind::Value,        0, 1, ValType::F64},
            {op::DConst1,      InsnKind::Value,        0, 1, ValType::F64},
            {op::Bipush,       InsnKind::Value,        0, 1, ValType::I32},
            {op::Sipush,       InsnKind::Value,        0, 1, ValType::I32},
            // ldc/ldc_w/ldc2_w 统一走 jsim Ldc：实际类型由 cp 条目决定
            // （Int→I32 / Long→I64 / Str→Ref），不走 popAndCompute，
            // 表内 resultType 仅占位。
            {op::Ldc,          InsnKind::Value,        0, 1, ValType::I32},
            {op::LdcW,         InsnKind::Value,        0, 1, ValType::I32},
            {op::Ldc2W,        InsnKind::Value,        0, 1, ValType::I64},
            {op::ILoad,        InsnKind::Value,        0, 1, ValType::I32},
            {op::LLoad,        InsnKind::Value,        0, 1, ValType::I64},
            {op::FLoad,        InsnKind::Value,        0, 1, ValType::F32},
            {op::DLoad,        InsnKind::Value,        0, 1, ValType::F64},
            {op::ALoad,        InsnKind::Value,        0, 1, ValType::Ref},
            {op::ILoad0,       InsnKind::Value,        0, 1, ValType::I32},
            {op::ILoad0 + 1,   InsnKind::Value,        0, 1, ValType::I32},
            {op::ILoad0 + 2,   InsnKind::Value,        0, 1, ValType::I32},
            {op::ILoad0 + 3,   InsnKind::Value,        0, 1, ValType::I32},
            {op::LLoad0,       InsnKind::Value,        0, 1, ValType::I64},
            {op::LLoad0 + 1,   InsnKind::Value,        0, 1, ValType::I64},
            {op::LLoad0 + 2,   InsnKind::Value,        0, 1, ValType::I64},
            {op::LLoad0 + 3,   InsnKind::Value,        0, 1, ValType::I64},
            {op::FLoad0,       InsnKind::Value,        0, 1, ValType::F32},
            {op::FLoad0 + 1,   InsnKind::Value,        0, 1, ValType::F32},
            {op::FLoad0 + 2,   InsnKind::Value,        0, 1, ValType::F32},
            {op::FLoad0 + 3,   InsnKind::Value,        0, 1, ValType::F32},
            {op::DLoad0,       InsnKind::Value,        0, 1, ValType::F64},
            {op::DLoad0 + 1,   InsnKind::Value,        0, 1, ValType::F64},
            {op::DLoad0 + 2,   InsnKind::Value,        0, 1, ValType::F64},
            {op::DLoad0 + 3,   InsnKind::Value,        0, 1, ValType::F64},
            {op::ALoad0,       InsnKind::Value,        0, 1, ValType::Ref},
            {op::ALoad0 + 1,   InsnKind::Value,        0, 1, ValType::Ref},
            {op::ALoad0 + 2,   InsnKind::Value,        0, 1, ValType::Ref},
            {op::ALoad0 + 3,   InsnKind::Value,        0, 1, ValType::Ref},
            {op::IALoad,       InsnKind::Array,        2, 1, ValType::I32},
            {op::LALoad,       InsnKind::Array,        2, 1, ValType::I64},
            {op::FALoad,       InsnKind::Array,        2, 1, ValType::F32},
            {op::DALoad,       InsnKind::Array,        2, 1, ValType::F64},
            {op::AALoad,       InsnKind::Array,        2, 1, ValType::Ref},
            {op::BALoad,       InsnKind::Array,        2, 1, ValType::I32},
            {op::CALoad,       InsnKind::Array,        2, 1, ValType::I32},
            {op::SALoad,       InsnKind::Array,        2, 1, ValType::I32},
            {op::IStore,       InsnKind::Value,        1, 0, ValType::I32},
            {op::LStore,       InsnKind::Value,        1, 0, ValType::I64},
            {op::FStore,       InsnKind::Value,        1, 0, ValType::F32},
            {op::DStore,       InsnKind::Value,        1, 0, ValType::F64},
            {op::AStore,       InsnKind::Value,        1, 0, ValType::Ref},
            {op::IStore0,      InsnKind::Value,        1, 0, ValType::I32},
            {op::IStore0 + 1,  InsnKind::Value,        1, 0, ValType::I32},
            {op::IStore0 + 2,  InsnKind::Value,        1, 0, ValType::I32},
            {op::IStore0 + 3,  InsnKind::Value,        1, 0, ValType::I32},
            {op::LStore0,      InsnKind::Value,        1, 0, ValType::I64},
            {op::LStore0 + 1,  InsnKind::Value,        1, 0, ValType::I64},
            {op::LStore0 + 2,  InsnKind::Value,        1, 0, ValType::I64},
            {op::LStore0 + 3,  InsnKind::Value,        1, 0, ValType::I64},
            {op::FStore0,      InsnKind::Value,        1, 0, ValType::F32},
            {op::FStore0 + 1,  InsnKind::Value,        1, 0, ValType::F32},
            {op::FStore0 + 2,  InsnKind::Value,        1, 0, ValType::F32},
            {op::FStore0 + 3,  InsnKind::Value,        1, 0, ValType::F32},
            {op::DStore0,      InsnKind::Value,        1, 0, ValType::F64},
            {op::DStore0 + 1,  InsnKind::Value,        1, 0, ValType::F64},
            {op::DStore0 + 2,  InsnKind::Value,        1, 0, ValType::F64},
            {op::DStore0 + 3,  InsnKind::Value,        1, 0, ValType::F64},
            {op::AStore0,      InsnKind::Value,        1, 0, ValType::Ref},
            {op::AStore0 + 1,  InsnKind::Value,        1, 0, ValType::Ref},
            {op::AStore0 + 2,  InsnKind::Value,        1, 0, ValType::Ref},
            {op::AStore0 + 3,  InsnKind::Value,        1, 0, ValType::Ref},
            {op::IAStore,      InsnKind::Array,        3, 0, ValType::I32},
            {op::LAStore,      InsnKind::Array,        3, 0, ValType::I64},
            {op::FAStore,      InsnKind::Array,        3, 0, ValType::F32},
            {op::DAStore,      InsnKind::Array,        3, 0, ValType::F64},
            {op::AAStore,      InsnKind::Array,        3, 0, ValType::Ref},
            {op::BAStore,      InsnKind::Array,        3, 0, ValType::I32},
            {op::CAStore,      InsnKind::Array,        3, 0, ValType::I32},
            {op::SAStore,      InsnKind::Array,        3, 0, ValType::I32},
            {op::Pop,          InsnKind::StackOp,      1, 0, ValType::I32},
            {op::Pop2,         InsnKind::StackOp,      2, 0, ValType::I32},
            {op::Dup,          InsnKind::StackOp,      1, 2, ValType::I32},
            {op::DupX1,        InsnKind::StackOp,      2, 3, ValType::I32},
            {op::DupX2,        InsnKind::StackOp,      3, 4, ValType::I32},
            {op::Dup2,         InsnKind::StackOp,      2, 4, ValType::I32},
            {op::Dup2X1,       InsnKind::StackOp,      3, 5, ValType::I32},
            {op::Dup2X2,       InsnKind::StackOp,      4, 6, ValType::I32},
            {op::Swap,         InsnKind::StackOp,      2, 2, ValType::I32},
            {op::IAdd,         InsnKind::Value,        2, 1, ValType::I32},
            {op::LAdd,         InsnKind::Value,        2, 1, ValType::I64},
            {op::FAdd,         InsnKind::Value,        2, 1, ValType::F32},
            {op::DAdd,         InsnKind::Value,        2, 1, ValType::F64},
            {op::ISub,         InsnKind::Value,        2, 1, ValType::I32},
            {op::LSub,         InsnKind::Value,        2, 1, ValType::I64},
            {op::FSub,         InsnKind::Value,        2, 1, ValType::F32},
            {op::DSub,         InsnKind::Value,        2, 1, ValType::F64},
            {op::IMul,         InsnKind::Value,        2, 1, ValType::I32},
            {op::LMul,         InsnKind::Value,        2, 1, ValType::I64},
            {op::FMul,         InsnKind::Value,        2, 1, ValType::F32},
            {op::DMul,         InsnKind::Value,        2, 1, ValType::F64},
            {op::IDiv,         InsnKind::Value,        2, 1, ValType::I32},
            {op::LDiv,         InsnKind::Value,        2, 1, ValType::I64},
            {op::FDiv,         InsnKind::Value,        2, 1, ValType::F32},
            {op::DDiv,         InsnKind::Value,        2, 1, ValType::F64},
            {op::IRem,         InsnKind::Value,        2, 1, ValType::I32},
            {op::LRem,         InsnKind::Value,        2, 1, ValType::I64},
            {op::FRem,         InsnKind::Value,        2, 1, ValType::F32},
            {op::DRem,         InsnKind::Value,        2, 1, ValType::F64},
            {op::INeg,         InsnKind::Value,        1, 1, ValType::I32},
            {op::LNeg,         InsnKind::Value,        1, 1, ValType::I64},
            {op::FNeg,         InsnKind::Value,        1, 1, ValType::F32},
            {op::DNeg,         InsnKind::Value,        1, 1, ValType::F64},
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
            {op::Iinc,         InsnKind::Value,        0, 0, ValType::I32},
            {op::I2l,          InsnKind::Value,        1, 1, ValType::I64},
            {op::I2f,          InsnKind::Value,        1, 1, ValType::F32},
            {op::I2d,          InsnKind::Value,        1, 1, ValType::F64},
            {op::L2i,          InsnKind::Value,        1, 1, ValType::I32},
            {op::L2f,          InsnKind::Value,        1, 1, ValType::F32},
            {op::L2d,          InsnKind::Value,        1, 1, ValType::F64},
            {op::F2i,          InsnKind::Value,        1, 1, ValType::I32},
            {op::F2l,          InsnKind::Value,        1, 1, ValType::I64},
            {op::F2d,          InsnKind::Value,        1, 1, ValType::F64},
            {op::D2i,          InsnKind::Value,        1, 1, ValType::I32},
            {op::D2l,          InsnKind::Value,        1, 1, ValType::I64},
            {op::D2f,          InsnKind::Value,        1, 1, ValType::F32},
            {op::I2b,          InsnKind::Value,        1, 1, ValType::I32},
            {op::I2c,          InsnKind::Value,        1, 1, ValType::I32},
            {op::I2s,          InsnKind::Value,        1, 1, ValType::I32},
            {op::LCmp,         InsnKind::Value,        2, 1, ValType::I32},
            {op::FCmpL,        InsnKind::Value,        2, 1, ValType::I32},
            {op::FCmpG,        InsnKind::Value,        2, 1, ValType::I32},
            {op::DCmpL,        InsnKind::Value,        2, 1, ValType::I32},
            {op::DCmpG,        InsnKind::Value,        2, 1, ValType::I32},
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
            {op::IfAcmpeq,     InsnKind::Branch,       2, 0, ValType::Ref},
            {op::IfAcmpne,     InsnKind::Branch,       2, 0, ValType::Ref},
            {op::IfNull,       InsnKind::Branch,       1, 0, ValType::Ref},
            {op::IfNonNull,    InsnKind::Branch,       1, 0, ValType::Ref},
            {op::TableSwitch,  InsnKind::Switch,       1, 0, ValType::I32},
            {op::LookupSwitch, InsnKind::Switch,       1, 0, ValType::I32},
            {op::Goto,         InsnKind::Control,      0, 0, ValType::I32},
            {op::IReturn,      InsnKind::Return,       1, 0, ValType::I32},
            {op::LReturn,      InsnKind::Return,       1, 0, ValType::I64},
            {op::FReturn,      InsnKind::Return,       1, 0, ValType::F32},
            {op::DReturn,      InsnKind::Return,       1, 0, ValType::F64},
            {op::AReturn,      InsnKind::Return,       1, 0, ValType::Ref},
            {op::Return,       InsnKind::Return,       0, 0, ValType::I32},
            {op::GetStatic,    InsnKind::Field,        0, 1, ValType::I32},
            {op::PutStatic,    InsnKind::Field,        1, 0, ValType::I32},
            {op::GetField,     InsnKind::Field,        1, 1, ValType::I32},
            {op::PutField,     InsnKind::Field,        2, 0, ValType::I32},
            {op::NewArray,     InsnKind::Array,        1, 1, ValType::Ref},
            {op::ANewArray,    InsnKind::Array,        1, 1, ValType::Ref},
            {op::MultiANewArray, InsnKind::Array,      1, 1, ValType::Ref},
            {op::ArrayLength,  InsnKind::Array,        1, 1, ValType::I32},
            {op::CheckCast,    InsnKind::Value,        1, 1, ValType::Ref},
            {op::InstanceOf,   InsnKind::Value,        1, 1, ValType::I32},
            {op::MonitorEnter, InsnKind::Value,        1, 0, ValType::Ref},
            {op::MonitorExit,  InsnKind::Value,        1, 0, ValType::Ref},
            {op::AThrow,       InsnKind::Value,        1, 0, ValType::Ref},
            // invoke 系的实际返回类型由描述符决定（引擎用 CallRef.retType），
            // 表内 resultType 仅占位；pushSlots 也由接管面与 hasReturn 决定。
            {op::InvokeVirtual,   InsnKind::BoundaryCall, 0, 1, ValType::I32},
            {op::InvokeSpecial,   InsnKind::BoundaryCall, 0, 0, ValType::I32},
            {op::InvokeStatic,    InsnKind::BoundaryCall, 0, 1, ValType::I32},
            {op::InvokeInterface, InsnKind::BoundaryCall, 0, 1, ValType::I32},
            {op::InvokeDynamic,   InsnKind::BoundaryCall, 0, 1, ValType::I32},
            {op::New,             InsnKind::Alloc,        0, 1, ValType::Ref},
        };
        for (const OpInfo& row : rows) {
            entries[row.opcode] = row;
            present[row.opcode] = true;
        }
    }
};

inline const OpInfo* infoOf(u8 opcode) {
    static const OpTableView view;
    return view.present[opcode] ? &view.entries[opcode] : nullptr;
}

/// 观测面用的 opcode 助记名。
inline const char* opcodeName(u8 opcode) {
    // 短形式族（iload_0..aload_3 / istore_0..astore_3）按区间合成名字，不铺 40 个
    // case。返回值指向 thread_local 缓冲，只用于展示。
    static const char* const kLoads[] = {"iload_", "lload_", "fload_", "dload_", "aload_"};
    static const char* const kStores[] = {"istore_", "lstore_", "fstore_", "dstore_", "astore_"};
    thread_local std::string shortName;
    if (opcode >= op::ILoad0 && opcode <= op::ALoad0 + 3) {
        const u8 offset = static_cast<u8>(opcode - op::ILoad0);
        shortName = std::string(kLoads[offset / 4]) + static_cast<char>('0' + offset % 4);
        return shortName.c_str();
    }
    if (opcode >= op::IStore0 && opcode <= op::AStore0 + 3) {
        const u8 offset = static_cast<u8>(opcode - op::IStore0);
        shortName = std::string(kStores[offset / 4]) + static_cast<char>('0' + offset % 4);
        return shortName.c_str();
    }
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
    case op::FConst0: return "fconst_0";
    case op::FConst1: return "fconst_1";
    case op::FConst2: return "fconst_2";
    case op::DConst0: return "dconst_0";
    case op::DConst1: return "dconst_1";
    case op::Bipush: return "bipush";
    case op::Sipush: return "sipush";
    case op::Ldc: return "ldc";
    case op::LdcW: return "ldc_w";
    case op::Ldc2W: return "ldc2_w";
    case op::ILoad: return "iload";
    case op::LLoad: return "lload";
    case op::FLoad: return "fload";
    case op::DLoad: return "dload";
    case op::ALoad: return "aload";
    case op::IALoad: return "iaload";
    case op::LALoad: return "laload";
    case op::FALoad: return "faload";
    case op::DALoad: return "daload";
    case op::AALoad: return "aaload";
    case op::BALoad: return "baload";
    case op::CALoad: return "caload";
    case op::SALoad: return "saload";
    case op::IStore: return "istore";
    case op::LStore: return "lstore";
    case op::FStore: return "fstore";
    case op::DStore: return "dstore";
    case op::AStore: return "astore";
    case op::IAStore: return "iastore";
    case op::LAStore: return "lastore";
    case op::FAStore: return "fastore";
    case op::DAStore: return "dastore";
    case op::AAStore: return "aastore";
    case op::BAStore: return "bastore";
    case op::CAStore: return "castore";
    case op::SAStore: return "sastore";
    case op::Iinc: return "iinc";
    case op::Pop: return "pop";
    case op::Pop2: return "pop2";
    case op::Dup: return "dup";
    case op::DupX1: return "dup_x1";
    case op::DupX2: return "dup_x2";
    case op::Dup2: return "dup2";
    case op::Dup2X1: return "dup2_x1";
    case op::Dup2X2: return "dup2_x2";
    case op::Swap: return "swap";
    case op::IAdd: return "iadd";
    case op::LAdd: return "ladd";
    case op::FAdd: return "fadd";
    case op::DAdd: return "dadd";
    case op::ISub: return "isub";
    case op::LSub: return "lsub";
    case op::FSub: return "fsub";
    case op::DSub: return "dsub";
    case op::IMul: return "imul";
    case op::LMul: return "lmul";
    case op::FMul: return "fmul";
    case op::DMul: return "dmul";
    case op::IDiv: return "idiv";
    case op::LDiv: return "ldiv";
    case op::FDiv: return "fdiv";
    case op::DDiv: return "ddiv";
    case op::IRem: return "irem";
    case op::LRem: return "lrem";
    case op::FRem: return "frem";
    case op::DRem: return "drem";
    case op::INeg: return "ineg";
    case op::LNeg: return "lneg";
    case op::FNeg: return "fneg";
    case op::DNeg: return "dneg";
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
    case op::I2f: return "i2f";
    case op::I2d: return "i2d";
    case op::L2i: return "l2i";
    case op::L2f: return "l2f";
    case op::L2d: return "l2d";
    case op::F2i: return "f2i";
    case op::F2l: return "f2l";
    case op::F2d: return "f2d";
    case op::D2i: return "d2i";
    case op::D2l: return "d2l";
    case op::D2f: return "d2f";
    case op::LCmp: return "lcmp";
    case op::FCmpL: return "fcmpl";
    case op::FCmpG: return "fcmpg";
    case op::DCmpL: return "dcmpl";
    case op::DCmpG: return "dcmpg";
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
    case op::FReturn: return "freturn";
    case op::DReturn: return "dreturn";
    case op::AReturn: return "areturn";
    case op::Return: return "return";
    case op::GetStatic: return "getstatic";
    case op::PutStatic: return "putstatic";
    case op::GetField: return "getfield";
    case op::PutField: return "putfield";
    case op::NewArray: return "newarray";
    case op::ANewArray: return "anewarray";
    case op::MultiANewArray: return "multianewarray";
    case op::ArrayLength: return "arraylength";
    case op::CheckCast: return "checkcast";
    case op::InstanceOf: return "instanceof";
    case op::AThrow: return "athrow";
    case op::MonitorEnter: return "monitorenter";
    case op::MonitorExit: return "monitorexit";
    case op::IfNull: return "ifnull";
    case op::IfNonNull: return "ifnonnull";
    case op::IfAcmpeq: return "if_acmpeq";
    case op::IfAcmpne: return "if_acmpne";
    case op::InvokeVirtual: return "invokevirtual";
    case op::InvokeSpecial: return "invokespecial";
    case op::InvokeStatic: return "invokestatic";
    case op::InvokeInterface: return "invokeinterface";
    case op::InvokeDynamic: return "invokedynamic";
    case op::New: return "new";
    default: return "<表外>";
    }
}

} // namespace jsim
