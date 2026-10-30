/// @file main.cpp
/// @brief 静态 deobf demo：运行时 javac 编译合成样本 → libjbc 解析 → 桥接
///        jsim::Method 模拟解密还原全部调用点密文 → libjbc 模型层改写导出
///        去混淆 jar → 双重复查验（libjzip 重开对拍 + 真 JVM 对拍）。
///
/// 管线：
///   JsimObfSample.java --javac --release 8--> JsimObfSample.class（临时目录，不入库）
///     --libjbc makeSessionFromBytes--> ClassSession（backing DOM + 指令级 IR）
///     --jsim-jbc 桥--> jsim::Method（ext/jsim/jbc.hpp：CodeUnit → Method 形状搬运）
///     --Sim--> 每站点灌密文跑解密器 --> 明文 --> 断言
///     --libjbc 改写--> setLdcString(ldc, 明文) + erase(invokestatic) + lowerMethod
///       （CP 追加与偏移/StackMapTable 重算由 libjbc 负责）+ removeMethod(解密器)
///       + writeClass --> class bytes --> JDK jar 工具打包 jsimobf-deobfed.jar
///     --查验 A--> libjzip 读回 jar 条目，libjbc 重开：解密器摘除/调用点清零/
///       明文 ldc 在位/可完整解析
///     --查验 B--> java -cp 原始编译产物 vs deobfed jar：stdout 逐字节一致
///       且等于期望明文（改写后的 jar 真能跑、输出不变）。
///
/// classfile 的解析与改写全部经 libjbc（luiox-repo 包）：demo 不自备字节码
/// 解析器/序列化器。CodeUnit → jsim::Method 走 jsim-jbc 官方桥扩展包
/// （ext/jsim/jbc.hpp）：指令摊 pc 索引、标签折 pc、ldc 族 CP 索引重映射成
/// 紧凑表下标；字符串按家族口径 MUTF-8 字节原样透传给 jsim 载荷（BMP 非
/// 零码元下 MUTF-8 与 UTF-8 逐字节相同，jsim-str 按 UTF-8 码元读）。
/// 核心/扩展不认的语义按接管面注册：jsim-str 出 String.length/charAt 与
/// new java/lang/String 的分配脚手架 + String.<init>([C)V 的「构造两阶段」
/// （str::installStringAlloc）。demo 自留的只有 profile 面：调用点形态扫描、
/// 解密器编排与判稳，不改库核心语义。

/// std::getenv / std::fopen 在 MSVC CRT 标记为 deprecated；demo 有意用可移植面。
#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "jdk_finder/jdk_finder.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

#include "jbc/core/class_file.hpp"
#include "jbc/core/constant_pool.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/error.hpp"
#include "jbc/ir/units.hpp"

#include "jzip/decompressor.hpp"
#include "jzip/zip_reader.hpp"

#include "jsim/engine.hpp"
#include "jsim/jbc.hpp"
#include "jsim/str.hpp"

using namespace jsim;

namespace fs = std::filesystem;

namespace {

// ─────────────────────────── 断言与报告 ───────────────────────────

int gChecks = 0;
int gFailures = 0;

void expect(bool ok, const std::string& label) {
    ++gChecks;
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", label.c_str());
    if (!ok) ++gFailures;
}

const char* reasonName(Bail::Reason r) {
    switch (r) {
    case Bail::Reason::UnsupportedOpcode: return "UnsupportedOpcode";
    case Bail::Reason::UnsupportedCall: return "UnsupportedCall";
    case Bail::Reason::UnsupportedAlloc: return "UnsupportedAlloc";
    case Bail::Reason::NotConcrete: return "NotConcrete";
    case Bail::Reason::BadOperand: return "BadOperand";
    case Bail::Reason::StackBroken: return "StackBroken";
    case Bail::Reason::OffMethodEnd: return "OffMethodEnd";
    }
    return "?";
}

void printResult(const RunResult& r) {
    switch (r.kind) {
    case RunResult::Kind::Completed:
        std::printf("Completed（%llu 步）", static_cast<unsigned long long>(r.steps));
        break;
    case RunResult::Kind::Bailed:
        std::printf("Bailed[%s] @pc=%u：%s", reasonName(r.bail.reason), r.bail.pc,
                    r.bail.detail.c_str());
        break;
    case RunResult::Kind::LimitHit:
        std::printf("LimitHit（%llu 步）", static_cast<unsigned long long>(r.steps));
        break;
    }
}

/// 密文（jsim UTF-8 码元串）打印成十六进制转义，报告用。
std::string escapeCipher(const std::string& simStr) {
    std::string out = "\"";
    const usize units = str::codeUnitCount(simStr);
    for (usize i = 0; i < units; ++i) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "\\u%04x", str::codeUnitAt(simStr, i));
        out += buf;
    }
    return out + "\"";
}

