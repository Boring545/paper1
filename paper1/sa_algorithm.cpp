#include "sa_algorithm.h"

namespace cfd::packing::heuristics {

	// fitness计算函数1
	double calculate_fitness_a(PackingScheme& scheme) {
		int schedualbility = cfd::schedual::paper1::assign_priority(scheme) == true ? 0 : 1;
		return scheme.calc_bandwidth_utilization() + schedualbility;
	}



	using FrameIndexMap = std::unordered_map<EcuPair, std::unordered_map<int, std::vector<int>>, EcuPairHash>;

	//--------------------------------- 工具函数 ---------------------------------
	// 
	// 周期为x的帧可以存放周期为valid_period_map[x]的消息,用于快速获知 一个帧 可以存放哪些周期的消息
	std::map<int, std::vector<int>> build_valid_period_map() {
		std::map<int, std::vector<int>> valid_period_map;
		for (int i = 0; i < NUM_MESSAGE_PERIOD; i++) {
			for (int j = i; j < NUM_MESSAGE_PERIOD; j++) {
				if (OPTION_MESSAGE_PERIOD[j] % OPTION_MESSAGE_PERIOD[i] == 0) {
					int N = OPTION_MESSAGE_PERIOD[j] / OPTION_MESSAGE_PERIOD[i];
					if (N <= FACTOR_M_F_PERIOD) {
						valid_period_map[OPTION_MESSAGE_PERIOD[i]].emplace_back(OPTION_MESSAGE_PERIOD[j]);
						if (N == FACTOR_M_F_PERIOD) break;
					}
				}
			}
		}
		return valid_period_map;
	}

	/**
	 * @brief 从索引里移除某个 frame_id（适用于该 frame 被清空或删除）
	 * @param fmap 维护帧索引的 map
	 * @param valid_period_map 周期映射表：frame_period -> 可放入的消息周期列表
	 * @param ecu ECU 对
	 * @param frame_period 帧的周期
	 * @param frame_id 要移除的帧 ID
	 */
	inline void index_remove_frame(FrameIndexMap& fmap,
		const std::map<int, std::vector<int>>& valid_period_map,
		const EcuPair& ecu,
		int frame_period,
		int frame_id) {
		auto vp_it = valid_period_map.find(frame_period);
		if (vp_it == valid_period_map.end()) return;

		auto ecu_it = fmap.find(ecu);
		if (ecu_it == fmap.end()) return;

		auto& by_period = ecu_it->second;
		for (int p : vp_it->second) {
			auto vec_it = by_period.find(p);
			if (vec_it == by_period.end()) continue;
			auto& vec = vec_it->second;
			// 删除 frame_id
			vec.erase(std::remove(vec.begin(), vec.end(), frame_id), vec.end());
		}
	}

	/**
	 * @brief 新增一个 frame 到索引（适用于 add_frame 之后）
	 * @param fmap 帧索引 map
	 * @param valid_period_map 周期映射表
	 * @param ecu ECU 对
	 * @param frame_period 帧周期
	 * @param frame_id 新增的帧 ID
	 */
	inline void index_add_frame(FrameIndexMap& fmap,
		const std::map<int, std::vector<int>>& valid_period_map,
		const EcuPair& ecu,
		int frame_period,
		int frame_id) {
		auto vp_it = valid_period_map.find(frame_period);
		if (vp_it == valid_period_map.end()) return;

		for (int p : vp_it->second) {
			// 向索引中增加帧 ID，允许创建不存在的键
			fmap[ecu][p].push_back(frame_id);
		}
	}

