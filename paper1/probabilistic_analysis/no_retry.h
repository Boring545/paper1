#pragma once
#include <array>
#include <unordered_map>

#include "../canfd_frame.h"
#include "../config.h"
#include "../scheme.h"
#include "normal.h"

/**
 * 关闭CAN FD重传机制，通过增加冗余的方式来提高可靠性，计算每个信号的传输故障概率
 */
namespace cfd::analysis::noretry {

// 计算每种信号的传输故障概率,一种信号可能有多个副本，假设独立，计算“全部副本都失败”的概率作为该信号的故障概率
// 返回: key为MessageCode，value为P_fault
std::unordered_map<MessageCode, double> sig_trans_fault_prob_analysis(PackingScheme& scheme,
                                                                      double lambda = LAMBDA_CONFERENCE);

// 计算每种信号的ECU语义故障概率，基于N模冗余多数投票机制（允许两路到达形成多数），考虑通信失败和ECU语义故障两种情况
// p_comm_fail: N路通信失败概率数组
// p_ecu_fail: 单 ECU 语义故障概率
double ecu_fault_prob_analysis(const std::vector<double>& p_comm_fail, double p_ecu_fail);

// 计算信号在异源备份后基于N模冗余的故障概率
// redundancy_n: N模冗余（N为奇数，默认3）
// 返回: key为MessageCode，value为P_fault
std::unordered_map<MessageCode, double> ecu_fault_prob_analysis(PackingScheme& scheme, int redundancy_n = REDUNDANCY_N,
                                                                double lambda = LAMBDA_CONFERENCE);

}  // namespace cfd::analysis::noretry
