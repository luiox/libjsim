/// @file jni.hpp
/// @brief jsim-jni 扩展包纯层(设计 docs/ext-jni.md):本地方法接管的接缝与
///        常用 mock 骨架。
///
/// j2c 混淆器把 Java 方法编成 native,体内经 JNIEnv 函数表回调 JNI。本包把这条
/// 链接进 jsim:模拟字节码 invoke 到 native 方法时移交本机执行器(unicorn 绑定
/// 层 jni_uc.hpp 或测试 FakeMachine)跑机器码,机器码调 JNI 时经 dispatch 转接
/// 到钩子——用户钩子优先,缺省走包内骨架,都没有则 Bail{UnsupportedCall},绝不
/// 静默编造语义。
///
/// 分层与依赖:
/// - 本文件零外部依赖,可独立测试(FakeMachine 预编脚本驱动 dispatch);
/// - JNI 函数表按 JDK 8 冻结布局(233 项,索引 0..3 保留),这是 j2c 家族的
///   固定锚点:锚 ABI,不锚混淆器形态;
/// - 句柄池、字段/类桥接、异常位、观测口径见 docs/ext-jni.md。
///
/// 接入:
///   jsim::jni::JniEnv env;
///   env.session = &session;
///   env.mem = ...;                  // GuestMemory 实现(绑定层或测试)
///   env.executor.run = ...;         // 本机执行器(绑定层或测试)
///   env.methods["sample/D.f()I"] = {entry, abi};   // 键 = 内部名.方法名+desc
///   jsim::jni::install(session, env);
#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "jsim/engine.hpp"

namespace jsim {
namespace jni {

using u16 = std::uint16_t;

/// guest 地址约定。绑定层按它物化页;纯层只用它发号与判断。消费方可整体改。
struct Layout
{
    /// JNIEnv*:native 收到的 env 值。真 JNI 是双重间接——env 指向一个结构体,
    /// 其首成员才是函数表(native 代码统一走 (*env)->Fn),物化时 [envTable]
    /// = funcTable,与 HotSpot 行为一致(真机对拍腿验证)。
    u64 envTable = 0x00300000;
    u64 funcTable = 0x00301000; ///< 函数指针表,233 项 × 8 字节,项值指向 stub
    u64 trapPage = 0x00302000;  ///< stub 页:表第 i 项 = trapPage + i * kStubStride
    u64 scratch  = 0x00303000;  ///< 物化区(GetStringUTFChars / 数组副本)
    usize scratchSize = 1u << 20;
    u64 stackBase = 0x00700000; ///< native 运行栈(绑定层灌)
    usize stackSize = 1u << 16;
    u64 sentinel = 0x00F00000;  ///< 假返回地址页,命中即停机
    static constexpr usize kStubStride = 8;
};

/// 本机内存面:纯层唯一面向执行器的口子。FakeMachine 与 unicorn 绑定各给一份。
struct GuestMemory
{
    /// 物化 size 字节(对齐 align),返回实际地址与实际大小。耗尽 Bail{NotConcrete}。
    std::function<Result<std::pair<u64, usize>, Bail>(usize size, usize align)> map;
    std::function<Result<void, Bail>(u64 addr, const void* src, usize n)> write;
    std::function<Result<void, Bail>(u64 addr, void* dst, usize n)> read;
    /// 回收物化副本(Release* 系);不支持回收的实现可空实现(容忍泄漏)。
    std::function<Result<void, Bail>(u64 addr)> free;
};

/// native 方法登记。包不做二进制加载:entry 是**映射后**的 guest 地址,消费方
/// 给布局与入口(手工填表,或模拟 JNI_OnLoad 时由 RegisterNatives 骨架录表)。
struct NativeSpec
{
    enum class Abi : u8 { Win64, SysV }; ///< AArch64 留扩展位

    u64 entry = 0;
    bool isStatic = false; ///< 登记信息;实参弹出以 callee(CallRef)为准
    Abi abi = Abi::Win64;
};

/// 一次编组好的 native 调用。args[0] = 接收者句柄(非 static)或 jclass 句柄
/// (static),其余按 desc 序的位型实参;JNIEnv* 由执行器按 Layout 自注入。
/// argIsFloat 与 args[1..] 对齐:执行器据此分浮点寄存器文件(SysV/Win64 非变长)。
struct NativeRun
{
    const NativeSpec* spec = nullptr;
    ValType retType = ValType::I32;
    bool hasReturn = true;
    std::vector<u64> args;
    std::vector<char> argIsFloat;
};

/// 本机执行器:绑定层给 unicorn 实现,测试给 FakeMachine。未接即 Bail。
struct JniEnv;

struct NativeExecutor
{
    std::function<Result<u64, Bail>(JniEnv&, const NativeRun&)> run;
};

// ---------------------------------------------------------------------------
// JNI 函数表(JDK 8 冻结布局,233 项)。索引是 ABI 事实:机器码按表内偏移取
// 函数指针,dispatch 按 (addr - trapBase) / kStubStride 还原函数号,两端必须
// 同一张表。测试钉住关键索引。
// ---------------------------------------------------------------------------

constexpr usize kFuncCount = 233;

struct FuncMeta
{
    enum class Ret : u8 { Void, Int, Ptr, F32, F64 };

