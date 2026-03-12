#include "priority_allocation.h"

#include "scheme.h"

#ifdef PAPER1_SCHEDUAL
namespace cfd::schedule::paper1 {
int offset_trans(int target, int basis, int T) {
  if (target < basis) {
    target = (int)ceil(((double)basis - target) / T) * T + target;
  }
  return target - basis;
}

bool find_interval(const std::vector<CanfdFrame*>& frame_set, std::vector<int>& lower_bound,
                   std::vector<int>& upper_bound) {
  if (frame_set.empty()) return false;
  std::vector<int> omax(frame_set.size(), 0);  // omax为frame_set中，除了自己index对应的frame外最大的offset

  int temp_p = frame_set[0]->get_period();  // temp_p为frame_set中所有周期的最小公倍数
  for (size_t j = 0; j < frame_set.size(); j++) {
    for (size_t i = 0; i < frame_set.size(); i++) {
      if (i == j) {
        continue;
      } else {
        omax[j] = std::max(omax[j], frame_set[i]->get_offset());
      }
    }
    temp_p = std::lcm(temp_p, frame_set[j]->get_period());
  }

  lower_bound.resize(frame_set.size());
  upper_bound.resize(frame_set.size());
  for (size_t i = 0; i < frame_set.size(); i++) {
    int quotient = ceil((double)(omax[i] - frame_set[i]->get_offset()) / frame_set[i]->get_period());
    lower_bound[i] = (int)(quotient * frame_set[i]->get_period());
    upper_bound[i] = lower_bound[i] + temp_p;
  }
  return true;
}

bool create_beta(const std::vector<CanfdFrame*>& frame_set, const CanfdFrame& frame, int lower_bound,
                 std::vector<betaset>& beta) {
  int upper = lower_bound;
  int lower = upper - frame.get_period() + frame.get_deadline();

  // 预分配beta容量，减少内存重新分配
  beta.reserve(frame_set.size() * 10);  // 假设每个frame会生成10个beta

  for (size_t i = 0; i < frame_set.size(); i++) {
    if (frame_set[i] == &frame) continue;

    int offset = offset_trans(frame_set[i]->get_offset(), frame.get_offset(), frame_set[i]->get_period());
    if (offset < upper) {
      for (int j = 0; j * frame_set[i]->get_period() + offset < upper; j++) {
        beta.emplace_back(frame_set[i]->get_trans_time(), j * frame_set[i]->get_period() + offset);
      }
    }
  }

  std::sort(beta.begin(), beta.end(), [](const betaset& a, const betaset& b) { return a.tr < b.tr; });

  return true;
}

double calc_remain_interf(const CanfdFrame& frame, int t, std::vector<betaset>& beta) {
  double response_time = 0;
  int time = t - frame.get_period() + frame.get_deadline();
  time = 0;  // 和伪代码不一样，但这样能跑通

  for (const betaset& b : beta) {
    // time为之前一次任务m的最晚结束时间
    // R为已经考虑的任务的执行的时间和，R+time<tr表示之前的任务自从release后，
    // 其一定执行完毕，下一个任务不需要再考虑以前的任务了。
    if (b.tr >= time + response_time) {
      response_time = 0;
      time = b.tr;  // 和伪代码不一样，但这样能跑通
    }

    response_time = response_time + b.C;
  }

  // 积累的任务在R+time时候才能彻底完成， response_time + time - t表示在当前考虑的任务release后，还需等待之前的任务多久
  response_time = response_time - (t - time);
  if (response_time < 0) {
    response_time = 0;
  }
  return response_time;
}

bool create_eta(const std::vector<CanfdFrame*>& frame_set, const CanfdFrame& frame, int t, double response_time,
                std::vector<betaset>& eta) {
  int lower = response_time + t;
  int upper = lower + frame.get_deadline();
  for (size_t i = 0; i < frame_set.size(); i++) {
    if (frame_set[i] == &frame) continue;
    int offset = offset_trans(frame_set[i]->get_offset(), frame.get_offset(), frame_set[i]->get_period());
    // tr=O+m*T,寻找【lower，upper】内所有可能的tr取值，o就是frame_set[i].offset，T是frame_set[i].period，m为常数
    int m = ceil(((double)lower - offset) / (double)frame_set[i]->get_period());
    if (m * frame_set[i]->get_period() + offset < upper) {
      for (int j = 0; (m + j) * frame_set[i]->get_period() + offset < upper; j++) {
        eta.emplace_back(frame_set[i]->get_trans_time(), (m + j) * frame_set[i]->get_period() + offset);
      }
    }
  }
  std::sort(eta.begin(), eta.end(), [](const betaset& a, const betaset& b) { return a.tr < b.tr; });
  return true;
}

double calc_create_interf(const CanfdFrame& frame, const int t, const int response_time,
                          const std::vector<betaset>& eta) {
  int next_free = response_time + t;
  double K = 0;
  double total_created = response_time;
  for (const betaset& e : eta) {
    total_created += e.C;
    if (next_free < e.tr) {
      next_free = e.tr;
    }
    K = K + std::min(t + (double)frame.get_deadline() - next_free, e.C);
    next_free = std::min(t + (double)frame.get_deadline(), next_free + e.C);
  }

  return K;
}

bool feasibility_check(const std::vector<CanfdFrame*>& frame_set, int taski, int pri, const std::vector<int>& lower,
                       const std::vector<int>& upper) {
  int t = 0;
  double response_time = 0, K = 0;
  std::vector<betaset> beta, eta;
  int score = 0;

  while (t < upper[taski]) {
    create_beta(frame_set, *frame_set[taski], lower[taski], beta);
    response_time = calc_remain_interf(*frame_set[taski], lower[taski], beta);
    create_eta(frame_set, *frame_set[taski], lower[taski], response_time, eta);
    K = std::max(0.0, calc_create_interf(*frame_set[taski], lower[taski], response_time, eta));

    if (frame_set[taski]->get_trans_time() + response_time + K > frame_set[taski]->get_deadline()) {
      // 任务i永远不能可行，这使得任务集也不可行
      beta.clear();
      eta.clear();
      DEBUG_MSG_DEBUG2(std::cout, "exec:", frame_set[taski]->get_trans_time(), "+ R:", response_time, " + K:", K,
                       " > D:", frame_set[taski]->get_deadline());
      DEBUG_MSG_DEBUG2(std::cout, "任务", frame_set[taski]->get_id(), "  分配优先级", pri, "失败");
      return false;
    }
    DEBUG_MSG_DEBUG2(std::cout, "exec:", frame_set[taski]->get_trans_time(), "+ R:", response_time, " + K:", K,
                     " <= D:", frame_set[taski]->get_deadline());
    beta.clear();
    eta.clear();
    t = t + frame_set[taski]->get_period();
  }
  return true;
}

bool assign_priority(CanfdFrameMap& frame_map) {
  std::vector<CanfdFrame*> frame_set_copy;
  for (auto& [key, frame] : frame_map) {
    if (frame.empty()) {
      continue;
    }
    frame_set_copy.push_back(&frame);
  }
  // deadline降序排列，这使得后面分配优先级时，将deadline小的帧分配高(大)的优先级
  std::sort(frame_set_copy.begin(), frame_set_copy.end(),
            [](CanfdFrame* f1, CanfdFrame* f2) { return f1->get_deadline() > f2->get_deadline(); });

  int frame_num = frame_set_copy.size();  // 实际需要分配优先级的帧数量
  std::vector<int> lower, upper;

  bool unassigned = true;

  // 每次循环，从大到0分配一个优先级
  for (int pri = frame_num - 1; pri >= 0; pri--) {
    unassigned = true;
    find_interval(frame_set_copy, lower, upper);
    // 填充可选任务集合
    for (size_t index = 0; index < frame_set_copy.size(); index++) {
      if (feasibility_check(frame_set_copy, index, pri, lower, upper)) {
        frame_set_copy[index]->set_priority(pri);
        DEBUG_MSG_DEBUG2(std::cout, "任务", frame_set_copy[index]->get_id(), "  分配优先级", pri,
                         "成功！！！！！！！！！");
        frame_set_copy.erase(frame_set_copy.begin() +
                             index);  // 从未分配集合中，删除分配成功的frmae，然后尝试分配下一个优先级
        unassigned = false;
        break;
      }
    }

    // 找不到一个合适的帧来分配优先级pri
    if (unassigned) {
      DEBUG_MSG_DEBUG1(std::cout, "=============================== ");
      DEBUG_MSG_DEBUG1(std::cout, "优先级分配失败。。。 ");
      DEBUG_MSG_DEBUG1(std::cout, "=============================== ");
      return false;
    }
    lower.clear();
    upper.clear();
  }

  DEBUG_MSG_DEBUG2(std::cout, "=============================== ");
  DEBUG_MSG_DEBUG2(std::cout, "优先级分配成功！！！ ");
  for (auto& fp : frame_map) {
    auto& frame = fp.second;
    DEBUG_MSG_DEBUG2(std::cout, "任务: ", frame.get_id(), "  优先级： ", frame.get_priority());
  }
  DEBUG_MSG_DEBUG2(std::cout, "=============================== ");
  return true;
}

bool assign_priority(cfd::PackingScheme& scheme) { return assign_priority(scheme.frame_map); }

}  // namespace cfd::schedule::paper1
#endif

