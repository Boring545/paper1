// paper1.cpp: 定义应用程序的入口点。

#include "config.h"
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

// 返回时间戳
std::string create_msg() {
  cfd::utils::generate_msg_info_set();
  auto ts = cfd::utils::store_msg(cfd::TEST_INFO_PATH);
  DEBUG_MSG_DEBUG1(std::cout, "生成信号集合,数量：", cfd::SIZE_ORIGINAL_MESSAGE);
  DEBUG_MSG_DEBUG1(std::cout, "已生成信号集合, 时间戳: ", ts);
  return ts;
}

// 读取某个信号集合
void read_data_1(std::string msg_file_name) {
  // 读取消息
  cfd::utils::read_message(cfd::TEST_INFO_PATH + msg_file_name);
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

// 任务：测试同源备份
void task_test_homo_backup() {
  // 生成一份随机信号集合
  DEBUG_MSG_DEBUG1(std::cout, "生成信号集合,数量：", cfd::SIZE_ORIGINAL_MESSAGE);
  create_msg();

  // 初始方案
  DEBUG_MSG_DEBUG1(std::cout, "生成打包方案");
  cfd::PackingScheme scheme_origin = build_scheme_from_current_msgs();

  // 同源备份
  DEBUG_MSG_DEBUG1(std::cout, "执行同源备份");
  cfd::PackingScheme scheme_homo = cfd::backups::homo_signal_backup(scheme_origin);

  // 简单统计带宽利用率
  DEBUG_MSG_DEBUG1(std::cout, "原方案带宽利用率: ", scheme_origin.calc_bandwidth_utilization());
  DEBUG_MSG_DEBUG1(std::cout, "同源备份带宽利用率: ", scheme_homo.calc_bandwidth_utilization());
}

// 任务：测试异源备份
void task_test_hetero_backup() {
  // 生成一份随机信号集合
  DEBUG_MSG_DEBUG1(std::cout, "生成信号集合,数量：", cfd::SIZE_ORIGINAL_MESSAGE);
  create_msg();

  // 初始方案
  DEBUG_MSG_DEBUG1(std::cout, "生成打包方案");
  cfd::PackingScheme scheme_origin = build_scheme_from_current_msgs();

  // 异源备份
  DEBUG_MSG_DEBUG1(std::cout, "执行异源备份, N = ", cfd::REDUNDANCY_N);
  cfd::PackingScheme scheme_hetero = cfd::backups::hetero_signal_backup(scheme_origin, cfd::REDUNDANCY_N);

  // 统计异源备份后的ECU故障成功概率（仅打印部分结果）
  DEBUG_MSG_DEBUG1(std::cout, "计算异源备份后的ECU故障成功概率");
  auto res_ecu = cfd::analysis::ecu_fault_prob_analysis(scheme_hetero, cfd::REDUNDANCY_N);
  int cnt = 0;
  for (const auto& [code, p_safe] : res_ecu) {
    DEBUG_MSG_DEBUG1(std::cout, "code=", code, " P_safe=", p_safe);
  }
}

// 任务：同源备份 + 异源备份全流程
void task_test_all() {
  // 生成一份随机信号集合
  DEBUG_MSG_DEBUG1(std::cout, "生成信号集合,数量：", cfd::SIZE_ORIGINAL_MESSAGE);
  create_msg();

  // 初始方案
  DEBUG_MSG_DEBUG1(std::cout, "生成打包方案");
  cfd::PackingScheme scheme_origin = build_scheme_from_current_msgs();

  // 同源备份
  DEBUG_MSG_DEBUG1(std::cout, "执行同源备份");
  cfd::PackingScheme scheme_homo = cfd::backups::homo_signal_backup(scheme_origin);

  // 异源备份（基于原始方案）
  int redundancy_n = 3;  // 可改成5/7等奇数
  DEBUG_MSG_DEBUG1(std::cout, "执行异源备份, N = ", redundancy_n);
  cfd::PackingScheme scheme_hetero = cfd::backups::hetero_signal_backup(scheme_origin, redundancy_n);

  // 打印带宽利用率对比
  DEBUG_MSG_DEBUG1(std::cout, "原方案带宽利用率: ", scheme_origin.calc_bandwidth_utilization());
  DEBUG_MSG_DEBUG1(std::cout, "同源备份带宽利用率: ", scheme_homo.calc_bandwidth_utilization());
  DEBUG_MSG_DEBUG1(std::cout, "异源备份带宽利用率: ", scheme_hetero.calc_bandwidth_utilization());

  // 统计异源备份后的ECU故障成功概率（仅打印部分结果）
  DEBUG_MSG_DEBUG1(std::cout, "计算异源备份后的ECU故障成功概率");
  auto res_ecu = cfd::analysis::ecu_fault_prob_analysis(scheme_hetero, redundancy_n);
  int cnt = 0;
  for (const auto& [code, p_safe] : res_ecu) {
    DEBUG_MSG_DEBUG1(std::cout, "code=", code, " P_safe=", p_safe);
    if (++cnt >= 10) break;  // 仅展示前10条
  }
}

int main() {
#ifdef _WIN32
  // 让 Windows 控制台按 UTF-8 显示
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
#endif

  // task_test_homo_backup();
  task_test_hetero_backup();
  // task_test_all();

  return 0;
}
