/// @file demo/jni/main.cpp
/// @brief jsim-jni 端到端样例(docs/ext-jni.md):模拟字节码 invoke 到 native
///        方法 → unicorn 跑手写 x64 Win64 机器码 → 机器码经 JNIEnv 表调
///        GetStringUTFChars / ReleaseStringUTFChars / NewStringUTF → 纯层骨架
///        钩子桥接回 Session → 明文还原。
///
/// native 体(伪 C):
///   jchar* c = GetStringUTFChars(env, input, 0);
///   for (i = 0; c[i]; i++) c[i] ^= 0x5A;      // 解密
///   ReleaseStringUTFChars(env, input, c);
///   return NewStringUTF(env, c);
///
/// 这是绑定层的真机验证:函数表偏移(169/170/167)、ABI 编组、trap 分发、句柄
/// 与物化副本的 guest 内存布局,任何一环错位都会在这里断链。期望值由 main 用
/// 同一异或独立复算。

#include <cstdio>
#include <string>

#include "jsim/engine.hpp"
#include "jsim/jni.hpp"
#include "jsim/jni_uc.hpp"

using namespace jsim;

namespace {

int g_total = 0;
int g_fail = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        ++g_total;                                                      \
        if (!(cond)) {                                                  \
            ++g_fail;                                                   \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                               \
    } while (0)

constexpr u64 kCodeBase = 0x00010000;

/// 手写 x64 Win64 机器码:decrypt(JNIEnv* rcx, jclass rdx, jstring r8)。
/// JNI 双重间接:env 指向 env 对象,函数表在 [env];调用 = mov rax,[env] +
/// mov rax,[rax+idx*8]。rbx=env、rsi=jstring、rdi=cstr 是被调用方保存寄存器。
const unsigned char kDecryptCode[] = {
    0x53,                                    // push rbx
    0x56,                                    // push rsi
    0x48, 0x83, 0xEC, 0x28,                  // sub rsp, 0x28
    0x48, 0x89, 0xCB,                        // mov rbx, rcx          ; env 对象
    0x4C, 0x89, 0xC6,                        // mov rsi, r8           ; jstring
    0x45, 0x31, 0xC9,                        // xor r9d, r9d          ; isCopy = NULL
    0x48, 0x89, 0xF2,                        // mov rdx, rsi
    0x48, 0x89, 0xD9,                        // mov rcx, rbx
    0x48, 0x8B, 0x03,                        // mov rax, [rbx]        ; 函数表(双重间接)
    0x48, 0x8B, 0x80, 0x48, 0x05, 0x00, 0x00,// mov rax, [rax + 169*8]  ; GetStringUTFChars
    0xFF, 0xD0,                              // call rax
    0x48, 0x89, 0xC7,                        // mov rdi, rax          ; cstr
    0x31, 0xC9,                              // xor ecx, ecx
    0x8A, 0x04, 0x0F,                        // loop: mov al, [rdi+rcx]
    0x84, 0xC0,                              // test al, al
    0x74, 0x09,                              // jz done
    0x34, 0x5A,                              // xor al, 0x5A
    0x88, 0x04, 0x0F,                        // mov [rdi+rcx], al
    0xFF, 0xC1,                              // inc ecx
    0xEB, 0xF0,                              // jmp loop
    0x4C, 0x89, 0xF8,                        // done: mov r8, rdi
    0x48, 0x89, 0xF2,                        // mov rdx, rsi
    0x48, 0x89, 0xD9,                        // mov rcx, rbx
    0x48, 0x8B, 0x03,                        // mov rax, [rbx]
    0x48, 0x8B, 0x80, 0x50, 0x05, 0x00, 0x00,// mov rax, [rax + 170*8]  ; ReleaseStringUTFChars
    0xFF, 0xD0,                              // call rax
    0x48, 0x89, 0xFA,                        // mov rdx, rdi
    0x48, 0x89, 0xD9,                        // mov rcx, rbx
    0x48, 0x8B, 0x03,                        // mov rax, [rbx]
    0x48, 0x8B, 0x80, 0x38, 0x05, 0x00, 0x00,// mov rax, [rax + 167*8]  ; NewStringUTF
    0xFF, 0xD0,                              // call rax
    0x48, 0x83, 0xC4, 0x28,                  // add rsp, 0x28
    0x5E,                                    // pop rsi
    0x5B,                                    // pop rbx
    0xC3,                                    // ret
};

} // namespace

