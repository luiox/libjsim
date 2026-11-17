/// @file indy.hpp
/// @defgroup indy jsim-indy invokedynamic 脚手架
/// @brief jsim-indy 扩展包：MethodType / MethodHandle / Lookup / CallSite 的最小建模。
///
/// 目的只有一个：**让 bootstrap 方法能跑到底，把「调用点到底指向谁」算出来**。
/// 折叠 invokedynamic 的正确做法不是去匹配某个混淆器的 BSM 形状（那样每换一版
/// 就得改一次），而是把 BSM 真跑一遍，读它返回的 CallSite 里的目标句柄——
/// 目标是谁、是静态还是虚调用，都是 BSM 自己算出来的，不是我们猜的。
///
/// 建模只到「够用」为止，不做 JVM 的方法句柄语义：
/// - `MethodType` 的载荷是描述符字符串，不做类型校验与适配；
/// - `MethodHandle` 的载荷是 (kind, owner, name, desc)，不可调用；
/// - `CallSite` 的载荷是它持有的那个句柄，不实现 `dynamicInvoker`；
/// - `Lookup` 完全不建模（它只是 `findXxx` 的接收者，身份不影响结果）。
///
/// 载荷口径与 num/reflect 一致：都挂在 `HeapObj.fields` 的 `@` 前缀槽上，
/// `MethodType` 的描述符放 `HeapObj.data`（它天然是「一个字符串」）。
///
/// 可选接入，核心零依赖本文件：
///   jsim::indy::install(session);
#pragma once

#include <string>
#include <utility>

#include "jsim/engine.hpp"

namespace jsim {
namespace indy {

/// 调用点指向的方法。折叠 pass 要的就是这四个字段。
struct Target
{
    enum class Kind : u8 {
        Static,
        Virtual,
        Special,
        Interface,
    };

    Kind        kind = Kind::Static;
    std::string owner;
    std::string name;
    std::string desc;

    bool valid() const noexcept { return !owner.empty() && !name.empty(); }
};

/// 读一个 CallSite（或 MethodHandle）对象指向的目标。
///
/// 两种入参都认：CallSite 取它持有的句柄，句柄直接读自己。读不到就返回 false
/// ——「不知道指向谁」绝不能当成「指向某个默认目标」。
bool targetOf(Session& session, const SimValue& callSite, Target& out);

/// 是不是一个 CallSite 对象（折叠 pass 用它判断 BSM 返回值能不能用）。
bool isCallSite(Session& session, const SimValue& v);

/// 造一个 MethodType 对象。给「按 BSM 静态参数表构造入参」的调用方用：
/// 参数表里 MethodType 那一项本来就是描述符字符串，不需要跑一遍
/// `fromMethodDescriptorString` 才能得到它。
SimValue makeMethodType(Session& session, std::string desc);

/// 造一个方法句柄对象。同上，供 BSM 静态参数表里的 MethodHandle 项使用。
SimValue makeMethodHandle(Session& session, Target::Kind kind, std::string owner,
                                 std::string name, std::string desc);

/// 安装方法句柄面。幂等：重复安装只是覆盖同名注册项。
void install(Session& session);

} // namespace indy
} // namespace jsim
