/// @file random.hpp
/// @defgroup random jsim-random 随机数包
/// @brief java.util.Random 的 48 位生成器（doc:ext-jdk-random）。

#pragma once

#include <functional>
#include <optional>

#include "jsim/engine.hpp"

namespace jsim::random {
/// 为无参构造提供种子。缺席或返回 std::nullopt 时，初始种子为 Unknown。
using SeedProvider = std::function<std::optional<i64>(const Frame&)>;

/// 注册 Random 构造、setSeed 与基础 next 系列。种子随堆对象保存在 Session。
void install(Session& session, SeedProvider defaultSeed = {});

} // namespace jsim::random
