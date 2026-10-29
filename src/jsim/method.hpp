/// @file method.hpp
/// @brief MVP 方法模型与指令集常量。这是 libjbc CodeUnit 会话面的替身桩，
///        接入后由 promoteMethod 产出的 CodeUnit 换掉本文件，引擎面不变。

#pragma once

#include <string>
#include <utility>
#include <vector>

#include "jsim/value.hpp"

namespace jsim {

// MVP 子集的 JVM opcode 值。用真实 opcode 编号，效应表随接入 libjbc 逐步补全到 202 条。
namespace op {
constexpr u8 Nop = 0x00;
constexpr u8 AConstNull = 0x01;
constexpr u8 IConstM1 = 0x02;
constexpr u8 IConst0 = 0x03;
constexpr u8 IConst1 = 0x04;
constexpr u8 IConst2 = 0x05;
constexpr u8 IConst3 = 0x06;
constexpr u8 IConst4 = 0x07;
constexpr u8 IConst5 = 0x08;
constexpr u8 LConst0 = 0x09;
constexpr u8 LConst1 = 0x0a;
constexpr u8 Bipush = 0x10;
constexpr u8 Sipush = 0x11;
constexpr u8 Ldc = 0x12;
constexpr u8 ILoad = 0x15;
constexpr u8 ALoad = 0x19;
constexpr u8 IALoad = 0x2e;
constexpr u8 IStore = 0x36;
constexpr u8 AStore = 0x4a;
constexpr u8 IAStore = 0x4f;
constexpr u8 Pop = 0x57;
constexpr u8 Dup = 0x59;
constexpr u8 DupX1 = 0x5a;
constexpr u8 DupX2 = 0x5d;
constexpr u8 Dup2 = 0x5c;
constexpr u8 Swap = 0x5f;
constexpr u8 IAdd = 0x60;
constexpr u8 LAdd = 0x61;
constexpr u8 ISub = 0x64;
constexpr u8 IMul = 0x68;
constexpr u8 INeg = 0x74;
constexpr u8 IShl = 0x78;
constexpr u8 LShl = 0x79;
constexpr u8 IShr = 0x7a;
constexpr u8 LShr = 0x7b;
constexpr u8 IUshr = 0x7c;
constexpr u8 LUshr = 0x7d;
constexpr u8 IAnd = 0x7e;
constexpr u8 LAnd = 0x7f;
constexpr u8 Ior = 0x80;
constexpr u8 Lor = 0x81;
constexpr u8 IXor = 0x82;
constexpr u8 LXor = 0x83;
constexpr u8 Iinc = 0x84;
constexpr u8 I2l = 0x85;
constexpr u8 L2i = 0x88;
constexpr u8 I2b = 0x91;
constexpr u8 I2c = 0x92;
constexpr u8 I2s = 0x93;
constexpr u8 Goto = 0xa7;
constexpr u8 IReturn = 0xac;
constexpr u8 LReturn = 0xad;
constexpr u8 AReturn = 0xb0;
constexpr u8 Return = 0xb1;
constexpr u8 GetStatic = 0xb2;
constexpr u8 PutStatic = 0xb3;
constexpr u8 NewArray = 0xbc;
constexpr u8 ArrayLength = 0xbe;
constexpr u8 IfEq = 0x99;
constexpr u8 IfNe = 0x9a;
constexpr u8 IfLt = 0x9b;
constexpr u8 IfGe = 0x9c;
constexpr u8 IfGt = 0x9d;
constexpr u8 IfLe = 0x9e;
constexpr u8 IfIcmpeq = 0x9f;
constexpr u8 IfIcmpne = 0xa0;
constexpr u8 IfIcmplt = 0xa1;
constexpr u8 IfIcmpge = 0xa2;
constexpr u8 IfIcmpgt = 0xa3;
constexpr u8 IfIcmple = 0xa4;
constexpr u8 TableSwitch = 0xaa;
constexpr u8 LookupSwitch = 0xab;
constexpr u8 InvokeVirtual = 0xb6;
constexpr u8 InvokeSpecial = 0xb7;
constexpr u8 InvokeStatic = 0xb8;
constexpr u8 New = 0xbb;
} // namespace op

/// 常量池条目。MVP 收 int / long / string 三类（long 供 LDC2_W 直取）。
struct CpEntry {
    enum class Kind : u8 {
        Int,
        Long,
        Str,
    };

