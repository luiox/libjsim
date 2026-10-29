/// @file value.hpp
/// @brief SimValue 三要素：类型、是否已知、值（设计 §3）。

#pragma once

#include <cstdint>
#include <cstring>
#include <string>

namespace jsim {

using i32 = std::int32_t;
using i64 = std::int64_t;
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using usize = std::size_t;
using f64 = double;

/// 值类型。I32/F32 是 cat1，I64/F64 是 cat2。类型是槽位静态事实，决定槽位形状。
enum class ValType : u8 {
    I32,
    I64,
    F32,
    F64,
    Ref,
};

/// F32 的位型搬运。JVM 的 float 值集按位型定义（含 NaN 规范化的历史包袱），
/// 所以 F32 载荷一律走位型而不是 C++ float——位型相等即值相等，不必碰
/// NaN 的坑。这里是唯一的转换点。
inline u32 floatToBits(float v) {
    u32 bits = 0;
    static_assert(sizeof(float) == sizeof(u32), "float 必须 32 位");
    std::memcpy(&bits, &v, sizeof(bits));
    return bits;
}

inline float bitsToFloat(u32 bits) {
    float v = 0.0f;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

/// F64 的位型搬运：常量池里的 double 是位型，不是数值。
inline u64 doubleToBits(f64 v) {
    u64 bits = 0;
    static_assert(sizeof(double) == sizeof(u64), "double 必须 64 位");
    std::memcpy(&bits, &v, sizeof(bits));
    return bits;
}

inline f64 bitsToDouble(u64 bits) {
    f64 v = 0.0;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

/// 引用载荷的已知形态。null 是已知值（设计 D3），不是未知。
struct RefPayload {
    enum class Kind : u8 {
        Null,
        Str,
        Obj,
    };

    Kind kind = Kind::Null;
    std::string str; // Kind == Str 时有效；MVP 用 std::string 替身，正式版换 libjstr Utf8String 家族
    u64 objId = 0;   // Kind == Obj 时有效，会话内单调序号，0 保留
};

/// SimValue 三要素。Unknown 时载荷不存在——不是另立的 bottom 对象，就是没有
/// （设计 D1）。成因、产生点、诊断一律不进 value（设计 D2）。
struct SimValue {
    ValType type = ValType::I32;
    bool known = false;

    i64 intBits = 0;     // I32/I64 直接存位型；F32 存 floatToIntBits 位型
    f64 realValue = 0.0; // F64 用
    RefPayload ref;      // Ref 用

    static SimValue knownI32(i32 v) {
        SimValue s;
        s.type = ValType::I32;
        s.known = true;
        s.intBits = static_cast<i64>(v);
        return s;
    }

    static SimValue knownI64(i64 v) {
        SimValue s;
        s.type = ValType::I64;
        s.known = true;
        s.intBits = v;
        return s;
    }

    /// F32 已知值：载荷是 floatToBits 位型，不存 C++ float（见 floatToBits 注释）。
    static SimValue knownF32(float v) {
        SimValue s;
        s.type = ValType::F32;
        s.known = true;
        s.intBits = static_cast<i64>(static_cast<u32>(floatToBits(v)));
        return s;
    }

    static SimValue knownF64(f64 v) {
        SimValue s;
        s.type = ValType::F64;
        s.known = true;
        s.realValue = v;
        return s;
    }

    static SimValue unknownOf(ValType t) {
        SimValue s;
        s.type = t;
        s.known = false;
        return s;
    }

    static SimValue knownStr(std::string text) {
        SimValue s;
        s.type = ValType::Ref;
        s.known = true;
        s.ref.kind = RefPayload::Kind::Str;
        s.ref.str = std::move(text);
        return s;
    }

    static SimValue knownNull() {
        SimValue s;
        s.type = ValType::Ref;
        s.known = true;
        s.ref.kind = RefPayload::Kind::Null;
        return s;
    }

    static SimValue knownObj(u64 id) {
        SimValue s;
        s.type = ValType::Ref;
        s.known = true;
        s.ref.kind = RefPayload::Kind::Obj;
        s.ref.objId = id;
        return s;
    }

    /// I32 位型按 JVM 包装语义取 u32 视图（溢出回绕、移位掩码都依赖它）。
    u32 asU32() const {
        return static_cast<u32>(static_cast<std::uint64_t>(intBits));
    }

    /// I64 位型取 u64 视图（cat2 位运算一律走这里）。
    u64 asU64() const {
        return static_cast<std::uint64_t>(intBits);
    }

    /// F32 取回 float：只有位型载荷，没有第二份真值。
    float asF32() const {
        return bitsToFloat(asU32());
    }

    /// F64 取回 double：F64 直接存真值，不经位型。
    f64 asF64() const {
        return realValue;
    }

    /// cat2 判定：I64/F64 在 JVM 帧里占两个槽位，栈指令（pop2/dup2/dup2_x*）
    /// 的分支形态由它决定。本模型一个值一个条目，槽位宽度只体现在这里。
    bool isCat2() const {
        return type == ValType::I64 || type == ValType::F64;
    }
};

/// 观测面用的展示形。诊断属于观测，不属于 value 本身（设计 D2）。
inline std::string toDisplay(const SimValue& v) {
    const char* typeName = "";
    switch (v.type) {
    case ValType::I32: typeName = "I32"; break;
    case ValType::I64: typeName = "I64"; break;
    case ValType::F32: typeName = "F32"; break;
    case ValType::F64: typeName = "F64"; break;
    case ValType::Ref: typeName = "Ref"; break;
    }
    if (!v.known) return std::string("Unknown(") + typeName + ")";
    switch (v.type) {
    case ValType::I32:
        return "Known(I32, " + std::to_string(static_cast<i32>(v.intBits)) + ")";
    case ValType::I64:
        return "Known(I64, " + std::to_string(v.intBits) + ")";
    case ValType::F32:
        return "Known(F32, " + std::to_string(v.asF32()) + ")";
    case ValType::F64:
        return "Known(F64, " + std::to_string(v.realValue) + ")";
    case ValType::Ref:
        switch (v.ref.kind) {
        case RefPayload::Kind::Null: return "Known(Ref, null)";
        case RefPayload::Kind::Str: return "Known(Ref, Str \"" + v.ref.str + "\")";
        case RefPayload::Kind::Obj: return "Known(Ref, Obj#" + std::to_string(v.ref.objId) + ")";
        }
        return "Known(Ref)";
    }
    return "?";
}

} // namespace jsim
