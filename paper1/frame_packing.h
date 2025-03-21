#include"canfd_frame.h"

#ifndef FRAMEPACKING_H
#define FRAMEPACKING_H

namespace cfd {

	struct ComparatorIndexByValue {
		const MessageVec& vec;  

		// 构造函数，传入数组指针
		ComparatorIndexByValue(const MessageVec& v) : vec(v) {}

		bool operator()(int idx1, int idx2) const {
			return vec[idx1].get_offset() < vec[idx2].get_offset();  // 根据 offset 排序
		}
	};


	//一个打包方案，包含一组装载了全部消息的CANFD帧
	class PackingScheme {
		using PeriodMessageMap = std::map<int, MessageVec>;	// 相同ECUpair下，不同周期对应的message索引分表
		using EcuToFrameMap = std::unordered_map<EcuPair, std::vector<int>>;	// CANFD帧分表（哈希表）：键是源ECU和目的ECU的组合，值是对应的frame的数组索引
		using EcuPeriodMessageMap = std::unordered_map<EcuPair, PeriodMessageMap, EcuPairHash>;	// message按照ECU对、period的二级分表

	private:
		MessageVec message_set;
		FrameVec frame_set;

		EcuPeriodMessageMap period_msg_map; //message按照ECU对、period的二级分表


	public:
		//计算带宽利用率
		double calc_bandwidth_utilization() {
			double U = 0;
			for (auto& frame : frame_set) {
				U += frame.get_trans_time() / frame.get_period();
			}
			return U;
		}

		PackingScheme(const MessageInfoVec& vec) {
			size_t n = vec.size();
			for (size_t i = 0; i < n;++i) {
				message_set.emplace_back(i);
				period_msg_map[vec[i].ecu_pair][vec[i].period].emplace_back(i);// 按ecu对和周期将消息分组
			}
			initial_frames();

		}
		// 根据消息集合生成一个初始帧列表，TODO 生成一张ECU对的分表
		void initial_frames() {
			int period = 0;
			int frame_id = 0;

			//每次遍历处理一对ecu的单向传输
			for (auto& ecu_map : period_msg_map) {
				//每次遍历处理一组period的消息打包
				for (auto& period_map : ecu_map.second) {
					period = period_map.first;
					//索引set，存储索引，按照 Message 的 offset 排序
					std::set<int, ComparatorIndexByValue> temp_set(ComparatorIndexByValue(period_map.second));
					// 插入索引
					for (int i = 0; i < period_map.second.size(); ++i) {
						temp_set.insert(i);
					}
					//根据消息分段方法遍历消息集合，向帧中填充，直至全部装载完毕
					auto mit = temp_set.begin();	//当前分析的消息迭代器
					while (mit != temp_set.end()) {
						auto next_mit = std::next(mit);	//下一个迭代器，为了避免mit删除后失效
						int frame_index = frame_set.size();	//标记当前正在装载的frame
						int left = period_map.second[*mit].get_offset();		//当前分析的消息mit对应的有效区间左边界
						int right = left + period_map.second[*mit].get_deadline() * FACTOR_MSG_WAIT_WINDOW;

						frame_set.emplace_back(frame_id++, period_map.second[*mit]);	// 创建一个帧，同时向帧中插入当前消息mit，此操作一定能成功
						temp_set.erase(mit);											// 删除已经插入的消息mit
						
						//向当前分析的帧frame_set[frame_index]反复插入message
						while (next_mit != temp_set.end() && period_map.second[*next_mit].get_offset() <= right) {
							mit = next_mit;
							next_mit = std::next(mit);
							bool inserted = frame_set[frame_index].add_message(period_map.second[*mit], SIZE_LIMIT_CANFD_DATA);
							if (inserted) {
								// 如果消息成功插入，删除该消息，插入失败则跳过该消息
								temp_set.erase(mit);
							}
						}
						mit = temp_set.begin();//frame装载完毕，重选要考虑的基准message
					}
				}

				
			}
		}


	};
}




#endif // !FRAMEPACKING_H