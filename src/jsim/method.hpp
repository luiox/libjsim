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
constexpr u8 FConst0 = 0x0b;
constexpr u8 FConst1 = 0x0c;
constexpr u8 FConst2 = 0x0d;
constexpr u8 DConst0 = 0x0e;
constexpr u8 DConst1 = 0x0f;
constexpr u8 Bipush = 0x10;
constexpr u8 Sipush = 0x11;
constexpr u8 Ldc = 0x12;
constexpr u8 LdcW = 0x13;
constexpr u8 Ldc2W = 0x14;
constexpr u8 ILoad = 0x15;
constexpr u8 LLoad = 0x16;
constexpr u8 FLoad = 0x17;
constexpr u8 DLoad = 0x18;
constexpr u8 ALoad = 0x19;
// 短形式 load：iload_0..3 / lload_0..3 / fload_0..3 / dload_0..3 / aload_0..3
// 是 0x1a..0x2d 连续排布，按 (op - 基准) 取槽号。
constexpr u8 ILoad0 = 0x1a;
constexpr u8 LLoad0 = 0x1e;
constexpr u8 FLoad0 = 0x22;
constexpr u8 DLoad0 = 0x26;
constexpr u8 ALoad0 = 0x2a;
constexpr u8 IALoad = 0x2e;
constexpr u8 LALoad = 0x2f;
constexpr u8 FALoad = 0x30;
constexpr u8 DALoad = 0x31;
constexpr u8 AALoad = 0x32;
constexpr u8 BALoad = 0x33;
constexpr u8 CALoad = 0x34;
constexpr u8 SALoad = 0x35;
constexpr u8 IStore = 0x36;
constexpr u8 LStore = 0x37;
constexpr u8 FStore = 0x38;
constexpr u8 DStore = 0x39;
constexpr u8 AStore = 0x3a;
constexpr u8 IStore0 = 0x3b;
constexpr u8 LStore0 = 0x3f;
constexpr u8 FStore0 = 0x43;
constexpr u8 DStore0 = 0x47;
constexpr u8 AStore0 = 0x4b;
constexpr u8 IAStore = 0x4f;
constexpr u8 LAStore = 0x50;
constexpr u8 FAStore = 0x51;
constexpr u8 DAStore = 0x52;
constexpr u8 AAStore = 0x53;
constexpr u8 BAStore = 0x54;
constexpr u8 CAStore = 0x55;
constexpr u8 SAStore = 0x56;
constexpr u8 Pop = 0x57;
constexpr u8 Pop2 = 0x58;
constexpr u8 Dup = 0x59;
constexpr u8 DupX1 = 0x5a;
constexpr u8 DupX2 = 0x5b;
constexpr u8 Dup2 = 0x5c;
constexpr u8 Dup2X1 = 0x5d;
constexpr u8 Dup2X2 = 0x5e;
constexpr u8 Swap = 0x5f;
constexpr u8 IAdd = 0x60;
constexpr u8 LAdd = 0x61;
constexpr u8 FAdd = 0x62;
constexpr u8 DAdd = 0x63;
constexpr u8 ISub = 0x64;
constexpr u8 LSub = 0x65;
constexpr u8 FSub = 0x66;
constexpr u8 DSub = 0x67;
constexpr u8 IMul = 0x68;
constexpr u8 LMul = 0x69;
constexpr u8 FMul = 0x6a;
constexpr u8 DMul = 0x6b;
constexpr u8 IDiv = 0x6c;
constexpr u8 LDiv = 0x6d;
constexpr u8 FDiv = 0x6e;
constexpr u8 DDiv = 0x6f;
constexpr u8 IRem = 0x70;
constexpr u8 LRem = 0x71;
constexpr u8 FRem = 0x72;
constexpr u8 DRem = 0x73;
constexpr u8 INeg = 0x74;
constexpr u8 LNeg = 0x75;
constexpr u8 FNeg = 0x76;
constexpr u8 DNeg = 0x77;
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
constexpr u8 I2f = 0x86;
constexpr u8 I2d = 0x87;
constexpr u8 L2i = 0x88;
constexpr u8 L2f = 0x89;
constexpr u8 L2d = 0x8a;
constexpr u8 F2i = 0x8b;
constexpr u8 F2l = 0x8c;
constexpr u8 F2d = 0x8d;
constexpr u8 D2i = 0x8e;
constexpr u8 D2l = 0x8f;
constexpr u8 D2f = 0x90;
constexpr u8 I2b = 0x91;
constexpr u8 I2c = 0x92;
constexpr u8 I2s = 0x93;
constexpr u8 LCmp = 0x94;
constexpr u8 FCmpL = 0x95;
constexpr u8 FCmpG = 0x96;
constexpr u8 DCmpL = 0x97;
constexpr u8 DCmpG = 0x98;
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
constexpr u8 IfAcmpeq = 0xa5;
constexpr u8 IfAcmpne = 0xa6;
constexpr u8 Goto = 0xa7;
constexpr u8 TableSwitch = 0xaa;
constexpr u8 LookupSwitch = 0xab;
constexpr u8 IReturn = 0xac;
constexpr u8 LReturn = 0xad;
constexpr u8 FReturn = 0xae;
constexpr u8 DReturn = 0xaf;
constexpr u8 AReturn = 0xb0;
constexpr u8 Return = 0xb1;
constexpr u8 GetStatic = 0xb2;
constexpr u8 PutStatic = 0xb3;
constexpr u8 GetField = 0xb4;
constexpr u8 PutField = 0xb5;
constexpr u8 InvokeVirtual = 0xb6;
constexpr u8 InvokeSpecial = 0xb7;
constexpr u8 InvokeStatic = 0xb8;
constexpr u8 InvokeInterface = 0xb9;
constexpr u8 InvokeDynamic = 0xba;
constexpr u8 New = 0xbb;
constexpr u8 NewArray = 0xbc;
constexpr u8 ANewArray = 0xbd;
constexpr u8 ArrayLength = 0xbe;
constexpr u8 AThrow = 0xbf;
constexpr u8 CheckCast = 0xc0;
constexpr u8 InstanceOf = 0xc1;
constexpr u8 MonitorEnter = 0xc2;
constexpr u8 MonitorExit = 0xc3;
constexpr u8 MultiANewArray = 0xc5;
constexpr u8 IfNull = 0xc6;
constexpr u8 IfNonNull = 0xc7;
} // namespace op

