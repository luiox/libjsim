/// @file jni_uc.hpp
/// @brief jsim-jni 绑定层(docs/ext-jni.md):unicorn x86-64(Win64 + SysV)。
///
/// 职责只有三件,JNI 语义全在纯层(jni.hpp):
/// 1. 物化:JNIEnv 函数表与 trap page 落进 guest 内存。表第 i 项指向 trap page
///    内唯一地址,机器码调表即落 stub,UC_HOOK_CODE 截获后按地址差还原函数号。
/// 2. ABI 编组:整数/浮点寄存器文件各按序消费(Win64 4+4,SysV 6+8),溢出实参
///    按 C 位置落栈(Win64 先垫 0x20 影子空间);返回值按 FuncMeta::Ret 写 RAX
///    或 XMM0。bare Call*Method 这类变长入口按 Win64 变长约定浮点走整数文件。
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

#include <cstring>
#include <string>
#include <vector>

#include "jsim/engine.hpp"
#include "jsim/jni.hpp"

namespace jsim {
namespace jniuc {

/// UC_* 错误码 → 结构化 Bail。guest 侧错误统一 BadOperand/NotConcrete 口径。
inline Bail ucBail(const char* what, uc_err err)
{
    return Bail{Bail::Reason::BadOperand, 0, std::string(what) + ": " + uc_strerror(err)};
}

class UcNative
{
public:
    explicit UcNative(jni::JniEnv& env, jni::Layout layout = jni::Layout{})
        : env_(env), layout_(layout)
    {
        uc_err err = uc_open(UC_ARCH_X86, UC_MODE_64, &uc_);
        if (err != UC_ERR_OK) {
            failed_ = true;
            openErr_ = ucBail("uc_open", err);
            return;
        }
        materialize();
        installHooks();
    }

    ~UcNative() { if (uc_ != nullptr) uc_close(uc_); }

    UcNative(const UcNative&) = delete;
    UcNative& operator=(const UcNative&) = delete;

    /// GuestMemory 实现:map 走 scratch 区 bump + 回收链,read/write 走
    /// uc_mem_read/write。灌进 env.mem。
    jni::GuestMemory memFace()
    {
        jni::GuestMemory m;
        m.map = [this](usize size, usize align) -> Result<std::pair<u64, usize>, Bail> {
            return mapScratch(size, align == 0 ? 1 : align);
        };
        m.write = [this](u64 addr, const void* src, usize n) -> Result<void, Bail> {
            uc_err err = uc_mem_write(uc_, addr, src, n);
            if (err != UC_ERR_OK) return Result<void, Bail>::err(ucBail("uc_mem_write", err));
            return Result<void, Bail>::ok();
        };
        m.read = [this](u64 addr, void* dst, usize n) -> Result<void, Bail> {
            uc_err err = uc_mem_read(uc_, addr, dst, n);
            if (err != UC_ERR_OK) return Result<void, Bail>::err(ucBail("uc_mem_read", err));
            return Result<void, Bail>::ok();
        };
        m.free = [this](u64 addr) -> Result<void, Bail> {
            auto it = scratchOwned_.find(addr);
            if (it == scratchOwned_.end()) {
                return Result<void, Bail>::err({Bail::Reason::BadOperand, 0, "释放未知物化块"});
            }
            scratchFree_.push_back({addr, it->second});
            scratchOwned_.erase(it);
            return Result<void, Bail>::ok();
        };
        return m;
    }

    /// NativeExecutor 实现。灌进 env.executor.run。
    jni::NativeExecutor executorFace()
    {
        jni::NativeExecutor e;
        e.run = [this](jni::JniEnv& env, const jni::NativeRun& nr) -> Result<u64, Bail> {
            return run(env, nr);
        };
        return e;
    }