    const char* name = nullptr; ///< nullptr = 保留槽
    u8 argc = 0;                ///< 除 JNIEnv* 外的定长参数个数(变长尾不计)
    Ret ret = Ret::Int;         ///< 定长函数无浮点返回;浮点只在 bare Call* 变体
};

/// 每行:(索引, 名, 定长参数个数, 返回类)。顺序即 JDK 8 jni.h 结构序。
#define JSIM_JNI_FUNCTIONS(X)                                                                                       \
    /* 0..3 保留 */                                                                                                  \
    X(4,   GetVersion, 0, Int)                                                                                      \
    X(5,   DefineClass, 4, Ptr)                                                                                     \
    X(6,   FindClass, 1, Ptr)                                                                                       \
    X(7,   FromReflectedMethod, 1, Ptr)                                                                             \
    X(8,   FromReflectedField, 1, Ptr)                                                                              \
    X(9,   ToReflectedMethod, 3, Ptr)                                                                               \
    X(10,  GetSuperclass, 1, Ptr)                                                                                   \
    X(11,  IsAssignableFrom, 2, Int)                                                                                \
    X(12,  ToReflectedField, 3, Ptr)                                                                                \
    X(13,  Throw, 1, Int)                                                                                           \
    X(14,  ThrowNew, 2, Int)                                                                                        \
    X(15,  ExceptionOccurred, 0, Ptr)                                                                               \
    X(16,  ExceptionDescribe, 0, Void)                                                                              \
    X(17,  ExceptionClear, 0, Void)                                                                                 \
    X(18,  FatalError, 1, Void)                                                                                     \
    X(19,  PushLocalFrame, 1, Int)                                                                                  \
    X(20,  PopLocalFrame, 1, Ptr)                                                                                   \
    X(21,  NewGlobalRef, 1, Ptr)                                                                                    \
    X(22,  DeleteGlobalRef, 1, Void)                                                                                \
    X(23,  DeleteLocalRef, 1, Void)                                                                                 \
    X(24,  IsSameObject, 2, Int)                                                                                    \
    X(25,  NewLocalRef, 1, Ptr)                                                                                     \
    X(26,  EnsureLocalCapacity, 1, Int)                                                                             \
    X(27,  AllocObject, 1, Ptr)                                                                                     \
    X(28,  NewObject, 2, Ptr)                                                                                       \
    X(29,  NewObjectV, 3, Ptr)                                                                                      \
    X(30,  NewObjectA, 3, Ptr)                                                                                      \
    X(31,  GetObjectClass, 1, Ptr)                                                                                  \
    X(32,  IsInstanceOf, 2, Int)                                                                                    \
    X(33,  GetMethodID, 3, Ptr)                                                                                     \
    X(34,  CallObjectMethod, 2, Ptr)                                                                                \
    X(35,  CallObjectMethodV, 3, Ptr)                                                                               \
    X(36,  CallObjectMethodA, 3, Ptr)                                                                               \
    X(37,  CallBooleanMethod, 2, Int)                                                                               \
    X(38,  CallBooleanMethodV, 3, Int)                                                                              \
    X(39,  CallBooleanMethodA, 3, Int)                                                                              \
    X(40,  CallByteMethod, 2, Int)                                                                                  \
    X(41,  CallByteMethodV, 3, Int)                                                                                 \
    X(42,  CallByteMethodA, 3, Int)                                                                                 \
    X(43,  CallCharMethod, 2, Int)                                                                                  \
    X(44,  CallCharMethodV, 3, Int)                                                                                 \
    X(45,  CallCharMethodA, 3, Int)                                                                                 \
    X(46,  CallShortMethod, 2, Int)                                                                                 \
    X(47,  CallShortMethodV, 3, Int)                                                                                \
    X(48,  CallShortMethodA, 3, Int)                                                                                \
    X(49,  CallIntMethod, 2, Int)                                                                                   \
    X(50,  CallIntMethodV, 3, Int)                                                                                  \
    X(51,  CallIntMethodA, 3, Int)                                                                                  \
    X(52,  CallLongMethod, 2, Int)                                                                                  \
    X(53,  CallLongMethodV, 3, Int)                                                                                 \
    X(54,  CallLongMethodA, 3, Int)                                                                                 \
    X(55,  CallFloatMethod, 2, F32)                                                                                 \
    X(56,  CallFloatMethodV, 3, F32)                                                                                \
    X(57,  CallFloatMethodA, 3, F32)                                                                                \
    X(58,  CallDoubleMethod, 2, F64)                                                                                \
    X(59,  CallDoubleMethodV, 3, F64)                                                                               \
    X(60,  CallDoubleMethodA, 3, F64)                                                                               \
    X(61,  CallVoidMethod, 2, Void)                                                                                 \
    X(62,  CallVoidMethodV, 3, Void)                                                                                \
    X(63,  CallVoidMethodA, 3, Void)                                                                                \
    X(64,  CallNonvirtualObjectMethod, 3, Ptr)                                                                      \
    X(65,  CallNonvirtualObjectMethodV, 4, Ptr)                                                                     \
    X(66,  CallNonvirtualObjectMethodA, 4, Ptr)                                                                     \
    X(67,  CallNonvirtualBooleanMethod, 3, Int)                                                                     \
    X(68,  CallNonvirtualBooleanMethodV, 4, Int)                                                                    \
    X(69,  CallNonvirtualBooleanMethodA, 4, Int)                                                                    \
    X(70,  CallNonvirtualByteMethod, 3, Int)                                                                        \
    X(71,  CallNonvirtualByteMethodV, 4, Int)                                                                       \
    X(72,  CallNonvirtualByteMethodA, 4, Int)                                                                       \
    X(73,  CallNonvirtualCharMethod, 3, Int)                                                                        \
    X(74,  CallNonvirtualCharMethodV, 4, Int)                                                                       \
    X(75,  CallNonvirtualCharMethodA, 4, Int)                                                                       \
    X(76,  CallNonvirtualShortMethod, 3, Int)                                                                       \
    X(77,  CallNonvirtualShortMethodV, 4, Int)                                                                      \
    X(78,  CallNonvirtualShortMethodA, 4, Int)                                                                      \
    X(79,  CallNonvirtualIntMethod, 3, Int)                                                                         \
    X(80,  CallNonvirtualIntMethodV, 4, Int)                                                                        \
    X(81,  CallNonvirtualIntMethodA, 4, Int)                                                                        \
    X(82,  CallNonvirtualLongMethod, 3, Int)                                                                        \
    X(83,  CallNonvirtualLongMethodV, 4, Int)                                                                       \
    X(84,  CallNonvirtualLongMethodA, 4, Int)                                                                       \
    X(85,  CallNonvirtualFloatMethod, 3, F32)                                                                       \
    X(86,  CallNonvirtualFloatMethodV, 4, F32)                                                                      \
    X(87,  CallNonvirtualFloatMethodA, 4, F32)                                                                      \
    X(88,  CallNonvirtualDoubleMethod, 3, F64)                                                                      \
    X(89,  CallNonvirtualDoubleMethodV, 4, F64)                                                                     \
    X(90,  CallNonvirtualDoubleMethodA, 4, F64)                                                                     \
    X(91,  CallNonvirtualVoidMethod, 3, Void)                                                                       \
    X(92,  CallNonvirtualVoidMethodV, 4, Void)                                                                      \
    X(93,  CallNonvirtualVoidMethodA, 4, Void)                                                                      \
    X(94,  GetFieldID, 3, Ptr)                                                                                      \
    X(95,  GetObjectField, 2, Ptr)                                                                                  \
    X(96,  GetBooleanField, 2, Int)                                                                                 \
    X(97,  GetByteField, 2, Int)                                                                                    \
    X(98,  GetCharField, 2, Int)                                                                                    \
    X(99,  GetShortField, 2, Int)                                                                                   \
    X(100, GetIntField, 2, Int)                                                                                     \
    X(101, GetLongField, 2, Int)                                                                                    \
    X(102, GetFloatField, 2, F32)                                                                                   \
    X(103, GetDoubleField, 2, F64)                                                                                  \
    X(104, SetObjectField, 3, Void)                                                                                 \
    X(105, SetBooleanField, 3, Void)                                                                                \
    X(106, SetByteField, 3, Void)                                                                                   \
    X(107, SetCharField, 3, Void)                                                                                   \
    X(108, SetShortField, 3, Void)                                                                                  \
    X(109, SetIntField, 3, Void)                                                                                    \
    X(110, SetLongField, 3, Void)                                                                                   \
    X(111, SetFloatField, 3, Void)                                                                                  \
    X(112, SetDoubleField, 3, Void)                                                                                 \
    X(113, GetStaticMethodID, 3, Ptr)                                                                               \
    X(114, CallStaticObjectMethod, 2, Ptr)                                                                          \
    X(115, CallStaticObjectMethodV, 3, Ptr)                                                                         \
    X(116, CallStaticObjectMethodA, 3, Ptr)                                                                         \
    X(117, CallStaticBooleanMethod, 2, Int)                                                                         \
    X(118, CallStaticBooleanMethodV, 3, Int)                                                                        \
    X(119, CallStaticBooleanMethodA, 3, Int)                                                                        \
    X(120, CallStaticByteMethod, 2, Int)                                                                            \
    X(121, CallStaticByteMethodV, 3, Int)                                                                           \
    X(122, CallStaticByteMethodA, 3, Int)                                                                           \
    X(123, CallStaticCharMethod, 2, Int)                                                                            \
    X(124, CallStaticCharMethodV, 3, Int)                                                                           \
    X(125, CallStaticCharMethodA, 3, Int)                                                                           \
    X(126, CallStaticShortMethod, 2, Int)                                                                           \
    X(127, CallStaticShortMethodV, 3, Int)                                                                          \
    X(128, CallStaticShortMethodA, 3, Int)                                                                          \
    X(129, CallStaticIntMethod, 2, Int)                                                                             \
    X(130, CallStaticIntMethodV, 3, Int)                                                                            \
    X(131, CallStaticIntMethodA, 3, Int)                                                                            \
    X(132, CallStaticLongMethod, 2, Int)                                                                            \
    X(133, CallStaticLongMethodV, 3, Int)                                                                           \
    X(134, CallStaticLongMethodA, 3, Int)                                                                           \
    X(135, CallStaticFloatMethod, 2, F32)                                                                           \
    X(136, CallStaticFloatMethodV, 3, F32)                                                                          \
    X(137, CallStaticFloatMethodA, 3, F32)                                                                          \
    X(138, CallStaticDoubleMethod, 2, F64)                                                                          \
    X(139, CallStaticDoubleMethodV, 3, F64)                                                                         \
    X(140, CallStaticDoubleMethodA, 3, F64)                                                                         \
    X(141, CallStaticVoidMethod, 2, Void)                                                                           \
    X(142, CallStaticVoidMethodV, 3, Void)                                                                          \
    X(143, CallStaticVoidMethodA, 3, Void)                                                                          \
    X(144, GetStaticFieldID, 3, Ptr)                                                                                \
    X(145, GetStaticObjectField, 2, Ptr)                                                                            \
    X(146, GetStaticBooleanField, 2, Int)                                                                           \
    X(147, GetStaticByteField, 2, Int)                                                                              \
    X(148, GetStaticCharField, 2, Int)                                                                              \
    X(149, GetStaticShortField, 2, Int)                                                                             \
    X(150, GetStaticIntField, 2, Int)                                                                               \
    X(151, GetStaticLongField, 2, Int)                                                                              \
    X(152, GetStaticFloatField, 2, F32)                                                                             \
    X(153, GetStaticDoubleField, 2, F64)                                                                            \
    X(154, SetStaticObjectField, 3, Void)                                                                           \
    X(155, SetStaticBooleanField, 3, Void)                                                                          \
    X(156, SetStaticByteField, 3, Void)                                                                             \
    X(157, SetStaticCharField, 3, Void)                                                                             \
    X(158, SetStaticShortField, 3, Void)                                                                            \
    X(159, SetStaticIntField, 3, Void)                                                                              \
    X(160, SetStaticLongField, 3, Void)                                                                             \
    X(161, SetStaticFloatField, 3, Void)                                                                            \
    X(162, SetStaticDoubleField, 3, Void)                                                                           \
    X(163, NewString, 2, Ptr)                                                                                       \
    X(164, GetStringLength, 1, Int)                                                                                 \
    X(165, GetStringChars, 2, Ptr)                                                                                  \
    X(166, ReleaseStringChars, 2, Void)                                                                             \
    X(167, NewStringUTF, 1, Ptr)                                                                                    \
    X(168, GetStringUTFLength, 1, Int)                                                                              \
    X(169, GetStringUTFChars, 2, Ptr)                                                                               \
    X(170, ReleaseStringUTFChars, 2, Void)                                                                          \
    X(171, GetArrayLength, 1, Int)                                                                                  \
    X(172, NewObjectArray, 3, Ptr)                                                                                  \
    X(173, GetObjectArrayElement, 2, Ptr)                                                                           \
    X(174, SetObjectArrayElement, 3, Void)                                                                          \
    X(175, NewBooleanArray, 1, Ptr)                                                                                 \
    X(176, NewByteArray, 1, Ptr)                                                                                    \
    X(177, NewCharArray, 1, Ptr)                                                                                    \
    X(178, NewShortArray, 1, Ptr)                                                                                   \
    X(179, NewIntArray, 1, Ptr)                                                                                     \
    X(180, NewLongArray, 1, Ptr)                                                                                    \
    X(181, NewFloatArray, 1, Ptr)                                                                                   \
    X(182, NewDoubleArray, 1, Ptr)                                                                                  \
    X(183, GetBooleanArrayElements, 2, Ptr)                                                                         \
    X(184, GetByteArrayElements, 2, Ptr)                                                                            \
    X(185, GetCharArrayElements, 2, Ptr)                                                                            \
    X(186, GetShortArrayElements, 2, Ptr)                                                                           \
    X(187, GetIntArrayElements, 2, Ptr)                                                                             \
    X(188, GetLongArrayElements, 2, Ptr)                                                                            \
    X(189, GetFloatArrayElements, 2, Ptr)                                                                           \
    X(190, GetDoubleArrayElements, 2, Ptr)                                                                          \
    X(191, ReleaseBooleanArrayElements, 3, Void)                                                                    \
    X(192, ReleaseByteArrayElements, 3, Void)                                                                       \
    X(193, ReleaseCharArrayElements, 3, Void)                                                                       \
    X(194, ReleaseShortArrayElements, 3, Void)                                                                      \
    X(195, ReleaseIntArrayElements, 3, Void)                                                                        \
    X(196, ReleaseLongArrayElements, 3, Void)                                                                       \
    X(197, ReleaseFloatArrayElements, 3, Void)                                                                      \
    X(198, ReleaseDoubleArrayElements, 3, Void)                                                                     \
    X(199, GetBooleanArrayRegion, 4, Void)                                                                          \
    X(200, GetByteArrayRegion, 4, Void)                                                                             \
    X(201, GetCharArrayRegion, 4, Void)                                                                             \
    X(202, GetShortArrayRegion, 4, Void)                                                                            \
    X(203, GetIntArrayRegion, 4, Void)                                                                              \
    X(204, GetLongArrayRegion, 4, Void)                                                                             \
    X(205, GetFloatArrayRegion, 4, Void)                                                                            \
    X(206, GetDoubleArrayRegion, 4, Void)                                                                           \
    X(207, SetBooleanArrayRegion, 4, Void)                                                                          \
    X(208, SetByteArrayRegion, 4, Void)                                                                             \
    X(209, SetCharArrayRegion, 4, Void)                                                                             \
    X(210, SetShortArrayRegion, 4, Void)                                                                            \
    X(211, SetIntArrayRegion, 4, Void)                                                                              \
    X(212, SetLongArrayRegion, 4, Void)                                                                             \
    X(213, SetFloatArrayRegion, 4, Void)                                                                            \
    X(214, SetDoubleArrayRegion, 4, Void)                                                                           \
    X(215, RegisterNatives, 3, Int)                                                                                 \
    X(216, UnregisterNatives, 1, Int)                                                                               \
    X(217, MonitorEnter, 1, Int)                                                                                    \
    X(218, MonitorExit, 1, Int)                                                                                     \
    X(219, GetJavaVM, 1, Int)                                                                                       \
    X(220, GetStringRegion, 4, Void)                                                                                \
    X(221, GetStringUTFRegion, 4, Void)                                                                             \
    X(222, GetPrimitiveArrayCritical, 2, Ptr)                                                                       \
    X(223, ReleasePrimitiveArrayCritical, 3, Void)                                                                  \
    X(224, GetStringCritical, 2, Ptr)                                                                               \
    X(225, ReleaseStringCritical, 2, Void)                                                                          \
    X(226, NewWeakGlobalRef, 1, Ptr)                                                                                \
    X(227, DeleteWeakGlobalRef, 1, Void)                                                                            \
    X(228, ExceptionCheck, 0, Int)                                                                                  \
    X(229, NewDirectByteBuffer, 2, Ptr)                                                                             \
    X(230, GetDirectBufferAddress, 1, Ptr)                                                                          \
    X(231, GetDirectBufferCapacity, 1, Int)                                                                         \
    X(232, GetObjectRefType, 1, Int)

inline const std::vector<FuncMeta>& funcTable()
{
    static const std::vector<FuncMeta> table = [] {
        std::vector<FuncMeta> v(kFuncCount);
#define JSIM_JNI_PLACE(id, name, argc, ret) v[id] = {#name, u8(argc), FuncMeta::Ret::ret};
        JSIM_JNI_FUNCTIONS(JSIM_JNI_PLACE)
#undef JSIM_JNI_PLACE
        return v;
    }();
    return table;
}

inline const FuncMeta* metaOf(u32 id)
{
    const std::vector<FuncMeta>& t = funcTable();
    if (id >= t.size() || t[id].name == nullptr) return nullptr;
    return &t[id];
}

/// 按名查函数号;找不到返回 kInvalidFunc。
constexpr u32 kInvalidFunc = 0xFFFFFFFFu;
inline u32 indexOf(std::string_view name)
{
    const std::vector<FuncMeta>& t = funcTable();
    for (usize i = 0; i < t.size(); ++i) {
        if (t[i].name != nullptr && name == t[i].name) return static_cast<u32>(i);
    }
    return kInvalidFunc;
}

// ---------------------------------------------------------------------------
// 句柄池
// ---------------------------------------------------------------------------

/// jobject 形状的假 guest 指针。不对应任何真实内存——绑定层不为它映射页,
/// 机器码解引用句柄会落到未映射读,由绑定层转 Bail{BadOperand}。
struct HandlePool
{
    static constexpr u64 kBase = 0x00400000;
    static constexpr u64 kStride = 8;
    static constexpr u64 kJavaVm = 0x00600000; ///< GetJavaVM 的假 VM 句柄

    struct RefEntry
    {
        SimValue value;
        bool global = false; ///< global 跨调用持久,local 一次 native 调用后回收
    };

    /// jmethodID / jfieldID 载荷:身份就是 (owner, name, desc)。
    struct Member
    {
        std::string owner;
        std::string name;
        std::string desc;
        bool isStatic = false;
    };

    u64 next = kBase;
    std::unordered_map<u64, RefEntry> refs;
    std::unordered_map<u64, Member> members;

    /// 发一个引用句柄。null 载荷不占句柄——按 JNI 口径编成 0(见 encodeRef)。
    Result<u64, Bail> makeRef(SimValue v, bool global)
    {
        if (!v.known || v.type != ValType::Ref) {
            return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "句柄池只收已知引用"});
        }
        if (v.ref.kind == RefPayload::Kind::Null) {
            return Result<u64, Bail>::ok(0);
        }
        const u64 h = next;
        next += kStride;
        refs[h] = {std::move(v), global};
        return Result<u64, Bail>::ok(h);
    }