    Kind kind = Kind::Int;
    i32 intValue = 0;
    i64 longValue = 0;
    std::string strValue;
};

/// 调用点引用。invoke 系的操作数指向这里；接管注册按 owner+name+desc 匹配。
struct CallRef {
    std::string owner;
    std::string name;
    std::string desc;
    ValType retType = ValType::I32;
    bool hasReturn = true; // <init> 等 void 调用为 false，引擎不压返回值
};

/// 静态字段引用。getstatic/putstatic 的操作数指向这里；值存在 Session.statics。
struct FieldRef {
    std::string owner;
    std::string name;
    std::string desc;
    ValType type = ValType::I32;
};

/// tableswitch 跳转表。targets 覆盖 [min, max] 逐项对应。
struct SwitchTable {
    i32 min = 0;
    i32 max = 0;
    std::vector<u32> targets;
    u32 defaultPc = 0;
};

/// lookupswitch 跳转表。pairs 为 key → target 的散序对，逐项线性比对。
struct LookupTable {
    std::vector<std::pair<i32, u32>> pairs;
    u32 defaultPc = 0;
};

struct Insn {
    u8 opcode = 0;
    i32 a = 0; // 槽号 / cp 下标 / calls 下标 / 跳转目标 / switchTables/lookupTables 下标 / allocTypes 下标
    i32 b = 0; // iinc 增量等第二操作数
};

/// MVP 方法模型。libjbc 接入后由 CodeUnit 承担，字段一一对应。
struct Method {
    std::string name;
    std::string desc;
    std::vector<Insn> code;
    std::vector<CpEntry> cp;
    std::vector<CallRef> calls;
    std::vector<FieldRef> fields;
    std::vector<std::string> allocTypes;
    std::vector<SwitchTable> switchTables;
    std::vector<LookupTable> lookupTables;
    u32 maxLocals = 0;
};

/// 链式小汇编器，demo 用手搭字节码。
class MethodBuilder {
public:
    MethodBuilder(std::string name, std::string desc, u32 maxLocals)
        : method_{std::move(name), std::move(desc), {}, {}, {}, {}, {}, {}, {}, maxLocals} {}

    MethodBuilder& raw(u8 opcode, i32 a = 0, i32 b = 0) {
        method_.code.push_back({opcode, a, b});
        return *this;
    }

    MethodBuilder& nop() { return raw(op::Nop); }
    MethodBuilder& aconstNull() { return raw(op::AConstNull); }

    MethodBuilder& iconst(i32 v) {
        if (v >= -1 && v <= 5) return raw(static_cast<u8>(op::IConst0 + v));
        return raw(op::Bipush, v);
    }

    MethodBuilder& ldcI(i32 v) {
        jsim::CpEntry entry;
        entry.kind = CpEntry::Kind::Int;
        entry.intValue = v;
        method_.cp.push_back(std::move(entry));
        return raw(op::Ldc, static_cast<i32>(method_.cp.size()) - 1);
    }

    MethodBuilder& ldcStr(std::string text) {
        jsim::CpEntry entry;
        entry.kind = CpEntry::Kind::Str;
        entry.strValue = std::move(text);
        method_.cp.push_back(std::move(entry));
        return raw(op::Ldc, static_cast<i32>(method_.cp.size()) - 1);
    }

