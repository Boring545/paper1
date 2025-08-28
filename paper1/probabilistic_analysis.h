#include"frame_packing.h"
#include"sa_algorithm.h"
#include"debug_tool.h"
#include <cmath>
#include<queue>
#include<stack>
#ifndef PROBABILISTIC_ANALYSIS_H
#define PROBABILISTIC_ANALYSIS_H

namespace cfd::schedual::paper2 {
	/*
	paper2 方法来自Probabilistic analysis of CAN with faults
	可以对已经分配优先级的帧集合进行 基于概率的 可调度性分析，最终得到每个帧可能的多个 响应时间和对应概率
	*/


	/*
		LAMBDA为每秒发生错误的次数，这个故障率反映的是系统在一个故障多发的环境下的实际表现，用来评估系统在恶劣条件下的容错能力。
		probabilistic_analysis中的epsilon使用的另一个概率要求（max_fault_rate[i]）则是在可靠性标准上提出的，
		也就是系统在长期运行中的容忍度，通常用于定义系统应当达到的“可靠性水平
	*/
	constexpr double LAMBDA = 0.03;	// 每秒30次，时间单位换算为毫秒

	// 每个CANFD帧对应一个概率分析结果
	struct ProbResult {
		double e_response_time;   // 响应时间期望
		double p_timeout;   // 超时概率
		ProbResult(double expected_response_time = 0, double prob_timeout = 0) :e_response_time(expected_response_time), p_timeout(prob_timeout) {}
	};
	struct ProbData {
		MessageID id;
		int period;
		int level;
		int type;
		double p_threshold = 0.0;    // 安全等级要求的最大容忍故障概率
		double p_timeout = 1.0;      // 添加备份的实际总故障概率，初始化为1.0
	};

	// 计算时间段t内，重传num次的概率
	double calc_probability_fault(double t, int num);
	// 计算时间段t内，高优先级帧对当前帧的干涉综合
	double calc_interference();

	// 计算并返回指定帧的概率分析结果
	ProbResult analyze_frame_probability(const CanfdFrame& frame,const std::vector<CanfdFrame>& sorted_frames,
		const std::unordered_map<FrameId, double>& max_fault_rate,
		int frame_index,double COST_MAX_ERROR_FRAME
	);
	#define BACKUP_OFF   -1  // 不备份，只分析
	#define BACKUP_DIRECT  0  // 直接备份
	#define BACKUP_REPACK  1  // 重打包
	// 对整个方案进行概率分析，返回每个帧对应的结果 enable_backup -1 不备份只分析 =0 直接备份 =1 重打包
	std::unordered_map<MessageCode, ProbData> probabilistic_analysis(PackingScheme& scheme, int enable_backup = BACKUP_OFF, std::string timestamp= get_time_stamp());

	void compare_prob_result(
		const std::string& filename,
		const std::unordered_map<MessageCode, ProbData>& origin_res,
		const std::unordered_map<MessageCode, ProbData>& repack_res,
		const PackingScheme& scheme_origin,
		const PackingScheme& scheme_repack);

	// 根据概率分析的结果，依照ASIL等级添加备份
	// ASIL A、B、C、D分别对应的故障率上限为1/10^6 、1/10^7 、1/10^7 、1/10^8 





}

#endif // !PROBABILISTIC_ANALYSIS_H