    /// 解句柄。0 按 JNI null 口径回已知 null;悬空即 Bail,不静默给默认对象。
    Result<SimValue, Bail> deref(u64 h) const
    {
        if (h == 0) return Result<SimValue, Bail>::ok(SimValue::knownNull());
        auto it = refs.find(h);
        if (it == refs.end()) {
            return Result<SimValue, Bail>::err({Bail::Reason::BadOperand, 0, "悬空句柄"});
        }
        return Result<SimValue, Bail>::ok(it->second.value);
    }

    Result<u64, Bail> makeMember(const Member& m)
    {
        const u64 h = next;
        next += kStride;
        members[h] = m;
        return Result<u64, Bail>::ok(h);
    }

    Result<const Member*, Bail> memberOf(u64 h) const
    {
        auto it = members.find(h);
        if (it == members.end()) {
            return Result<const Member*, Bail>::err({Bail::Reason::BadOperand, 0, "悬空成员 ID"});
        }
        return Result<const Member*, Bail>::ok(&it->second);
    }

    /// 一次 native 调用结束回收 local 域;global 留给下一次调用(缓存 jclass)。
    void popLocals()
    {
        for (auto it = refs.begin(); it != refs.end();) {
            if (it->second.global) {
                ++it;
            } else {
                it = refs.erase(it);
            }
        }
    }

    usize alive() const { return refs.size(); }

    /// IsSameObject 口径:同句柄、双 null 或同 objId 判真;Str 载荷只在同句柄时
    /// 判真(动态字符串身份不可判是既定口径)。
    bool sameObject(u64 a, u64 b) const
    {
        if (a == b) return true;
        Result<SimValue, Bail> x = deref(a);
        Result<SimValue, Bail> y = deref(b);
        if (!x.isOk() || !y.isOk()) return false;
        const SimValue& u = x.value();
        const SimValue& v = y.value();
        if (u.ref.kind == RefPayload::Kind::Null && v.ref.kind == RefPayload::Kind::Null) return true;
        if (u.ref.kind == RefPayload::Kind::Obj && v.ref.kind == RefPayload::Kind::Obj) {
            return u.ref.objId == v.ref.objId;
        }
        return false;
    }
};

// ---------------------------------------------------------------------------
// JniEnv 与分发
// ---------------------------------------------------------------------------

/// 描述符 → 参数类型序列。I/Z/B/C/S → I32,J → I64,F → F32,D → F64,L…;/[ → Ref。
inline Result<std::vector<ValType>, Bail> argTypesOfDesc(std::string_view desc)
{
    std::vector<ValType> out;
    if (desc.empty() || desc.front() != '(') {
        return Result<std::vector<ValType>, Bail>::err({Bail::Reason::BadOperand, 0, "描述符缺 '('"});
    }
    usize i = 1;
    while (i < desc.size() && desc[i] != ')') {
        const char c = desc[i];
        switch (c) {
        case 'I': case 'Z': case 'B': case 'C': case 'S':
            out.push_back(ValType::I32); ++i; break;
        case 'J': out.push_back(ValType::I64); ++i; break;
        case 'F': out.push_back(ValType::F32); ++i; break;
        case 'D': out.push_back(ValType::F64); ++i; break;
        case 'L': {
            const usize j = desc.find(';', i);
            if (j == std::string_view::npos) {
                return Result<std::vector<ValType>, Bail>::err({Bail::Reason::BadOperand, 0, "描述符缺 ';'"});
            }
            out.push_back(ValType::Ref); i = j + 1; break;
        }
        case '[': {
            usize j = i;
            while (j < desc.size() && desc[j] == '[') ++j;
            if (j < desc.size() && desc[j] == 'L') {
                const usize k = desc.find(';', j);
                if (k == std::string_view::npos) {
                    return Result<std::vector<ValType>, Bail>::err({Bail::Reason::BadOperand, 0, "描述符缺 ';'"});
                }
                i = k + 1;
            } else if (j < desc.size()) {
                i = j + 1;
            } else {
                return Result<std::vector<ValType>, Bail>::err({Bail::Reason::BadOperand, 0, "描述符截断"});
            }
            out.push_back(ValType::Ref); break;
        }
        default:
            return Result<std::vector<ValType>, Bail>::err({Bail::Reason::BadOperand, 0, "描述符非法字符"});
        }
    }
    if (i >= desc.size() || desc[i] != ')') {
        return Result<std::vector<ValType>, Bail>::err({Bail::Reason::BadOperand, 0, "描述符缺 ')'"});
    }
    return Result<std::vector<ValType>, Bail>::ok(std::move(out));
}

/// 描述符返回类型;void 调用 hasReturn=false。
inline Result<std::pair<ValType, bool>, Bail> retTypeOfDesc(std::string_view desc)
{
    const usize r = desc.rfind(')');
    if (r == std::string_view::npos || r + 1 >= desc.size()) {
        return Result<std::pair<ValType, bool>, Bail>::err({Bail::Reason::BadOperand, 0, "描述符缺返回类型"});
    }
    switch (desc[r + 1]) {
    case 'V': return Result<std::pair<ValType, bool>, Bail>::ok({ValType::I32, false});
    case 'J': return Result<std::pair<ValType, bool>, Bail>::ok({ValType::I64, true});
    case 'F': return Result<std::pair<ValType, bool>, Bail>::ok({ValType::F32, true});
    case 'D': return Result<std::pair<ValType, bool>, Bail>::ok({ValType::F64, true});
    case 'L': case '[': return Result<std::pair<ValType, bool>, Bail>::ok({ValType::Ref, true});
    default: return Result<std::pair<ValType, bool>, Bail>::ok({ValType::I32, true});
    }
}

/// 一次 JNI 调用的现场。定长函数的实参已按表取好(args 下标 = C 位置 - 1);
/// bare Call*Method 变体的变长尾经 argI/argF 按位置取(浮点实参走 XMM 位型)。
struct JniCall
{
    u32 id = 0;
    const char* name = nullptr;
    JniEnv& env;
    const std::vector<u64>& args;
    const std::function<Result<u64, Bail>(usize pos)>& argI;
    const std::function<Result<u64, Bail>(usize pos)>& argF;

    JniCall(u32 id_, const char* name_, JniEnv& env_, const std::vector<u64>& args_,
            const std::function<Result<u64, Bail>(usize)>& i,
            const std::function<Result<u64, Bail>(usize)>& f)
        : id(id_), name(name_), env(env_), args(args_), argI(i), argF(f) {}
};

using JniHook = std::function<Result<u64, Bail>(JniCall&)>;

/// 会话 JNI 面。生命周期由消费方持有(与 Session 同级),install 只挂引用。
struct JniEnv
{
    Session* session = nullptr;
    Layout layout;
    GuestMemory mem;
    /// native 方法表:键 = owner.name+desc(同 callMocks 键形)。
    std::unordered_map<std::string, NativeSpec> methods;
    /// 用户钩子:键 = 函数名,优先于默认骨架,整体替换不做参数级组合。
    std::unordered_map<std::string, JniHook> hooks;
    /// Call*Method 回跳:被调 Java 方法交给消费方开嵌套 Sim(核心 D9 不破)。
    std::function<Result<SimValue, Bail>(const CallRef&, std::vector<SimValue>)> javaInvoker;
    std::function<void(const std::string&)> trace; ///< 观测面:每次 JNI 调用一行
    bool pendingThrow = false;
    std::string throwClass;
    HandlePool pool;
    NativeExecutor executor;
    u64 maxSteps = 1u << 22; ///< native 步数熔断,绑定层消费
    NativeSpec::Abi currentAbi = NativeSpec::Abi::Win64; ///< 运行中的 ABI(RegisterNatives 录表用)

    /// resolver 产出的 native mock 缓存:键 = 方法键,值地址交引擎只读一次即拷贝。
    std::unordered_map<std::string, CallMock> nativeMocks;
};

/// 异常已挂起时仍放行的函数:JNI 契约要求 native 在 Throw 后立即返回,但
/// ExceptionCheck / Occurred / Clear / Describe 本身就是检查与清理入口。
inline bool throwableSafe(const char* name)
{
    return name != nullptr
        && (std::strcmp(name, "ExceptionCheck") == 0 || std::strcmp(name, "ExceptionOccurred") == 0
            || std::strcmp(name, "ExceptionClear") == 0 || std::strcmp(name, "ExceptionDescribe") == 0);
}

inline std::string hexOf(u64 v)
{
    static const char* digits = "0123456789abcdef";
    std::string s = "0x";
    if (v == 0) return "0x0";
    char buf[16];
    usize n = 0;
    while (v != 0) {
        buf[n++] = digits[v & 0xF];
        v >>= 4;
    }
    while (n != 0) s += buf[--n];
    return s;
}

namespace detail {

/// 读 NUL 结尾的 guest 字符串。只用于 FindClass / GetMethodID 实参这类冷路径。
inline Result<std::string, Bail> readCStr(JniEnv& env, u64 addr, usize cap = 1u << 16)
{
    std::string s;
    for (usize i = 0; i < cap; ++i) {
        char c = 0;
        Result<void, Bail> r = env.mem.read(addr + i, &c, 1);
        if (!r.isOk()) return Result<std::string, Bail>::err(r.error());
        if (c == '\0') return Result<std::string, Bail>::ok(std::move(s));
        s.push_back(c);
    }
    return Result<std::string, Bail>::err({Bail::Reason::BadOperand, 0, "guest 字符串缺 NUL 结尾"});
}

inline Result<std::vector<u8>, Bail> readBytes(JniEnv& env, u64 addr, usize n)
{
    std::vector<u8> buf(n);
    if (n != 0) {
        Result<void, Bail> r = env.mem.read(addr, buf.data(), n);
        if (!r.isOk()) return Result<std::vector<u8>, Bail>::err(r.error());
    }
    return Result<std::vector<u8>, Bail>::ok(std::move(buf));
}

inline Result<void, Bail> writeBytes(JniEnv& env, u64 addr, const void* p, usize n)
{
    return env.mem.write(addr, p, n);
}

/// jclass 句柄 → 内部类名。Class 对象是 session.classObjectOf 建的,类名在 data。
inline Result<std::string, Bail> classNameOfClass(JniEnv& env, u64 h)
{
    Result<SimValue, Bail> v = env.pool.deref(h);
    if (!v.isOk()) return Result<std::string, Bail>::err(v.error());
    if (v.value().ref.kind != RefPayload::Kind::Obj) {
        return Result<std::string, Bail>::err({Bail::Reason::BadOperand, 0, "jclass 句柄不是对象"});
    }
    auto it = env.session->heap.find(v.value().ref.objId);
    if (it == env.session->heap.end()) {
        return Result<std::string, Bail>::err({Bail::Reason::BadOperand, 0, "jclass 堆对象缺失"});
    }
    return Result<std::string, Bail>::ok(it->second.data);
}

/// 数组类 → 元素字节宽([Z/[B=1,[C/[S=2,[I/[F=4,[J/[D=8)。
inline usize elemWidthOf(const std::string& arrayClass)
{
    if (arrayClass == "[J" || arrayClass == "[D") return 8;
    if (arrayClass == "[C" || arrayClass == "[S" || arrayClass == "[I" || arrayClass == "[F") return 2;
    return 1;
}

inline bool isRefArrayClass(const std::string& arrayClass)
{
    return arrayClass.size() >= 2 && (arrayClass[1] == 'L' || arrayClass[1] == '[');
}

/// MUTF-8 → UTF-16 码元序列(BMP 家族口径,同 jsim-str;4 字节序列按坏形拒绝)。
inline Result<std::vector<u16>, Bail> mutf8ToUtf16(const std::string& bytes)
{
    std::vector<u16> out;
    usize i = 0;
    while (i < bytes.size()) {
        const u8 b = static_cast<u8>(bytes[i]);
        if ((b & 0x80) == 0) {
            out.push_back(b);
            i += 1;
        } else if ((b & 0xE0) == 0xC0) {
            if (i + 1 >= bytes.size() || (bytes[i + 1] & 0xC0) != 0x80) {
                return Result<std::vector<u16>, Bail>::err({Bail::Reason::BadOperand, 0, "MUTF-8 续字节缺失"});
            }
            out.push_back(static_cast<u16>(((b & 0x1F) << 6) | (bytes[i + 1] & 0x3F)));
            i += 2;
        } else if ((b & 0xF0) == 0xE0) {
            if (i + 2 >= bytes.size() || (bytes[i + 1] & 0xC0) != 0x80 || (bytes[i + 2] & 0xC0) != 0x80) {
                return Result<std::vector<u16>, Bail>::err({Bail::Reason::BadOperand, 0, "MUTF-8 续字节缺失"});
            }
            out.push_back(static_cast<u16>(((b & 0x0F) << 12) | ((bytes[i + 1] & 0x3F) << 6) | (bytes[i + 2] & 0x3F)));
            i += 3;
        } else {
            return Result<std::vector<u16>, Bail>::err({Bail::Reason::BadOperand, 0, "MUTF-8 不含 4 字节形"});
        }
    }
    return Result<std::vector<u16>, Bail>::ok(std::move(out));
}

/// UTF-16 码元 → MUTF-8(NUL 编成 C0 80,代理对按两个 3 字节序列原样编)。
inline std::string utf16ToMutf8(const std::vector<u16>& units)
{
    std::string out;
    for (u16 u : units) {
        if (u == 0) {
            out.push_back(static_cast<char>(0xC0));
            out.push_back(static_cast<char>(0x80));
        } else if (u < 0x80) {
            out.push_back(static_cast<char>(u));
        } else if (u < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (u >> 6)));
            out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (u >> 12)));
            out.push_back(static_cast<char>(0x80 | ((u >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
        }
    }
    return out;
}

/// 位型 → SimValue;引用走句柄池(0 按 null)。Unknown 无法过 JNI 边界,
/// 由调用方在编组前把「不具体」折成 Bail。
inline Result<SimValue, Bail> decodeByValType(ValType t, u64 bits, JniEnv& env)
{
    switch (t) {
    case ValType::I32: return Result<SimValue, Bail>::ok(SimValue::knownI32(static_cast<i32>(bits)));
    case ValType::I64: return Result<SimValue, Bail>::ok(SimValue::knownI64(static_cast<i64>(bits)));
    case ValType::F32: return Result<SimValue, Bail>::ok(SimValue::knownF32(bitsToFloat(static_cast<u32>(bits))));
    case ValType::F64: return Result<SimValue, Bail>::ok(SimValue::knownF64(bitsToDouble(bits)));
    case ValType::Ref: return env.pool.deref(bits);
    }
    return Result<SimValue, Bail>::err({Bail::Reason::BadOperand, 0, "非法值类型"});
}

/// SimValue → 位型;引用发 local 句柄(null 编 0),不具体即 Bail{NotConcrete}。
inline Result<u64, Bail> encodeByValType(const SimValue& v, JniEnv& env)
{
    if (!v.known) {
        return Result<u64, Bail>::err({Bail::Reason::NotConcrete, 0, "过 JNI 边界的值不具体"});
    }
    switch (v.type) {
    case ValType::I32: return Result<u64, Bail>::ok(v.asU32());
    case ValType::I64: return Result<u64, Bail>::ok(v.asU64());
    case ValType::F32: return Result<u64, Bail>::ok(floatToBits(v.asF32()));
    case ValType::F64: return Result<u64, Bail>::ok(doubleToBits(v.asF64()));
    case ValType::Ref: {
        Result<u64, Bail> h = env.pool.makeRef(v, false);
        if (!h.isOk()) return Result<u64, Bail>::err(h.error());
        return Result<u64, Bail>::ok(h.value());
    }
    }
    return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "非法值类型"});
}