    /// 映射并灌入一段代码(可多次;起始地址按 4K 页对齐由消费方保证)。
    Result<void, Bail> mapCode(u64 addr, const void* data, usize size)
    {
        if (failed_) return Result<void, Bail>::err(openErr_);
        if (size == 0) return Result<void, Bail>::ok();
        const usize mapped = (size + 0xFFF) & ~usize(0xFFF);
        uc_err err = uc_mem_map(uc_, addr, static_cast<u32>(mapped), UC_PROT_ALL);
        if (err != UC_ERR_OK) return Result<void, Bail>::err(ucBail("uc_mem_map 代码区", err));
        err = uc_mem_write(uc_, addr, data, size);
        if (err != UC_ERR_OK) return Result<void, Bail>::err(ucBail("uc_mem_write 代码区", err));
        return Result<void, Bail>::ok();
    }

private:
    void materialize()
    {
        mapFixed(layout_.envTable, 0x1000);
        mapFixed(layout_.funcTable, 0x1000);
        mapFixed(layout_.trapPage, 0x1000);
        mapFixed(layout_.scratch, static_cast<u32>(layout_.scratchSize));
        mapFixed(layout_.stackBase, static_cast<u32>(layout_.stackSize));
        mapFixed(layout_.sentinel, 0x1000);
        if (failed_) return;

        // JNI 双重间接:env 指向 env 对象,其首成员是函数表地址(与 HotSpot 的
        // (*env)->Fn 约定一致——真机对拍腿会验证这一点)。
        const u64 tableBase = layout_.funcTable;
        uc_mem_write(uc_, layout_.envTable, &tableBase, 8);
        // 函数表:第 i 项 = trapBase + i * kStubStride。
        std::vector<u64> entries(jni::kFuncCount);
        for (usize i = 0; i < jni::kFuncCount; ++i) {
            entries[i] = layout_.trapPage + i * jni::Layout::kStubStride;
        }
        uc_mem_write(uc_, layout_.funcTable, entries.data(), entries.size() * 8);
        // trap 页整页填 ret(0xC3):钩子必然先截获;万一直落也是干净返回。
        std::vector<u8> trapFill(0x1000, 0xC3);
        uc_mem_write(uc_, layout_.trapPage, trapFill.data(), trapFill.size());
        // sentinel 页填 ud2:正常路径在 until 哨兵处停,执行到它本身说明跑飞。
        std::vector<u8> sentFill(0x1000, 0x0F);
        for (usize i = 1; i < sentFill.size(); i += 2) sentFill[i] = 0x0B;
        uc_mem_write(uc_, layout_.sentinel, sentFill.data(), sentFill.size());
    }

    void mapFixed(u64 addr, u32 size)
    {
        uc_err err = uc_mem_map(uc_, addr, size, UC_PROT_ALL);
        if (err != UC_ERR_OK) {
            failed_ = true;
            openErr_ = ucBail("uc_mem_map 固定区", err);
        }
    }

    static void hookTrampoline(uc_engine* uc, u64 addr, u32 size, void* user)
    {
        static_cast<UcNative*>(user)->onTrap(uc, addr);
    }

    static bool hookBadMem(uc_engine* uc, uc_mem_type type, u64 addr, int size, i64 value,
                           void* user)
    {
        UcNative* self = static_cast<UcNative*>(user);
        const char* what = "guest 坏访存";
        if (addr >= jni::HandlePool::kBase && addr < jni::HandlePool::kBase + 0x100000) {
            what = "句柄被解引用(句柄是假指针)";
        } else if (addr >= self->layout_.trapPage && addr < self->layout_.trapPage + 0x1000) {
            what = "trap 页被当数据访";
        }
        self->lastBail_ = Bail{Bail::Reason::BadOperand, 0,
                               std::string(what) + " @ " + jni::hexOf(addr)};
        uc_emu_stop(uc);
        return false;
    }

    void installHooks()
    {
        uc_hook hook = 0;
        uc_hook_add(uc_, &hook, UC_HOOK_CODE, reinterpret_cast<void*>(hookTrampoline), this,
                    layout_.trapPage, layout_.trapPage + 0xFFF);
        uc_hook_add(uc_, &hook,
                    UC_HOOK_MEM_UNMAPPED | UC_HOOK_MEM_READ_UNMAPPED | UC_HOOK_MEM_WRITE_UNMAPPED |
                        UC_HOOK_MEM_FETCH_UNMAPPED,
                    reinterpret_cast<void*>(hookBadMem), this, 1, 0);
    }

    // ---- ABI:寄存器文件与栈位 ---------------------------------------------------

    static constexpr int kWinIntRegs[4] = {UC_X86_REG_RCX, UC_X86_REG_RDX, UC_X86_REG_R8,
                                           UC_X86_REG_R9};
    static constexpr int kSysVIntRegs[6] = {UC_X86_REG_RDI, UC_X86_REG_RSI, UC_X86_REG_RDX,
                                            UC_X86_REG_RCX, UC_X86_REG_R8, UC_X86_REG_R9};

