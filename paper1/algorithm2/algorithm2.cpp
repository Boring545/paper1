#include "algorithm2.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <optional>
#include <sstream>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../config.h"
#include "../backups/signal_backup.h"
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

struct EventClusterMeta {
  MessageCode code = 0;
  int period_ms = 0;
  int deadline_ms = 0;
  EcuId primary_src_ecu = 0;
  EcuId second_main_src_ecu = 0;
  EcuId backup_src_ecu = 0;
  EcuId dst_ecu = 0;
};

struct SchemeAnalysisConfig {
  std::string name;
  bool include_backup_routes_in_normal_mode = false;
  bool include_backup_routes_in_fault_mode = true;
  bool include_event_frames_in_fault_mode = false;
  bool enable_route_source_perturbation = false;
  enum class ClusterWcrtMode { None, OnDemand, AlwaysOnTmr };
  ClusterWcrtMode cluster_wcrt_mode = ClusterWcrtMode::None;
};

struct OptimizedSchemeResult {
  MessageInfoVec infos;
  PackingScheme scheme;
};

struct BuiltSchemeResult {
  MessageInfoVec infos;
  PackingScheme scheme;
  SchemeMetrics metrics;
  double periodic_bandwidth_utilization = 0.0;
};

constexpr int kPrimaryRouteCommId = 0;
constexpr int kSecondMainRouteCommId = 1;
constexpr int kBackupRouteCommId = 2;
constexpr int kActEventCommId = 1001;
constexpr int kIsoEventCommId = 1002;
constexpr int kHangEventCommId = 1003;
constexpr int kEventMessageType = -1;
constexpr int kEventPayloadBytes = 1;
constexpr int kActEventVirtualPriorityPeriodMs = 2;
constexpr int kMaxRouteBackupIterations = 50;
constexpr size_t kKeepNumerator = 2;
constexpr size_t kKeepDenominator = 3;
constexpr double kCompareTimeMs = 0.01;
constexpr double kBackupComputeTimeMs = 0.0;
constexpr double kOuterSaInitialTemperature = 1.0;
constexpr double kOuterSaFinalTemperature = 0.2;
constexpr double kOuterSaAlpha = 0.5;
constexpr double kOuterSaCostScale = 50.0;
constexpr double kFaultBandwidthWeight = 0.001;

SchemeMetrics analyze_scheme(const SchemeAnalysisConfig& config, PackingScheme& scheme,
                             const CanfdFrameMap& fault_frame_map);

RouteKey make_route_key(const MessageInfo& info) { return {info.code, info.comm_id}; }

RouteKey make_route_key(const Message& msg) { return {msg.get_code(), msg.get_comm_id()}; }

bool is_backup_route(int comm_id) { return comm_id == kBackupRouteCommId; }

bool is_event_info(const MessageInfo& info) { return info.type == kEventMessageType; }

bool is_event_message(const Message& msg) { return is_event_info(MESSAGE_INFO_VEC[msg.get_id_message()]); }

bool is_act_event_message(const Message& msg) { return is_event_message(msg) && msg.get_comm_id() == kActEventCommId; }

int calc_priority_period_for_frame(const CanfdFrame& frame) {
  for (const auto& msg : frame.msg_set) {
    if (is_act_event_message(msg)) {
      return kActEventVirtualPriorityPeriodMs;
    }
  }
  return frame.get_period();
}

