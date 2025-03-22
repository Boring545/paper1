#include"frame_packing.h"
namespace cfd {
	//计算带宽利用率

	double PackingScheme::calc_bandwidth_utilization() {
		double U = 0;
		for (auto& frame : frame_set) {
			U += frame.get_trans_time() / frame.get_period();
		}
		return U;
	}
	void PackingScheme::initial_frames()
	{
		//frame_set.reserve(100);
		//每次遍历处理一对ecu的单向传输
		for (auto& ecu_map : period_msg_map) {
			//每次遍历处理一组period的消息打包
			for (auto& period_map : ecu_map.second) {
				//索引set，存储索引，按照 Message 的 offset 排序
				std::set<int, ComparatorIndexByValue> temp_set(ComparatorIndexByValue(message_set, period_map.second));
				// 插入索引
				for (int i = 0; i < period_map.second.size(); ++i) {
					temp_set.insert(i);
				}
				//根据消息分段方法遍历消息集合，向帧中填充，直至全部装载完毕
				auto mit = temp_set.begin();	//当前分析的消息迭代器
				while (mit != temp_set.end()) {
					auto next_mit = std::next(mit);	//下一个迭代器，为了避免mit删除后失效
					FrameId frame_index = frame_set.size();	//标记当前正在装载的frame
					int left = message_set[period_map.second[*mit]].get_offset();		//当前分析的消息mit对应的有效区间左边界
					int right = left + message_set[period_map.second[*mit]].get_deadline() * FACTOR_MSG_WAIT_WINDOW;

					frame_set.emplace_back(frame_index, message_set[period_map.second[*mit]]);	// 创建一个帧，同时向帧中插入当前消息mit，此操作一定能成功
					temp_set.erase(mit);											// 删除已经插入的消息mit

					//向当前分析的帧frame_set[frame_index]反复插入message
					while (next_mit != temp_set.end() && message_set[period_map.second[*next_mit]].get_offset() <= right) {
						mit = next_mit;
						next_mit = std::next(mit);
						bool inserted = frame_set[frame_index].add_message(message_set[period_map.second[*mit]], SIZE_LIMIT_CANFD_DATA);
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
}