	/**
	 * @brief 尝试移动消息 msg 到新帧或已有帧
	 * @param sol 当前打包方案
	 * @param fmap 帧索引 map
	 * @param valid_period_map 周期映射表
	 * @param msg 待移动的消息
	 * @param gen 随机数生成器
	 * @param dist 随机分布 [0,1)
	 * @return 是否成功移动
	 */
	inline bool try_move_message(PackingScheme& sol,
		FrameIndexMap& fmap,
		const std::map<int, std::vector<int>>& valid_period_map,
		Message& msg,
		std::mt19937& gen,
		std::uniform_real_distribution<>& dist) {

		constexpr double PROBABILITY_NEW_FRAME = 0.01; // 移动到新帧的概率,但实际概率会比这个略高，因为尝试移动到已有帧不一定成功
		

		int ot_frm_index = msg.get_id_frame();

		// --- 1. 随机尝试移动到新帧 ---
		if (dist(gen) < PROBABILITY_NEW_FRAME) {
			// 从原帧中移除消息
			if (sol.frame_map.at(ot_frm_index).extract_message(msg)) {
				// 如果原帧为空，则删除并更新索引
				if (sol.frame_map.at(ot_frm_index).empty()) {
					index_remove_frame(fmap, valid_period_map,
						msg.get_ecu_pair(),
						sol.frame_map.at(ot_frm_index).get_period(),
						ot_frm_index);
					sol.recover_id(ot_frm_index);
					sol.frame_map.erase(ot_frm_index);
				}

				// 新增帧并更新索引
				int new_frame_id = sol.add_frame(msg);
				index_add_frame(fmap, valid_period_map,
					msg.get_ecu_pair(),
					msg.get_period(),
					new_frame_id);
				return true;
			}
			return false; // 提取消息失败
		}

		// --- 2. 尝试移动到已有帧 ---
		auto& candidates = fmap[msg.get_ecu_pair()][msg.get_period()];
		size_t RANDOM_RETRY_NUM = candidates.size()*2;         // 移动到已有帧的最大尝试次数

		if (candidates.empty()) return false; // 没有可用目标帧

		std::uniform_int_distribution<> frm_dist(0, candidates.size() - 1);
		for (int retry = 0; retry < RANDOM_RETRY_NUM; retry++) {
			int in_frm_index = candidates[frm_dist(gen)];
			if (in_frm_index == ot_frm_index) continue; // 跳过原帧

			// 尝试移动消息到目标帧
			if (sol.frame_map.at(ot_frm_index).move_message(sol.frame_map.at(in_frm_index), msg)) {
				if (sol.frame_map.at(ot_frm_index).empty()) {
					// 原帧为空，更新索引并删除
					index_remove_frame(fmap, valid_period_map,
						msg.get_ecu_pair(),
						sol.frame_map.at(ot_frm_index).get_period(),
						ot_frm_index);
					sol.recover_id(ot_frm_index);
					sol.frame_map.erase(ot_frm_index);
				}
				return true;
			}
		}
		return false;
	}

	//--------------------------------- SA 模拟退火算法 ---------------------------------