int main()
{
    const std::string cipher = "jinx-2026\01\7fZ"; // 密文:含控制字节,不含 NUL
    std::string expected;
    for (char c : cipher) {
        const char x = static_cast<char>(c ^ 0x5A);
        if (x == '\0') break; // JNI C 字符串契约:NUL 之后不可往返(HotSpot 同样如此)
        expected.push_back(x);
    }

    // ---- 装配:Session + JniEnv + unicorn 绑定 ----
    Session session;
    jni::JniEnv env;
    env.session = &session;
    jniuc::UcNative uc(env);
    env.mem = uc.memFace();
    env.executor = uc.executorFace();

    Result<void, Bail> mapped = uc.mapCode(kCodeBase, kDecryptCode, sizeof(kDecryptCode));
    CHECK(mapped.isOk());
    env.methods["sample/Decrypt.decrypt(Ljava/lang/String;)Ljava/lang/String;"] =
        jni::NativeSpec{kCodeBase, true, jni::NativeSpec::Abi::Win64};

    env.trace = [](const std::string& line) { std::printf("  %s\n", line.c_str()); };
    jni::install(session, env);

    // ---- 调用方字节码:invokestatic 到 native 解密器 ----
    MethodBuilder b("Caller", "(Ljava/lang/String;)Ljava/lang/String;", 2);
    b.aload(0);
    b.invoke("sample/Decrypt", "decrypt", "(Ljava/lang/String;)Ljava/lang/String;", ValType::Ref);
    b.areturn();
    Method m = b.build();

    Prefill p;
    p.locals = {SimValue::knownStr(cipher)};
    RunResult r = Sim(m, session).runEntry(std::move(p), 1u << 16);

    CHECK(r.kind == RunResult::Kind::Completed);
    CHECK(r.hasReturnValue && r.returnValue.known &&
          r.returnValue.ref.kind == RefPayload::Kind::Str);
    if (r.hasReturnValue && r.returnValue.ref.kind == RefPayload::Kind::Str) {
        CHECK(r.returnValue.ref.str == expected);
        if (r.returnValue.ref.str == expected) {
            std::printf("还原明文: %s\n", r.returnValue.ref.str.c_str());
        } else {
            std::printf("得到:     %s\n", r.returnValue.ref.str.c_str());
            std::printf("期望:     %s\n", expected.c_str());
        }
    }
    CHECK(env.pool.alive() == 0); // local 句柄全回收

    // ---- 第二次调用:global 句柄持久、物化区回收链生效 ----
    Session session2;
    jni::JniEnv env2;
    env2.session = &session2;
    jniuc::UcNative uc2(env2);
    env2.mem = uc2.memFace();
    env2.executor = uc2.executorFace();
    CHECK(uc2.mapCode(kCodeBase, kDecryptCode, sizeof(kDecryptCode)).isOk());
    env2.methods["sample/Decrypt.decrypt(Ljava/lang/String;)Ljava/lang/String;"] =
        jni::NativeSpec{kCodeBase, true, jni::NativeSpec::Abi::Win64};
    jni::install(session2, env2);

    // 连续三次调用:验证物化区回收链与 local 句柄回收不成泄漏。
    MethodBuilder b2("Caller", "(Ljava/lang/String;)Ljava/lang/String;", 2);
    b2.aload(0);
    b2.invoke("sample/Decrypt", "decrypt", "(Ljava/lang/String;)Ljava/lang/String;", ValType::Ref);
    b2.areturn();
    Method m2 = b2.build();
    for (int i = 0; i < 3; ++i) {
        Prefill p2;
        p2.locals = {SimValue::knownStr(cipher)};
        RunResult r2 = Sim(m2, session2).runEntry(std::move(p2), 1u << 16);
        CHECK(r2.kind == RunResult::Kind::Completed);
        CHECK(r2.hasReturnValue && r2.returnValue.ref.kind == RefPayload::Kind::Str &&
              r2.returnValue.ref.str == expected);
    }
    std::printf("循环 3 次:物化区回收链与 local 句柄回收均未泄漏\n");

    std::printf("jsim-demo-jni: %d/%d 通过\n", g_total - g_fail, g_total);
    return g_fail == 0 ? 0 : 1;
}
