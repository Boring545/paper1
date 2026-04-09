// paper1.cpp: 定义应用程序的入口点。

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "backups/frame_backup.h"
#include "backups/signal_backup.h"
#include "config.h"
#include "debug_tool.h"
#include "priority_allocation.h"
#include "probabilistic_analysis/no_retry.h"
#include "probabilistic_analysis/retry.h"
#include "scheme.h"
#include "storage_layout.h"

#ifdef _WIN32
extern "C" __declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int);
extern "C" __declspec(dllimport) int __stdcall SetConsoleCP(unsigned int);
#endif

#ifndef CP_UTF8
#define CP_UTF8 65001
#endif

using namespace cfd;
using namespace cfd::analysis;
namespace fs = std::filesystem;

namespace {

struct CodeMeta {
  int level = 0;
  int period_ms = 0;
  int deadline_ms = 0;
  int type = 0;
  EcuId src_ecu = 0;
  EcuId dst_ecu = 0;
};

struct ExperimentDatasetSpec {
  const char* name = "";
  size_t ecu_count = 0;
  size_t signal_count = 0;
};

struct SignalMetric {
  MessageCode code = 0;
  CodeMeta meta;
  int signal_instance_count = 1;
  int added_signal_copies = 0;
  double p_fault = 0.0;
  double p_threshold = 0.0;
  double p_wcrt_over_deadline = 0.0;
  double expected_wcrt_ms = 0.0;
  double wcrt_p95_ms = 0.0;
  double wcrt_ratio_p95 = 0.0;
};

struct SchemeMetrics {
  std::string name;
  double compare_bandwidth_utilization = 0.0;
  double static_bandwidth_utilization = 0.0;
  int total_added_signal_copies = 0;
  std::vector<SignalMetric> signals;
};

struct DatasetCompareSummary {
  std::string dataset_tag;
  std::vector<SchemeMetrics> schemes;
};

struct AsilStats {
  int count = 0;
  int added_signal_copies = 0;
  double fault_sum = 0.0;
  double fault_max = 0.0;
  double wcrt_deadline_miss_sum = 0.0;
  double wcrt_deadline_miss_max = 0.0;
  double wcrt_ratio_p95_sum = 0.0;
  double wcrt_ratio_p95_max = 0.0;
};

char level_to_asil(int level) {
  switch (level) {
    case 0:
      return 'A';
    case 1:
      return 'B';
    case 2:
      return 'C';
    case 3:
      return 'D';
    default:
      return '?';
  }
}

const std::array<ExperimentDatasetSpec, 6> kScalingDatasetSpecs = {{
    {"M1", 3, 75},
    {"M2", 3, 105},
    {"M3", 4, 135},
    {"M4", 4, 170},
    {"M5", 5, 200},
    {"M6", 5, 225},
}};

// 默认实验输入指向 storage/datasets 下当前固定的这组 M1~M6 数据集。
// 日常运行直接复用这些文件；如需覆盖重生成，修改下面的布尔开关即可。
const std::array<const char*, 6> kDefaultDatasetFiles = {{
    "msg_M1_3ecu_75signals_tab.txt",
    "msg_M2_3ecu_105signals_tab.txt",
    "msg_M3_4ecu_135signals_tab.txt",
    "msg_M4_4ecu_170signals_tab.txt",
    "msg_M5_5ecu_200signals_tab.txt",
    "msg_M6_5ecu_225signals_tab.txt",
}};

// 为了保证各对比方法从同一个初始打包结果出发，先对原始信号集做多次 SA，
// 取带宽利用率最低的那个作为共享初始 scheme。
constexpr int kInitialSchemeRestartCount = 3;

std::string scaling_dataset_filename(const ExperimentDatasetSpec& spec) {
  return "msg_" + std::string(spec.name) + "_" + std::to_string(spec.ecu_count) + "ecu_" +
         std::to_string(spec.signal_count) + "signals_tab.txt";
}

double calc_ratio(double wcrt_ms, int period_ms) {
  if (period_ms <= 0) {
    return 0.0;
  }
  return wcrt_ms / static_cast<double>(period_ms);
}

double clamp_probability(double value) { return std::clamp(value, 0.0, 1.0); }

std::unordered_map<MessageCode, CodeMeta> build_code_meta(const MessageInfoVec& original_infos) {
  std::unordered_map<MessageCode, CodeMeta> meta_by_code;
  meta_by_code.reserve(original_infos.size());

  for (const auto& info : original_infos) {
    auto it = meta_by_code.find(info.code);
    if (it == meta_by_code.end()) {
      meta_by_code.emplace(info.code, CodeMeta{info.level, info.period, info.deadline, info.type,
                                               info.ecu_pair.src_ecu, info.ecu_pair.dst_ecu});
    }
  }

  return meta_by_code;
}

std::vector<MessageCode> sorted_codes(const std::unordered_map<MessageCode, CodeMeta>& meta_by_code) {
  std::vector<MessageCode> codes;
  codes.reserve(meta_by_code.size());
  for (const auto& [code, _] : meta_by_code) {
    codes.push_back(code);
  }

  std::sort(codes.begin(), codes.end(), [&](MessageCode lhs, MessageCode rhs) {
    const auto& left = meta_by_code.at(lhs);
    const auto& right = meta_by_code.at(rhs);
    if (left.level != right.level) {
      return left.level > right.level;
    }
    if (left.period_ms != right.period_ms) {
      return left.period_ms < right.period_ms;
    }
    return lhs < rhs;
  });

  return codes;
}

// 返回时间戳
std::string normalize_dataset_output_path(const std::string& dataset_file);

std::string normalize_dataset_output_path(const std::string& dataset_file);

std::string create_msg(const std::string& dataset_file = "", size_t message_count = cfd::SIZE_ORIGINAL_MESSAGE,
                       size_t ecu_count = cfd::NUM_ECU) {
  cfd::utils::generate_msg_info_set(cfd::MESSAGE_INFO_VEC, message_count, ecu_count);
  const std::string output_path = normalize_dataset_output_path(dataset_file);
  DEBUG_MSG_DEBUG1(std::cout, "生成信号集合, ECU/数量: ", ecu_count, "/", message_count);
  DEBUG_MSG_DEBUG1(std::cout, "已写入消息数据集: ", output_path);
  cfd::utils::write_message(cfd::MESSAGE_INFO_VEC, output_path, false);
  return output_path;
}

std::vector<std::string> create_scaling_experiment_datasets() {
  std::vector<std::string> dataset_paths;
  dataset_paths.reserve(kScalingDatasetSpecs.size());
  MessageInfoVec cumulative_infos;
  size_t generated_signal_count = 0;

  for (const auto& spec : kScalingDatasetSpecs) {
    if (spec.ecu_count > cfd::NUM_ECU) {
      throw std::invalid_argument("Current config does not provide enough ECU ids for " + std::string(spec.name));
    }
    if (spec.signal_count < generated_signal_count) {
      throw std::invalid_argument("Scaling dataset signal_count must be non-decreasing");
    }

    const size_t incremental_signal_count = spec.signal_count - generated_signal_count;
    if (incremental_signal_count > 0) {
      MessageInfoVec incremental_infos;
      cfd::utils::generate_msg_info_set(incremental_infos, incremental_signal_count, spec.ecu_count);
      cumulative_infos.insert(cumulative_infos.end(), incremental_infos.begin(), incremental_infos.end());
    }

    cfd::MESSAGE_INFO_VEC = cumulative_infos;
    const std::string output_path = normalize_dataset_output_path(scaling_dataset_filename(spec));
    DEBUG_MSG_DEBUG1(std::cout, "渐进生成信号集合, 场景/新增/累计: ", spec.name, "/", incremental_signal_count, "/",
                     cumulative_infos.size());
    cfd::utils::write_message(cfd::MESSAGE_INFO_VEC, output_path, false);
    dataset_paths.push_back(output_path);
    generated_signal_count = spec.signal_count;
  }

  return dataset_paths;
}

std::string normalize_dataset_path(const std::string& dataset_file) {
  return cfd::storage::resolve_dataset_input_path(dataset_file);
}

std::string normalize_dataset_output_path(const std::string& dataset_file) {
  return cfd::storage::dataset_output_path(dataset_file);
}

std::string dataset_tag(const std::string& dataset_file) { return cfd::storage::dataset_tag_from_file(dataset_file); }

std::vector<std::string> default_dataset_files() {
  return std::vector<std::string>(kDefaultDatasetFiles.begin(), kDefaultDatasetFiles.end());
}

std::vector<std::string> resolve_experiment_dataset_files(bool regenerate_datasets) {
  // 重新生成会覆盖 storage/datasets 下当前的 M1~M6 数据集文件；
  // 默认模式始终复用现有固定数据集。
  if (regenerate_datasets) {
    return create_scaling_experiment_datasets();
  }
  return default_dataset_files();
}

// 读取某个信号集合
void read_data_1(const std::string& dataset_file = cfd::DEFAULT_MSG_FILE) {
  const std::string full_path = normalize_dataset_path(dataset_file);
  cfd::utils::read_message(full_path);
  DEBUG_MSG_DEBUG1(std::cout, "读取信号集合: ", fs::path(full_path).filename().string());
}

// 基于当前 MESSAGE_INFO_VEC 构建并优化打包方案
PackingScheme build_scheme_from_current_msgs() {
  PackingScheme scheme{};
  cfd::packing::frame_pack(scheme, cfd::DEFAULT_PACK_METHOD);
  return scheme;
}

PackingScheme build_best_shared_initial_scheme_from_current_msgs(int restart_count = kInitialSchemeRestartCount) {
  const int actual_restart_count = std::max(1, restart_count);

  PackingScheme best_scheme = build_scheme_from_current_msgs();
  double best_utilization = best_scheme.calc_bandwidth_utilization();

  for (int attempt = 2; attempt <= actual_restart_count; ++attempt) {
    PackingScheme candidate = build_scheme_from_current_msgs();
    const double candidate_utilization = candidate.calc_bandwidth_utilization();
    if (candidate_utilization < best_utilization) {
      best_scheme = std::move(candidate);
      best_utilization = candidate_utilization;
    }
  }

  DEBUG_MSG_DEBUG1(std::cout, "共享初始打包结果(多次重启后最优) Utilization = ", best_utilization);
  return best_scheme;
}

struct SignalArrivalSample {
  double completion_ms = 0.0;
  double p_fail = 1.0;
};

struct SignalWcrtStats {
  double expected_wcrt_ms = 0.0;
  double wcrt_p95_ms = 0.0;
  double p_timeout = 0.0;
};

std::unordered_map<MessageCode, std::vector<SignalArrivalSample>> build_signal_arrival_samples(const PackingScheme& scheme) {
  const auto frame_response_times = cfd::schedule::calc_frame_response_times(scheme.frame_map);

  std::unordered_map<MessageCode, std::vector<SignalArrivalSample>> samples_by_code;
  samples_by_code.reserve(MESSAGE_INFO_VEC.size());

  for (const auto& [frame_id, frame] : scheme.frame_map) {
    if (frame.empty()) {
      continue;
    }

    const auto response_it = frame_response_times.find(frame_id);
    const double completion_ms = response_it == frame_response_times.end() ? frame.get_trans_time() : response_it->second;
    const double p_fail = clamp_probability(prob_fault_one_more(frame.get_trans_time(), LAMBDA_CONFERENCE));

    for (const auto& msg : frame.msg_set) {
      samples_by_code[msg.get_code()].push_back({completion_ms, p_fail});
    }
  }

  return samples_by_code;
}

SignalWcrtStats calc_signal_probabilistic_wcrt(std::vector<SignalArrivalSample> samples, int deadline_ms) {
  SignalWcrtStats stats{};
  if (samples.empty()) {
    stats.expected_wcrt_ms = deadline_ms;
    stats.wcrt_p95_ms = deadline_ms;
    stats.p_timeout = 1.0;
    return stats;
  }

  std::sort(samples.begin(), samples.end(),
            [](const SignalArrivalSample& lhs, const SignalArrivalSample& rhs) { return lhs.completion_ms < rhs.completion_ms; });

  std::vector<std::pair<double, long double>> timely_outcomes;
  timely_outcomes.reserve(samples.size());

  long double remaining_fail_prob = 1.0L;
  long double timely_prob = 0.0L;
  for (const auto& sample : samples) {
    const long double success_prob = remaining_fail_prob * (1.0L - static_cast<long double>(sample.p_fail));
    if (sample.completion_ms <= static_cast<double>(deadline_ms)) {
      timely_outcomes.emplace_back(sample.completion_ms, success_prob);
      timely_prob += success_prob;
    }
    remaining_fail_prob *= static_cast<long double>(sample.p_fail);
  }

  stats.p_timeout = clamp_probability(1.0 - static_cast<double>(timely_prob));
  if (timely_prob <= 0.0L) {
    stats.expected_wcrt_ms = deadline_ms;
    stats.wcrt_p95_ms = deadline_ms;
    return stats;
  }

  long double weighted_sum = 0.0L;
  long double cumulative_prob = 0.0L;
  stats.wcrt_p95_ms = timely_outcomes.back().first;
  for (const auto& [completion_ms, success_prob] : timely_outcomes) {
    weighted_sum += static_cast<long double>(completion_ms) * success_prob;
    cumulative_prob += success_prob / timely_prob;
    if (cumulative_prob >= 0.95L) {
      stats.wcrt_p95_ms = completion_ms;
      break;
    }
  }

  stats.expected_wcrt_ms = static_cast<double>(weighted_sum / timely_prob);
  return stats;
}

std::unordered_map<MessageCode, int> calc_signal_instance_count(const PackingScheme& scheme) {
  std::unordered_map<MessageCode, int> counts;
  counts.reserve(MESSAGE_INFO_VEC.size());

  for (const auto& [frame_id, frame] : scheme.frame_map) {
    if (frame.empty()) {
      continue;
    }
    for (const auto& msg : frame.msg_set) {
      counts[msg.get_code()] += 1;
    }
  }

  return counts;
}

std::string build_frame_signature(const CanfdFrame& frame) {
  std::ostringstream oss;
  oss << frame.get_period() << '|' << frame.get_deadline() << '|' << frame.get_ecu_pair().src_ecu << '|'
      << frame.get_ecu_pair().dst_ecu;
  for (const auto& msg : frame.msg_set) {
    oss << '|' << msg.get_id_message();
  }
  return oss.str();
}

std::unordered_map<FrameId, double> calc_zero_offset_reference_completion(const PackingScheme& scheme) {
  PackingScheme zero_offset_scheme = scheme;
  for (auto& [frame_id, frame] : zero_offset_scheme.frame_map) {
    if (frame.empty()) {
      continue;
    }
    frame.set_offset(0.0);
  }
  return cfd::schedule::calc_frame_response_times(zero_offset_scheme.frame_map);
}

void assign_staggered_offsets_to_frame_copies(PackingScheme& scheme) {
  const auto zero_offset_response_times = calc_zero_offset_reference_completion(scheme);

  std::unordered_map<std::string, std::vector<FrameId>> groups;
  groups.reserve(scheme.frame_map.size());
  for (const auto& [frame_id, frame] : scheme.frame_map) {
    if (frame.empty()) {
      continue;
    }
    groups[build_frame_signature(frame)].push_back(frame_id);
  }

  for (auto& [signature, frame_ids] : groups) {
    (void)signature;
    if (frame_ids.size() <= 1) {
      continue;
    }

    std::sort(frame_ids.begin(), frame_ids.end(), [&](FrameId lhs, FrameId rhs) {
      const double left_response = zero_offset_response_times.count(lhs) ? zero_offset_response_times.at(lhs) : 0.0;
      const double right_response = zero_offset_response_times.count(rhs) ? zero_offset_response_times.at(rhs) : 0.0;
      if (left_response != right_response) {
        return left_response < right_response;
      }
      return lhs < rhs;
    });

    double accumulated_offset = 0.0;
    bool is_first_copy = true;
    for (const auto frame_id : frame_ids) {
      auto frame_it = scheme.frame_map.find(frame_id);
      if (frame_it == scheme.frame_map.end() || frame_it->second.empty()) {
        continue;
      }

      auto& frame = frame_it->second;
      if (is_first_copy) {
        frame.set_offset(0.0);
        is_first_copy = false;
      } else {
        const double upper_bound = std::max(0.0, static_cast<double>(frame.get_deadline()) - frame.get_trans_time());
        const double assigned_offset = std::min(accumulated_offset, upper_bound);
        frame.set_offset(assigned_offset);
      }

      const double reference_completion =
          zero_offset_response_times.count(frame_id) ? zero_offset_response_times.at(frame_id) : frame.get_trans_time();
      accumulated_offset += reference_completion;
    }
  }
}

void assign_staggered_offsets_to_signal_copies(PackingScheme& scheme) {
  const auto zero_offset_response_times = calc_zero_offset_reference_completion(scheme);

  std::unordered_map<MessageCode, std::vector<FrameId>> code_to_frames;
  code_to_frames.reserve(MESSAGE_INFO_VEC.size());
  for (const auto& [frame_id, frame] : scheme.frame_map) {
    if (frame.empty()) {
      continue;
    }
    for (const auto& msg : frame.msg_set) {
      code_to_frames[msg.get_code()].push_back(frame_id);
    }
  }

  std::unordered_map<FrameId, double> desired_offsets;
  desired_offsets.reserve(scheme.frame_map.size());

  for (auto& [code, frame_ids] : code_to_frames) {
    (void)code;
    std::sort(frame_ids.begin(), frame_ids.end());
    frame_ids.erase(std::unique(frame_ids.begin(), frame_ids.end()), frame_ids.end());
    if (frame_ids.size() <= 1) {
      continue;
    }

    std::sort(frame_ids.begin(), frame_ids.end(), [&](FrameId lhs, FrameId rhs) {
      const double left_response = zero_offset_response_times.count(lhs) ? zero_offset_response_times.at(lhs) : 0.0;
      const double right_response = zero_offset_response_times.count(rhs) ? zero_offset_response_times.at(rhs) : 0.0;
      if (left_response != right_response) {
        return left_response < right_response;
      }
      return lhs < rhs;
    });

    double accumulated_offset = 0.0;
    bool is_first_copy = true;
    for (const auto frame_id : frame_ids) {
      auto frame_it = scheme.frame_map.find(frame_id);
      if (frame_it == scheme.frame_map.end() || frame_it->second.empty()) {
        continue;
      }

      const auto& frame = frame_it->second;
      if (is_first_copy) {
        desired_offsets[frame_id] = std::max(desired_offsets[frame_id], 0.0);
        is_first_copy = false;
      } else {
        const double upper_bound = std::max(0.0, static_cast<double>(frame.get_deadline()) - frame.get_trans_time());
        desired_offsets[frame_id] = std::max(desired_offsets[frame_id], std::min(accumulated_offset, upper_bound));
      }

      const double reference_completion =
          zero_offset_response_times.count(frame_id) ? zero_offset_response_times.at(frame_id) : frame.get_trans_time();
      accumulated_offset += reference_completion;
    }
  }

  for (auto& [frame_id, frame] : scheme.frame_map) {
    if (frame.empty()) {
      continue;
    }
    const double desired_offset = desired_offsets.count(frame_id) ? desired_offsets.at(frame_id) : 0.0;
    frame.set_offset(desired_offset);
  }
}

SchemeMetrics analyze_deterministic_scheme(const std::string& name, PackingScheme& scheme,
                                           const std::unordered_map<MessageCode, CodeMeta>& meta_by_code) {
  SchemeMetrics metrics;
  metrics.name = name;
  metrics.compare_bandwidth_utilization = scheme.calc_bandwidth_utilization();
  metrics.static_bandwidth_utilization = metrics.compare_bandwidth_utilization;

  const auto p_fault_map = noretry::sig_trans_fault_prob_analysis(scheme, LAMBDA_CONFERENCE);
  const auto arrival_samples_by_code = build_signal_arrival_samples(scheme);
  const auto instance_count_map = calc_signal_instance_count(scheme);
  const auto codes = sorted_codes(meta_by_code);

  metrics.signals.reserve(codes.size());
  for (const auto code : codes) {
    const auto meta_it = meta_by_code.find(code);
    if (meta_it == meta_by_code.end()) {
      continue;
    }

    const auto p_fault_it = p_fault_map.find(code);
    const auto arrival_it = arrival_samples_by_code.find(code);
    if (p_fault_it == p_fault_map.end() || arrival_it == arrival_samples_by_code.end()) {
      continue;
    }

    const auto& meta = meta_it->second;
    const auto wcrt_stats = calc_signal_probabilistic_wcrt(arrival_it->second, meta.deadline_ms);
    const int signal_instance_count = instance_count_map.count(code) ? instance_count_map.at(code) : 1;
    const int added_signal_copies = std::max(0, signal_instance_count - 1);
    const double reported_p_fault = clamp_probability(p_fault_it->second);
    metrics.total_added_signal_copies += added_signal_copies;
    metrics.signals.push_back({code, meta, signal_instance_count, added_signal_copies, reported_p_fault,
                               threshold_per_window(meta.level, meta.period_ms), wcrt_stats.p_timeout,
                               wcrt_stats.expected_wcrt_ms, wcrt_stats.wcrt_p95_ms,
                               calc_ratio(wcrt_stats.wcrt_p95_ms, meta.period_ms)});
  }

  return metrics;
}

SchemeMetrics analyze_retry_scheme(const std::string& name, const retry::AnalysisReport& report,
                                   const std::unordered_map<MessageCode, CodeMeta>& meta_by_code) {
  SchemeMetrics metrics;
  metrics.name = name;
  // 对重传方案，直接使用期望发送次数对应的平均带宽占用。
  metrics.compare_bandwidth_utilization = report.expected_bandwidth_utilization;
  metrics.static_bandwidth_utilization = report.base_bandwidth_utilization;

  const auto codes = sorted_codes(meta_by_code);
  metrics.signals.reserve(codes.size());
  for (const auto code : codes) {
    const auto meta_it = meta_by_code.find(code);
    const auto signal_it = report.signal_results.find(code);
    if (meta_it == meta_by_code.end() || signal_it == report.signal_results.end()) {
      continue;
    }

    const auto& meta = meta_it->second;
    const auto& signal = signal_it->second;
    const double reported_p_timeout = clamp_probability(signal.p_timeout);
    metrics.signals.push_back({code, meta, 1, 0, reported_p_timeout, signal.p_threshold, reported_p_timeout,
                               signal.expected_wcrt, signal.wcrt_p95, calc_ratio(signal.wcrt_p95, meta.period_ms)});
  }

  return metrics;
}

std::array<AsilStats, NUM_MESSAGE_LEVEL> calc_asil_stats(const SchemeMetrics& metrics) {
  std::array<AsilStats, NUM_MESSAGE_LEVEL> stats{};

  for (const auto& signal : metrics.signals) {
    if (signal.meta.level < 0 || signal.meta.level >= NUM_MESSAGE_LEVEL) {
      continue;
    }

    auto& level_stats = stats[signal.meta.level];
    level_stats.count += 1;
    level_stats.added_signal_copies += signal.added_signal_copies;
    level_stats.fault_sum += signal.p_fault;
    level_stats.fault_max = std::max(level_stats.fault_max, signal.p_fault);
    level_stats.wcrt_deadline_miss_sum += signal.p_wcrt_over_deadline;
    level_stats.wcrt_deadline_miss_max = std::max(level_stats.wcrt_deadline_miss_max, signal.p_wcrt_over_deadline);
    level_stats.wcrt_ratio_p95_sum += signal.wcrt_ratio_p95;
    level_stats.wcrt_ratio_p95_max = std::max(level_stats.wcrt_ratio_p95_max, signal.wcrt_ratio_p95);
  }

  return stats;
}

std::string build_compare_output_path(const std::string& run_tag, const std::string& dataset_name) {
  return cfd::storage::compare_report_path(run_tag, dataset_name);
}

std::string build_compare_summary_output_path(const std::string& run_tag) {
  return cfd::storage::compare_summary_report_path(run_tag);
}

std::string build_retry_output_path(const std::string& run_tag, const std::string& dataset_name) {
  return cfd::storage::retry_report_path(run_tag, dataset_name);
}

void run_figure_generation_script(const std::string& run_tag) {
  const fs::path analysis_dir = cfd::storage::analysis_batch_dir(run_tag);
  const fs::path project_root = fs::absolute(fs::path(__FILE__)).parent_path().parent_path();
  const fs::path script_path = project_root / "scripts" / "generate_all_figures.py";
  const std::string command =
      "python \"" + cfd::storage::path_string(script_path) + "\" \"" + cfd::storage::path_string(analysis_dir) + "\"";
  DEBUG_MSG_DEBUG1(std::cout, "开始调用绘图脚本: ", command);
  const int exit_code = std::system(command.c_str());
  if (exit_code != 0) {
    DEBUG_MSG_DEBUG1(std::cout, "绘图脚本执行失败, exit_code=", exit_code);
  }
}

void write_comparison_report(const std::string& output_path, const std::vector<SchemeMetrics>& schemes,
                             const std::string& retry_report_path) {
  std::ofstream ofs(output_path, std::ios::trunc);
  if (!ofs) {
    DEBUG_MSG_DEBUG1(std::cout, "无法写入对比结果文件: ", output_path);
    return;
  }

  ofs << std::setprecision(17);
  ofs << "# compare_bandwidth_utilization: 用于方案对比的带宽利用率。\n";
  ofs << "# static_bandwidth_utilization: 不考虑重传时，静态打包方案本身的带宽利用率。\n";
  ofs << "# signal_count: 按原始 MessageCode 去重后的信号种类数，不是副本总数。\n";
  ofs << "# total_added_signal_copies: 相比原始 1 份信号，方案中新增的信号副本总数。\n";
  ofs << "# 三种方案的 WCRT 均按信号级统计值输出；foundation / baseline1 基于“最早成功副本到达时间”分布，baseline2 基于重传分布。\n";
  ofs << "# 本文件中的 WCRT/周期 指标统一使用 P95（95%分位响应时间）；signal_detail 里的 WCRT 列统一是信号级统计值（单位 ms）。\n";
  ofs << '\n';
  ofs << "retry_distribution_report\t" << retry_report_path << "\n\n";

  ofs << "# 每个方案的总体指标。\n";
  ofs << "[scheme_summary]\n";
  ofs << "scheme\tcompare_bandwidth_utilization\tstatic_bandwidth_utilization\tsignal_count\t"
         "total_added_signal_copies\n";
  for (const auto& scheme : schemes) {
    ofs << scheme.name << '\t' << scheme.compare_bandwidth_utilization << '\t' << scheme.static_bandwidth_utilization
        << '\t' << scheme.signals.size() << '\t' << scheme.total_added_signal_copies << '\n';
  }

  ofs << "\n# 按 ASIL 分组统计的 WCRT/周期（P95，95%分位响应时间）结果。\n";
  ofs << "\n[asil_wcrt_ratio_p95]\n";
  ofs << "scheme\tasil\tsignal_count\tavg_wcrt_ratio_p95\tmax_wcrt_ratio_p95\n";
  for (const auto& scheme : schemes) {
    const auto stats = calc_asil_stats(scheme);
    for (int level = 0; level < NUM_MESSAGE_LEVEL; ++level) {
      const auto& item = stats[level];
      if (item.count <= 0) {
        continue;
      }
      ofs << scheme.name << '\t' << level_to_asil(level) << '\t' << item.count << '\t'
          << (item.wcrt_ratio_p95_sum / item.count) << '\t' << item.wcrt_ratio_p95_max << '\n';
    }
  }

  ofs << "\n# 按 ASIL 分组统计的信号故障概率。\n";
  ofs << "\n[asil_fault_probability]\n";
  ofs << "scheme\tasil\tsignal_count\tavg_fault_probability\tmax_fault_probability\n";
  for (const auto& scheme : schemes) {
    const auto stats = calc_asil_stats(scheme);
    for (int level = 0; level < NUM_MESSAGE_LEVEL; ++level) {
      const auto& item = stats[level];
      if (item.count <= 0) {
        continue;
      }
      ofs << scheme.name << '\t' << level_to_asil(level) << '\t' << item.count << '\t' << (item.fault_sum / item.count)
          << '\t' << item.fault_max << '\n';
    }
  }

  ofs << "\n# 按 ASIL 分组统计的新增信号副本数量。\n";
  ofs << "\n[asil_added_signal_copies]\n";
  ofs << "scheme\tasil\tsignal_count\tadded_signal_copies\n";
  for (const auto& scheme : schemes) {
    const auto stats = calc_asil_stats(scheme);
    for (int level = 0; level < NUM_MESSAGE_LEVEL; ++level) {
      const auto& item = stats[level];
      if (item.count <= 0) {
        continue;
      }
      ofs << scheme.name << '\t' << level_to_asil(level) << '\t' << item.count << '\t' << item.added_signal_copies
          << '\n';
    }
  }

  ofs << "\n# 每个原始信号的明细指标。WCRT 列统一为信号级统计值（单位 ms），不是 WCRT/周期。\n";
  ofs << "\n[signal_detail]\n";
  ofs << "scheme\tcode\tasil\tlevel\ttype\tperiod_ms\tsrc_ecu\tdst_ecu\tsignal_instance_count\t"
         "added_signal_copies\tp_fault\tp_threshold\texpected_wcrt_ms\tp_wcrt_over_deadline\twcrt_p95_ms\n";
  for (const auto& scheme : schemes) {
    for (const auto& signal : scheme.signals) {
      ofs << scheme.name << '\t' << signal.code << '\t' << level_to_asil(signal.meta.level) << '\t' << signal.meta.level
          << '\t' << signal.meta.type << '\t' << signal.meta.period_ms << '\t' << signal.meta.src_ecu << '\t'
          << signal.meta.dst_ecu << '\t' << signal.signal_instance_count << '\t' << signal.added_signal_copies << '\t'
          << signal.p_fault << '\t' << signal.p_threshold << '\t' << signal.expected_wcrt_ms << '\t'
          << signal.p_wcrt_over_deadline << '\t' << signal.wcrt_p95_ms << '\n';
    }
  }
}

void write_batch_compare_summary(const std::string& output_path,
                                 const std::vector<DatasetCompareSummary>& dataset_summaries) {
  std::ofstream ofs(output_path, std::ios::trunc);
  if (!ofs) {
    DEBUG_MSG_DEBUG1(std::cout, "无法写入批量汇总结果文件: ", output_path);
    return;
  }

  ofs << std::setprecision(17);
  ofs << "# 跨六组信号集汇总的对比结果，便于直接复制到表格或画图脚本。\n";
  ofs << "# dataset: 数据集标签，对应 M1~M6。\n";
  ofs << "# scheme: foundation / baseline1 / baseline2。\n";
  ofs << "# ASIL 分组指标均可直接按 dataset + scheme + asil 进行筛选或透视。\n\n";

  ofs << "[bandwidth_utilization]\n";
  ofs << "dataset\tscheme\tcompare_bandwidth_utilization\tstatic_bandwidth_utilization\tsignal_count\t"
         "total_added_signal_copies\n";
  for (const auto& dataset_summary : dataset_summaries) {
    for (const auto& scheme : dataset_summary.schemes) {
      ofs << dataset_summary.dataset_tag << '\t' << scheme.name << '\t' << scheme.compare_bandwidth_utilization << '\t'
          << scheme.static_bandwidth_utilization << '\t' << scheme.signals.size() << '\t'
          << scheme.total_added_signal_copies << '\n';
    }
  }

  ofs << "\n[wcrt_ratio_p95]\n";
  ofs << "dataset\tscheme\tasil\tsignal_count\tavg_wcrt_ratio_p95\tmax_wcrt_ratio_p95\n";
  for (const auto& dataset_summary : dataset_summaries) {
    for (const auto& scheme : dataset_summary.schemes) {
      const auto stats = calc_asil_stats(scheme);
      for (int level = 0; level < NUM_MESSAGE_LEVEL; ++level) {
        const auto& item = stats[level];
        if (item.count <= 0) {
          continue;
        }
        ofs << dataset_summary.dataset_tag << '\t' << scheme.name << '\t' << level_to_asil(level) << '\t' << item.count
            << '\t' << (item.wcrt_ratio_p95_sum / item.count) << '\t' << item.wcrt_ratio_p95_max << '\n';
      }
    }
  }

  ofs << "\n[fault_probability]\n";
  ofs << "dataset\tscheme\tasil\tsignal_count\tavg_fault_probability\tmax_fault_probability\n";
  for (const auto& dataset_summary : dataset_summaries) {
    for (const auto& scheme : dataset_summary.schemes) {
      const auto stats = calc_asil_stats(scheme);
      for (int level = 0; level < NUM_MESSAGE_LEVEL; ++level) {
        const auto& item = stats[level];
        if (item.count <= 0) {
          continue;
        }
        ofs << dataset_summary.dataset_tag << '\t' << scheme.name << '\t' << level_to_asil(level) << '\t' << item.count
            << '\t' << (item.fault_sum / item.count) << '\t' << item.fault_max << '\n';
      }
    }
  }

  ofs << "\n[added_signal_copies]\n";
  ofs << "dataset\tscheme\tasil\tsignal_count\tadded_signal_copies\n";
  for (const auto& dataset_summary : dataset_summaries) {
    for (const auto& scheme : dataset_summary.schemes) {
      const auto stats = calc_asil_stats(scheme);
      for (int level = 0; level < NUM_MESSAGE_LEVEL; ++level) {
        const auto& item = stats[level];
        if (item.count <= 0) {
          continue;
        }
        ofs << dataset_summary.dataset_tag << '\t' << scheme.name << '\t' << level_to_asil(level) << '\t' << item.count
            << '\t' << item.added_signal_copies << '\n';
      }
    }
  }
}

DatasetCompareSummary run_compare_experiment(const std::string& dataset_file, const std::string& run_tag) {
  read_data_1(dataset_file);
  const MessageInfoVec original_infos = MESSAGE_INFO_VEC;
  const auto meta_by_code = build_code_meta(original_infos);
  const std::string dataset_name = dataset_tag(dataset_file);
  const std::string compare_output_path = build_compare_output_path(run_tag, dataset_name);
  const std::string retry_output_path = build_retry_output_path(run_tag, dataset_name);

  MESSAGE_INFO_VEC = original_infos;
  const PackingScheme shared_initial_scheme = build_best_shared_initial_scheme_from_current_msgs();

  // 基础方法：仅做信号同源备份。当前对比实验先关闭异源备份。
  MESSAGE_INFO_VEC = original_infos;
  PackingScheme scheme_base = shared_initial_scheme;
  scheme_base = backups::signal::homo_signal_backup(scheme_base);
  assign_staggered_offsets_to_signal_copies(scheme_base);
  SchemeMetrics foundation_metrics = analyze_deterministic_scheme("foundation", scheme_base, meta_by_code);

  // baseline1：从同一个共享初始打包结果出发，按帧故障概率直接生成报文副本。
  MESSAGE_INFO_VEC = original_infos;
  PackingScheme scheme_baseline1 = shared_initial_scheme;
  scheme_baseline1 = backups::frame::homo_frame_backup(scheme_baseline1);
  assign_staggered_offsets_to_frame_copies(scheme_baseline1);
  SchemeMetrics baseline1_metrics = analyze_deterministic_scheme("baseline1", scheme_baseline1, meta_by_code);

  // baseline2：不加副本，直接复用共享初始打包结果做重传分析。
  MESSAGE_INFO_VEC = original_infos;
  PackingScheme scheme_baseline2 = shared_initial_scheme;
  retry::AnalysisReport retry_report = retry::probabilistic_analysis_report(scheme_baseline2, retry_output_path);
  SchemeMetrics baseline2_metrics = analyze_retry_scheme("baseline2", retry_report, meta_by_code);
  write_comparison_report(compare_output_path, {foundation_metrics, baseline1_metrics, baseline2_metrics},
                          retry_output_path);

  DEBUG_MSG_DEBUG1(std::cout, "对比结果已输出: ", compare_output_path);

  return {dataset_name, {foundation_metrics, baseline1_metrics, baseline2_metrics}};
}

}  // namespace

