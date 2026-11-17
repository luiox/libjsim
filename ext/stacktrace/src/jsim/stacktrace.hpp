/// @file stacktrace.hpp
/// @defgroup stacktrace jsim-stacktrace 调用栈包
/// @brief 把调用方提供的帧物化成 Thread.getStackTrace 结果（doc:ext-jdk-stacktrace）。

#pragma once

#include <functional>
#include <optional>

#include "jsim/engine.hpp"

namespace jsim::stacktrace {
/// 单个 JVM 栈帧。类名使用点分二进制名，负行号表示缺少源码行，-2 表示 native。
struct Element {
    std::string className;
    std::string methodName;
    std::optional<std::string> fileName;
    i32 lineNumber = -1;
};

/// 按最新帧优先提供栈帧。调用方可读取模拟方法体与 pc，并决定失败或返回空栈。
using Provider = std::function<Result<std::vector<Element>, Bail>(const Frame&)>;

/// 将 currentThread 绑定到当前会话中的虚拟线程。仅为该线程提供 getStackTrace。
void install(Session& session, Provider provider);

} // namespace jsim::stacktrace
