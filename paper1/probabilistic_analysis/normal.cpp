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
  const double t_seconds = std::max(0.0, t) / MILLISECONDS_PER_SECOND;
  const double rate = lambda * t_seconds;
  if (rate <= 0.0) {
    return num == 0 ? 1.0 : 0.0;
  }

  // 使用 lgamma 计算泊松分布，避免 factorial 在重传次数较大时溢出或抛异常。
  const double log_prob = -rate + num * std::log(rate) - std::lgamma(static_cast<double>(num) + 1.0);
  return std::exp(log_prob);
}

double prob_fault_one_more(double interference_win, double lambda) {
  return 1 - prob_fault(interference_win, 0, lambda);
}

}  // namespace cfd::analysis
