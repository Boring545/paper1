// config.h
#include <string>
#include <xutility>
#ifndef CONFIG_H
#define CONFIG_H

#define V0 1
#define V1 0
#define V2 0
#define OFFSET_TEST  // 注释掉此行则关闭所有OFFSET_TEST相关逻辑

namespace cfd::packing {
enum class PACK_METHOD {
  SIMULATED_ANNEALING = 0,  // SA 打包
};
}  // namespace cfd::packing

namespace cfd::schedule {
enum class PRIORITY_ASSIGN_METHOD {
  OPTIMAL = 0,    // paper1方法，理论最优但计算量较大
  BY_PERIOD = 1,  // 按周期分配，周期越短优先级越高，简单但可能不可调度
};
};  // namespace cfd::schedule

namespace cfd {
const std::string TEST_INFO_PATH = "D:/document/CODE/paper1/storage/";  // 存储测试数据的文件
const std::string DEFAULT_MSG_FILE = "msg_2026312_21355.txt";   // 默认读取的消息文件名（需要你自己改成实际存在的文件）
const std::string DEFAULT_FRM_FILE = "frm_2025611_132913.txt";  // 默认读取的帧文件名（需要你自己改成实际存在的文件）

const cfd::packing::PACK_METHOD DEFAULT_PACK_METHOD = cfd::packing::PACK_METHOD::SIMULATED_ANNEALING;  // 默认打包算法

const cfd::schedule::PRIORITY_ASSIGN_METHOD DEFAULT_PRIORITY_ASSIGN_METHOD =
    cfd::schedule::PRIORITY_ASSIGN_METHOD::BY_PERIOD;  // 默认优先级分配算法

constexpr int REDUNDANCY_N = 3;  // N模冗余，必须是奇数

constexpr double LAMBDA_CONFERENCE = 2.7e-4;  // 干扰强度

constexpr int FACTOR_M_F_PERIOD = 2;  // 被打包到一个帧中的消息，消息周期必须为帧周期的[1,factor]倍

constexpr int OPTION_MESSAGE_SIZE[] = {1, 2, 4, 8, 16, 32, 64};  // 信号尺寸选项,单位为b
constexpr int NUM_MESSAGE_SIZE = std::size(OPTION_MESSAGE_SIZE);
constexpr double PROBABILITY_MESSAGE_SIZE[] = {0.35, 0.49, 0.13, 0.008, 0.013, 0.005, 0.002};  // 选择概率

constexpr int OPTION_MESSAGE_LEVEL[] = {0, 1, 2, 3};  // 安全等级选项,对应A\B\C\D
constexpr int NUM_MESSAGE_LEVEL = std::size(OPTION_MESSAGE_LEVEL);
constexpr double PROBABILITY_MESSAGE_LEVEL[] = {0.75, 0.1, 0.1, 0.05};  // 选择概率

// Threshold per hour (upper bound)
constexpr double THRESHOLD_RELIABILITY[] = {
    1e10, 1e-7, 1e-7, 1e-8};  // 1e10表示不要求可靠性，其他值为每小时的故障概率上限，对应安全等级A\B\C\D
constexpr int NUM_THRESHOLD_RELIABILITY = sizeof(THRESHOLD_RELIABILITY) / sizeof(THRESHOLD_RELIABILITY[0]);

#if V0
constexpr int SIZE_ORIGINAL_MESSAGE = 100;                           // 初始信号数量
constexpr int OPTION_MESSAGE_PERIOD[] = {1, 2, 5, 10, 20, 50, 100};  // 周期大小选项,单位为ms
constexpr int NUM_MESSAGE_PERIOD = std::size(OPTION_MESSAGE_PERIOD);
constexpr double PROBABILITY_MESSAGE_PERIOD[] = {0.03, 0.02, 0.02, 0.25, 0.25, 0.03, 0.2};  // 选择概率

constexpr int OPTION_ECU[] = {0, 1, 2, 3, 4, 5};  // ecu集合选项
constexpr int NUM_ECU = std::size(OPTION_ECU);

constexpr int OPTION_MESSAGE_TYPE[] = {0, 1, 2};  // 冗余备份选项 // 0 允许同源备份；1 必须异源备份
constexpr int NUM_MESSAGE_TYPE = std::size(OPTION_MESSAGE_TYPE);
constexpr double PROBABILITY_MESSAGE_TYPE[] = {0.90, 0.1, 0};  // type 的概率分布

#elif V1
constexpr int SIZE_ORIGINAL_MESSAGE = 1000;                       // 信号数量
constexpr int OPTION_MESSAGE_PERIOD[] = {2, 5, 10, 20, 50, 100};  // 周期大小选项,单位为ms
constexpr int NUM_MESSAGE_PERIOD = std::size(OPTION_MESSAGE_PERIOD);
constexpr double PROBABILITY_MESSAGE_PERIOD[] = {0.02, 0.02, 0.26, 0.26, 0.04, 0.4};  // 调整后的选择概率

constexpr int OPTION_ECU[] = {0, 1, 2, 3};
constexpr int NUM_ECU = std::size(OPTION_ECU);

constexpr int OPTION_MESSAGE_TYPE[] = {0, 1,
                                       2};  // 冗余备份选项 0 无需异源备份 1 需要异源备份（原本） 2 需要异源备份（副本）
constexpr int NUM_MESSAGE_TYPE = std::size(OPTION_MESSAGE_TYPE);
constexpr double PROBABILITY_MESSAGE_TYPE[] = {0.99, 0.01, 0};  // type 的概率分布

#elif V2
constexpr int SIZE_ORIGINAL_MESSAGE = 700;                        // 信号数量
constexpr int OPTION_MESSAGE_PERIOD[] = {2, 5, 10, 20, 50, 100};  // 周期大小选项,单位为ms
constexpr int NUM_MESSAGE_PERIOD = std::size(OPTION_MESSAGE_PERIOD);
constexpr double PROBABILITY_MESSAGE_PERIOD[] = {0.02, 0.02, 0.26, 0.26, 0.04, 0.4};  // 调整后的选择概率

constexpr int OPTION_ECU[] = {0, 1, 2, 3, 4, 5, 6, 7};
constexpr int NUM_ECU = std::size(OPTION_ECU);

constexpr int OPTION_MESSAGE_TYPE[] = {0, 1,
                                       2};  // 冗余备份选项 0 无需异源备份 1 需要异源备份（原本） 2 需要异源备份（副本）
constexpr int NUM_MESSAGE_TYPE = std::size(OPTION_MESSAGE_TYPE);
constexpr double PROBABILITY_MESSAGE_TYPE[] = {0.99, 0.01, 0};  // type 的概率分布
#endif
};  // namespace cfd

#endif  // CONFIG_H