/// jvalue* / va_list 形的变长实参读取:每个 jvalue 占 8 字节,按 desc 序读位型。
/// I32 槽读低 32 位(高位是 ABI 垃圾,截掉)。
inline Result<std::vector<u64>, Bail> readJValueArray(JniEnv& env, u64 addr, const std::string& desc)
{
    Result<std::vector<ValType>, Bail> types = argTypesOfDesc(desc);
    if (!types.isOk()) return Result<std::vector<u64>, Bail>::err(types.error());
    std::vector<u64> bits;
    for (ValType t : types.value()) {
        u64 raw = 0;
        Result<void, Bail> r = env.mem.read(addr + bits.size() * 8, &raw, 8);
        if (!r.isOk()) return Result<std::vector<u64>, Bail>::err(r.error());
        bits.push_back(t == ValType::I32 ? (raw & 0xFFFFFFFFull) : raw);
    }
    return Result<std::vector<u64>, Bail>::ok(std::move(bits));
}

/// bare Call*Method 变体:变长实参在寄存器/栈上,按 desc 逐位取。env=0、接收者
/// 或 clazz=1、jmethodID=2,实参从位置 3 起;浮点实参走 XMM 位型。
inline Result<std::vector<u64>, Bail> collectBareArgs(JniCall& call, const std::string& desc)
{
    Result<std::vector<ValType>, Bail> types = argTypesOfDesc(desc);
    if (!types.isOk()) return Result<std::vector<u64>, Bail>::err(types.error());
    std::vector<u64> bits;
    usize pos = 3;
    for (ValType t : types.value()) {
        if (t == ValType::F32 || t == ValType::F64) {
            Result<u64, Bail> v = call.argF(pos++);
            if (!v.isOk()) return Result<std::vector<u64>, Bail>::err(v.error());
            bits.push_back(t == ValType::F32 ? (v.value() & 0xFFFFFFFFull) : v.value());
        } else {
            Result<u64, Bail> v = call.argI(pos++);
            if (!v.isOk()) return Result<std::vector<u64>, Bail>::err(v.error());
            bits.push_back(t == ValType::I32 ? (v.value() & 0xFFFFFFFFull) : v.value());
        }
    }
    return Result<std::vector<u64>, Bail>::ok(std::move(bits));
}

/// 位型序列 → SimValue 序列(按 desc)。
inline Result<std::vector<SimValue>, Bail> decodeArgs(const std::string& desc, const std::vector<u64>& bits,
                                                      JniEnv& env)
{
    Result<std::vector<ValType>, Bail> types = argTypesOfDesc(desc);
    if (!types.isOk()) return Result<std::vector<SimValue>, Bail>::err(types.error());
    if (types.value().size() != bits.size()) {
        return Result<std::vector<SimValue>, Bail>::err({Bail::Reason::BadOperand, 0, "实参数与描述符不符"});
    }
    std::vector<SimValue> out;
    for (usize i = 0; i < types.value().size(); ++i) {
        Result<SimValue, Bail> v = decodeByValType(types.value()[i], bits[i], env);
        if (!v.isOk()) return Result<std::vector<SimValue>, Bail>::err(v.error());
        out.push_back(std::move(v.value()));
    }
    return Result<std::vector<SimValue>, Bail>::ok(std::move(out));
}

/// 字符串句柄 → Str 载荷拷贝。非字符串引用一律 Bail,不静默当空串。
/// 按值返回:deref 给的是 SimValue 拷贝,取其内嵌字符串的地址会悬空。
inline Result<std::string, Bail> strPayloadOf(JniEnv& env, u64 h)
{
    Result<SimValue, Bail> v = env.pool.deref(h);
    if (!v.isOk()) return Result<std::string, Bail>::err(v.error());
    if (v.value().ref.kind != RefPayload::Kind::Str) {
        return Result<std::string, Bail>::err({Bail::Reason::BadOperand, 0, "jstring 句柄不是字符串"});
    }
    return Result<std::string, Bail>::ok(v.value().ref.str);
}

/// 在 scratch 物化一段字节,返回 guest 指针。
inline Result<u64, Bail> materialize(JniEnv& env, const void* p, usize n, usize align = 1)
{
    if (env.mem.map == nullptr) {
        return Result<u64, Bail>::err({Bail::Reason::UnsupportedCall, 0, "GuestMemory.map 未接"});
    }
    Result<std::pair<u64, usize>, Bail> m = env.mem.map(n, align);
    if (!m.isOk()) return Result<u64, Bail>::err(m.error());
    Result<void, Bail> w = env.mem.write(m.value().first, p, n);
    if (!w.isOk()) return Result<u64, Bail>::err(w.error());
    return Result<u64, Bail>::ok(m.value().first);
}

/// 写一个 jboolean 到 isCopy 出参。
inline void markCopy(JniEnv& env, u64 isCopyAddr)
{
    if (isCopyAddr != 0) {
        const u8 one = 1;
        env.mem.write(isCopyAddr, &one, 1);
    }
}

} // namespace detail

/// 一次 JNI 调用的参数类布局:C 位置 p 是否浮点类。定长函数全整;bare Call*
/// Method / NewObject 变体按 jmethodID 的 desc 展开。绑定层据此把「位置 + 类」
/// 映到寄存器文件(Win64/SysV 的整数与浮点寄存器各自按序消费)。
inline Result<std::vector<char>, Bail> argClassesOf(JniEnv& env, u32 id, u64 midHandle)
{
    const FuncMeta* meta = metaOf(id);
    if (meta == nullptr) {
        return Result<std::vector<char>, Bail>::err({Bail::Reason::UnsupportedCall, 0, "JNI 函数号无效"});
    }
    std::vector<char> flags(meta->argc + 1, 0); ///< 位置 0 = env,恒整
    // 变长形态只出现在 bare Call*Method / NewObject:实参在寄存器/栈上,类由
    // jmethodID 的 desc 决定。A/V 变体的实参在内存(jvalue*/va_list),不经寄存器类。
    const std::string name = meta->name;
    const bool dynamic = name.rfind("Call", 0) == 0 || name.rfind("NewObject", 0) == 0;
    if (!dynamic) return Result<std::vector<char>, Bail>::ok(std::move(flags));
    const char last = name.back();
    if (last == 'A' || last == 'V') return Result<std::vector<char>, Bail>::ok(std::move(flags));
    Result<const HandlePool::Member*, Bail> mid = env.pool.memberOf(midHandle);
    if (!mid.isOk()) return Result<std::vector<char>, Bail>::err(mid.error());
    Result<std::vector<ValType>, Bail> types = argTypesOfDesc(mid.value()->desc);
    if (!types.isOk()) return Result<std::vector<char>, Bail>::err(types.error());
    flags.clear();
    flags.push_back(0); // env
    flags.push_back(0); // 接收者 / clazz
    flags.push_back(0); // mid
    for (ValType t : types.value()) flags.push_back(t == ValType::F32 || t == ValType::F64 ? 1 : 0);
    return Result<std::vector<char>, Bail>::ok(std::move(flags));
}

/// 分发一次 JNI 调用。argI/argF 由执行器按 ABI 实现并各自闭包参数类布局
/// (argClassesOf 的产物;unicorn:寄存器 + 栈,FakeMachine:预编脚本)。
/// 返回 guest 位型写回返回寄存器。钩子缺省一律 Bail{UnsupportedCall}——补什么
/// 由用户决定。
inline Result<u64, Bail> dispatch(JniEnv& env, u32 id,
                                  const std::function<Result<u64, Bail>(usize)>& argI,
                                  const std::function<Result<u64, Bail>(usize)>& argF)
{
    const FuncMeta* meta = metaOf(id);
    if (meta == nullptr) {
        return Result<u64, Bail>::err({Bail::Reason::UnsupportedCall, 0,
                                       "JNI 函数号越界或保留:" + std::to_string(id)});
    }
    if (env.pendingThrow && !throwableSafe(meta->name)) {
        return Result<u64, Bail>::err({Bail::Reason::UnsupportedCall, 0,
                                       "异常已挂起,JNI 契约要求立即返回(调用的是 " + std::string(meta->name) + ")"});
    }
    std::vector<u64> fixed;
    fixed.reserve(meta->argc);
    for (usize p = 1; p <= meta->argc; ++p) {
        Result<u64, Bail> v = argI(p); // 定长函数全整类(浮点只在变长尾)
        if (!v.isOk()) return Result<u64, Bail>::err(v.error());
        fixed.push_back(v.value());
    }
    JniCall call(id, meta->name, env, fixed, argI, argF);

    auto hit = env.hooks.find(meta->name);
    Result<u64, Bail> out = hit != env.hooks.end()
        ? hit->second(call)
        : Result<u64, Bail>::err({Bail::Reason::UnsupportedCall, 0,
                                  std::string("JNI 钩子未实现:") + meta->name});
    if (env.trace) {
        std::string line = std::string("[JNI] ") + meta->name + "(";
        for (usize i = 0; i < fixed.size(); ++i) {
            if (i != 0) line += ", ";
            line += hexOf(fixed[i]);
        }
        line += ")";
        line += out.isOk() ? " -> " + hexOf(out.value()) : " -> Bail{" + out.error().detail + "}";
        env.trace(line);
    }
    return out;
}

