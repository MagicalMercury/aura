#pragma once
// ============================================================
// aura_rt/builtin/math.h — `math` 内置模块
//
// README §16: math 提供纯函数数学运算，无副作用、无 throws、无 GC 分配
//
// 使用方式：
//   import math
//   let x = math.abs(-3.5)
//
// C++ 映射：math.abs(x) → math::abs(x)（import 生成 namespace math = aura_rt::math）
// int 实参经 C++ 隐式转换（math.abs(-7) → math::abs(int) → int→double 提升）
// ============================================================

#include <cmath>

namespace aura_rt {
namespace math {

// abs(x) → |x|（float 签名；int 实参隐式提升）
inline double abs(double x)          { return std::abs(x); }
// sqrt(x) → √x（x<0 → NaN）
inline double sqrt(double x)         { return std::sqrt(x); }
// floor(x) → 向下取整
inline double floor(double x)        { return std::floor(x); }
// ceil(x) → 向上取整
inline double ceil(double x)         { return std::ceil(x); }
// round(x) → 四舍五入（.5 远离零）
inline double round(double x)        { return std::round(x); }
// pow(x, y) → x^y
inline double pow(double x, double y) { return std::pow(x, y); }
// exp(x) → e^x
inline double exp(double x)          { return std::exp(x); }
// log(x) → ln(x)（x<=0 → -inf/NaN）
inline double log(double x)          { return std::log(x); }

} // namespace math
} // namespace aura_rt
