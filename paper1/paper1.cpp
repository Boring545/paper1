// paper1.cpp: 定义应用程序的入口点。

#include <algorithm>
#include <fstream>
#include <unordered_map>
#include <vector>

#include "config.h"
#include "debug_tool.h"
#include "probabilistic_analysis/normal.h"
#include "scheme.h"
#include "signal_backup/backup.h"

#ifdef _WIN32
extern "C" __declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int);
extern "C" __declspec(dllimport) int __stdcall SetConsoleCP(unsigned int);
#endif

#ifndef CP_UTF8
#define CP_UTF8 65001
#endif

using namespace cfd;

// 返回时间戳
std::string create_msg() {
  cfd::utils::generate_msg_info_set();
  auto ts = cfd::utils::store_msg(cfd::TEST_INFO_PATH);
  DEBUG_MSG_DEBUG1(std::cout, "生成信号集合,数量：", cfd::SIZE_ORIGINAL_MESSAGE);
  DEBUG_MSG_DEBUG1(std::cout, "已生成信号集合, 时间戳: ", ts);
  return ts;
}

// 读取某个信号集合
void read_data_1() {
  // 读取消息
  cfd::utils::read_message(cfd::TEST_INFO_PATH + cfd::DEFAULT_MSG_FILE);
  DEBUG_MSG_DEBUG1(std::cout, "读取信号集合: ", cfd::DEFAULT_MSG_FILE);
  return;
}

// 读取某个信号集合及其对应的打包方案
cfd::PackingScheme read_data_2() {
  // 读取消息
  cfd::utils::read_message(cfd::TEST_INFO_PATH + cfd::DEFAULT_MSG_FILE);

  // 读取对应的打包方案
  cfd::CanfdFrameMap fmap;
  cfd::utils::read_frame(fmap, cfd::TEST_INFO_PATH + cfd::DEFAULT_FRM_FILE);
  return cfd::PackingScheme(fmap);
}

// 基于当前MESSAGE_INFO_VEC构建并优化打包方案
cfd::PackingScheme build_scheme_from_current_msgs() {
  cfd::PackingScheme scheme{};
  cfd::packing::frame_pack(scheme, cfd::DEFAULT_PACK_METHOD);
  return scheme;
}

// 输出当前各信号的故障概率、阈值与副本数量
void dump_signal_fault_summary(cfd::PackingScheme& scheme, double lambda, const char* title) {
  auto prob_map = cfd::analysis::sig_trans_fault_prob_analysis(scheme, lambda);

  std::unordered_map<MessageCode, int> code_level;
  std::unordered_map<MessageCode, int> code_count;
  std::unordered_map<MessageCode, int> code_period;
  code_level.reserve(MESSAGE_INFO_VEC.size());
  code_count.reserve(MESSAGE_INFO_VEC.size());
  code_period.reserve(MESSAGE_INFO_VEC.size());

  for (const auto& [id, frame] : scheme.frame_map) {
    if (frame.empty()) continue;
    for (const auto& msg : frame.msg_set) {
      const auto& info = MESSAGE_INFO_VEC[msg.get_id_message()];
      auto it = code_level.find(info.code);
      if (it == code_level.end()) {
        code_level.emplace(info.code, info.level);
      } else {
        it->second = std::max(it->second, info.level);
      }
      if (code_period.find(info.code) == code_period.end()) {
        code_period.emplace(info.code, info.period);
      }
      code_count[info.code] += 1;
    }
  }

  DEBUG_MSG_DEBUG1(std::cout, "=============================== ");
  DEBUG_MSG_DEBUG1(std::cout, title, " 信号故障概率/阈值/副本数量");
  for (const auto& [code, level] : code_level) {
    double p_fail = 0.0;
    auto it_p = prob_map.find(code);
    if (it_p != prob_map.end()) {
      p_fail = it_p->second;
    }

    int period_ms = -1;
    auto it_p0 = code_period.find(code);
    if (it_p0 != code_period.end()) {
      period_ms = it_p0->second;
    }
    double threshold = cfd::analysis::threshold_per_window(level, period_ms);

    int count = 0;
    auto it_c = code_count.find(code);
    if (it_c != code_count.end()) {
      count = it_c->second;
    }

    DEBUG_MSG_DEBUG1(std::cout, "code=", code, " P_fault=", sci(p_fail), " threshold=", sci(threshold),
                     " copies=", count, " level=", level);
  }
  DEBUG_MSG_DEBUG1(std::cout, "=============================== ");
}

