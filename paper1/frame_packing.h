#include"canfd_frame.h"

#ifndef FRAMEPACKING_H
#define FRAMEPACKING_H

namespace cfd {

	struct ComparatorIndexByValue {
		const MessageVec& mvec;  
		const std::vector<int>& vec;
		// 构造函数，传入数组指针
		ComparatorIndexByValue(const MessageVec& mv, const std::vector<int>& v) : mvec(mv),vec(v) {}

		bool operator()(int idx1, int idx2) const {
			if (mvec[vec[idx1]].get_offset() != mvec[vec[idx2]].get_offset()) {
				return mvec[vec[idx1]].get_offset() < mvec[vec[idx2]].get_offset();
			}
			return idx1 < idx2;  // offset相同，按索引升序
		}
	};


	//一个打包方案，包含一组装载了全部消息的CANFD帧
	class PackingScheme {
		using PeriodFrameeMap = std::map<int, std::vector<int>>;		// 相同ECUpair下，不同周期对应的frame索引分表,通过frame_set[索引]选取frame
		using EcuToFrameMap = std::unordered_map<EcuPair, PeriodFrameeMap, EcuPairHash>;	// 根据【ECUpair，period】选择PeriodFrameeMap分表

		using PeriodMessageMap = std::map<int, std::vector<int>>;		// 相同ECUpair下，不同周期对应的message索引分表,通过message_set[索引]选取message
		using EcuPeriodMessageMap = std::unordered_map<EcuPair, PeriodMessageMap, EcuPairHash>;	// 根据【ECUpair，period】选择PeriodMessageMap分表

	public:
		MessageVec message_set;
		CanfdFrameVec frame_set;

		EcuPeriodMessageMap period_msg_map; //message按照ECU对、period的二级分表

		PackingScheme(const MessageInfoVec& vec) {
			size_t n = vec.size();
			for (size_t i = 0; i < n; ++i) {
				message_set.emplace_back(i);
				period_msg_map[vec[i].ecu_pair][vec[i].period].emplace_back(i);// 按ecu对和周期将消息分组
			}
			initial_frames();

		}
		//计算带宽利用率
		double calc_bandwidth_utilization();

		// 根据消息集合生成一个初始帧列表，TODO 生成一张ECU对的分表
		void initial_frames();


	};
}




#endif // !FRAMEPACKING_H