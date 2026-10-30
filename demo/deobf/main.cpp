/// @file main.cpp
/// @brief 静态 deobf demo：运行时 javac 编译合成样本 → 最小面 classfile 解析 →
///        桥接 jsim::Method → 模拟执行解密器，还原全部调用点密文并断言。
///
/// 管线：
///   JsimObfSample.java --javac --release 8--> JsimObfSample.class（临时目录，不入库）
///     --解析--> 解密器指令序列 + 调用点密文（ldc 字符串）
///     --桥接--> jsim::Method（指令 → Insn 表，字节偏移 → 指令下标）
///     --Sim--> 每站点灌密文跑解密器 --> 明文 --> 断言 --> deobf 报告。
///
/// JDK 定位走 libjdk_finder 0.1.1（demo target 独占依赖，核心零依赖纪律不动）：
/// find_jdk 探测（JAVA_HOME → MJT_JDK_HOME → 默认安装位置扫描）→
/// jdk_executable 解析 bin/javac 绝对路径。
///
/// 本文件的 classfile 解析器是 demo 自备桥（脚手架），不是库能力：仓的核心
/// 零依赖纪律不动，正式消费方桥见设计稿 §9（CodeUnit → jsim::Method 的
/// 会话面由消费方自备，桥不在本仓）。核心/扩展不认的语义按接管面注册：
/// jsim-str 出 String.length/charAt；new java/lang/String 的分配脚手架与
/// String.<init>([C)V 的「构造两阶段」语义由本 demo 注册（demo/main.cpp
/// 场景 8/9 同款用法），不改库核心语义。

/// std::getenv / std::fopen 在 MSVC CRT 标记为 deprecated；demo 有意用可移植面。
#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "jdk_finder/jdk_finder.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

#include "jsim/engine.hpp"
#include "jsim/str.hpp"

using namespace jsim;

namespace fs = std::filesystem;

