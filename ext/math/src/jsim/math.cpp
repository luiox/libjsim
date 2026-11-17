#include "jsim/math.hpp"

#include <cmath>
#include <limits>

namespace jsim::math {
namespace {

template <typename T> struct Number;

template <> struct Number<i32> {
    static constexpr ValType type = ValType::I32;
    static i32 read(const SimValue& v) { return static_cast<i32>(v.intBits); }
    static SimValue write(i32 v) { return SimValue::knownI32(v); }
};
template <> struct Number<i64> {
    static constexpr ValType type = ValType::I64;
    static i64 read(const SimValue& v) { return v.intBits; }
    static SimValue write(i64 v) { return SimValue::knownI64(v); }
};
template <> struct Number<float> {
    static constexpr ValType type = ValType::F32;
    static float read(const SimValue& v) { return v.asF32(); }
    static SimValue write(float v) { return SimValue::knownF32(v); }
};
template <> struct Number<double> {
    static constexpr ValType type = ValType::F64;
    static double read(const SimValue& v) { return v.asF64(); }
    static SimValue write(double v) { return SimValue::knownF64(v); }
};

using ValueResult = Result<SimValue, Bail>;

ValueResult popNumber(Frame& frame, ValType type) {
    auto arg = frame.popChecked();
    if (!arg.isOk()) return arg;
    if (arg.value().type != type) {
        return ValueResult::err({Bail::Reason::BadOperand, frame.pc(), "Math 参数类型不匹配"});
    }
    return arg;
}

template <typename T, typename R = T>
CallMock unary(R (*fn)(T)) {
    return [fn](Frame& frame) -> ValueResult {
        auto arg = popNumber(frame, Number<T>::type);
        if (!arg.isOk()) return arg;
        const SimValue& v = arg.value();
        return ValueResult::ok(v.known ? Number<R>::write(fn(Number<T>::read(v)))
                                      : SimValue::unknownOf(Number<R>::type));
    };
}

template <typename T>
CallMock binary(T (*fn)(T, T)) {
    return [fn](Frame& frame) -> ValueResult {
        auto right = popNumber(frame, Number<T>::type);
        if (!right.isOk()) return right;
        auto left = popNumber(frame, Number<T>::type);
        if (!left.isOk()) return left;
        const SimValue& a = left.value();
        const SimValue& b = right.value();
        return ValueResult::ok(a.known && b.known
            ? Number<T>::write(fn(Number<T>::read(a), Number<T>::read(b)))
            : SimValue::unknownOf(Number<T>::type));
    };
}

template <typename T>
T absInt(T v) {
    return v < 0 && v != (std::numeric_limits<T>::min)() ? -v : v;
}

template <typename T, bool maximum>
T extremum(T a, T b) {
    if constexpr (std::numeric_limits<T>::is_iec559) {
        if (std::isnan(a)) return a;
        if (std::isnan(b)) return b;
        if (a == 0 && b == 0) {
            // Java 对两种零作全序比较，max 选 +0，min 选 -0。
            return std::signbit(a) == maximum ? b : a;
        }
    }
    return (maximum ? b > a : b < a) ? b : a;
}

template <typename T>
T signum(T v) {
    return v == 0 || std::isnan(v) ? v : std::copysign(T{1}, v);
}

template <typename T, typename I>
I round(T v) {
    if (std::isnan(v)) return 0;
    const T limit = std::ldexp(T{1}, std::numeric_limits<I>::digits);
    if (v >= limit) return (std::numeric_limits<I>::max)();
    if (v <= -limit) return (std::numeric_limits<I>::min)();

    // 拆整数与小数，避免 v + 0.5 在半数邻域或大整数上先发生浮点舍入。
    T whole;
    const T fraction = std::modf(v, &whole);
    const I truncated = static_cast<I>(whole);
    if (fraction >= T{0.5}) return truncated + 1;
    if (fraction < T{-0.5}) return truncated - 1;
    return truncated;
}

template <typename T>
void installExtrema(Session& session, const std::string& owner, char descriptor) {
    const std::string signature = std::string("(") + descriptor + descriptor + ")" + descriptor;
    session.callMocks[owner + "min" + signature] = binary<T>(extremum<T, false>);
    session.callMocks[owner + "max" + signature] = binary<T>(extremum<T, true>);
}

double roundToEven(double value) {
    double integral;
    const double fraction = std::fabs(std::modf(value, &integral));
    if (fraction > 0.5 || (fraction == 0.5 && std::fmod(integral, 2.0) != 0.0)) {
        return integral + std::copysign(1.0, value);
    }
    return integral;
}

template <typename T, typename U, bool modulus, typename R = T>
ValueResult floorDivMod(Frame& frame) {
    auto right = popNumber(frame, Number<U>::type);
    if (!right.isOk()) return right;
    auto left = popNumber(frame, Number<T>::type);
    if (!left.isOk()) return left;
    const T divisor = Number<U>::read(right.value());
    if (right.value().known && divisor == 0) {
        return ValueResult::err({Bail::Reason::BadOperand, frame.pc(), "Math floorDiv/floorMod 除数为零"});
    }
    if (!left.value().known || !right.value().known) {
        return ValueResult::ok(SimValue::unknownOf(Number<R>::type));
    }
    const T dividend = Number<T>::read(left.value());
    if (dividend == (std::numeric_limits<T>::min)() && divisor == -1) {
        return ValueResult::ok(Number<R>::write(modulus ? R{0} : static_cast<R>(dividend)));
    }
    const T remainder = dividend % divisor;
    const bool adjust = remainder != 0 && (remainder < 0) != (divisor < 0);
    if constexpr (modulus) {
        return ValueResult::ok(Number<R>::write(static_cast<R>(adjust ? remainder + divisor : remainder)));
    } else {
        return ValueResult::ok(Number<R>::write(dividend / divisor - adjust));
    }
}

void installForClass(Session& session, const std::string& owner) {
    auto& mocks = session.callMocks;
    mocks[owner + "abs(I)I"] = unary<i32>(absInt<i32>);
    mocks[owner + "abs(J)J"] = unary<i64>(absInt<i64>);
    mocks[owner + "abs(F)F"] = unary<float>(std::fabs);
    mocks[owner + "abs(D)D"] = unary<double>(std::fabs);
    installExtrema<i32>(session, owner, 'I');
    installExtrema<i64>(session, owner, 'J');
    installExtrema<float>(session, owner, 'F');
    installExtrema<double>(session, owner, 'D');
    mocks[owner + "signum(F)F"] = unary<float>(signum<float>);
    mocks[owner + "signum(D)D"] = unary<double>(signum<double>);
    mocks[owner + "round(F)I"] = unary<float, i32>(round<float, i32>);
    mocks[owner + "round(D)J"] = unary<double, i64>(round<double, i64>);
    mocks[owner + "floor(D)D"] = unary<double>(std::floor);
    mocks[owner + "ceil(D)D"] = unary<double>(std::ceil);
    mocks[owner + "sqrt(D)D"] = unary<double>(std::sqrt);
    mocks[owner + "rint(D)D"] = unary<double>(roundToEven);
    mocks[owner + "floorDiv(II)I"] = floorDivMod<i32, i32, false>;
    mocks[owner + "floorDiv(JI)J"] = floorDivMod<i64, i32, false>;
    mocks[owner + "floorDiv(JJ)J"] = floorDivMod<i64, i64, false>;
    mocks[owner + "floorMod(II)I"] = floorDivMod<i32, i32, true>;
    mocks[owner + "floorMod(JI)I"] = floorDivMod<i64, i32, true, i32>;
    mocks[owner + "floorMod(JJ)J"] = floorDivMod<i64, i64, true>;
}

} // namespace

void install(Session& session) {
    installForClass(session, "java/lang/Math.");
    installForClass(session, "java/lang/StrictMath.");
}

} // namespace jsim::math
