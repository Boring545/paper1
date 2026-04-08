#include "normal.h"

#include <cmath>
#include <stdexcept>

#include "../debug_tool.h"
#include "../scheme.h"
namespace cfd::analysis {

namespace {
constexpr double MILLISECONDS_PER_SECOND = 1000.0;
}

// 计算阶乘
long long int factorial(int m) {
  // 别用它计算太大的数
  if (m < 0) {
    throw std::invalid_argument("M 必须大于0");
  }
  if (m == 0) {
    return 1;
  }
  if (m > 20) {
    throw std::invalid_argument("M>20");
  }
  long long int result = 1;
  while (m > 0) {
    result *= m;
    m--;
  }
  return result;
}
double prob_fault(double t, int num, double lambda) {
  if (num < 0) {
    throw std::invalid_argument("num must be non-negative");
  }

  // 调用方在工程内统一传入 ms，这里换算成秒后再与“每秒故障次数”相乘。
  const long double t_seconds = static_cast<long double>(std::max(0.0, t)) / MILLISECONDS_PER_SECOND;
  const long double rate = static_cast<long double>(lambda) * t_seconds;
  if (rate <= 0.0L) {
    return num == 0 ? 1.0 : 0.0;
  }

  // 使用 lgamma 计算泊松分布，避免 factorial 在重传次数较大时溢出或抛异常。
  const long double log_prob =
      -rate + static_cast<long double>(num) * std::log(rate) - std::lgammal(static_cast<long double>(num) + 1.0L);
  return static_cast<double>(std::exp(log_prob));
}

double prob_fault_one_more(double interference_win, double lambda) {
  const long double t_seconds = static_cast<long double>(std::max(0.0, interference_win)) / MILLISECONDS_PER_SECOND;
  const long double rate = static_cast<long double>(lambda) * t_seconds;
  if (rate <= 0.0L) {
    return 0.0;
  }
  return static_cast<double>(-std::expm1(-rate));
}

}  // namespace cfd::analysis
