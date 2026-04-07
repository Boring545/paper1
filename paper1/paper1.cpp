// paper1.cpp: 定义应用程序的入口点。

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <string>
#include <unordered_map>
#include <vector>

#include "backups/frame_backup.h"
#include "backups/signal_backup.h"
#include "config.h"
#include "debug_tool.h"
#include "probabilistic_analysis/no_retry.h"
#include "probabilistic_analysis/retry.h"
#include "scheme.h"

#ifdef _WIN32
extern "C" __declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int);
extern "C" __declspec(dllimport) int __stdcall SetConsoleCP(unsigned int);
#endif

#ifndef CP_UTF8
#define CP_UTF8 65001
#endif

using namespace cfd;
using namespace cfd::analysis;

namespace {

struct CodeMeta {
  int level = 0;
  int period_ms = 0;
  int type = 0;
  EcuId src_ecu = 0;
  EcuId dst_ecu = 0;
};

struct SignalMetric {
  MessageCode code = 0;
  CodeMeta meta;
  double p_fault = 0.0;
  double p_threshold = 0.0;
  double p_wcrt_over_deadline = 0.0;
  double expected_wcrt_ms = 0.0;
  double wcrt_p95_ms = 0.0;
  double wcrt_p99_ms = 0.0;
  double wcrt_ratio_p95 = 0.0;
  double wcrt_ratio_p99 = 0.0;
};

struct SchemeMetrics {
  std::string name;
  double compare_bandwidth_utilization = 0.0;
  double static_bandwidth_utilization = 0.0;
  std::vector<SignalMetric> signals;
};

struct AsilStats {
  int count = 0;
  double fault_sum = 0.0;
  double fault_max = 0.0;
  double wcrt_deadline_miss_sum = 0.0;
  double wcrt_deadline_miss_max = 0.0;
  double wcrt_ratio_p95_sum = 0.0;
  double wcrt_ratio_p95_max = 0.0;
  double wcrt_ratio_p99_sum = 0.0;
  double wcrt_ratio_p99_max = 0.0;
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

double calc_ratio(double wcrt_ms, int period_ms) {
  if (period_ms <= 0) {
    return 0.0;
  }
  return wcrt_ms / static_cast<double>(period_ms);
}

std::unordered_map<MessageCode, CodeMeta> build_code_meta(const MessageInfoVec& original_infos) {
  std::unordered_map<MessageCode, CodeMeta> meta_by_code;
  meta_by_code.reserve(original_infos.size());

  for (const auto& info : original_infos) {
    auto it = meta_by_code.find(info.code);
    if (it == meta_by_code.end()) {
      meta_by_code.emplace(info.code, CodeMeta{info.level, info.period, info.type, info.ecu_pair.src_ecu,
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
std::string create_msg() {
  cfd::utils::generate_msg_info_set();
  const auto ts = cfd::utils::store_msg(cfd::TEST_INFO_PATH);
  DEBUG_MSG_DEBUG1(std::cout, "生成信号集合,数量：", cfd::SIZE_ORIGINAL_MESSAGE);
  DEBUG_MSG_DEBUG1(std::cout, "已生成信号集合, 时间戳: ", ts);
  return ts;
}

// 读取某个信号集合
void read_data_1() {
  cfd::utils::read_message(cfd::TEST_INFO_PATH + cfd::DEFAULT_MSG_FILE);
  DEBUG_MSG_DEBUG1(std::cout, "读取信号集合: ", cfd::DEFAULT_MSG_FILE);
}

// 基于当前 MESSAGE_INFO_VEC 构建并优化打包方案
PackingScheme build_scheme_from_current_msgs() {
  PackingScheme scheme{};
  cfd::packing::frame_pack(scheme, cfd::DEFAULT_PACK_METHOD);
  return scheme;
}

std::unordered_map<MessageCode, double> calc_best_signal_wcrt_ms(const PackingScheme& scheme) {
  std::unordered_map<MessageCode, double> best_wcrt_ms;
  best_wcrt_ms.reserve(MESSAGE_INFO_VEC.size());

  for (const auto& [frame_id, frame] : scheme.frame_map) {
    if (frame.empty()) {
      continue;
    }

    const double frame_wcrt_ms = frame.get_trans_time();
    for (const auto& msg : frame.msg_set) {
      auto it = best_wcrt_ms.find(msg.get_code());
      if (it == best_wcrt_ms.end()) {
        best_wcrt_ms.emplace(msg.get_code(), frame_wcrt_ms);
      } else {
        it->second = std::min(it->second, frame_wcrt_ms);
      }
    }
  }

  return best_wcrt_ms;
}

SchemeMetrics analyze_deterministic_scheme(const std::string& name, PackingScheme& scheme,
                                           const std::unordered_map<MessageCode, CodeMeta>& meta_by_code) {
  SchemeMetrics metrics;
  metrics.name = name;
  metrics.compare_bandwidth_utilization = scheme.calc_bandwidth_utilization();
  metrics.static_bandwidth_utilization = metrics.compare_bandwidth_utilization;

  const auto p_fault_map = noretry::sig_trans_fault_prob_analysis(scheme, LAMBDA_CONFERENCE);
  const auto best_wcrt_ms = calc_best_signal_wcrt_ms(scheme);
  const auto codes = sorted_codes(meta_by_code);

  metrics.signals.reserve(codes.size());
  for (const auto code : codes) {
    const auto meta_it = meta_by_code.find(code);
    if (meta_it == meta_by_code.end()) {
      continue;
    }

    const auto p_fault_it = p_fault_map.find(code);
    const auto wcrt_it = best_wcrt_ms.find(code);
    if (p_fault_it == p_fault_map.end() || wcrt_it == best_wcrt_ms.end()) {
      continue;
    }

    const auto& meta = meta_it->second;
    const double wcrt_ms = wcrt_it->second;
    metrics.signals.push_back(
        {code, meta, p_fault_it->second, threshold_per_window(meta.level, meta.period_ms), p_fault_it->second,
         wcrt_ms, wcrt_ms, wcrt_ms,
         calc_ratio(wcrt_ms, meta.period_ms), calc_ratio(wcrt_ms, meta.period_ms)});
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
    metrics.signals.push_back(
        {code, meta, signal.p_timeout, signal.p_threshold, signal.p_timeout, signal.expected_wcrt, signal.wcrt_p95,
         signal.wcrt_p99,
         calc_ratio(signal.wcrt_p95, meta.period_ms), calc_ratio(signal.wcrt_p99, meta.period_ms)});
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
    level_stats.fault_sum += signal.p_fault;
    level_stats.fault_max = std::max(level_stats.fault_max, signal.p_fault);
    level_stats.wcrt_deadline_miss_sum += signal.p_wcrt_over_deadline;
    level_stats.wcrt_deadline_miss_max = std::max(level_stats.wcrt_deadline_miss_max, signal.p_wcrt_over_deadline);
    level_stats.wcrt_ratio_p95_sum += signal.wcrt_ratio_p95;
    level_stats.wcrt_ratio_p95_max = std::max(level_stats.wcrt_ratio_p95_max, signal.wcrt_ratio_p95);
    level_stats.wcrt_ratio_p99_sum += signal.wcrt_ratio_p99;
    level_stats.wcrt_ratio_p99_max = std::max(level_stats.wcrt_ratio_p99_max, signal.wcrt_ratio_p99);
  }

  return stats;
}

std::string build_compare_output_path(const std::string& timestamp) {
  return TEST_INFO_PATH + "compare_methods_" + timestamp + ".txt";
}

std::string build_retry_output_path(const std::string& timestamp) {
  return TEST_INFO_PATH + "retry_analysis_" + timestamp + ".txt";
}

void write_comparison_report(const std::string& output_path, const std::vector<SchemeMetrics>& schemes,
                             const std::string& retry_report_path) {
  std::ofstream ofs(output_path, std::ios::trunc);
  if (!ofs) {
    DEBUG_MSG_DEBUG1(std::cout, "无法写入对比结果文件: ", output_path);
    return;
  }

  ofs << std::setprecision(17);
  ofs << "retry_distribution_report\t" << retry_report_path << "\n\n";

  ofs << "[scheme_summary]\n";
  ofs << "scheme\tcompare_bandwidth_utilization\tstatic_bandwidth_utilization\tsignal_count\n";
  for (const auto& scheme : schemes) {
    ofs << scheme.name << '\t' << scheme.compare_bandwidth_utilization << '\t' << scheme.static_bandwidth_utilization
        << '\t' << scheme.signals.size() << '\n';
  }

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

  ofs << "\n[asil_wcrt_ratio_p99]\n";
  ofs << "scheme\tasil\tsignal_count\tavg_wcrt_ratio_p99\tmax_wcrt_ratio_p99\n";
  for (const auto& scheme : schemes) {
    const auto stats = calc_asil_stats(scheme);
    for (int level = 0; level < NUM_MESSAGE_LEVEL; ++level) {
      const auto& item = stats[level];
      if (item.count <= 0) {
        continue;
      }
      ofs << scheme.name << '\t' << level_to_asil(level) << '\t' << item.count << '\t'
          << (item.wcrt_ratio_p99_sum / item.count) << '\t' << item.wcrt_ratio_p99_max << '\n';
    }
  }

  ofs << "\n[asil_fault_probability]\n";
  ofs << "scheme\tasil\tsignal_count\tavg_fault_probability\tmax_fault_probability\n";
  for (const auto& scheme : schemes) {
    const auto stats = calc_asil_stats(scheme);
    for (int level = 0; level < NUM_MESSAGE_LEVEL; ++level) {
      const auto& item = stats[level];
      if (item.count <= 0) {
        continue;
      }
      ofs << scheme.name << '\t' << level_to_asil(level) << '\t' << item.count << '\t'
          << (item.fault_sum / item.count) << '\t' << item.fault_max << '\n';
    }
  }

  ofs << "\n[asil_wcrt_deadline_miss_probability]\n";
  ofs << "scheme\tasil\tsignal_count\tavg_wcrt_deadline_miss_probability\tmax_wcrt_deadline_miss_probability\n";
  for (const auto& scheme : schemes) {
    const auto stats = calc_asil_stats(scheme);
    for (int level = 0; level < NUM_MESSAGE_LEVEL; ++level) {
      const auto& item = stats[level];
      if (item.count <= 0) {
        continue;
      }
      ofs << scheme.name << '\t' << level_to_asil(level) << '\t' << item.count << '\t'
          << (item.wcrt_deadline_miss_sum / item.count) << '\t' << item.wcrt_deadline_miss_max << '\n';
    }
  }

  ofs << "\n[signal_detail]\n";
  ofs << "scheme\tcode\tasil\tlevel\ttype\tperiod_ms\tsrc_ecu\tdst_ecu\tp_fault\tp_threshold\texpected_wcrt_ms\t"
         "p_wcrt_over_deadline\twcrt_p95_ms\twcrt_p99_ms\twcrt_ratio_p95\twcrt_ratio_p99\n";
  for (const auto& scheme : schemes) {
    for (const auto& signal : scheme.signals) {
      ofs << scheme.name << '\t' << signal.code << '\t' << level_to_asil(signal.meta.level) << '\t'
          << signal.meta.level << '\t' << signal.meta.type << '\t' << signal.meta.period_ms << '\t'
          << signal.meta.src_ecu << '\t' << signal.meta.dst_ecu << '\t' << signal.p_fault << '\t'
          << signal.p_threshold << '\t' << signal.expected_wcrt_ms << '\t' << signal.p_wcrt_over_deadline << '\t'
          << signal.wcrt_p95_ms << '\t'
          << signal.wcrt_p99_ms << '\t' << signal.wcrt_ratio_p95 << '\t' << signal.wcrt_ratio_p99 << '\n';
    }
  }
}

void run_compare_experiment() {
  const MessageInfoVec original_infos = MESSAGE_INFO_VEC;
  const auto meta_by_code = build_code_meta(original_infos);
  const std::string timestamp = get_time_stamp();

  // 基础方法：先做信号同源备份，再对要求异源冗余的信号补异源副本。
  MESSAGE_INFO_VEC = original_infos;
  PackingScheme scheme_base = build_scheme_from_current_msgs();
  scheme_base = backups::signal::homo_signal_backup(scheme_base);
  scheme_base = backups::signal::hetero_signal_backup(scheme_base);
  SchemeMetrics foundation_metrics = analyze_deterministic_scheme("foundation", scheme_base, meta_by_code);

  // baseline1：按帧故障概率直接生成报文副本。
  MESSAGE_INFO_VEC = original_infos;
  PackingScheme scheme_baseline1 = build_scheme_from_current_msgs();
  scheme_baseline1 = backups::frame::homo_frame_backup(scheme_baseline1);
  SchemeMetrics baseline1_metrics = analyze_deterministic_scheme("baseline1", scheme_baseline1, meta_by_code);

  // baseline2：ASIL B/C 生成 1 个报文副本，ASIL D 生成 2 个报文副本。
  MESSAGE_INFO_VEC = original_infos;
  PackingScheme scheme_baseline2 = build_scheme_from_current_msgs();
  scheme_baseline2 = backups::frame::homo_frame_backup_method2(scheme_baseline2);
  SchemeMetrics baseline2_metrics = analyze_deterministic_scheme("baseline2", scheme_baseline2, meta_by_code);

  // baseline3：保留重传，并输出每个帧的重传分布与 WCRT 分布。
  MESSAGE_INFO_VEC = original_infos;
  PackingScheme scheme_baseline3 = build_scheme_from_current_msgs();
  retry::AnalysisReport retry_report = retry::probabilistic_analysis_report(scheme_baseline3, timestamp);
  SchemeMetrics baseline3_metrics = analyze_retry_scheme("baseline3", retry_report, meta_by_code);

  const std::string compare_output_path = build_compare_output_path(timestamp);
  const std::string retry_output_path = build_retry_output_path(timestamp);
  write_comparison_report(compare_output_path,
                          {foundation_metrics, baseline1_metrics, baseline2_metrics, baseline3_metrics},
                          retry_output_path);

  DEBUG_MSG_DEBUG1(std::cout, "对比结果已输出: ", compare_output_path);
  DEBUG_MSG_DEBUG1(std::cout, "baseline3 分布结果已输出: ", retry_output_path);
}

}  // namespace

int main() {
#ifdef _WIN32
  // 让 Windows 控制台按 UTF-8 显示
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
#endif

  read_data_1();
  // create_msg();
  run_compare_experiment();

  return 0;
}