namespace {

/// 本文件用的窄位宽别名（jsim 家族只到 u8/u32，这里补齐 classfile 需要的档位）。
using u16 = std::uint16_t;
using i16 = std::int16_t;
using i8 = std::int8_t;

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

// ──────────────── JDK 定位（libjdk_finder）与现场编译 ────────────────

/// JDK 定位：libjdk_finder 0.1.1 的 find_jdk（JAVA_HOME → MJT_JDK_HOME →
/// 默认安装位置扫描），再从定位到的 JDK 解析 bin/javac 绝对路径。
/// 找不到时打印指引并返回空 path。
fs::path locateJavac() {
    const jdk_finder::JdkLocation jdk = jdk_finder::find_jdk();
    if (!jdk.available) {
        std::printf("libjdk_finder 未定位到 JDK：%s\n", jdk.reason.c_str());
        std::printf("请安装 JDK（12+ 支持 --release 8；建议 JDK 17），或设置 JAVA_HOME /\n");
        std::printf("MJT_JDK_HOME 指向含 bin/javac 的 JDK 主目录。\n");
        return {};
    }
    const fs::path javac = jdk_finder::jdk_executable(jdk, "javac");
    if (javac.empty()) {
        std::printf("JDK 已定位（%s）但 bin 下无 javac，请确认是完整 JDK 而非 JRE。\n",
                    jdk.java_home.string().c_str());
        return {};
    }
    std::printf("JDK 定位（libjdk_finder 0.1.1）：%s\n", jdk.java_home.string().c_str());
    return javac;
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
    // cmd.exe 的老坑：整条命令以引号开头时会被剥错引号，外面再包一层。
    const int rc = std::system(("\"" + cmd + "\"").c_str());
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

// ──────────────── 最小面 classfile 解析（demo 自备桥） ────────────────

/// 常量池条目：只存 demo 需要的字段，槽位含义按 tag 解释。
struct CpInfo {
    u8 tag = 0;
    u16 a = 0;         ///< Class/String→utf8 下标；Memberref→class 下标；NameAndType→name 下标
    u16 b = 0;         ///< Memberref→NameAndType 下标；NameAndType→desc 下标
    i32 intValue = 0;  ///< Integer 值 / Float 位型
    i64 longValue = 0; ///< Long 值 / Double 位型
    std::string utf8;  ///< Utf8 原始字节（MUTF-8）
};

struct MethodInfo {
    std::string name;
    std::string desc;
    u16 access = 0;
    u16 maxLocals = 0;
    std::vector<u8> code; ///< 空 = 无 Code 属性
};

struct ClassFile {
    u16 major = 0;
    std::string thisClass;
    std::vector<CpInfo> cp; ///< 1 基，cp[0] 占位
    std::vector<MethodInfo> methods;

    const std::string& utf8At(u16 idx) const {
        if (idx == 0 || idx >= cp.size() || cp[idx].tag != 1) {
            throw std::runtime_error("常量池 utf8 下标非法：" + std::to_string(idx));
        }
        return cp[idx].utf8;
    }

    std::string classNameAt(u16 idx) const {
        if (idx == 0 || idx >= cp.size() || cp[idx].tag != 7) {
            throw std::runtime_error("常量池 class 下标非法：" + std::to_string(idx));
        }
        return utf8At(cp[idx].a);
    }

    /// Methodref/InterfaceMethodref → owner + name + desc。
    struct MemberRef {
        std::string owner;
        std::string name;
        std::string desc;
    };

    MemberRef methodRefAt(u16 idx) const {
        if (idx == 0 || idx >= cp.size() || (cp[idx].tag != 9 && cp[idx].tag != 10 && cp[idx].tag != 11)) {
            throw std::runtime_error("常量池 methodref 下标非法：" + std::to_string(idx));
        }
        const CpInfo& ref = cp[idx];
        const CpInfo& nat = cp[ref.b];
        return {classNameAt(ref.a), utf8At(nat.a), utf8At(nat.b)};
    }
};

class ByteReader {
public:
    explicit ByteReader(const std::vector<u8>& bytes) : bytes_(&bytes) {}

    u8 u1() {
        need(1);
        return (*bytes_)[pos_++];
    }
    u16 u2() {
        need(2);
        const u16 v = static_cast<u16>((static_cast<u16>((*bytes_)[pos_]) << 8) | (*bytes_)[pos_ + 1]);
        pos_ += 2;
        return v;
    }
    u32 u4() {
        need(4);
        const u32 v = (static_cast<u32>(u2()) << 16) | u2();
        return v;
    }
    i32 s2() { return static_cast<i16>(u2()); }
    i32 s4() { return static_cast<i32>(u4()); }
    std::string raw(usize len) {
        need(len);
        std::string out(reinterpret_cast<const char*>(bytes_->data() + pos_), len);
        pos_ += len;
        return out;
    }
    void skip(usize n) {
        need(n);
        pos_ += n;
    }
    usize pos() const { return pos_; }
    usize remaining() const { return bytes_->size() - pos_; }
    void seek(usize p) {
        if (p > bytes_->size()) throw std::runtime_error("classfile 越界 seek");
        pos_ = p;
    }

private:
    void need(usize n) const {
        if (pos_ + n > bytes_->size()) throw std::runtime_error("classfile 越界读取");
    }
    const std::vector<u8>* bytes_;
    usize pos_ = 0;
};

/// 只解析常量池 + 方法表 + Code 属性；字段表/属性表按长度跳过。
ClassFile parseClass(const std::vector<u8>& bytes) {
    ByteReader r(bytes);
    if (r.u4() != 0xCAFEBABEu) throw std::runtime_error("非 class 文件（magic 不符）");
    ClassFile cf;
    r.u2(); // minor
    cf.major = r.u2();
    const u16 cpCount = r.u2();
    cf.cp.resize(cpCount);
    for (u16 i = 1; i < cpCount; ++i) {
        CpInfo e;
        e.tag = r.u1();
        switch (e.tag) {
        case 1: e.utf8 = r.raw(r.u2()); break;             // Utf8
        case 3: e.intValue = static_cast<i32>(r.u4()); break; // Integer
        case 4: e.intValue = static_cast<i32>(r.u4()); break; // Float（位型）
        case 5:                                                // Long（占两槽）
            e.longValue = (static_cast<i64>(r.u4()) << 32) | r.u4();
            cf.cp[i++] = e;
            break;
        case 6:                                                // Double（占两槽）
            e.longValue = (static_cast<i64>(r.u4()) << 32) | r.u4();
            cf.cp[i++] = e;
            break;
        case 7: e.a = r.u2(); break;                          // Class → utf8
        case 8: e.a = r.u2(); break;                          // String → utf8
        case 9: case 10: case 11: e.a = r.u2(); e.b = r.u2(); break; // 成员引用
        case 12: e.a = r.u2(); e.b = r.u2(); break;           // NameAndType
        case 15: e.a = r.u1(); e.b = r.u2(); break;           // MethodHandle
        case 16: e.a = r.u2(); break;                         // MethodType
        case 17: case 18: e.a = r.u2(); e.b = r.u2(); break;  // Dynamic/InvokeDynamic
        case 19: case 20: e.a = r.u2(); break;                // Module/Package
        default: throw std::runtime_error("未知常量池 tag：" + std::to_string(e.tag));
        }
        cf.cp[i] = std::move(e);
    }
    r.skip(2); // access_flags
    cf.thisClass = cf.classNameAt(r.u2());
    r.skip(2); // super_class
    r.skip(static_cast<usize>(r.u2()) * 2); // interfaces

    const auto skipAttributes = [&r, &cf]() {
        const u16 count = r.u2();
        for (u16 i = 0; i < count; ++i) {
            r.u2(); // name_index
            const u32 len = r.u4();
            r.skip(len);
        }
    };
    const u16 fieldCount = r.u2(); // 字段表：按长度跳过
    for (u16 i = 0; i < fieldCount; ++i) {
        r.skip(6);
        skipAttributes();
    }
    const u16 methodCount = r.u2();
    cf.methods.resize(methodCount);
    for (u16 i = 0; i < methodCount; ++i) {
        MethodInfo& mi = cf.methods[i];
        mi.access = r.u2();
        mi.name = cf.utf8At(r.u2());
        mi.desc = cf.utf8At(r.u2());
        const u16 attrCount = r.u2();
        for (u16 k = 0; k < attrCount; ++k) {
            const std::string attrName = cf.utf8At(r.u2());
            const u32 len = r.u4();
            const usize attrStart = r.pos();
            if (attrName == "Code") {
                r.u2();          // max_stack
                mi.maxLocals = r.u2();
                const u32 codeLen = r.u4();
                if (r.pos() + codeLen > attrStart + len) {
                    throw std::runtime_error("Code 属性 code_length 越界");
                }
                mi.code = std::vector<u8>(bytes.begin() + static_cast<std::ptrdiff_t>(r.pos()),
                                          bytes.begin() + static_cast<std::ptrdiff_t>(r.pos() + codeLen));
            }
            r.seek(attrStart + len); // 异常表/其余属性整体跳过
        }
    }
    return cf;
}

/// MUTF-8 → UTF-16 码元。BMP 内与标准 UTF-8 同形，差别只在 NUL（C0 80）与
/// 代理项（按两个独立码元保留，与 str 扩展的码元模拟口径一致）；坏编码抛错。
std::vector<i32> decodeMutf8(std::string_view bytes) {
    std::vector<i32> units;
    usize i = 0;
    while (i < bytes.size()) {
        const u8 lead = static_cast<u8>(bytes[i]);
        i32 unit = 0;
        if (lead < 0x80) {
            unit = lead;
            i += 1;
        } else if ((lead & 0xE0) == 0xC0 && i + 1 < bytes.size()) {
            unit = ((lead & 0x1F) << 6) | (static_cast<u8>(bytes[i + 1]) & 0x3F);
            i += 2;
        } else if ((lead & 0xF0) == 0xE0 && i + 2 < bytes.size()) {
            unit = ((lead & 0x0F) << 12) | ((static_cast<u8>(bytes[i + 1]) & 0x3F) << 6) |
                   (static_cast<u8>(bytes[i + 2]) & 0x3F);
            i += 3;
        } else {
            throw std::runtime_error("MUTF-8 坏编码（demo 只覆盖 BMP 码元）");
        }
        units.push_back(unit);
    }
    return units;
}

/// 码元序列 → jsim 字符串（str 扩展的 UTF-8 码元模拟口径）。
std::string unitsToSimStr(const std::vector<i32>& units) {
    std::string out;
    for (i32 unit : units) str::appendCodeUnit(out, unit);
    return out;
}

// ─────────────────── 指令解码与 jsim::Method 桥接 ───────────────────

/// 中间形：一条解码后的指令。a/b 含义按 opcode：分支/跳转的 a 是字节偏移
/// （相对本指令起点），其余是指令操作数原值。
struct RawInsn {
    u32 offset = 0;
    u8 opcode = 0;
    i32 a = 0;
    i32 b = 0;
};

/// 按字节边界完整走一遍方法体（含变长指令与 wide 展开）。
std::vector<RawInsn> walkCode(const std::vector<u8>& code) {
    std::vector<RawInsn> out;
    ByteReader r(code);
    while (r.remaining() > 0) {
        RawInsn insn;
        insn.offset = static_cast<u32>(r.pos());
        insn.opcode = r.u1();
        switch (insn.opcode) {
        case 0x10: insn.a = static_cast<i8>(r.u1()); break;            // bipush
        case 0x11: insn.a = r.s2(); break;                              // sipush
        case 0x12: insn.a = r.u1(); break;                              // ldc
        case 0x13: case 0x14: insn.a = r.u2(); break;                   // ldc_w / ldc2_w
        case 0x15: case 0x16: case 0x17: case 0x18: case 0x19:          // 变长 load
        case 0x36: case 0x37: case 0x38: case 0x39: case 0x3a:          // 变长 store
            insn.a = r.u1();
            break;
        case 0x84: insn.a = r.u1(); insn.b = static_cast<i8>(r.u1()); break; // iinc
        case 0x99: case 0x9a: case 0x9b: case 0x9c: case 0x9d: case 0x9e: // if<cond>
        case 0x9f: case 0xa0: case 0xa1: case 0xa2: case 0xa3: case 0xa4:
        case 0xa5: case 0xa6: case 0xa7: case 0xa9:                     // goto / jsr
        case 0xc6: case 0xc7: insn.a = r.s2(); break;                   // ifnull 族
        case 0xaa: { // tableswitch：4 字节对齐后 default/lo/hi/偏移表
            r.skip((4u - ((insn.offset + 1u) % 4u)) % 4u);
            r.s4();
            const i32 lo = r.s4();
            const i32 hi = r.s4();
            r.skip(static_cast<usize>(hi - lo + 1) * 4);
            break;
        }
        case 0xab: { // lookupswitch
            r.skip((4u - ((insn.offset + 1u) % 4u)) % 4u);
            r.s4();
            const u32 pairs = r.u4() / 2; // match+offset 成对，每对 8 字节
            r.skip(static_cast<usize>(pairs) * 8);
            break;
        }
        case 0xb2: case 0xb3: case 0xb4: case 0xb5: // 字段
        case 0xb6: case 0xb7: case 0xb8: case 0xb9: // 调用
        case 0xbb: case 0xbd: case 0xc0: case 0xc1: // new/anewarray/checkcast/instanceof
            insn.a = r.u2();
            break;
        case 0xba: insn.a = r.u2(); r.skip(2); break;                   // invokedynamic
        case 0xbc: insn.b = r.u1(); break;                              // newarray（atype）
        case 0xc5: insn.a = r.u2(); insn.b = r.u1(); break;             // multianewarray
        case 0xc8: case 0xc9: insn.a = r.s4(); break;                   // goto_w / jsr_w
        case 0xc4: { // wide：展开成底层指令，宽操作数直接落位
            const u8 widened = r.u1();
            if (widened == 0x84) { // iinc：u2 槽位 + s2 增量
                insn.opcode = widened;
                insn.a = r.u2();
                insn.b = r.s2();
            } else if ((widened >= 0x15 && widened <= 0x19) || (widened >= 0x36 && widened <= 0x3a)) {
                insn.opcode = widened;
                insn.a = r.u2();
            } else {
                throw std::runtime_error("wide 形态 demo 未桥接");
            }
            break;
        }
        default: break; // 无操作数指令（常量/栈操作/算术/转换/比较/返回/数组/monitor…）
        }
        out.push_back(insn);
    }
    return out;
}

/// 描述符返回段 → SimValue 类型；V 置 hasReturn=false。
ValType returnTypeOf(std::string_view desc, bool& hasReturn) {
    const usize close = desc.rfind(')');
    const std::string_view ret = close == std::string_view::npos ? desc : desc.substr(close + 1);
    hasReturn = !(ret == "V");
    if (ret.empty()) throw std::runtime_error("描述符返回段为空");
    switch (ret[0]) {
    case 'B': case 'C': case 'S': case 'Z': case 'I': return ValType::I32;
    case 'J': return ValType::I64;
    case 'F': return ValType::F32;
    case 'D': return ValType::F64;
    default: return ValType::Ref; // L...; / [
    }
}

/// 字段描述符整体 → SimValue 类型。
ValType fieldTypeOf(std::string_view desc) {
    bool unused = false;
    return returnTypeOf("(" + std::string(desc), unused); // 复用返回段解析
}

/// RawInsn → jsim::Method。跳转/分支的 a 从字节偏移重映射成指令下标；ldc 与
/// invoke/new 的常量池引用搬进 Method 自带的 cp/calls/fields/allocTypes 侧表。
Method translateMethod(const ClassFile& cf, const MethodInfo& mi) {
    const std::vector<RawInsn> raw = walkCode(mi.code);
    std::unordered_map<u32, u32> indexAt;
    for (u32 i = 0; i < raw.size(); ++i) indexAt[raw[i].offset] = i;

    Method m;
    m.name = mi.name;
    m.desc = mi.desc;
    m.maxLocals = mi.maxLocals;

    for (const RawInsn& insn : raw) {
        const auto emit = [&](i32 a = 0, i32 b = 0) { m.code.push_back({insn.opcode, a, b}); };
        const auto targetIndex = [&]() -> i32 {
            const auto it = indexAt.find(insn.offset + static_cast<u32>(insn.a));
            if (it == indexAt.end()) throw std::runtime_error("跳转目标不对齐指令边界");
            return static_cast<i32>(it->second);
        };

        switch (insn.opcode) {
        // ldc 家族：常量池条目搬进 Method.cp，按条目种类定型。
        case op::Ldc: case op::LdcW: case op::Ldc2W: {
            const CpInfo& src = cf.cp.at(static_cast<usize>(insn.a));
            CpEntry entry;
            switch (src.tag) {
            case 8: entry.kind = CpEntry::Kind::Str; entry.strValue = unitsToSimStr(decodeMutf8(cf.utf8At(src.a))); break;
            case 3: entry.kind = CpEntry::Kind::Int; entry.intValue = src.intValue; break;
            case 4: entry.kind = CpEntry::Kind::Float; entry.intValue = src.intValue; break; // 位型
            case 5: entry.kind = CpEntry::Kind::Long; entry.longValue = src.longValue; break;
            case 6: entry.kind = CpEntry::Kind::Double; entry.longValue = src.longValue; break; // 位型
            case 7: entry.kind = CpEntry::Kind::Class; entry.className = cf.classNameAt(static_cast<u16>(insn.a)); break;
            default: throw std::runtime_error("ldc 常量池条目种类 demo 未桥接");
            }
            m.cp.push_back(std::move(entry));
            emit(static_cast<i32>(m.cp.size()) - 1);
            break;
        }
        // 调用族：Methodref → CallRef（isStatic 由 opcode 决定，引擎按它弹参）。
        case op::InvokeVirtual: case op::InvokeSpecial: case op::InvokeStatic:
        case op::InvokeInterface: {
            const ClassFile::MemberRef ref = cf.methodRefAt(static_cast<u16>(insn.a));
            CallRef call;
            call.owner = ref.owner;
            call.name = ref.name;
            call.desc = ref.desc;
            call.retType = returnTypeOf(ref.desc, call.hasReturn);
            call.isStatic = insn.opcode == op::InvokeStatic;
            m.calls.push_back(std::move(call));
            emit(static_cast<i32>(m.calls.size()) - 1);
            break;
        }
        // 字段族：Fieldref → FieldRef（值类型由描述符决定）。
        case op::GetStatic: case op::PutStatic: case op::GetField: case op::PutField: {
            const ClassFile::MemberRef ref = cf.methodRefAt(static_cast<u16>(insn.a));
            FieldRef field;
            field.owner = ref.owner;
            field.name = ref.name;
            field.desc = ref.desc;
            field.type = fieldTypeOf(ref.desc);
            m.fields.push_back(std::move(field));
            emit(static_cast<i32>(m.fields.size()) - 1);
            break;
        }
        case op::New: // new：类名进 allocTypes，分配语义由 allocScaffolds 提供
            m.allocTypes.push_back(cf.classNameAt(static_cast<u16>(insn.a)));
            emit(static_cast<i32>(m.allocTypes.size()) - 1);
            break;
        case op::ANewArray: // anewarray：CP 类名即元素类型（引擎自加前缀 [）
            m.allocTypes.push_back(cf.classNameAt(static_cast<u16>(insn.a)));
            emit(static_cast<i32>(m.allocTypes.size()) - 1);
            break;
        case op::MultiANewArray: { // multianewarray：剥掉 dims 个 [ 得元素类型
            std::string cls = cf.classNameAt(static_cast<u16>(insn.a));
            const usize dims = static_cast<usize>(insn.b);
            bool leadingBrackets = cls.size() >= dims;
            for (usize k = 0; k < dims; ++k) leadingBrackets = leadingBrackets && cls[k] == '[';
            if (!leadingBrackets) {
                throw std::runtime_error("multianewarray 类名与维数不符");
            }
            m.allocTypes.push_back(cls.substr(dims));
            emit(static_cast<i32>(m.allocTypes.size()) - 1, insn.b);
            break;
        }
        // checkcast/instanceof：引擎默认按恒等/Unknown 处理，操作数无需落位。
        case op::CheckCast: case op::InstanceOf:
            emit();
            break;
        // 分支与跳转：字节偏移 → 指令下标。
        case op::IfEq: case op::IfNe: case op::IfLt: case op::IfGe: case op::IfGt:
        case op::IfLe: case op::IfIcmpeq: case op::IfIcmpne: case op::IfIcmplt:
        case op::IfIcmpge: case op::IfIcmpgt: case op::IfIcmple: case op::IfAcmpeq:
        case op::IfAcmpne: case op::IfNull: case op::IfNonNull:
            emit(targetIndex());
            break;
        case op::Goto:
            emit(targetIndex());
            break;
        default:
            // 其余（常量/装载存储/算术/转换/栈操作/iinc/newarray/返回/athrow/
            // monitor）操作数原样透传，jsim 效应表已有语义。
            emit(insn.a, insn.b);
            break;
        }
    }
    return m;
}

// ───────────────────────── 调用点扫描 ─────────────────────────

struct CallSite {
    std::string method;  ///< 调用点所在方法名
    std::string cipher;  ///< ldc 密文（jsim UTF-8 码元串）
};

/// 形态匹配：ldc(String) 紧邻 invokestatic 解密器（owner/name/desc 全等）。
std::vector<CallSite> scanCallSites(const ClassFile& cf, const std::string& owner,
                                    const std::string& name, const std::string& desc) {
    std::vector<CallSite> sites;
    for (const MethodInfo& mi : cf.methods) {
        if (mi.name == name && mi.desc == desc) continue; // 解密器自身不含调用点
        const std::vector<RawInsn> raw = walkCode(mi.code);
        for (usize i = 0; i + 1 < raw.size(); ++i) {
            const bool isLdcStr = (raw[i].opcode == op::Ldc || raw[i].opcode == op::LdcW) &&
                                  cf.cp.at(static_cast<usize>(raw[i].a)).tag == 8;
            if (!isLdcStr || raw[i + 1].opcode != op::InvokeStatic) continue;
            const ClassFile::MemberRef callee = cf.methodRefAt(static_cast<u16>(raw[i + 1].a));
            if (callee.owner != owner || callee.name != name || callee.desc != desc) continue;
            const std::string& mutf8 = cf.utf8At(cf.cp.at(static_cast<usize>(raw[i].a)).a);
            sites.push_back({mi.name, unitsToSimStr(decodeMutf8(mutf8))});
        }
    }
    return sites;
}

// ───────────────────────── 解密器模拟还原 ─────────────────────────

/// 每站点一个干净 Session：jsim-str 出 String.length/charAt；String 构造
/// 两阶段语义 demo 自备——new 出脚手架对象，<init>([C)V 把 char[] 码元逐个
/// appendCodeUnit 写进对象 data；areturn 返回该对象，报告从堆里读 data。
Session makeSession() {
    Session session;
    str::install(session);
    session.allocScaffolds["java/lang/String"] = [] { return std::string(); };
    session.callMocks["java/lang/String.<init>([C)V"] =
        [](Frame& frame) -> Result<SimValue, Bail> {
        Result<SimValue, Bail> arg = frame.popChecked(); // char[] 引用
        if (!arg.isOk()) return Result<SimValue, Bail>::err(arg.error());
        Result<SimValue, Bail> recv = frame.popChecked(); // new 出的 String 对象
        if (!recv.isOk()) return Result<SimValue, Bail>::err(recv.error());
        if (!arg.value().known || !recv.value().known ||
            arg.value().ref.kind != RefPayload::Kind::Obj ||
            recv.value().ref.kind != RefPayload::Kind::Obj) {
            return Result<SimValue, Bail>::err(
                {Bail::Reason::BadOperand, frame.pc(), "String.<init>：栈形态非 (String, char[])"});
        }
        auto& heap = frame.session().heap;
        auto argIt = heap.find(arg.value().ref.objId);
        auto recvIt = heap.find(recv.value().ref.objId);
        if (argIt == heap.end() || recvIt == heap.end() || argIt->second.className != "[C") {
            return Result<SimValue, Bail>::err(
                {Bail::Reason::BadOperand, frame.pc(), "String.<init>：char[] 载荷缺失"});
        }
        std::string data;
        for (i64 unit : argIt->second.elems) str::appendCodeUnit(data, static_cast<i32>(unit));
        recvIt->second.data = std::move(data);
        // hasReturn=false：<init> 无返回值，引擎不压栈；初始化完的接收者留在栈顶。
        return Result<SimValue, Bail>::ok(SimValue::knownObj(recv.value().ref.objId));
    };
    return session;
}

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

} // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8); // 源码字面量是 UTF-8，报告含非 ASCII 明文
#endif
    std::printf("[jsim-demo-deobf] 静态 deobf：javac 现场编译 + classfile 自备桥 + 解密器模拟还原\n");