// 导出：原方案信号故障概率、备份后信号故障概率、备份后ECU故障概率
void dump_signal_fault_compare(const cfd::PackingScheme& origin, const cfd::PackingScheme& backup, double lambda,
                               const std::string& filename) {
  struct CodeMeta {
    int level = 0;
    int period_ms = -1;
    EcuId src_ecu = 0;
    EcuId dst_ecu = 0;
    int type = 0;
  };
  struct EcuFaultResult {
    std::unordered_map<MessageCode, double> p_fault;
    std::unordered_map<MessageCode, double> p_ecu_fail;
  };

  auto level_to_char = [](int level) -> char {
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
  };

  auto build_code_meta = []() {
    std::unordered_map<MessageCode, CodeMeta> meta;
    meta.reserve(MESSAGE_INFO_VEC.size());
    for (const auto& info : MESSAGE_INFO_VEC) {
      auto it = meta.find(info.code);
      if (it == meta.end()) {
        meta.emplace(info.code,
                     CodeMeta{info.level, info.period, info.ecu_pair.src_ecu, info.ecu_pair.dst_ecu, info.type});
      } else if (it->second.type != 1 && info.type == 1) {
        it->second = {info.level, info.period, info.ecu_pair.src_ecu, info.ecu_pair.dst_ecu, info.type};
      }
    }
    return meta;
  };

  auto build_ecu_max_level = []() {
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
    return ecu_max_level;
  };

  auto count_copies = [](const cfd::PackingScheme& scheme) {
    std::unordered_map<MessageCode, int> count;
    count.reserve(MESSAGE_INFO_VEC.size());
    for (const auto& [id, frame] : scheme.frame_map) {
      if (frame.empty()) continue;
      for (const auto& msg : frame.msg_set) {
        const auto& info = MESSAGE_INFO_VEC[msg.get_id_message()];
        count[info.code] += 1;
      }
    }
    return count;
  };

  auto calc_ecu_fault_actual = [&](const cfd::PackingScheme& scheme,
                                   const std::unordered_map<MessageCode, CodeMeta>& meta,
                                   const std::unordered_map<EcuId, int>& ecu_max_level) {
    std::unordered_map<MessageCode, std::unordered_map<EcuId, double>> code_comm_fail;
    code_comm_fail.reserve(MESSAGE_INFO_VEC.size());

    for (const auto& [id, frame] : scheme.frame_map) {
      if (frame.empty()) continue;
      double p_comm = cfd::analysis::prob_fault_one_more(frame.get_trans_time(), lambda);
      for (const auto& msg : frame.msg_set) {
        const auto& info = MESSAGE_INFO_VEC[msg.get_id_message()];
        if (info.type != 1 && info.type != 2) continue;
        auto& ecu_map = code_comm_fail[info.code];
        auto it = ecu_map.find(info.ecu_pair.src_ecu);
        if (it == ecu_map.end()) {
          ecu_map[info.ecu_pair.src_ecu] = p_comm;
        } else {
          it->second *= p_comm;
        }
      }
    }

    EcuFaultResult result;
    result.p_fault.reserve(code_comm_fail.size());
    result.p_ecu_fail.reserve(code_comm_fail.size());
    for (const auto& [code, ecu_map] : code_comm_fail) {
      std::vector<double> p_comm_fail;
      p_comm_fail.reserve(ecu_map.size());
      double p_ecu_fail = 0.0;

      auto it_meta = meta.find(code);
      int period_ms = (it_meta != meta.end()) ? it_meta->second.period_ms : -1;

      for (const auto& kv : ecu_map) {
        p_comm_fail.emplace_back(kv.second);
        auto ecu = kv.first;
        int level = ecu_max_level.count(ecu) ? ecu_max_level.at(ecu) : 0;
        p_ecu_fail = std::max(p_ecu_fail, cfd::analysis::threshold_per_window(level, period_ms));
      }

      if (!p_comm_fail.empty()) {
        result.p_fault[code] = cfd::analysis::ecu_fault_prob_analysis(p_comm_fail, p_ecu_fail);
        result.p_ecu_fail[code] = p_ecu_fail;
      }
    }
    return result;
  };

  const auto meta = build_code_meta();
  const auto ecu_max_level = build_ecu_max_level();
  const auto prob_origin =
      cfd::analysis::sig_trans_fault_prob_analysis(const_cast<cfd::PackingScheme&>(origin), lambda);
  const auto prob_backup =
      cfd::analysis::sig_trans_fault_prob_analysis(const_cast<cfd::PackingScheme&>(backup), lambda);
  const auto ecu_origin = calc_ecu_fault_actual(origin, meta, ecu_max_level);
  const auto ecu_backup = calc_ecu_fault_actual(backup, meta, ecu_max_level);
  const auto copies_origin = count_copies(origin);
  const auto copies_backup = count_copies(backup);

  std::ofstream ofs(filename, std::ios::trunc);
  if (!ofs) {
    DEBUG_MSG_DEBUG1(std::cout, "Failed to open output file: ", filename);
    return;
  }

  ofs << "origin_bandwidth_util\t" << sci(origin.calc_bandwidth_utilization()) << '\n';
  ofs << "backup_bandwidth_util\t" << sci(backup.calc_bandwidth_utilization()) << '\n' << '\n';

  ofs << "[signal_fault_compare]\n";
  ofs << "code\tlevel\tlevel_asil\tsrc_ecu\tdst_ecu\tperiod_ms\tcopies_backup\tp_fault_origin\tp_fault_backup\tp_"
         "threshold\n";
  struct SignalRow {
    MessageCode code;
    CodeMeta meta;
    int copies_backup;
    double p0;
    double p1;
    double threshold;
  };
  std::vector<SignalRow> rows;
  rows.reserve(meta.size());
  for (const auto& [code, m] : meta) {
    int c0 = copies_origin.count(code) ? copies_origin.at(code) : 0;
    int c1 = copies_backup.count(code) ? copies_backup.at(code) : 0;
    if (c0 <= 0 && c1 <= 0) continue;

    double threshold = cfd::analysis::threshold_per_window(m.level, m.period_ms);
    double p0 = prob_origin.count(code) ? prob_origin.at(code) : 0.0;
    double p1 = prob_backup.count(code) ? prob_backup.at(code) : 0.0;

    rows.push_back(SignalRow{code, m, c1, p0, p1, threshold});
  }
  std::sort(rows.begin(), rows.end(), [](const SignalRow& a, const SignalRow& b) {
    if (a.threshold != b.threshold) return a.threshold > b.threshold;
    return a.code < b.code;
  });
  for (const auto& r : rows) {
    const auto& m = r.meta;
    ofs << r.code << '\t' << m.level << '\t' << level_to_char(m.level) << '\t' << m.src_ecu << '\t' << m.dst_ecu << '\t'
        << m.period_ms << '\t' << r.copies_backup << '\t' << sci(r.p0) << '\t' << sci(r.p1) << '\t' << sci(r.threshold)
        << '\n';
  }

  ofs << "\n[ecu_fault_compare]\n";
  ofs << "code\tlevel\tlevel_asil\tsrc_ecu\tdst_ecu\tperiod_ms\tp_fault_origin\tp_fault_backup\n";
  std::unordered_map<MessageCode, double> ecu_union = ecu_origin.p_fault;
  for (const auto& [code, p_fault] : ecu_backup.p_fault) {
    ecu_union[code] = p_fault;
  }
  for (const auto& [code, _] : ecu_union) {
    double p1 = ecu_backup.p_fault.count(code) ? ecu_backup.p_fault.at(code) : 0.0;

    double p_comm = prob_origin.count(code) ? prob_origin.at(code) : 0.0;
    double p_ecu = ecu_origin.p_ecu_fail.count(code) ? ecu_origin.p_ecu_fail.at(code) : 0.0;
    double p0 = 1.0 - (1.0 - p_comm) * (1.0 - p_ecu);
    if (p0 < 0.0) p0 = 0.0;
    if (p0 > 1.0) p0 = 1.0;

    auto it_meta = meta.find(code);
    if (it_meta != meta.end()) {
      const auto& m = it_meta->second;
      ofs << code << '\t' << m.level << '\t' << level_to_char(m.level) << '\t' << m.src_ecu << '\t' << m.dst_ecu << '\t'
          << m.period_ms << '\t' << sci(p0) << '\t' << sci(p1) << '\n';
    } else {
      ofs << code << '\t' << -1 << '\t' << '?' << '\t' << -1 << '\t' << -1 << '\t' << -1 << '\t' << sci(p0) << '\t'
          << sci(p1) << '\n';
    }
  }
  ofs.close();
}