// ---------------------------------------------------------------------------
// 编组:native CallMock(jsim 帧 ↔ 位型)
// ---------------------------------------------------------------------------

/// 弹参编组:接收者(非 static)先收,再按 desc 逆序弹实参。不具体的值过不了
/// JNI 边界——Bail{NotConcrete},与「不编造」同一纪律。
inline Result<NativeRun, Bail> buildNativeRun(JniEnv& env, const NativeSpec& spec, const CallRef& callee,
                                              Frame& frame)
{
    NativeRun run;
    run.spec = &spec;
    run.hasReturn = callee.hasReturn;
    run.retType = callee.retType;

    if (!callee.isStatic) {
        Result<SimValue, Bail> recv = frame.popChecked();
        if (!recv.isOk()) return Result<NativeRun, Bail>::err(recv.error());
        if (!recv.value().known || recv.value().type != ValType::Ref) {
            return Result<NativeRun, Bail>::err({Bail::Reason::NotConcrete, frame.pc(),
                                                 "native 接收者不具体"});
        }
        Result<u64, Bail> h = env.pool.makeRef(recv.value(), false);
        if (!h.isOk()) return Result<NativeRun, Bail>::err(h.error());
        run.args.push_back(h.value());
    } else {
        // static native 的 jclass:与 ldc 类常量同一个对象(身份一致)。
        const u64 cid = env.session->classObjectOf(callee.owner);
        Result<u64, Bail> h = env.pool.makeRef(SimValue::knownObj(cid), false);
        if (!h.isOk()) return Result<NativeRun, Bail>::err(h.error());
        run.args.push_back(h.value());
    }

    Result<std::vector<ValType>, Bail> types = argTypesOfDesc(callee.desc);
    if (!types.isOk()) return Result<NativeRun, Bail>::err(types.error());
    std::vector<u64> raw(types.value().size(), 0);
    for (usize i = types.value().size(); i-- > 0;) {
        Result<SimValue, Bail> popped = frame.popChecked();
        if (!popped.isOk()) return Result<NativeRun, Bail>::err(popped.error());
        Result<u64, Bail> bits = detail::encodeByValType(popped.value(), env);
        if (!bits.isOk()) return Result<NativeRun, Bail>::err(bits.error());
        raw[i] = bits.value();
    }
    run.args.insert(run.args.end(), raw.begin(), raw.end());
    for (ValType t : types.value()) {
        run.argIsFloat.push_back(t == ValType::F32 || t == ValType::F64 ? 1 : 0);
    }
    return Result<NativeRun, Bail>::ok(std::move(run));
}

/// native CallMock:弹参编组 → 执行器跑机器码 → 返回解码 → local 回收 → 异常位。
inline CallMock makeNativeMock(JniEnv& env, const NativeSpec& spec, const CallRef& callee)
{
    return [&env, spec, callee](Frame& frame) -> Result<SimValue, Bail> {
        if (env.executor.run == nullptr) {
            return Result<SimValue, Bail>::err({Bail::Reason::UnsupportedCall, frame.pc(),
                                                "native 执行器未接(env.executor.run)"});
        }
        Result<NativeRun, Bail> run = buildNativeRun(env, spec, callee, frame);
        if (!run.isOk()) return Result<SimValue, Bail>::err(run.error());

        env.currentAbi = spec.abi;
        Result<u64, Bail> ret = env.executor.run(env, run.value());

        if (env.pendingThrow) {
            env.pool.popLocals();
            const std::string cls = env.throwClass;
            env.pendingThrow = false;
            env.throwClass.clear();
            return Result<SimValue, Bail>::err({Bail::Reason::UnsupportedCall, frame.pc(),
                                                "native 抛出异常:" + cls});
        }
        if (!ret.isOk()) {
            env.pool.popLocals();
            return Result<SimValue, Bail>::err(ret.error());
        }

        SimValue out = SimValue::unknownOf(ValType::I32);
        if (callee.hasReturn) {
            Result<SimValue, Bail> v = detail::decodeByValType(callee.retType, ret.value(), env);
            if (!v.isOk()) {
                env.pool.popLocals();
                return Result<SimValue, Bail>::err(v.error());
            }
            out = std::move(v.value());
        }
        env.pool.popLocals();
        return Result<SimValue, Bail>::ok(std::move(out));
    };
}

// ---------------------------------------------------------------------------
// 默认骨架
// ---------------------------------------------------------------------------

/// 只补缺:用户预置的钩子不被覆盖(用户钩子 > 默认骨架)。
inline void putHook(JniEnv& env, const char* name, JniHook fn)
{
    if (env.hooks.find(name) == env.hooks.end()) env.hooks[name] = std::move(fn);
}

/// Call*Method 家族共用实现。variant:0=bare(变长在寄存器/栈) 1=V 2=A(jvalue*)。
inline Result<u64, Bail> callMethodImpl(JniCall& call, bool isStatic, int variant)
{
    JniEnv& env = call.env;
    if (call.args.size() < 2) {
        return Result<u64, Bail>::err({Bail::Reason::StackBroken, 0, "Call* 实参不足"});
    }
    Result<const HandlePool::Member*, Bail> mid = env.pool.memberOf(call.args[1]);
    if (!mid.isOk()) return Result<u64, Bail>::err(mid.error());
    const HandlePool::Member& m = *mid.value();

    std::vector<u64> bits;
    if (variant == 0) {
        Result<std::vector<u64>, Bail> b = detail::collectBareArgs(call, m.desc);
        if (!b.isOk()) return Result<u64, Bail>::err(b.error());
        bits = std::move(b.value());
    } else if (variant == 2) {
        if (call.args.size() < 3) {
            return Result<u64, Bail>::err({Bail::Reason::StackBroken, 0, "Call*A 缺 jvalue*"});
        }
        Result<std::vector<u64>, Bail> b = detail::readJValueArray(env, call.args[2], m.desc);
        if (!b.isOk()) return Result<u64, Bail>::err(b.error());
        bits = std::move(b.value());
    } else { // V
        if (call.args.size() < 3) {
            return Result<u64, Bail>::err({Bail::Reason::StackBroken, 0, "Call*V 缺 va_list"});
        }
        if (env.currentAbi == NativeSpec::Abi::SysV) {
            return Result<u64, Bail>::err({Bail::Reason::UnsupportedCall, 0,
                                           "SysV va_list 结构 v1 不做,用 A 形变体"});
        }
        // Win64 va_list 是 char*,指向实参溢出区,逐 8 字节槽读。
        Result<std::vector<u64>, Bail> b = detail::readJValueArray(env, call.args[2], m.desc);
        if (!b.isOk()) return Result<u64, Bail>::err(b.error());
        bits = std::move(b.value());
    }

    Result<std::vector<SimValue>, Bail> argv = detail::decodeArgs(m.desc, bits, env);
    if (!argv.isOk()) return Result<u64, Bail>::err(argv.error());

    if (env.javaInvoker == nullptr) {
        return Result<u64, Bail>::err({Bail::Reason::UnsupportedCall, 0,
                                       "Call*Method 需要 env.javaInvoker(嵌套 Sim 编排)"});
    }
    Result<std::pair<ValType, bool>, Bail> rt = retTypeOfDesc(m.desc);
    if (!rt.isOk()) return Result<u64, Bail>::err(rt.error());
    CallRef callee;
    callee.owner = m.owner;
    callee.name = m.name;
    callee.desc = m.desc;
    callee.retType = rt.value().first;
    callee.hasReturn = rt.value().second;
    callee.isStatic = isStatic;
    Result<SimValue, Bail> out = env.javaInvoker(callee, std::move(argv.value()));
    if (!out.isOk()) return Result<u64, Bail>::err(out.error());
    if (!callee.hasReturn) return Result<u64, Bail>::ok(0);

    // 回跳结果过 JNI 边界:不具体即 Bail,不编 0。
    Result<u64, Bail> bits2 = detail::encodeByValType(out.value(), env);
    if (!bits2.isOk()) return Result<u64, Bail>::err(bits2.error());
    // 浮点返回走 XMM 位型(执行器按 FuncMeta::Ret 决定写哪个寄存器文件)。
    return Result<u64, Bail>::ok(bits2.value());
}

/// NewObject 家族:分配空壳(类名来自 jmethodID 的 owner),<init> 经 javaInvoker
/// 跑(构造语义属消费方)。variant 同 callMethodImpl。
inline Result<u64, Bail> newObjectImpl(JniCall& call, int variant)
{
    JniEnv& env = call.env;
    if (call.args.size() < 2) {
        return Result<u64, Bail>::err({Bail::Reason::StackBroken, 0, "NewObject 实参不足"});
    }
    Result<const HandlePool::Member*, Bail> mid = env.pool.memberOf(call.args[1]);
    if (!mid.isOk()) return Result<u64, Bail>::err(mid.error());
    const HandlePool::Member& m = *mid.value();

    const u64 id = env.session->nextAllocId++;
    HeapObj obj;
    obj.className = m.owner;
    env.session->heap[id] = std::move(obj);
    Result<u64, Bail> selfRef = env.pool.makeRef(SimValue::knownObj(id), false);
    if (!selfRef.isOk()) return Result<u64, Bail>::err(selfRef.error());
    const u64 selfHandle = selfRef.value();

    if (env.javaInvoker == nullptr) {
        return Result<u64, Bail>::err({Bail::Reason::UnsupportedCall, 0,
                                       "NewObject 需要 env.javaInvoker(构造语义属消费方)"});
    }
    std::vector<u64> bits;
    if (variant == 0) {
        Result<std::vector<u64>, Bail> b = detail::collectBareArgs(call, m.desc);
        if (!b.isOk()) return Result<u64, Bail>::err(b.error());
        bits = std::move(b.value());
    } else if (variant == 2) {
        if (call.args.size() < 3) {
            return Result<u64, Bail>::err({Bail::Reason::StackBroken, 0, "NewObjectA 缺 jvalue*"});
        }
        Result<std::vector<u64>, Bail> b = detail::readJValueArray(env, call.args[2], m.desc);
        if (!b.isOk()) return Result<u64, Bail>::err(b.error());
        bits = std::move(b.value());
    } else {
        if (env.currentAbi == NativeSpec::Abi::SysV) {
            return Result<u64, Bail>::err({Bail::Reason::UnsupportedCall, 0, "SysV va_list v1 不做"});
        }
        if (call.args.size() < 3) {
            return Result<u64, Bail>::err({Bail::Reason::StackBroken, 0, "NewObjectV 缺 va_list"});
        }
        Result<std::vector<u64>, Bail> b = detail::readJValueArray(env, call.args[2], m.desc);
        if (!b.isOk()) return Result<u64, Bail>::err(b.error());
        bits = std::move(b.value());
    }
    // 实参按 <init> 的 desc 解码(this 不在 desc 里,解码后前插)。
    Result<std::vector<SimValue>, Bail> argv = detail::decodeArgs(m.desc, bits, env);
    if (!argv.isOk()) return Result<u64, Bail>::err(argv.error());
    argv.value().insert(argv.value().begin(), SimValue::knownObj(id));

    CallRef callee;
    callee.owner = m.owner;
    callee.name = "<init>";
    callee.desc = m.desc;
    callee.retType = ValType::I32;
    callee.hasReturn = false;
    callee.isStatic = false;
    Result<SimValue, Bail> out = env.javaInvoker(callee, std::move(argv.value()));
    if (!out.isOk()) return Result<u64, Bail>::err(out.error());
    return Result<u64, Bail>::ok(selfHandle);
}

