/// @file str.hpp
/// @defgroup str jsim-str 字符串语义包
/// @brief jsim-str 扩展包：String、StringBuilder 与 StringBuffer 接管和分配。
///        扩展包与用户代码走同一注册面，核心零依赖本文件（doc:design-packages/D10）。

#pragma once

#include <string>

#include "jsim/engine.hpp"

namespace jsim {
namespace str {

/// 取参数的低 16 位作为 UTF-16 码元追加，可编码独立代理项。
void appendCodeUnit(std::string& buffer, i32 codeUnit);

/// 计算 UTF-8 或 MUTF-8 有效前缀的 UTF-16 码元数。
usize codeUnitCount(const std::string& buffer);

/// 取 UTF-16 码元（0 基），越界或在指定位置之前遇到非法编码时返回 -1。
i32 codeUnitAt(const std::string& buffer, usize index);

/// 注册本包全部接管。profile 一行接入：jsim::str::install(session)。
void install(Session& session);

/// 字符串构造两阶段（doc:design-packages jsim-str 职责面）：
///   1. new java/lang/String 走分配脚手架（空 data 载荷，形态未初始化）；
///   2. \<init>([C)V 把 char[] 码元逐个 appendCodeUnit 写进接收者 data——
///      构造完成前的对象只是脚手架壳，语义在第二阶段落位。
/// \<init> 的 CallRef.hasReturn=false（引擎不压返回值），初始化完的接收者
/// 留在栈顶。形态不符（非 (String, char[]) 或载荷缺失）明确 Bail，不编造。
void installStringAlloc(Session& session);

} // namespace str
} // namespace jsim
