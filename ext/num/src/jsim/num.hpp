/// @file num.hpp
/// @defgroup num jsim-num 包装类型与解析包
/// @brief jsim-num 扩展包：包装类型（Integer/Long）与十进制解析。
///
/// 这些是 JDK 语义，不在核心（设计 D10）。混淆器的元数据契约里 `Integer`
/// 常被当成「把 int 藏进 Object」的载体，解析失败还要抛 NumberFormatException，
/// 所以本包对解析失败明确 Bail{NotConcrete}——异常派发 v1 不做，不编造 0。
///
///   jsim::num::install(session);

#pragma once

#include <string>

#include "jsim/engine.hpp"

namespace jsim {
namespace num {

void install(Session& session);

} // namespace num
} // namespace jsim
