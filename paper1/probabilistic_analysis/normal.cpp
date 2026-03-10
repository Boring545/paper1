#include"probabilistic_analysis/normal.h"
#include <stdexcept>
#include<cmath>

#include "../scheme.h"
#include "../debug_tool.h"
namespace cfd::analysis {

	// 计算阶乘
	long long int factorial(int m)
	{
		// 别用它计算太大的数
		if (m < 0)
		{
			throw std::invalid_argument("M 必须大于0");
		}
		if (m == 0)
		{
			return 1;
		}`
		if (m > 20)
		{
			throw std::invalid_argument("M>20");
		}
		long long int result = 1;
		while (m > 0)
		{
			result *= m;
			m--;
		}
		return result;
	}
	double prob_fault(double t, int num, double lambda)
	{
		return std::exp(-1 * lambda * t) * std::pow(lambda * t, num) / factorial(num);
	}

	double prob_fault_one_more(double interference_win, double lambda)
	{
		return 1 - prob_fault(interference_win, 0, lambda);
	}



	std::unordered_map<MessageCode, double> probabilistic_analysis(PackingScheme& scheme,double lambda) {

		std::unordered_map<MessageCode, double> fault_prob_map;
		for (const auto& [id, frame] : scheme.frame_map) {
			int win = frame.get_trans_time();

			for (const auto& msg : frame.msg_set) {
				if (fault_prob_map.find(msg.get_code()) == fault_prob_map.end()){
					fault_prob_map[msg.get_code()] = prob_fault_one_more(win, lambda);
				}
				else {
					fault_prob_map[msg.get_code()] *= prob_fault_one_more(win, lambda);
				}
			}
		}
		return fault_prob_map;
	}



}