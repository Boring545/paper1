#pragma once
#include<unordered_map>
#include"../canfd_frame.h"
#include"../scheme.h"
#include"../config.h"

namespace cfd::analysis {
	
	// 计算时间段t内，重传num次的概率
	double prob_fault(double t, int num, double lambda = LAMBDA_CONFERENCE);
	// 在电磁干扰的干涉窗口内，至少发生一次故障的概率
	double prob_fault_one_more(double interference_win, double lambda = LAMBDA_CONFERENCE);

	std::unordered_map<MessageCode, double> probabilistic_analysis(PackingScheme& scheme, double lambda = LAMBDA_CONFERENCE);

}