int main() {
#ifdef _WIN32
  // 让 Windows 控制台按 UTF-8 显示
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
#endif

  // 默认模式复用 storage/datasets 下当前固定的 M1~M6 数据集；
  // 如需生成一套新数据，改为 true。
  const bool regenerate_datasets = false;
  // 跑完整批实验后，自动调用 Python 绘图脚本生成图表。
  const bool generate_figures_after_analysis = true;

  const std::vector<std::string> dataset_files = resolve_experiment_dataset_files(regenerate_datasets);

  const std::string batch_run_tag = get_time_stamp();
  DEBUG_MSG_DEBUG1(std::cout,
                   "批量测试输出目录: ", cfd::storage::path_string(cfd::storage::analysis_batch_dir(batch_run_tag)));

  // 对每个数据集依次运行 4 种方案，并把该数据集的汇总结果暂存在内存中。
  std::vector<DatasetCompareSummary> dataset_summaries;
  dataset_summaries.reserve(dataset_files.size());
  for (const auto& dataset_file : dataset_files) {
    dataset_summaries.push_back(run_compare_experiment(dataset_file, batch_run_tag));
  }

  // 输出跨数据集的总汇总文件，供后续表格整理和绘图直接使用。
  const std::string compare_summary_output_path = build_compare_summary_output_path(batch_run_tag);
  write_batch_compare_summary(compare_summary_output_path, dataset_summaries);
  DEBUG_MSG_DEBUG1(std::cout, "批量汇总结果已输出: ", compare_summary_output_path);

  if (generate_figures_after_analysis) {
    run_figure_generation_script(batch_run_tag);
  }

  return 0;
}