    // 1. 定位源码 + 现场编译。
    fs::path src = locateSampleSource(argc > 0 ? argv[0] : "");
    if (src.empty()) {
        std::printf("找不到 demo/deobf/java/JsimObfSample.java（CWD 与可执行文件目录均未命中）\n");
        return 1;
    }
    std::error_code canonicalEc;
    src = fs::weakly_canonical(src, canonicalEc); // 转绝对路径，交给 javac 前钉死
    std::printf("样本源码：%s\n", src.string().c_str());
    const fs::path javac = locateJavac();
    if (javac.empty()) return 1;
    const fs::path outDir = fs::temp_directory_path() / "jsim-demo-deobf";
    if (!compileSample(javac, src, outDir)) return 1;

    // 2. 读 class + 最小面解析。
    std::vector<u8> bytes;
    {
        FILE* f = std::fopen((outDir / "JsimObfSample.class").string().c_str(), "rb");
        if (f == nullptr) {
            std::printf("编译产物 JsimObfSample.class 不存在（javac 输出命名异常）\n");
            return 1;
        }
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        bytes.resize(static_cast<usize>(size));
        const usize got = size > 0 ? std::fread(bytes.data(), 1, bytes.size(), f) : 0;
        std::fclose(f);
        if (got != bytes.size()) {
            std::printf("JsimObfSample.class 读取不完整\n");
            return 1;
        }
    }
    const ClassFile cf = parseClass(bytes);
    std::printf("解析：%s（class v%u，%zu 个方法）\n", cf.thisClass.c_str(), cf.major,
                cf.methods.size());
    expect(cf.major == 52, "编译代次为 Java 8（class v52）");

