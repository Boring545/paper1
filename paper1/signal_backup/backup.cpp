#include "signal_backup/backup.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "priority_allocation.h"
#include "probabilistic_analysis/normal.h"
namespace cfd::backups {
namespace {
constexpr int MAX_ITER = 50;

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
  PackingScheme best = scheme;
  auto code_index_map = build_code_index_map();

  for (int iter = 0; iter < MAX_ITER; ++iter) {
    auto result = analysis::sig_trans_fault_prob_analysis(working, lambda);
    bool need_backup = false;
    int added_cnt = 0;

    PackingScheme prev = working;

    for (const auto& [code, p_total] : result) {
      auto it = code_index_map.find(code);
      if (it == code_index_map.end()) continue;
      size_t idx = it->second;
      int level = MESSAGE_INFO_VEC[idx].level;
      if (level < 0 || level >= NUM_MESSAGE_LEVEL) continue;

      double threshold = THRESHOLD_RELIABILITY[level];
      if (p_total <= threshold) continue;

      int backup_num = calc_required_backup_num(p_total, threshold);
      if (backup_num <= 0) continue;

      need_backup = true;
      for (int i = 0; i < backup_num; ++i) {
        working.message_set.emplace_back(idx);
        added_cnt++;
      }
    }

    if (!need_backup) {
      return working;
    }

    DEBUG_MSG_DEBUG1(std::cout, "本轮新增同源副本数量: ", added_cnt);

    if (!working.re_init_frames()) {
      DEBUG_MSG_DEBUG1(std::cout, "重打包失败，返回上一轮可行解");
      return prev;
    }

    auto sa = cfd::packing::PACK_METHOD::SIMULATED_ANNEALING;
    cfd::packing::frame_pack(working, sa);

    if (!cfd::schedule::feasibility_check(working.frame_map)) {
      DEBUG_MSG_DEBUG1(std::cout, "不可调度，返回上一轮可行解");
      return prev;
    }

    best = working;
  }

  DEBUG_MSG_DEBUG1(std::cout, "达到最大迭代次数，返回当前可行解");
  return best;
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
    const auto& info = MESSAGE_INFO_VEC[i];
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

}  // namespace cfd::backups