bool assign_algorithm2_priority(CanfdFrameMap& frame_map) {
  std::vector<CanfdFrame*> frames;
  frames.reserve(frame_map.size());

  for (auto& [frame_id, frame] : frame_map) {
    (void)frame_id;
    if (frame.empty()) {
      continue;
    }
    frames.push_back(&frame);
  }

  std::sort(frames.begin(), frames.end(), [](const CanfdFrame* lhs, const CanfdFrame* rhs) {
    const int lhs_period = calc_priority_period_for_frame(*lhs);
    const int rhs_period = calc_priority_period_for_frame(*rhs);
    if (lhs_period != rhs_period) return lhs_period < rhs_period;
    if (lhs->get_deadline() != rhs->get_deadline()) return lhs->get_deadline() < rhs->get_deadline();
    return lhs->get_id() < rhs->get_id();
  });

  for (int pri = 0; pri < static_cast<int>(frames.size()); ++pri) {
    frames[pri]->set_priority(pri);
  }

  return true;
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

std::vector<EventClusterMeta> build_event_cluster_meta() {
  std::unordered_map<MessageCode, EventClusterMeta> cluster_by_code;
  for (const auto& info : MESSAGE_INFO_VEC) {
    if (is_event_info(info) || (info.type != 1 && info.type != 2)) {
      continue;
    }

    auto& cluster = cluster_by_code[info.code];
    cluster.code = info.code;
    cluster.period_ms = info.period;
    cluster.deadline_ms = info.deadline;
    cluster.dst_ecu = info.ecu_pair.dst_ecu;

    if (info.comm_id == kPrimaryRouteCommId) {
      cluster.primary_src_ecu = info.ecu_pair.src_ecu;
    } else if (info.comm_id == kSecondMainRouteCommId) {
      cluster.second_main_src_ecu = info.ecu_pair.src_ecu;
    } else if (info.comm_id == kBackupRouteCommId) {
      cluster.backup_src_ecu = info.ecu_pair.src_ecu;
    }
  }

  std::vector<EventClusterMeta> clusters;
  clusters.reserve(cluster_by_code.size());
  for (const auto& [code, cluster] : cluster_by_code) {
    (void)code;
    if (cluster.period_ms > 0 && cluster.dst_ecu != cluster.backup_src_ecu) {
      clusters.push_back(cluster);
    }
  }
  std::sort(clusters.begin(), clusters.end(), [](const EventClusterMeta& lhs, const EventClusterMeta& rhs) {
    return lhs.code < rhs.code;
  });
  return clusters;
}

void append_event_frame(CanfdFrameMap& frame_map, const EventClusterMeta& cluster, int comm_id, const EcuPair& ecu_pair) {
  const size_t message_index = MESSAGE_INFO_VEC.size();
  MESSAGE_INFO_VEC.emplace_back(cluster.code, kEventPayloadBytes * 8, cluster.period_ms, cluster.deadline_ms,
                                static_cast<int>(ecu_pair.src_ecu), static_cast<int>(ecu_pair.dst_ecu), 0, 0,
                                kEventMessageType, comm_id);

  Message event_message(message_index);
  FrameId next_frame_id = 0;
  for (const auto& [frame_id, _] : frame_map) {
    next_frame_id = std::max(next_frame_id, frame_id + 1);
  }

  frame_map.emplace(next_frame_id, CanfdFrame(next_frame_id, event_message));
}

CanfdFrameMap build_fault_state_frame_map(const PackingScheme& scheme) {
  CanfdFrameMap frame_map = scheme.frame_map;
  for (const auto& cluster : build_event_cluster_meta()) {
    append_event_frame(frame_map, cluster, kActEventCommId, EcuPair(cluster.dst_ecu, cluster.backup_src_ecu));
    append_event_frame(frame_map, cluster, kIsoEventCommId, EcuPair(cluster.dst_ecu, cluster.primary_src_ecu));
    append_event_frame(frame_map, cluster, kHangEventCommId, EcuPair(cluster.dst_ecu, cluster.backup_src_ecu));
  }

  assign_algorithm2_priority(frame_map);
  return frame_map;
}

CanfdFrameMap build_full_tmr_frame_map(const PackingScheme& scheme) {
  CanfdFrameMap frame_map = scheme.frame_map;
  assign_algorithm2_priority(frame_map);
  return frame_map;
}

CanfdFrameMap build_analysis_frame_map(const PackingScheme& scheme, const SchemeAnalysisConfig& config) {
  if (config.include_event_frames_in_fault_mode) {
    return build_fault_state_frame_map(scheme);
  }
  return build_full_tmr_frame_map(scheme);
}

CanfdFrameMap filter_active_frame_map(const CanfdFrameMap& frame_map, bool include_backup_routes, bool include_event_frames) {
  CanfdFrameMap filtered;
  for (const auto& [frame_id, frame] : frame_map) {
    if (frame.empty()) {
      continue;
    }

    bool active = false;
    for (const auto& msg : frame.msg_set) {
      if (is_event_message(msg)) {
        if (include_event_frames) {
          active = true;
          break;
        }
        continue;
      }

      if (include_backup_routes || !is_backup_route(msg.get_comm_id())) {
        active = true;
        break;
      }
    }

  if (active) {
      filtered.emplace(frame_id, frame);
    }
  }

  assign_algorithm2_priority(filtered);
  return filtered;
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
      if (is_event_message(msg)) {
        continue;
      }
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

std::unordered_map<RouteKey, double, RouteKeyHash> calc_route_max_wcrt(const CanfdFrameMap& frame_map) {
  const auto response_times = cfd::schedule::calc_frame_response_times(frame_map);
  std::unordered_map<RouteKey, double, RouteKeyHash> route_wcrt;
  route_wcrt.reserve(MESSAGE_INFO_VEC.size());

  for (const auto& [frame_id, frame] : frame_map) {
    if (frame.empty()) {
      continue;
    }
    const double response_time_ms =
        response_times.count(frame_id) != 0 ? response_times.at(frame_id) : frame.get_trans_time();
    for (const auto& msg : frame.msg_set) {
      if (is_event_message(msg)) {
        continue;
      }
      const RouteKey key = make_route_key(msg);
      auto& current = route_wcrt[key];
      current = std::max(current, response_time_ms);
    }
  }

  return route_wcrt;
}

std::unordered_map<RouteKey, int, RouteKeyHash> calc_route_instance_count(const CanfdFrameMap& frame_map) {
  std::unordered_map<RouteKey, int, RouteKeyHash> counts;
  counts.reserve(MESSAGE_INFO_VEC.size());
  for (const auto& [frame_id, frame] : frame_map) {
    (void)frame_id;
    if (frame.empty()) {
      continue;
    }
    for (const auto& msg : frame.msg_set) {
      if (is_event_message(msg)) {
        continue;
      }
      counts[make_route_key(msg)] += 1;
    }
  }
  return counts;
}

std::vector<EventClusterMeta> build_protected_cluster_meta() {
  std::unordered_map<MessageCode, EventClusterMeta> cluster_by_code;
  for (const auto& info : MESSAGE_INFO_VEC) {
    if (is_event_info(info) || info.type != 1 || info.comm_id != kPrimaryRouteCommId) {
      continue;
    }

    auto& cluster = cluster_by_code[info.code];
    cluster.code = info.code;
    cluster.period_ms = info.period;
    cluster.deadline_ms = info.deadline;
    cluster.primary_src_ecu = info.ecu_pair.src_ecu;
    cluster.dst_ecu = info.ecu_pair.dst_ecu;
  }

  for (const auto& info : MESSAGE_INFO_VEC) {
    if (is_event_info(info) || cluster_by_code.count(info.code) == 0) {
      continue;
    }

    auto& cluster = cluster_by_code[info.code];
    if (info.comm_id == kSecondMainRouteCommId) {
      cluster.second_main_src_ecu = info.ecu_pair.src_ecu;
    } else if (info.comm_id == kBackupRouteCommId) {
      cluster.backup_src_ecu = info.ecu_pair.src_ecu;
    }
  }

  std::vector<EventClusterMeta> clusters;
  clusters.reserve(cluster_by_code.size());
  for (const auto& [code, cluster] : cluster_by_code) {
    (void)code;
    clusters.push_back(cluster);
  }
  std::sort(clusters.begin(), clusters.end(), [](const EventClusterMeta& lhs, const EventClusterMeta& rhs) {
    return lhs.code < rhs.code;
  });
  return clusters;
}

std::unordered_map<MessageCode, double> calc_activation_wcrt(const CanfdFrameMap& fault_frame_map) {
  const auto response_times = cfd::schedule::calc_frame_response_times(fault_frame_map);
  std::unordered_map<MessageCode, double> activation_wcrt_by_code;
  activation_wcrt_by_code.reserve(MESSAGE_INFO_VEC.size());

  for (const auto& [frame_id, frame] : fault_frame_map) {
    if (frame.empty()) {
      continue;
    }

    const double response_time_ms =
        response_times.count(frame_id) != 0 ? response_times.at(frame_id) : frame.get_trans_time();
    for (const auto& msg : frame.msg_set) {
      if (!is_event_message(msg) || msg.get_comm_id() != kActEventCommId) {
        continue;
      }
      auto& current = activation_wcrt_by_code[msg.get_code()];
      current = std::max(current, response_time_ms);
    }
  }

  return activation_wcrt_by_code;
}

std::vector<ClusterWcrtMetric> calc_cluster_wcrts_on_demand(
    const std::unordered_map<RouteKey, double, RouteKeyHash>& normal_route_wcrt,
    const std::unordered_map<RouteKey, double, RouteKeyHash>& fault_route_wcrt, const CanfdFrameMap& fault_frame_map) {
  const auto activation_wcrt_by_code = calc_activation_wcrt(fault_frame_map);
  const auto protected_clusters = build_protected_cluster_meta();

  std::vector<ClusterWcrtMetric> metrics;
  metrics.reserve(protected_clusters.size());

  for (const auto& cluster : protected_clusters) {
    const RouteKey primary_key{cluster.code, kPrimaryRouteCommId};
    const RouteKey second_main_key{cluster.code, kSecondMainRouteCommId};
    const RouteKey backup_key{cluster.code, kBackupRouteCommId};

    const auto normal_primary_it = normal_route_wcrt.find(primary_key);
    const auto normal_second_main_it = normal_route_wcrt.find(second_main_key);
    const auto fault_primary_it = fault_route_wcrt.find(primary_key);
    const auto fault_second_main_it = fault_route_wcrt.find(second_main_key);
    const auto backup_it = fault_route_wcrt.find(backup_key);
    const auto activation_it = activation_wcrt_by_code.find(cluster.code);

    ClusterWcrtMetric metric;
    metric.code = cluster.code;
    metric.period_ms = cluster.period_ms;
    metric.deadline_ms = cluster.deadline_ms;
    metric.compare_time_ms = kCompareTimeMs;
    metric.backup_compute_time_ms = kBackupComputeTimeMs;

    const bool has_normal_primary = normal_primary_it != normal_route_wcrt.end();
    const bool has_normal_second_main = normal_second_main_it != normal_route_wcrt.end();
    const bool has_fault_primary = fault_primary_it != fault_route_wcrt.end();
    const bool has_fault_second_main = fault_second_main_it != fault_route_wcrt.end();
    const bool has_backup = backup_it != fault_route_wcrt.end();
    const bool has_activation = activation_it != activation_wcrt_by_code.end();

    if (has_normal_primary) {
      metric.normal_main_route_wcrt_ms = normal_primary_it->second;
    }
    if (has_normal_second_main) {
      metric.normal_main_route_wcrt_ms = std::max(metric.normal_main_route_wcrt_ms, normal_second_main_it->second);
    }
    if (has_fault_primary) {
      metric.fault_main_route_wcrt_ms = fault_primary_it->second;
    }
    if (has_fault_second_main) {
      metric.fault_main_route_wcrt_ms = std::max(metric.fault_main_route_wcrt_ms, fault_second_main_it->second);
    }
    if (has_activation) {
      metric.activation_wcrt_ms = activation_it->second;
    }
    if (has_backup) {
      metric.backup_route_wcrt_ms = backup_it->second;
    }

    if (has_normal_primary && has_normal_second_main) {
      metric.end_to_end_normal_wcrt_ms = metric.normal_main_route_wcrt_ms + metric.compare_time_ms;
    }

    if (has_fault_primary && has_fault_second_main && has_backup && has_activation) {
      metric.end_to_end_fault_wcrt_ms = metric.fault_main_route_wcrt_ms + 2.0 * metric.compare_time_ms +
                                        metric.activation_wcrt_ms + metric.backup_compute_time_ms +
                                        metric.backup_route_wcrt_ms;
      metric.meets_deadline = metric.end_to_end_normal_wcrt_ms <= static_cast<double>(metric.deadline_ms) &&
                              metric.end_to_end_fault_wcrt_ms <= static_cast<double>(metric.deadline_ms);
    } else {
      metric.meets_deadline = false;
    }

    metrics.push_back(metric);
  }

  return metrics;
}

std::vector<ClusterWcrtMetric> calc_cluster_wcrts_always_on_tmr(
    const std::unordered_map<RouteKey, double, RouteKeyHash>& route_wcrt) {
  const auto protected_clusters = build_protected_cluster_meta();

  std::vector<ClusterWcrtMetric> metrics;
  metrics.reserve(protected_clusters.size());

  for (const auto& cluster : protected_clusters) {
    const RouteKey primary_key{cluster.code, kPrimaryRouteCommId};
    const RouteKey second_main_key{cluster.code, kSecondMainRouteCommId};
    const RouteKey backup_key{cluster.code, kBackupRouteCommId};

    const auto primary_it = route_wcrt.find(primary_key);
    const auto second_main_it = route_wcrt.find(second_main_key);
    const auto backup_it = route_wcrt.find(backup_key);

    ClusterWcrtMetric metric;
    metric.code = cluster.code;
    metric.period_ms = cluster.period_ms;
    metric.deadline_ms = cluster.deadline_ms;
    metric.compare_time_ms = kCompareTimeMs;

    const bool has_primary = primary_it != route_wcrt.end();
    const bool has_second_main = second_main_it != route_wcrt.end();
    const bool has_backup = backup_it != route_wcrt.end();

    if (has_primary) {
      metric.tmr_all_route_wcrt_ms = primary_it->second;
    }
    if (has_second_main) {
      metric.tmr_all_route_wcrt_ms = std::max(metric.tmr_all_route_wcrt_ms, second_main_it->second);
    }
    if (has_backup) {
      metric.tmr_all_route_wcrt_ms = std::max(metric.tmr_all_route_wcrt_ms, backup_it->second);
    }

    if (has_primary && has_second_main && has_backup) {
      metric.end_to_end_normal_wcrt_ms = metric.tmr_all_route_wcrt_ms + metric.compare_time_ms;
      metric.end_to_end_fault_wcrt_ms = metric.end_to_end_normal_wcrt_ms;
      metric.meets_deadline = metric.end_to_end_normal_wcrt_ms <= static_cast<double>(metric.deadline_ms);
    } else {
      metric.meets_deadline = false;
    }

    metrics.push_back(metric);
  }

  return metrics;
}

bool is_scheme_acceptable(const PackingScheme& scheme, const SchemeAnalysisConfig& config) {
  const size_t message_count_before_events = MESSAGE_INFO_VEC.size();
  PackingScheme trial = scheme;
  const CanfdFrameMap fault_frame_map = build_analysis_frame_map(trial, config);
  SchemeMetrics metrics = analyze_scheme(config, trial, fault_frame_map);
  MESSAGE_INFO_VEC.resize(message_count_before_events);
  return metrics.schedulable;
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
                                   size_t keep_count, PackingScheme& out_scheme, const SchemeAnalysisConfig& config) {
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
  if (!is_scheme_acceptable(trial, config)) {
    return false;
  }

  out_scheme = std::move(trial);
  return true;
}

PackingScheme homo_route_backup(PackingScheme& scheme, const SchemeAnalysisConfig& config,
                                double lambda = LAMBDA_CONFERENCE) {
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
      if (try_apply_route_backup_prefix(working, candidates, keep_count, feasible_scheme, config)) {
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

PackingScheme build_algorithm2_scheme_from_current_msgs(const SchemeAnalysisConfig& config) {
  PackingScheme scheme{};
  cfd::packing::frame_pack(scheme, cfd::DEFAULT_PACK_METHOD);
  return homo_route_backup(scheme, config);
}

double calc_mode_bandwidth_utilization(const CanfdFrameMap& frame_map, bool include_backup_routes,
                                       bool include_event_frames) {
  double utilization = 0.0;
  for (const auto& [frame_id, frame] : frame_map) {
    (void)frame_id;
    if (frame.empty()) {
      continue;
    }

    bool active = false;
    for (const auto& msg : frame.msg_set) {
      if (is_event_message(msg)) {
        if (include_event_frames) {
          active = true;
          break;
        }
        continue;
      }

      if (include_backup_routes || !is_backup_route(msg.get_comm_id())) {
        active = true;
        break;
      }
    }

    if (active) {
      utilization += frame.get_trans_time() / frame.get_period();
    }
  }
  return utilization;
}

std::vector<RouteKey> build_perturbable_route_keys(const MessageInfoVec& infos) {
  std::unordered_set<MessageCode> protected_codes;
  for (const auto& info : infos) {
    if (info.type == 1 && info.comm_id == kPrimaryRouteCommId) {
      protected_codes.insert(info.code);
    }
  }

  std::vector<RouteKey> keys;
  std::unordered_set<RouteKey, RouteKeyHash> seen;
  for (const auto& info : infos) {
    if (protected_codes.count(info.code) == 0) {
      continue;
    }
    if (info.comm_id != kSecondMainRouteCommId && info.comm_id != kBackupRouteCommId) {
      continue;
    }
    const RouteKey key = make_route_key(info);
    if (seen.insert(key).second) {
      keys.push_back(key);
    }
  }
  std::sort(keys.begin(), keys.end(), [](const RouteKey& lhs, const RouteKey& rhs) {
    if (lhs.code != rhs.code) return lhs.code < rhs.code;
    return lhs.comm_id < rhs.comm_id;
  });
  return keys;
}

bool perturb_route_source(MessageInfoVec& infos, const std::vector<EcuId>& active_ecus, std::mt19937& gen) {
  const auto keys = build_perturbable_route_keys(infos);
  if (keys.empty()) {
    return false;
  }

  std::uniform_int_distribution<size_t> route_dist(0, keys.size() - 1);
  std::vector<size_t> route_order(keys.size());
  std::iota(route_order.begin(), route_order.end(), 0);
  std::shuffle(route_order.begin(), route_order.end(), gen);

  for (const size_t route_index : route_order) {
    const RouteKey selected_key = keys[route_index];

    std::optional<EcuId> primary_src;
    std::optional<EcuId> sibling_src;
    std::optional<EcuId> current_src;
    std::optional<EcuId> dst_ecu;

    for (const auto& info : infos) {
      if (info.code != selected_key.code) {
        continue;
      }
      if (info.comm_id == kPrimaryRouteCommId) {
        primary_src = info.ecu_pair.src_ecu;
        dst_ecu = info.ecu_pair.dst_ecu;
      } else if (info.comm_id == selected_key.comm_id) {
        current_src = info.ecu_pair.src_ecu;
        dst_ecu = info.ecu_pair.dst_ecu;
      } else if ((selected_key.comm_id == kSecondMainRouteCommId && info.comm_id == kBackupRouteCommId) ||
                 (selected_key.comm_id == kBackupRouteCommId && info.comm_id == kSecondMainRouteCommId)) {
        sibling_src = info.ecu_pair.src_ecu;
      }
    }

    if (!primary_src.has_value() || !current_src.has_value() || !dst_ecu.has_value()) {
      continue;
    }

    std::vector<EcuId> candidates;
    for (const auto ecu : active_ecus) {
      if (ecu == *dst_ecu || ecu == *primary_src || ecu == *current_src) {
        continue;
      }
      if (sibling_src.has_value() && ecu == *sibling_src) {
        continue;
      }
      candidates.push_back(ecu);
    }

    if (candidates.empty()) {
      continue;
    }

    std::uniform_int_distribution<size_t> ecu_dist(0, candidates.size() - 1);
    const EcuId new_src = candidates[ecu_dist(gen)];
    bool changed = false;
    for (auto& info : infos) {
      if (info.code == selected_key.code && info.comm_id == selected_key.comm_id) {
        info.ecu_pair.src_ecu = new_src;
        changed = true;
      }
    }
    if (changed) {
      return true;
    }
  }

  return false;
}

double calculate_scheme_score(const SchemeMetrics& metrics) {
  double score = metrics.normal_bandwidth_utilization +
                 kFaultBandwidthWeight * metrics.fault_bandwidth_utilization;
  if (!metrics.schedulable) {
    score += 1.0;
  }
  return score;
}

OptimizedSchemeResult optimize_infos_with_route_sa(const MessageInfoVec& initial_infos, const SchemeAnalysisConfig& config) {
  if (!config.enable_route_source_perturbation) {
    MESSAGE_INFO_VEC = initial_infos;
    PackingScheme scheme = build_algorithm2_scheme_from_current_msgs(config);
    return {MESSAGE_INFO_VEC, std::move(scheme)};
  }

  const auto active_ecus = infer_active_ecus(initial_infos);
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_real_distribution<> dist(0.0, 1.0);

  MessageInfoVec current_infos = initial_infos;
  MESSAGE_INFO_VEC = current_infos;
  PackingScheme current_scheme = build_algorithm2_scheme_from_current_msgs(config);
  MessageInfoVec current_scheme_infos = MESSAGE_INFO_VEC;
  CanfdFrameMap current_fault_frame_map = build_analysis_frame_map(current_scheme, config);
  SchemeMetrics current_metrics = analyze_scheme(config, current_scheme, current_fault_frame_map);
  double current_score = calculate_scheme_score(current_metrics);
  MESSAGE_INFO_VEC.resize(current_infos.size());

  MessageInfoVec best_infos = current_scheme_infos;
  PackingScheme best_scheme = current_scheme;
  double best_score = current_score;
  bool have_feasible_best = current_metrics.schedulable;

  const auto perturbable_keys = build_perturbable_route_keys(initial_infos);
  if (perturbable_keys.empty()) {
    MESSAGE_INFO_VEC = best_infos;
    return {best_infos, best_scheme};
  }

  double temperature = kOuterSaInitialTemperature;
  const int local_trials = 1;

  while (temperature > kOuterSaFinalTemperature) {
    for (int trial = 0; trial < local_trials; ++trial) {
      MessageInfoVec candidate_infos = current_infos;
      if (!perturb_route_source(candidate_infos, active_ecus, gen)) {
        continue;
      }

      MESSAGE_INFO_VEC = candidate_infos;
      PackingScheme candidate_scheme = build_algorithm2_scheme_from_current_msgs(config);
      MessageInfoVec candidate_scheme_infos = MESSAGE_INFO_VEC;
      CanfdFrameMap candidate_fault_frame_map = build_analysis_frame_map(candidate_scheme, config);
      SchemeMetrics candidate_metrics = analyze_scheme(config, candidate_scheme, candidate_fault_frame_map);
      const double candidate_score = calculate_scheme_score(candidate_metrics);
      MESSAGE_INFO_VEC.resize(candidate_infos.size());

      if (candidate_metrics.schedulable && (!have_feasible_best || candidate_score < best_score)) {
        best_infos = candidate_scheme_infos;
        best_scheme = candidate_scheme;
        best_score = candidate_score;
        have_feasible_best = true;
      }

      const bool accept_better = candidate_score <= current_score;
      const double acceptance_probability =
          std::exp(-(candidate_score - current_score) * kOuterSaCostScale / temperature);
      if (accept_better || dist(gen) < acceptance_probability) {
        current_infos = std::move(candidate_infos);
        current_scheme = std::move(candidate_scheme);
        current_metrics = std::move(candidate_metrics);
        current_score = candidate_score;
      }
    }
    temperature *= kOuterSaAlpha;
  }

  MESSAGE_INFO_VEC = best_infos;
  return {best_infos, best_scheme};
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
    if (is_event_info(info)) {
      continue;
    }
    const RouteKey key = make_route_key(info);
    if (meta_by_route.count(key) == 0) {
      meta_by_route.emplace(key, RouteMeta{info.level, info.period, info.deadline, info.type, info.ecu_pair.src_ecu,
                                           info.ecu_pair.dst_ecu});
    }
  }
  return meta_by_route;
}

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

FoundationQuickMetrics build_foundation_quick_metrics(const MessageInfoVec& original_infos) {
  MESSAGE_INFO_VEC = original_infos;
  PackingScheme scheme{};
  cfd::packing::frame_pack(scheme, cfd::DEFAULT_PACK_METHOD);
  scheme = backups::signal::homo_signal_backup(scheme);

  FoundationQuickMetrics metrics;
  metrics.bandwidth_utilization = scheme.calc_bandwidth_utilization();
  metrics.schedulable = cfd::schedule::feasibility_check(scheme.frame_map);

  const auto max_wcrt_by_code = build_signal_group_max_wcrt_map(scheme);
  for (const auto& [code, max_wcrt_ms] : max_wcrt_by_code) {
    (void)code;
    metrics.max_wcrt_ms = std::max(metrics.max_wcrt_ms, max_wcrt_ms);
  }

  const auto instance_count_map = calc_signal_instance_count(scheme);
  for (const auto& [code, instance_count] : instance_count_map) {
    (void)code;
    metrics.total_added_signal_copies += std::max(0, instance_count - 1);
  }

  return metrics;
}

double max_cluster_normal_e2e(const std::vector<ClusterWcrtMetric>& cluster_wcrts) {
  double value = 0.0;
  for (const auto& cluster : cluster_wcrts) {
    value = std::max(value, cluster.end_to_end_normal_wcrt_ms);
  }
  return value;
}

double max_cluster_fault_e2e(const std::vector<ClusterWcrtMetric>& cluster_wcrts) {
  double value = 0.0;
  for (const auto& cluster : cluster_wcrts) {
    value = std::max(value, cluster.end_to_end_fault_wcrt_ms);
  }
  return value;
}

BuiltSchemeResult build_scheme_result(const MessageInfoVec& functional_infos, const SchemeAnalysisConfig& config,
                                      bool build_fault_state_with_events) {
  OptimizedSchemeResult optimized = optimize_infos_with_route_sa(functional_infos, config);
  MESSAGE_INFO_VEC = optimized.infos;
  PackingScheme scheme = std::move(optimized.scheme);
  const double periodic_bandwidth_utilization = scheme.calc_bandwidth_utilization();
  const CanfdFrameMap fault_frame_map =
      build_fault_state_with_events ? build_fault_state_frame_map(scheme) : build_full_tmr_frame_map(scheme);
  SchemeMetrics metrics = analyze_scheme(config, scheme, fault_frame_map);
  return {MESSAGE_INFO_VEC, std::move(scheme), std::move(metrics), periodic_bandwidth_utilization};
}

SchemeMetrics analyze_scheme(const SchemeAnalysisConfig& config, PackingScheme& scheme,
                             const CanfdFrameMap& fault_frame_map) {
  SchemeMetrics metrics;
  metrics.name = config.name;
  metrics.normal_bandwidth_utilization =
      calc_mode_bandwidth_utilization(scheme.frame_map, config.include_backup_routes_in_normal_mode, false);
  metrics.fault_bandwidth_utilization = calc_mode_bandwidth_utilization(
      fault_frame_map, config.include_backup_routes_in_fault_mode, config.include_event_frames_in_fault_mode);
  metrics.schedulable = metrics.fault_bandwidth_utilization <= 1.0 &&
                        cfd::schedule::feasibility_check(fault_frame_map);

  const auto meta_by_route = build_route_meta_map();
  const auto route_fault_probabilities = calc_route_fault_probabilities(scheme, LAMBDA_CONFERENCE);
  const auto route_wcrt = calc_route_max_wcrt(fault_frame_map);
  const auto normal_frame_map =
      filter_active_frame_map(scheme.frame_map, config.include_backup_routes_in_normal_mode, false);
  const auto normal_route_wcrt = calc_route_max_wcrt(normal_frame_map);
  const auto route_instance_count = calc_route_instance_count(scheme.frame_map);
  PackingScheme fault_scheme(fault_frame_map);
  const auto cluster_fault_probabilities = analysis::noretry::ecu_fault_prob_analysis(fault_scheme, REDUNDANCY_N,
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

  if (config.cluster_wcrt_mode == SchemeAnalysisConfig::ClusterWcrtMode::OnDemand) {
    metrics.cluster_wcrts = calc_cluster_wcrts_on_demand(normal_route_wcrt, route_wcrt, fault_frame_map);
  } else if (config.cluster_wcrt_mode == SchemeAnalysisConfig::ClusterWcrtMode::AlwaysOnTmr) {
    metrics.cluster_wcrts = calc_cluster_wcrts_always_on_tmr(route_wcrt);
  }

  for (const auto& cluster_wcrt : metrics.cluster_wcrts) {
    if (!cluster_wcrt.meets_deadline) {
      metrics.schedulable = false;
    }
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
  ofs << "# 正常模式带宽只统计 comm_id=0/1 的功能路；故障模式带宽统计全量功能路与事件触发控制报文。\n";
  ofs << "# 事件报文集合 F_evt={f_act,f_iso,f_hang}，统一取 1 byte 负载，MIT 取对应功能簇原始信号周期。\n\n";

  ofs << "[scheme_summary]\n";
  ofs << "scheme\tnormal_bandwidth_utilization\tfault_bandwidth_utilization\ttotal_added_signal_copies\tschedulable\n";
  for (const auto& scheme : summary.schemes) {
    ofs << scheme.name << '\t' << scheme.normal_bandwidth_utilization << '\t' << scheme.fault_bandwidth_utilization
        << '\t' << scheme.total_added_signal_copies << '\t' << (scheme.schedulable ? 1 : 0) << '\n';
  }
  ofs << '\n';

  ofs << "[cluster_summary]\n";
  ofs << "scheme\tcode\troute_count\tp_function_fault\tp_threshold\n";
  for (const auto& scheme : summary.schemes) {
    for (const auto& cluster : scheme.clusters) {
      ofs << scheme.name << '\t' << cluster.code << '\t' << cluster.route_count << '\t' << cluster.p_function_fault
          << '\t' << cluster.p_threshold << '\n';
    }
  }
  ofs << '\n';

  ofs << "[cluster_wcrt_summary]\n";
  ofs << "scheme\tcode\tperiod_ms\tdeadline_ms\tnormal_main_route_wcrt_ms\tfault_main_route_wcrt_ms\t"
         "tmr_all_route_wcrt_ms\tactivation_wcrt_ms\tbackup_route_wcrt_ms\tcompare_time_ms\t"
         "backup_compute_time_ms\tend_to_end_normal_wcrt_ms\tend_to_end_fault_wcrt_ms\tmeets_deadline\n";
  for (const auto& scheme : summary.schemes) {
    for (const auto& cluster : scheme.cluster_wcrts) {
      ofs << scheme.name << '\t' << cluster.code << '\t' << cluster.period_ms << '\t' << cluster.deadline_ms << '\t'
          << cluster.normal_main_route_wcrt_ms << '\t' << cluster.fault_main_route_wcrt_ms << '\t'
          << cluster.tmr_all_route_wcrt_ms << '\t' << cluster.activation_wcrt_ms << '\t'
          << cluster.backup_route_wcrt_ms << '\t' << cluster.compare_time_ms << '\t'
          << cluster.backup_compute_time_ms << '\t' << cluster.end_to_end_normal_wcrt_ms << '\t'
          << cluster.end_to_end_fault_wcrt_ms << '\t' << (cluster.meets_deadline ? 1 : 0) << '\n';
    }
  }
  ofs << '\n';

  ofs << "[route_summary]\n";
  ofs << "scheme\tcode\tcomm_id\tlevel\ttype\tsrc_ecu\tdst_ecu\tperiod_ms\tdeadline_ms\tsignal_instance_count\t"
         "added_signal_copies\tp_comm_fault\tp_threshold\tmax_wcrt_ms\n";
  for (const auto& scheme : summary.schemes) {
    for (const auto& route : scheme.routes) {
      ofs << scheme.name << '\t' << route.code << '\t' << route.comm_id << '\t' << route.level << '\t' << route.type
          << '\t' << route.src_ecu << '\t' << route.dst_ecu << '\t' << route.period_ms << '\t'
          << route.deadline_ms << '\t' << route.signal_instance_count << '\t' << route.added_signal_copies << '\t'
          << route.p_comm_fault << '\t' << route.p_threshold << '\t' << route.max_wcrt_ms << '\n';
    }
  }
}

void append_scheme_result(DatasetSummary& summary, const MessageInfoVec& functional_infos,
                          const SchemeAnalysisConfig& config, bool build_fault_state_with_events) {
  OptimizedSchemeResult optimized = optimize_infos_with_route_sa(functional_infos, config);
  MESSAGE_INFO_VEC = optimized.infos;
  PackingScheme scheme = std::move(optimized.scheme);
  const CanfdFrameMap fault_frame_map =
      build_fault_state_with_events ? build_fault_state_frame_map(scheme) : build_full_tmr_frame_map(scheme);

  summary.schemes.push_back(analyze_scheme(config, scheme, fault_frame_map));
  MESSAGE_INFO_VEC.resize(optimized.infos.size());
  const auto& scheme_metrics = summary.schemes.back();
  auto mappings = collect_signal_frame_mappings(summary.dataset_tag, scheme_metrics.name, scheme);
  summary.signal_frame_mappings.insert(summary.signal_frame_mappings.end(), mappings.begin(), mappings.end());
}

}  // namespace

DatasetSummary run_compare_experiment(const std::string& dataset_file, const std::string& run_tag) {
  const std::string full_path = cfd::storage::resolve_dataset_input_path(dataset_file);
  cfd::utils::read_message(full_path);
  const MessageInfoVec original_infos = MESSAGE_INFO_VEC;
  const MessageInfoVec functional_infos = generate_functional_routes(original_infos);
  const SchemeAnalysisConfig on_demand_config{"on_demand_tmr", false, true, true, true,
                                              SchemeAnalysisConfig::ClusterWcrtMode::OnDemand};
  const SchemeAnalysisConfig always_on_config{"always_on_tmr", true, true, false, false,
                                              SchemeAnalysisConfig::ClusterWcrtMode::AlwaysOnTmr};

  DatasetSummary summary;
  summary.dataset_tag = cfd::storage::dataset_tag_from_file(dataset_file);
  summary.config_tag = dataset_config_tag_from_dataset_tag(summary.dataset_tag);
  append_scheme_result(summary, functional_infos, on_demand_config, true);
  append_scheme_result(summary, functional_infos, always_on_config, false);

  write_dataset_report(compare_report_path(run_tag, summary.dataset_tag), summary);
  return summary;
}

QuickCompareResult quick_compare_signal_set(const MessageInfoVec& signal_infos) {
  const MessageInfoVec saved_infos = MESSAGE_INFO_VEC;
  QuickCompareResult result;

  const MessageInfoVec functional_infos = generate_functional_routes(signal_infos);
  const SchemeAnalysisConfig on_demand_config{"on_demand_tmr", false, true, true, true,
                                              SchemeAnalysisConfig::ClusterWcrtMode::OnDemand};
  const SchemeAnalysisConfig always_on_config{"always_on_tmr", true, true, false, false,
                                              SchemeAnalysisConfig::ClusterWcrtMode::AlwaysOnTmr};

  result.homo_only_foundation = build_foundation_quick_metrics(signal_infos);

  BuiltSchemeResult on_demand = build_scheme_result(functional_infos, on_demand_config, true);
  result.on_demand_tmr = on_demand.metrics;
  result.on_demand_fault_periodic_bandwidth_utilization = on_demand.periodic_bandwidth_utilization;
  result.on_demand_max_normal_end_to_end_wcrt_ms = max_cluster_normal_e2e(result.on_demand_tmr.cluster_wcrts);
  result.on_demand_max_fault_end_to_end_wcrt_ms = max_cluster_fault_e2e(result.on_demand_tmr.cluster_wcrts);

  BuiltSchemeResult always_on = build_scheme_result(functional_infos, always_on_config, false);
  result.always_on_tmr = always_on.metrics;
  result.always_on_end_to_end_wcrt_ms = max_cluster_normal_e2e(result.always_on_tmr.cluster_wcrts);

  MESSAGE_INFO_VEC = saved_infos;
  return result;
}

QuickCompareResult quick_compare_dataset_file(const std::string& dataset_file) {
  const MessageInfoVec saved_infos = MESSAGE_INFO_VEC;
  const std::string full_path = cfd::storage::resolve_dataset_input_path(dataset_file);
  cfd::utils::read_message(full_path);
  const MessageInfoVec signal_infos = MESSAGE_INFO_VEC;
  QuickCompareResult result = quick_compare_signal_set(signal_infos);
  MESSAGE_INFO_VEC = saved_infos;
  return result;
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
    for (const auto& scheme : summary.schemes) {
      ofs << summary.dataset_tag << '\t' << summary.config_tag << '\t' << scheme.name << '\t'
          << scheme.normal_bandwidth_utilization << '\t' << scheme.fault_bandwidth_utilization << '\t'
          << scheme.total_added_signal_copies << '\t' << (scheme.schedulable ? 1 : 0) << '\n';
    }
  }
  ofs << '\n';

  ofs << "[cluster_summary]\n";
  ofs << "dataset\tconfig\tscheme\tcode\troute_count\tp_function_fault\tp_threshold\n";
  for (const auto& summary : dataset_summaries) {
    for (const auto& scheme : summary.schemes) {
      for (const auto& cluster : scheme.clusters) {
        ofs << summary.dataset_tag << '\t' << summary.config_tag << '\t' << scheme.name << '\t' << cluster.code
            << '\t' << cluster.route_count << '\t' << cluster.p_function_fault << '\t' << cluster.p_threshold
            << '\n';
      }
    }
  }
  ofs << '\n';

  ofs << "[cluster_wcrt_summary]\n";
  ofs << "dataset\tconfig\tscheme\tcode\tperiod_ms\tdeadline_ms\tnormal_main_route_wcrt_ms\t"
         "fault_main_route_wcrt_ms\ttmr_all_route_wcrt_ms\tactivation_wcrt_ms\tbackup_route_wcrt_ms\t"
         "compare_time_ms\tbackup_compute_time_ms\tend_to_end_normal_wcrt_ms\tend_to_end_fault_wcrt_ms\t"
         "meets_deadline\n";
  for (const auto& summary : dataset_summaries) {
    for (const auto& scheme : summary.schemes) {
      for (const auto& cluster : scheme.cluster_wcrts) {
        ofs << summary.dataset_tag << '\t' << summary.config_tag << '\t' << scheme.name << '\t' << cluster.code
            << '\t' << cluster.period_ms << '\t' << cluster.deadline_ms << '\t'
            << cluster.normal_main_route_wcrt_ms << '\t' << cluster.fault_main_route_wcrt_ms << '\t'
            << cluster.tmr_all_route_wcrt_ms << '\t' << cluster.activation_wcrt_ms << '\t'
            << cluster.backup_route_wcrt_ms << '\t' << cluster.compare_time_ms << '\t'
            << cluster.backup_compute_time_ms << '\t' << cluster.end_to_end_normal_wcrt_ms << '\t'
            << cluster.end_to_end_fault_wcrt_ms << '\t' << (cluster.meets_deadline ? 1 : 0) << '\n';
      }
    }
  }
  ofs << '\n';

  ofs << "[route_summary]\n";
  ofs << "dataset\tconfig\tscheme\tcode\tcomm_id\tlevel\ttype\tsrc_ecu\tdst_ecu\tperiod_ms\tdeadline_ms\t"
         "signal_instance_count\tadded_signal_copies\tp_comm_fault\tp_threshold\tmax_wcrt_ms\n";
  for (const auto& summary : dataset_summaries) {
    for (const auto& scheme : summary.schemes) {
      for (const auto& route : scheme.routes) {
        ofs << summary.dataset_tag << '\t' << summary.config_tag << '\t' << scheme.name << '\t' << route.code << '\t'
            << route.comm_id << '\t' << route.level << '\t' << route.type << '\t' << route.src_ecu << '\t'
            << route.dst_ecu << '\t' << route.period_ms << '\t' << route.deadline_ms << '\t'
            << route.signal_instance_count << '\t' << route.added_signal_copies << '\t' << route.p_comm_fault << '\t'
            << route.p_threshold << '\t' << route.max_wcrt_ms << '\n';
      }
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