    bool isWin64() const { return currentAbi_ == jni::NativeSpec::Abi::Win64; }

    Result<u64, Bail> regRead(int reg)
    {
        u64 v = 0;
        uc_err err = uc_reg_read(uc_, reg, &v);
        if (err != UC_ERR_OK) return Result<u64, Bail>::err(ucBail("uc_reg_read", err));
        return Result<u64, Bail>::ok(v);
    }

    Result<void, Bail> regWrite(int reg, u64 v)
    {
        uc_err err = uc_reg_write(uc_, reg, &v);
        if (err != UC_ERR_OK) return Result<void, Bail>::err(ucBail("uc_reg_write", err));
        return Result<void, Bail>::ok();
    }

    /// XMM 低 64 位读写。unicorn 各版本对 XMM 的值类型形状不一(uc_x86_xmm /
    /// fp128 / 裸 16 字节),寄存器值本质是 16 字节 POD,按原始缓冲 memcpy 最稳。
    Result<u64, Bail> xmmRead(usize k)
    {
        u8 buf[16] = {};
        uc_err err = uc_reg_read(uc_, kXmmReg(k), buf);
        if (err != UC_ERR_OK) return Result<u64, Bail>::err(ucBail("uc_reg_read xmm", err));
        u64 low = 0;
        std::memcpy(&low, buf, sizeof(low));
        return Result<u64, Bail>::ok(low);
    }

    Result<void, Bail> xmmWrite(usize k, u64 low)
    {
        u8 buf[16] = {};
        std::memcpy(buf, &low, sizeof(low));
        uc_err err = uc_reg_write(uc_, kXmmReg(k), buf);
        if (err != UC_ERR_OK) return Result<void, Bail>::err(ucBail("uc_reg_write xmm", err));
        return Result<void, Bail>::ok();
    }

    static int kXmmReg(usize k)
    {
        static const int kXmm[8] = {UC_X86_REG_XMM0, UC_X86_REG_XMM1, UC_X86_REG_XMM2,
                                    UC_X86_REG_XMM3, UC_X86_REG_XMM4, UC_X86_REG_XMM5,
                                    UC_X86_REG_XMM6, UC_X86_REG_XMM7};
        return kXmm[k];
    }

    /// 栈上实参:C 位置 p 的溢出槽。截获点在 stub 首指令,[RSP] = 返回地址。
    /// Win64 溢出自 p=4 起且先垫 0x20 影子空间;SysV 自 p=6 起。
    Result<u64, Bail> stackArg(usize p)
    {
        Result<u64, Bail> rsp = regRead(UC_X86_REG_RSP);
        if (!rsp.isOk()) return rsp;
        const u64 off = isWin64() ? 0x28 + (p - 4) * 8 : 8 + (p - 6) * 8;
        u64 v = 0;
        uc_err err = uc_mem_read(uc_, rsp.value() + off, &v, 8);
        if (err != UC_ERR_OK) return Result<u64, Bail>::err(ucBail("uc_mem_read 栈实参", err));
        return Result<u64, Bail>::ok(v);
    }

    usize countClass(usize p, char want) const
    {
        usize k = 0;
        for (usize i = 0; i < p && i < classes_.size(); ++i) {
            if ((classes_[i] != 0) == (want != 0)) ++k;
        }
        return k;
    }

    /// dispatch 的参数面(SysV):按 argClassesOf 的类表分寄存器文件,溢出落栈。
    Result<u64, Bail> readSysV(usize p, bool wantFloat)
    {
        if (wantFloat) {
            const usize k = countClass(p, 1);
            if (k < 8) return xmmRead(k);
        } else {
            const usize k = countClass(p, 0);
            if (k < 6) return regRead(kSysVIntRegs[k]);
        }
        return stackArg(p);
    }

    // ---- trap 分发 ---------------------------------------------------------------

