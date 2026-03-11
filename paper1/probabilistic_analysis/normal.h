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

// 用于计算每种信号的传输故障概率
std::unordered_map<MessageCode, double> sig_trans_fault_prob_analysis(PackingScheme& scheme,
                                                                      double lambda = LAMBDA_CONFERENCE);

// 计算ECU故障概率分析，基于N模冗余多数投票机制（允许两路到达形成多数），考虑通信失败和ECU语义故障两种情况
// p_comm_fail: N路通信失败概率
// p_ecu_fail: 单 ECU 语义故障概率
double ecu_fault_prob_analysis(const std::vector<double>& p_comm_fail, double p_ecu_fail);

// 兼容三模冗余的便捷重载
double ecu_fault_prob_analysis(const std::array<double, 3>& p_comm_fail, double p_ecu_fail);

// 计算type1信号在异源备份后基于N模冗余的成功概率
// redundancy_n: N模冗余（N为奇数，默认3）
// 返回: key为MessageCode，value为P_safe
std::unordered_map<MessageCode, double> ecu_fault_prob_analysis(PackingScheme& scheme,
                                                                int redundancy_n = 3,
                                                                double lambda = LAMBDA_CONFERENCE);

// 计算type1信号在异源备份后基于TMR的成功概率
// 返回: key为MessageCode，value为P_safe
std::unordered_map<MessageCode, double> ecu_fault_prob_analysis(PackingScheme& scheme,
                                                                double lambda = LAMBDA_CONFERENCE);

}  // namespace cfd::analysis
