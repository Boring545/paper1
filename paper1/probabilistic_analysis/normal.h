#pragma once

#include "../config.h"

namespace cfd::analysis {

// 计算阶乘
long long int factorial(int m);

// 计算时间段 t_ms（单位 ms）内，发生 num 次故障的概率。
// lambda 的单位是“每秒故障次数”，函数内部会先把 ms 转成 s。
double prob_fault(double t, int num, double lambda = LAMBDA_CONFERENCE);

// 在干涉窗口 interference_win（单位 ms）内，至少发生一次故障的概率
double prob_fault_one_more(double interference_win, double lambda = LAMBDA_CONFERENCE);

// 将每小时故障概率上限转换为frame对应的发送窗口内的故障概率
// level: 安全等级（0=A, 1=B, 2=C, 3=D）
// period_ms: 帧周期，单位ms
inline double threshold_per_window(int level, int period_ms) {
  if (level < 0 || level >= NUM_THRESHOLD_RELIABILITY) return 0.0;
  const double p_hour = THRESHOLD_RELIABILITY[level];
  if (period_ms <= 0) return p_hour;
  const double p_window = p_hour * (static_cast<double>(period_ms) / 3600000.0);
  if (p_window < 0.0) return 0.0;
  if (p_window > 1.0) return 1.0;
  return p_window;
}  // 安全等级对应的帧传输时的故障率上限

}  // namespace cfd::analysis
