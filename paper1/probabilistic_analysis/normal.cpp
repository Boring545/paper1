#include "probabilistic_analysis/normal.h"

#include <cmath>
#include <stdexcept>

#include "../debug_tool.h"
#include "../scheme.h"
namespace cfd::analysis {

// 计算阶乘
long long int factorial(int m) {
  // 别用它计算太大的数
  if (m < 0) {
    throw std::invalid_argument("M 必须大于0");
  }
  if (m == 0) {
    return 1;
  }
  if (m > 20) {
    throw std::invalid_argument("M>20");
  }
  long long int result = 1;
  while (m > 0) {
    result *= m;
    m--;
  }
  return result;
}
double prob_fault(double t, int num, double lambda) {
  return std::exp(-1 * lambda * t) * std::pow(lambda * t, num) / factorial(num);
}

double prob_fault_one_more(double interference_win, double lambda) {
  return 1 - prob_fault(interference_win, 0, lambda);
}

std::unordered_map<MessageCode, double> sig_trans_fault_prob_analysis(PackingScheme& scheme, double lambda) {
  std::unordered_map<MessageCode, double> fault_prob_map;
  for (const auto& [id, frame] : scheme.frame_map) {
    int win = frame.get_trans_time();

    for (const auto& msg : frame.msg_set) {
      if (fault_prob_map.find(msg.get_code()) == fault_prob_map.end()) {
        fault_prob_map[msg.get_code()] = prob_fault_one_more(win, lambda);
      } else {
        fault_prob_map[msg.get_code()] *= prob_fault_one_more(win, lambda);
      }
    }
  }
  return fault_prob_map;
}

double ecu_fault_prob_analysis(const std::vector<double>& p_comm_fail, double p_ecu_fail) {
  auto clamp01 = [](double v) {
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
  };

  int n = static_cast<int>(p_comm_fail.size());
  if (n <= 0) return 0.0;

  // 至少需要过半正确
  int need = n / 2 + 1;

  double pe = clamp01(p_ecu_fail);

  // dp[k]：恰好k路“到达且正确”的概率
  std::vector<double> dp(n + 1, 0.0), ndp(n + 1, 0.0);
  dp[0] = 1.0;

  for (int i = 0; i < n; ++i) {
    double q = (1.0 - clamp01(p_comm_fail[i])) * (1.0 - pe);
    std::fill(ndp.begin(), ndp.end(), 0.0);
    for (int k = 0; k <= i; ++k) {
      if (dp[k] <= 0.0) continue;
      ndp[k] += dp[k] * (1.0 - q);
      ndp[k + 1] += dp[k] * q;
    }
    dp.swap(ndp);
  }

  double p_safe = 0.0;
  for (int k = need; k <= n; ++k) {
    p_safe += dp[k];
  }
  return clamp01(p_safe);
}

double ecu_fault_prob_analysis(const std::array<double, 3>& p_comm_fail, double p_ecu_fail) {
  std::vector<double> v = {p_comm_fail[0], p_comm_fail[1], p_comm_fail[2]};
  return ecu_fault_prob_analysis(v, p_ecu_fail);
}

std::unordered_map<MessageCode, double> ecu_fault_prob_analysis(PackingScheme& scheme, int redundancy_n, double lambda) {
  // N模冗余要求：N为奇数且>=3
  if (redundancy_n < 3 || (redundancy_n % 2) == 0) {
    DEBUG_MSG_DEBUG1(std::cout, "N模冗余参数非法，N应为奇数且>=3");
    return {};
  }

  // 统计每个ECU发送信号的最高ASIL等级
  std::unordered_map<EcuId, int> ecu_max_level;
  ecu_max_level.reserve(NUM_ECU);
  for (int e : OPTION_ECU) {
    ecu_max_level[static_cast<EcuId>(e)] = 0;
  }
  for (const auto& info : MESSAGE_INFO_VEC) {
    auto src = info.ecu_pair.src_ecu;
    auto it = ecu_max_level.find(src);
    if (it == ecu_max_level.end()) {
      ecu_max_level[src] = info.level;
    } else {
      it->second = std::max(it->second, info.level);
    }
  }

  // 收集每个type1信号的通信失败概率（按不同源ECU区分）
  std::unordered_map<MessageCode, std::unordered_map<EcuId, double>> code_comm_fail;
  for (const auto& [id, frame] : scheme.frame_map) {
    if (frame.empty()) continue;
    double p_comm = prob_fault_one_more(frame.get_trans_time(), lambda);
    for (const auto& msg : frame.msg_set) {
      const auto& info = MESSAGE_INFO_VEC[msg.get_id_message()];
      if (info.type != 1 && info.type != 2) continue;  // 仅关注type1及其异源备份

      auto& ecu_map = code_comm_fail[info.code];
      auto it = ecu_map.find(info.ecu_pair.src_ecu);
      if (it == ecu_map.end()) {
        ecu_map[info.ecu_pair.src_ecu] = p_comm;
      } else {
        // 若同一ECU存在多路副本，取更保守的通信失败概率
        it->second = std::max(it->second, p_comm);
      }
    }
  }

  std::unordered_map<MessageCode, double> result;
  result.reserve(code_comm_fail.size());

  for (const auto& [code, ecu_map] : code_comm_fail) {
    std::vector<std::pair<EcuId, double>> items;
    items.reserve(ecu_map.size());
    for (const auto& kv : ecu_map) {
      items.emplace_back(kv.first, kv.second);
    }

    // 只取N路，若多于N路则取故障概率最大的N路（保守估计）
    std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.second > b.second; });

    std::vector<double> p_comm_fail;
    p_comm_fail.reserve(redundancy_n);
    double p_ecu_fail = 0.0;
    for (size_t i = 0; i < items.size() && i < static_cast<size_t>(redundancy_n); ++i) {
      p_comm_fail.emplace_back(items[i].second);
      auto ecu = items[i].first;
      int level = ecu_max_level.count(ecu) ? ecu_max_level[ecu] : 0;
      p_ecu_fail = std::max(p_ecu_fail, THRESHOLD_RELIABILITY[level]);
    }

    // 若不足N路，用“通信失败概率=1”补齐到N路（保守估计）
    while (p_comm_fail.size() < static_cast<size_t>(redundancy_n)) {
      p_comm_fail.emplace_back(1.0);
    }

    result[code] = ecu_fault_prob_analysis(p_comm_fail, p_ecu_fail);
  }

  return result;
}

}  // namespace cfd::analysis
