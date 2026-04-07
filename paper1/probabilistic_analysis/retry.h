#ifndef PROBABILISTIC_ANALYSIS_H
#define PROBABILISTIC_ANALYSIS_H

#include <string>
#include <unordered_map>
#include <vector>

#include "debug_tool.h"
#include "scheme.h"

namespace cfd::analysis::retry {

/*方法来自 Probabilistic analysis of CAN with faults。
对已经分配优先级的帧集合做概率分析，输出每个帧的重传分布、WCRT 分布，以及每个信号的超时概率。*/

// 电磁兼容性（EMC）测试标准对应的干扰脉冲宽度，单位 ms，这里默认视为 0。
extern double EI_LEN;

struct RetryDistributionPoint {
  int retry_count = 0;
  double probability = 0.0;
};

struct ResponseDistributionPoint {
  double response_time = 0.0;
  double probability = 0.0;
};

struct ProbData {
  MessageID id = 0;
  FrameId frame_id = 0;
  int period = 0;
  int level = 0;
  int type = 0;
  double p_threshold = 0.0;  // 按信号周期换算后的故障概率阈值
  double p_timeout = 1.0;    // 信号总超时概率
  double expected_wcrt = 0.0;
  double wcrt_p95 = 0.0;
  double wcrt_p99 = 0.0;
  double expected_retry_count = 0.0;
};

struct FrameProbData {
  FrameId frame_id = 0;
  int priority = -1;
  int period = 0;
  int deadline = 0;
  double trans_time = 0.0;
  double expected_response_time = 0.0;
  double expected_retry_count = 0.0;
  double expected_tx_count = 1.0;
  int rounded_expected_retry_count = 0;
  double rounded_expected_tx_count = 1.0;
  int retry_p99 = 0;
  double tx_count_p99 = 1.0;
  double p_timeout = 0.0;
  double wcrt_p95 = 0.0;
  double wcrt_p99 = 0.0;
  std::vector<RetryDistributionPoint> retry_distribution;
  std::vector<ResponseDistributionPoint> wcrt_distribution;
};

struct AnalysisReport {
  double base_bandwidth_utilization = 0.0;
  double expected_bandwidth_utilization = 0.0;
  double rounded_expected_bandwidth_utilization = 0.0;
  double bandwidth_utilization_p99 = 0.0;
  std::unordered_map<FrameId, FrameProbData> frame_results;
  std::unordered_map<MessageCode, ProbData> signal_results;
};

// 返回完整概率分析报告，并将分布结果写入 storage 目录。
AnalysisReport probabilistic_analysis_report(PackingScheme& scheme, std::string timestamp = get_time_stamp());

// 兼容旧接口：仅返回信号级结果。
std::unordered_map<MessageCode, ProbData> probabilistic_analysis(PackingScheme& scheme,
                                                                 std::string timestamp = get_time_stamp());

}  // namespace cfd::analysis::retry

#endif  // !PROBABILISTIC_ANALYSIS_H
