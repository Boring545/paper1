#include<memory>
#include<vector>
#include<set>
#include<unordered_set>
#include<unordered_map>
#include<map>
#include <mutex>
#include <random>
#include<iostream>
#include <fstream>
#include <sstream>
#include<iomanip>
#include <array>
#include <optional>
#include <functional>

#ifndef CANFDFRAME_H
#define CANFDFRAME_H



namespace cfd {
	constexpr int SIZE_MAX_CANFD_DATA = 64*8;			// CANFD帧 最大数据负载，单位为b
	constexpr int SIZE_LIMIT_CANFD_DATA = 48*8;			// CANFD帧 最大数据负载，单位为b
	constexpr int SIZE_IDENTIFIER = 11;					// CANFD帧 标识符长度，标准帧的11位标识符，扩展帧可以有29位
	constexpr int FACTOR_M_F_PERIOD = 2;				// 消息打包时，所允许的 消息周期/帧周期 的最大倍数
	constexpr double FACTOR_TIME_MSG_USABLE_WAIT=0.25;	// 消息有效等待时间的因子
	constexpr double FACTOR_MSG_WAIT_WINDOW = 0.5;		// 消息有效等待区间因子
	constexpr double TIME_BIT_ARBITRATION = 1;			// 传输一个bit所用时间，单位为微秒(μs)（1Mbps）
	constexpr double TIME_BIT_TRANSMISSION = 0.2;		// 传输一个bit所用时间，单位为微秒(μs)（5Mbps）


	constexpr int OPTION_MESSAGE_SIZE[]  = { 1, 2, 4, 8, 16, 32, 64 };									// 信号尺寸选项
	constexpr int NUM_MESSAGE_SIZE = std::size(OPTION_MESSAGE_SIZE);
	constexpr double PROBABILITY_MESSAGE_SIZE[] = { 0.35, 0.49, 0.13, 0.008, 0.013, 0.005, 0.002 };	// 选择概率
	
	constexpr int OPTION_MESSAGE_PERIOD[] = { 1, 2, 5, 10, 20, 50, 100 };								// 周期大小选项
	constexpr int NUM_MESSAGE_PERIOD = std::size(OPTION_MESSAGE_PERIOD);
	constexpr double PROBABILITY_MESSAGE_PERIOD[] = { 0.03, 0.02, 0.02, 0.25, 0.25, 0.03, 0.2 };		// 选择概率

	constexpr int OPTION_MESSAGE_ECU[] = { 0,1,2,3,4,5 };												// ecu集合选项
	constexpr int NUM_MESSAGE_ECU = std::size(OPTION_MESSAGE_ECU);

	constexpr int OPTION_MESSAGE_LEVEL[] = { 0, 1, 2 };												// 安全等级选项
	constexpr int NUM_MESSAGE_LEVEL = std::size(OPTION_MESSAGE_LEVEL);
	constexpr double PROBABILITY_MESSAGE_LEVEL[3] = { 0.85,0.1,0.05 };									// 选择概率


	using EcuId = size_t;
	using FrameId = size_t;
	using MessageCode = size_t;

	struct EcuPair {
		EcuId src_ecu = 0;        // 源ECU
		EcuId dst_ecu = 0;        // 目ECU
		EcuPair(int src = 0, int dst = 0)
			: src_ecu(src), dst_ecu(dst) {}
		bool  operator==(const EcuPair& other) const {
			return (src_ecu == other.src_ecu) && (dst_ecu == other.dst_ecu);
		}
		bool  operator!=(const EcuPair& other) const {
			return (src_ecu != other.src_ecu) || (dst_ecu != other.dst_ecu);
		}
	};
	struct EcuPairHash {
		size_t operator()(const EcuPair& key) const {
			return std::hash<int>()(key.src_ecu) ^ (std::hash<int>()(key.dst_ecu) << 1);
		}
	};


	//存储一个消息的基本信息
	class MessageInfo {
	public:
		MessageCode code = 0;	//消息的身份码
		int data_size = 0;		// 数据长度，默认为空,单位为b，取值为[0, 512]
		int period = -1;        // 周期，单位为微秒(μs)
		int deadline = -1;      // 时限，同周期
		EcuPair ecu_pair;		// 源、目ECU对
		int offset = 0;         // 偏移，单位为微秒(μs)，
		int level = 0;			//安全优先级 
		MessageInfo(MessageCode _code,int _data_size, int _period, int _deadline, int _src_ecu, int _dst_ecu, int _offset = 0, int _level = 0)
			:code(_code),data_size(_data_size), period(_period), deadline(_deadline), ecu_pair(_src_ecu, _dst_ecu), offset(_offset), level(_level) {}

