#include"frame_packing.h"
#include"debug_tool.h"
#include"priority_allocation.h"
#ifndef  SAALGORITHM_H
#define SAALGORITHM_H
namespace cfd::packing::heuristics {

	// 方法A：计算fitness=fitness=U+可调度性（0可调度，1不可调度）
	double calculate_fitness_a(PackingScheme& solution);
	//SA 模拟退火算法 以scheme中的打包方案为初始解，迭代计算近似最优的打包方案，返回带宽利用率
	double simulated_annealing(PackingScheme& scheme);

}


#endif // ! SAALGORITHM_H