    MethodBuilder& iload(u32 slot) { return raw(op::ILoad, static_cast<i32>(slot)); }
    MethodBuilder& aload(u32 slot) { return raw(op::ALoad, static_cast<i32>(slot)); }
    MethodBuilder& istore(u32 slot) { return raw(op::IStore, static_cast<i32>(slot)); }
    MethodBuilder& astore(u32 slot) { return raw(op::AStore, static_cast<i32>(slot)); }
    MethodBuilder& iinc(u32 slot, i32 delta) {
        return raw(op::Iinc, static_cast<i32>(slot), delta);
    }

    MethodBuilder& pop() { return raw(op::Pop); }
    MethodBuilder& pop2() { return raw(op::Pop); }
    MethodBuilder& dup() { return raw(op::Dup); }
    MethodBuilder& dupX1() { return raw(op::DupX1); }
    MethodBuilder& dupX2() { return raw(op::DupX2); }
    MethodBuilder& dup2() { return raw(op::Dup2); }
    MethodBuilder& swap() { return raw(op::Swap); }

    MethodBuilder& iadd() { return raw(op::IAdd); }
    MethodBuilder& isub() { return raw(op::ISub); }
    MethodBuilder& imul() { return raw(op::IMul); }
    MethodBuilder& iand() { return raw(op::IAnd); }
    MethodBuilder& ior() { return raw(op::Ior); }
    MethodBuilder& ixor() { return raw(op::IXor); }
    MethodBuilder& i2c() { return raw(op::I2c); }

    MethodBuilder& gotoPc(u32 target) { return raw(op::Goto, static_cast<i32>(target)); }
    MethodBuilder& ifEq(u32 target) { return raw(op::IfEq, static_cast<i32>(target)); }
    MethodBuilder& ifGe(u32 target) { return raw(op::IfGe, static_cast<i32>(target)); }
    MethodBuilder& ifIcmpeq(u32 target) { return raw(op::IfIcmpeq, static_cast<i32>(target)); }
    MethodBuilder& ifIcmpge(u32 target) { return raw(op::IfIcmpge, static_cast<i32>(target)); }

    /// cases 逐项对应 [min, min + cases.size() - 1]；范围外走 defaultPc。
    MethodBuilder& tableswitch(i32 min, std::vector<u32> cases, u32 defaultPc) {
        SwitchTable table;
        table.min = min;
        table.max = min + static_cast<i32>(cases.size()) - 1;
        table.targets = std::move(cases);
        table.defaultPc = defaultPc;
        method_.switchTables.push_back(std::move(table));
        return raw(op::TableSwitch, static_cast<i32>(method_.switchTables.size()) - 1);
    }

    MethodBuilder& invoke(std::string owner, std::string name, std::string desc,
                          ValType retType, bool hasReturn = true) {
        method_.calls.push_back(
            {std::move(owner), std::move(name), std::move(desc), retType, hasReturn});
        return raw(op::InvokeStatic, static_cast<i32>(method_.calls.size()) - 1);
    }

    MethodBuilder& invokeVirtual(std::string owner, std::string name, std::string desc,
                                 ValType retType, bool hasReturn = true) {
        method_.calls.push_back(
            {std::move(owner), std::move(name), std::move(desc), retType, hasReturn});
        return raw(op::InvokeVirtual, static_cast<i32>(method_.calls.size()) - 1);
    }

    MethodBuilder& invokeSpecial(std::string owner, std::string name, std::string desc) {
        method_.calls.push_back({std::move(owner), std::move(name), std::move(desc),
                                 ValType::I32, false});
        return raw(op::InvokeSpecial, static_cast<i32>(method_.calls.size()) - 1);
    }

    MethodBuilder& newInst(std::string className) {
        method_.allocTypes.push_back(std::move(className));
        return raw(op::New, static_cast<i32>(method_.allocTypes.size()) - 1);
    }

    MethodBuilder& ireturn() { return raw(op::IReturn); }
    MethodBuilder& areturn() { return raw(op::AReturn); }
    MethodBuilder& vreturn() { return raw(op::Return); }

    Method build() { return std::move(method_); }

private:
    Method method_;
};

} // namespace jsim
