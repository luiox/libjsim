/// @file jni.hpp
/// @defgroup jni jsim-jni 本地方法纯层
/// @brief jsim-jni 扩展包纯层(doc:ext-jni):本地方法接管的接缝与
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
/// - 句柄池、字段/类桥接、异常位、观测口径见 doc:ext-jni。
///
/// 接入:
///   jsim::jni::JniEnv env;
///   env.session = \&session;
///   env.mem = ...;                  // GuestMemory 实现(绑定层或测试)
///   env.executor.run = ...;         // 本机执行器(绑定层或测试)
///   env.methods["sample/D.f()I"] = {entry, abi};   // 键 = 内部名.方法名+desc
///   jsim::jni::install(session, env);
#pragma once

#include <cstdint>
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

const std::vector<FuncMeta>& funcTable();

const FuncMeta* metaOf(u32 id);

/// 按名查函数号;找不到返回 kInvalidFunc。
constexpr u32 kInvalidFunc = 0xFFFFFFFFu;
u32 indexOf(std::string_view name);

// ---------------------------------------------------------------------------
// 句柄池
// ---------------------------------------------------------------------------

/// jobject 形状的假 guest 指针。不对应任何真实内存——绑定层不为它映射页,
/// 机器码解引用句柄会落到未映射读,由绑定层转 Bail{BadOperand}。
struct HandlePool
{
    /// 句柄发号区起点。必须落在 scratch 物化区(Layout::scratch + scratchSize)
    /// 之后:句柄区不映射,若与 scratch 重叠,解引用句柄会读到物化数据而不是
    /// 落未映射读 Bail(D14 的失败模式就废了)。
    static constexpr u64 kBase = 0x00410000;
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
    Result<u64, Bail> makeRef(SimValue v, bool global);

    /// 解句柄。0 按 JNI null 口径回已知 null;悬空即 Bail,不静默给默认对象。
    Result<SimValue, Bail> deref(u64 h) const;

    Result<u64, Bail> makeMember(const Member& m);

    Result<const Member*, Bail> memberOf(u64 h) const;

    /// 一次 native 调用结束回收 local 域;global 留给下一次调用(缓存 jclass)。
    void popLocals();

    usize alive() const;

    /// IsSameObject 口径:同句柄、双 null 或同 objId 判真;Str 载荷只在同句柄时
    /// 判真(动态字符串身份不可判是既定口径)。
    bool sameObject(u64 a, u64 b) const;
};

// ---------------------------------------------------------------------------
// JniEnv 与分发
// ---------------------------------------------------------------------------

/// 描述符 → 参数类型序列。I/Z/B/C/S → I32,J → I64,F → F32,D → F64,L…;/[ → Ref。
Result<std::vector<ValType>, Bail> argTypesOfDesc(std::string_view desc);

/// 描述符返回类型;void 调用 hasReturn=false。
Result<std::pair<ValType, bool>, Bail> retTypeOfDesc(std::string_view desc);

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
bool throwableSafe(const char* name);

std::string hexOf(u64 v);

/// 一次 JNI 调用的参数类布局:C 位置 p 是否浮点类。定长函数全整;bare Call*
/// Method / NewObject 变体按 jmethodID 的 desc 展开。绑定层据此把「位置 + 类」
/// 映到寄存器文件(Win64/SysV 的整数与浮点寄存器各自按序消费)。
Result<std::vector<char>, Bail> argClassesOf(JniEnv& env, u32 id, u64 midHandle);

/// 分发一次 JNI 调用。argI/argF 由执行器按 ABI 实现并各自闭包参数类布局
/// (argClassesOf 的产物;unicorn:寄存器 + 栈,FakeMachine:预编脚本)。
/// 返回 guest 位型写回返回寄存器。钩子缺省一律 Bail{UnsupportedCall}——补什么
/// 由用户决定。
Result<u64, Bail> dispatch(JniEnv& env, u32 id,
                                  const std::function<Result<u64, Bail>(usize)>& argI,
                                  const std::function<Result<u64, Bail>(usize)>& argF);

// ---------------------------------------------------------------------------
// 编组:native CallMock(jsim 帧 ↔ 位型)
// ---------------------------------------------------------------------------

/// 弹参编组:接收者(非 static)先收,再按 desc 逆序弹实参。不具体的值过不了
/// JNI 边界——Bail{NotConcrete},与「不编造」同一纪律。
Result<NativeRun, Bail> buildNativeRun(JniEnv& env, const NativeSpec& spec, const CallRef& callee,
                                              Frame& frame);

/// native CallMock:弹参编组 → 执行器跑机器码 → 返回解码 → local 回收 → 异常位。
CallMock makeNativeMock(JniEnv& env, const NativeSpec& spec, const CallRef& callee);

// ---------------------------------------------------------------------------
// 默认骨架
// ---------------------------------------------------------------------------

/// 只补缺:用户预置的钩子不被覆盖(用户钩子 > 默认骨架)。
void putHook(JniEnv& env, const char* name, JniHook fn);

/// Call*Method 家族共用实现。variant:0=bare(变长在寄存器/栈) 1=V 2=A(jvalue*)。
Result<u64, Bail> callMethodImpl(JniCall& call, bool isStatic, int variant);

/// NewObject 家族:分配空壳(类名来自 jmethodID 的 owner),\<init> 经 javaInvoker
/// 跑(构造语义属消费方)。variant 同 callMethodImpl。
Result<u64, Bail> newObjectImpl(JniCall& call, int variant);

/// 注册默认骨架。约 40 个函数按解密场景的调用频度选;覆盖面见 doc:ext-jni。
/// 幂等:只补缺,不覆盖用户钩子。
void installSkeletons(JniEnv& env);

// ---------------------------------------------------------------------------
// install:接缝
// ---------------------------------------------------------------------------

/// 安装接缝:注册默认骨架 + 向 callMockResolvers 兜底解析链挂 native 解析器。
/// 只认领 methods 里登记的方法;callMocks / callMocksAtPc 优先级天然更高,用户
/// 可强行 mock 掉某个 native 不进机器。幂等:重复调用只覆盖骨架补缺与缓存填充。
void install(Session& session, JniEnv& env);

} // namespace jni
} // namespace jsim
