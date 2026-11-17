/// @file jbc.hpp
/// @defgroup jbc jsim-jbc 模型搬运桥
/// @brief jsim-jbc 扩展包：libjbc 字节码模型 ↔ libjsim 模拟执行的官方桥。
///        jbc dom/instruction 面 → jsim 执行输入（Method）的形状搬运、
///        字符串/LDC 面读写（setString 自动升格口径）、消费方最小接管点。
///
/// 依赖声明：本包依赖外部包 libjbc（jbc/ir 会话面 + core Op），是扩展包
/// 位面的边界胶水（doc:design-packages）；核心（src/jsim）零依赖红线不动，核心对本包
/// 零反向依赖。与 jsim-str 等 JDK 语义包互不依赖，组合发生在用户侧。
///
/// 字符串口径：classfile 常量是 MUTF-8 字节。桥按家族口径**原样透传**给
/// jsim 载荷——BMP 非零码元下 MUTF-8 与 UTF-8 逐字节相同，jsim-str 按
/// UTF-8 码元读。来自 ldc 字面量的字符串带常量池 intern 身份事实，灌预填
/// 时用 SimValue::internedStr（同一 ldc 字面量在常量池里必然同一对象，
/// if_acmpeq 的相等语义依赖它）。
///
/// 消费方最小接管点：桥只做模型搬运，不注册任何语义。桥出来的 Method 配
/// 普通 jsim::Session 跑，缺的语义照 doc:design-takeover 接管面补——
///   - callMocks / callMocksAtPc：invoke 系（含解密器编排的跨方法接力）；
///     indy 与 invoke 同管道，CallRef 无 owner，按调用点 pc 注册是惯用法；
///   - allocScaffolds：new 系（String 构造两阶段走 jsim-str 的
///     str::installStringAlloc）；
///   - opcodeHandlers：表外 opcode 或默认语义不够时的整条接管；
///   - fieldMocks / branchHooks / callMockResolver：字段注入、分支三态裁决、
///     按接收者解析。
/// athrow 例外：异常表派发是引擎内建（catch-all/类名精确可确证时跳处理器，
/// 不可确证时 Bail），不需接管。三个注册表始终对消费方开放（透出），桥不
/// 包裹、不隐藏。

#pragma once

#include <string>
#include <string_view>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/units.hpp"

#include "jsim/engine.hpp"

namespace jsim {
namespace jbcbridge {

/// 桥接失败：结构化分 stage。桥的纪律是宁可整方法放弃，也不产出
/// 「看起来能跑」的半截方法——任何一步拿不到确定信息就整方法报错。
struct BridgeError {
    enum class Stage : u8 {
        NoCode,         ///< 方法无 Code 属性（abstract/native）
        PromoteFailed,  ///< libjbc promoteMethod 失败
        BadDom,         ///< 指令句柄悬垂 / 方法下标越界
        BadLabel,       ///< 分支或 switch 目标标签解析失败
        BadConst,       ///< ldc 族常量种类不可桥（MethodType/MethodHandle/Dynamic）
        BadMemberRef,   ///< invoke / 字段引用解析失败（CP 结构不符）
        BadDesc,        ///< 方法或字段描述符不可解析
        BadClass,       ///< new 族类常量解析失败
        UnsupportedOp,  ///< v1 明确不桥的指令族（jsr / ret 子例程）
    };

    Stage stage = Stage::BadDom;
    std::string detail;
};

/// 描述符返回段 → ValType；`V` 置 hasReturn=false（\<init> 等 void 调用）。
bool retTypeOfDesc(std::string_view desc, ValType& out, bool& hasReturn);

/// 字段描述符首段 → ValType。
bool fieldTypeOfDesc(std::string_view desc, ValType& out);

/// 指令句柄 → order() 下标的稳定散列键（owner 在单 DOM 内恒定）。
u64 insnKey(jbc::ir::InsnId id);

u64 labelKey(jbc::ir::LabelId id);

/// ldc 族判定（LDC / LDC_W / LDC2_W）。
bool isLdcFamily(jbc::core::Op op);

/// 读 ldc 族指令装载的字符串常量（MUTF-8 字节按家族口径原样透传）。
/// 非 ldc 族、非 String 常量或解析失败返回 false；命中时 out 收字节，
/// 灌预填用 SimValue::internedStr(out)（见文件头的 intern 身份口径）。
bool ldcStringOf(const jbc::ir::ClassSession& s, const jbc::ir::Insn& in,
                        std::string& out);

/// 把模拟还原出的明文写回 ldc（解密改写的出口）。语义改写走 libjbc 的
/// setString：CP 追加去重（intern），新索引超 u1 域自动升格 LDC→LDC_W
/// ——宽度陷阱由 libjbc 挡掉，不留给调用方。非 ldc 族或改写失败返回 false。
bool setLdcString(jbc::ir::ClassSession& s, jbc::ir::CodeDom& dom,
                         jbc::ir::InsnId id, std::string_view text);

/// 单方法桥接（形状搬运，无语义解释）：libjbc CodeUnit → jsim::Method。
///
/// 指令序列摊成 pc 下标、标签解析成 pc（方法末尾锚折成 code.size()）、
/// ldc 族 CP 索引重映射成 jsim 紧凑表下标（backing CP 与 jsim 表解耦，
/// 稀疏索引不搬进 jsim）。语义视图全部走 ClassSession 的 constOf /
/// memberOf / classOf——桥不手走常量池。NEWARRAY 的 atype 在 libjbc 的
/// a 槽、jsim 引擎读 b 槽，桥内显式搬运；MULTIANEWARRAY 的元素类型由
/// 类名剥掉 dims 个 '[' 得到，维数落 b。
Result<Method, BridgeError> toSimMethod(jbc::ir::ClassSession& s,
                                               ca::usize methodIdx);

} // namespace jbcbridge
} // namespace jsim