    void onTrap(uc_engine* uc, u64 addr)
    {
        const u64 rel = addr - layout_.trapPage;
        if (rel % jni::Layout::kStubStride != 0) {
            lastBail_ = {Bail::Reason::BadOperand, 0, "trap 地址未按 stub 对齐"};
            uc_emu_stop(uc);
            return;
        }
        const u32 id = static_cast<u32>(rel / jni::Layout::kStubStride);

        auto argI = [this](usize p) -> Result<u64, Bail> {
            if (isWin64()) {
                // 本包分发的所有入口(定长 JNI 函数、变长 Call*Method)在 Win64 下
                // 都按位置走整数文件:定长函数无浮点参数,变长调用的浮点按变长
                // 约定进整数寄存器。
                if (p < 4) return regRead(kWinIntRegs[p]);
                return stackArg(p);
            }
            return readSysV(p, false);
        };
        auto argF = [this](usize p) -> Result<u64, Bail> {
            if (isWin64()) return readPosWin(p);
            return readSysV(p, true);
        };

        if (!isWin64()) {
            // SysV 的类布局需要 jmethodID(bare Call 系),它恒在整数位置 2(RDX)。
            Result<u64, Bail> midRaw = regRead(kSysVIntRegs[2]);
            if (!midRaw.isOk()) {
                lastBail_ = midRaw.error();
                uc_emu_stop(uc);
                return;
            }
            Result<std::vector<char>, Bail> classes = jni::argClassesOf(env_, id, midRaw.value());
            if (!classes.isOk()) {
                lastBail_ = classes.error();
                uc_emu_stop(uc);
                return;
            }
            classes_ = classes.value();
        }

        Result<u64, Bail> out = jni::dispatch(env_, id, argI, argF);
        if (!out.isOk()) {
            lastBail_ = out.error();
            uc_emu_stop(uc);
            return;
        }
        writeReturn(id, out.value());
        // 手工 ret:折回返回地址(stub 的 0xC3 不执行)。
        Result<u64, Bail> rsp = regRead(UC_X86_REG_RSP);
        if (!rsp.isOk()) {
            lastBail_ = rsp.error();
            uc_emu_stop(uc);
            return;
        }
        u64 retAddr = 0;
        uc_err err = uc_mem_read(uc_, rsp.value(), &retAddr, 8);
        if (err != UC_ERR_OK) {
            lastBail_ = ucBail("uc_mem_read 返回地址", err);
            uc_emu_stop(uc);
            return;
        }
        regWrite(UC_X86_REG_RSP, rsp.value() + 8);
        regWrite(UC_X86_REG_RIP, retAddr);
    }

    /// Win64 浮点读:定长函数无浮点;变长调用的浮点在整数文件,与 argI 同通道。
    Result<u64, Bail> readPosWin(usize p)
    {
        if (p < 4) return regRead(kWinIntRegs[p]);
        return stackArg(p);
    }

    void writeReturn(u32 id, u64 bits)
    {
        const jni::FuncMeta* meta = jni::metaOf(id);
        if (meta == nullptr) return;
        using R = jni::FuncMeta::Ret;
        if (meta->ret == R::F32) {
            xmmWrite(0, bits & 0xFFFFFFFFull);
        } else if (meta->ret == R::F64) {
            xmmWrite(0, bits);
        } else if (meta->ret != R::Void) {
            regWrite(UC_X86_REG_RAX, bits);
        }
    }

    // ---- NativeRun → 机器态 --------------------------------------------------------

