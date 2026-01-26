#ifndef PACKINGSCHEME_H
#define PACKINGSCHEME_H

#include<unordered_set>
#include"canfd_frame.h"


namespace cfd {
	constexpr double LIMIT_BANDWIDTH = 0.9;	// 带宽利用率上限
	class PackingScheme;




	//一个打包方案，包含一组装载了全部消息的CANFD帧
	class PackingScheme {
	private:

		using PeriodFrameeMap = std::map<int, std::vector<int>>;		// 相同ECUpair下，不同周期对应的frame索引分表,通过frame_set[索引]选取frame
		using EcuToFrameMap = std::unordered_map<EcuPair, PeriodFrameeMap, EcuPairHash>;	// 根据【ECUpair，period】选择PeriodFrameeMap分表

		using PeriodMessageMap = std::map<int, std::vector<size_t>>;		// 相同ECUpair下，不同周期对应的message索引分表,通过message_set[索引]选取message
		using EcuPeriodMessageMap = std::unordered_map<EcuPair, PeriodMessageMap, EcuPairHash>;	// 根据【ECUpair，period】选择PeriodMessageMap分表

		std::unordered_set<int> free_ids;  // 可复用的ID池,给帧标号用,加入新帧时从池里选择id，帧清空时id返回池，池为空则分配frame_map.size()作为id

		// 根据消息集合生成初始化的帧集合，作为生成初始打包方案
		bool init_frames();






	public:

		MessageVec message_set;  // 信号集合

		CanfdFrameMap frame_map;	// key为 frame的id，id应该唯一，分配id时注意使用get_free_id()获取唯一的id

		//获取一个可用帧id,取必用！
		int get_free_id();
		//回收帧的id
		void recover_id(int id);

		// 重新初始化，生成初始打包方案
		bool re_init_frames() {
			for (auto& m : message_set) {
				m.clear_frame();
			}
			frame_map.clear();
			return init_frames();
		}


		// 增加一个新帧，其只包含一个基准msg，返回新帧id
		int add_frame(Message& msg) {
			FrameId  id = get_free_id();
			if (id == -1) {
				std::cout << "id error\n";
			}
			auto result = this->frame_map.try_emplace(id, id, msg);
			int offset = 0;

			this->frame_map[id].set_offset(offset);

			if (result.second) {
				msg.assign_frame(id);
				return id;
			}
			else {
				return -1;
			}
		}

		// 增加一个新帧，其只包含一个基准msg，返回新帧id
		int add_frame(int _period, int _deadline, const EcuPair& _ecu_pair, int _offset) {
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



		//计算带宽利用率
		double calc_bandwidth_utilization()const;
		void print_frame() {
			cfd::utils::print_frame(this->frame_map);
		}

		PackingScheme(bool backup=true, MessageInfoVec& vec = cfd::MESSAGE_INFO_VEC) {
			int original_size = vec.size();
			// 添加异源备份
			// if (NUM_ECU >= 3 && backup == true) {
				
			// 	for (int i = 0; i < original_size; i++) {
			// 		if (vec[i].type == 1) {
			// 			//TODO 增加两个异源副本到MESSAGE_INFO_VEC
			// 			int origin_src_ecu = vec[i].ecu_pair.src_ecu;

			// 			// 顺序挑选两个不同的 ECU
			// 			int new_ecu1 = -1, new_ecu2 = -1;
			// 			for (int e : OPTION_ECU) {
			// 				if (e != origin_src_ecu) {
			// 					if (new_ecu1 == -1)
			// 						new_ecu1 = e;
			// 					else if (new_ecu2 == -1) {
			// 						new_ecu2 = e;
			// 						break; // 已经找到两个，退出
			// 					}
			// 				}
			// 			}

			// 			MessageInfo backup1(vec[i], new_ecu1);
			// 			MessageInfo backup2(vec[i], new_ecu2);
			// 			vec.push_back(backup1);
			// 			vec.push_back(backup2);
			// 		}
			// 	}
			// 	DEBUG_MSG_DEBUG1(std::cout, "总计新增 ", vec.size() - original_size, " 个异源信号副本");
			// }

			
			

			for (size_t i = 0; i < vec.size(); i++) {
				message_set.emplace_back(i);
			}
			init_frames();

		}

		// 用于从fmap表示的打包方案初始化一个PackingScheme类
		PackingScheme(const CanfdFrameMap& fmap) {
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
		PackingScheme(const PackingScheme& other) {
			this->message_set = other.message_set;
			this->frame_map = other.frame_map;
			this->free_ids = other.free_ids;
		}
		PackingScheme(PackingScheme&& other)noexcept {
			this->message_set = std::move(other.message_set);
			this->frame_map = std::move(other.frame_map);
			this->free_ids = std::move(other.free_ids);
		}
		PackingScheme& operator=(const PackingScheme& other) {
			this->message_set = other.message_set;
			this->frame_map = other.frame_map;
			this->free_ids = other.free_ids;
			return *this;
		}
	};
}

namespace cfd::packing {
	enum class PACK_METHOD {
		SIMULATED_ANNEALING=0, // SA 打包
	};
	// 对初始化后的scheme进行打包
	double frame_pack(PackingScheme& scheme, PACK_METHOD method);
}


#endif // !FRAMEPACKING_H