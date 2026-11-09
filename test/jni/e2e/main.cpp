/// @file test/jni/e2e/main.cpp
/// @brief jsim-jni 端到端测试(真 jar + 真 DLL,doc:ext-jni 测试节):
///        libjzip 打开 test/corpus/jni/jni-sample.jar → 取出 native DLL 与
///        Caller.class → PE 装载(按首选基址映射 + 导出表定位入口)→
///        jsim-jbc 桥出调用方字节码 → jsim 执行 → invoke native → unicorn
///        跑真机器码 → JNI 骨架钩子桥接回 Session → 明文还原。
///        期望值 C++ 独立复算;JDK 可用时另跑真 JVM 对拍腿
///        (sample.Main 走 HotSpot 的真 JNIEnv,校验函数表索引与约定)。
///
/// 样本与再生成:test/jni/sample/(build-sample.ps1 或 gradle sampleJar)。
/// PE 解析走字节偏移手工读取,不用结构体——SDK 头的宏(machine/sections 等)
/// 会污染 pack 结构的字段名。

#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "jdk_finder/jdk_finder.hpp"

#include "jbc/ir/units.hpp"

#include "jzip/decompressor.hpp"
#include "jzip/zip_reader.hpp"

#include "jsim/engine.hpp"
#include "jsim/jbc.hpp"
#include "jsim/jni.hpp"
#include "jsim/jni_uc.hpp"

namespace fs = std::filesystem;

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

// ── 文件与 jar 读取 ─────────────────────────────────────────────────────────

