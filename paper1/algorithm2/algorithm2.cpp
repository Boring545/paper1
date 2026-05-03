#include "algorithm2.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../config.h"
#include "../debug_tool.h"
#include "../priority_allocation.h"
#include "../probabilistic_analysis/no_retry.h"
#include "../scheme.h"
#include "../storage_layout.h"

namespace cfd::algorithm2 {

namespace fs = std::filesystem;

namespace {

struct RouteKey {
  MessageCode code = 0;
  int comm_id = 0;

  bool operator==(const RouteKey& other) const { return code == other.code && comm_id == other.comm_id; }
};

struct RouteKeyHash {
  size_t operator()(const RouteKey& key) const {
    return std::hash<MessageCode>()(key.code) ^ (std::hash<int>()(key.comm_id) << 1);
  }
};

struct RouteMeta {
  int level = 0;
  int period_ms = 0;
  int deadline_ms = 0;
  int type = 0;
  EcuId src_ecu = 0;
  EcuId dst_ecu = 0;
};

struct BackupCandidate {
  MessageID message_index = 0;
  RouteKey key;
  int level = 0;
  int period_ms = 0;
  int round = 0;
};

constexpr int kPrimaryRouteCommId = 0;
constexpr int kSecondMainRouteCommId = 1;
constexpr int kBackupRouteCommId = 2;
constexpr int kMaxRouteBackupIterations = 50;
constexpr size_t kKeepNumerator = 2;
constexpr size_t kKeepDenominator = 3;

RouteKey make_route_key(const MessageInfo& info) { return {info.code, info.comm_id}; }

RouteKey make_route_key(const Message& msg) { return {msg.get_code(), msg.get_comm_id()}; }

bool is_backup_route(int comm_id) { return comm_id == kBackupRouteCommId; }

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

std::string compare_report_path(const std::string& run_tag, const std::string& dataset_tag) {
  const fs::path dir = cfd::storage::analysis_batch_dir(run_tag) / "algorithm2_reports";
  cfd::storage::ensure_directory(dir);
  return cfd::storage::path_string(dir / (dataset_tag + ".txt"));
}

std::string compare_summary_report_path(const std::string& run_tag) {
  return cfd::storage::path_string(cfd::storage::analysis_batch_dir(run_tag) / "algorithm2_summary_tab.txt");
}

std::string signal_frame_mapping_report_path(const std::string& run_tag) {
  return cfd::storage::path_string(cfd::storage::analysis_batch_dir(run_tag) / "algorithm2_signal_frame_mapping_tab.txt");
}

std::vector<EcuId> infer_active_ecus(const MessageInfoVec& infos) {
  std::unordered_set<EcuId> seen;
  for (const auto& info : infos) {
    seen.insert(info.ecu_pair.src_ecu);
    seen.insert(info.ecu_pair.dst_ecu);
  }

  std::vector<EcuId> ecus(seen.begin(), seen.end());
  std::sort(ecus.begin(), ecus.end());
  return ecus;
}

std::unordered_map<EcuId, double> calc_info_based_ecu_load(const MessageInfoVec& infos) {
  std::unordered_map<EcuId, double> load;
  for (const auto& info : infos) {
    if (info.period <= 0) {
      continue;
    }
    load[info.ecu_pair.src_ecu] += static_cast<double>(info.data_size) / static_cast<double>(info.period);
  }
  return load;
}

bool ecu_has_same_period_and_destination(const MessageInfoVec& infos, EcuId src_ecu, int period_ms, EcuId dst_ecu) {
  for (const auto& info : infos) {
    if (info.ecu_pair.src_ecu == src_ecu && info.period == period_ms && info.ecu_pair.dst_ecu == dst_ecu) {
      return true;
    }
  }
  return false;
}

std::optional<EcuId> pick_route_source_ecu(const MessageInfoVec& infos, const std::vector<EcuId>& active_ecus,
                                           const std::unordered_set<EcuId>& excluded, int period_ms, EcuId dst_ecu) {
  std::vector<EcuId> candidates;
  candidates.reserve(active_ecus.size());
  for (const auto ecu : active_ecus) {
    if (ecu == dst_ecu || excluded.count(ecu) != 0) {
      continue;
    }
    candidates.push_back(ecu);
  }
  if (candidates.empty()) {
    return std::nullopt;
  }

  const auto load_by_ecu = calc_info_based_ecu_load(infos);
  auto rank = [&](EcuId ecu) {
    const bool preferred = ecu_has_same_period_and_destination(infos, ecu, period_ms, dst_ecu);
    const double load = load_by_ecu.count(ecu) != 0 ? load_by_ecu.at(ecu) : 0.0;
    return std::tuple<int, double, EcuId>(preferred ? 0 : 1, load, ecu);
  };

  return *std::min_element(candidates.begin(), candidates.end(), [&](EcuId lhs, EcuId rhs) {
    return rank(lhs) < rank(rhs);
  });
}

MessageInfoVec generate_functional_routes(const MessageInfoVec& original_infos) {
  MessageInfoVec expanded_infos = original_infos;
  const auto active_ecus = infer_active_ecus(original_infos);

  std::unordered_map<MessageCode, size_t> primary_route_index_by_code;
  for (size_t index = 0; index < original_infos.size(); ++index) {
    const auto& info = original_infos[index];
    if (info.type == 1 && info.comm_id == kPrimaryRouteCommId &&
        primary_route_index_by_code.count(info.code) == 0) {
      primary_route_index_by_code.emplace(info.code, index);
    }
  }

  for (const auto& [code, info_index] : primary_route_index_by_code) {
    (void)code;
    const auto& primary_info = original_infos[info_index];
    std::unordered_set<EcuId> excluded = {primary_info.ecu_pair.src_ecu};

    const auto second_main_src = pick_route_source_ecu(expanded_infos, active_ecus, excluded, primary_info.period,
                                                       primary_info.ecu_pair.dst_ecu);
    if (!second_main_src.has_value()) {
      continue;
    }

    excluded.insert(*second_main_src);
    expanded_infos.emplace_back(primary_info, static_cast<int>(*second_main_src), 2, kSecondMainRouteCommId);

    const auto backup_src = pick_route_source_ecu(expanded_infos, active_ecus, excluded, primary_info.period,
                                                  primary_info.ecu_pair.dst_ecu);
    if (!backup_src.has_value()) {
      continue;
    }

    expanded_infos.emplace_back(primary_info, static_cast<int>(*backup_src), 2, kBackupRouteCommId);
  }

  return expanded_infos;
}

double calc_required_backup_num(double p_total, double threshold) {
  if (p_total <= 0.0 || p_total <= threshold) return 0;
  if (p_total >= 1.0) return 1;

  const double log_p_total = std::log(p_total);
  const double log_p_threshold = std::log(threshold);
  if (log_p_total >= 0.0) return 1;

  const int total_required = static_cast<int>(std::ceil(log_p_threshold / log_p_total));
  return std::max(0, total_required - 1);
}

std::unordered_map<RouteKey, size_t, RouteKeyHash> build_route_index_map() {
  std::unordered_map<RouteKey, size_t, RouteKeyHash> map;
  map.reserve(MESSAGE_INFO_VEC.size());
  for (size_t index = 0; index < MESSAGE_INFO_VEC.size(); ++index) {
    const RouteKey key = make_route_key(MESSAGE_INFO_VEC[index]);
    if (map.count(key) == 0) {
      map.emplace(key, index);
    }
  }
  return map;
}

std::unordered_map<RouteKey, double, RouteKeyHash> calc_route_fault_probabilities(PackingScheme& scheme, double lambda) {
  std::unordered_map<RouteKey, double, RouteKeyHash> route_fault_probabilities;
  for (const auto& [frame_id, frame] : scheme.frame_map) {
    (void)frame_id;
    if (frame.empty()) {
      continue;
    }

    const double p_fail = analysis::prob_fault_one_more(frame.get_trans_time(), lambda);
    for (const auto& msg : frame.msg_set) {
      const RouteKey key = make_route_key(msg);
      auto it = route_fault_probabilities.find(key);
      if (it == route_fault_probabilities.end()) {
        route_fault_probabilities.emplace(key, p_fail);
      } else {
        const long double product = static_cast<long double>(it->second) * static_cast<long double>(p_fail);
        it->second = product <= 0.0L ? 0.0 : static_cast<double>(product);
      }
    }
  }
  return route_fault_probabilities;
}

std::unordered_map<RouteKey, double, RouteKeyHash> calc_route_max_wcrt(const PackingScheme& scheme) {
  const auto response_times = cfd::schedule::calc_frame_response_times(scheme.frame_map);
  std::unordered_map<RouteKey, double, RouteKeyHash> route_wcrt;
  route_wcrt.reserve(MESSAGE_INFO_VEC.size());

  for (const auto& [frame_id, frame] : scheme.frame_map) {
    if (frame.empty()) {
      continue;
    }
    const double response_time_ms =
        response_times.count(frame_id) != 0 ? response_times.at(frame_id) : frame.get_trans_time();
    for (const auto& msg : frame.msg_set) {
      const RouteKey key = make_route_key(msg);
      auto& current = route_wcrt[key];
      current = std::max(current, response_time_ms);
    }
  }

  return route_wcrt;
}

std::unordered_map<RouteKey, int, RouteKeyHash> calc_route_instance_count(const PackingScheme& scheme) {
  std::unordered_map<RouteKey, int, RouteKeyHash> counts;
  counts.reserve(MESSAGE_INFO_VEC.size());
  for (const auto& [frame_id, frame] : scheme.frame_map) {
    (void)frame_id;
    if (frame.empty()) {
      continue;
    }
    for (const auto& msg : frame.msg_set) {
      counts[make_route_key(msg)] += 1;
    }
  }
  return counts;
}

bool is_scheme_acceptable(const PackingScheme& scheme) {
  if (scheme.calc_bandwidth_utilization() > 1.0) {
    return false;
  }
  return cfd::schedule::feasibility_check(scheme.frame_map);
}

size_t shrink_keep_count(size_t current_keep_count) {
  if (current_keep_count <= 1) {
    return 0;
  }

  size_t next_keep_count = (current_keep_count * kKeepNumerator) / kKeepDenominator;
  if (next_keep_count >= current_keep_count) {
    next_keep_count = current_keep_count - 1;
  }
  return next_keep_count;
}

std::vector<BackupCandidate> build_route_backup_candidates(
    PackingScheme& scheme, double lambda, const std::unordered_map<RouteKey, size_t, RouteKeyHash>& route_index_map) {
  std::vector<BackupCandidate> candidates;
  const auto route_fault_probabilities = calc_route_fault_probabilities(scheme, lambda);

  for (const auto& [key, p_total] : route_fault_probabilities) {
    const auto route_it = route_index_map.find(key);
    if (route_it == route_index_map.end()) {
      continue;
    }

    const auto& info = MESSAGE_INFO_VEC[route_it->second];
    const double threshold = analysis::threshold_per_window(info.level, info.period);
    const int backup_num = calc_required_backup_num(p_total, threshold);
    if (backup_num <= 0) {
      continue;
    }

    for (int round = 1; round <= backup_num; ++round) {
      candidates.push_back({route_it->second, key, info.level, info.period, round});
    }
  }

  std::sort(candidates.begin(), candidates.end(), [](const BackupCandidate& lhs, const BackupCandidate& rhs) {
    if (lhs.level != rhs.level) return lhs.level > rhs.level;
    if (lhs.round != rhs.round) return lhs.round < rhs.round;
    if (lhs.period_ms != rhs.period_ms) return lhs.period_ms > rhs.period_ms;
    if (lhs.key.code != rhs.key.code) return lhs.key.code < rhs.key.code;
    return lhs.key.comm_id < rhs.key.comm_id;
  });

  return candidates;
}

bool try_apply_route_backup_prefix(const PackingScheme& base_scheme, const std::vector<BackupCandidate>& candidates,
                                   size_t keep_count, PackingScheme& out_scheme) {
  if (keep_count == 0 || keep_count > candidates.size()) {
    return false;
  }

  PackingScheme trial = base_scheme;
  for (size_t index = 0; index < keep_count; ++index) {
    trial.message_set.emplace_back(candidates[index].message_index);
  }

  if (!trial.re_init_frames()) {
    return false;
  }

  cfd::packing::frame_pack(trial, cfd::DEFAULT_PACK_METHOD);
  if (!is_scheme_acceptable(trial)) {
    return false;
  }

  out_scheme = std::move(trial);
  return true;
}

PackingScheme homo_route_backup(PackingScheme& scheme, double lambda = LAMBDA_CONFERENCE) {
  PackingScheme working = scheme;
  const auto route_index_map = build_route_index_map();

  for (int iter = 0; iter < kMaxRouteBackupIterations; ++iter) {
    const auto candidates = build_route_backup_candidates(working, lambda, route_index_map);
    if (candidates.empty()) {
      return working;
    }

    PackingScheme feasible_scheme = working;
    bool found_feasible_prefix = false;
    size_t keep_count = candidates.size();
    while (keep_count > 0) {
      if (try_apply_route_backup_prefix(working, candidates, keep_count, feasible_scheme)) {
        found_feasible_prefix = true;
        break;
      }
      keep_count = shrink_keep_count(keep_count);
    }

    if (!found_feasible_prefix) {
      return working;
    }

    const int added_count = static_cast<int>(feasible_scheme.message_set.size() - working.message_set.size());
    if (added_count <= 0) {
      return working;
    }

    working = std::move(feasible_scheme);
  }

  return working;
}

PackingScheme build_algorithm2_scheme_from_current_msgs() {
  PackingScheme scheme{};
  cfd::packing::frame_pack(scheme, cfd::DEFAULT_PACK_METHOD);
  return homo_route_backup(scheme);
}

double calc_mode_bandwidth_utilization(const PackingScheme& scheme, bool include_backup_routes) {
  double utilization = 0.0;
  for (const auto& [frame_id, frame] : scheme.frame_map) {
    (void)frame_id;
    if (frame.empty()) {
      continue;
    }

    bool active = include_backup_routes;
    if (!include_backup_routes) {
      for (const auto& msg : frame.msg_set) {
        if (!is_backup_route(msg.get_comm_id())) {
          active = true;
          break;
        }
      }
    }

    if (active) {
      utilization += frame.get_trans_time() / frame.get_period();
    }
  }
  return utilization;
}

std::vector<RouteKey> sorted_route_keys(const std::unordered_map<RouteKey, RouteMeta, RouteKeyHash>& meta_by_route) {
  std::vector<RouteKey> keys;
  keys.reserve(meta_by_route.size());
  for (const auto& [key, _] : meta_by_route) {
    keys.push_back(key);
  }

  std::sort(keys.begin(), keys.end(), [&](const RouteKey& lhs, const RouteKey& rhs) {
    const auto& left = meta_by_route.at(lhs);
    const auto& right = meta_by_route.at(rhs);
    if (left.level != right.level) return left.level > right.level;
    if (lhs.code != rhs.code) return lhs.code < rhs.code;
    return lhs.comm_id < rhs.comm_id;
  });

  return keys;
}

std::unordered_map<RouteKey, RouteMeta, RouteKeyHash> build_route_meta_map() {
  std::unordered_map<RouteKey, RouteMeta, RouteKeyHash> meta_by_route;
  meta_by_route.reserve(MESSAGE_INFO_VEC.size());
  for (const auto& info : MESSAGE_INFO_VEC) {
    const RouteKey key = make_route_key(info);
    if (meta_by_route.count(key) == 0) {
      meta_by_route.emplace(key, RouteMeta{info.level, info.period, info.deadline, info.type, info.ecu_pair.src_ecu,
                                           info.ecu_pair.dst_ecu});
    }
  }
  return meta_by_route;
}

SchemeMetrics analyze_scheme(const std::string& name, PackingScheme& scheme) {
  SchemeMetrics metrics;
  metrics.name = name;
  metrics.normal_bandwidth_utilization = calc_mode_bandwidth_utilization(scheme, false);
  metrics.fault_bandwidth_utilization = calc_mode_bandwidth_utilization(scheme, true);
  metrics.schedulable = cfd::schedule::feasibility_check(scheme.frame_map);

  const auto meta_by_route = build_route_meta_map();
  const auto route_fault_probabilities = calc_route_fault_probabilities(scheme, LAMBDA_CONFERENCE);
  const auto route_wcrt = calc_route_max_wcrt(scheme);
  const auto route_instance_count = calc_route_instance_count(scheme);
  const auto cluster_fault_probabilities = analysis::noretry::ecu_fault_prob_analysis(scheme, REDUNDANCY_N,
                                                                                       LAMBDA_CONFERENCE);

  for (const auto& key : sorted_route_keys(meta_by_route)) {
    const auto& meta = meta_by_route.at(key);
    const int signal_instance_count = route_instance_count.count(key) != 0 ? route_instance_count.at(key) : 1;
    const int added_signal_copies = std::max(0, signal_instance_count - 1);
    const double p_comm_fault = route_fault_probabilities.count(key) != 0 ? route_fault_probabilities.at(key) : 0.0;
    const double p_threshold = analysis::threshold_per_window(meta.level, meta.period_ms);
    const double max_wcrt_ms = route_wcrt.count(key) != 0 ? route_wcrt.at(key) : 0.0;

    if (p_comm_fault > p_threshold + 1e-15 || max_wcrt_ms > static_cast<double>(meta.deadline_ms)) {
      metrics.schedulable = false;
    }

    metrics.total_added_signal_copies += added_signal_copies;
    metrics.routes.push_back({key.code, key.comm_id, meta.level, meta.type, meta.src_ecu, meta.dst_ecu, meta.period_ms,
                              meta.deadline_ms, signal_instance_count, added_signal_copies, p_comm_fault, p_threshold,
                              max_wcrt_ms});
  }

  std::unordered_map<MessageCode, std::pair<int, double>> cluster_meta;
  for (const auto& route : metrics.routes) {
    if (route.type == 0) {
      continue;
    }
    auto& item = cluster_meta[route.code];
    item.first += 1;
    item.second = std::max(item.second, route.p_threshold);
  }

  std::vector<MessageCode> cluster_codes;
  cluster_codes.reserve(cluster_meta.size());
  for (const auto& [code, _] : cluster_meta) {
    cluster_codes.push_back(code);
  }
  std::sort(cluster_codes.begin(), cluster_codes.end());

  for (const auto code : cluster_codes) {
    const double p_function_fault =
        cluster_fault_probabilities.count(code) != 0 ? cluster_fault_probabilities.at(code) : 0.0;
    const double p_threshold = cluster_meta.at(code).second;
    if (p_function_fault > p_threshold + 1e-15) {
      metrics.schedulable = false;
    }
    metrics.clusters.push_back({code, cluster_meta.at(code).first, p_function_fault, p_threshold});
  }

  return metrics;
}

std::vector<SignalFrameMappingRow> collect_signal_frame_mappings(const std::string& dataset_tag,
                                                                 const std::string& scheme_name,
                                                                 const PackingScheme& scheme) {
  std::vector<const CanfdFrame*> frames;
  frames.reserve(scheme.frame_map.size());
  for (const auto& [frame_id, frame] : scheme.frame_map) {
    (void)frame_id;
    if (!frame.empty()) {
      frames.push_back(&frame);
    }
  }

  std::sort(frames.begin(), frames.end(), [](const CanfdFrame* lhs, const CanfdFrame* rhs) {
    if (lhs->get_priority() != rhs->get_priority()) return lhs->get_priority() < rhs->get_priority();
    return lhs->get_id() < rhs->get_id();
  });

  std::unordered_map<RouteKey, int, RouteKeyHash> instance_count_by_route;
  for (const auto& [frame_id, frame] : scheme.frame_map) {
    (void)frame_id;
    if (frame.empty()) {
      continue;
    }
    for (const auto& msg : frame.msg_set) {
      instance_count_by_route[make_route_key(msg)] += 1;
    }
  }

  std::unordered_map<RouteKey, int, RouteKeyHash> seen_count_by_route;
  std::vector<SignalFrameMappingRow> rows;
  rows.reserve(scheme.message_set.size());

  for (const auto* frame : frames) {
    for (const auto& msg : frame->msg_set) {
      const RouteKey key = make_route_key(msg);
      const int signal_instance_index = ++seen_count_by_route[key];
      const int signal_instance_count =
          instance_count_by_route.count(key) != 0 ? instance_count_by_route.at(key) : signal_instance_index;
      const auto& info = MESSAGE_INFO_VEC[msg.get_id_message()];
      rows.push_back({dataset_tag,
                      scheme_name,
                      msg.get_code(),
                      msg.get_comm_id(),
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
                      static_cast<int>(frame->msg_set.size()),
                      msg.get_period(),
                      msg.get_deadline(),
                      msg.get_level(),
                      msg.get_type(),
                      msg.get_ecu_pair().src_ecu,
                      msg.get_ecu_pair().dst_ecu});
    }
  }

  std::sort(rows.begin(), rows.end(), [](const SignalFrameMappingRow& lhs, const SignalFrameMappingRow& rhs) {
    if (lhs.code != rhs.code) return lhs.code < rhs.code;
    if (lhs.comm_id != rhs.comm_id) return lhs.comm_id < rhs.comm_id;
    if (lhs.signal_instance_index != rhs.signal_instance_index) return lhs.signal_instance_index < rhs.signal_instance_index;
    return lhs.frame_priority < rhs.frame_priority;
  });

  return rows;
}

void write_dataset_report(const std::string& output_path, const DatasetSummary& summary) {
  std::ofstream ofs(output_path, std::ios::trunc);
  if (!ofs) {
    DEBUG_MSG_DEBUG1(std::cout, "无法写入算法二结果文件: ", output_path);
    return;
  }

  ofs << "# algorithm2: 按需三模冗余 + 功能路内同源通信副本。\n";
  ofs << "# 正常模式带宽默认只统计 comm_id=0/1 的功能路；故障模式带宽统计全量功能路。\n\n";

  ofs << "[scheme_summary]\n";
  ofs << "scheme\tnormal_bandwidth_utilization\tfault_bandwidth_utilization\ttotal_added_signal_copies\tschedulable\n";
  ofs << summary.scheme.name << '\t' << summary.scheme.normal_bandwidth_utilization << '\t'
      << summary.scheme.fault_bandwidth_utilization << '\t' << summary.scheme.total_added_signal_copies << '\t'
      << (summary.scheme.schedulable ? 1 : 0) << "\n\n";

  ofs << "[cluster_summary]\n";
  ofs << "code\troute_count\tp_function_fault\tp_threshold\n";
  for (const auto& cluster : summary.scheme.clusters) {
    ofs << cluster.code << '\t' << cluster.route_count << '\t' << cluster.p_function_fault << '\t'
        << cluster.p_threshold << '\n';
  }
  ofs << '\n';

  ofs << "[route_summary]\n";
  ofs << "code\tcomm_id\tlevel\ttype\tsrc_ecu\tdst_ecu\tperiod_ms\tdeadline_ms\tsignal_instance_count\t"
         "added_signal_copies\tp_comm_fault\tp_threshold\tmax_wcrt_ms\n";
  for (const auto& route : summary.scheme.routes) {
    ofs << route.code << '\t' << route.comm_id << '\t' << route.level << '\t' << route.type << '\t' << route.src_ecu
        << '\t' << route.dst_ecu << '\t' << route.period_ms << '\t' << route.deadline_ms << '\t'
        << route.signal_instance_count << '\t' << route.added_signal_copies << '\t' << route.p_comm_fault << '\t'
        << route.p_threshold << '\t' << route.max_wcrt_ms << '\n';
  }
}

}  // namespace

DatasetSummary run_compare_experiment(const std::string& dataset_file, const std::string& run_tag) {
  const std::string full_path = cfd::storage::resolve_dataset_input_path(dataset_file);
  cfd::utils::read_message(full_path);
  const MessageInfoVec original_infos = MESSAGE_INFO_VEC;

  MESSAGE_INFO_VEC = generate_functional_routes(original_infos);
  PackingScheme scheme = build_algorithm2_scheme_from_current_msgs();
  DatasetSummary summary;
  summary.dataset_tag = cfd::storage::dataset_tag_from_file(dataset_file);
  summary.config_tag = dataset_config_tag_from_dataset_tag(summary.dataset_tag);
  summary.scheme = analyze_scheme("algorithm2", scheme);
  summary.signal_frame_mappings = collect_signal_frame_mappings(summary.dataset_tag, summary.scheme.name, scheme);

  write_dataset_report(compare_report_path(run_tag, summary.dataset_tag), summary);
  return summary;
}

void write_batch_summary(const std::string& run_tag, const std::vector<DatasetSummary>& dataset_summaries) {
  const std::string output_path = compare_summary_report_path(run_tag);
  std::ofstream ofs(output_path, std::ios::trunc);
  if (!ofs) {
    DEBUG_MSG_DEBUG1(std::cout, "无法写入算法二批量汇总文件: ", output_path);
    return;
  }

  ofs << "[scheme_summary]\n";
  ofs << "dataset\tconfig\tscheme\tnormal_bandwidth_utilization\tfault_bandwidth_utilization\t"
         "total_added_signal_copies\tschedulable\n";
  for (const auto& summary : dataset_summaries) {
    ofs << summary.dataset_tag << '\t' << summary.config_tag << '\t' << summary.scheme.name << '\t'
        << summary.scheme.normal_bandwidth_utilization << '\t' << summary.scheme.fault_bandwidth_utilization << '\t'
        << summary.scheme.total_added_signal_copies << '\t' << (summary.scheme.schedulable ? 1 : 0) << '\n';
  }
  ofs << '\n';

  ofs << "[cluster_summary]\n";
  ofs << "dataset\tconfig\tcode\troute_count\tp_function_fault\tp_threshold\n";
  for (const auto& summary : dataset_summaries) {
    for (const auto& cluster : summary.scheme.clusters) {
      ofs << summary.dataset_tag << '\t' << summary.config_tag << '\t' << cluster.code << '\t' << cluster.route_count
          << '\t' << cluster.p_function_fault << '\t' << cluster.p_threshold << '\n';
    }
  }
  ofs << '\n';

  ofs << "[route_summary]\n";
  ofs << "dataset\tconfig\tcode\tcomm_id\tlevel\ttype\tsrc_ecu\tdst_ecu\tperiod_ms\tdeadline_ms\t"
         "signal_instance_count\tadded_signal_copies\tp_comm_fault\tp_threshold\tmax_wcrt_ms\n";
  for (const auto& summary : dataset_summaries) {
    for (const auto& route : summary.scheme.routes) {
      ofs << summary.dataset_tag << '\t' << summary.config_tag << '\t' << route.code << '\t' << route.comm_id << '\t'
          << route.level << '\t' << route.type << '\t' << route.src_ecu << '\t' << route.dst_ecu << '\t'
          << route.period_ms << '\t' << route.deadline_ms << '\t' << route.signal_instance_count << '\t'
          << route.added_signal_copies << '\t' << route.p_comm_fault << '\t' << route.p_threshold << '\t'
          << route.max_wcrt_ms << '\n';
    }
  }
}

void write_batch_signal_frame_mapping_summary(const std::string& run_tag,
                                              const std::vector<DatasetSummary>& dataset_summaries) {
  const std::string output_path = signal_frame_mapping_report_path(run_tag);
  std::ofstream ofs(output_path, std::ios::trunc);
  if (!ofs) {
    DEBUG_MSG_DEBUG1(std::cout, "无法写入算法二装帧映射汇总文件: ", output_path);
    return;
  }

  ofs << "[signal_frame_mapping]\n";
  ofs << "dataset\tscheme\tcode\tcomm_id\tmessage_id\tsignal_instance_index\tsignal_instance_count\tis_added_copy\t"
         "frame_id\tframe_priority\tframe_period_ms\tframe_deadline_ms\tframe_offset_ms\tframe_trans_time_ms\t"
         "frame_payload_bytes\tframe_message_count\tsignal_period_ms\tsignal_deadline_ms\tlevel\ttype\tsrc_ecu\t"
         "dst_ecu\n";

  for (const auto& summary : dataset_summaries) {
    for (const auto& row : summary.signal_frame_mappings) {
      ofs << row.dataset_tag << '\t' << row.scheme_name << '\t' << row.code << '\t' << row.comm_id << '\t'
          << row.message_id << '\t' << row.signal_instance_index << '\t' << row.signal_instance_count << '\t'
          << row.is_added_copy << '\t' << row.frame_id << '\t' << row.frame_priority << '\t' << row.frame_period_ms
          << '\t' << row.frame_deadline_ms << '\t' << row.frame_offset_ms << '\t' << row.frame_trans_time_ms << '\t'
          << row.frame_payload_bytes << '\t' << row.frame_message_count << '\t' << row.signal_period_ms << '\t'
          << row.signal_deadline_ms << '\t' << row.level << '\t' << row.type << '\t' << row.src_ecu << '\t'
          << row.dst_ecu << '\n';
    }
  }
}

}  // namespace cfd::algorithm2