namespace cfd::schedule {
bool feasibility_check(const CanfdFrameMap& frame_map) {
  std::vector<const CanfdFrame*> sorted_frames;
  sorted_frames.reserve(frame_map.size());

  double utilization = 0.0;

  for (const auto& [key, frame] : frame_map) {
    if (frame.empty()) continue;
    sorted_frames.push_back(&frame);
    utilization += frame.get_trans_time() / frame.get_period();
  }

  if (sorted_frames.empty()) return true;

  // 2) SORT：按优先级从高到低（0最高），这样 index 越小优先级越高
  std::sort(sorted_frames.begin(), sorted_frames.end(),
            [](const CanfdFrame* a, const CanfdFrame* b) { return a->get_priority() < b->get_priority(); });

  constexpr double EPS = 1e-7;
  const double jitter = 0.0;  // 暂不考虑抖动

  // 3) 对每个帧做 WCRT 迭代，若任一帧超 deadline -> 不可行
  for (int frame_index = 0; frame_index < static_cast<int>(sorted_frames.size()); ++frame_index) {
    const CanfdFrame& frame = *sorted_frames[frame_index];
    const double period = frame.get_period();
    const double trans_time = frame.get_trans_time();
    const double deadline = frame.get_deadline();

    // 3.1 计算该帧的 blocking（更低优先级帧的最大传输时间）
    double max_lp_trans_time = 0.0;
    for (int j = frame_index + 1; j < static_cast<int>(sorted_frames.size()); ++j) {
      const double ft = sorted_frames[j]->get_trans_time();
      if (ft > max_lp_trans_time) max_lp_trans_time = ft;
    }

    // 3.2 响应时间迭代：R = B + C + sum ceil((R - C + Cbit + jitter)/Tj)*Cj
    double E_R = max_lp_trans_time + trans_time;
    double E_R_PRE = 0;

    // 可选：迭代上限，防止死循环（也可以不用）
    for (int iter = 0; iter < 1000 && std::abs(E_R - E_R_PRE) > EPS; ++iter) {
      double interference = 0.0;

      // 累加更高优先级帧的干扰（0..frame_index-1）
      for (int j = frame_index - 1; j >= 0; --j) {
        const double Tj = sorted_frames[j]->get_period();
        const double Cj = sorted_frames[j]->get_trans_time();

        interference += std::ceil((E_R - trans_time + TIME_BIT_ARBITRATION + jitter) / Tj) * Cj;
      }

      E_R_PRE = E_R;
      E_R = max_lp_trans_time + interference + trans_time;

      // 一旦超过 deadline，可直接判失败
      if (E_R > deadline + EPS) {
        DEBUG_MSG_DEBUG2(std::cout, "[Sched FAIL] frame=", frame.get_id(), " pri=", frame.get_priority(), " R=", E_R,
                         " D=", deadline, " B=", max_lp_trans_time, " C=", trans_time);
        DEBUG_MSG_DEBUG1(std::cout, "不可行原因: 响应时间超截止期限", " frame=", frame.get_id(),
                         " pri=", frame.get_priority(), " R=", E_R, " D=", deadline, " B=", max_lp_trans_time,
                         " C=", trans_time, " period=", period, " utilization=", utilization);

        return false;
      }
    }
  }

  return true;
}

bool assign_priority(CanfdFrameMap& frame_map, PRIORITY_ASSIGN_METHOD method) {
  switch (method) {
    case PRIORITY_ASSIGN_METHOD::BY_PERIOD:
      return assign_priority_by_period(frame_map);
    default:
      break;
  }

  return false;
}

bool assign_priority_by_period(CanfdFrameMap& frame_map) {
  std::vector<CanfdFrame*> frames;
  frames.reserve(frame_map.size());

  for (auto& [key, frame] : frame_map) {
    if (frame.empty()) continue;
    frames.push_back(&frame);
  }

  // 周期越小优先级越高（0最高）
  // period 相同 -> deadline 越小越高；再相同 -> id 越小越高
  std::sort(frames.begin(), frames.end(), [](const CanfdFrame* a, const CanfdFrame* b) {
    if (a->get_period() != b->get_period()) return a->get_period() < b->get_period();
    if (a->get_deadline() != b->get_deadline()) return a->get_deadline() < b->get_deadline();
    return a->get_id() < b->get_id();
  });

  for (int pri = 0; pri < static_cast<int>(frames.size()); ++pri) {
    frames[pri]->set_priority(pri);
    DEBUG_MSG_DEBUG2(std::cout, "任务", frames[pri]->get_id(), " 分配优先级 ", pri);
  }

  DEBUG_MSG_DEBUG2(std::cout, "=============================== ");
  DEBUG_MSG_DEBUG2(std::cout, "优先级分配完成（按周期升序，0最高）");
  for (auto& fp : frame_map) {
    auto& frame = fp.second;
    if (frame.empty()) continue;
    DEBUG_MSG_DEBUG2(std::cout, "任务: ", frame.get_id(), " 周期: ", frame.get_period(),
                     " 截止期: ", frame.get_deadline(), " 优先级: ", frame.get_priority());
  }
  DEBUG_MSG_DEBUG2(std::cout, "=============================== ");
  return true;
}

}  // namespace cfd::schedule
