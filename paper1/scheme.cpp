#include"priority_allocation.h"
#include"packing_algorithms/sa_algorithm.h"
#include "scheme.h"
namespace cfd {
	//计算带宽利用率



	double PackingScheme::calc_bandwidth_utilization()const {
		double U = 0;
		for (const auto& [key, frame] : frame_map) {
			if (!frame.empty()) {
				U += frame.get_trans_time() / frame.get_period();
			}

		}
		return U;
	}


	// 用于从fmap表示的打包方案初始化一个PackingScheme类

	PackingScheme::PackingScheme(const CanfdFrameMap& fmap) {
		frame_map = fmap;

		int size = MESSAGE_INFO_VEC.size();
		message_set.reserve(size);
		for (int i = 0; i < size; i++) {
			message_set.emplace_back();
		}

		FrameId max_id = 0;
		free_ids.clear();
		std::unordered_set<int> used_ids;

		for (auto& [key, frame] : fmap) {
			used_ids.insert(key);
			if (key > max_id) {
				max_id = key;
			}
			for (auto& msg : frame.msg_set) {
				message_set[msg.get_id_message()] = msg;
			}
		}

		for (int i = 0; i <= max_id; ++i) {
			if (used_ids.count(i) == 0) {
				free_ids.insert(i);  // i 是空闲 ID
			}
		}

	}

	PackingScheme::PackingScheme(const PackingScheme& other) {
		this->message_set = other.message_set;
		this->frame_map = other.frame_map;
		this->free_ids = other.free_ids;
	}

	PackingScheme::PackingScheme(PackingScheme&& other) noexcept {
		this->message_set = std::move(other.message_set);
		this->frame_map = std::move(other.frame_map);
		this->free_ids = std::move(other.free_ids);
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
					FrameId frame_index = new_frame(message_set[temp_set.back()]);
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





		if (calc_bandwidth_utilization() >= 1.0) {
			DEBUG_MSG_DEBUG1(std::cerr, "ERROR", "信号类型太多，导致帧过多，传输困难", " U = ", calc_bandwidth_utilization());
			return false;
		}

		if (!cfd::schedule::assign_priority_by_period(this->frame_map)) {
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

	// 重新初始化，生成初始打包方案

	bool PackingScheme::re_init_frames() {
		for (auto& m : message_set) {
			m.clear_frame();
		}
		frame_map.clear();
		return init_frames();
	}

	// 增加一个新帧，其只包含一个基准msg，返回新帧id

	int PackingScheme::new_frame(Message& msg) {
		FrameId  id = get_free_id();
		if (id == -1) {
			std::cout << "id error\n";
		}
		auto result = this->frame_map.try_emplace(id, id, msg);

		this->frame_map[id].set_offset(0);

		if (result.second) {
			msg.assign_frame(id);
			return id;
		}
		else {
			return -1;
		}
	}

	// 增加一个新帧，其只包含一个基准msg，返回新帧id

	int PackingScheme::new_frame(int _period, int _deadline, const EcuPair& _ecu_pair, int _offset) {
		FrameId  id = get_free_id();
		if (id == -1) {
			std::cout << "id error\n";
		}
		auto result = this->frame_map.try_emplace(id, id, _period, _deadline, _ecu_pair, _offset);
		if (result.second) {
			return id;
		}
		else {
			return -1;
		}
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