// ─────────────────────── 文件与进程小件 ───────────────────────

std::vector<u8> readFileBytes(const fs::path& p, bool& ok) {
    ok = false;
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::vector<u8> bytes((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
    // istreambuf_iterator 直接耗尽 streambuf，不置 eofbit——以 bad 判失败。
    ok = !in.bad();
    return bytes;
}

bool writeFileBytes(const fs::path& p, const std::vector<u8>& bytes) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

// ──────────────── JDK 工具定位（libjdk_finder）与现场编译 ────────────────

struct JdkTools {
    fs::path javaHome;
    fs::path javac;
    fs::path javaExe;
    fs::path jar;
};

/// JDK 定位：libjdk_finder 0.1.1 的 find_jdk（JAVA_HOME → MJT_JDK_HOME →
/// 默认安装位置扫描），再解析 bin 下 javac/java/jar 三个工具的绝对路径。
/// 找不到时打印指引并置空成员。
JdkTools locateJdkTools() {
    JdkTools tools;
    const jdk_finder::JdkLocation jdk = jdk_finder::find_jdk();
    if (!jdk.available) {
        std::printf("libjdk_finder 未定位到 JDK：%s\n", jdk.reason.c_str());
        std::printf("请安装 JDK（12+ 支持 --release 8；建议 JDK 17），或设置 JAVA_HOME /\n");
        std::printf("MJT_JDK_HOME 指向含 bin/javac 的 JDK 主目录。\n");
        return tools;
    }
    tools.javaHome = jdk.java_home;
    tools.javac = jdk_finder::jdk_executable(jdk, "javac");
    tools.javaExe = jdk_finder::jdk_executable(jdk, "java");
    tools.jar = jdk_finder::jdk_executable(jdk, "jar");
    if (tools.javac.empty()) {
        std::printf("JDK 已定位（%s）但 bin 下无 javac，请确认是完整 JDK 而非 JRE。\n",
                    tools.javaHome.string().c_str());
        return JdkTools{};
    }
    if (tools.javaExe.empty() || tools.jar.empty()) {
        std::printf("JDK 已定位但 bin 下缺 java/jar 工具，无法完成对拍查验。\n");
        return JdkTools{};
    }
    std::printf("JDK 定位（libjdk_finder 0.1.1）：%s\n", tools.javaHome.string().c_str());
    return tools;
}

/// 现场编译：javac --release 8（class v52，与合成样本声明的目标代次一致）。
/// Windows 下 std::system 走 cmd.exe，路径全部加引号；退出码非零视为失败。
bool compileSample(const fs::path& javac, const fs::path& src, const fs::path& outDir) {
    std::error_code ec;
    fs::remove_all(outDir, ec);
    fs::create_directories(outDir, ec);
    if (ec) {
        std::printf("临时目录 %s 创建失败：%s\n", outDir.string().c_str(), ec.message().c_str());
        return false;
    }
    const std::string cmd = "\"" + javac.string() + "\" --release 8 -encoding UTF-8 -d \"" +
                            outDir.string() + "\" \"" + src.string() + "\"";
    std::printf("编译：%s\n", cmd.c_str());
    // cmd.exe 的老坑：整条命令以引号开头时会被剥错引号，Windows 下外面再包一层。
#if defined(_WIN32)
    const int rc = std::system(("\"" + cmd + "\"").c_str());
#else
    const int rc = std::system(cmd.c_str());
#endif
    if (rc != 0) {
        std::printf("javac 退出码 %d。请确认 JDK 安装完整（bin/javac 可用）。\n", rc);
        return false;
    }
    return true;
}

/// 定位合成样本源码：CWD 相对优先（xmake run 在仓根），再按可执行文件位置
/// 回溯（build/<plat>/<arch>/<mode> 上溯三级）。
fs::path locateSampleSource(const char* argv0) {
    const fs::path relative = "demo/deobf/java/JsimObfSample.java";
    std::error_code ec;
    if (fs::exists(relative)) return relative;
    const fs::path exeDir = fs::weakly_canonical(fs::path(argv0), ec).parent_path();
    for (const fs::path& up : {exeDir / ".." / ".." / "..", exeDir / ".." / ".."}) {
        const fs::path candidate = up / relative;
        if (fs::exists(candidate)) return candidate;
    }
    return {};
}

// ─────────── 桥接（jsim-jbc 扩展包，ext/jsim/jbc.hpp） ───────────
//
// CodeUnit → jsim::Method 的形状搬运升格为官方扩展包：toSimMethod（指令摊
// pc 索引、标签折 pc、ldc 族 CP 索引重映射成 jsim 紧凑表下标）、isLdcFamily、
// setLdcString（setString 语义改写，LDC→LDC_W 自动升格由 libjbc 负责）。
// demo 只保留自己的 profile 面：调用点形态扫描、解密器编排与判稳。

// ───────────────────────── 解密器模拟还原 ─────────────────────────

/// 每站点一个干净 Session：jsim-str 出 String.length/charAt 与字符串构造
/// 两阶段（new 出脚手架对象，<init>([C)V 把 char[] 码元逐个 appendCodeUnit
/// 写进对象 data）；areturn 返回该对象，报告从堆里读 data。
Session makeSession() {
    Session session;
    str::install(session);
    str::installStringAlloc(session);
    return session;
}

// ───────────────────── 调用点扫描与解密器引用统计（libjbc 会话面） ─────────────────────

/// 一个调用点：相邻指令对 ldc(String 密文) → invokestatic 解密器。
struct CallSite {
    std::string method;             ///< 调用点所在方法名
    ca::usize methodIdx = 0;        ///< ClassUnit 下标（改写与 lower 用）
    jbc::ir::CodeUnitRef unit;      /// promote 保活（改写直接在这个 IR 上做）
    jbc::ir::InsnId ldc{};          ///< 密文 ldc 句柄
    jbc::ir::InsnId invoke{};       ///< 紧邻 invokestatic 句柄
    std::string cipher;             ///< 密文（MUTF-8 原样透传给 jsim 载荷）
    u32 pairBytes = 0;              ///< 原指令对字节数（ldc 2/3 + invokestatic 3）
};

/// 形态匹配：ldc(String) 紧邻 invokestatic 解密器（owner=本类 + name + desc 全等）。
std::vector<CallSite> scanCallSites(jbc::ir::ClassSession& s, const std::string& decryptName,
                                    const std::string& decryptDesc) {
    std::vector<CallSite> sites;
    const std::string thisName{s.backing().thisName()};
    const jbc::ir::ClassUnit& unit = s.unit();
    for (ca::usize m = 0; m < unit.methods.size(); ++m) {
        if (unit.symbols().str(unit.methods[m].name) == decryptName
            && unit.symbols().str(unit.methods[m].desc) == decryptDesc) {
            continue; // 解密器自身不含调用点
        }
        if (!jbc::ir::hasCode(s, m)) continue;
        auto promoted = jbc::ir::promoteMethod(s, m);
        if (promoted.is_err()) continue;
        jbc::ir::CodeUnitRef cu = promoted.unwrap();
        const std::vector<jbc::ir::InsnId>& order = cu->dom.order();
        for (ca::usize i = 0; i + 1 < order.size(); ++i) {
            const jbc::ir::Insn* ldcInsn = cu->dom.peek(order[i]);
            const jbc::ir::Insn* invInsn = cu->dom.peek(order[i + 1]);
            if (ldcInsn == nullptr || invInsn == nullptr) continue;
            if (!jbcbridge::isLdcFamily(ldcInsn->op) || invInsn->op != jbc::core::Op::INVOKESTATIC) {
                continue;
            }
            jbc::ir::ClassSession::ConstSem cst;
            if (!s.constOf(*ldcInsn, cst) || cst.kind != jbc::ir::ClassSession::ConstSem::Kind::String) {
                continue;
            }
            jbc::ir::ClassSession::MemberSem mem;
            if (!s.memberOf(*invInsn, mem) || mem.owner != thisName || mem.name != decryptName
                || mem.desc != decryptDesc) {
                continue;
            }
            CallSite site;
            site.method = std::string(unit.symbols().str(unit.methods[m].name));
            site.methodIdx = m;
            site.unit = cu;
            site.ldc = order[i];
            site.invoke = order[i + 1];
            site.cipher = std::string(cst.str);
            site.pairBytes = (ldcInsn->op == jbc::core::Op::LDC ? 2u : 3u) + 3u;
            sites.push_back(std::move(site));
        }
    }
    return sites;
}

/// 全类统计仍指向解密器的调用指令数（改写后应为 0——含未被形态匹配的引用）。
ca::usize countDecryptReferences(jbc::ir::ClassSession& s, const std::string& decryptName,
                                 const std::string& decryptDesc) {
    ca::usize count = 0;
    const std::string thisName{s.backing().thisName()};
    const jbc::ir::ClassUnit& unit = s.unit();
    for (ca::usize m = 0; m < unit.methods.size(); ++m) {
        if (!jbc::ir::hasCode(s, m)) continue;
        auto promoted = jbc::ir::promoteMethod(s, m);
        if (promoted.is_err()) continue;
        jbc::ir::CodeUnitRef cu = promoted.unwrap();
        for (const jbc::ir::InsnId id : cu->dom.order()) {
            const jbc::ir::Insn* in = cu->dom.peek(id);
            if (in == nullptr) continue;
            if (in->op != jbc::core::Op::INVOKESTATIC && in->op != jbc::core::Op::INVOKEVIRTUAL
                && in->op != jbc::core::Op::INVOKESPECIAL
                && in->op != jbc::core::Op::INVOKEINTERFACE) {
                continue;
            }
            jbc::ir::ClassSession::MemberSem mem;
            if (s.memberOf(*in, mem) && mem.owner == thisName && mem.name == decryptName
                && mem.desc == decryptDesc) {
                ++count;
            }
        }
    }
    return count;
}

// ───────────────────── 导出：jar 打包与读回 ─────────────────────

/// 打包：定位 JDK 的 jar 工具（libjzip 写面是逐头手填型——CRC/时间戳自填，
/// 不顺手；按许可回落家族工具链）。
bool packJar(const fs::path& jarTool, const fs::path& classesDir, const std::string& entryName,
             const fs::path& outJar) {
    std::error_code ec;
    fs::remove(outJar, ec);
    const std::string cmd = "\"" + jarTool.string() + "\" --create --file \"" + outJar.string()
                            + "\" -C \"" + classesDir.string() + "\" " + entryName;
#if defined(_WIN32)
    const int rc = std::system(("\"" + cmd + "\"").c_str());
#else
    const int rc = std::system(cmd.c_str());
#endif
    if (rc != 0) {
        std::printf("jar 打包退出码 %d\n", rc);
        return false;
    }
    return fs::exists(outJar);
}

/// 读回：libjzip 的 JvmZipReader（JVM 语义读面）取指定条目字节。
std::vector<u8> readJarEntry(const fs::path& jar, const std::string& entryName, std::string& why) {
    bool ok = false;
    const std::vector<u8> jarBytes = readFileBytes(jar, ok);
    if (!ok) {
        why = "jar 文件读取失败";
        return {};
    }
    try {
        jzip::ZipArchive archive;
        jzip::JvmZipReader reader;
        reader.read(archive, jzip::MemorySegment{jarBytes});
        const jzip::LocalFileHeader* entry = archive.getLocalFileByName(entryName);
        if (entry == nullptr) {
            why = "jar 内无条目 " + entryName;
            return {};
        }
        if (entry->getCompressionMethod() != 8) {
            why = "条目非 deflated（jar 默认压缩，此形态不应出现）";
            return {};
        }
        jzip::DeflateDecompressor dec;
        return entry->decompress(dec);
    } catch (const std::exception& e) {
        why = std::string("libjzip 读取异常：") + e.what();
        return {};
    }
}

// ───────────────────────── 真 JVM 对拍 ─────────────────────────

struct Expectation {
    const char* method;
    const char* plain; ///< 明文（UTF-8），与 JsimObfSample.PLAINS 一一对应
};

const Expectation kExpectations[] = {
    {"banner", "jsim static deobf demo"},   // 偶数长度（22 码元，ASCII）
    {"dusk", "the keys swap at dusk"},      // 奇数长度（21 码元，ASCII）
    {"dance", "双钥匙逆序起舞"},             // 非 ASCII 码元（7 码元，奇数长度）
    {"solo", "灯"},                          // 单码元边界
};

/// 跑 main 类并捕获 stdout（stderr 分流，不污染对拍字节）。
bool runJavaCapture(const fs::path& javaExe, const std::string& cpArg, const fs::path& outFile,
                    const fs::path& errFile) {
    const std::string cmd = "\"" + javaExe.string() + "\" -Dfile.encoding=UTF-8 -cp \"" + cpArg
                            + "\" JsimObfSample > \"" + outFile.string() + "\" 2> \""
                            + errFile.string() + "\"";
#if defined(_WIN32)
    const int rc = std::system(("\"" + cmd + "\"").c_str());
#else
    const int rc = std::system(cmd.c_str());
#endif
    return rc == 0;
}

/// 期望输出：kExpectations 逐行（println 用平台行分隔符）。
std::string expectedJvmOutput() {
#if defined(_WIN32)
    static const char* nl = "\r\n";
#else
    static const char* nl = "\n";
#endif
    std::string out;
    for (const Expectation& e : kExpectations) {
        out += e.plain;
        out += nl;
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8); // 源码字面量是 UTF-8，报告含非 ASCII 明文
#endif
    std::printf("[jsim-demo-deobf] 静态 deobf：libjbc 解析改写 + jsim 模拟还原 + 导出与双查验\n");

    // ── 1. 定位源码与 JDK 工具，现场编译 ──
    fs::path src = locateSampleSource(argc > 0 ? argv[0] : "");
    if (src.empty()) {
        std::printf("找不到 demo/deobf/java/JsimObfSample.java（CWD 与可执行文件目录均未命中）\n");
        return 1;
    }
    std::error_code canonicalEc;
    src = fs::weakly_canonical(src, canonicalEc);
    std::printf("样本源码：%s\n", src.string().c_str());

    const JdkTools tools = locateJdkTools();
    if (tools.javac.empty()) return 1;

    const fs::path outDir = fs::temp_directory_path() / "jsim-demo-deobf";
    const fs::path deobfDir = outDir / "deobfed";
    const fs::path origClass = outDir / "JsimObfSample.class";
    const fs::path deobfClass = deobfDir / "JsimObfSample.class";
    const fs::path deobfJar = deobfDir / "jsimobf-deobfed.jar";
    if (!compileSample(tools.javac, src, outDir)) return 1;

    bool readOk = false;
    const std::vector<u8> origBytes = readFileBytes(origClass, readOk);
    if (!readOk) {
        std::printf("编译产物 JsimObfSample.class 读取失败\n");
        return 1;
    }

    // ── 2. libjbc 建会话，定位解密器 ──
    auto sessionRes = jbc::ir::makeSessionFromBytes(jbc::core::ByteView{origBytes});
    if (sessionRes.is_err()) {
        std::printf("libjbc 建会话失败（stage=%d）\n",
                    static_cast<int>(sessionRes.unwrap_err().stage));
        return 1;
    }
    std::shared_ptr<jbc::ir::ClassSession> session = sessionRes.unwrap();
    const std::string thisName{session->backing().thisName()};
    std::printf("解析（libjbc）：%s（class v%u，%zu 个方法）\n", thisName.c_str(),
                session->backing().major, session->unit().methods.size());
    expect(session->backing().major == 52, "编译代次为 Java 8（class v52）");

    const std::string kDecryptName = "decrypt";
    const std::string kDecryptDesc = "(Ljava/lang/Object;)Ljava/lang/String;";
    const ca::usize decryptIdx = session->unit().findMethod(kDecryptName, kDecryptDesc);
    if (decryptIdx == jbc::ir::kNoMethod) {
        std::printf("未找到解密器 %s%s\n", kDecryptName.c_str(), kDecryptDesc.c_str());
        return 1;
    }
    const jbc::ir::MethodId decryptId = session->unit().methods[decryptIdx].id;
    std::printf("解密器：%s%s%s\n", thisName.c_str(), kDecryptName.c_str(), kDecryptDesc.c_str());

    // ── 3. 桥接（jsim-jbc）：解密器 CodeUnit → jsim::Method，指令形态核对 ──
    auto bridged = jbcbridge::toSimMethod(*session, decryptIdx);
    if (!bridged.isOk()) {
        std::printf("桥接失败：%s\n", bridged.error().detail.c_str());
        return 1;
    }
    Method simMethod = std::move(bridged.value());
    std::printf("桥接（CodeUnit → jsim::Method）：%zu 条指令，maxLocals=%u\n",
                simMethod.code.size(), simMethod.maxLocals);

    bool hasNewArrayChar = false;
    bool hasCastore = false;
    bool hasStringCtor = false;
    bool hasBackJump = false;
    for (u32 i = 0; i < simMethod.code.size(); ++i) {
        const Insn& insn = simMethod.code[i];
        if (insn.opcode == op::NewArray && insn.b == 5) hasNewArrayChar = true; // newarray char
        if (insn.opcode == op::CAStore) hasCastore = true;
        if (insn.opcode == op::InvokeSpecial) {
            const CallRef& call = simMethod.calls.at(static_cast<usize>(insn.a));
            if (call.owner == "java/lang/String" && call.name == "<init>" && call.desc == "([C)V") {
                hasStringCtor = true;
            }
        }
        const bool isJump = insn.opcode == op::Goto
                            || (insn.opcode >= op::IfEq && insn.opcode <= op::IfAcmpne);
        if (isJump && static_cast<u32>(insn.a) < i) hasBackJump = true;
    }
    expect(hasNewArrayChar, "形态：newarray char（char[] 写回）");
    expect(hasCastore, "形态：castore（逐码元落格）");
    expect(hasStringCtor, "形态：new String(char[]) 构造收尾");
    expect(hasBackJump, "形态：回边（逆序循环）");

    // ── 4. 扫描调用点，逐站点模拟还原 ──
    std::vector<CallSite> sites = scanCallSites(*session, kDecryptName, kDecryptDesc);
    std::printf("调用点（ldc 密文 → invokestatic 解密器）：%zu 处\n", sites.size());
    expect(sites.size() == sizeof(kExpectations) / sizeof(kExpectations[0]),
           "调用点数与期望表一致（"
               + std::to_string(sizeof(kExpectations) / sizeof(kExpectations[0])) + " 处）");

    std::vector<std::string> recoveredPlain; // 与 sites 一一对应，改写复用
    for (const CallSite& site : sites) {
        const Expectation* want = nullptr;
        for (const Expectation& e : kExpectations) {
            if (site.method == e.method) want = &e;
        }
        std::printf("  站点 %s：%s\n", site.method.c_str(), escapeCipher(site.cipher).c_str());

        Session jsimSession = makeSession();
        Prefill prefill;
        // 密文来自 ldc 字面量：带常量池 intern 身份事实；字节为 MUTF-8 原样透传。
        prefill.locals = {SimValue::internedStr(site.cipher)};
        RunResult r = Sim(simMethod, jsimSession).runEntry(std::move(prefill), 1u << 16);

        std::string recovered;
        bool completed = r.kind == RunResult::Kind::Completed && r.hasReturnValue
                         && r.returnValue.known && r.returnValue.ref.kind == RefPayload::Kind::Obj;
        if (completed) {
            const auto it = jsimSession.heap.find(r.returnValue.ref.objId);
            if (it != jsimSession.heap.end()) recovered = it->second.data;
            else completed = false;
        }
        std::printf("    还原：\"%s\"（", recovered.c_str());
        printResult(r);
        std::printf("）\n");
        recoveredPlain.push_back(recovered);
        if (!completed) {
            expect(false, site.method + "：解密器未正常返回字符串");
            continue;
        }
        if (want == nullptr) {
            expect(false, site.method + "：无对应期望条目");
            continue;
        }
        std::printf("    期望：\"%s\"\n", want->plain);
        expect(recovered == want->plain,
               std::string(site.method) + "：明文 == 期望（" + want->plain + "）");
    }

    // ── 5. libjbc 模型层改写：密文对 → ldc 明文，摘除解密器，导出 class ──
    std::printf("改写（libjbc 模型层）：\n");
    bool rewriteOk = true;
    std::vector<ca::usize> touchedMethods;
    i64 pairBytesTotal = 0;
    i64 replBytesTotal = 0;
    for (usize k = 0; k < sites.size(); ++k) {
        CallSite& site = sites[k];
        const std::string& plain = recoveredPlain[k];
        // setLdcString（桥包）：CP 追加明文 String（intern 去重），索引超 u1 域
        // 自动升格 ldc→ldc_w（宽度陷阱由 libjbc 挡掉）。
        if (!jbcbridge::setLdcString(*session, site.unit->dom, site.ldc, plain)) {
            std::printf("  setLdcString 失败（站点 %s）\n", site.method.c_str());
            rewriteOk = false;
            break;
        }
        // erase invokestatic：仍被引用会拒绝（StillUsed），此处调用点不可能被引用。
        auto erased = site.unit->dom.erase(site.invoke);
        if (erased.is_err()) {
            std::printf("  erase invokestatic 失败：%s\n", jbc::ir::to_string(erased.unwrap_err()));
            rewriteOk = false;
            break;
        }
        const jbc::ir::Insn* replaced = site.unit->dom.peek(site.ldc);
        const u32 replBytes = replaced != nullptr && replaced->op == jbc::core::Op::LDC ? 2u : 3u;
        pairBytesTotal += static_cast<i64>(site.pairBytes);
        replBytesTotal += static_cast<i64>(replBytes);
        std::printf("  %s：%lluB（ldc %uB + invokestatic 3B）→ ldc 明文 %uB，净 %+lldB\n",
                    site.method.c_str(), static_cast<unsigned long long>(site.pairBytes),
                    site.pairBytes - 3u, replBytes,
                    static_cast<long long>(replBytes) - static_cast<long long>(site.pairBytes));
        bool known = false;
        for (ca::usize m : touchedMethods) known = known || m == site.methodIdx;
        if (!known) touchedMethods.push_back(site.methodIdx);
    }
    expect(rewriteOk, "模型层改写全部成功（" + std::to_string(sites.size()) + " 对）");
    if (!rewriteOk) {
        std::printf("断言 %d/%d 通过\n", gChecks - gFailures, gChecks);
        return 1;
    }

    // lower：偏移与 StackMapTable/LineNumberTable 重定位由 libjbc 序列化机制负责。
    for (ca::usize m : touchedMethods) {
        auto lowered = jbc::ir::lowerMethod(*session, m);
        if (lowered.is_err()) {
            std::printf("lowerMethod 失败：%s\n", jbc::ir::to_string(lowered.unwrap_err()));
            return 1;
        }
    }
    // 摘除解密器（其 CP 引用条目留着无害；调用点已清零）。
    auto removed = session->removeMethod(decryptId);
    if (removed.is_err()) {
        std::printf("removeMethod 失败：%s\n", jbc::ir::to_string(removed.unwrap_err()));
        return 1;
    }

    auto written = jbc::core::writeClass(session->backing());
    if (written.is_err()) {
        std::printf("writeClass 失败：%s\n", jbc::core::to_string(written.unwrap_err()));
        return 1;
    }
    const std::vector<ca::u8> deobfBytes = written.unwrap();
    if (!writeFileBytes(deobfClass, deobfBytes)) {
        std::printf("导出 class 写盘失败：%s\n", deobfClass.string().c_str());
        return 1;
    }
    std::printf("导出 class：%s（%zuB；原 %zuB，指令对 %lldB→%lldB）\n",
                deobfClass.string().c_str(), deobfBytes.size(), origBytes.size(), pairBytesTotal,
                replBytesTotal);

    // ── 6. jar 打包（JDK jar 工具）──
    if (!packJar(tools.jar, deobfDir, "JsimObfSample.class", deobfJar)) {
        std::printf("断言 %d/%d 通过\n", gChecks - gFailures, gChecks);
        return 1;
    }
    std::printf("导出 jar：%s\n", deobfJar.string().c_str());

    // ── 7. 查验 A：libjzip 读回 jar，libjbc 重开结构对拍 ──
    std::string readWhy;
    const std::vector<u8> jarEntry = readJarEntry(deobfJar, "JsimObfSample.class", readWhy);
    expect(!jarEntry.empty(), "结构：jar 条目可读（libjzip）——" + readWhy);
    if (jarEntry.empty()) {
        std::printf("断言 %d/%d 通过\n", gChecks - gFailures, gChecks);
        return 1;
    }
    expect(jarEntry == deobfBytes, "结构：jar 条目与导出 class 逐字节一致");

    auto reopenRes = jbc::ir::makeSessionFromBytes(jbc::core::ByteView{jarEntry});
    if (reopenRes.is_err()) {
        std::printf("重开导出 class 失败\n");
        return 1;
    }
    std::shared_ptr<jbc::ir::ClassSession> reopened = reopenRes.unwrap();
    expect(reopened->backing().state() == jbc::core::ParseState::Validated,
           "结构：导出 class 可完整解析（无降级）");
    expect(reopened->unit().findMethod(kDecryptName, kDecryptDesc) == jbc::ir::kNoMethod,
           "结构：解密器方法已摘除");
    expect(scanCallSites(*reopened, kDecryptName, kDecryptDesc).empty(),
           "结构：密文调用点清零（ldc→invokestatic 形态为 0）");
    expect(countDecryptReferences(*reopened, kDecryptName, kDecryptDesc) == 0,
           "结构：解密器引用清零（全部 invoke 形态为 0）");

    for (const Expectation& e : kExpectations) {
        const ca::usize m = reopened->unit().findMethod(e.method);
        bool plainInPlace = false;
        if (m != jbc::ir::kNoMethod && jbc::ir::hasCode(*reopened, m)) {
            auto promoted = jbc::ir::promoteMethod(*reopened, m);
            if (!promoted.is_err()) {
                jbc::ir::CodeUnitRef cu = promoted.unwrap();
                for (const jbc::ir::InsnId id : cu->dom.order()) {
                    const jbc::ir::Insn* in = cu->dom.peek(id);
                    if (in == nullptr || !jbcbridge::isLdcFamily(in->op)) continue;
                    jbc::ir::ClassSession::ConstSem cst;
                    if (reopened->constOf(*in, cst)
                        && cst.kind == jbc::ir::ClassSession::ConstSem::Kind::String
                        && cst.str == std::string_view(e.plain)) {
                        plainInPlace = true;
                    }
                }
            }
        }
        expect(plainInPlace, std::string("结构：") + e.method + " 明文 ldc 在位");
    }

    // ── 8. 查验 B：真 JVM 对拍（原始编译产物 vs deobfed jar）──
    const fs::path origOut = deobfDir / "orig.stdout";
    const fs::path origErr = deobfDir / "orig.stderr";
    const fs::path deobfOut = deobfDir / "deobfed.stdout";
    const fs::path deobfErr = deobfDir / "deobfed.stderr";
    const bool ranOrig = runJavaCapture(tools.javaExe, outDir.string(), origOut, origErr);
    const bool ranDeobf = runJavaCapture(tools.javaExe, deobfJar.string(), deobfOut, deobfErr);
    expect(ranOrig && ranDeobf, "对拍：两次 java 运行退出码均为 0");

    bool capOk = false;
    const std::vector<u8> origOutBytes = readFileBytes(origOut, capOk);
    const std::vector<u8> deobfOutBytes = readFileBytes(deobfOut, capOk);
    expect(origOutBytes == deobfOutBytes,
           "对拍：原始版与 deobfed 版 stdout 逐字节一致（"
               + std::to_string(origOutBytes.size()) + "B / "
               + std::to_string(deobfOutBytes.size()) + "B）");
    if (origOutBytes != deobfOutBytes && !origOutBytes.empty() && !deobfOutBytes.empty()) {
        const usize n = origOutBytes.size() < deobfOutBytes.size() ? origOutBytes.size()
                                                                  : deobfOutBytes.size();
        for (usize i = 0; i < n; ++i) {
            if (origOutBytes[i] != deobfOutBytes[i]) {
                std::printf("    首个差异字节 @%zu：%02x vs %02x\n", i, origOutBytes[i],
                            deobfOutBytes[i]);
                break;
            }
        }
    }
    const std::string want = expectedJvmOutput();
    expect(std::string(origOutBytes.begin(), origOutBytes.end()) == want,
           "对拍：stdout == 期望明文（逐字节）");

    // ── 9. 汇总 ──
    std::printf("断言 %d/%d 通过\n", gChecks - gFailures, gChecks);
    if (gFailures != 0) {
        std::printf("deobf 报告：存在失败断言\n");
        return 1;
    }
    std::printf("deobf 报告：全部调用点密文还原成功，导出 jar 双查验通过\n");
    return 0;
}