void task_test_homo_backup() {
  // 初始方案
  DEBUG_MSG_DEBUG1(std::cout, "生成打包方案");
  cfd::PackingScheme scheme_origin = build_scheme_from_current_msgs();

  // 备份前统计
  // dump_signal_fault_summary(scheme_origin, cfd::LAMBDA_CONFERENCE, "备份前");

  // 同源备份
  DEBUG_MSG_DEBUG1(std::cout, "执行同源备份");
  cfd::PackingScheme scheme_homo = cfd::backups::homo_signal_backup(scheme_origin);

  // 备份后统计
  // dump_signal_fault_summary(scheme_homo, cfd::LAMBDA_CONFERENCE, "备份后");

  // 简单统计带宽利用率
  DEBUG_MSG_DEBUG1(std::cout, "原方案带宽利用率: ", scheme_origin.calc_bandwidth_utilization());
  DEBUG_MSG_DEBUG1(std::cout, "同源备份带宽利用率: ", scheme_homo.calc_bandwidth_utilization());
}

// 任务：测试异源备份
void task_test_hetero_backup() {
  // 初始方案
  DEBUG_MSG_DEBUG1(std::cout, "生成打包方案");
  cfd::PackingScheme scheme_origin = build_scheme_from_current_msgs();

  // 异源备份
  DEBUG_MSG_DEBUG1(std::cout, "执行异源备份, N = ", cfd::REDUNDANCY_N);
  cfd::PackingScheme scheme_hetero = cfd::backups::hetero_signal_backup(scheme_origin, cfd::REDUNDANCY_N);

  // 统计异源备份后的ECU故障概率（仅打印部分结果）
  DEBUG_MSG_DEBUG1(std::cout, "计算异源备份后的ECU故障概率");
  auto res_ecu = cfd::analysis::ecu_fault_prob_analysis(scheme_hetero, cfd::REDUNDANCY_N);
  int cnt = 0;
  for (const auto& [code, p_fault] : res_ecu) {
    DEBUG_MSG_DEBUG1(std::cout, "code=", code, " P_fault=", sci(p_fault));
  }
}

