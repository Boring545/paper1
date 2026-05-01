#include "retry.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <queue>
#include <random>
#include <utility>
#include <vector>

#include "normal.h"
#include "storage_layout.h"

namespace cfd::analysis::retry {

double EI_LEN = 0;  // 电磁干扰宽度，单位 ms

namespace {

constexpr double COST_MAX_ERROR_FRAME = 20.0 * TIME_BIT_ARBITRATION;  // 错误帧开销，约为仲裁段20bit时间
constexpr double EPS_RESPONSE = 1e-7;
constexpr int MAX_ENUMERATED_RETRY = 64;
struct AnalysisState {
  int retry_count = 0;
  double prev_response_time = 0.0;
  double response_time = 0.0;
  double probability = 1.0;
};

struct FrameContext {
  double max_hp_trans_time = 0.0;
  double max_lp_trans_time = 0.0;
  double single_error_cost = 0.0;
  double prune_epsilon = 1e-12;
};

double clamp_probability(double value) { return std::clamp(value, 0.0, 1.0); }

double multiply_probability_precise(double lhs, double rhs) {
  const long double product = static_cast<long double>(lhs) * static_cast<long double>(rhs);
  if (product <= 0.0L) {
    return 0.0;
  }
  if (product >= 1.0L) {
    return 1.0;
  }
  if (product < static_cast<long double>(std::numeric_limits<double>::min())) {
    return 0.0;
  }
  return static_cast<double>(product);
}

FrameContext build_frame_context(const std::vector<CanfdFrame>& sorted_frames, int frame_index) {
  FrameContext ctx;

  int max_level = 0;
  for (const auto& msg : sorted_frames[frame_index].msg_set) {
    max_level = std::max(max_level, msg.get_level());
  }

  for (int index = 0; index < static_cast<int>(sorted_frames.size()); ++index) {
    const double trans_time = sorted_frames[index].get_trans_time();
    if (index <= frame_index) {
      ctx.max_hp_trans_time = std::max(ctx.max_hp_trans_time, trans_time);
    } else {
      ctx.max_lp_trans_time = std::max(ctx.max_lp_trans_time, trans_time);
    }
  }

  (void)max_level;
  ctx.prune_epsilon = 1e-31;
  ctx.single_error_cost = COST_MAX_ERROR_FRAME + ctx.max_hp_trans_time;
  return ctx;
}

double calc_interference(const std::vector<CanfdFrame>& sorted_frames, int frame_index, double response_time,
                         double trans_time) {
  constexpr double jitter = 0.0;
  double interference = 0.0;

  for (int index = frame_index - 1; index >= 0; --index) {
    const double numerator = response_time - trans_time + TIME_BIT_ARBITRATION + jitter;
    const double release_num = std::max(0.0, std::ceil(numerator / sorted_frames[index].get_period()));
    interference += release_num * sorted_frames[index].get_trans_time();
  }

  return interference;
}

void add_retry_probability(std::vector<RetryDistributionPoint>& distribution, int retry_count, double probability) {
  if (probability <= 0.0) {
    return;
  }

  for (auto& point : distribution) {
    if (point.retry_count == retry_count) {
      point.probability += probability;
      return;
    }
  }

  distribution.push_back({retry_count, probability});
}

void add_response_probability(std::vector<ResponseDistributionPoint>& distribution, double response_time,
                              double probability) {
  if (probability <= 0.0) {
    return;
  }

  for (auto& point : distribution) {
    if (std::abs(point.response_time - response_time) < 1e-9) {
      point.probability += probability;
      return;
    }
  }

  distribution.push_back({response_time, probability});
}

void finalize_retry_distribution(std::vector<RetryDistributionPoint>& distribution) {
  std::sort(distribution.begin(), distribution.end(),
            [](const RetryDistributionPoint& lhs, const RetryDistributionPoint& rhs) {
              return lhs.retry_count < rhs.retry_count;
            });
}

void finalize_response_distribution(std::vector<ResponseDistributionPoint>& distribution) {
  std::sort(distribution.begin(), distribution.end(),
            [](const ResponseDistributionPoint& lhs, const ResponseDistributionPoint& rhs) {
              return lhs.response_time < rhs.response_time;
            });

  std::vector<ResponseDistributionPoint> merged;
  merged.reserve(distribution.size());
  for (const auto& point : distribution) {
    if (!merged.empty() && std::abs(merged.back().response_time - point.response_time) < 1e-9) {
      merged.back().probability += point.probability;
    } else {
      merged.push_back(point);
    }
  }

  distribution = std::move(merged);
}

double sum_retry_probability(const std::vector<RetryDistributionPoint>& distribution) {
  double total = 0.0;
  for (const auto& point : distribution) {
    total += point.probability;
  }
  return total;
}

double sum_response_probability(const std::vector<ResponseDistributionPoint>& distribution) {
  double total = 0.0;
  for (const auto& point : distribution) {
    total += point.probability;
  }
  return total;
}

double calc_expected_retry_count(const std::vector<RetryDistributionPoint>& distribution) {
  double expected = 0.0;
  for (const auto& point : distribution) {
    expected += static_cast<double>(point.retry_count) * point.probability;
  }
  return expected;
}

int calc_retry_quantile(const std::vector<RetryDistributionPoint>& distribution, double quantile) {
  if (distribution.empty()) {
    return 0;
  }

  const double target = clamp_probability(quantile);
  double cumulative = 0.0;
  for (const auto& point : distribution) {
    cumulative += point.probability;
    if (cumulative + 1e-12 >= target) {
      return point.retry_count;
    }
  }

  return distribution.back().retry_count;
}

double calc_retry_cdf(const std::vector<RetryDistributionPoint>& distribution, int retry_count) {
  double cumulative = 0.0;
  for (const auto& point : distribution) {
    if (point.retry_count > retry_count) {
      break;
    }
    cumulative += point.probability;
  }
  return clamp_probability(cumulative);
}

struct RetrySampler {
  double utilization_weight = 0.0;
  std::vector<std::pair<int, double>> retry_cdf;
};

int sample_retry_count(const RetrySampler& sampler, double random01) {
  for (const auto& [retry_count, cumulative_probability] : sampler.retry_cdf) {
    if (random01 <= cumulative_probability + 1e-15) {
      return retry_count;
    }
  }
  return sampler.retry_cdf.empty() ? 0 : sampler.retry_cdf.back().first;
}

double calc_system_bandwidth_quantile(const std::vector<CanfdFrame>& sorted_frames,
                                      const std::unordered_map<FrameId, FrameProbData>& frame_results,
                                      double quantile) {
  if (sorted_frames.empty()) {
    return 0.0;
  }

  std::vector<RetrySampler> samplers;
  samplers.reserve(sorted_frames.size());
  for (const auto& frame : sorted_frames) {
    const auto result_it = frame_results.find(frame.get_id());
    if (result_it == frame_results.end()) {
      continue;
    }

    RetrySampler sampler;
    sampler.utilization_weight = frame.get_trans_time() / static_cast<double>(frame.get_period());

    double cumulative = 0.0;
    for (const auto& point : result_it->second.retry_distribution) {
      cumulative += point.probability;
      sampler.retry_cdf.emplace_back(point.retry_count, clamp_probability(cumulative));
    }

    if (!sampler.retry_cdf.empty()) {
      sampler.retry_cdf.back().second = 1.0;
      samplers.push_back(std::move(sampler));
    }
  }

  if (samplers.empty()) {
    return 0.0;
  }

  constexpr int NUM_MONTE_CARLO_SAMPLES = 200000;
  std::mt19937_64 rng(20260407ULL);
  std::uniform_real_distribution<double> dist(0.0, 1.0);
  std::vector<double> total_utils;
  total_utils.reserve(NUM_MONTE_CARLO_SAMPLES);

  for (int sample_index = 0; sample_index < NUM_MONTE_CARLO_SAMPLES; ++sample_index) {
    double total_util = 0.0;
    for (const auto& sampler : samplers) {
      const int retry_count = sample_retry_count(sampler, dist(rng));
      total_util += sampler.utilization_weight * (1.0 + static_cast<double>(retry_count));
    }
    total_utils.push_back(total_util);
  }

  std::sort(total_utils.begin(), total_utils.end());
  const double target = clamp_probability(quantile);
  const size_t index = std::min(total_utils.size() - 1,
                                static_cast<size_t>(std::ceil(target * static_cast<double>(total_utils.size())) - 1.0));
  return total_utils[index];
}

double calc_expected_response_time(const std::vector<ResponseDistributionPoint>& distribution) {
  double expected = 0.0;
  for (const auto& point : distribution) {
    expected += point.response_time * point.probability;
  }
  return expected;
}

double calc_response_quantile(const std::vector<ResponseDistributionPoint>& distribution, double quantile) {
  if (distribution.empty()) {
    return 0.0;
  }

  const double target = clamp_probability(quantile);
  double cumulative = 0.0;
  for (const auto& point : distribution) {
    cumulative += point.probability;
    if (cumulative + 1e-12 >= target) {
      return point.response_time;
    }
  }

  return distribution.back().response_time;
}

double calc_response_cdf(const std::vector<ResponseDistributionPoint>& distribution, double response_time) {
  double cumulative = 0.0;
  for (const auto& point : distribution) {
    if (point.response_time > response_time + 1e-12) {
      break;
    }
    cumulative += point.probability;
  }
  return clamp_probability(cumulative);
}

void add_probability_gap(FrameProbData& result, const CanfdFrame& frame, double single_error_cost) {
  const double total_probability = sum_response_probability(result.wcrt_distribution);
  const double probability_gap = 1.0 - total_probability;
  if (probability_gap <= 0.0) {
    return;
  }

  const int conservative_retry =
      result.retry_distribution.empty() ? 1 : (result.retry_distribution.back().retry_count + 1);
  const double conservative_response =
      std::max(static_cast<double>(frame.get_deadline()) + single_error_cost,
               result.wcrt_distribution.empty() ? frame.get_trans_time()
                                                : (result.wcrt_distribution.back().response_time + single_error_cost));

  add_retry_probability(result.retry_distribution, conservative_retry, probability_gap);
  add_response_probability(result.wcrt_distribution, conservative_response, probability_gap);
}

double calc_response_time_with_fixed_retry(const std::vector<CanfdFrame>& sorted_frames, int frame_index,
                                           const FrameContext& ctx, int total_retry_count) {
  const CanfdFrame& frame = sorted_frames[frame_index];
  double response_time = ctx.max_lp_trans_time + frame.get_trans_time() + total_retry_count * ctx.single_error_cost;

  for (int iter = 0; iter < 256; ++iter) {
    const double interference = calc_interference(sorted_frames, frame_index, response_time, frame.get_trans_time());
    const double next_response_time =
        ctx.max_lp_trans_time + frame.get_trans_time() + interference + total_retry_count * ctx.single_error_cost;
    if (std::abs(next_response_time - response_time) < EPS_RESPONSE) {
      return next_response_time;
    }
    response_time = next_response_time;
  }

  return response_time;
}

double calc_timeout_probability_floor(const std::vector<CanfdFrame>& sorted_frames, int frame_index,
                                      const FrameContext& ctx) {
  const CanfdFrame& frame = sorted_frames[frame_index];
  int timeout_retry_threshold = -1;

  for (int retry_count = 0; retry_count <= 512; ++retry_count) {
    if (calc_response_time_with_fixed_retry(sorted_frames, frame_index, ctx, retry_count) > frame.get_deadline()) {
      timeout_retry_threshold = retry_count;
      break;
    }
  }

  if (timeout_retry_threshold < 0) {
    return 0.0;
  }

  const double rate = LAMBDA_CONFERENCE * std::max(0.0, EI_LEN + static_cast<double>(frame.get_deadline()));
  if (timeout_retry_threshold <= 0) {
    return 1.0;
  }

  double term = prob_fault(std::max(0.0, EI_LEN + static_cast<double>(frame.get_deadline())), timeout_retry_threshold);
  double tail = term;
  for (int retry_count = timeout_retry_threshold + 1; retry_count < timeout_retry_threshold + 4096; ++retry_count) {
    if (term <= 0.0) {
      break;
    }
    term *= rate / static_cast<double>(retry_count);
    tail += term;
    if (term < std::max(1e-300, tail * 1e-12)) {
      break;
    }
  }

  return clamp_probability(tail);
}

FrameProbData analyze_frame_probability(const std::vector<CanfdFrame>& sorted_frames, int frame_index) {
  const CanfdFrame& frame = sorted_frames[frame_index];
  const FrameContext ctx = build_frame_context(sorted_frames, frame_index);

  FrameProbData result;
  result.frame_id = frame.get_id();
  result.priority = frame.get_priority();
  result.period = frame.get_period();
  result.deadline = frame.get_deadline();
  result.trans_time = frame.get_trans_time();

  std::queue<AnalysisState> queue;
  queue.push({0, 0.0, frame.get_trans_time(), 1.0});

  while (!queue.empty()) {
    const AnalysisState state = queue.front();
    queue.pop();

    const double interference =
        calc_interference(sorted_frames, frame_index, state.response_time, frame.get_trans_time());
    const double fault_window = std::max(0.0, EI_LEN + state.response_time - state.prev_response_time);

    for (int retry_in_window = 0; retry_in_window <= MAX_ENUMERATED_RETRY; ++retry_in_window) {
      const double branch_probability = state.probability * prob_fault(fault_window, retry_in_window);
      if (branch_probability <= 0.0) {
        if (retry_in_window > 0) {
          break;
        }
        continue;
      }

      const int total_retry_count = state.retry_count + retry_in_window;
      const double next_response_time =
          ctx.max_lp_trans_time + frame.get_trans_time() + interference + total_retry_count * ctx.single_error_cost;

      const bool probability_too_small = branch_probability < ctx.prune_epsilon;
      const bool converged = std::abs(next_response_time - state.response_time) < EPS_RESPONSE;
      const bool timeout = next_response_time > frame.get_deadline();
      const bool retry_limit_reached = retry_in_window == MAX_ENUMERATED_RETRY;

      if (probability_too_small || converged || timeout || retry_limit_reached) {
        add_retry_probability(result.retry_distribution, total_retry_count, branch_probability);
        add_response_probability(result.wcrt_distribution, next_response_time, branch_probability);
      } else {
        queue.push({total_retry_count, state.response_time, next_response_time, branch_probability});
      }

      if (probability_too_small) {
        break;
      }
    }
  }

  finalize_retry_distribution(result.retry_distribution);
  finalize_response_distribution(result.wcrt_distribution);
  add_probability_gap(result, frame, ctx.single_error_cost);
  finalize_retry_distribution(result.retry_distribution);
  finalize_response_distribution(result.wcrt_distribution);

  result.expected_retry_count = calc_expected_retry_count(result.retry_distribution);
  result.expected_tx_count = 1.0 + result.expected_retry_count;
  result.rounded_expected_retry_count = static_cast<int>(std::ceil(result.expected_retry_count));
  result.rounded_expected_tx_count = 1.0 + static_cast<double>(result.rounded_expected_retry_count);
  result.retry_p99 = calc_retry_quantile(result.retry_distribution, 0.99);
  result.tx_count_p99 = 1.0 + static_cast<double>(result.retry_p99);
  result.expected_response_time = calc_expected_response_time(result.wcrt_distribution);
  result.wcrt_p95 = calc_response_quantile(result.wcrt_distribution, 0.95);

  double p_timeout = 0.0;
  for (const auto& point : result.wcrt_distribution) {
    if (point.response_time > frame.get_deadline()) {
      p_timeout += point.probability;
    }
  }
  const double timeout_floor = calc_timeout_probability_floor(sorted_frames, frame_index, ctx);
  // Only use tail compensation when it is above the current analysis resolution.
  // Otherwise the reported timeout probability would be dominated by an extremely
  // small floor that is far below the branch-pruning granularity.
  const double effective_timeout_floor = (timeout_floor >= ctx.prune_epsilon) ? timeout_floor : 0.0;
  result.p_timeout = clamp_probability(std::max(clamp_probability(p_timeout), effective_timeout_floor));

  return result;
}

std::string build_output_path(const std::string& timestamp) {
  return cfd::storage::normalize_retry_report_output_path(timestamp);
}

void save_prob_result(const std::string& address, const AnalysisReport& report) {
  std::ofstream ofs(address, std::ios::trunc);
  if (!ofs) {
    std::cerr << "Failed to open retry analysis output file\n";
    return;
  }

  ofs << std::setprecision(17);
  ofs << "base_bandwidth_utilization\t" << report.base_bandwidth_utilization << '\n';
  ofs << "expected_bandwidth_utilization\t" << report.expected_bandwidth_utilization << "\n\n";
  ofs << "rounded_expected_bandwidth_utilization\t" << report.rounded_expected_bandwidth_utilization << "\n\n";
  ofs << "bandwidth_utilization_p99\t" << report.bandwidth_utilization_p99 << "\n\n";

  std::vector<FrameId> frame_ids;
  frame_ids.reserve(report.frame_results.size());
  for (const auto& [frame_id, _] : report.frame_results) {
    frame_ids.push_back(frame_id);
  }
  std::sort(frame_ids.begin(), frame_ids.end());

  ofs << "[frame_summary]\n";
  ofs << "frame_id\tpriority\tperiod_ms\tdeadline_ms\ttrans_time_ms\texpected_retry_count\texpected_tx_count\t"
         "rounded_expected_retry_count\trounded_expected_tx_count\tretry_p99\ttx_count_p99\texpected_wcrt_ms\t"
         "p_timeout\twcrt_p95_ms\n";
  for (const auto frame_id : frame_ids) {
    const auto& frame = report.frame_results.at(frame_id);
    ofs << frame.frame_id << '\t' << frame.priority << '\t' << frame.period << '\t' << frame.deadline << '\t'
        << frame.trans_time << '\t' << frame.expected_retry_count << '\t' << frame.expected_tx_count << '\t'
        << frame.rounded_expected_retry_count << '\t' << frame.rounded_expected_tx_count << '\t' << frame.retry_p99
        << '\t' << frame.tx_count_p99 << '\t' << frame.expected_response_time << '\t' << frame.p_timeout << '\t'
        << frame.wcrt_p95 << '\n';
  }

  ofs << "\n[frame_retry_distribution]\n";
  ofs << "frame_id\tretry_count\tprobability\n";
  for (const auto frame_id : frame_ids) {
    const auto& frame = report.frame_results.at(frame_id);
    for (const auto& point : frame.retry_distribution) {
      ofs << frame_id << '\t' << point.retry_count << '\t' << point.probability << '\n';
    }
  }

  ofs << "\n[frame_retry_cdf]\n";
  ofs << "frame_id\tretry_count\tcumulative_probability\n";
  for (const auto frame_id : frame_ids) {
    const auto& frame = report.frame_results.at(frame_id);
    for (const auto& point : frame.retry_distribution) {
      ofs << frame_id << '\t' << point.retry_count << '\t'
          << calc_retry_cdf(frame.retry_distribution, point.retry_count) << '\n';
    }
  }

  ofs << "\n[frame_wcrt_distribution]\n";
  ofs << "frame_id\tresponse_time_ms\tprobability\n";
  for (const auto frame_id : frame_ids) {
    const auto& frame = report.frame_results.at(frame_id);
    for (const auto& point : frame.wcrt_distribution) {
      ofs << frame_id << '\t' << point.response_time << '\t' << point.probability << '\n';
    }
  }

  ofs << "\n[frame_wcrt_cdf]\n";
  ofs << "frame_id\tresponse_time_ms\tcumulative_probability\n";
  for (const auto frame_id : frame_ids) {
    const auto& frame = report.frame_results.at(frame_id);
    for (const auto& point : frame.wcrt_distribution) {
      ofs << frame_id << '\t' << point.response_time << '\t'
          << calc_response_cdf(frame.wcrt_distribution, point.response_time) << '\n';
    }
  }

  ofs << "\n[frame_distribution_quantiles]\n";
  ofs << "frame_id\tretry_q50\tretry_q90\tretry_q95\tretry_q99\twcrt_q50_ms\twcrt_q90_ms\twcrt_q95_ms\twcrt_q99_ms\n";
  for (const auto frame_id : frame_ids) {
    const auto& frame = report.frame_results.at(frame_id);
    ofs << frame_id << '\t' << calc_retry_quantile(frame.retry_distribution, 0.50) << '\t'
        << calc_retry_quantile(frame.retry_distribution, 0.90) << '\t'
        << calc_retry_quantile(frame.retry_distribution, 0.95) << '\t'
        << calc_retry_quantile(frame.retry_distribution, 0.99) << '\t'
        << calc_response_quantile(frame.wcrt_distribution, 0.50) << '\t'
        << calc_response_quantile(frame.wcrt_distribution, 0.90) << '\t'
        << calc_response_quantile(frame.wcrt_distribution, 0.95) << '\t'
        << calc_response_quantile(frame.wcrt_distribution, 0.99) << '\n';
  }

  std::vector<MessageCode> codes;
  codes.reserve(report.signal_results.size());
  for (const auto& [code, _] : report.signal_results) {
    codes.push_back(code);
  }
  std::sort(codes.begin(), codes.end());

  ofs << "\n[signal_summary]\n";
  ofs << "code\tframe_id\tperiod_ms\tlevel\ttype\tp_timeout\texpected_wcrt_ms\twcrt_p95_ms\t"
         "expected_retry_count\tp_threshold\n";
  for (const auto code : codes) {
    const auto& signal = report.signal_results.at(code);
    ofs << code << '\t' << signal.frame_id << '\t' << signal.period << '\t' << signal.level << '\t' << signal.type
        << '\t' << signal.p_timeout << '\t' << signal.expected_wcrt << '\t' << signal.wcrt_p95 << '\t'
        << signal.expected_retry_count << '\t' << signal.p_threshold << '\n';
  }
}

}  // namespace

AnalysisReport probabilistic_analysis_report(PackingScheme& scheme, std::string timestamp) {
  std::vector<CanfdFrame> sorted_frames;
  sorted_frames.reserve(scheme.frame_map.size());
  for (const auto& [id, frame] : scheme.frame_map) {
    if (!frame.empty()) {
      sorted_frames.push_back(frame);
    }
  }

  std::sort(sorted_frames.begin(), sorted_frames.end(),
            [](const CanfdFrame& lhs, const CanfdFrame& rhs) { return lhs.get_priority() < rhs.get_priority(); });

  AnalysisReport report;
  report.base_bandwidth_utilization = scheme.calc_bandwidth_utilization();

  for (int frame_index = 0; frame_index < static_cast<int>(sorted_frames.size()); ++frame_index) {
    const CanfdFrame& frame = sorted_frames[frame_index];
    FrameProbData frame_result = analyze_frame_probability(sorted_frames, frame_index);

    report.expected_bandwidth_utilization +=
        frame.get_trans_time() * frame_result.expected_tx_count / static_cast<double>(frame.get_period());
    report.rounded_expected_bandwidth_utilization +=
        frame.get_trans_time() * frame_result.rounded_expected_tx_count / static_cast<double>(frame.get_period());
    report.frame_results[frame.get_id()] = frame_result;

    DEBUG_MSG_DEBUG2(std::cout, cfd::utils::get_frame_string(frame));
    DEBUG_MSG_DEBUG2(std::cout, "frame=", frame.get_id(), " p_timeout=", frame_result.p_timeout,
                     " expected_retry_count=", frame_result.expected_retry_count, "\n");

    for (const auto& msg : frame.msg_set) {
      const auto code = msg.get_code();
      auto signal_it = report.signal_results.find(code);
      if (signal_it == report.signal_results.end()) {
        report.signal_results[code] = {msg.get_id_message(),   frame.get_id(),
                                       msg.get_period(),       msg.get_level(),
                                       msg.get_type(),         threshold_per_window(msg.get_level(), msg.get_period()),
                                       frame_result.p_timeout, frame_result.expected_response_time,
                                       frame_result.wcrt_p95,  frame_result.expected_retry_count};
      } else {
        signal_it->second.p_timeout = multiply_probability_precise(signal_it->second.p_timeout, frame_result.p_timeout);
        if (frame_result.wcrt_p95 < signal_it->second.wcrt_p95) {
          signal_it->second.frame_id = frame.get_id();
          signal_it->second.expected_wcrt = frame_result.expected_response_time;
          signal_it->second.wcrt_p95 = frame_result.wcrt_p95;
          signal_it->second.expected_retry_count = frame_result.expected_retry_count;
        }
      }
    }
  }

  report.bandwidth_utilization_p99 = calc_system_bandwidth_quantile(sorted_frames, report.frame_results, 0.99);

  save_prob_result(build_output_path(timestamp), report);
  return report;
}

std::unordered_map<MessageCode, ProbData> probabilistic_analysis(PackingScheme& scheme, std::string timestamp) {
  return probabilistic_analysis_report(scheme, std::move(timestamp)).signal_results;
}

}  // namespace cfd::analysis::retry
