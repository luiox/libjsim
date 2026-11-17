/// @file reflect.hpp
/// @defgroup reflect jsim-reflect 反射语义包
/// @brief jsim-reflect 扩展包：Class.forName / getAnnotation / 注解元素访问器。
///
/// 反射是「方法名就是数据」的调用面：`invokeinterface Booger.d1()` 的语义不在
/// 签名里，而在接收者对象上。本包通过 Session 的兜底解析链识别注解代理。
///
/// 元数据不自己解析：provider 是唯一入口，libjbc 桥、JSON 表、手写脚本都从
/// 这里灌进来。核心与本包都不碰容器格式（设计 D10）。
///
/// 可选接入，核心零依赖本文件：
///   jsim::reflect::install(session, [](const std::string& n) { ... });

#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "jsim/engine.hpp"

namespace jsim {
namespace reflect {

/// 注解元素值的数据形——与 Session 无关，可跨会话复用。
/// 堆对象（数组）在物化时才分配，因此 provider 不必知道 allocId 分配规则。
struct AnnoValue {
    enum class Kind : u8 {
        Int,
        Long,
        Float,
        Double,
        Bool,
        Char,
        Byte,
        Short,
        Str,
        StrArray,
        IntArray,
    };

    Kind kind = Kind::Int;
    i32 i = 0;
    i64 j = 0;
    f64 d = 0.0;
    std::string s;
    std::vector<std::string> strs;
    std::vector<i32> ints;

    static AnnoValue int32(i32 v) {
        AnnoValue a;
        a.kind = Kind::Int;
        a.i = v;
        return a;
    }
    static AnnoValue int64(i64 v) {
        AnnoValue a;
        a.kind = Kind::Long;
        a.j = v;
        return a;
    }
    static AnnoValue boolean(bool v) {
        AnnoValue a;
        a.kind = Kind::Bool;
        a.i = v ? 1 : 0;
        return a;
    }
    static AnnoValue text(std::string v) {
        AnnoValue a;
        a.kind = Kind::Str;
        a.s = std::move(v);
        return a;
    }
    static AnnoValue textArray(std::vector<std::string> v) {
        AnnoValue a;
        a.kind = Kind::StrArray;
        a.strs = std::move(v);
        return a;
    }
    static AnnoValue intArray(std::vector<i32> v) {
        AnnoValue a;
        a.kind = Kind::IntArray;
        a.ints = std::move(v);
        return a;
    }

    /// 注解元素在字节码里就是一个无参有返回值的方法，描述符由值类型决定。
    const char* accessorDesc() const {
        switch (kind) {
        case Kind::Int: return "()I";
        case Kind::Long: return "()J";
        case Kind::Float: return "()F";
        case Kind::Double: return "()D";
        case Kind::Bool: return "()Z";
        case Kind::Char: return "()C";
        case Kind::Byte: return "()B";
        case Kind::Short: return "()S";
        case Kind::Str: return "()Ljava/lang/String;";
        case Kind::StrArray: return "()[Ljava/lang/String;";
        case Kind::IntArray: return "()[I";
        }
        return "()I";
    }
};

/// 一个注解实例：类型 + 元素表。元素顺序即声明顺序（访问器无顺序语义）。
struct AnnotationData {
    std::string type; // 内部名，如 "dev/realme/Booger"（不带 L;）
    std::vector<std::pair<std::string, AnnoValue>> elements;
};

/// 类元数据。provider 只需回答「这个类有哪些注解」——反射面用到的就这些。
struct ClassMeta {
    std::string internalName;
    std::vector<AnnotationData> annotations;
};

using MetaProvider = std::function<const ClassMeta*(const std::string& internalName)>;

/// 安装反射接管，元数据 provider 由 Session 的接管持有。
void install(Session& session, MetaProvider provider);

} // namespace reflect
} // namespace jsim