// 任务：同源备份 + 异源备份全流程
void task_test_all() {
  // 初始方案
  DEBUG_MSG_DEBUG1(std::cout, "生成打包方案");
  cfd::PackingScheme scheme_origin = build_scheme_from_current_msgs();
  double origin_u = scheme_origin.calc_bandwidth_utilization();

  // 同源备份
  DEBUG_MSG_DEBUG1(std::cout, "执行同源备份");
  cfd::PackingScheme scheme_homo = cfd::backups::homo_signal_backup(scheme_origin);
  double homo_u = scheme_homo.calc_bandwidth_utilization();

  // 异源备份（基于原始方案）
  DEBUG_MSG_DEBUG1(std::cout, "执行异源备份, N = ", REDUNDANCY_N);
  cfd::PackingScheme scheme_hetero = cfd::backups::hetero_signal_backup(scheme_homo);
  double hetero_u = scheme_hetero.calc_bandwidth_utilization();

  // 打印带宽利用率对比
  DEBUG_MSG_DEBUG1(std::cout, "原方案带宽利用率: ", sci(origin_u));
  DEBUG_MSG_DEBUG1(std::cout, "同源备份带宽利用率: ", sci(homo_u));
  DEBUG_MSG_DEBUG1(std::cout, "异源备份带宽利用率: ", sci(hetero_u));

  // 统计异源备份后的ECU故障概率（仅打印部分结果）
  DEBUG_MSG_DEBUG1(std::cout, "计算异源备份后的ECU故障概率");
  auto res_ecu = cfd::analysis::ecu_fault_prob_analysis(scheme_hetero);
  int cnt = 0;
  for (const auto& [code, p_fault] : res_ecu) {
    DEBUG_MSG_DEBUG1(std::cout, "code=", code, " P_fault=", p_fault);
  }

  // dump_signal_fault_summary(scheme_hetero, cfd::LAMBDA_CONFERENCE, "异源备份后");

  // 导出“原方案 vs 备份后”对比结果
  const std::string ts = get_time_stamp();
  const std::string out_file = cfd::TEST_INFO_PATH + "signal_fault_compare_" + ts + ".txt";
  dump_signal_fault_compare(scheme_origin, scheme_hetero, cfd::LAMBDA_CONFERENCE, out_file);
  DEBUG_MSG_DEBUG1(std::cout, "对比结果已导出: ", out_file);
}

int main() {
#ifdef _WIN32
  // 让 Windows 控制台按 UTF-8 显示
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
#endif

  read_data_1();
  // create_msg();
  //   task_test_homo_backup();
  //   task_test_hetero_backup();
  task_test_all();

  return 0;
}
