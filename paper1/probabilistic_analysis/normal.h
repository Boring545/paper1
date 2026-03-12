#pragma once
#include <array>
#include <unordered_map>

#include "../canfd_frame.h"
#include "../config.h"
#include "../scheme.h"

namespace cfd::analysis {

// 计算时间段t内，重传num次的概率
double prob_fault(double t, int num, double lambda = LAMBDA_CONFERENCE);
// 在电磁干扰的干涉窗口内，至少发生一次故障的概率
double prob_fault_one_more(double interference_win, double lambda = LAMBDA_CONFERENCE);

// 用于计算每种信号的传输故障概率（全部失败）
std::unordered_map<MessageCode, double> sig_trans_fault_prob_analysis(PackingScheme& scheme,
                                                                      double lambda = LAMBDA_CONFERENCE);

// 计算ECU故障概率，基于N模冗余多数投票机制（允许两路到达形成多数），考虑通信失败和ECU语义故障两种情况
// p_comm_fail: N路通信失败概率
// p_ecu_fail: 单 ECU 语义故障概率
double ecu_fault_prob_analysis(const std::vector<double>& p_comm_fail, double p_ecu_fail);

// 兼容三模冗余的便捷重载
double ecu_fault_prob_analysis(const std::array<double, 3>& p_comm_fail, double p_ecu_fail);

// 计算type1信号在异源备份后基于N模冗余的故障概率
// redundancy_n: N模冗余（N为奇数，默认3）
// 返回: key为MessageCode，value为P_fault
std::unordered_map<MessageCode, double> ecu_fault_prob_analysis(PackingScheme& scheme, int redundancy_n = REDUNDANCY_N,
                                                                double lambda = LAMBDA_CONFERENCE);

// 将每小时故障概率上限转换为frame对应的发送窗口内的故障概率
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
