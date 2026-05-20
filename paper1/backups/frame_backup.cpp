#include "frame_backup.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "priority_allocation.h"
#include "probabilistic_analysis/no_retry.h"

namespace cfd::backups::frame {
namespace {
struct FrameBackupCandidate {
  CanfdFrame frame;
  int level = 0;
  int period = 0;
  int round = 0;
};

int calc_required_backup_num(double p_total, double threshold) {
  if (p_total <= 0.0) return 0;
  if (p_total <= threshold) return 0;
  if (p_total >= 1.0) return 1;

  const double log_p_total = std::log(p_total);
  const double log_p_thresh = std::log(threshold);
  if (log_p_total >= 0.0) return 1;

  const int total_required = static_cast<int>(std::ceil(log_p_thresh / log_p_total));
  return std::max(0, total_required - 1);
}

int calc_frame_backup_num(const CanfdFrame& frame, double lambda) {
  if (frame.empty()) return 0;

  const double p_frame = analysis::prob_fault_one_more(frame.get_trans_time(), lambda);
  int backup_num = 0;

  for (const auto& msg : frame.msg_set) {
    const int level = msg.get_level();
    if (level < 0 || level >= NUM_MESSAGE_LEVEL) continue;

    const double threshold = analysis::threshold_per_window(level, msg.get_period());
    backup_num = std::max(backup_num, calc_required_backup_num(p_frame, threshold));
  }

  return backup_num;
}

int calc_frame_backup_num_method2(const CanfdFrame& frame, int bc_backup_num, int d_backup_num) {
  if (frame.empty()) return 0;

  int backup_num = 0;
  for (const auto& msg : frame.msg_set) {
    const int level = msg.get_level();
    if (level == 3) {
      backup_num = std::max(backup_num, d_backup_num);
    } else if (level == 1 || level == 2) {
      backup_num = std::max(backup_num, bc_backup_num);
    }
  }

  return std::max(0, backup_num);
}

int calc_frame_max_level(const CanfdFrame& frame) {
  int max_level = 0;
  for (const auto& msg : frame.msg_set) {
    max_level = std::max(max_level, msg.get_level());
  }
  return max_level;
}

bool duplicate_frame(PackingScheme& scheme, const CanfdFrame& origin) {
  if (origin.empty()) return true;

  const FrameId new_id = scheme.new_frame(origin.get_period(), origin.get_deadline(), origin.get_ecu_pair(),
                                          static_cast<int>(origin.get_offset()));
  if (new_id == static_cast<FrameId>(-1)) {
    return false;
  }

  auto frame_it = scheme.frame_map.find(new_id);
  if (frame_it == scheme.frame_map.end()) {
    return false;
  }

  for (const auto& msg : origin.msg_set) {
    scheme.message_set.emplace_back(msg.get_id_message());
    auto& new_msg = scheme.message_set.back();
    if (!frame_it->second.add_message(new_msg)) {
      return false;
    }
  }

  frame_it->second.set_offset(origin.get_offset());
  return true;
}

template <typename BackupNumFn>
std::vector<FrameBackupCandidate> build_frame_backup_candidates(const PackingScheme& scheme, BackupNumFn backup_num_fn) {
  std::vector<FrameBackupCandidate> candidates;
  candidates.reserve(scheme.frame_map.size());

  for (const auto& [id, frame] : scheme.frame_map) {
    (void)id;
    if (frame.empty()) continue;

    const int backup_num = std::max(0, backup_num_fn(frame));
    if (backup_num <= 0) continue;

    const int max_level = calc_frame_max_level(frame);
    if (max_level <= 0) continue;

    for (int round = 1; round <= backup_num; ++round) {
      candidates.push_back({frame, max_level, frame.get_period(), round});
    }
  }

  std::sort(candidates.begin(), candidates.end(), [](const FrameBackupCandidate& lhs, const FrameBackupCandidate& rhs) {
    if (lhs.level != rhs.level) return lhs.level > rhs.level;
    if (lhs.round != rhs.round) return lhs.round < rhs.round;
    if (lhs.period != rhs.period) return lhs.period > rhs.period;
    return lhs.frame.get_id() < rhs.frame.get_id();
  });

  return candidates;
}

PackingScheme apply_frame_backup_candidates(PackingScheme& scheme, const std::vector<FrameBackupCandidate>& candidates,
                                            const char* tag) {
  if (candidates.empty()) {
    return scheme;
  }

  DEBUG_MSG_DEBUG1(std::cout, tag, " frame backup candidates: ", static_cast<int>(candidates.size()));

  const double base_utilization = scheme.calc_bandwidth_utilization();
  double accepted_utilization = base_utilization;

  std::vector<const FrameBackupCandidate*> accepted_candidates;
  accepted_candidates.reserve(candidates.size());
  for (const auto& candidate : candidates) {
    const double delta_utilization = candidate.frame.get_trans_time() / candidate.frame.get_period();
    if (accepted_utilization + delta_utilization > 1.0) {
      continue;
    }
    accepted_candidates.push_back(&candidate);
    accepted_utilization += delta_utilization;
  }

  if (accepted_candidates.empty()) {
    DEBUG_MSG_DEBUG1(std::cout, tag, " no frame backup copy fits remaining bandwidth budget");
    return scheme;
  }

  PackingScheme working = scheme;
  for (const auto* candidate : accepted_candidates) {
    if (!duplicate_frame(working, candidate->frame)) {
      DEBUG_MSG_DEBUG1(std::cout, tag, " frame duplication failed after budget selection, fallback to original scheme");
      return scheme;
    }
  }

  if (!cfd::schedule::assign_priority(working.frame_map)) {
    DEBUG_MSG_DEBUG1(std::cout, tag, " priority assignment failed after frame backup, fallback to original scheme");
    return scheme;
  }

  if (!cfd::schedule::feasibility_check(working.frame_map)) {
    DEBUG_MSG_DEBUG1(std::cout, tag, " frame backup result is not schedulable, fallback to original scheme");
    return scheme;
  }

  DEBUG_MSG_DEBUG1(std::cout, tag, " accepted frame backup copies: ", static_cast<int>(accepted_candidates.size()));
  DEBUG_MSG_DEBUG1(std::cout, tag, " utilization after exact frame backup selection: ", accepted_utilization);
  return working;
}

}  // namespace

PackingScheme homo_frame_backup(PackingScheme& scheme, double lambda) {
  const auto candidates =
      build_frame_backup_candidates(scheme, [&](const CanfdFrame& frame) { return calc_frame_backup_num(frame, lambda); });
  return apply_frame_backup_candidates(scheme, candidates, "method1");
}

PackingScheme homo_frame_backup_method2(PackingScheme& scheme, int bc_backup_num, int d_backup_num) {
  const auto candidates = build_frame_backup_candidates(
      scheme, [&](const CanfdFrame& frame) { return calc_frame_backup_num_method2(frame, bc_backup_num, d_backup_num); });
  return apply_frame_backup_candidates(scheme, candidates, "method2");
}

PackingScheme hetero_frame_backup(PackingScheme& scheme, int redundancy_n, double lambda) {
  (void)redundancy_n;
  (void)lambda;
  DEBUG_MSG_DEBUG1(std::cout, "hetero frame backup is not implemented yet, return original scheme");
  return scheme;
}

}  // namespace cfd::backups::frame
