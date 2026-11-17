/// @file math.hpp
/// @defgroup math jsim-math 数学函数包
/// @brief java.lang.Math 的纯函数接管（doc:ext-jdk-math）。

#pragma once

#include "jsim/engine.hpp"

namespace jsim::math {

/// 为 Math 和 StrictMath 注册数值函数。已知的整数除零以 Bail 报告。
void install(Session& session);

} // namespace jsim::math