/// 注册默认骨架。约 40 个函数按解密场景的调用频度选;覆盖面见 docs/ext-jni.md。
/// 幂等:只补缺,不覆盖用户钩子。
inline void installSkeletons(JniEnv& env)
{
    using detail::materialize;
    using detail::readJValueArray;
    using detail::strPayloadOf;

    // ---- 版本与类 ----
    putHook(env, "GetVersion", [](JniCall&) -> Result<u64, Bail> {
        return Result<u64, Bail>::ok(0x00010008); // JNI_VERSION_1_8
    });
    putHook(env, "FindClass", [](JniCall& call) -> Result<u64, Bail> {
        if (call.args[0] == 0) return Result<u64, Bail>::ok(0);
        Result<std::string, Bail> name = detail::readCStr(call.env, call.args[0]);
        if (!name.isOk()) return Result<u64, Bail>::err(name.error());
        // 模型无类层次,任何名字都建对象——typo 类当找到用是已接受的近似。
        const u64 cid = call.env.session->classObjectOf(name.value());
        return call.env.pool.makeRef(SimValue::knownObj(cid), false);
    });
    putHook(env, "GetObjectClass", [](JniCall& call) -> Result<u64, Bail> {
        Result<SimValue, Bail> v = call.env.pool.deref(call.args[0]);
        if (!v.isOk()) return Result<u64, Bail>::err(v.error());
        std::string cls;
        if (v.value().ref.kind == RefPayload::Kind::Null) {
            return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "GetObjectClass(null)"});
        }
        if (v.value().ref.kind == RefPayload::Kind::Str) {
            cls = "java/lang/String";
        } else {
            auto it = call.env.session->heap.find(v.value().ref.objId);
            if (it == call.env.session->heap.end()) {
                return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "GetObjectClass 堆对象缺失"});
            }
            cls = it->second.className;
        }
        const u64 cid = call.env.session->classObjectOf(cls);
        return call.env.pool.makeRef(SimValue::knownObj(cid), false);
    });
    putHook(env, "GetSuperclass", [](JniCall&) -> Result<u64, Bail> {
        return Result<u64, Bail>::ok(0); // 无类层次,v1 恒 null
    });
    putHook(env, "IsAssignableFrom", [](JniCall& call) -> Result<u64, Bail> {
        Result<std::string, Bail> a = detail::classNameOfClass(call.env, call.args[0]);
        Result<std::string, Bail> b = detail::classNameOfClass(call.env, call.args[1]);
        // 同名可赋值;其余无层次可判,按 false 并留观测。
        const bool ok = a.isOk() && b.isOk() && a.value() == b.value();
        if (!ok && call.env.trace) call.env.trace("[JNI] IsAssignableFrom 无类型层次,按同名判定");
        return Result<u64, Bail>::ok(ok ? 1 : 0);
    });

    // ---- 异常面 ----
    putHook(env, "ThrowNew", [](JniCall& call) -> Result<u64, Bail> {
        Result<std::string, Bail> cls = detail::classNameOfClass(call.env, call.args[0]);
        call.env.pendingThrow = true;
        call.env.throwClass = cls.isOk() ? cls.value() : "<unknown>";
        return Result<u64, Bail>::ok(0);
    });
    putHook(env, "Throw", [](JniCall& call) -> Result<u64, Bail> {
        Result<SimValue, Bail> v = call.env.pool.deref(call.args[0]);
        std::string cls = "<unknown>";
        if (v.isOk() && v.value().ref.kind == RefPayload::Kind::Obj) {
            auto it = call.env.session->heap.find(v.value().ref.objId);
            if (it != call.env.session->heap.end()) cls = it->second.className;
        }
        call.env.pendingThrow = true;
        call.env.throwClass = cls;
        return Result<u64, Bail>::ok(0);
    });
    putHook(env, "ExceptionOccurred", [](JniCall& call) -> Result<u64, Bail> {
        if (!call.env.pendingThrow) return Result<u64, Bail>::ok(0);
        // 假 throwable:空壳对象,类名即异常类名(观测用)。
        const u64 id = call.env.session->nextAllocId++;
        HeapObj obj;
        obj.className = call.env.throwClass;
        call.env.session->heap[id] = std::move(obj);
        return call.env.pool.makeRef(SimValue::knownObj(id), false);
    });
    putHook(env, "ExceptionDescribe", [](JniCall&) -> Result<u64, Bail> {
        return Result<u64, Bail>::ok(0); // 无 stderr 面,no-op
    });
    putHook(env, "ExceptionClear", [](JniCall& call) -> Result<u64, Bail> {
        call.env.pendingThrow = false;
        call.env.throwClass.clear();
        return Result<u64, Bail>::ok(0);
    });
    putHook(env, "ExceptionCheck", [](JniCall& call) -> Result<u64, Bail> {
        return Result<u64, Bail>::ok(call.env.pendingThrow ? 1 : 0);
    });

    // ---- 引用面 ----
    putHook(env, "NewGlobalRef", [](JniCall& call) -> Result<u64, Bail> {
        if (call.args[0] == 0) return Result<u64, Bail>::ok(0);
        Result<SimValue, Bail> v = call.env.pool.deref(call.args[0]);
        if (!v.isOk()) return Result<u64, Bail>::err(v.error());
        return call.env.pool.makeRef(v.value(), true);
    });
    putHook(env, "NewLocalRef", [](JniCall& call) -> Result<u64, Bail> {
        if (call.args[0] == 0) return Result<u64, Bail>::ok(0);
        Result<SimValue, Bail> v = call.env.pool.deref(call.args[0]);
        if (!v.isOk()) return Result<u64, Bail>::err(v.error());
        return call.env.pool.makeRef(v.value(), false);
    });
    putHook(env, "DeleteGlobalRef", [](JniCall& call) -> Result<u64, Bail> {
        call.env.pool.refs.erase(call.args[0]);
        return Result<u64, Bail>::ok(0);
    });
    putHook(env, "DeleteLocalRef", [](JniCall& call) -> Result<u64, Bail> {
        call.env.pool.refs.erase(call.args[0]);
        return Result<u64, Bail>::ok(0);
    });
    putHook(env, "DeleteWeakGlobalRef", [](JniCall& call) -> Result<u64, Bail> {
        call.env.pool.refs.erase(call.args[0]); // 弱引用按强引用建模(v1 简化)
        return Result<u64, Bail>::ok(0);
    });
    putHook(env, "NewWeakGlobalRef", [](JniCall& call) -> Result<u64, Bail> {
        if (call.args[0] == 0) return Result<u64, Bail>::ok(0);
        Result<SimValue, Bail> v = call.env.pool.deref(call.args[0]);
        if (!v.isOk()) return Result<u64, Bail>::err(v.error());
        return call.env.pool.makeRef(v.value(), true);
    });
    putHook(env, "IsSameObject", [](JniCall& call) -> Result<u64, Bail> {
        return Result<u64, Bail>::ok(call.env.pool.sameObject(call.args[0], call.args[1]) ? 1 : 0);
    });
    putHook(env, "EnsureLocalCapacity", [](JniCall&) -> Result<u64, Bail> {
        return Result<u64, Bail>::ok(0); // 池无上限
    });
    putHook(env, "PushLocalFrame", [](JniCall&) -> Result<u64, Bail> {
        return Result<u64, Bail>::ok(0); // 帧嵌套 v1 不分域,统一 popLocals
    });
    putHook(env, "PopLocalFrame", [](JniCall& call) -> Result<u64, Bail> {
        return Result<u64, Bail>::ok(call.args[0]); // result 句柄原样带回
    });

    // ---- 成员 ID ----
    auto memberHook = [](bool isStatic) {
        return [isStatic](JniCall& call) -> Result<u64, Bail> {
            Result<std::string, Bail> cls = detail::classNameOfClass(call.env, call.args[0]);
            if (!cls.isOk()) return Result<u64, Bail>::err(cls.error());
            Result<std::string, Bail> name = detail::readCStr(call.env, call.args[1]);
            if (!name.isOk()) return Result<u64, Bail>::err(name.error());
            Result<std::string, Bail> sig = detail::readCStr(call.env, call.args[2]);
            if (!sig.isOk()) return Result<u64, Bail>::err(sig.error());
            HandlePool::Member m;
            m.owner = cls.value();
            m.name = name.value();
            m.desc = sig.value();
            m.isStatic = isStatic;
            return call.env.pool.makeMember(m);
        };
    };
    putHook(env, "GetMethodID", memberHook(false));
    putHook(env, "GetStaticMethodID", memberHook(true));
    putHook(env, "GetFieldID", memberHook(false));
    putHook(env, "GetStaticFieldID", memberHook(true));

    // ---- Call*Method:十类型 × (bare/V/A) × (实例/静态),同名共用实现 ----
    static const char* kMethodTypes[] = {"Object", "Boolean", "Byte",  "Char",   "Short",
                                         "Int",    "Long",    "Float", "Double", "Void"};
    auto installCallFamily = [&env](bool isStatic, int variant) {
        const char* stem = isStatic ? "CallStatic" : "Call";
        const char* tail = variant == 0 ? "" : (variant == 1 ? "V" : "A");
        for (const char* t : kMethodTypes) {
            putHook(env, (std::string(stem) + t + "Method" + tail).c_str(),
                    [isStatic, variant](JniCall& call) -> Result<u64, Bail> {
                        return callMethodImpl(call, isStatic, variant);
                    });
        }
    };
    installCallFamily(false, 0);  // Call*Method
    installCallFamily(false, 1);  // Call*MethodV
    installCallFamily(false, 2);  // Call*MethodA
    installCallFamily(true, 0);   // CallStatic*Method
    installCallFamily(true, 1);
    installCallFamily(true, 2);

    auto installNewObjectFamily = [&env](int variant) {
        std::string name = std::string("NewObject") + (variant == 0 ? "" : (variant == 1 ? "V" : "A"));
        putHook(env, name.c_str(), [variant](JniCall& call) -> Result<u64, Bail> {
            return newObjectImpl(call, variant);
        });
    };
    installNewObjectFamily(0);
    installNewObjectFamily(1);
    installNewObjectFamily(2);

    // ---- 字段读写:类型由 fieldID 的 desc 决定,九形共用四个实现 ----
    auto getStaticField = [](JniCall& call) -> Result<u64, Bail> {
        Result<const HandlePool::Member*, Bail> f = call.env.pool.memberOf(call.args[1]);
        if (!f.isOk()) return Result<u64, Bail>::err(f.error());
        const std::string key = f.value()->owner + "::" + f.value()->name + "::" + f.value()->desc;
        auto it = call.env.session->statics.find(key);
        if (it == call.env.session->statics.end()) {
            // native 侧无法表达 Unknown:缺值即断链,消费方用 Session.statics 预填。
            return Result<u64, Bail>::err({Bail::Reason::NotConcrete, 0,
                                           "静态字段无值(预填 Session.statics):" + key});
        }
        return detail::encodeByValType(it->second, call.env);
    };
    auto setStaticField = [](JniCall& call) -> Result<u64, Bail> {
        Result<const HandlePool::Member*, Bail> f = call.env.pool.memberOf(call.args[1]);
        if (!f.isOk()) return Result<u64, Bail>::err(f.error());
        const std::string key = f.value()->owner + "::" + f.value()->name + "::" + f.value()->desc;
        Result<std::pair<ValType, bool>, Bail> rt = retTypeOfDesc("(x)" + f.value()->desc);
        if (!rt.isOk()) return Result<u64, Bail>::err(rt.error());
        Result<SimValue, Bail> v = detail::decodeByValType(rt.value().first, call.args[2], call.env);
        if (!v.isOk()) return Result<u64, Bail>::err(v.error());
        call.env.session->statics[key] = std::move(v.value());
        return Result<u64, Bail>::ok(0);
    };
    auto getInstanceField = [](JniCall& call) -> Result<u64, Bail> {
        Result<SimValue, Bail> recv = call.env.pool.deref(call.args[0]);
        if (!recv.isOk()) return Result<u64, Bail>::err(recv.error());
        if (recv.value().ref.kind == RefPayload::Kind::Null) {
            return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "getXField(null) 需 NPE 派发"});
        }
        if (recv.value().ref.kind != RefPayload::Kind::Obj) {
            return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "接收者无实例字段面"});
        }
        Result<const HandlePool::Member*, Bail> f = call.env.pool.memberOf(call.args[1]);
        if (!f.isOk()) return Result<u64, Bail>::err(f.error());
        auto it = call.env.session->heap.find(recv.value().ref.objId);
        if (it == call.env.session->heap.end()) {
            return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "接收者堆对象缺失"});
        }
        auto slot = it->second.fields.find(f.value()->name + "::" + f.value()->desc);
        if (slot == it->second.fields.end()) {
            return Result<u64, Bail>::err({Bail::Reason::NotConcrete, 0,
                                           "实例字段无值:" + f.value()->name});
        }
        return detail::encodeByValType(slot->second, call.env);
    };
    auto setInstanceField = [](JniCall& call) -> Result<u64, Bail> {
        Result<SimValue, Bail> recv = call.env.pool.deref(call.args[0]);
        if (!recv.isOk()) return Result<u64, Bail>::err(recv.error());
        if (recv.value().ref.kind == RefPayload::Kind::Null) {
            return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "putXField(null) 需 NPE 派发"});
        }
        if (recv.value().ref.kind != RefPayload::Kind::Obj) {
            return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "接收者无实例字段面"});
        }
        Result<const HandlePool::Member*, Bail> f = call.env.pool.memberOf(call.args[1]);
        if (!f.isOk()) return Result<u64, Bail>::err(f.error());
        Result<std::pair<ValType, bool>, Bail> rt = retTypeOfDesc("(x)" + f.value()->desc);
        if (!rt.isOk()) return Result<u64, Bail>::err(rt.error());
        Result<SimValue, Bail> v = detail::decodeByValType(rt.value().first, call.args[2], call.env);
        if (!v.isOk()) return Result<u64, Bail>::err(v.error());
        auto it = call.env.session->heap.find(recv.value().ref.objId);
        if (it == call.env.session->heap.end()) {
            return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "接收者堆对象缺失"});
        }
        it->second.fields[f.value()->name + "::" + f.value()->desc] = std::move(v.value());
        return Result<u64, Bail>::ok(0);
    };
    static const char* kFieldTypes[] = {"Object", "Boolean", "Byte",  "Char",   "Short",
                                        "Int",    "Long",    "Float", "Double"};
    for (const char* t : kFieldTypes) {
        putHook(env, (std::string("GetStatic") + t + "Field").c_str(), getStaticField);
        putHook(env, (std::string("SetStatic") + t + "Field").c_str(), setStaticField);
        putHook(env, (std::string("Get") + t + "Field").c_str(), getInstanceField);
        putHook(env, (std::string("Set") + t + "Field").c_str(), setInstanceField);
    }

    // ---- 字符串 ----
    putHook(env, "NewStringUTF", [](JniCall& call) -> Result<u64, Bail> {
        if (call.args[0] == 0) return Result<u64, Bail>::ok(0);
        Result<std::string, Bail> s = detail::readCStr(call.env, call.args[0]);
        if (!s.isOk()) return Result<u64, Bail>::err(s.error());
        return call.env.pool.makeRef(SimValue::knownStr(std::move(s.value())), false);
    });
    putHook(env, "GetStringUTFChars", [](JniCall& call) -> Result<u64, Bail> {
        if (call.args[0] == 0) return Result<u64, Bail>::ok(0);
        Result<std::string, Bail> s = strPayloadOf(call.env, call.args[0]);
        if (!s.isOk()) return Result<u64, Bail>::err(s.error());
        std::string buf = s.value();
        buf.push_back('\0'); // JNI 契约:NUL 结尾
        Result<u64, Bail> p = materialize(call.env, buf.data(), buf.size());
        if (!p.isOk()) return Result<u64, Bail>::err(p.error());
        detail::markCopy(call.env, call.args[1]);
        return Result<u64, Bail>::ok(p.value());
    });
    putHook(env, "ReleaseStringUTFChars", [](JniCall& call) -> Result<u64, Bail> {
        if (call.args[1] != 0 && call.env.mem.free != nullptr) call.env.mem.free(call.args[1]);
        return Result<u64, Bail>::ok(0);
    });
    putHook(env, "GetStringUTFLength", [](JniCall& call) -> Result<u64, Bail> {
        Result<std::string, Bail> s = strPayloadOf(call.env, call.args[0]);
        if (!s.isOk()) return Result<u64, Bail>::err(s.error());
        return Result<u64, Bail>::ok(s.value().size()); // 不含结尾 NUL(对齐 JDK)
    });
    putHook(env, "NewString", [](JniCall& call) -> Result<u64, Bail> {
        if (call.args[0] == 0) return Result<u64, Bail>::ok(0);
        const usize len = static_cast<usize>(static_cast<i32>(call.args[1]));
        Result<std::vector<u8>, Bail> raw = detail::readBytes(call.env, call.args[0], len * 2);
        if (!raw.isOk()) return Result<u64, Bail>::err(raw.error());
        std::vector<u16> units(len);
        for (usize i = 0; i < len; ++i) {
            units[i] = static_cast<u16>(raw.value()[i * 2] | (raw.value()[i * 2 + 1] << 8)); // UTF-16LE
        }
        return call.env.pool.makeRef(SimValue::knownStr(detail::utf16ToMutf8(units)), false);
    });
    putHook(env, "GetStringLength", [](JniCall& call) -> Result<u64, Bail> {
        Result<std::string, Bail> s = strPayloadOf(call.env, call.args[0]);
        if (!s.isOk()) return Result<u64, Bail>::err(s.error());
        Result<std::vector<u16>, Bail> units = detail::mutf8ToUtf16(s.value());
        if (!units.isOk()) return Result<u64, Bail>::err(units.error());
        return Result<u64, Bail>::ok(units.value().size());
    });
    putHook(env, "GetStringChars", [](JniCall& call) -> Result<u64, Bail> {
        Result<std::string, Bail> s = strPayloadOf(call.env, call.args[0]);
        if (!s.isOk()) return Result<u64, Bail>::err(s.error());
        Result<std::vector<u16>, Bail> units = detail::mutf8ToUtf16(s.value());
        if (!units.isOk()) return Result<u64, Bail>::err(units.error());
        std::vector<u8> buf;
        buf.reserve(units.value().size() * 2 + 2);
        for (u16 u : units.value()) {
            buf.push_back(static_cast<u8>(u & 0xFF));
            buf.push_back(static_cast<u8>(u >> 8));
        }
        buf.push_back(0);
        buf.push_back(0);
        Result<u64, Bail> p = materialize(call.env, buf.data(), buf.size(), 2);
        if (!p.isOk()) return Result<u64, Bail>::err(p.error());
        detail::markCopy(call.env, call.args[1]);
        return Result<u64, Bail>::ok(p.value());
    });
    putHook(env, "ReleaseStringChars", [](JniCall& call) -> Result<u64, Bail> {
        if (call.args[1] != 0 && call.env.mem.free != nullptr) call.env.mem.free(call.args[1]);
        return Result<u64, Bail>::ok(0);
    });
    putHook(env, "GetStringUTFRegion", [](JniCall& call) -> Result<u64, Bail> {
        Result<std::string, Bail> s = strPayloadOf(call.env, call.args[0]);
        if (!s.isOk()) return Result<u64, Bail>::err(s.error());
        const i32 start = static_cast<i32>(call.args[1]);
        const i32 len = static_cast<i32>(call.args[2]);
        if (start < 0 || len < 0 || static_cast<usize>(start) + static_cast<usize>(len) > s.value().size()) {
            return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "GetStringUTFRegion 区间越界"});
        }
        Result<void, Bail> w =
            detail::writeBytes(call.env, call.args[3], s.value().data() + start, static_cast<usize>(len));
        if (!w.isOk()) return Result<u64, Bail>::err(w.error());
        return Result<u64, Bail>::ok(0);
    });
    putHook(env, "GetStringRegion", [](JniCall& call) -> Result<u64, Bail> {
        Result<std::string, Bail> s = strPayloadOf(call.env, call.args[0]);
        if (!s.isOk()) return Result<u64, Bail>::err(s.error());
        Result<std::vector<u16>, Bail> units = detail::mutf8ToUtf16(s.value());
        if (!units.isOk()) return Result<u64, Bail>::err(units.error());
        const i32 start = static_cast<i32>(call.args[1]);
        const i32 len = static_cast<i32>(call.args[2]);
        if (start < 0 || len < 0 || static_cast<usize>(start) + static_cast<usize>(len) > units.value().size()) {
            return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "GetStringRegion 区间越界"});
        }
        std::vector<u8> buf;
        buf.reserve(static_cast<usize>(len) * 2);
        for (i32 i = start; i < start + len; ++i) {
            const u16 u = units.value()[static_cast<usize>(i)];
            buf.push_back(static_cast<u8>(u & 0xFF));
            buf.push_back(static_cast<u8>(u >> 8));
        }
        Result<void, Bail> w = detail::writeBytes(call.env, call.args[3], buf.data(), buf.size());
        if (!w.isOk()) return Result<u64, Bail>::err(w.error());
        return Result<u64, Bail>::ok(0);
    });
    putHook(env, "GetStringCritical", [](JniCall& call) -> Result<u64, Bail> {
        Result<std::string, Bail> s = strPayloadOf(call.env, call.args[0]);
        if (!s.isOk()) return Result<u64, Bail>::err(s.error());
        std::string buf = s.value();
        buf.push_back('\0');
        Result<u64, Bail> p = materialize(call.env, buf.data(), buf.size());
        if (!p.isOk()) return Result<u64, Bail>::err(p.error());
        detail::markCopy(call.env, call.args[1]);
        return Result<u64, Bail>::ok(p.value());
    });
    putHook(env, "ReleaseStringCritical", [](JniCall& call) -> Result<u64, Bail> {
        if (call.args[1] != 0 && call.env.mem.free != nullptr) call.env.mem.free(call.args[1]);
        return Result<u64, Bail>::ok(0);
    });

    // ---- 数组 ----
    auto newArrayHook = [](const char* cls) {
        return [cls](JniCall& call) -> Result<u64, Bail> {
            const i32 len = static_cast<i32>(call.args[0]);
            if (len < 0) {
                return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "负数组长度"});
            }
            const u64 id = call.env.session->nextAllocId++;
            HeapObj obj;
            obj.className = cls;
            obj.elems.assign(static_cast<usize>(len), 0);
            call.env.session->heap[id] = std::move(obj);
            return call.env.pool.makeRef(SimValue::knownObj(id), false);
        };
    };
    putHook(env, "NewBooleanArray", newArrayHook("[Z"));
    putHook(env, "NewByteArray", newArrayHook("[B"));
    putHook(env, "NewCharArray", newArrayHook("[C"));
    putHook(env, "NewShortArray", newArrayHook("[S"));
    putHook(env, "NewIntArray", newArrayHook("[I"));
    putHook(env, "NewLongArray", newArrayHook("[J"));
    putHook(env, "NewFloatArray", newArrayHook("[F"));
    putHook(env, "NewDoubleArray", newArrayHook("[D"));

    putHook(env, "GetArrayLength", [](JniCall& call) -> Result<u64, Bail> {
        Result<SimValue, Bail> v = call.env.pool.deref(call.args[0]);
        if (!v.isOk()) return Result<u64, Bail>::err(v.error());
        if (v.value().ref.kind != RefPayload::Kind::Obj) {
            return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "GetArrayLength 接收者非数组"});
        }
        auto it = call.env.session->heap.find(v.value().ref.objId);
        if (it == call.env.session->heap.end()) {
            return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "GetArrayLength 堆对象缺失"});
        }
        const HeapObj& obj = it->second;
        const usize n = detail::isRefArrayClass(obj.className) ? obj.refs.size() : obj.elems.size();
        return Result<u64, Bail>::ok(n);
    });
    putHook(env, "NewObjectArray", [](JniCall& call) -> Result<u64, Bail> {
        const i32 len = static_cast<i32>(call.args[0]);
        if (len < 0) return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "负数组长度"});
        Result<std::string, Bail> elem = detail::classNameOfClass(call.env, call.args[1]);
        if (!elem.isOk()) return Result<u64, Bail>::err(elem.error());
        Result<SimValue, Bail> init = call.env.pool.deref(call.args[2]);
        if (!init.isOk()) return Result<u64, Bail>::err(init.error());
        const u64 id = call.env.session->nextAllocId++;
        HeapObj obj;
        obj.className = "[L" + elem.value() + ";";
        obj.refs.assign(static_cast<usize>(len), init.value());
        call.env.session->heap[id] = std::move(obj);
        return call.env.pool.makeRef(SimValue::knownObj(id), false);
    });
    putHook(env, "GetObjectArrayElement", [](JniCall& call) -> Result<u64, Bail> {
        Result<SimValue, Bail> v = call.env.pool.deref(call.args[0]);
        if (!v.isOk()) return Result<u64, Bail>::err(v.error());
        auto it = call.env.session->heap.find(v.value().ref.objId);
        if (it == call.env.session->heap.end()) {
            return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "数组堆对象缺失"});
        }
        const usize idx = static_cast<usize>(static_cast<i32>(call.args[1]));
        if (idx >= it->second.refs.size()) {
            return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "数组下标越界"});
        }
        return call.env.pool.makeRef(it->second.refs[idx], false);
    });
    putHook(env, "SetObjectArrayElement", [](JniCall& call) -> Result<u64, Bail> {
        Result<SimValue, Bail> v = call.env.pool.deref(call.args[0]);
        if (!v.isOk()) return Result<u64, Bail>::err(v.error());
        auto it = call.env.session->heap.find(v.value().ref.objId);
        if (it == call.env.session->heap.end()) {
            return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "数组堆对象缺失"});
        }
        const usize idx = static_cast<usize>(static_cast<i32>(call.args[1]));
        if (idx >= it->second.refs.size()) {
            return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "数组下标越界"});
        }
        Result<SimValue, Bail> val = call.env.pool.deref(call.args[2]);
        if (!val.isOk()) return Result<u64, Bail>::err(val.error());
        it->second.refs[idx] = std::move(val.value());
        return Result<u64, Bail>::ok(0);
    });

    // 元素宽与符号性由数组类名决定(引擎同款口径);Region 系直通堆不经副本。
    auto regionHook = [](bool isSet) {
        return [isSet](JniCall& call) -> Result<u64, Bail> {
            Result<SimValue, Bail> v = call.env.pool.deref(call.args[0]);
            if (!v.isOk()) return Result<u64, Bail>::err(v.error());
            auto it = call.env.session->heap.find(v.value().ref.objId);
            if (it == call.env.session->heap.end()) {
                return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "数组堆对象缺失"});
            }
            HeapObj& arr = it->second;
            if (detail::isRefArrayClass(arr.className)) {
                return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "引用数组不走 Region 面"});
            }
            const usize w = detail::elemWidthOf(arr.className);
            const i32 start = static_cast<i32>(call.args[1]);
            const i32 len = static_cast<i32>(call.args[2]);
            if (start < 0 || len < 0 || static_cast<usize>(start) + static_cast<usize>(len) > arr.elems.size()) {
                return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "数组区间越界"});
            }
            const usize nBytes = static_cast<usize>(len) * w;
            if (!isSet) {
                std::vector<u8> buf(nBytes);
                for (i32 i = 0; i < len; ++i) {
                    const u64 raw = arr.elems[static_cast<usize>(start) + static_cast<usize>(i)];
                    for (usize b = 0; b < w; ++b) {
                        buf[static_cast<usize>(i) * w + b] = static_cast<u8>((raw >> (b * 8)) & 0xFF);
                    }
                }
                Result<void, Bail> w = detail::writeBytes(call.env, call.args[3], buf.data(), buf.size());
                if (!w.isOk()) return Result<u64, Bail>::err(w.error());
                return Result<u64, Bail>::ok(0);
            }
            std::vector<u8> buf(nBytes);
            Result<void, Bail> r = call.env.mem.read(call.args[3], buf.data(), buf.size());
            if (!r.isOk()) return Result<u64, Bail>::err(r.error());
            for (i32 i = 0; i < len; ++i) {
                u64 raw = 0;
                for (usize b = 0; b < w; ++b) {
                    raw |= static_cast<u64>(buf[static_cast<usize>(i) * w + b]) << (b * 8);
                }
                arr.elems[static_cast<usize>(start) + static_cast<usize>(i)] = static_cast<i64>(raw);
            }
            return Result<u64, Bail>::ok(0);
        };
    };
    putHook(env, "GetBooleanArrayRegion", regionHook(false));
    putHook(env, "GetByteArrayRegion", regionHook(false));
    putHook(env, "GetCharArrayRegion", regionHook(false));
    putHook(env, "GetShortArrayRegion", regionHook(false));
    putHook(env, "GetIntArrayRegion", regionHook(false));
    putHook(env, "GetLongArrayRegion", regionHook(false));
    putHook(env, "GetFloatArrayRegion", regionHook(false));
    putHook(env, "GetDoubleArrayRegion", regionHook(false));
    putHook(env, "SetBooleanArrayRegion", regionHook(true));
    putHook(env, "SetByteArrayRegion", regionHook(true));
    putHook(env, "SetCharArrayRegion", regionHook(true));
    putHook(env, "SetShortArrayRegion", regionHook(true));
    putHook(env, "SetIntArrayRegion", regionHook(true));
    putHook(env, "SetLongArrayRegion", regionHook(true));
    putHook(env, "SetFloatArrayRegion", regionHook(true));
    putHook(env, "SetDoubleArrayRegion", regionHook(true));

    // Elements/Critical:副本物化,Release 按 mode 写回(0/1 写回,JNI_ABORT 丢弃)。
    auto getElementsHook = [](JniCall& call) -> Result<u64, Bail> {
        Result<SimValue, Bail> v = call.env.pool.deref(call.args[0]);
        if (!v.isOk()) return Result<u64, Bail>::err(v.error());
        auto it = call.env.session->heap.find(v.value().ref.objId);
        if (it == call.env.session->heap.end()) {
            return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "数组堆对象缺失"});
        }
        HeapObj& arr = it->second;
        if (detail::isRefArrayClass(arr.className)) {
            return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "引用数组不走 Elements 面"});
        }
        const usize w = detail::elemWidthOf(arr.className);
        std::vector<u8> buf(arr.elems.size() * w);
        for (usize i = 0; i < arr.elems.size(); ++i) {
            for (usize b = 0; b < w; ++b) {
                buf[i * w + b] = static_cast<u8>((static_cast<u64>(arr.elems[i]) >> (b * 8)) & 0xFF);
            }
        }
        Result<u64, Bail> p = materialize(call.env, buf.data(), buf.size(), static_cast<usize>(w));
        if (!p.isOk()) return Result<u64, Bail>::err(p.error());
        detail::markCopy(call.env, call.args[1]);
        return Result<u64, Bail>::ok(p.value());
    };
    auto releaseElementsHook = [](JniCall& call) -> Result<u64, Bail> {
        const u64 mode = call.args[2];
        if ((mode & 1) != 0) return Result<u64, Bail>::ok(0); // JNI_ABORT:丢弃副本
        Result<SimValue, Bail> v = call.env.pool.deref(call.args[0]);
        if (!v.isOk()) return Result<u64, Bail>::err(v.error());
        auto it = call.env.session->heap.find(v.value().ref.objId);
        if (it == call.env.session->heap.end()) {
            return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "数组堆对象缺失"});
        }
        HeapObj& arr = it->second;
        const usize w = detail::elemWidthOf(arr.className);
        const usize n = arr.elems.size();
        std::vector<u8> buf(n * w);
        Result<void, Bail> r = call.env.mem.read(call.args[1], buf.data(), buf.size());
        if (!r.isOk()) return Result<u64, Bail>::err(r.error());
        for (usize i = 0; i < n; ++i) {
            u64 raw = 0;
            for (usize b = 0; b < w; ++b) raw |= static_cast<u64>(buf[i * w + b]) << (b * 8);
            arr.elems[i] = static_cast<i64>(raw);
        }
        if (call.env.mem.free != nullptr) call.env.mem.free(call.args[1]);
        return Result<u64, Bail>::ok(0);
    };
    putHook(env, "GetBooleanArrayElements", getElementsHook);
    putHook(env, "GetByteArrayElements", getElementsHook);
    putHook(env, "GetCharArrayElements", getElementsHook);
    putHook(env, "GetShortArrayElements", getElementsHook);
    putHook(env, "GetIntArrayElements", getElementsHook);
    putHook(env, "GetLongArrayElements", getElementsHook);
    putHook(env, "GetFloatArrayElements", getElementsHook);
    putHook(env, "GetDoubleArrayElements", getElementsHook);
    putHook(env, "ReleaseBooleanArrayElements", releaseElementsHook);
    putHook(env, "ReleaseByteArrayElements", releaseElementsHook);
    putHook(env, "ReleaseCharArrayElements", releaseElementsHook);
    putHook(env, "ReleaseShortArrayElements", releaseElementsHook);
    putHook(env, "ReleaseIntArrayElements", releaseElementsHook);
    putHook(env, "ReleaseLongArrayElements", releaseElementsHook);
    putHook(env, "ReleaseFloatArrayElements", releaseElementsHook);
    putHook(env, "ReleaseDoubleArrayElements", releaseElementsHook);
    putHook(env, "GetPrimitiveArrayCritical", getElementsHook);
    putHook(env, "ReleasePrimitiveArrayCritical", releaseElementsHook);

    // ---- 杂项 ----
    putHook(env, "RegisterNatives", [](JniCall& call) -> Result<u64, Bail> {
        Result<std::string, Bail> cls = detail::classNameOfClass(call.env, call.args[0]);
        if (!cls.isOk()) return Result<u64, Bail>::err(cls.error());
        const u64 tableAddr = call.args[1];
        const i32 n = static_cast<i32>(call.args[2]);
        // JNINativeMethod = { const char* name; const char* sig; void* fnPtr; } × 8 字节
        for (i32 i = 0; i < n; ++i) {
            u64 row[3] = {0, 0, 0};
            Result<void, Bail> r = call.env.mem.read(tableAddr + static_cast<usize>(i) * 24, row, 24);
            if (!r.isOk()) return Result<u64, Bail>::err(r.error());
            Result<std::string, Bail> name = detail::readCStr(call.env, row[0]);
            if (!name.isOk()) return Result<u64, Bail>::err(name.error());
            Result<std::string, Bail> sig = detail::readCStr(call.env, row[1]);
            if (!sig.isOk()) return Result<u64, Bail>::err(sig.error());
            NativeSpec spec;
            spec.entry = row[2];
            spec.abi = call.env.currentAbi;
            call.env.methods[cls.value() + "." + name.value() + sig.value()] = spec;
        }
        return Result<u64, Bail>::ok(0);
    });
    putHook(env, "UnregisterNatives", [](JniCall&) -> Result<u64, Bail> {
        return Result<u64, Bail>::ok(0); // v1 不清表(j2c 注册一次跑全程)
    });
    putHook(env, "MonitorEnter", [](JniCall&) -> Result<u64, Bail> {
        return Result<u64, Bail>::ok(0); // monitor 不进模型(设计 §11)
    });
    putHook(env, "MonitorExit", [](JniCall&) -> Result<u64, Bail> {
        return Result<u64, Bail>::ok(0);
    });
    putHook(env, "GetJavaVM", [](JniCall& call) -> Result<u64, Bail> {
        const u64 vm = HandlePool::kJavaVm;
        Result<void, Bail> r = call.env.mem.write(call.args[0], &vm, 8);
        if (!r.isOk()) return Result<u64, Bail>::err(r.error());
        return Result<u64, Bail>::ok(0); // JNI_OK
    });
    putHook(env, "AllocObject", [](JniCall& call) -> Result<u64, Bail> {
        Result<std::string, Bail> cls = detail::classNameOfClass(call.env, call.args[0]);
        if (!cls.isOk()) return Result<u64, Bail>::err(cls.error());
        const u64 id = call.env.session->nextAllocId++;
        HeapObj obj;
        obj.className = cls.value();
        call.env.session->heap[id] = std::move(obj);
        return call.env.pool.makeRef(SimValue::knownObj(id), false);
    });
    putHook(env, "IsInstanceOf", [](JniCall& call) -> Result<u64, Bail> {
        // 无类型层次:同对象判真,其余判假并留观测(docs/ext-jni.md 口径)。
        const bool same = call.env.pool.sameObject(call.args[0], call.args[1]);
        if (!same && call.env.trace) call.env.trace("[JNI] IsInstanceOf 无类型层次,按身份判定");
        return Result<u64, Bail>::ok(same ? 1 : 0);
    });
}

// ---------------------------------------------------------------------------
// install:接缝
// ---------------------------------------------------------------------------

/// 安装接缝:注册默认骨架 + 向 callMockResolvers 兜底解析链挂 native 解析器。
/// 只认领 methods 里登记的方法;callMocks / callMocksAtPc 优先级天然更高,用户
/// 可强行 mock 掉某个 native 不进机器。幂等:重复调用只覆盖骨架补缺与缓存填充。
inline void install(Session& session, JniEnv& env)
{
    installSkeletons(env);
    if (env.session == nullptr) env.session = &session;
    session.callMockResolvers.push_back([&session, &env](const CallRef& callee, Frame&) -> CallMock* {
        const std::string key = callee.owner + "." + callee.name + callee.desc;
        auto specIt = env.methods.find(key);
        if (specIt == env.methods.end()) return nullptr; // 放行:核心照常 Bail{UnsupportedCall}
        auto& slot = env.nativeMocks[key];
        if (!slot) slot = makeNativeMock(env, specIt->second, callee);
        return &slot;
    });
}

} // namespace jni
} // namespace jsim