/// 常量池条目。MVP 收 int / long / string / class 四类（class 常量供
/// `ldc Type` 用——BSM 里 `getAnnotation(Ljava/lang/Class;)` 的实参就是它）。
struct CpEntry {
    enum class Kind : u8 {
        Int,
        Long,
        Str,
        Class,
    };

    Kind kind = Kind::Int;
    i32 intValue = 0;
    i64 longValue = 0;
    std::string strValue;
    std::string className; // Kind == Class 时有效：内部名，不带 L;
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

    MethodBuilder& ldcLong(i64 v) {
        jsim::CpEntry entry;
        entry.kind = CpEntry::Kind::Long;
        entry.longValue = v;
        method_.cp.push_back(std::move(entry));
        return raw(op::Ldc2W, static_cast<i32>(method_.cp.size()) - 1);
    }

    /// ldc 类常量：压出该类的 Class 对象（与 Class.forName 同一个对象）。
    MethodBuilder& ldcClass(std::string internalName) {
        jsim::CpEntry entry;
        entry.kind = CpEntry::Kind::Class;
        entry.className = std::move(internalName);
        method_.cp.push_back(std::move(entry));
        return raw(op::Ldc, static_cast<i32>(method_.cp.size()) - 1);
    }

    /// newarray：atype 按 JVMS §6.5（4=Z 5=C 6=F 7=D 8=B 9=S 10=I 11=J）。
    MethodBuilder& newArray(i32 atype) { return raw(op::NewArray, 0, atype); }

    MethodBuilder& anewArray(std::string elemType) {
        method_.allocTypes.push_back(std::move(elemType));
        return raw(op::ANewArray, static_cast<i32>(method_.allocTypes.size()) - 1);
    }

    MethodBuilder& arraylength() { return raw(op::ArrayLength); }
    MethodBuilder& iaload() { return raw(op::IALoad); }
    MethodBuilder& iastore() { return raw(op::IAStore); }
    MethodBuilder& aaload() { return raw(op::AALoad); }
    MethodBuilder& aastore() { return raw(op::AAStore); }