		~MessageInfo() {}
	};
	using MessageInfoVec = std::vector<MessageInfo>;
	extern MessageInfoVec message_info_vec;//全局维护一个message info 表

	//消息实体类，每个消息有一个隐式的id，即其对应的info在数组中的index
	class Message {
	private:
		size_t message_index = -1;		//	指向对应的MessageInfo

		size_t frame_index = -1;	//	指向对应的Frame
	public:
		void assign_frame(FrameId fid) {
			frame_index = fid;
		}
		size_t get_id_message() const { return message_index; }
		size_t get_id_frame() const { return frame_index; }

		Message(size_t m_index):message_index(m_index){}
		Message(size_t m_index, size_t f_index) :message_index(m_index), frame_index(f_index){}

		int get_period() const {
			return message_info_vec[message_index].period;
		}
		int get_data_size() const {
			return message_info_vec[message_index].data_size;
		}
		int get_deadline() const {
			return message_info_vec[message_index].deadline;
		}
		MessageCode get_code() const {
			return message_info_vec[message_index].code;
		}
		EcuPair get_ecu_pair() const {
			return message_info_vec[message_index].ecu_pair;
		}
		int get_offset() const {
			return message_info_vec[message_index].offset;
		}
		int get_level() const {
			return message_info_vec[message_index].level;
		}
	};
	using MessageVec = std::vector<Message>;


	// 按 offset 升序 的Message比较器
	struct ComparatorMsgOffsetAscend {
		bool operator()(const MessageInfo& lhs, const MessageInfo& rhs) const {
			return lhs.offset < rhs.offset;
		}
		bool operator()(const Message& lhs, const Message& rhs) const {
			return lhs.get_offset() < rhs.get_offset(); 
		}
	};

	class CanfdFrame {
	private:
		
		int data_size = 0;      // 装载的数据长度，默认为空,单位为b，取值为[0, 512]
		int payload_size = 0;   // payload尺寸和数据尺寸不完全一样，取值有0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48 or 64 bytes
		int priority = -1;      // 优先级，取值为[0,2047],数字越小优先级越高。【仅考虑标准帧的11位标识符，扩展帧可以有29位】
		int period = -1;        // 周期，单位为微秒(μs)
		int deadline = -1;      // 时限，同周期
		int offset = 0;         // 偏移，单位为微秒(μs)，可以使用assign_offset方法分配frame集合中所有任务的合适offset
		EcuPair ecu_pair;
		double trans_time = 0;   // 数据帧在系统内的传输时间

	

		// 将数据尺寸（单位b）转换为合适的payload尺寸（单位byte），payload取值有0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48 or 64 bytes
		int payload_size_trans(int size);
		// 根据paylaod_size计算最坏传输时间
		double calc_wctt(int paylaod_size);

		bool set_priority(int v) {
			if (v > 0 && v < (2 << SIZE_IDENTIFIER)) {
				this->priority = v;
				return true;
			}
			else {
				return false;
			}
		}
		bool set_data_size(int v) {
			if (v < 0 || v > SIZE_MAX_CANFD_DATA) {
				return false;
			}
			else {
				update_with_data_size(v);
				return true;
			}
		}
		//bool update_trans_time() {
		//	this->trans_time = calc_wctt(this->payload_size);
		//}
		
		//根据同步data_size更新data_size、payload_size,exec_time
		void update_with_data_size(int size) {
			this->data_size = size;
			this->payload_size = payload_size_trans(size);
			this->trans_time = calc_wctt(this->payload_size);
		}

	public:
		FrameId id = 0;//id由生成时顺序给出 

		std::set<Message, ComparatorMsgOffsetAscend> msg_set;//CANFD帧装载的消息 的指针， 按 offset 升序排列

		//获取所装载消息中最小的offset
		int get_min_msg_offset();
		//获取所装载消息中最大的offset
		int get_max_msg_offset();