    // 3. 找解密器并桥接（按自起名 + 签名定位；解密器是静态 (Object)String）。
    const MethodInfo* decryptor = nullptr;
    for (const MethodInfo& mi : cf.methods) {
        if (mi.name == "decrypt" && mi.desc == "(Ljava/lang/Object;)Ljava/lang/String;" &&
            (mi.access & 0x0008) != 0) {
            decryptor = &mi;
        }
    }
    if (decryptor == nullptr) {
        std::printf("未找到静态解密器 decrypt(Ljava/lang/Object;)Ljava/lang/String;\n");
        return 1;
    }
    const Method method = translateMethod(cf, *decryptor);
    std::printf("解密器：decrypt(Ljava/lang/Object;)Ljava/lang/String;，%zu 条指令，maxLocals=%u\n",
                method.code.size(), method.maxLocals);

    // 4. 指令形态核对（与合成样本声明的混淆模式对齐）。
    bool hasNewArrayChar = false;
    bool hasCastore = false;
    bool hasStringCtor = false;
    bool hasBackJump = false;
    for (u32 i = 0; i < method.code.size(); ++i) {
        const Insn& insn = method.code[i];
        if (insn.opcode == op::NewArray && insn.b == 5) hasNewArrayChar = true; // newarray char
        if (insn.opcode == op::CAStore) hasCastore = true;
        if (insn.opcode == op::InvokeSpecial) {
            const CallRef& call = method.calls.at(static_cast<usize>(insn.a));
            if (call.owner == "java/lang/String" && call.name == "<init>" && call.desc == "([C)V") {
                hasStringCtor = true;
            }
        }
        const bool isJump = insn.opcode == op::Goto ||
                            (insn.opcode >= op::IfEq && insn.opcode <= op::IfAcmpne);
        if (isJump && static_cast<u32>(insn.a) < i) hasBackJump = true;
    }
    expect(hasNewArrayChar, "形态：newarray char（char[] 写回）");
    expect(hasCastore, "形态：castore（逐码元落格）");
    expect(hasStringCtor, "形态：new String(char[]) 构造收尾");
    expect(hasBackJump, "形态：回边（逆序循环）");