std::vector<u8> readFileBytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::vector<u8>((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
}

/// 语料定位:与 jsim-jbc-test 同款——CWD 相对优先,可执行目录逐级上溯。
fs::path locateCorpusJar(const char* argv0) {
    const fs::path relative = "test/corpus/jni/jni-sample.jar";
    std::error_code ec;
    if (fs::exists(relative)) return relative;
    fs::path dir = fs::weakly_canonical(fs::path(argv0), ec).parent_path();
    for (int up = 0; up < 6 && !dir.empty(); ++up) {
        const fs::path candidate = dir / relative;
        if (fs::exists(candidate)) return candidate;
        dir = dir.parent_path();
    }
    return {};
}

/// jar 条目读取:stored(0)与 deflated(8)都收。
std::vector<u8> readJarEntry(const std::vector<u8>& jarBytes, const std::string& entryName,
                             std::string& why) {
    try {
        jzip::ZipArchive archive;
        jzip::JvmZipReader reader;
        reader.read(archive, jzip::MemorySegment{jarBytes});
        const jzip::LocalFileHeader* entry = archive.getLocalFileByName(entryName);
        if (entry == nullptr) {
            why = "jar 内无条目 " + entryName;
            return {};
        }
        if (entry->getCompressionMethod() != 0 && entry->getCompressionMethod() != 8) {
            why = "条目压缩方法非 stored/deflate";
            return {};
        }
        jzip::DeflateDecompressor dec;
        return entry->decompress(dec);
    } catch (const std::exception& e) {
        why = std::string("libjzip 读取异常:") + e.what();
        return {};
    }
}

// ── PE 装载(消费方层;包本身不做二进制加载)───────────────────────────────

inline std::uint16_t rd16(const std::uint8_t* p) {
    std::uint16_t v; std::memcpy(&v, p, 2); return v;
}
inline std::uint32_t rd32(const std::uint8_t* p) {
    std::uint32_t v; std::memcpy(&v, p, 4); return v;
}
inline std::uint64_t rd64(const std::uint8_t* p) {
    std::uint64_t v; std::memcpy(&v, p, 8); return v;
}

/// 小端字段读器:节表逐节扫 RVA → 文件偏移。全部走字节偏移,不声明结构体。
struct PeView {
    const std::vector<u8>& pe;
    usize peOff = 0;
    std::uint16_t numSections = 0;
    usize optOff = 0;
    u32 sizeOfHeaders = 0;

    explicit PeView(const std::vector<u8>& bytes) : pe(bytes) {}

    bool init(std::string& why) {
        if (pe.size() < 0x40 || pe[0] != 'M' || pe[1] != 'Z') {
            why = "缺 MZ 头";
            return false;
        }
        peOff = rd32(pe.data() + 0x3C);
        if (peOff + 24 > pe.size() || std::memcmp(pe.data() + peOff, "PE\0\0", 4) != 0) {
            why = "缺 PE 签名";
            return false;
        }
        if (rd16(pe.data() + peOff + 4) != 0x8664) {
            why = "非 x64 PE";
            return false;
        }
        numSections = rd16(pe.data() + peOff + 6);
        const std::uint16_t optSize = rd16(pe.data() + peOff + 20);
        optOff = peOff + 24;
        if (optOff + optSize > pe.size() || optSize < 112 || rd16(pe.data() + optOff) != 0x20B) {
            why = "非 PE32+ 或可选头越界";
            return false;
        }
        sizeOfHeaders = rd32(pe.data() + optOff + 60);
        return true;
    }

    u64 imageBase() const { return rd64(pe.data() + optOff + 24); }
    u32 sizeOfImage() const { return rd32(pe.data() + optOff + 56); }
    u32 exportDirRva() const { return rd32(pe.data() + optOff + 112); }
    usize sectionAt(std::uint16_t i) const { return optOff + 112 + 16 * 8 + static_cast<usize>(i) * 40; }
    u32 sectVirtualAddress(usize s) const { return rd32(pe.data() + s + 12); }
    u32 sectVirtualSize(usize s) const { return rd32(pe.data() + s + 8); }
    u32 sectSizeOfRaw(usize s) const { return rd32(pe.data() + s + 16); }
    u32 sectPtrRaw(usize s) const { return rd32(pe.data() + s + 20); }
};

/// RVA → 文件偏移的字节面读取。avail 保证 [rva, rva+avail) 都在映射内。
const u8* rvaInFile(const PeView& v, u32 rva, usize avail) {
    if (rva < v.sizeOfHeaders && static_cast<u64>(rva) + avail <= v.sizeOfHeaders) {
        return v.pe.data() + rva;
    }
    for (std::uint16_t i = 0; i < v.numSections; ++i) {
        const usize s = v.sectionAt(i);
        const u32 va = v.sectVirtualAddress(s);
        if (rva >= va && static_cast<u64>(rva) + avail <= va + v.sectSizeOfRaw(s)) {
            return v.pe.data() + v.sectPtrRaw(s) + (rva - va);
        }
    }
    return nullptr;
}

/// 按首选基址把映像铺平并灌进 unicorn;定位导出函数返回 guest 入口。
bool peMapAndFindExport(const std::vector<u8>& peBytes, const char* exportName, jniuc::UcNative& uc,
                        u64& imageBaseOut, u64& entryOut, std::string& why) {
    PeView v(peBytes);
    if (!v.init(why)) return false;

    // 样本约定 /BASE:0x10000:按首选基址映射,delta=0,免重定位处理。
    const u64 imageBase = v.imageBase();
    const u32 sizeOfImage = v.sizeOfImage();
    std::vector<u8> image(sizeOfImage, 0);
    if (v.sizeOfHeaders > sizeOfImage) {
        why = "头区越出映像";
        return false;
    }
    std::memcpy(image.data(), peBytes.data(), v.sizeOfHeaders);
    for (std::uint16_t i = 0; i < v.numSections; ++i) {
        const usize s = v.sectionAt(i);
        const u32 va = v.sectVirtualAddress(s);
        const usize raw = v.sectSizeOfRaw(s) < v.sectVirtualSize(s) ? v.sectSizeOfRaw(s)
                                                                    : v.sectVirtualSize(s);
        if (static_cast<u64>(va) + raw > sizeOfImage ||
            static_cast<u64>(v.sectPtrRaw(s)) + raw > peBytes.size()) {
            why = "节映射越界";
            return false;
        }
        std::memcpy(image.data() + va, peBytes.data() + v.sectPtrRaw(s), raw);
    }
    Result<void, Bail> mapped = uc.mapCode(imageBase, image.data(), image.size());
    if (!mapped.isOk()) {
        why = "mapCode 失败:" + mapped.error().detail;
        return false;
    }

    // 导出表 → 入口。
    const u32 exportRva = v.exportDirRva();
    if (exportRva == 0) {
        why = "无导出表";
        return false;
    }
    const u8* exp = rvaInFile(v, exportRva, 40);
    if (exp == nullptr) {
        why = "导出表 RVA 不可定位";
        return false;
    }
    const u32 numNames = rd32(exp + 20);
    const u32 addrFunctions = rd32(exp + 28);
    const u32 addrNames = rd32(exp + 32);
    const u32 addrOrdinals = rd32(exp + 36);
    for (u32 i = 0; i < numNames; ++i) {
        const u8* nameRvaPtr = rvaInFile(v, addrNames + i * 4, 4);
        if (nameRvaPtr == nullptr) continue;
        const u8* name = rvaInFile(v, rd32(nameRvaPtr), 1);
        if (name == nullptr) continue;
        if (std::strcmp(reinterpret_cast<const char*>(name), exportName) != 0) continue;
        const u8* ordPtr = rvaInFile(v, addrOrdinals + static_cast<usize>(i) * 2, 2);
        if (ordPtr == nullptr) {
            why = "导出序号表不可定位";
            return false;
        }
        const u8* funcPtr = rvaInFile(v, addrFunctions + static_cast<usize>(rd16(ordPtr)) * 4, 4);
        if (funcPtr == nullptr) {
            why = "导出函数表不可定位";
            return false;
        }
        imageBaseOut = imageBase;
        entryOut = imageBase + rd32(funcPtr);
        return true;
    }
    why = std::string("未找到导出 ") + exportName;
    return false;
}

// ── 真 JVM 对拍腿 ───────────────────────────────────────────────────────────

/// 找 JDK,跑 sample.Main(dll 路径 + cipher),stdout 即解密输出。
bool jvmCrossCheck(const fs::path& jar, const fs::path& dllFile, const std::string& cipher,
                   std::string& out, std::string& why) {
    const jdk_finder::JdkLocation jdk = jdk_finder::find_jdk();
    if (!jdk.available) {
        why = "libjdk_finder 未定位到 JDK";
        return false;
    }
    const fs::path javaExe = jdk_finder::jdk_executable(jdk, "java");
    if (javaExe.empty()) {
        why = "JDK 缺 bin/java";
        return false;
    }
    const fs::path outFile = dllFile.parent_path() / "jni-e2e-jvm-out.txt";
    const std::string cmd = "\"" + javaExe.string() + "\" -cp \"" + jar.string() +
                            "\" sample.Main \"" + dllFile.string() + "\" \"" + cipher +
                            "\" > \"" + outFile.string() + "\" 2>&1";
#if defined(_WIN32)
    const int rc = std::system(("\"" + cmd + "\"").c_str());
#else
    const int rc = std::system(cmd.c_str());
#endif
    if (rc != 0) {
        why = "java 退出码 " + std::to_string(rc);
        return false;
    }
    std::string text;
    for (char c : readFileBytes(outFile)) text.push_back(c);
    // 去行尾(Windows \r\n / POSIX \n),样本输出单行。
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    out = std::move(text);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    const std::string cipher = "j2c-sample\01\7fZ";
    std::string expected;
    for (char c : cipher) {
        const char x = static_cast<char>(c ^ 0x5A);
        if (x == '\0') break; // JNI C 字符串契约:NUL 之后不可往返(对拍腿同样如此)
        expected.push_back(x);
    }

    // ---- 1. jar 定位与解包 ----
    // jar 是样本构建产物(build-sample.ps1 / gradle sampleJar 生成),不入库;缺失时整条 E2E 跳过。
    const fs::path jar = locateCorpusJar(argc > 0 ? argv[0] : "");
    if (jar.empty()) {
        std::printf(
            "jsim-jni-e2e-test 跳过:语料 jar 不存在(构建产物,不入库)。"
            "生成:test/jni/sample/build-sample.ps1 或 gradle sampleJar\n");
        return 0;
    }
    const std::vector<u8> jarBytes = readFileBytes(jar);
    CHECK(!jarBytes.empty());

    std::string why;
    std::vector<u8> dllBytes = readJarEntry(jarBytes, "decrypt.dll", why);
    CHECK(!dllBytes.empty());
    std::vector<u8> callerClass = readJarEntry(jarBytes, "sample/Caller.class", why);
    CHECK(!callerClass.empty());
    if (dllBytes.empty() || callerClass.empty()) {
        std::printf("jar 解包失败:%s\n", why.c_str());
        return 1;
    }

    // ---- 2. 模拟侧装配:unicorn 绑定 + JNI 接缝 ----
    Session session;
    jni::JniEnv env;
    env.session = &session;
    jniuc::UcNative uc(env);
    env.mem = uc.memFace();
    env.executor = uc.executorFace();
    env.trace = [](const std::string& line) { std::printf("  %s\n", line.c_str()); };

    // ---- 3. PE 装载与导出入口 ----
    u64 imageBase = 0;
    u64 entry = 0;
    const bool found =
        peMapAndFindExport(dllBytes, "Java_sample_Decrypt_decrypt", uc, imageBase, entry, why);
    CHECK(found);
    if (!found) {
        std::printf("PE 装载失败:%s\n", why.c_str());
        return 1;
    }
    std::printf("DLL 映射 @ 0x%llx,导出 Java_sample_Decrypt_decrypt @ 0x%llx\n",
                static_cast<unsigned long long>(imageBase), static_cast<unsigned long long>(entry));
    env.methods["sample/Decrypt.decrypt(Ljava/lang/String;)Ljava/lang/String;"] =
        jni::NativeSpec{entry, true, jni::NativeSpec::Abi::Win64};
    jni::install(session, env);

    // ---- 4. 调用方字节码:jsim-jbc 桥 Caller.run ----
    auto sessionRes = jbc::ir::makeSessionFromBytes(jbc::core::ByteView{callerClass});
    CHECK(sessionRes.is_ok());
    if (sessionRes.is_err()) {
        std::printf("libjbc 建会话失败\n");
        return 1;
    }
    std::shared_ptr<jbc::ir::ClassSession> jbcSession = sessionRes.unwrap();
    const ca::usize idx =
        jbcSession->unit().findMethod("run", "(Ljava/lang/String;)Ljava/lang/String;");
    CHECK(idx != ca::usize(-1));
    auto bridged = jbcbridge::toSimMethod(*jbcSession, idx);
    CHECK(bridged.isOk());
    if (!bridged.isOk()) {
        std::printf("jsim-jbc 桥失败:%s\n", bridged.error().detail.c_str());
        return 1;
    }

    Prefill prefill;
    prefill.locals = {SimValue::knownStr(cipher)};
    RunResult r = Sim(bridged.value(), session).runEntry(std::move(prefill), 1u << 16);

    // ---- 5. 断言:明文还原 ----
    CHECK(r.kind == RunResult::Kind::Completed);
    CHECK(r.hasReturnValue && r.returnValue.known &&
          r.returnValue.ref.kind == RefPayload::Kind::Str);
    if (r.hasReturnValue && r.returnValue.ref.kind == RefPayload::Kind::Str) {
        CHECK(r.returnValue.ref.str == expected);
        if (r.returnValue.ref.str == expected) {
            std::printf("模拟还原明文: %s\n", r.returnValue.ref.str.c_str());
        } else {
            std::printf("模拟得到: %s\n期望:     %s\n", r.returnValue.ref.str.c_str(),
                        expected.c_str());
        }
    }
    CHECK(env.pool.alive() == 0);

    // ---- 6. 真 JVM 对拍腿(HotSpot 加载同一份 DLL)----
    std::error_code ec;
    const fs::path tmpDir = fs::temp_directory_path(ec);
    const fs::path dllFile = tmpDir / "jsim-jni-e2e-decrypt.dll";
    std::ofstream dllOut(dllFile, std::ios::binary);
    dllOut.write(reinterpret_cast<const char*>(dllBytes.data()),
                 static_cast<std::streamsize>(dllBytes.size()));
    dllOut.close();
    CHECK(static_cast<bool>(dllOut));
    std::string jvmOut;
    if (jvmCrossCheck(jar, dllFile, cipher, jvmOut, why)) {
        CHECK(jvmOut == expected);
        if (jvmOut == expected) {
            std::printf("真机对拍(HotSpot): %s —— 一致\n", jvmOut.c_str());
        } else {
            std::printf("真机对拍不一致: JVM=%s 期望=%s\n", jvmOut.c_str(), expected.c_str());
        }
    } else {
        std::printf("真机对拍腿跳过:%s\n", why.c_str());
    }

    std::printf("jsim-jni-e2e-test: %d/%d 通过\n", g_total - g_fail, g_total);
    return g_fail == 0 ? 0 : 1;
}
