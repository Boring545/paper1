#include"packing_scheme.h"
#include"priority_allocation.h"
#include"packing_algorithms/sa_algorithm.h"
namespace cfd {
	//计算带宽利用率



	double PackingScheme::calc_bandwidth_utilization()const {
		double U = 0;
		for (const auto& [key,frame] : frame_map) {
			if (!frame.empty()) {
				U += frame.get_trans_time() / frame.get_period();
			}

		}
		return U;
	}



	bool PackingScheme::init_frames()
	{
		free_ids.clear();
		EcuPeriodMessageMap period_msg_map; //message按照ECU对、period的二级分表
		for (size_t i = 0; i < message_set.size(); ++i) {
			const auto& m = message_set[i];
			period_msg_map[m.get_ecu_pair()][m.get_period()].emplace_back(i);
		}

		//frame_set.reserve(100);
		//每次遍历处理一对ecu的单向传输
			// 遍历 ECU 对
		for (auto& ecu_map : period_msg_map) {
			// 遍历每个 period
			for (auto& period_map : ecu_map.second) {
				auto& temp_set = period_map.second;

				while (!temp_set.empty()) {
					// 随便取一个 message 新建一个 frame
					FrameId frame_index = add_frame(message_set[temp_set.back()]);
					temp_set.pop_back();

					// 尝试把剩下的 message 往这个 frame 里塞
					for (auto it = temp_set.begin(); it != temp_set.end();) {
						if (frame_map[frame_index].add_message(message_set[*it])) {
							it = temp_set.erase(it);
						}
						else {
							++it;
						}
					}
				}
			}
		}

#ifdef OFFSET_TEST
		std::random_device rd;
		std::mt19937 gen(rd());

		for (auto& [id, frame] : frame_map) {
			int period = frame.get_period(); // 获取当前帧的周期
			// 随机分配 [0, period-1) 范围内的自然数
			std::uniform_int_distribution<int> offset_dist(0, period - 1); // 左闭右闭，范围 0 到 period-1
			int offset = offset_dist(gen);
			frame.set_offset(offset);
		}
#endif // OFFSET_TEST



		if (calc_bandwidth_utilization() > 0.95) {
			DEBUG_MSG_DEBUG1(std::cerr, "ERROR", "信号类型太多，导致帧过多，传输困难", " U = ", calc_bandwidth_utilization());
			return false;
		}

		if (!cfd::schedule::paper1::assign_priority(this->frame_map)) {
			DEBUG_MSG_DEBUG1(std::cerr, "ERROR", "初始方案无法分配优先级，无法调度");
			return false;
		}

		return true;
	}
	int PackingScheme::get_free_id()
	{
		int new_id = -1;
		// 优先从池中获取可复用的 ID
		if (!free_ids.empty()) {
			new_id = *free_ids.begin();
			free_ids.erase(free_ids.begin());
		}
		// 池为空时，分配新 ID（当前 map 的 size）
		else {
			new_id = this->frame_map.size();
		}
		return new_id;
	}
	void PackingScheme::recover_id(int id)
	{
		if (id == -1) {
			std::cout << "999999999999999999999\n";
		}
		auto it = frame_map.find(id);
		if (it == frame_map.end()) {
			throw std::out_of_range("Frame ID not found!");
		}

		free_ids.insert(id);  // 回收 ID
	}


}

double cfd::packing::frame_pack(PackingScheme& scheme, PACK_METHOD method) {

	double utilization = 0;
	switch (method) {
	case PACK_METHOD::SIMULATED_ANNEALING:
		utilization = heuristics::simulated_annealing(scheme);
		break;
	default:
		break;
	}

	return utilization;
}