// paper1.cpp: 定义应用程序的入口点。

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "algorithm2/algorithm2.h"
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
  std::string name;
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
  double threshold_wcrt_ms = 0.0;
  double threshold_wcrt_ratio = 0.0;
};

struct SchemeMetrics {
  std::string name;
  double compare_bandwidth_utilization = 0.0;
  double static_bandwidth_utilization = 0.0;
  int total_added_signal_copies = 0;
  bool schedulable = false;
  std::vector<SignalMetric> signals;
};

struct SignalFrameMappingRow {
  std::string dataset_tag;
  std::string scheme_name;
  MessageCode code = 0;
  MessageID message_id = 0;
  int signal_instance_index = 1;
  int signal_instance_count = 1;
  int is_added_copy = 0;
  FrameId frame_id = 0;
  int frame_priority = -1;
  int frame_period_ms = 0;
  int frame_deadline_ms = 0;
  double frame_offset_ms = 0.0;
  double frame_trans_time_ms = 0.0;
  int frame_payload_bytes = 0;
  int frame_message_count = 0;
  int signal_period_ms = 0;
  int signal_deadline_ms = 0;
  int level = 0;
  int type = 0;
  EcuId src_ecu = 0;
  EcuId dst_ecu = 0;
};

struct DatasetCompareSummary {
  std::string dataset_tag;
  std::string config_tag;
  std::vector<SchemeMetrics> schemes;
  std::vector<SchemeMetrics> wcrt_schemes;
  std::vector<SignalFrameMappingRow> signal_frame_mappings;
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

const std::array<size_t, 3> kScalingEcuCounts = {{5, 8, 12}};
const std::array<size_t, 6> kScalingSignalCounts = {{50, 80, 120, 150, 200, 250}};
const std::array<size_t, 1> kSelectedRunSignalCounts = {{250}};

const std::vector<ExperimentDatasetSpec> kScalingDatasetSpecs = [] {
  std::vector<ExperimentDatasetSpec> specs;
  specs.reserve(kScalingEcuCounts.size() * kScalingSignalCounts.size());
  for (size_t ecu_count : kScalingEcuCounts) {
    for (size_t signal_count : kScalingSignalCounts) {
      specs.push_back({"E" + std::to_string(ecu_count) + "S" + std::to_string(signal_count), ecu_count, signal_count});
    }
  }
  return specs;
}();

const std::array<const char*, 3> kFocusedBatchSchemeNames = {{"foundation", "baseline1", "baseline2"}};
const std::array<const char*, 4> kWcrtSchemeNames = {
    {"foundation_no_offset", "baseline1_no_offset", "foundation", "baseline1"}};
constexpr size_t kEdAsildCandidateCount = 10;
constexpr int kEdAsildMinPeriodMs = 2;

const ExperimentDatasetSpec* find_dataset_spec(std::string_view name) {
  for (const auto& spec : kScalingDatasetSpecs) {
    if (spec.name == name) {
      return &spec;
    }
  }
  return nullptr;
}

std::vector<std::string> split_csv_arg(const std::string& csv) {
  std::vector<std::string> items;
  std::stringstream ss(csv);
  std::string item;
  while (std::getline(ss, item, ',')) {
    item.erase(std::remove_if(item.begin(), item.end(), [](unsigned char ch) { return std::isspace(ch); }), item.end());
    if (!item.empty()) {
      items.push_back(item);
    }
  }
  return items;
}

std::string dataset_config_tag_from_dataset_tag(const std::string& dataset_tag) {
  const size_t last_sep = dataset_tag.find_last_of('_');
  if (last_sep == std::string::npos || last_sep + 1 >= dataset_tag.size()) {
    return dataset_tag;
  }

  const std::string suffix = dataset_tag.substr(last_sep + 1);
  if (!suffix.empty() &&
      std::all_of(suffix.begin(), suffix.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
    return dataset_tag.substr(0, last_sep);
  }
  return dataset_tag;
}

// 为了保证各对比方法从同一个初始打包结果出发，只构建一次共享初始 scheme。

std::string scaling_dataset_filename(const ExperimentDatasetSpec& spec) {
  return "msg_" + std::string(spec.name) + "_" + std::to_string(spec.ecu_count) + "ecu_" +
         std::to_string(spec.signal_count) + "signals_tab.txt";
}

std::string batched_dataset_directory_name(const ExperimentDatasetSpec& spec) {
  return std::string(spec.name) + "_" + std::to_string(spec.ecu_count) + "ecu_" + std::to_string(spec.signal_count) +
         "signals";
}

std::string batched_dataset_filename(const ExperimentDatasetSpec& spec, size_t batch_index) {
  std::ostringstream oss;
  oss << "datasets/" << batched_dataset_directory_name(spec) << "/msg_" << spec.name << "_"
      << std::to_string(spec.ecu_count) << "ecu_" << std::to_string(spec.signal_count) << "signals_" << std::setw(3)
      << std::setfill('0') << batch_index << "_tab.txt";
  return oss.str();
}

std::vector<const ExperimentDatasetSpec*> resolve_selected_specs(const std::vector<std::string>& selected_spec_names);

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
      meta_by_code.emplace(info.code, CodeMeta{info.level, info.period, info.deadline, info.type, info.ecu_pair.src_ecu,
                                               info.ecu_pair.dst_ecu});
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
  std::unordered_map<size_t, MessageInfoVec> cumulative_infos_by_ecu_count;
  std::unordered_map<size_t, size_t> generated_signal_count_by_ecu_count;

  for (const auto& spec : kScalingDatasetSpecs) {
    if (spec.ecu_count > cfd::NUM_ECU) {
      throw std::invalid_argument("Current config does not provide enough ECU ids for " + spec.name);
    }
    auto& cumulative_infos = cumulative_infos_by_ecu_count[spec.ecu_count];
    auto& generated_signal_count = generated_signal_count_by_ecu_count[spec.ecu_count];

    if (spec.signal_count < generated_signal_count) {
      throw std::invalid_argument("Scaling dataset signal_count must be non-decreasing within the same ecu_count");
    }

    const size_t incremental_signal_count = spec.signal_count - generated_signal_count;
    if (incremental_signal_count > 0) {
      MessageInfoVec incremental_infos;
      cfd::utils::generate_msg_info_set(incremental_infos, incremental_signal_count, spec.ecu_count);
      cumulative_infos.insert(cumulative_infos.end(), incremental_infos.begin(), incremental_infos.end());
    }

    cfd::MESSAGE_INFO_VEC = cumulative_infos;
    const std::string output_path = normalize_dataset_output_path(scaling_dataset_filename(spec));
    DEBUG_MSG_DEBUG1(std::cout, "渐进生成信号集合, 场景/ECU/新增/累计: ", spec.name, "/", spec.ecu_count, "/",
                     incremental_signal_count, "/", cumulative_infos.size());
    cfd::utils::write_message(cfd::MESSAGE_INFO_VEC, output_path, false);
    dataset_paths.push_back(output_path);
    generated_signal_count = spec.signal_count;
  }

  return dataset_paths;
}

std::vector<std::string> create_random_dataset_batches(size_t batch_count_per_spec) {
  std::vector<std::string> dataset_paths;
  dataset_paths.reserve(kScalingDatasetSpecs.size() * batch_count_per_spec);

  for (const auto& spec : kScalingDatasetSpecs) {
    if (spec.ecu_count > cfd::NUM_ECU) {
      throw std::invalid_argument("Current config does not provide enough ECU ids for " + spec.name);
    }

    DEBUG_MSG_DEBUG1(std::cout, "开始批量生成配置: ", spec.name, ", count=", batch_count_per_spec);
    for (size_t batch_index = 1; batch_index <= batch_count_per_spec; ++batch_index) {
      cfd::utils::generate_msg_info_set(cfd::MESSAGE_INFO_VEC, spec.signal_count, spec.ecu_count);
      const std::string output_path = normalize_dataset_output_path(batched_dataset_filename(spec, batch_index));
      cfd::utils::write_message(cfd::MESSAGE_INFO_VEC, output_path, false);
      dataset_paths.push_back(output_path);
    }
  }

  return dataset_paths;
}

int remaining_ed_signal_level_for_rank(size_t rank, size_t total) {
  size_t count_a = static_cast<size_t>(std::llround(static_cast<double>(total) * 0.83));
  size_t count_b = static_cast<size_t>(std::llround(static_cast<double>(total) * 0.11));
  if (count_a + count_b > total) {
    count_b = total - count_a;
  }

  if (rank < count_a) return 0;
  if (rank < count_a + count_b) return 1;
  return 2;
}

void apply_ed_asild_candidate_distribution(MessageInfoVec& infos, size_t candidate_count = kEdAsildCandidateCount) {
  std::vector<size_t> eligible_indices;
  eligible_indices.reserve(infos.size());
  for (size_t index = 0; index < infos.size(); ++index) {
    const auto& info = infos[index];
    if (info.comm_id == 0 && info.period >= kEdAsildMinPeriodMs) {
      eligible_indices.push_back(index);
    }
  }
  if (eligible_indices.size() < candidate_count) {
    throw std::runtime_error("ED dataset needs " + std::to_string(candidate_count) +
                             " ASIL D candidates with period >= " + std::to_string(kEdAsildMinPeriodMs) +
                             "ms, but only found " + std::to_string(eligible_indices.size()));
  }

  std::random_device rd;
  std::mt19937 gen(rd());
  std::shuffle(eligible_indices.begin(), eligible_indices.end(), gen);
  eligible_indices.resize(candidate_count);
  std::sort(eligible_indices.begin(), eligible_indices.end());

  std::set<size_t> candidate_indices(eligible_indices.begin(), eligible_indices.end());
  std::vector<size_t> remaining_primary_indices;
  remaining_primary_indices.reserve(infos.size() - candidate_indices.size());
  for (size_t index = 0; index < infos.size(); ++index) {
    if (infos[index].comm_id != 0) continue;
    if (candidate_indices.find(index) == candidate_indices.end()) {
      remaining_primary_indices.push_back(index);
    }
  }

  for (auto& info : infos) {
    if (info.comm_id == 0) {
      info.type = 0;
    }
  }
  for (const size_t index : candidate_indices) {
    infos[index].level = 3;
    infos[index].type = 0;
  }
  for (size_t rank = 0; rank < remaining_primary_indices.size(); ++rank) {
    infos[remaining_primary_indices[rank]].level =
        remaining_ed_signal_level_for_rank(rank, remaining_primary_indices.size());
  }
}

std::vector<std::string> create_ed_asild_dataset_batches(size_t batch_count_per_spec,
                                                         const std::vector<std::string>& selected_spec_names) {
  const auto specs = resolve_selected_specs(selected_spec_names);
  std::vector<std::string> dataset_paths;
  dataset_paths.reserve(specs.size() * batch_count_per_spec);

  for (const auto* spec : specs) {
    if (spec->ecu_count > cfd::NUM_ECU) {
      throw std::invalid_argument("Current config does not provide enough ECU ids for " + spec->name);
    }

    DEBUG_MSG_DEBUG1(std::cout, "开始生成ED专用ASIL D候选数据集: ", spec->name, ", count=", batch_count_per_spec);
    for (size_t batch_index = 1; batch_index <= batch_count_per_spec; ++batch_index) {
      cfd::utils::generate_msg_info_set(cfd::MESSAGE_INFO_VEC, spec->signal_count, spec->ecu_count);
      apply_ed_asild_candidate_distribution(cfd::MESSAGE_INFO_VEC);
      const std::string output_path = normalize_dataset_output_path(batched_dataset_filename(*spec, batch_index));
      cfd::utils::write_message(cfd::MESSAGE_INFO_VEC, output_path, false);
      dataset_paths.push_back(output_path);
    }
  }

  return dataset_paths;
}

std::vector<const ExperimentDatasetSpec*> resolve_selected_specs(const std::vector<std::string>& selected_spec_names) {
  std::vector<const ExperimentDatasetSpec*> specs;
  if (selected_spec_names.empty()) {
    specs.reserve(kScalingDatasetSpecs.size());
    for (const auto& spec : kScalingDatasetSpecs) {
      specs.push_back(&spec);
    }
    return specs;
  }

  specs.reserve(selected_spec_names.size());
  for (const auto& spec_name : selected_spec_names) {
    const ExperimentDatasetSpec* spec = find_dataset_spec(spec_name);
    if (spec == nullptr) {
      throw std::invalid_argument("Unknown dataset spec: " + spec_name);
    }
    specs.push_back(spec);
  }
  return specs;
}

std::vector<std::string> resolve_dataset_batch_files(const std::vector<std::string>& selected_spec_names,
                                                     size_t max_batches_per_spec = 0) {
  const auto specs = resolve_selected_specs(selected_spec_names);
  std::vector<std::string> dataset_files;

  for (const auto* spec : specs) {
    const fs::path dir = cfd::storage::dataset_root() / batched_dataset_directory_name(*spec);
    if (!fs::exists(dir) || !fs::is_directory(dir)) {
      throw std::runtime_error("Missing dataset batch directory: " + cfd::storage::path_string(dir));
    }

    std::vector<fs::path> paths;
    for (const auto& entry : fs::directory_iterator(dir)) {
      if (!entry.is_regular_file()) {
        continue;
      }
      if (entry.path().extension() != ".txt") {
        continue;
      }
      paths.push_back(entry.path());
    }

    std::sort(paths.begin(), paths.end());
    if (paths.empty()) {
      throw std::runtime_error("No dataset batch files found in: " + cfd::storage::path_string(dir));
    }

    if (max_batches_per_spec > 0 && paths.size() > max_batches_per_spec) {
      paths.resize(max_batches_per_spec);
    }

    for (const auto& path : paths) {
      dataset_files.push_back(cfd::storage::path_string(path));
    }
  }

  return dataset_files;
}

std::string normalize_dataset_path(const std::string& dataset_file) {
  return cfd::storage::resolve_dataset_input_path(dataset_file);
}

std::string normalize_dataset_output_path(const std::string& dataset_file) {
  return cfd::storage::dataset_output_path(dataset_file);
}

std::string dataset_tag(const std::string& dataset_file) { return cfd::storage::dataset_tag_from_file(dataset_file); }

std::vector<std::string> default_dataset_files() {
  std::vector<std::string> dataset_files;
  dataset_files.reserve(kScalingEcuCounts.size() * kSelectedRunSignalCounts.size());
  for (const auto& spec : kScalingDatasetSpecs) {
    if (std::find(kSelectedRunSignalCounts.begin(), kSelectedRunSignalCounts.end(), spec.signal_count) ==
        kSelectedRunSignalCounts.end()) {
      continue;
    }
    dataset_files.push_back(scaling_dataset_filename(spec));
  }
  return dataset_files;
}

bool dataset_file_exists(const std::string& dataset_file) {
  return fs::exists(normalize_dataset_path(dataset_file));
}

std::vector<std::string> resolve_experiment_dataset_files(bool regenerate_datasets) {
  // 重新生成会覆盖 storage/datasets 下当前配置的实验数据文件；
  // 默认模式始终复用现有固定数据集。
  if (regenerate_datasets) {
    return create_scaling_experiment_datasets();
  }

  const std::vector<std::string> dataset_files = default_dataset_files();
  const bool all_dataset_files_exist =
      std::all_of(dataset_files.begin(), dataset_files.end(), [](const std::string& dataset_file) {
        return dataset_file_exists(dataset_file);
      });
  if (all_dataset_files_exist) {
    return dataset_files;
  }

  DEBUG_MSG_DEBUG1(std::cout, "默认实验数据集缺失，自动重新生成当前配置所需数据集。");
  return create_scaling_experiment_datasets();
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

PackingScheme build_shared_initial_scheme_from_current_msgs() {
  PackingScheme scheme = build_scheme_from_current_msgs();
  DEBUG_MSG_DEBUG1(std::cout, "共享初始打包结果(单次打包) Utilization = ", scheme.calc_bandwidth_utilization());
  return scheme;
}

bool log_scheme_schedulability(const std::string& scheme_name, const PackingScheme& scheme) {
  const bool schedulable = cfd::schedule::feasibility_check(scheme.frame_map);
  DEBUG_MSG_DEBUG1(std::cout, "最终可调度性判定, scheme=", scheme_name, ", result=",
                   schedulable ? "schedulable" : "unschedulable");
  return schedulable;
}

struct SignalArrivalSample {
  double completion_ms = 0.0;
  double p_fail = 1.0;
};

struct SignalWcrtStats {
  double expected_wcrt_ms = 0.0;
  double wcrt_p95_ms = 0.0;
  double p_timeout = 0.0;
  double threshold_wcrt_ms = 0.0;
};

std::unordered_map<MessageCode, double> build_signal_group_max_wcrt_map(const PackingScheme& scheme) {
  const auto frame_response_times = cfd::schedule::calc_frame_response_times(scheme.frame_map);

  std::unordered_map<MessageCode, double> max_wcrt_by_code;
  max_wcrt_by_code.reserve(MESSAGE_INFO_VEC.size());

  for (const auto& [frame_id, frame] : scheme.frame_map) {
    if (frame.empty()) {
      continue;
    }

    const double response_time_ms =
        frame_response_times.count(frame_id) ? frame_response_times.at(frame_id) : frame.get_trans_time();

    for (const auto& msg : frame.msg_set) {
      auto& current = max_wcrt_by_code[msg.get_code()];
      current = std::max(current, response_time_ms);
    }
  }

  return max_wcrt_by_code;
}

std::unordered_map<MessageCode, std::vector<SignalArrivalSample>> build_signal_arrival_samples(
    const PackingScheme& scheme) {
  const auto frame_response_times = cfd::schedule::calc_frame_response_times(scheme.frame_map);

  std::unordered_map<MessageCode, std::vector<SignalArrivalSample>> samples_by_code;
  samples_by_code.reserve(MESSAGE_INFO_VEC.size());

  for (const auto& [frame_id, frame] : scheme.frame_map) {
    if (frame.empty()) {
      continue;
    }

    const auto response_it = frame_response_times.find(frame_id);
    const double completion_ms =
        response_it == frame_response_times.end() ? frame.get_trans_time() : response_it->second;
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
    stats.threshold_wcrt_ms = deadline_ms;
    return stats;
  }

  std::sort(samples.begin(), samples.end(), [](const SignalArrivalSample& lhs, const SignalArrivalSample& rhs) {
    return lhs.completion_ms < rhs.completion_ms;
  });

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
    stats.threshold_wcrt_ms = deadline_ms;
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
  stats.threshold_wcrt_ms = deadline_ms;
  return stats;
}

double calc_signal_threshold_wcrt(std::vector<SignalArrivalSample> samples, int deadline_ms, double timeout_threshold) {
  if (samples.empty()) {
    return static_cast<double>(deadline_ms);
  }

  std::sort(samples.begin(), samples.end(), [](const SignalArrivalSample& lhs, const SignalArrivalSample& rhs) {
    return lhs.completion_ms < rhs.completion_ms;
  });

  long double remaining_fail_prob = 1.0L;
  long double timely_success_prob = 0.0L;
  for (const auto& sample : samples) {
    const long double success_prob = remaining_fail_prob * (1.0L - static_cast<long double>(sample.p_fail));
    if (sample.completion_ms <= static_cast<double>(deadline_ms)) {
      timely_success_prob += success_prob;
      const double miss_prob = clamp_probability(1.0 - static_cast<double>(timely_success_prob));
      if (miss_prob <= timeout_threshold + 1e-15) {
        return sample.completion_ms;
      }
    }
    remaining_fail_prob *= static_cast<long double>(sample.p_fail);
  }

  return static_cast<double>(deadline_ms);
}

double calc_response_threshold_wcrt(const std::vector<retry::ResponseDistributionPoint>& distribution, double deadline_ms,
                                    double timeout_threshold) {
  if (distribution.empty()) {
    return deadline_ms;
  }

  double cumulative = 0.0;
  for (const auto& point : distribution) {
    cumulative += point.probability;
    const double miss_prob = clamp_probability(1.0 - cumulative);
    if (miss_prob <= timeout_threshold + 1e-15) {
      return point.response_time;
    }
  }

  return deadline_ms;
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
  metrics.schedulable = cfd::schedule::feasibility_check(scheme.frame_map);

  const auto p_fault_map = noretry::sig_trans_fault_prob_analysis(scheme, LAMBDA_CONFERENCE);
  const auto arrival_samples_by_code = build_signal_arrival_samples(scheme);
  const auto max_wcrt_by_code = build_signal_group_max_wcrt_map(scheme);
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
    const auto max_wcrt_it = max_wcrt_by_code.find(code);
    if (p_fault_it == p_fault_map.end() || arrival_it == arrival_samples_by_code.end() ||
        max_wcrt_it == max_wcrt_by_code.end()) {
      continue;
    }

    const auto& meta = meta_it->second;
    const auto wcrt_stats = calc_signal_probabilistic_wcrt(arrival_it->second, meta.deadline_ms);
    const double group_wcrt_ms = max_wcrt_it->second;
    const int signal_instance_count = instance_count_map.count(code) ? instance_count_map.at(code) : 1;
    const int added_signal_copies = std::max(0, signal_instance_count - 1);
    const double reported_p_fault = clamp_probability(p_fault_it->second);
    const double timeout_threshold = threshold_per_window(meta.level, meta.period_ms);
    if (wcrt_stats.p_timeout > timeout_threshold + 1e-15 || group_wcrt_ms > static_cast<double>(meta.deadline_ms)) {
      metrics.schedulable = false;
    }
    metrics.total_added_signal_copies += added_signal_copies;
    metrics.signals.push_back({code, meta, signal_instance_count, added_signal_copies, reported_p_fault,
                               timeout_threshold, wcrt_stats.p_timeout,
                               group_wcrt_ms, group_wcrt_ms, calc_ratio(group_wcrt_ms, meta.period_ms), group_wcrt_ms,
                               calc_ratio(group_wcrt_ms, meta.period_ms)});
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
  metrics.schedulable = true;

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
    const double threshold_wcrt_ms = signal.threshold_wcrt;
    if (reported_p_timeout > signal.p_threshold + 1e-15) {
      metrics.schedulable = false;
    }
    metrics.signals.push_back({code, meta, 1, 0, reported_p_timeout, signal.p_threshold, reported_p_timeout,
                               signal.expected_wcrt, signal.wcrt_p95, calc_ratio(signal.wcrt_p95, meta.period_ms),
                               threshold_wcrt_ms, calc_ratio(threshold_wcrt_ms, meta.period_ms)});
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

std::vector<SignalFrameMappingRow> collect_signal_frame_mappings(const std::string& dataset_tag,
                                                                 const std::string& scheme_name,
                                                                 const PackingScheme& scheme) {
  std::vector<const CanfdFrame*> frames;
  frames.reserve(scheme.frame_map.size());
  for (const auto& [_, frame] : scheme.frame_map) {
    if (!frame.empty()) {
      frames.push_back(&frame);
    }
  }

  std::sort(frames.begin(), frames.end(), [](const CanfdFrame* lhs, const CanfdFrame* rhs) {
    if (lhs->get_id() != rhs->get_id()) {
      return lhs->get_id() < rhs->get_id();
    }
    return lhs->get_priority() < rhs->get_priority();
  });

  std::unordered_map<MessageCode, int> instance_count_by_code;
  for (const auto* frame : frames) {
    for (const auto& msg : frame->msg_set) {
      instance_count_by_code[msg.get_code()] += 1;
    }
  }

  std::unordered_map<MessageCode, int> seen_count_by_code;
  std::vector<SignalFrameMappingRow> rows;
  rows.reserve(scheme.message_set.size());
  for (const auto* frame : frames) {
    const int frame_message_count = static_cast<int>(frame->msg_set.size());
    for (const auto& msg : frame->msg_set) {
      const MessageCode code = msg.get_code();
      const int signal_instance_index = ++seen_count_by_code[code];
      const int signal_instance_count =
          instance_count_by_code.count(code) ? instance_count_by_code.at(code) : signal_instance_index;
      rows.push_back({dataset_tag,
                      scheme_name,
                      code,
                      msg.get_id_message(),
                      signal_instance_index,
                      signal_instance_count,
                      signal_instance_index > 1 ? 1 : 0,
                      frame->get_id(),
                      frame->get_priority(),
                      frame->get_period(),
                      frame->get_deadline(),
                      frame->get_offset(),
                      frame->get_trans_time(),
                      frame->get_paylaod_size(),
                      frame_message_count,
                      msg.get_period(),
                      msg.get_deadline(),
                      msg.get_level(),
                      msg.get_type(),
                      msg.get_ecu_pair().src_ecu,
                      msg.get_ecu_pair().dst_ecu});
    }
  }

  std::sort(rows.begin(), rows.end(), [](const SignalFrameMappingRow& lhs, const SignalFrameMappingRow& rhs) {
    if (lhs.dataset_tag != rhs.dataset_tag) {
      return lhs.dataset_tag < rhs.dataset_tag;
    }
    if (lhs.scheme_name != rhs.scheme_name) {
      return lhs.scheme_name < rhs.scheme_name;
    }
    if (lhs.code != rhs.code) {
      return lhs.code < rhs.code;
    }
    if (lhs.signal_instance_index != rhs.signal_instance_index) {
      return lhs.signal_instance_index < rhs.signal_instance_index;
    }
    return lhs.frame_id < rhs.frame_id;
  });

  return rows;
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
  ofs << "# foundation / baseline1 中，同一 code 的多个副本视为一个信号组；WCRT 直接取该组对应报文中最大的帧响应时间。"
         "baseline2 基于重传分布。\n";
  ofs << "# foundation / baseline1 的 expected_wcrt_ms、wcrt_p95_ms、threshold_wcrt_ms 在当前口径下相同，"
         "都等于对应信号组的最大帧响应时间（单位 ms）。\n";
  ofs << '\n';
  ofs << "retry_distribution_report\t" << retry_report_path << "\n\n";

  ofs << "# 每个方案的总体指标。\n";
  ofs << "[scheme_summary]\n";
  ofs << "scheme\tcompare_bandwidth_utilization\tstatic_bandwidth_utilization\tsignal_count\t"
         "total_added_signal_copies\tschedulable\n";
  for (const auto& scheme : schemes) {
    ofs << scheme.name << '\t' << scheme.compare_bandwidth_utilization << '\t' << scheme.static_bandwidth_utilization
        << '\t' << scheme.signals.size() << '\t' << scheme.total_added_signal_copies << '\t'
        << (scheme.schedulable ? 1 : 0) << '\n';
  }

  ofs << "\n# 按 ASIL 分组统计的 WCRT/周期结果。字段名沿用 wcrt_ratio_p95 以兼容现有脚本；"
         "foundation / baseline1 实际填入的是信号组最大帧响应时间比值。\n";
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
         "added_signal_copies\tp_fault\tp_threshold\texpected_wcrt_ms\tp_wcrt_over_deadline\twcrt_p95_ms\t"
         "threshold_wcrt_ms\n";
  for (const auto& scheme : schemes) {
    for (const auto& signal : scheme.signals) {
      ofs << scheme.name << '\t' << signal.code << '\t' << level_to_asil(signal.meta.level) << '\t' << signal.meta.level
          << '\t' << signal.meta.type << '\t' << signal.meta.period_ms << '\t' << signal.meta.src_ecu << '\t'
          << signal.meta.dst_ecu << '\t' << signal.signal_instance_count << '\t' << signal.added_signal_copies << '\t'
          << signal.p_fault << '\t' << signal.p_threshold << '\t' << signal.expected_wcrt_ms << '\t'
          << signal.p_wcrt_over_deadline << '\t' << signal.wcrt_p95_ms << '\t' << signal.threshold_wcrt_ms << '\n';
    }
  }
}

const SchemeMetrics* find_scheme_metrics(const std::vector<SchemeMetrics>& schemes, std::string_view scheme_name) {
  for (const auto& scheme : schemes) {
    if (scheme.name == scheme_name) {
      return &scheme;
    }
  }
  return nullptr;
}

struct ConfigSchedulabilityAccumulator {
  int dataset_count = 0;
  int unschedulable_count = 0;
};

struct ConfigPeriodWcrtAccumulator {
  int sample_count = 0;
  double ratio_sum = 0.0;
};

void write_batch_compare_summary(const std::string& output_path,
                                 const std::vector<DatasetCompareSummary>& dataset_summaries) {
  std::ofstream ofs(output_path, std::ios::trunc);
  if (!ofs) {
    DEBUG_MSG_DEBUG1(std::cout, "无法写入批量汇总结果文件: ", output_path);
    return;
  }

  ofs << std::setprecision(17);
  ofs << "# 跨多组信号集汇总的对比结果，便于直接复制到表格或画图脚本。\n";
  ofs << "# dataset: 数据集标签，对应当前批量实验配置中的数据集。\n";
  ofs << "# config: 批量配置标签；对 batched dataset 会去掉末尾的 _001 之类批次编号。\n";
  ofs << "# scheme: foundation / baseline1 / baseline2。\n";
  ofs << "# wcrt_ratio_p95 / threshold_wcrt_ms 等字段名为兼容旧脚本保留；foundation / baseline1 的实际含义为"
         "同 code 信号组对应报文中的最大帧响应时间。\n";
  ofs << "# ASIL 分组指标均可直接按 dataset + scheme + asil 进行筛选或透视。\n\n";

  ofs << "[bandwidth_utilization]\n";
  ofs << "dataset\tconfig\tscheme\tcompare_bandwidth_utilization\tstatic_bandwidth_utilization\tsignal_count\t"
         "total_added_signal_copies\tschedulable\n";
  for (const auto& dataset_summary : dataset_summaries) {
    for (const auto& scheme : dataset_summary.schemes) {
      ofs << dataset_summary.dataset_tag << '\t' << dataset_summary.config_tag << '\t' << scheme.name << '\t'
          << scheme.compare_bandwidth_utilization << '\t' << scheme.static_bandwidth_utilization << '\t'
          << scheme.signals.size() << '\t' << scheme.total_added_signal_copies << '\t'
          << (scheme.schedulable ? 1 : 0) << '\n';
    }
  }

  ofs << "\n[wcrt_ratio_p95]\n";
  ofs << "dataset\tconfig\tscheme\tasil\tsignal_count\tavg_wcrt_ratio_p95\tmax_wcrt_ratio_p95\n";
  for (const auto& dataset_summary : dataset_summaries) {
    for (const auto& scheme : dataset_summary.schemes) {
      const auto stats = calc_asil_stats(scheme);
      for (int level = 0; level < NUM_MESSAGE_LEVEL; ++level) {
        const auto& item = stats[level];
        if (item.count <= 0) {
          continue;
        }
        ofs << dataset_summary.dataset_tag << '\t' << dataset_summary.config_tag << '\t' << scheme.name << '\t'
            << level_to_asil(level) << '\t' << item.count << '\t' << (item.wcrt_ratio_p95_sum / item.count) << '\t'
            << item.wcrt_ratio_p95_max << '\n';
      }
    }
  }

  ofs << "\n[fault_probability]\n";
  ofs << "dataset\tconfig\tscheme\tasil\tsignal_count\tavg_fault_probability\tmax_fault_probability\n";
  for (const auto& dataset_summary : dataset_summaries) {
    for (const auto& scheme : dataset_summary.schemes) {
      const auto stats = calc_asil_stats(scheme);
      for (int level = 0; level < NUM_MESSAGE_LEVEL; ++level) {
        const auto& item = stats[level];
        if (item.count <= 0) {
          continue;
        }
        ofs << dataset_summary.dataset_tag << '\t' << dataset_summary.config_tag << '\t' << scheme.name << '\t'
            << level_to_asil(level) << '\t' << item.count << '\t' << (item.fault_sum / item.count) << '\t'
            << item.fault_max << '\n';
      }
    }
  }

  ofs << "\n[added_signal_copies]\n";
  ofs << "dataset\tconfig\tscheme\tasil\tsignal_count\tadded_signal_copies\n";
  for (const auto& dataset_summary : dataset_summaries) {
    for (const auto& scheme : dataset_summary.schemes) {
      const auto stats = calc_asil_stats(scheme);
      for (int level = 0; level < NUM_MESSAGE_LEVEL; ++level) {
        const auto& item = stats[level];
        if (item.count <= 0) {
          continue;
        }
        ofs << dataset_summary.dataset_tag << '\t' << dataset_summary.config_tag << '\t' << scheme.name << '\t'
            << level_to_asil(level) << '\t' << item.count << '\t' << item.added_signal_copies << '\n';
      }
    }
  }

  std::unordered_map<std::string, std::unordered_map<std::string, ConfigSchedulabilityAccumulator>> schedulability_by_config;
  std::unordered_map<std::string, std::unordered_map<std::string, std::unordered_map<int, ConfigPeriodWcrtAccumulator>>>
      wcrt_ratio_by_config;

  for (const auto& dataset_summary : dataset_summaries) {
    for (const auto& scheme : dataset_summary.schemes) {
      auto& item = schedulability_by_config[dataset_summary.config_tag][scheme.name];
      item.dataset_count += 1;
      if (!scheme.schedulable) {
        item.unschedulable_count += 1;
      }
    }

    const SchemeMetrics* baseline2_metrics = find_scheme_metrics(dataset_summary.wcrt_schemes, "baseline2");
    if (baseline2_metrics == nullptr) {
      continue;
    }

    std::unordered_map<int, std::pair<double, int>> baseline2_period_avg;
    for (const auto& signal : baseline2_metrics->signals) {
      auto& entry = baseline2_period_avg[signal.meta.period_ms];
      entry.first += signal.threshold_wcrt_ms;
      entry.second += 1;
    }

    std::unordered_map<int, double> baseline2_period_mean;
    for (const auto& [period_ms, value] : baseline2_period_avg) {
      if (value.second > 0) {
        baseline2_period_mean[period_ms] = value.first / value.second;
      }
    }

    for (const auto scheme_name : kWcrtSchemeNames) {
      const SchemeMetrics* scheme = find_scheme_metrics(dataset_summary.wcrt_schemes, scheme_name);
      if (scheme == nullptr) {
        continue;
      }

      std::unordered_map<int, std::pair<double, int>> period_avg;
      for (const auto& signal : scheme->signals) {
        auto& entry = period_avg[signal.meta.period_ms];
        entry.first += signal.threshold_wcrt_ms;
        entry.second += 1;
      }

      for (const auto& [period_ms, value] : period_avg) {
        if (value.second <= 0 || !baseline2_period_mean.count(period_ms) || baseline2_period_mean.at(period_ms) <= 0.0) {
          continue;
        }
        const double scheme_mean = value.first / value.second;
        const double ratio = scheme_mean / baseline2_period_mean.at(period_ms);
        auto& acc = wcrt_ratio_by_config[dataset_summary.config_tag][scheme->name][period_ms];
        acc.sample_count += 1;
        acc.ratio_sum += ratio;
      }
    }
  }

  ofs << "\n[config_schedulability]\n";
  ofs << "config\tscheme\tdataset_count\tunschedulable_dataset_count\tunschedulable_ratio\n";
  for (const auto& [config_tag, scheme_map] : schedulability_by_config) {
    for (const auto& scheme_name : kFocusedBatchSchemeNames) {
      const auto it = scheme_map.find(scheme_name);
      if (it == scheme_map.end() || it->second.dataset_count <= 0) {
        continue;
      }
      const auto& item = it->second;
      ofs << config_tag << '\t' << scheme_name << '\t' << item.dataset_count << '\t' << item.unschedulable_count
          << '\t' << (static_cast<double>(item.unschedulable_count) / item.dataset_count) << '\n';
    }
  }

  ofs << "\n[config_period_wcrt_ratio]\n";
  ofs << "config\tscheme\tperiod_ms\tdataset_count\tavg_wcrt_ratio_to_baseline2\n";
  for (const auto& [config_tag, scheme_map] : wcrt_ratio_by_config) {
    for (const auto scheme_name : kWcrtSchemeNames) {
      const auto scheme_it = scheme_map.find(scheme_name);
      if (scheme_it == scheme_map.end()) {
        continue;
      }
      for (const auto& [period_ms, item] : scheme_it->second) {
        if (item.sample_count <= 0) {
          continue;
        }
        ofs << config_tag << '\t' << scheme_name << '\t' << period_ms << '\t' << item.sample_count << '\t'
            << (item.ratio_sum / item.sample_count) << '\n';
      }
    }
  }
}

void write_batch_signal_frame_mapping_summary(const std::string& output_path,
                                              const std::vector<DatasetCompareSummary>& dataset_summaries) {
  std::ofstream ofs(output_path, std::ios::trunc);
  if (!ofs) {
    DEBUG_MSG_DEBUG1(std::cout, "无法写入信号装帧映射汇总文件: ", output_path);
    return;
  }

  ofs << std::setprecision(17);
  ofs << "# 跨数据集汇总的信号装帧映射表，用于查看每个信号实例被打包到哪个 CANFD 帧。\n";
  ofs << "# 同一 code 在 foundation / baseline1 中可能出现多次，对应新增副本或同源备份。\n";
  ofs << "# baseline2 不新增副本，因此通常每个 code 只有 1 条映射记录。\n\n";

  ofs << "[signal_frame_mapping]\n";
  ofs << "dataset\tscheme\tcode\tmessage_id\tsignal_instance_index\tsignal_instance_count\tis_added_copy\tframe_id\t"
         "frame_priority\tframe_period_ms\tframe_deadline_ms\tframe_offset_ms\tframe_trans_time_ms\t"
         "frame_payload_bytes\tframe_message_count\tsignal_period_ms\tsignal_deadline_ms\tasil\tlevel\ttype\t"
         "src_ecu\tdst_ecu\n";

  for (const auto& dataset_summary : dataset_summaries) {
    for (const auto& row : dataset_summary.signal_frame_mappings) {
      ofs << row.dataset_tag << '\t' << row.scheme_name << '\t' << row.code << '\t' << row.message_id << '\t'
          << row.signal_instance_index << '\t' << row.signal_instance_count << '\t' << row.is_added_copy << '\t'
          << row.frame_id << '\t' << row.frame_priority << '\t' << row.frame_period_ms << '\t' << row.frame_deadline_ms
          << '\t' << row.frame_offset_ms << '\t' << row.frame_trans_time_ms << '\t' << row.frame_payload_bytes << '\t'
          << row.frame_message_count << '\t' << row.signal_period_ms << '\t' << row.signal_deadline_ms << '\t'
          << level_to_asil(row.level) << '\t' << row.level << '\t' << row.type << '\t' << row.src_ecu << '\t'
          << row.dst_ecu << '\n';
    }
  }
}

DatasetCompareSummary run_compare_experiment(const std::string& dataset_file, const std::string& run_tag) {
  read_data_1(dataset_file);
  const MessageInfoVec original_infos = MESSAGE_INFO_VEC;
  const auto meta_by_code = build_code_meta(original_infos);
  const std::string dataset_name = dataset_tag(dataset_file);
  const std::string config_tag = dataset_config_tag_from_dataset_tag(dataset_name);
  const std::string compare_output_path = build_compare_output_path(run_tag, dataset_name);
  const std::string retry_output_path = build_retry_output_path(run_tag, dataset_name);

  MESSAGE_INFO_VEC = original_infos;
  const PackingScheme shared_initial_scheme = build_shared_initial_scheme_from_current_msgs();

  // 基础方法：仅做信号同源备份。当前版本不再分配 offset，所有方案均保持 offset=0。
  MESSAGE_INFO_VEC = original_infos;
  PackingScheme scheme_foundation_no_offset = shared_initial_scheme;
  scheme_foundation_no_offset = backups::signal::homo_signal_backup(scheme_foundation_no_offset);
  log_scheme_schedulability("foundation_no_offset", scheme_foundation_no_offset);
  SchemeMetrics foundation_no_offset_metrics =
      analyze_deterministic_scheme("foundation_no_offset", scheme_foundation_no_offset, meta_by_code);

  PackingScheme scheme_foundation = scheme_foundation_no_offset;
  log_scheme_schedulability("foundation", scheme_foundation);
  SchemeMetrics foundation_metrics = analyze_deterministic_scheme("foundation", scheme_foundation, meta_by_code);
  auto foundation_signal_frame_mappings = collect_signal_frame_mappings(dataset_name, "foundation", scheme_foundation);

  // baseline1：从同一个共享初始打包结果出发，按帧故障概率直接生成报文副本。
  MESSAGE_INFO_VEC = original_infos;
  PackingScheme scheme_baseline1_no_offset = shared_initial_scheme;
  scheme_baseline1_no_offset = backups::frame::homo_frame_backup(scheme_baseline1_no_offset);
  log_scheme_schedulability("baseline1_no_offset", scheme_baseline1_no_offset);
  SchemeMetrics baseline1_no_offset_metrics =
      analyze_deterministic_scheme("baseline1_no_offset", scheme_baseline1_no_offset, meta_by_code);

  PackingScheme scheme_baseline1 = scheme_baseline1_no_offset;
  log_scheme_schedulability("baseline1", scheme_baseline1);
  SchemeMetrics baseline1_metrics = analyze_deterministic_scheme("baseline1", scheme_baseline1, meta_by_code);
  auto baseline1_signal_frame_mappings = collect_signal_frame_mappings(dataset_name, "baseline1", scheme_baseline1);

  // baseline2：不加副本，直接复用共享初始打包结果做重传分析。
  MESSAGE_INFO_VEC = original_infos;
  PackingScheme scheme_baseline2 = shared_initial_scheme;
  log_scheme_schedulability("baseline2", scheme_baseline2);
  retry::AnalysisReport retry_report = retry::probabilistic_analysis_report(scheme_baseline2, retry_output_path);
  SchemeMetrics baseline2_metrics = analyze_retry_scheme("baseline2", retry_report, meta_by_code);
  auto baseline2_signal_frame_mappings = collect_signal_frame_mappings(dataset_name, "baseline2", scheme_baseline2);
  write_comparison_report(compare_output_path, {foundation_metrics, baseline1_metrics, baseline2_metrics},
                          retry_output_path);

  DEBUG_MSG_DEBUG1(std::cout, "对比结果已输出: ", compare_output_path);

  std::vector<SignalFrameMappingRow> signal_frame_mappings;
  signal_frame_mappings.reserve(foundation_signal_frame_mappings.size() + baseline1_signal_frame_mappings.size() +
                                baseline2_signal_frame_mappings.size());
  signal_frame_mappings.insert(signal_frame_mappings.end(), foundation_signal_frame_mappings.begin(),
                               foundation_signal_frame_mappings.end());
  signal_frame_mappings.insert(signal_frame_mappings.end(), baseline1_signal_frame_mappings.begin(),
                               baseline1_signal_frame_mappings.end());
  signal_frame_mappings.insert(signal_frame_mappings.end(), baseline2_signal_frame_mappings.begin(),
                               baseline2_signal_frame_mappings.end());

  return {dataset_name,
          config_tag,
          {foundation_metrics, baseline1_metrics, baseline2_metrics},
          {foundation_no_offset_metrics, baseline1_no_offset_metrics, foundation_metrics, baseline1_metrics,
           baseline2_metrics},
          signal_frame_mappings};
}

}  // namespace

int main(int argc, char* argv[]) {
#ifdef _WIN32
  // 让 Windows 控制台按 UTF-8 显示
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
#endif

  bool regenerate_datasets = false;
  bool generate_dataset_batches = false;
  bool generate_ed_asild_dataset_batches = false;
  bool run_dataset_batches = false;
  bool run_algorithm2 = false;
  bool generate_figures_after_analysis = true;
  bool algorithm2_route_source_perturbation_enabled = true;
  bool algorithm2_skip_foundation = false;
  size_t dataset_batch_count = 100;
  size_t max_batches_per_spec = 0;
  std::vector<std::string> selected_spec_names;
  std::vector<std::string> explicit_dataset_files;

  for (int argi = 1; argi < argc; ++argi) {
    const std::string arg = argv[argi];
    if (arg == "--regenerate-datasets") {
      regenerate_datasets = true;
    } else if (arg == "--generate-dataset-batches") {
      generate_dataset_batches = true;
      if (argi + 1 < argc) {
        const std::string next_arg = argv[argi + 1];
        if (!next_arg.empty() && next_arg[0] != '-') {
          dataset_batch_count = static_cast<size_t>(std::stoul(next_arg));
          ++argi;
        }
      }
    } else if (arg == "--generate-ed-asild-dataset-batches") {
      generate_ed_asild_dataset_batches = true;
      if (argi + 1 < argc) {
        const std::string next_arg = argv[argi + 1];
        if (!next_arg.empty() && next_arg[0] != '-') {
          dataset_batch_count = static_cast<size_t>(std::stoul(next_arg));
          ++argi;
        }
      }
    } else if (arg == "--run-dataset-batches") {
      run_dataset_batches = true;
    } else if (arg == "--algorithm2") {
      run_algorithm2 = true;
    } else if (arg == "--algorithm2-disable-route-source-perturbation") {
      algorithm2_route_source_perturbation_enabled = false;
    } else if (arg == "--algorithm2-skip-foundation") {
      algorithm2_skip_foundation = true;
    } else if (arg == "--max-batches-per-spec") {
      if (argi + 1 >= argc) {
        throw std::invalid_argument("--max-batches-per-spec requires a positive integer");
      }
      max_batches_per_spec = static_cast<size_t>(std::stoul(argv[++argi]));
    } else if (arg == "--dataset-specs") {
      if (argi + 1 >= argc) {
        throw std::invalid_argument("--dataset-specs requires a comma-separated value");
      }
      selected_spec_names = split_csv_arg(argv[++argi]);
    } else if (arg == "--dataset-files") {
      if (argi + 1 >= argc) {
        throw std::invalid_argument("--dataset-files requires a comma-separated value");
      }
      explicit_dataset_files = split_csv_arg(argv[++argi]);
    } else if (arg == "--skip-figures") {
      generate_figures_after_analysis = false;
    }
  }

  if (generate_ed_asild_dataset_batches) {
    const std::vector<std::string> dataset_paths =
        create_ed_asild_dataset_batches(dataset_batch_count, selected_spec_names);
    DEBUG_MSG_DEBUG1(std::cout, "ED专用ASIL D候选数据集已生成, total=", dataset_paths.size());
    return 0;
  }

  if (generate_dataset_batches) {
    const std::vector<std::string> dataset_paths = create_random_dataset_batches(dataset_batch_count);
    DEBUG_MSG_DEBUG1(std::cout, "批量随机数据集已生成, total=", dataset_paths.size());
    return 0;
  }

  std::vector<std::string> dataset_files;
  if (!explicit_dataset_files.empty()) {
    dataset_files = explicit_dataset_files;
    DEBUG_MSG_DEBUG1(std::cout, "显式实验数据集数量: ", dataset_files.size());
  } else if (run_dataset_batches) {
    dataset_files = resolve_dataset_batch_files(selected_spec_names, max_batches_per_spec);
    DEBUG_MSG_DEBUG1(std::cout, "批量实验数据集数量: ", dataset_files.size());
  } else {
    // 默认模式复用 storage/datasets 下当前配置的数据集；
    // 如需生成一套新数据，改为 true。
    // 如需重新生成默认实验数据，也可以通过命令行传入 --regenerate-datasets。
    dataset_files = resolve_experiment_dataset_files(regenerate_datasets);
  }

  const std::string batch_run_tag = get_time_stamp();
  DEBUG_MSG_DEBUG1(std::cout,
                   "批量测试输出目录: ", cfd::storage::path_string(cfd::storage::analysis_batch_dir(batch_run_tag)));

  if (run_algorithm2) {
    cfd::algorithm2::set_route_source_perturbation_enabled(algorithm2_route_source_perturbation_enabled);
    cfd::algorithm2::set_skip_foundation_enabled(algorithm2_skip_foundation);
    std::vector<cfd::algorithm2::DatasetSummary> dataset_summaries;
    dataset_summaries.reserve(dataset_files.size());
    for (const auto& dataset_file : dataset_files) {
      dataset_summaries.push_back(cfd::algorithm2::run_compare_experiment(dataset_file, batch_run_tag));
    }

    cfd::algorithm2::write_batch_summary(batch_run_tag, dataset_summaries);
    cfd::algorithm2::write_batch_signal_frame_mapping_summary(batch_run_tag, dataset_summaries);
    DEBUG_MSG_DEBUG1(std::cout, "算法二批量汇总结果已输出: ",
                     cfd::storage::path_string(cfd::storage::analysis_batch_dir(batch_run_tag)));

    if (generate_figures_after_analysis) {
      DEBUG_MSG_DEBUG1(std::cout, "算法二暂未接入自动绘图脚本，跳过绘图生成");
    }
    return 0;
  }

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

  const std::string signal_frame_mapping_output_path = cfd::storage::signal_frame_mapping_report_path(batch_run_tag);
  write_batch_signal_frame_mapping_summary(signal_frame_mapping_output_path, dataset_summaries);
  DEBUG_MSG_DEBUG1(std::cout, "信号装帧映射汇总已输出: ", signal_frame_mapping_output_path);

  if (generate_figures_after_analysis) {
    run_figure_generation_script(batch_run_tag);
  }

  return 0;
}