		bool set_period(int v) {
			if (v > 0) {
				this->period = v;
				return true;
			}
			else {
				return false;
			}

		}
		bool set_deadline(int v) {
			if (v > 0) {
				this->deadline = v;
				return true;
			}
			else {
				return false;
			}

		}
		bool set_offset(int v) {
			if (v > 0 && v < deadline) {
				this->offset = v;
				return true;
			}
			else {
				return false;
			}
		}
		bool set_ecu(EcuPair v) {
			if (v.src_ecu != v.dst_ecu) {
				ecu_pair = v;
				return true;
			}
			else {
				return false;
			}
		}


		int get_priority() const {
			return this->priority;
		}
		int get_period() const {
			return this->period;
		}
		int get_paylaod_size() const {
			return this->payload_size;
		}
		int get_deadline() const {
			return this->deadline;
		}
		double get_trans_time() const {
			return this->trans_time;
		}
		EcuPair get_ecu_pair() const {
			return this->ecu_pair;
		}

		int get_offset() const {
			return this->offset;
		}

		//向 CANFD帧 添加 单个消息m，同步更新data_size、payload_size，deadline、period
		bool add_message(Message& m, int limit_size = SIZE_MAX_CANFD_DATA);

		//向 CANFD帧 添加 消息集合mset，同步更新data_size、payload_size，deadline
		bool add_message(MessageVec& s);

		//向 CANFD帧 frame中的消息移动到当前帧中
		bool add_frame(CanfdFrame&& frame);

		CanfdFrame(FrameId _id,Message& msg) {
			this->id = _id;

			this->period = msg.get_period();
			this->deadline = msg.get_deadline();
			this->ecu_pair = msg.get_ecu_pair();
			this->offset = msg.get_offset();
			update_with_data_size(msg.get_data_size());

			msg_set.insert(msg);
		}

		CanfdFrame() {}
		CanfdFrame(int _id) {
			this->id = _id;
		}
		CanfdFrame(int _offset, int _exec, int _deadline, int _period, int _id) {
			offset = _offset;
			trans_time = _exec;
			deadline = _deadline;
			period = _period;
			id = _id;
		}
		CanfdFrame(const CanfdFrame& other) {
			msg_set = other.msg_set;

			data_size = other.data_size;
			payload_size = other.payload_size;
			deadline = other.deadline;
			period = other.period;
			priority = other.priority;
			trans_time = other.trans_time;
			id = other.id;
			offset = other.offset;
		}
		CanfdFrame(CanfdFrame&& other) noexcept {
			msg_set = std::move(other.msg_set);

			data_size = other.data_size;
			payload_size = other.payload_size;
			deadline = other.deadline;
			period = other.period;
			priority = other.priority;
			trans_time = other.trans_time;
			id = other.id;
			offset = other.offset;
		}
		~CanfdFrame() {
		}


		void clear() {
			this->msg_set.clear();

			data_size = 0;
			payload_size = 0;
			deadline = -1;
			period = -1;
			priority = -1;
			trans_time = 0.0;
			id = -1;
			offset = 0;
		}
	};
	using FrameVec = std::vector<CanfdFrame>;

	class CanfdUtils {
	public:

		//从 文件file 读取 消息info集合mset
		static void read_messages(MessageInfoVec& mset, const std::string& file);

		//向 流os 写入表头的工具函数
		static void write_heading_to_stream(std::ostream& os);
		//向 流os 写入 单个消息m，heading=true表示输出表头
		static void write_msg_to_stream(std::ostream& os, const MessageInfo& msg, bool heading = true);
		//向 流os 写入 消息集合mset，heading=true表示输出表头
		static void write_mset_to_stream(std::ostream& os, const MessageInfoVec& mset, bool heading = true);


		//将消息集合mset写入文件file内，默认为重写，append=true使得直接在上次的内容后追加写入
		static void write_messages(MessageInfoVec& mset, const std::string& file, bool append = false);

		//生成大小为num的消息info集合mset
		static void  generate_msg_info_set(MessageInfoVec& mset, size_t num);

		//打印message集合到cout
		static void print_message(const MessageInfoVec& mset, bool append = false);
		static void print_message(const MessageVec& mset, bool append = false);
		static void print_message(const Message& msg, bool append);
		static void print_message(const MessageInfo& msg, bool append = false);
	};

}

#endif // CANFDFRAME_H