    Result<u64, Bail> run(jni::JniEnv& env, const jni::NativeRun& nr)
    {
        if (failed_) return Result<u64, Bail>::err(openErr_);
        if (nr.spec == nullptr) {
            return Result<u64, Bail>::err({Bail::Reason::BadOperand, 0, "NativeRun 缺 spec"});
        }
        currentAbi_ = nr.spec->abi;
        env.currentAbi = nr.spec->abi;
        lastBail_ = Bail{};

        // 入口栈:[RSP] = sentinel(两种 ABI 的入口约定都是 rsp ≡ 8 (mod 16))。
        u64 rsp = (layout_.stackBase + layout_.stackSize - 0x100) & ~u64(0xF);
        rsp -= 8;
        u64 sentinel = layout_.sentinel;
        uc_err err = uc_mem_write(uc_, rsp, &sentinel, 8);
        if (err != UC_ERR_OK) return Result<u64, Bail>::err(ucBail("uc_mem_write 哨兵", err));

        // C 参数序列:pos0 = JNIEnv*,pos1 = 接收者/jclass,pos2.. = 实参(实参类由
        // nr.argIsFloat 给,buildNativeRun 按 desc 填)。
        const usize maxInt = isWin64() ? 4 : 6;
        const usize maxFloat = isWin64() ? 4 : 8;
        int intK = 0, fK = 0;
        for (usize p = 0; p <= nr.args.size(); ++p) {
            // v 与类:pos0 取 envTable,pos1 取 nr.args[0](恒整),pos2+ 取实参。
            const u64 v = (p == 0) ? layout_.envTable : nr.args[p - 1];
            const bool isFloat =
                p >= 2 && static_cast<usize>(p - 2) < nr.argIsFloat.size() && nr.argIsFloat[p - 2] != 0;
            if (isFloat && static_cast<usize>(fK) < maxFloat) {
                Result<void, Bail> r = xmmWrite(static_cast<usize>(fK++), v);
                if (!r.isOk()) return Result<u64, Bail>::err(r.error());
                continue;
            }
            if (!isFloat && static_cast<usize>(intK) < maxInt) {
                Result<void, Bail> r = regWrite(isWin64() ? kWinIntRegs[intK] : kSysVIntRegs[intK], v);
                if (!r.isOk()) return Result<u64, Bail>::err(r.error());
                ++intK;
                continue;
            }
            // 溢出:按 C 位置落栈,与 onTrap 的读法同一公式。
            const u64 off = isWin64() ? 0x28 + (p - 4) * 8 : 8 + (p - 6) * 8;
            err = uc_mem_write(uc_, rsp + off, &v, 8);
            if (err != UC_ERR_OK) return Result<u64, Bail>::err(ucBail("uc_mem_write 栈实参", err));
        }
        Result<void, Bail> rs = regWrite(UC_X86_REG_RSP, rsp);
        if (!rs.isOk()) return Result<u64, Bail>::err(rs.error());

        err = uc_emu_start(uc_, nr.spec->entry, layout_.sentinel, 0,
                           static_cast<usize>(env.maxSteps));
        if (!lastBail_.detail.empty()) return Result<u64, Bail>::err(lastBail_);
        if (err != UC_ERR_OK) return Result<u64, Bail>::err(ucBail("uc_emu_start", err));
        // until 哨兵停机不触发 code 钩子:RIP 落在 sentinel 即正常返回;不在
        // sentinel 说明步数耗尽或跑飞,一律熔断。
        Result<u64, Bail> rip = regRead(UC_X86_REG_RIP);
        if (!rip.isOk()) return Result<u64, Bail>::err(rip.error());
        if (rip.value() != layout_.sentinel) {
            return Result<u64, Bail>::err({Bail::Reason::NotConcrete, 0, "native 步数超限熔断"});
        }
        // 返回位型:浮点在 XMM0,其余在 RAX。
        if (nr.hasReturn && nr.retType == ValType::F32) {
            Result<u64, Bail> v = xmmRead(0);
            if (v.isOk()) v = Result<u64, Bail>::ok(v.value() & 0xFFFFFFFFull);
            return v;
        }
        if (nr.hasReturn && nr.retType == ValType::F64) return xmmRead(0);
        return regRead(UC_X86_REG_RAX);
    }

    // ---- scratch 物化区 -------------------------------------------------------------

    Result<std::pair<u64, usize>, Bail> mapScratch(usize size, usize align)
    {
        const usize stride = (size + 15) & ~usize(15);
        for (usize i = 0; i < scratchFree_.size(); ++i) {
            if (scratchFree_[i].second >= stride) {
                const u64 addr = scratchFree_[i].first;
                scratchFree_.erase(scratchFree_.begin() + static_cast<usize>(i));
                scratchOwned_[addr] = stride;
                return Result<std::pair<u64, usize>, Bail>::ok({addr, stride});
            }
        }
        if (scratchBump_ + stride > layout_.scratchSize) {
            return Result<std::pair<u64, usize>, Bail>::err({Bail::Reason::NotConcrete, 0, "物化区耗尽"});
        }
        const u64 addr = layout_.scratch + (scratchBump_ + align - 1) / align * align;
        scratchBump_ = (addr - layout_.scratch) + stride;
        scratchOwned_[addr] = stride;
        return Result<std::pair<u64, usize>, Bail>::ok({addr, stride});
    }

    uc_engine* uc_ = nullptr;
    jni::JniEnv& env_;
    jni::Layout layout_;
    jni::NativeSpec::Abi currentAbi_ = jni::NativeSpec::Abi::Win64;
    std::vector<char> classes_; ///< 当前 trap 的参数类布局(SysV 用)
    Bail lastBail_;
    bool failed_ = false;
    Bail openErr_;
    u64 scratchBump_ = 0;
    std::vector<std::pair<u64, usize>> scratchFree_;
    std::unordered_map<u64, usize> scratchOwned_;
};

} // namespace jniuc
} // namespace jsim
