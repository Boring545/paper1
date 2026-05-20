#include "signal_backup.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "priority_allocation.h"
#include "probabilistic_analysis/no_retry.h"
namespace cfd::backups::signal {
namespace {
constexpr int MAX_ITER = 50;
constexpr bool CHECK_SCHED_DURING_BACKUP = false;

struct BackupCandidate {
  MessageID message_index = 0;
  MessageCode code = 0;
  int level = 0;
  int period = 0;
  int round = 0;
};

// 构建 code -> message_info 索引映射（取首次出现）
std::unordered_map<MessageCode, size_t> build_code_index_map() {
  std::unordered_map<MessageCode, size_t> map;
  map.reserve(MESSAGE_INFO_VEC.size());
  for (size_t i = 0; i < MESSAGE_INFO_VEC.size(); ++i) {
    auto code = MESSAGE_INFO_VEC[i].code;
    if (map.find(code) == map.end()) {
      map.emplace(code, i);
    }
  }
  return map;
}

// 估算需要新增的同源副本数量
int calc_required_backup_num(double p_total, double threshold) {
  if (p_total <= 0.0) return 0;
  if (p_total <= threshold) return 0;
  if (p_total >= 1.0) return 1;

  double log_p_total = std::log(p_total);
  double log_p_thresh = std::log(threshold);
  if (log_p_total >= 0.0) return 1;

  int total_required = static_cast<int>(std::ceil(log_p_thresh / log_p_total));
  int backup_num = std::max(0, total_required - 1);
  return backup_num;
}

std::vector<BackupCandidate> build_homo_backup_candidates(
    PackingScheme& scheme, double lambda, const std::unordered_map<MessageCode, size_t>& code_index_map) {
  std::vector<BackupCandidate> candidates;
  const auto result = analysis::noretry::sig_trans_fault_prob_analysis(scheme, lambda);

  for (const auto& [code, p_total] : result) {
    const auto it = code_index_map.find(code);
    if (it == code_index_map.end()) continue;

    const size_t idx = it->second;
    const auto& info = MESSAGE_INFO_VEC[idx];
    const int level = info.level;
    if (level <= 0 || level >= NUM_MESSAGE_LEVEL) continue;

    const double threshold = analysis::threshold_per_window(level, info.period);
    const int backup_num = calc_required_backup_num(p_total, threshold);
    if (backup_num <= 0) continue;

    for (int round = 1; round <= backup_num; ++round) {
      candidates.push_back({idx, code, level, info.period, round});
    }
  }

  std::sort(candidates.begin(), candidates.end(), [](const BackupCandidate& lhs, const BackupCandidate& rhs) {
    if (lhs.level != rhs.level) return lhs.level > rhs.level;
    if (lhs.round != rhs.round) return lhs.round < rhs.round;
    if (lhs.period != rhs.period) return lhs.period > rhs.period;
    return lhs.code < rhs.code;
  });

  return candidates;
}

bool is_scheme_acceptable(const PackingScheme& scheme) {
  if (scheme.calc_bandwidth_utilization() > 1.0) return false;
  if constexpr (CHECK_SCHED_DURING_BACKUP) {
    return cfd::schedule::feasibility_check(scheme.frame_map);
  }
  return true;
}

bool try_apply_homo_backup_all(const PackingScheme& base_scheme, const std::vector<BackupCandidate>& candidates,
                               PackingScheme& out_scheme) {
  if (candidates.empty()) return false;

  PackingScheme trial = base_scheme;
  for (const auto& candidate : candidates) {
    trial.message_set.emplace_back(candidate.message_index);
  }

  if (!trial.re_init_frames()) {
    return false;
  }

  auto sa = cfd::packing::PACK_METHOD::SIMULATED_ANNEALING;
  cfd::packing::frame_pack(trial, sa);

  if (!is_scheme_acceptable(trial)) {
    return false;
  }

  out_scheme = std::move(trial);
  return true;
}

// 统计每个 ECU 的带宽利用率（按源 ECU）
std::unordered_map<EcuId, double> calc_ecu_utilization(const PackingScheme& scheme) {
  std::unordered_map<EcuId, double> util;
  util.reserve(NUM_ECU);
  for (int e : OPTION_ECU) {
    util[static_cast<EcuId>(e)] = 0.0;
  }
  for (const auto& [id, frame] : scheme.frame_map) {
    if (frame.empty()) continue;
    auto src = frame.get_ecu_pair().src_ecu;
    util[src] += frame.get_trans_time() / frame.get_period();
  }
  return util;
}

// 尝试将消息插入现有报文
bool try_insert_into_existing_frames(PackingScheme& scheme, Message& msg) {
  for (auto& [fid, frame] : scheme.frame_map) {
    if (frame.empty()) continue;
    if (!(frame.get_ecu_pair() == msg.get_ecu_pair())) continue;
    if (msg.get_period() % frame.get_period() != 0) continue;
    if (msg.get_data_size() > frame.get_free_size()) continue;
    if (frame.add_message(msg)) {
      return true;
    }
  }
  return false;
}

// 选择带宽利用率最低的 ECU 作为备份源
std::vector<EcuId> pick_backup_ecus(const std::vector<EcuId>& candidates, const std::unordered_map<EcuId, double>& util,
                                    int need) {
  std::vector<EcuId> sorted = candidates;
  std::sort(sorted.begin(), sorted.end(), [&](EcuId a, EcuId b) {
    double ua = util.count(a) ? util.at(a) : 0.0;
    double ub = util.count(b) ? util.at(b) : 0.0;
    if (ua != ub) return ua < ub;
    return a < b;
  });
  if (need < 0) need = 0;
  if (need > static_cast<int>(sorted.size())) need = static_cast<int>(sorted.size());
  return std::vector<EcuId>(sorted.begin(), sorted.begin() + need);
}
}  // namespace

// 同源备份
PackingScheme homo_signal_backup(PackingScheme& scheme, double lambda) {
  PackingScheme working = scheme;
  auto code_index_map = build_code_index_map();

  for (int iter = 0; iter < MAX_ITER; ++iter) {
    auto candidates = build_homo_backup_candidates(working, lambda, code_index_map);
    if (candidates.empty()) {
      return working;
    }

    DEBUG_MSG_DEBUG1(std::cout, "本轮同源备份候选数量: ", static_cast<int>(candidates.size()));

    PackingScheme feasible_scheme = working;
    DEBUG_MSG_DEBUG1(std::cout, "try applying all homo backup candidates: ", static_cast<int>(candidates.size()));

    if (!try_apply_homo_backup_all(working, candidates, feasible_scheme)) {
      DEBUG_MSG_DEBUG1(std::cout, "all homo backup candidates rejected; keep previous scheme");
      return working;
    }

#if 0
    bool found_feasible_prefix = false;
    size_t keep_count = candidates.size();

    while (keep_count > 0) {
      DEBUG_MSG_DEBUG1(std::cout, "尝试保留前缀候选数量: ", static_cast<int>(keep_count));
      if (try_apply_homo_backup_prefix(working, candidates, keep_count, feasible_scheme)) {
        found_feasible_prefix = true;
        break;
      }
      keep_count = shrink_keep_count(keep_count);
    }

    if (!found_feasible_prefix) {
      DEBUG_MSG_DEBUG1(std::cout, "所有正长度前缀均不可行，返回上一轮可行解");
      return working;
    }

#endif

    const int added_count = static_cast<int>(feasible_scheme.message_set.size() - working.message_set.size());
    DEBUG_MSG_DEBUG1(std::cout, "本轮接受同源备份数量: ", added_count);
    if (added_count <= 0) {
      return working;
    }

    working = std::move(feasible_scheme);
  }

  DEBUG_MSG_DEBUG1(std::cout, "达到最大迭代次数，返回当前可行解");
  return working;
}

// 异源备份：N模冗余（N为奇数）
PackingScheme hetero_signal_backup(PackingScheme& scheme, int redundancy_n, double lambda) {
  (void)lambda;  // 当前异源备份流程不依赖lambda

  // N模冗余要求：N为奇数且>=3
  if (redundancy_n < 3 || (redundancy_n % 2) == 0) {
    DEBUG_MSG_DEBUG1(std::cout, "N模冗余参数非法，N应为奇数且>=3");
    return scheme;
  }

  size_t origin_info_size = MESSAGE_INFO_VEC.size();
  PackingScheme base_scheme = scheme;
  PackingScheme working = scheme;

  auto util = calc_ecu_utilization(working);
  std::unordered_set<MessageCode> processed;
  processed.reserve(MESSAGE_INFO_VEC.size());

  bool any_added = false;
  bool need_repack = false;

  for (size_t i = 0; i < origin_info_size; ++i) {
    const MessageInfo info = MESSAGE_INFO_VEC[i];
    if (info.type != 1) continue;  // 仅处理需要异源备份的原始信号
    if (processed.count(info.code)) continue;
    processed.insert(info.code);

    // 统计当前信号已占用的源ECU（同一个code视为同一信号）
    std::unordered_set<EcuId> used_src;
    for (const auto& m : MESSAGE_INFO_VEC) {
      if (m.code == info.code) {
        used_src.insert(m.ecu_pair.src_ecu);
      }
    }

    // 已满足N模冗余，跳过
    if (used_src.size() >= static_cast<size_t>(redundancy_n)) {
      continue;
    }

    // 备选源ECU：排除已使用的源ECU与目的ECU
    std::vector<EcuId> candidates;
    for (int e : OPTION_ECU) {
      EcuId ecu = static_cast<EcuId>(e);
      if (ecu == info.ecu_pair.dst_ecu) continue;
      if (used_src.count(ecu)) continue;
      candidates.push_back(ecu);
    }

    int need = static_cast<int>(redundancy_n - used_src.size());
    if (static_cast<int>(candidates.size()) < need) {
      DEBUG_MSG_DEBUG1(std::cout, "信号", info.code, "可用ECU不足，跳过");
      continue;
    }

    // 选择源ECU策略：按带宽利用率从低到高选取need个
    auto pick = pick_backup_ecus(candidates, util, need);
    for (auto new_src : pick) {
      // 生成新的异源副本（同code，不同源ECU）
      MessageInfo backup_info(info, static_cast<int>(new_src), 2);
      MESSAGE_INFO_VEC.emplace_back(backup_info);
      size_t new_index = MESSAGE_INFO_VEC.size() - 1;

      // 先尝试直接插入现有报文，避免新建报文占用带宽
      working.message_set.emplace_back(new_index);
      auto& new_msg = working.message_set.back();

      bool inserted = try_insert_into_existing_frames(working, new_msg);
      if (!inserted) {
        need_repack = true;
      }

      any_added = true;
    }
  }

  if (!any_added) {
    return working;
  }

  if (!need_repack && !cfd::schedule::feasibility_check(working.frame_map)) {
    need_repack = true;
  }

  if (need_repack) {
    if (!working.re_init_frames()) {
      DEBUG_MSG_DEBUG1(std::cout, "异源备份重打包失败，回退");
      MESSAGE_INFO_VEC.resize(origin_info_size);
      return base_scheme;
    }

    cfd::packing::frame_pack(working, cfd::DEFAULT_PACK_METHOD);

    if (!cfd::schedule::feasibility_check(working.frame_map)) {
      DEBUG_MSG_DEBUG1(std::cout, "异源备份不可调度，回退");
      MESSAGE_INFO_VEC.resize(origin_info_size);
      return base_scheme;
    }
  }

  return working;
}

}  // namespace cfd::backups::signal
