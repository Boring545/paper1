#include "algorithm2.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
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
#include "../utils/fixed_worker_pool.h"

#define MESSAGE_INFO_VEC (::cfd::current_message_infos())

namespace cfd::algorithm2 {

namespace fs = std::filesystem;

namespace {

bool g_route_source_perturbation_enabled = true;
bool g_skip_foundation_enabled = false;
bool g_foundation_only_enabled = false;

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
  bool enable_route_backup_acceptance_check = false;
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

enum class RouteBackupAcceptanceMode { PackOnly, StrictFinal };

constexpr int kPrimaryRouteCommId = 0;
constexpr int kSecondMainRouteCommId = 1;
constexpr int kBackupRouteCommId = 2;
constexpr int kActEventCommId = 1001;
constexpr int kHangEventCommId = 1003;
constexpr int kEventMessageType = -1;
constexpr int kEventPayloadBytes = 1;
constexpr int kActEventPriorityPeriodMs = 2;
constexpr int kHangEventPriorityPeriodMs = 20;
constexpr int kMaxRouteBackupIterations = 50;
constexpr double kCompareTimeMs = 0.01;
constexpr double kBackupComputeTimeMs = 0.0;
constexpr double kOuterSaInitialTemperature = 5.0;
constexpr double kOuterSaFinalTemperature = 0.05;
constexpr double kOuterSaAlpha = 0.90;
constexpr double kOuterSaCostScale = 30.0;
constexpr int kOuterSaLocalTrials = 4;
constexpr int kIntegratedSaMinMoveMessages = 2;
constexpr double kFaultBandwidthWeight = 0.001;
thread_local std::string g_current_dataset_log_tag;

SchemeMetrics analyze_scheme(const SchemeAnalysisConfig& config, PackingScheme& scheme,
                             const CanfdFrameMap& fault_frame_map);
double calculate_scheme_score(const SchemeMetrics& metrics);
double calc_mode_bandwidth_utilization(const CanfdFrameMap& frame_map, bool include_backup_routes,
                                       bool include_event_frames);

RouteKey make_route_key(const MessageInfo& info) { return {info.code, info.comm_id}; }

RouteKey make_route_key(const Message& msg) { return {msg.get_code(), msg.get_comm_id()}; }

bool is_backup_route(int comm_id) { return comm_id == kBackupRouteCommId; }

void log_stage(const std::string& scheme, const std::string& stage) {
  if (g_current_dataset_log_tag.empty()) {
    DEBUG_MSG_DEBUG1(std::cout, "[algorithm2] ", scheme, " :: ", stage);
    return;
  }
  DEBUG_MSG_DEBUG1(std::cout, "[algorithm2] dataset=", g_current_dataset_log_tag, ", scheme=", scheme, " :: ", stage);
}

bool is_event_info(const MessageInfo& info) { return info.type == kEventMessageType; }

bool is_event_message(const Message& msg) { return is_event_info(MESSAGE_INFO_VEC[msg.get_id_message()]); }

int calc_priority_period_for_frame(const CanfdFrame& frame, const SchemeAnalysisConfig& config) {
  (void)config;
  for (const auto& msg : frame.msg_set) {
    if (is_event_message(msg)) {
      if (msg.get_comm_id() == kActEventCommId) {
        return kActEventPriorityPeriodMs;
      }
      return kHangEventPriorityPeriodMs;
    }
  }
  return frame.get_period();
}

bool assign_algorithm2_priority(CanfdFrameMap& frame_map, const SchemeAnalysisConfig& config) {
  std::vector<CanfdFrame*> frames;
  frames.reserve(frame_map.size());

  for (auto& [frame_id, frame] : frame_map) {
    (void)frame_id;
    if (frame.empty()) {
      continue;
    }
    frames.push_back(&frame);
  }

  std::sort(frames.begin(), frames.end(), [&](const CanfdFrame* lhs, const CanfdFrame* rhs) {
    const int lhs_period = calc_priority_period_for_frame(*lhs, config);
    const int rhs_period = calc_priority_period_for_frame(*rhs, config);
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

std::string packing_plan_report_path(const std::string& run_tag) {
  return cfd::storage::path_string(cfd::storage::analysis_batch_dir(run_tag) / "algorithm2_packing_plan_tab.txt");
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

CanfdFrameMap build_fault_state_frame_map(const PackingScheme& scheme, const SchemeAnalysisConfig& config) {
  CanfdFrameMap frame_map = scheme.frame_map;
  for (const auto& cluster : build_event_cluster_meta()) {
    append_event_frame(frame_map, cluster, kActEventCommId, EcuPair(cluster.dst_ecu, cluster.backup_src_ecu));
    append_event_frame(frame_map, cluster, kHangEventCommId, EcuPair(cluster.dst_ecu, cluster.backup_src_ecu));
  }

  assign_algorithm2_priority(frame_map, config);
  return frame_map;
}

CanfdFrameMap build_full_tmr_frame_map(const PackingScheme& scheme, const SchemeAnalysisConfig& config) {
  CanfdFrameMap frame_map = scheme.frame_map;
  assign_algorithm2_priority(frame_map, config);
  return frame_map;
}

CanfdFrameMap build_analysis_frame_map(const PackingScheme& scheme, const SchemeAnalysisConfig& config) {
  if (config.include_event_frames_in_fault_mode) {
    return build_fault_state_frame_map(scheme, config);
  }
  return build_full_tmr_frame_map(scheme, config);
}

CanfdFrameMap filter_active_frame_map(const CanfdFrameMap& frame_map, bool include_backup_routes, bool include_event_frames,
                                      const SchemeAnalysisConfig& config) {
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

  assign_algorithm2_priority(filtered, config);
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

bool is_scheme_acceptable(const PackingScheme& scheme, const SchemeAnalysisConfig& config,
                          std::string* rejection_reason = nullptr) {
  const size_t message_count_before_events = MESSAGE_INFO_VEC.size();
  PackingScheme trial = scheme;
  const CanfdFrameMap fault_frame_map = build_analysis_frame_map(trial, config);
  SchemeMetrics metrics = analyze_scheme(config, trial, fault_frame_map);
  if (!metrics.schedulable && rejection_reason != nullptr) {
    std::vector<std::string> reasons;
    const double normal_bandwidth = scheme.calc_bandwidth_utilization();
    const double fault_bandwidth =
        calc_mode_bandwidth_utilization(fault_frame_map, config.include_backup_routes_in_fault_mode,
                                        config.include_event_frames_in_fault_mode);
    if (normal_bandwidth > 1.0) {
      reasons.push_back("normal bandwidth exceeds limit: " + std::to_string(normal_bandwidth));
    }
    if (fault_bandwidth > 1.0) {
      reasons.push_back("fault bandwidth exceeds limit: " + std::to_string(fault_bandwidth));
    }
    if (!cfd::schedule::feasibility_check(scheme.frame_map)) {
      reasons.push_back("normal frame set is not schedulable");
    }
    if (!cfd::schedule::feasibility_check(fault_frame_map)) {
      reasons.push_back("fault frame set is not schedulable");
    }

    int route_reliability_failures = 0;
    int route_wcrt_failures = 0;
    for (const auto& route : metrics.routes) {
      if (route.p_comm_fault > route.p_threshold) {
        ++route_reliability_failures;
      }
      if (route.max_wcrt_ms > static_cast<double>(route.deadline_ms)) {
        ++route_wcrt_failures;
      }
    }
    if (route_reliability_failures > 0) {
      reasons.push_back("route reliability failures: " + std::to_string(route_reliability_failures));
    }
    if (route_wcrt_failures > 0) {
      reasons.push_back("route WCRT deadline failures: " + std::to_string(route_wcrt_failures));
    }

    int cluster_reliability_failures = 0;
    for (const auto& cluster : metrics.clusters) {
      if (cluster.p_function_fault > cluster.p_threshold) {
        ++cluster_reliability_failures;
      }
    }
    if (cluster_reliability_failures > 0) {
      reasons.push_back("cluster reliability failures: " + std::to_string(cluster_reliability_failures));
    }

    int cluster_wcrt_failures = 0;
    double max_cluster_e2e_overrun_ms = 0.0;
    MessageCode max_cluster_e2e_overrun_code = 0;
    for (const auto& cluster_wcrt : metrics.cluster_wcrts) {
      if (!cluster_wcrt.meets_deadline) {
        ++cluster_wcrt_failures;
        const double overrun =
            cluster_wcrt.end_to_end_fault_wcrt_ms - static_cast<double>(cluster_wcrt.deadline_ms);
        if (overrun > max_cluster_e2e_overrun_ms) {
          max_cluster_e2e_overrun_ms = overrun;
          max_cluster_e2e_overrun_code = cluster_wcrt.code;
        }
      }
    }
    if (cluster_wcrt_failures > 0) {
      reasons.push_back("cluster E2E WCRT failures: " + std::to_string(cluster_wcrt_failures) +
                        ", max_overrun_code=" + std::to_string(max_cluster_e2e_overrun_code) +
                        ", max_overrun_ms=" + std::to_string(max_cluster_e2e_overrun_ms));
    }

    if (reasons.empty()) {
      *rejection_reason = "strict final analysis rejected scheme for an unclassified reason";
    } else {
      std::ostringstream oss;
      for (size_t index = 0; index < reasons.size(); ++index) {
        if (index > 0) oss << "; ";
        oss << reasons[index];
      }
      *rejection_reason = oss.str();
    }
  }
  MESSAGE_INFO_VEC.resize(message_count_before_events);
  return metrics.schedulable;
}

bool is_route_backup_pack_acceptable(const PackingScheme& scheme, const SchemeAnalysisConfig& config,
                                     std::string* rejection_reason = nullptr) {
  const double normal_bandwidth = scheme.calc_bandwidth_utilization();
  if (normal_bandwidth > 1.0) {
    if (rejection_reason != nullptr) {
      *rejection_reason = "normal bandwidth exceeds limit: " + std::to_string(normal_bandwidth);
    }
    return false;
  }
  if (!cfd::schedule::feasibility_check(scheme.frame_map)) {
    if (rejection_reason != nullptr) {
      *rejection_reason = "normal frame set is not schedulable";
    }
    return false;
  }

  const size_t message_count_before_events = MESSAGE_INFO_VEC.size();
  PackingScheme trial = scheme;
  const CanfdFrameMap fault_frame_map = build_analysis_frame_map(trial, config);
  const double fault_bandwidth =
      calc_mode_bandwidth_utilization(fault_frame_map, config.include_backup_routes_in_fault_mode,
                                      config.include_event_frames_in_fault_mode);
  if (fault_bandwidth > 1.0) {
    if (rejection_reason != nullptr) {
      *rejection_reason = "fault bandwidth exceeds limit: " + std::to_string(fault_bandwidth);
    }
    MESSAGE_INFO_VEC.resize(message_count_before_events);
    return false;
  }
  const bool fault_schedulable = cfd::schedule::feasibility_check(fault_frame_map);
  if (!fault_schedulable && rejection_reason != nullptr) {
    *rejection_reason = "fault frame set is not schedulable";
  }
  MESSAGE_INFO_VEC.resize(message_count_before_events);
  return fault_schedulable;
}

std::map<int, std::vector<int>> build_valid_period_map_for_algorithm2() {
  std::map<int, std::vector<int>> valid_period_map;
  for (int i = 0; i < NUM_MESSAGE_PERIOD; i++) {
    for (int j = i; j < NUM_MESSAGE_PERIOD; j++) {
      if (OPTION_MESSAGE_PERIOD[j] % OPTION_MESSAGE_PERIOD[i] == 0) {
        const int factor = OPTION_MESSAGE_PERIOD[j] / OPTION_MESSAGE_PERIOD[i];
        if (factor <= FACTOR_M_F_PERIOD) {
          valid_period_map[OPTION_MESSAGE_PERIOD[i]].push_back(OPTION_MESSAGE_PERIOD[j]);
          if (factor == FACTOR_M_F_PERIOD) {
            break;
          }
        }
      }
    }
  }
  return valid_period_map;
}

void cleanup_empty_frames(PackingScheme& scheme) {
  std::vector<int> to_remove;
  for (const auto& [frame_id, frame] : scheme.frame_map) {
    if (frame.empty()) {
      to_remove.push_back(static_cast<int>(frame_id));
    }
  }
  for (const int frame_id : to_remove) {
    scheme.recover_id(frame_id);
    scheme.frame_map.erase(frame_id);
  }
}

bool insert_message_into_existing_or_new_frame(PackingScheme& scheme, Message& msg, std::mt19937& gen) {
  std::vector<FrameId> candidates;
  candidates.reserve(scheme.frame_map.size());
  for (const auto& [frame_id, frame] : scheme.frame_map) {
    if (frame.empty()) {
      continue;
    }
    if (!(frame.get_ecu_pair() == msg.get_ecu_pair())) {
      continue;
    }
    candidates.push_back(frame_id);
  }
  std::shuffle(candidates.begin(), candidates.end(), gen);

  for (const FrameId frame_id : candidates) {
    auto it = scheme.frame_map.find(frame_id);
    if (it == scheme.frame_map.end()) {
      continue;
    }
    if (it->second.add_message(msg)) {
      return true;
    }
  }

  return scheme.new_frame(msg) >= 0;
}

bool move_message_to_new_frame(PackingScheme& scheme, Message& msg) {
  const FrameId old_frame_id = msg.get_id_frame();
  auto old_frame_it = scheme.frame_map.find(old_frame_id);
  if (old_frame_it == scheme.frame_map.end()) {
    return false;
  }
  if (!old_frame_it->second.extract_message(msg)) {
    return false;
  }
  if (old_frame_it->second.empty()) {
    scheme.recover_id(static_cast<int>(old_frame_id));
    scheme.frame_map.erase(old_frame_id);
  }
  return scheme.new_frame(msg) >= 0;
}

bool move_message_to_existing_frame(PackingScheme& scheme, Message& msg, std::mt19937& gen) {
  const FrameId old_frame_id = msg.get_id_frame();
  if (scheme.frame_map.find(old_frame_id) == scheme.frame_map.end()) {
    return false;
  }

  std::vector<FrameId> candidates;
  candidates.reserve(scheme.frame_map.size());
  for (const auto& [frame_id, frame] : scheme.frame_map) {
    if (frame_id == old_frame_id || frame.empty()) {
      continue;
    }
    if (!(frame.get_ecu_pair() == msg.get_ecu_pair())) {
      continue;
    }
    candidates.push_back(frame_id);
  }
  if (candidates.empty()) {
    return false;
  }
  std::shuffle(candidates.begin(), candidates.end(), gen);

  for (const FrameId target_frame_id : candidates) {
    auto old_it = scheme.frame_map.find(old_frame_id);
    auto target_it = scheme.frame_map.find(target_frame_id);
    if (old_it == scheme.frame_map.end() || target_it == scheme.frame_map.end()) {
      return false;
    }
    if (old_it->second.move_message(target_it->second, msg)) {
      if (old_it->second.empty()) {
        scheme.recover_id(static_cast<int>(old_frame_id));
        scheme.frame_map.erase(old_frame_id);
      }
      return true;
    }
  }

  return false;
}

void perturb_regular_messages(PackingScheme& scheme, int move_count, std::mt19937& gen,
                              std::uniform_real_distribution<>& dist) {
  if (scheme.message_set.empty()) {
    return;
  }

  constexpr double kProbabilityNewFrame = 0.01;
  std::uniform_int_distribution<size_t> msg_dist(0, scheme.message_set.size() - 1);
  for (int move_index = 0; move_index < move_count; ++move_index) {
    Message& msg = scheme.message_set[msg_dist(gen)];
    if (msg.get_id_frame() == static_cast<FrameId>(-1)) {
      continue;
    }
    if (dist(gen) < kProbabilityNewFrame) {
      move_message_to_new_frame(scheme, msg);
    } else {
      move_message_to_existing_frame(scheme, msg, gen);
    }
  }
  cleanup_empty_frames(scheme);
}

std::vector<MessageCode> build_node_signal_codes() {
  std::vector<MessageCode> codes;
  std::unordered_set<MessageCode> seen;
  for (const auto& info : MESSAGE_INFO_VEC) {
    if (info.type == 1 && info.comm_id == kPrimaryRouteCommId && seen.insert(info.code).second) {
      codes.push_back(info.code);
    }
  }
  std::sort(codes.begin(), codes.end());
  return codes;
}

bool collect_route_sources(MessageCode code, int comm_id, std::optional<EcuId>& primary_src,
                           std::optional<EcuId>& sibling_src, std::optional<EcuId>& current_src,
                           std::optional<EcuId>& dst_ecu) {
  for (const auto& info : MESSAGE_INFO_VEC) {
    if (info.code != code) {
      continue;
    }
    if (info.comm_id == kPrimaryRouteCommId) {
      primary_src = info.ecu_pair.src_ecu;
      dst_ecu = info.ecu_pair.dst_ecu;
    } else if (info.comm_id == comm_id) {
      current_src = info.ecu_pair.src_ecu;
      dst_ecu = info.ecu_pair.dst_ecu;
    } else if ((comm_id == kSecondMainRouteCommId && info.comm_id == kBackupRouteCommId) ||
               (comm_id == kBackupRouteCommId && info.comm_id == kSecondMainRouteCommId)) {
      sibling_src = info.ecu_pair.src_ecu;
    }
  }
  return primary_src.has_value() && current_src.has_value() && dst_ecu.has_value();
}

std::optional<EcuId> pick_new_route_source(MessageCode code, int comm_id, const std::vector<EcuId>& active_ecus,
                                           std::mt19937& gen) {
  std::optional<EcuId> primary_src;
  std::optional<EcuId> sibling_src;
  std::optional<EcuId> current_src;
  std::optional<EcuId> dst_ecu;
  if (!collect_route_sources(code, comm_id, primary_src, sibling_src, current_src, dst_ecu)) {
    return std::nullopt;
  }

  std::vector<EcuId> candidates;
  for (const EcuId ecu : active_ecus) {
    if (ecu == *dst_ecu || ecu == *primary_src || ecu == *current_src) {
      continue;
    }
    if (sibling_src.has_value() && ecu == *sibling_src) {
      continue;
    }
    candidates.push_back(ecu);
  }
  if (candidates.empty()) {
    return std::nullopt;
  }

  std::uniform_int_distribution<size_t> ecu_dist(0, candidates.size() - 1);
  return candidates[ecu_dist(gen)];
}

bool migrate_route_source_in_scheme(PackingScheme& scheme, MessageCode code, int comm_id, EcuId new_src,
                                    std::mt19937& gen) {
  std::vector<size_t> message_positions;
  std::unordered_set<MessageID> info_indices;
  for (size_t index = 0; index < scheme.message_set.size(); ++index) {
    const auto& msg = scheme.message_set[index];
    if (msg.get_code() == code && msg.get_comm_id() == comm_id) {
      message_positions.push_back(index);
      info_indices.insert(msg.get_id_message());
    }
  }
  if (message_positions.empty()) {
    return false;
  }

  for (const size_t position : message_positions) {
    Message& msg = scheme.message_set[position];
    const FrameId old_frame_id = msg.get_id_frame();
    auto old_frame_it = scheme.frame_map.find(old_frame_id);
    if (old_frame_it == scheme.frame_map.end()) {
      continue;
    }
    old_frame_it->second.extract_message(msg);
    if (old_frame_it->second.empty()) {
      scheme.recover_id(static_cast<int>(old_frame_id));
      scheme.frame_map.erase(old_frame_id);
    }
  }

  for (const MessageID info_index : info_indices) {
    MESSAGE_INFO_VEC[info_index].ecu_pair.src_ecu = new_src;
  }

  bool inserted_all = true;
  for (const size_t position : message_positions) {
    Message& msg = scheme.message_set[position];
    if (!insert_message_into_existing_or_new_frame(scheme, msg, gen)) {
      inserted_all = false;
    }
  }
  cleanup_empty_frames(scheme);
  return inserted_all;
}

void perturb_node_route_sources(PackingScheme& scheme, const std::vector<EcuId>& active_ecus, double temperature,
                                std::mt19937& gen) {
  auto node_codes = build_node_signal_codes();
  if (node_codes.empty()) {
    return;
  }

  const int perturb_count = std::min<int>(
      static_cast<int>(node_codes.size()),
      std::max(1, static_cast<int>(std::ceil(static_cast<double>(node_codes.size()) * temperature /
                                             kOuterSaInitialTemperature))));
  std::shuffle(node_codes.begin(), node_codes.end(), gen);
  std::uniform_int_distribution<int> route_dist(0, 1);

  for (int index = 0; index < perturb_count; ++index) {
    const MessageCode code = node_codes[index];
    std::array<int, 2> route_candidates = {kSecondMainRouteCommId, kBackupRouteCommId};
    if (route_dist(gen) == 1) {
      std::swap(route_candidates[0], route_candidates[1]);
    }
    for (const int comm_id : route_candidates) {
      const auto new_src = pick_new_route_source(code, comm_id, active_ecus, gen);
      if (!new_src.has_value()) {
        continue;
      }
      if (migrate_route_source_in_scheme(scheme, code, comm_id, *new_src, gen)) {
        break;
      }
    }
  }
}

double evaluate_algorithm2_scheme_score(PackingScheme& scheme, const SchemeAnalysisConfig& config) {
  const size_t message_count_before_events = MESSAGE_INFO_VEC.size();
  const CanfdFrameMap fault_frame_map = build_analysis_frame_map(scheme, config);
  const double normal_bandwidth =
      calc_mode_bandwidth_utilization(scheme.frame_map, config.include_backup_routes_in_normal_mode, false);
  const double fault_bandwidth =
      calc_mode_bandwidth_utilization(fault_frame_map, config.include_backup_routes_in_fault_mode,
                                      config.include_event_frames_in_fault_mode);
  const bool schedulable =
      cfd::schedule::feasibility_check(scheme.frame_map) && cfd::schedule::feasibility_check(fault_frame_map);
  MESSAGE_INFO_VEC.resize(message_count_before_events);
  return normal_bandwidth + kFaultBandwidthWeight * fault_bandwidth + (schedulable ? 0.0 : 1.0);
}

PackingScheme integrated_algorithm2_sa_single_chain(PackingScheme scheme, const SchemeAnalysisConfig& config,
                                                    const std::vector<EcuId>& active_ecus) {
  if (!config.enable_route_source_perturbation) {
    return scheme;
  }

  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_real_distribution<> dist(0.0, 1.0);

  MessageInfoVec current_infos = MESSAGE_INFO_VEC;
  PackingScheme current_scheme = scheme;
  double current_score = evaluate_algorithm2_scheme_score(current_scheme, config);

  MessageInfoVec best_infos = current_infos;
  PackingScheme best_scheme = current_scheme;
  double best_score = current_score;

  double temperature = kOuterSaInitialTemperature;
  const int message_count = static_cast<int>(current_scheme.message_set.size());
  while (temperature > kOuterSaFinalTemperature) {
    const double temperature_ratio = temperature / kOuterSaInitialTemperature;
    const int max_move_count = std::max(kIntegratedSaMinMoveMessages, message_count / 10);
    const int move_count =
        std::max(kIntegratedSaMinMoveMessages, static_cast<int>(std::ceil(max_move_count * temperature_ratio)));

    for (int trial = 0; trial < kOuterSaLocalTrials; ++trial) {
      MessageInfoVec candidate_infos = current_infos;
      PackingScheme candidate_scheme = current_scheme;
      MESSAGE_INFO_VEC = candidate_infos;

      perturb_node_route_sources(candidate_scheme, active_ecus, temperature, gen);
      perturb_regular_messages(candidate_scheme, move_count, gen, dist);
      assign_algorithm2_priority(candidate_scheme.frame_map, config);

      candidate_infos = MESSAGE_INFO_VEC;
      const double candidate_score = evaluate_algorithm2_scheme_score(candidate_scheme, config);

      if (candidate_score <= best_score) {
        best_infos = candidate_infos;
        best_scheme = candidate_scheme;
        best_score = candidate_score;
      }

      const bool accept_better = candidate_score <= current_score;
      const double acceptance_probability =
          std::exp(-(candidate_score - current_score) * kOuterSaCostScale / temperature);
      if (accept_better || dist(gen) < acceptance_probability) {
        current_infos = std::move(candidate_infos);
        current_scheme = std::move(candidate_scheme);
        current_score = candidate_score;
      }
    }

    temperature *= kOuterSaAlpha;
  }

  MESSAGE_INFO_VEC = best_infos;
  assign_algorithm2_priority(best_scheme.frame_map, config);
  return best_scheme;
}

PackingScheme integrated_algorithm2_sa(PackingScheme scheme, const SchemeAnalysisConfig& config,
                                       const std::vector<EcuId>& active_ecus) {
  if (!config.enable_route_source_perturbation) {
    return scheme;
  }

  constexpr int SA_POPULATION_SIZE = 6;
  constexpr int SA_MIGRATION_INTERVAL = 8;
  constexpr bool SA_PARALLEL_ENABLED = true;

  struct Algorithm2SaIndividual {
    MessageInfoVec current_infos;
    PackingScheme current_scheme;
    double current_score = std::numeric_limits<double>::infinity();
    MessageInfoVec best_infos;
    PackingScheme best_scheme;
    double best_score = std::numeric_limits<double>::infinity();
    std::mt19937 gen;
    std::uniform_real_distribution<> dist{0.0, 1.0};
  };

  const MessageInfoVec initial_infos = MESSAGE_INFO_VEC;
  const int population_size = std::max(1, SA_POPULATION_SIZE);
  std::random_device rd;
  std::vector<Algorithm2SaIndividual> population;
  population.reserve(population_size);
  for (int i = 0; i < population_size; ++i) {
    Algorithm2SaIndividual individual;
    individual.current_infos = initial_infos;
    individual.current_scheme = scheme;
    {
      MessageInfoScope scope(individual.current_infos);
      individual.current_score = evaluate_algorithm2_scheme_score(individual.current_scheme, config);
    }
    individual.best_infos = individual.current_infos;
    individual.best_scheme = individual.current_scheme;
    individual.best_score = individual.current_score;
    individual.gen.seed(rd() ^ (static_cast<unsigned int>(i) * 0x9e3779b9U));
    population.push_back(std::move(individual));
  }

  MessageInfoVec global_best_infos = population.front().best_infos;
  PackingScheme global_best_scheme = population.front().best_scheme;
  double global_best_score = population.front().best_score;
  auto refresh_global_best = [&]() {
    for (const auto& individual : population) {
      if (individual.best_score < global_best_score) {
        global_best_infos = individual.best_infos;
        global_best_scheme = individual.best_scheme;
        global_best_score = individual.best_score;
      }
    }
  };

  double temperature = kOuterSaInitialTemperature;
  int temperature_layer = 0;
  const int message_count = static_cast<int>(scheme.message_set.size());
  std::unique_ptr<cfd::utils::FixedWorkerPool> worker_pool;
  if (SA_PARALLEL_ENABLED && population_size > 1) {
    worker_pool =
        std::make_unique<cfd::utils::FixedWorkerPool>(cfd::utils::recommended_worker_count(population_size));
  }
  while (temperature > kOuterSaFinalTemperature) {
    const double temperature_ratio = temperature / kOuterSaInitialTemperature;
    const int max_move_count = std::max(kIntegratedSaMinMoveMessages, message_count / 10);
    const int move_count =
        std::max(kIntegratedSaMinMoveMessages, static_cast<int>(std::ceil(max_move_count * temperature_ratio)));
    const int individual_trial_count =
        std::max(1, static_cast<int>(std::ceil(static_cast<double>(kOuterSaLocalTrials) / population_size)));

    auto update_individual = [&](Algorithm2SaIndividual individual) {
      for (int trial = 0; trial < individual_trial_count; ++trial) {
        MessageInfoVec candidate_infos = individual.current_infos;
        PackingScheme candidate_scheme = individual.current_scheme;
        double candidate_score = std::numeric_limits<double>::infinity();

        {
          MessageInfoScope scope(candidate_infos);
          perturb_node_route_sources(candidate_scheme, active_ecus, temperature, individual.gen);
          perturb_regular_messages(candidate_scheme, move_count, individual.gen, individual.dist);
          assign_algorithm2_priority(candidate_scheme.frame_map, config);
          candidate_score = evaluate_algorithm2_scheme_score(candidate_scheme, config);
        }

        if (candidate_score <= individual.best_score) {
          individual.best_infos = candidate_infos;
          individual.best_scheme = candidate_scheme;
          individual.best_score = candidate_score;
        }

        const bool accept_better = candidate_score <= individual.current_score;
        const double acceptance_probability =
            std::exp(-(candidate_score - individual.current_score) * kOuterSaCostScale / temperature);
        if (accept_better || individual.dist(individual.gen) < acceptance_probability) {
          individual.current_infos = std::move(candidate_infos);
          individual.current_scheme = std::move(candidate_scheme);
          individual.current_score = candidate_score;
        }
      }
      return individual;
    };

    if (worker_pool != nullptr) {
      worker_pool->parallel_for(population.size(), [&](size_t i) {
        population[i] = update_individual(std::move(population[i]));
      });
    } else {
      for (auto& individual : population) {
        individual = update_individual(std::move(individual));
      }
    }

    refresh_global_best();

    if (population_size > 1 && SA_MIGRATION_INTERVAL > 0 && temperature_layer > 0 &&
        temperature_layer % SA_MIGRATION_INTERVAL == 0) {
      auto worst_it = std::max_element(population.begin(), population.end(),
                                       [](const Algorithm2SaIndividual& lhs, const Algorithm2SaIndividual& rhs) {
                                         return lhs.current_score < rhs.current_score;
                                       });
      if (worst_it != population.end() && global_best_score < worst_it->current_score) {
        worst_it->current_infos = global_best_infos;
        worst_it->current_scheme = global_best_scheme;
        worst_it->current_score = global_best_score;
      }
    }

    temperature *= kOuterSaAlpha;
    ++temperature_layer;
  }

  refresh_global_best();
  MESSAGE_INFO_VEC = global_best_infos;
  assign_algorithm2_priority(global_best_scheme.frame_map, config);
  log_stage(config.name, "population integrated SA best score=" + std::to_string(global_best_score));
  return global_best_scheme;
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

bool try_apply_all_route_backup_candidates(const PackingScheme& base_scheme, const std::vector<BackupCandidate>& candidates,
                                           PackingScheme& out_scheme, const SchemeAnalysisConfig& config,
                                           RouteBackupAcceptanceMode acceptance_mode, std::string& rejection_reason) {
  if (candidates.empty()) {
    rejection_reason = "no route backup candidates";
    return false;
  }

  PackingScheme trial = base_scheme;
  for (const auto& candidate : candidates) {
    trial.message_set.emplace_back(candidate.message_index);
  }

  if (!trial.re_init_frames()) {
    rejection_reason = "re_init_frames failed after adding all route backup candidates";
    return false;
  }

  cfd::packing::frame_pack(trial, cfd::DEFAULT_PACK_METHOD);
  if (!config.enable_route_backup_acceptance_check) {
    out_scheme = std::move(trial);
    return true;
  }

  const bool acceptable = acceptance_mode == RouteBackupAcceptanceMode::PackOnly
                              ? is_route_backup_pack_acceptable(trial, config, &rejection_reason)
                              : is_scheme_acceptable(trial, config, &rejection_reason);
  if (!acceptable) {
    if (rejection_reason.empty()) {
      rejection_reason = "acceptance check failed";
    }
    return false;
  }

  out_scheme = std::move(trial);
  return true;
}

PackingScheme homo_route_backup(PackingScheme& scheme, const SchemeAnalysisConfig& config,
                                double lambda = LAMBDA_CONFERENCE,
                                RouteBackupAcceptanceMode acceptance_mode = RouteBackupAcceptanceMode::StrictFinal) {
  PackingScheme working = scheme;
  const auto route_index_map = build_route_index_map();

  for (int iter = 0; iter < kMaxRouteBackupIterations; ++iter) {
    const auto candidates = build_route_backup_candidates(working, lambda, route_index_map);
    if (candidates.empty()) {
      log_stage(config.name, "homo route backup has no candidates");
      return working;
    }

    PackingScheme feasible_scheme = working;
    std::string rejection_reason;
    log_stage(config.name, "homo route backup try all candidates=" + std::to_string(candidates.size()));
    if (!try_apply_all_route_backup_candidates(working, candidates, feasible_scheme, config, acceptance_mode,
                                               rejection_reason)) {
      log_stage(config.name, "homo route backup rejected all candidates: " + rejection_reason);
      return working;
    }

    const int added_count = static_cast<int>(feasible_scheme.message_set.size() - working.message_set.size());
    if (added_count <= 0) {
      log_stage(config.name, "homo route backup accepted no new copy");
      return working;
    }

    log_stage(config.name, "homo route backup accepted copies=" + std::to_string(added_count));
    working = std::move(feasible_scheme);
  }

  return working;
}

PackingScheme build_on_demand_tmr_scheme_from_current_msgs(const SchemeAnalysisConfig& config) {
  PackingScheme scheme{};
  log_stage(config.name, "frame_pack");
  cfd::packing::frame_pack(scheme, cfd::DEFAULT_PACK_METHOD);
  if (config.enable_route_source_perturbation) {
    scheme = integrated_algorithm2_sa(std::move(scheme), config, infer_active_ecus(MESSAGE_INFO_VEC));
  }
  log_stage(config.name, "homo route backup");
  scheme = homo_route_backup(scheme, config, LAMBDA_CONFERENCE, RouteBackupAcceptanceMode::PackOnly);
  return scheme;
}

PackingScheme build_always_on_tmr_scheme_from_current_msgs(const SchemeAnalysisConfig& config) {
  PackingScheme scheme{};
  log_stage(config.name, "frame_pack");
  cfd::packing::frame_pack(scheme, cfd::DEFAULT_PACK_METHOD);
  log_stage(config.name, "homo route backup");
  scheme = homo_route_backup(scheme, config, LAMBDA_CONFERENCE, RouteBackupAcceptanceMode::StrictFinal);
  return scheme;
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
  MESSAGE_INFO_VEC = initial_infos;
  log_stage(config.name, "build scheme begin");
  PackingScheme scheme = config.cluster_wcrt_mode == SchemeAnalysisConfig::ClusterWcrtMode::OnDemand
                             ? build_on_demand_tmr_scheme_from_current_msgs(config)
                             : build_always_on_tmr_scheme_from_current_msgs(config);
  log_stage(config.name, "build scheme done");
  return {MESSAGE_INFO_VEC, std::move(scheme)};
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
  log_stage("homo_only_foundation", "build begin");
  MESSAGE_INFO_VEC = original_infos;
  PackingScheme scheme{};
  log_stage("homo_only_foundation", "frame_pack");
  cfd::packing::frame_pack(scheme, cfd::DEFAULT_PACK_METHOD);
  log_stage("homo_only_foundation", "homo signal backup");
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

  log_stage("homo_only_foundation", "build done");
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
      build_fault_state_with_events ? build_fault_state_frame_map(scheme, config) : build_full_tmr_frame_map(scheme, config);
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
      filter_active_frame_map(scheme.frame_map, config.include_backup_routes_in_normal_mode, false, config);
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
  ofs << "# 事件报文集合 F_evt={f_act,f_hang}，统一取 1 byte 负载，MIT 取对应功能簇原始信号周期。\n\n";

  ofs << "[scheme_summary]\n";
  ofs << "scheme\tnormal_bandwidth_utilization\tfault_bandwidth_utilization\ttotal_added_signal_copies\tschedulable\n";
  for (const auto& scheme : summary.schemes) {
    ofs << scheme.name << '\t' << scheme.normal_bandwidth_utilization << '\t' << scheme.fault_bandwidth_utilization
        << '\t' << scheme.total_added_signal_copies << '\t' << (scheme.schedulable ? 1 : 0) << '\n';
  }
  ofs << '\n';

  ofs << "[foundation_summary]\n";
  ofs << "scheme\tbandwidth_utilization\tmax_wcrt_ms\ttotal_added_signal_copies\tschedulable\n";
  if (summary.has_homo_only_foundation) {
    ofs << "homo_only_foundation\t" << summary.homo_only_foundation.bandwidth_utilization << '\t'
        << summary.homo_only_foundation.max_wcrt_ms << '\t' << summary.homo_only_foundation.total_added_signal_copies
        << '\t' << (summary.homo_only_foundation.schedulable ? 1 : 0) << '\n';
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
  log_stage(config.name, build_fault_state_with_events ? "build fault frame map with events"
                                                       : "build full TMR frame map without events");
  const CanfdFrameMap fault_frame_map =
      build_fault_state_with_events ? build_fault_state_frame_map(scheme, config) : build_full_tmr_frame_map(scheme, config);

  log_stage(config.name, "final analyze");
  summary.schemes.push_back(analyze_scheme(config, scheme, fault_frame_map));
  MESSAGE_INFO_VEC.resize(optimized.infos.size());
  const auto& scheme_metrics = summary.schemes.back();
  auto mappings = collect_signal_frame_mappings(summary.dataset_tag, scheme_metrics.name, scheme);
  summary.signal_frame_mappings.insert(summary.signal_frame_mappings.end(), mappings.begin(), mappings.end());
}

}  // namespace

void set_route_source_perturbation_enabled(bool enabled) { g_route_source_perturbation_enabled = enabled; }
void set_skip_foundation_enabled(bool enabled) { g_skip_foundation_enabled = enabled; }
void set_foundation_only_enabled(bool enabled) { g_foundation_only_enabled = enabled; }

DatasetSummary run_compare_experiment(const std::string& dataset_file, const std::string& run_tag) {
  const std::string full_path = cfd::storage::resolve_dataset_input_path(dataset_file);
  cfd::utils::read_message(full_path);
  const MessageInfoVec original_infos = MESSAGE_INFO_VEC;
  const MessageInfoVec functional_infos = generate_functional_routes(original_infos);
  const SchemeAnalysisConfig on_demand_config{"on_demand_tmr", false, true, true, g_route_source_perturbation_enabled,
                                              false, SchemeAnalysisConfig::ClusterWcrtMode::OnDemand};
  const SchemeAnalysisConfig always_on_config{"always_on_tmr", true, true, false, false, false,
                                              SchemeAnalysisConfig::ClusterWcrtMode::AlwaysOnTmr};

  DatasetSummary summary;
  summary.dataset_tag = cfd::storage::dataset_tag_from_file(dataset_file);
  summary.config_tag = dataset_config_tag_from_dataset_tag(summary.dataset_tag);
  g_current_dataset_log_tag = summary.dataset_tag;
  log_stage("dataset", "begin");
  summary.has_homo_only_foundation = !g_skip_foundation_enabled;
  if (summary.has_homo_only_foundation) {
    summary.homo_only_foundation = build_foundation_quick_metrics(original_infos);
  }
  if (!g_foundation_only_enabled) {
    append_scheme_result(summary, functional_infos, on_demand_config, true);
    append_scheme_result(summary, functional_infos, always_on_config, false);
  }

  write_dataset_report(compare_report_path(run_tag, summary.dataset_tag), summary);
  log_stage("dataset", "done");
  g_current_dataset_log_tag.clear();
  return summary;
}

QuickCompareResult quick_compare_signal_set(const MessageInfoVec& signal_infos) {
  const MessageInfoVec saved_infos = MESSAGE_INFO_VEC;
  QuickCompareResult result;

  const MessageInfoVec functional_infos = generate_functional_routes(signal_infos);
  const SchemeAnalysisConfig on_demand_config{"on_demand_tmr", false, true, true, g_route_source_perturbation_enabled,
                                              false, SchemeAnalysisConfig::ClusterWcrtMode::OnDemand};
  const SchemeAnalysisConfig always_on_config{"always_on_tmr", true, true, false, false, false,
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

  ofs << "[foundation_summary]\n";
  ofs << "dataset\tconfig\tscheme\tbandwidth_utilization\tmax_wcrt_ms\ttotal_added_signal_copies\tschedulable\n";
  for (const auto& summary : dataset_summaries) {
    if (!summary.has_homo_only_foundation) {
      continue;
    }
    ofs << summary.dataset_tag << '\t' << summary.config_tag << "\thomo_only_foundation\t"
        << summary.homo_only_foundation.bandwidth_utilization << '\t' << summary.homo_only_foundation.max_wcrt_ms
        << '\t' << summary.homo_only_foundation.total_added_signal_copies << '\t'
        << (summary.homo_only_foundation.schedulable ? 1 : 0) << '\n';
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

std::string route_role_name(int comm_id) {
  if (comm_id == kPrimaryRouteCommId) {
    return "primary";
  }
  if (comm_id == kSecondMainRouteCommId) {
    return "second_main";
  }
  if (comm_id == kBackupRouteCommId) {
    return "backup";
  }
  return "comm_" + std::to_string(comm_id);
}

std::string join_ints(std::vector<int> values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());

  std::ostringstream oss;
  for (size_t i = 0; i < values.size(); ++i) {
    if (i > 0) {
      oss << ',';
    }
    oss << values[i];
  }
  return oss.str();
}

struct PackingPlanKey {
  std::string dataset;
  std::string scheme;
  MessageCode code = 0;
  int comm_id = 0;
  EcuId src_ecu = 0;
  EcuId dst_ecu = 0;
  int period_ms = 0;
  int deadline_ms = 0;
  int level = 0;
  int type = 0;

  bool operator<(const PackingPlanKey& other) const {
    return std::tie(dataset, scheme, code, comm_id, src_ecu, dst_ecu, period_ms, deadline_ms, level, type) <
           std::tie(other.dataset, other.scheme, other.code, other.comm_id, other.src_ecu, other.dst_ecu,
                    other.period_ms, other.deadline_ms, other.level, other.type);
  }
};

struct PackingPlanAggregate {
  int signal_copy_count = 0;
  int added_copy_count = 0;
  std::vector<int> frame_ids;
  std::vector<int> frame_priorities;
  std::vector<int> frame_payload_bytes;
};

void write_batch_packing_plan_summary(const std::string& run_tag,
                                      const std::vector<DatasetSummary>& dataset_summaries) {
  const std::string output_path = packing_plan_report_path(run_tag);
  std::ofstream ofs(output_path, std::ios::trunc);
  if (!ofs) {
    DEBUG_MSG_DEBUG1(std::cout, "鏃犳硶鍐欏叆绠楁硶浜岃甯ц仛鍚堟枃浠? ", output_path);
    return;
  }

  std::map<PackingPlanKey, PackingPlanAggregate> plan_by_route;
  for (const auto& summary : dataset_summaries) {
    for (const auto& row : summary.signal_frame_mappings) {
      PackingPlanKey key;
      key.dataset = row.dataset_tag;
      key.scheme = row.scheme_name;
      key.code = row.code;
      key.comm_id = row.comm_id;
      key.src_ecu = row.src_ecu;
      key.dst_ecu = row.dst_ecu;
      key.period_ms = row.signal_period_ms;
      key.deadline_ms = row.signal_deadline_ms;
      key.level = row.level;
      key.type = row.type;

      auto& aggregate = plan_by_route[key];
      aggregate.signal_copy_count += 1;
      aggregate.added_copy_count += row.is_added_copy != 0 ? 1 : 0;
      aggregate.frame_ids.push_back(static_cast<int>(row.frame_id));
      aggregate.frame_priorities.push_back(row.frame_priority);
      aggregate.frame_payload_bytes.push_back(row.frame_payload_bytes);
    }
  }

  ofs << "[packing_plan]\n";
  ofs << "dataset\tscheme\tcode\tcomm_id\troute_role\tsrc_ecu\tdst_ecu\tperiod_ms\tdeadline_ms\tlevel\ttype\t"
         "signal_copy_count\tadded_copy_count\tframe_ids\tframe_priorities\tframe_payload_bytes\n";

  for (const auto& [key, aggregate] : plan_by_route) {
    ofs << key.dataset << '\t' << key.scheme << '\t' << key.code << '\t' << key.comm_id << '\t'
        << route_role_name(key.comm_id) << '\t' << key.src_ecu << '\t' << key.dst_ecu << '\t' << key.period_ms
        << '\t' << key.deadline_ms << '\t' << key.level << '\t' << key.type << '\t'
        << aggregate.signal_copy_count << '\t' << aggregate.added_copy_count << '\t'
        << join_ints(aggregate.frame_ids) << '\t' << join_ints(aggregate.frame_priorities) << '\t'
        << join_ints(aggregate.frame_payload_bytes) << '\n';
  }
}

}  // namespace cfd::algorithm2