    // 5. 扫描调用点密文。
    const std::vector<CallSite> sites = scanCallSites(cf, cf.thisClass, "decrypt",
                                                      "(Ljava/lang/Object;)Ljava/lang/String;");
    std::printf("调用点（ldc 密文 → invokestatic decrypt）：%zu 处\n", sites.size());
    expect(sites.size() == sizeof(kExpectations) / sizeof(kExpectations[0]),
           "调用点数与期望表一致（" + std::to_string(sizeof(kExpectations) / sizeof(kExpectations[0])) + " 处）");

    // 6. 逐站点：密文灌成 ldc 字面量（internedStr），跑解密器，断言明文。
    usize index = 0;
    for (const CallSite& site : sites) {
        const Expectation* want = nullptr;
        for (const Expectation& e : kExpectations) {
            if (site.method == e.method) want = &e;
        }
        std::printf("  站点 %s：%s\n", site.method.c_str(), escapeCipher(site.cipher).c_str());

        Session session = makeSession();
        Prefill prefill;
        prefill.locals = {SimValue::internedStr(site.cipher)}; // 密文来自 ldc 字面量
        RunResult r = Sim(method, session).runEntry(std::move(prefill), 1u << 16);

        std::string recovered;
        bool completed = r.kind == RunResult::Kind::Completed && r.hasReturnValue &&
                         r.returnValue.known && r.returnValue.ref.kind == RefPayload::Kind::Obj;
        if (completed) {
            const auto it = session.heap.find(r.returnValue.ref.objId);
            if (it != session.heap.end()) recovered = it->second.data;
            else completed = false;
        }
        std::printf("    还原：\"%s\"（", recovered.c_str());
        printResult(r);
        std::printf("）\n");
        if (!completed) {
            expect(false, site.method + "：解密器未正常返回字符串");
            ++index;
            continue;
        }
        if (want == nullptr) {
            expect(false, site.method + "：无对应期望条目");
            ++index;
            continue;
        }
        std::printf("    期望：\"%s\"\n", want->plain);
        expect(recovered == want->plain,
               std::string(site.method) + "：明文 == 期望（" + want->plain + "）");
        ++index;
    }

    // 7. 汇总。
    std::printf("断言 %d/%d 通过\n", gChecks - gFailures, gChecks);
    if (gFailures != 0) {
        std::printf("deobf 报告：存在失败断言\n");
        return 1;
    }
    std::printf("deobf 报告：全部调用点密文还原成功\n");
    return 0;
}
