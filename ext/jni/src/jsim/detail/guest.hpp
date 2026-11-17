#pragma once

#include "jsim/jni.hpp"

namespace jsim::jni::detail {

Result<std::string, Bail> readCStr(JniEnv& env, u64 addr, usize cap = 1u << 16);
Result<std::vector<u8>, Bail> readBytes(JniEnv& env, u64 addr, usize n);
Result<void, Bail> writeBytes(JniEnv& env, u64 addr, const void* p, usize n);
Result<std::string, Bail> classNameOfClass(JniEnv& env, u64 h);
usize elemWidthOf(const std::string& arrayClass);
bool isRefArrayClass(const std::string& arrayClass);
Result<std::vector<u16>, Bail> mutf8ToUtf16(const std::string& bytes);
std::string utf16ToMutf8(const std::vector<u16>& units);
Result<SimValue, Bail> decodeByValType(ValType t, u64 bits, JniEnv& env);
Result<u64, Bail> encodeByValType(const SimValue& v, JniEnv& env);
Result<std::vector<u64>, Bail> readJValueArray(JniEnv& env, u64 addr, const std::string& desc);
Result<std::vector<u64>, Bail> collectBareArgs(JniCall& call, const std::string& desc);
Result<std::vector<SimValue>, Bail> decodeArgs(const std::string& desc, const std::vector<u64>& bits,
                                                      JniEnv& env);
Result<std::string, Bail> strPayloadOf(JniEnv& env, u64 h);
Result<u64, Bail> materialize(JniEnv& env, const void* p, usize n, usize align = 1);
void markCopy(JniEnv& env, u64 isCopyAddr);

void installStringHooks(JniEnv& env);
void installArrayHooks(JniEnv& env);

} // namespace jsim::jni::detail