	/**
	 * @brief 使用模拟退火算法优化打包方案
	 * @param scheme 初始打包方案，函数返回最优方案
	 *
	 * 算法流程：
	 * 1. 构建 valid_period_map：周期为 x 的帧可以放入哪些周期的消息
	 * 2. 初始化随机数生成器
	 * 3. 初始化 SA 参数：初始温度、终止温度、温度衰减因子、最小移动消息数、成本缩放因子
	 * 4. 迭代：
	 *    a) 根据当前温度，随机选择一定数量的消息进行移动
	 *    b) 对每条消息，尝试移动到新帧或已有帧（调用 try_move_message）
	 *    c) 计算新方案的 cost
	 *    d) 根据 Metropolis 准则决定是否接受新方案
	 *    e) 清理空帧
	 *    f) 温度衰减
	 * 5. 返回最优解，返回带宽利用率
	 */
	double simulated_annealing(PackingScheme& scheme) {
		DEBUG_MSG_DEBUG1(std::cout, "sa 优化 打包方案");

		//----- 构建 valid_period_map
		std::map<int, std::vector<int>> valid_period_map = build_valid_period_map();

		//----- 随机数初始化
		std::random_device rd;
		std::mt19937 gen(rd());
		std::uniform_real_distribution<> dist(0.0, 1.0);

		//----- SA参数初始化
		constexpr double INITIAL_TEMPERATURE = 10;  // 初始温度
		constexpr double FINAL_TEMPERATURE = 0.01;  // 收敛终止温度
		constexpr double ALPHA = 0.995;             // 温度衰减因子
		constexpr int NUM_MIN_MOVE = 1;             // 每次至少移动消息数
		constexpr double FACTOR_COST_SCALE = 1000.0;// 成本差值放大系数，值越大收敛越快，探索程度越低；值越小，收敛越慢，搜索更深

		double current_temperature = INITIAL_TEMPERATURE;

		PackingScheme best_solution = scheme;
		PackingScheme pre_solution = scheme;

		double pre_cost = calculate_fitness_a(best_solution);
		double new_cost = pre_cost;
		double best_cost = pre_cost;

		//----- SA过程
		while (current_temperature > FINAL_TEMPERATURE) {
			PackingScheme new_solution = pre_solution;

			// 构建 frame_index_map，用于快速找到同 ECU 对下可用帧
			FrameIndexMap frame_index_map;
			for (auto& [fid, frame] : new_solution.frame_map) {
				if (frame.empty()) continue;
				for (auto& p : valid_period_map[frame.get_period()]) {
					frame_index_map[frame.get_ecu_pair()][p].emplace_back(frame.get_id());
				}
			}

			// 根据温度决定移动消息数量，高温移动多，低温移动少
			double num_probability = current_temperature / INITIAL_TEMPERATURE;
			int max_select_num = new_solution.message_set.size() / 10;
			int select_num = static_cast<int>(max_select_num * num_probability);
			if (select_num < NUM_MIN_MOVE) select_num = NUM_MIN_MOVE;

			std::uniform_int_distribution<> msg_dist(0, new_solution.message_set.size() - 1);

			//----- 随机尝试移动 select_num 个消息
			for (int i = 0; i < select_num; i++) {
				int mid = msg_dist(gen);
				auto& msg = new_solution.message_set[mid];
				try_move_message(new_solution, frame_index_map, valid_period_map, msg, gen, dist);
			}

			new_cost = calculate_fitness_a(new_solution);

			//----- 判断是否接受新解（Metropolis 准则）
			if (new_cost <= pre_cost) {
				if (new_cost <= best_cost) {
					best_solution = new_solution;
					best_cost = new_cost;
				}
				pre_solution = std::move(new_solution);
				pre_cost = new_cost;
			}
			else {
				double acceptance_probability =
					std::exp(-(new_cost - pre_cost) * FACTOR_COST_SCALE / current_temperature);
				if (dist(gen) < acceptance_probability) {
					pre_solution = std::move(new_solution);
					pre_cost = new_cost;
				}
			}

			//----- 清理空帧，回收 ID
			std::vector<int> to_remove;
			for (const auto& [key, frame] : pre_solution.frame_map) {
				if (frame.empty()) {
					to_remove.push_back(key);
				}
			}
			for (auto i : to_remove) {
				pre_solution.recover_id(i);
				pre_solution.frame_map.erase(i);
			}

			//----- 打印调试信息
			DEBUG_MSG_DEBUG2(std::cout, "Fitness = ", pre_cost);
			DEBUG_MSG_DEBUG2(std::cout, "Utilization = ", pre_solution.calc_bandwidth_utilization());
			DEBUG_MSG_DEBUG2(std::cout, "Temperature = ", current_temperature, "\n");

			// 温度衰减
			current_temperature *= ALPHA;
		}

		// 返回最优解
		scheme = std::move(best_solution);
		double utilization = scheme.calc_bandwidth_utilization();
		DEBUG_MSG_DEBUG1(std::cout, "Utilization = ", utilization);
		DEBUG_MSG_DEBUG1(std::cout, "sa 优化 打包方案 完毕");
		return utilization;
	}




}


