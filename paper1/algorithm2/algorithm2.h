#pragma once

#include <string>
#include <vector>

#include "../canfd_frame.h"
#include "../scheme.h"

namespace cfd::algorithm2 {

void set_route_source_perturbation_enabled(bool enabled);
void set_skip_foundation_enabled(bool enabled);
void set_foundation_only_enabled(bool enabled);

struct RouteMetric {
  MessageCode code = 0;
  int comm_id = 0;
  int level = 0;
  int type = 0;
  EcuId src_ecu = 0;
  EcuId dst_ecu = 0;
  int period_ms = 0;
  int deadline_ms = 0;
  int signal_instance_count = 1;
  int added_signal_copies = 0;
  double p_comm_fault = 0.0;
  double p_threshold = 0.0;
  double max_wcrt_ms = 0.0;
};

struct ClusterMetric {
  MessageCode code = 0;
  int route_count = 0;
  double p_function_fault = 0.0;
  double p_threshold = 0.0;
};

struct ClusterWcrtMetric {
  MessageCode code = 0;
  int period_ms = 0;
  int deadline_ms = 0;
  double normal_main_route_wcrt_ms = 0.0;
  double fault_main_route_wcrt_ms = 0.0;
  double tmr_all_route_wcrt_ms = 0.0;
  double activation_wcrt_ms = 0.0;
  double backup_route_wcrt_ms = 0.0;
  double compare_time_ms = 0.0;
  double backup_compute_time_ms = 0.0;
  double end_to_end_normal_wcrt_ms = 0.0;
  double end_to_end_fault_wcrt_ms = 0.0;
  bool meets_deadline = false;
};

struct SchemeMetrics {
  std::string name;
  double normal_bandwidth_utilization = 0.0;
  double fault_bandwidth_utilization = 0.0;
  int total_added_signal_copies = 0;
  bool schedulable = false;
  std::vector<RouteMetric> routes;
  std::vector<ClusterMetric> clusters;
  std::vector<ClusterWcrtMetric> cluster_wcrts;
};

struct SignalFrameMappingRow {
  std::string dataset_tag;
  std::string scheme_name;
  MessageCode code = 0;
  int comm_id = 0;
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

struct FoundationQuickMetrics {
  double bandwidth_utilization = 0.0;
  double max_wcrt_ms = 0.0;
  int total_added_signal_copies = 0;
  bool schedulable = false;
};

struct DatasetSummary {
  std::string dataset_tag;
  std::string config_tag;
  FoundationQuickMetrics homo_only_foundation;
  bool has_homo_only_foundation = true;
  std::vector<SchemeMetrics> schemes;
  std::vector<SignalFrameMappingRow> signal_frame_mappings;
};

struct QuickCompareResult {
  FoundationQuickMetrics homo_only_foundation;
  SchemeMetrics on_demand_tmr;
  SchemeMetrics always_on_tmr;
  double on_demand_fault_periodic_bandwidth_utilization = 0.0;
  double on_demand_max_normal_end_to_end_wcrt_ms = 0.0;
  double on_demand_max_fault_end_to_end_wcrt_ms = 0.0;
  double always_on_end_to_end_wcrt_ms = 0.0;
};

DatasetSummary run_compare_experiment(const std::string& dataset_file, const std::string& run_tag);

QuickCompareResult quick_compare_signal_set(const MessageInfoVec& signal_infos);

QuickCompareResult quick_compare_dataset_file(const std::string& dataset_file);

void write_batch_summary(const std::string& run_tag, const std::vector<DatasetSummary>& dataset_summaries);

void write_batch_signal_frame_mapping_summary(const std::string& run_tag,
                                              const std::vector<DatasetSummary>& dataset_summaries);

}  // namespace cfd::algorithm2
