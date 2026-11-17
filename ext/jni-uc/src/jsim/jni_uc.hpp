/// @file jni_uc.hpp
/// @defgroup jni_uc jsim-jni unicorn 绑定层
/// @brief jsim-jni 绑定层(doc:ext-jni):unicorn x86-64(Win64 + SysV)。
///
/// 职责只有三件,JNI 语义全在纯层(jni.hpp):
/// 1. 物化:JNIEnv 函数表与 trap page 落进 guest 内存。表第 i 项指向 trap page
///    内唯一地址,机器码调表即落 stub,UC_HOOK_CODE 截获后按地址差还原函数号。
/// 2. ABI 编组:Win64 位置制(前四槽 = RCX/RDX/R8/R9 与 XMM0..3 同槽,溢出实参
///    自第 5 槽落栈,先垫 0x20 影子空间);SysV 类计数制(整数 6 + 浮点 8 各自按
///    序,耗尽后按落栈序占 8 字节槽);返回值按 FuncMeta::Ret 写 RAX 或 XMM0。
///    bare Call*Method 这类变长入口按 Win64 变长约定浮点走整数文件。
/// 3. 熔断与停机:uc_emu_start 步数上限(JniEnv::maxSteps)防死循环;guest 坏
///    访存(含解引用句柄——句柄区故意不映射)转 Bail{BadOperand}。
///
/// 包不做二进制加载:消费方用 mapCode 灌映射好的代码(PE/ELF 展平段或裸机器
/// 码),入口地址进 JniEnv::methods。install 前把 memFace() 灌进 env.mem、
/// executorFace() 灌进 env.executor:
///   jsim::jniuc::UcNative uc(env);
///   env.mem = uc.memFace();
///   env.executor = uc.executorFace();
///   uc.mapCode(0x00010000, blob, size);
#pragma once

#include <unicorn/unicorn.h>

#include <memory>

#include "jsim/engine.hpp"
#include "jsim/jni.hpp"

namespace jsim {
namespace jniuc {

/// UC_* 错误码 → 结构化 Bail。guest 侧错误统一 BadOperand/NotConcrete 口径。
Bail ucBail(const char* what, uc_err err);

class UcNative
{
public:
    explicit UcNative(jni::JniEnv& env, jni::Layout layout = jni::Layout{});
    ~UcNative();

    UcNative(const UcNative&) = delete;
    UcNative& operator=(const UcNative&) = delete;

    /// GuestMemory 实现:map 走 scratch 区 bump + 回收链,read/write 走
    /// uc_mem_read/write。灌进 env.mem。
    jni::GuestMemory memFace();

    /// NativeExecutor 实现。灌进 env.executor.run。
    jni::NativeExecutor executorFace();

    /// 映射并灌入一段代码(可多次;起始地址按 4K 页对齐由消费方保证)。
    Result<void, Bail> mapCode(u64 addr, const void* data, usize size);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace jniuc
} // namespace jsim
