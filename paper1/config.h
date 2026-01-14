// config.h
#ifndef CONFIG_H
#define CONFIG_H


#define V0 1
#define V1 0
#define V2 0
#define OFFSET_TEST  // 注释掉此行则关闭所有OFFSET_TEST相关逻辑

namespace cfd {
	constexpr int FACTOR_M_F_PERIOD = 2;				// 被打包到一个帧中的消息，消息周期必须为帧周期的[1,factor]倍

	constexpr int OPTION_MESSAGE_SIZE[] = { 1, 2, 4, 8, 16, 32, 64 };									// 信号尺寸选项,单位为b
	constexpr int NUM_MESSAGE_SIZE = std::size(OPTION_MESSAGE_SIZE);
	constexpr double PROBABILITY_MESSAGE_SIZE[] = { 0.35, 0.49, 0.13, 0.008, 0.013, 0.005, 0.002 };		// 选择概率


	constexpr int OPTION_MESSAGE_LEVEL[] = { 0, 1, 2, 3 };								// 安全等级选项,对应A\B\C\D
	constexpr int NUM_MESSAGE_LEVEL = std::size(OPTION_MESSAGE_LEVEL);
	constexpr double PROBABILITY_MESSAGE_LEVEL[] = { 0.75,0.1,0.1,0.05 };				// 选择概率
	constexpr double THRESHOLD_RELIABILITY[] = { 1e-3,1e-7,1e-7,1e-8 };					// 安全等级对应的帧传输时的故障率上限

#if V0
	constexpr int SIZE_ORIGINAL_MESSAGE = 100;											// 初始信号数量
	constexpr int OPTION_MESSAGE_PERIOD[] = { 1, 2, 5, 10, 20, 50, 100 };								// 周期大小选项,单位为ms
	constexpr int NUM_MESSAGE_PERIOD = std::size(OPTION_MESSAGE_PERIOD);
	constexpr double PROBABILITY_MESSAGE_PERIOD[] = { 0.03, 0.02, 0.02, 0.25, 0.25, 0.03, 0.2 };		// 选择概率

	constexpr int OPTION_ECU[] = { 0,1,2,3,4,5 };										// ecu集合选项
	constexpr int NUM_ECU = std::size(OPTION_ECU);

	constexpr int OPTION_MESSAGE_TYPE[] = { 0, 1, 2 };									// 冗余备份选项 0 无需异源备份 1 需要异源备份（原本） 2 需要异源备份（副本）
	constexpr int NUM_MESSAGE_TYPE = std::size(OPTION_MESSAGE_TYPE);
	constexpr double PROBABILITY_MESSAGE_TYPE[] = { 0.98,0.02,0 };							// type 的概率分布

#elif V1
	constexpr int SIZE_ORIGINAL_MESSAGE = 1000;																	// 信号数量
	constexpr int OPTION_MESSAGE_PERIOD[] = { 2, 5, 10, 20, 50, 100 };								// 周期大小选项,单位为ms
	constexpr int NUM_MESSAGE_PERIOD = std::size(OPTION_MESSAGE_PERIOD);
	constexpr double PROBABILITY_MESSAGE_PERIOD[] = { 0.02, 0.02, 0.26, 0.26, 0.04, 0.4 };			// 调整后的选择概率

	constexpr int OPTION_ECU[] = { 0,1,2,3 };
	constexpr int NUM_ECU = std::size(OPTION_ECU);

	constexpr int OPTION_MESSAGE_TYPE[] = { 0, 1, 2 };									// 冗余备份选项 0 无需异源备份 1 需要异源备份（原本） 2 需要异源备份（副本）
	constexpr int NUM_MESSAGE_TYPE = std::size(OPTION_MESSAGE_TYPE);
	constexpr double PROBABILITY_MESSAGE_TYPE[] = { 0.99,0.01,0 };							// type 的概率分布

#elif V2
	constexpr int SIZE_ORIGINAL_MESSAGE = 700;																	// 信号数量
	constexpr int OPTION_MESSAGE_PERIOD[] = { 2, 5, 10, 20, 50, 100 };								// 周期大小选项,单位为ms
	constexpr int NUM_MESSAGE_PERIOD = std::size(OPTION_MESSAGE_PERIOD);
	constexpr double PROBABILITY_MESSAGE_PERIOD[] = { 0.02, 0.02, 0.26, 0.26, 0.04, 0.4 };			// 调整后的选择概率

	constexpr int OPTION_ECU[] = { 0,1,2,3,4,5,6,7 };
	constexpr int NUM_ECU = std::size(OPTION_ECU);

	constexpr int OPTION_MESSAGE_TYPE[] = { 0, 1, 2 };									// 冗余备份选项 0 无需异源备份 1 需要异源备份（原本） 2 需要异源备份（副本）
	constexpr int NUM_MESSAGE_TYPE = std::size(OPTION_MESSAGE_TYPE);
	constexpr double PROBABILITY_MESSAGE_TYPE[] = { 0.99,0.01,0 };							// type 的概率分布
#endif
};


#endif // CONFIG_H