    MethodBuilder& iload(u32 slot) { return raw(op::ILoad, static_cast<i32>(slot)); }
    MethodBuilder& aload(u32 slot) { return raw(op::ALoad, static_cast<i32>(slot)); }
    MethodBuilder& istore(u32 slot) { return raw(op::IStore, static_cast<i32>(slot)); }
    MethodBuilder& astore(u32 slot) { return raw(op::AStore, static_cast<i32>(slot)); }
    MethodBuilder& iinc(u32 slot, i32 delta) {
        return raw(op::Iinc, static_cast<i32>(slot), delta);
    }

    MethodBuilder& pop() { return raw(op::Pop); }
    MethodBuilder& pop2() { return raw(op::Pop2); }
    MethodBuilder& dup() { return raw(op::Dup); }
    MethodBuilder& dupX1() { return raw(op::DupX1); }
    MethodBuilder& dupX2() { return raw(op::DupX2); }
    MethodBuilder& dup2() { return raw(op::Dup2); }
    MethodBuilder& dup2X1() { return raw(op::Dup2X1); }
    MethodBuilder& dup2X2() { return raw(op::Dup2X2); }
    MethodBuilder& swap() { return raw(op::Swap); }

    MethodBuilder& iadd() { return raw(op::IAdd); }
    MethodBuilder& isub() { return raw(op::ISub); }
    MethodBuilder& imul() { return raw(op::IMul); }
    MethodBuilder& idiv() { return raw(op::IDiv); }
    MethodBuilder& irem() { return raw(op::IRem); }
    MethodBuilder& iand() { return raw(op::IAnd); }
    MethodBuilder& ior() { return raw(op::Ior); }
    MethodBuilder& ixor() { return raw(op::IXor); }
    MethodBuilder& i2c() { return raw(op::I2c); }
    MethodBuilder& ladd() { return raw(op::LAdd); }
    MethodBuilder& lcmp() { return raw(op::LCmp); }

    MethodBuilder& gotoPc(u32 target) { return raw(op::Goto, static_cast<i32>(target)); }
    MethodBuilder& ifEq(u32 target) { return raw(op::IfEq, static_cast<i32>(target)); }
    MethodBuilder& ifNe(u32 target) { return raw(op::IfNe, static_cast<i32>(target)); }
    MethodBuilder& ifGe(u32 target) { return raw(op::IfGe, static_cast<i32>(target)); }
    MethodBuilder& ifIcmpeq(u32 target) { return raw(op::IfIcmpeq, static_cast<i32>(target)); }
    MethodBuilder& ifIcmpne(u32 target) { return raw(op::IfIcmpne, static_cast<i32>(target)); }
    MethodBuilder& ifIcmpge(u32 target) { return raw(op::IfIcmpge, static_cast<i32>(target)); }
    MethodBuilder& ifIcmplt(u32 target) { return raw(op::IfIcmplt, static_cast<i32>(target)); }

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

    MethodBuilder& invokeInterface(std::string owner, std::string name, std::string desc,
                                   ValType retType, bool hasReturn = true) {
        method_.calls.push_back(
            {std::move(owner), std::move(name), std::move(desc), retType, hasReturn});
        return raw(op::InvokeInterface, static_cast<i32>(method_.calls.size()) - 1);
    }

    MethodBuilder& fieldInsn(u8 opcode, std::string owner, std::string name, std::string desc,
                             ValType type) {
        method_.fields.push_back(
            {std::move(owner), std::move(name), std::move(desc), type});
        return raw(opcode, static_cast<i32>(method_.fields.size()) - 1);
    }

    MethodBuilder& getStatic(std::string owner, std::string name, std::string desc,
                             ValType type) {
        return fieldInsn(op::GetStatic, std::move(owner), std::move(name), std::move(desc), type);
    }

    MethodBuilder& putStatic(std::string owner, std::string name, std::string desc,
                             ValType type) {
        return fieldInsn(op::PutStatic, std::move(owner), std::move(name), std::move(desc), type);
    }

    MethodBuilder& getField(std::string owner, std::string name, std::string desc,
                            ValType type) {
        return fieldInsn(op::GetField, std::move(owner), std::move(name), std::move(desc), type);
    }

    MethodBuilder& putField(std::string owner, std::string name, std::string desc,
                            ValType type) {
        return fieldInsn(op::PutField, std::move(owner), std::move(name), std::move(desc), type);
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
