/// @file value.hpp
/// @brief SimValue 三要素：类型、是否已知、值（设计 §3）。

#pragma once

#include <cstdint>
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
        return "Known(F32)";
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
