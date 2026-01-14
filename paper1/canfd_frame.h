#include<memory>
#include<vector>
#include<set>
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
#include <numeric> 
#include<algorithm>
#include <nlohmann/json.hpp>

#include"debug_tool.h"
#include"config.h"
#ifndef CANFDFRAME_H
#define CANFDFRAME_H


namespace cfd {
	/*
		全局维护一个消息INFO集合message_info_vec，在初始化后不应对其进行修改
		Message为一个身份令牌，其message_index属性，对应message_info_vec中的下标指示的元素；
		Message的frame_index属性对应CanfdFrame的ID
		CanfdFrame存储的是Message令牌，而非MessageInfo。
	*/
	constexpr int SIZE_MAX_CANFD_DATA = 64 * 8;			// CANFD帧 最大数据负载，单位为b
	constexpr int SIZE_LIMIT_CANFD_DATA = 64 * 8;			// CANFD帧 最大数据负载限制，单位为b
	constexpr int SIZE_IDENTIFIER = 11;					// CANFD帧 标识符长度，标准帧的11位标识符，扩展帧可以有29位
	constexpr int NUM_MAX_FRAME = 2 << 11;				// 帧最大数数量

	constexpr double TIME_BIT_ARBITRATION = 0.001;			// 仲裁段传输一个bit所用时间，单位为毫秒(ms)（1Mbps）
	constexpr double TIME_BIT_TRANSMISSION = 0.0002;		// 数据段传输一个bit所用时间，单位为毫秒(ms)（5Mbps）

	constexpr double TIME_INTERMISSION = 3.0 * TIME_BIT_ARBITRATION; // 帧间隔


	constexpr int OPTION_CANFD_PAYLOAD_SIZE[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64 }; // CANFD帧payload可选长度，单位为byte
	constexpr int NUM_CANFD_PAYLOAD_SIZE = std::size(OPTION_CANFD_PAYLOAD_SIZE);


	using EcuId = size_t;
	using FrameId = size_t;
	using MessageCode = size_t;
	using json = nlohmann::json;

	class EcuPair {
	public:
		EcuId src_ecu = 0;        // 源ECU
		EcuId dst_ecu = 0;        // 目ECU
		EcuPair(int src = 0, int dst = 0)
			: src_ecu(src), dst_ecu(dst) {}
		bool  operator==(const EcuPair& other) const {
			return (src_ecu == other.src_ecu) && (dst_ecu == other.dst_ecu);
		}
		EcuPair(const EcuPair& other) {
			src_ecu = other.src_ecu;
			dst_ecu = other.dst_ecu;
		}
		// 序列化到 JSON
		json to_json() const;

		// 从 JSON 反序列化
		static EcuPair from_json(const json& j);

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
		int level = 0;			// 等级，比如安全优先级【LEVEL == 1 表示R2备份消息】
		int type = 0;			// 冗余类型，0 无需异源备份；1 需要异源备份

		MessageInfo(MessageCode _code, int _data_size, int _period, int _deadline, int _src_ecu, int _dst_ecu, int _offset = 0, int _level = 0, int _type = 0)
			:code(_code), data_size(_data_size), period(_period), deadline(_deadline), ecu_pair(_src_ecu, _dst_ecu), offset(_offset), level(_level), type(_type){}

		MessageInfo(const MessageInfo& minfo, int new_src_ecu,int _type=1) 
			:code(minfo.code), data_size(minfo.data_size), period(minfo.period), deadline(minfo.deadline), ecu_pair(new_src_ecu, minfo.ecu_pair.dst_ecu), offset(minfo.offset), level(minfo.level), type(_type) {}



		~MessageInfo() {}
		// 序列化到 JSON
		json to_json() const;

		// 从 JSON 反序列化
		static MessageInfo from_json(const json& j);
	};
	using MessageInfoVec = std::vector<MessageInfo>;

	extern MessageInfoVec MESSAGE_INFO_VEC;//全局维护一个message info 表 



	class CanfdFrame;

	using MessageID = size_t;
	//消息实体类
	class Message {
		friend CanfdFrame;
	private:
		MessageID message_index = -1;		//	指向对应的MessageInfo，可以重复
		FrameId frame_index = -1;	//	指向对应的Frameid


	public:
		// 将 Message 对象转换为 JSON 对象
		json to_json() const;
		// 从 JSON 对象中反序列化 Message 对象
		static Message from_json(const json& j);

		void assign_frame(FrameId fid) {
			this->frame_index = fid;
		}
		void clear_frame() {
			this->frame_index = -1;
		}
		MessageID get_id_message() const { return message_index; }
		FrameId get_id_frame() const { return frame_index; }
		Message() {}
		Message(size_t m_index) :message_index(m_index) {}
		Message(size_t m_index, size_t f_index) :message_index(m_index), frame_index(f_index) {}
		Message(const Message& other) {
			message_index = other.message_index;
			frame_index = other.frame_index;
		}
		Message(Message&& other) noexcept {
			message_index = other.message_index;
			frame_index = other.frame_index;
		}
		Message& operator = (const Message& other) {
			message_index = other.message_index;
			frame_index = other.frame_index;
			return *this;
		}
		bool operator== (const Message& other)const {
			return this->get_code() == other.get_code();
		}
		int get_period() const {
			return MESSAGE_INFO_VEC[message_index].period;
		}
		int get_data_size() const {
			return MESSAGE_INFO_VEC[message_index].data_size;
		}
		int get_deadline() const {
			return MESSAGE_INFO_VEC[message_index].deadline;
		}
		MessageCode get_code() const {
			return MESSAGE_INFO_VEC[message_index].code;
		}
		EcuPair get_ecu_pair() const {
			return MESSAGE_INFO_VEC[message_index].ecu_pair;
		}
		int get_offset() const {
			return MESSAGE_INFO_VEC[message_index].offset;
		}
		int get_level() const {
			return MESSAGE_INFO_VEC[message_index].level;
		}
		int get_type() const {
			return MESSAGE_INFO_VEC[message_index].type;
		}

	};
	using MessageVec = std::vector<Message>;


	struct ComparatorMsg {
		bool operator()(const MessageInfo& lhs, const MessageInfo& rhs) const {
			return lhs.code < rhs.code;
		}
		bool operator()(const Message& lhs, const Message& rhs) const {
			return lhs.get_code() < rhs.get_code();
		}
	};

	class CanfdFrame {

	private:
		int data_size = 0;      // 装载的数据长度，默认为空,单位为b，取值为[0, 512]
		int payload_size = 0;   // payload尺寸和数据尺寸不完全一样，取值有0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48 or 64 bytes
		int priority = -1;      // 优先级，取值为[0,2047],数字越小优先级越高。【仅考虑标准帧的11位标识符，扩展帧可以有29位】
		int period = -1;        // 周期，单位为毫秒(ms)
		int deadline = -1;      // 时限，单位为毫秒(ms)
		int offset = 0;         // 偏移，单位为毫秒(ms)，可以使用assign_offset方法分配frame集合中所有任务的合适offset
		double trans_time = 0;   // 数据帧在系统内的传输时间，单位为毫秒(ms)
	public:
		FrameId id = 0;//id由生成时顺序给出 
		EcuPair ecu_pair;
		std::multiset<Message, ComparatorMsg> msg_set;//CANFD帧装载的消息 的索引， 按 offset 升序排列


	private:
		// 将数据尺寸（单位b）转换为合适的payload尺寸（单位byte），payload取值有0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48 or 64 bytes
		int payload_size_trans(int size);


		//根据data_size更新data_size、payload_size,exec_time
		bool set_data_size(int v);

	public:

		// 因为payload可能取整，freesize即可利用的空间。
		int get_free_size() {
			return payload_size * 8 - data_size;
		}

		// 根据paylaod_size计算最坏传输时间,paylaod_size单位是bit， 返回值是毫秒
		static double calc_wctt(int paylaod_size);
		// 序列化 CanfdFrame 对象为 JSON
		json to_json() const;

		// 从 JSON 反序列化为 CanfdFrame 对象
		static CanfdFrame from_json(const json& j);

		//获取所装载消息中最大的等价offset
		int get_max_offset();

		//遍历CanfdFrameVec时，必须判断帧是否为空
		bool empty() const {
			return this->msg_set.empty();
		}

		// 仅用于不改变消息集合，想要更改帧offset时使用,这会同步改变deadline
		bool set_offset(int v);

		bool set_priority(int v);

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
		FrameId get_id() const {
			return this->id;
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

		//向 CANFD帧 添加 单个消息m，同步更新data_size、payload_size，deadline，offset，更新message的frameID
		bool add_message(Message& m);

		//从 CANFD帧 取出 消息集合mset，同步更新data_size、payload_size，deadline，offset，更新message的frameID
		bool extract_message(Message& m);

		//从当前帧中移动消息到other
		bool move_message(CanfdFrame& other, Message& m);

		CanfdFrame(FrameId _id, int _period, int _deadline,
			const EcuPair& _ecu_pair, int _offset) {
			this->id = _id;
			this->period = _period;
			this->deadline = _deadline;
			this->ecu_pair = _ecu_pair;
			this->offset = _offset;
			this->set_data_size(0);
		}


		CanfdFrame(FrameId _id, Message& msg) {
			this->id = _id;

			this->period = msg.get_period();
			this->deadline = msg.get_deadline();
			this->ecu_pair = msg.get_ecu_pair();

			//this->offset = msg.get_offset();

#ifdef OFFSET_RANDOM_CREATE
			std::random_device rd;
			std::mt19937 gen(rd());
			std::uniform_int_distribution<int> dist(0, period - 1); // 左闭右闭，范围 0 到 period-1
			int offset_t= dist(gen);
#else
			int offset_t = 0;
#endif // OFFSET_RANDOM_CREATE

			this->set_offset(offset_t);
			 
			this->set_data_size(msg.get_data_size());

			msg.assign_frame(_id);


			msg_set.insert(msg);
		}
		CanfdFrame(FrameId _id) {
			this->id = _id;
		}
		CanfdFrame() {
			return;
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
			ecu_pair = other.ecu_pair;
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
			ecu_pair = other.ecu_pair;
			offset = other.offset;

		}
		CanfdFrame& operator=(const CanfdFrame& other) {
			msg_set = other.msg_set;

			data_size = other.data_size;
			payload_size = other.payload_size;
			deadline = other.deadline;
			period = other.period;
			priority = other.priority;
			trans_time = other.trans_time;
			id = other.id;
			ecu_pair = other.ecu_pair;
			offset = other.offset;

			return *this;
		}
		void clear() {


			msg_set.clear();
			data_size = 0;
			payload_size = 0;
			deadline = period;
			//period = period;		//不变
			priority = -1;
			trans_time = -1;
			//id = id;				//不变
			//ecu_pair = ecu_pair;	//不变
			offset = 0;
		}
		~CanfdFrame() {
		}
	};
	using CanfdFrameVec = std::vector<CanfdFrame>;
	using CanfdFrameMap = std::unordered_map<FrameId, CanfdFrame>;	// key为FRAME ID 


}
namespace cfd::utils {


	// 向流os写入消息列名
	void write_msg_heading_to_stream(std::ostream& os);
	// 向流os写入帧列名
	void write_frame_heading_to_stream(std::ostream& os);

	// 向流os打印方便阅读的消息内容
	void write_msg_to_stream(std::ostream& os, const MessageInfo& msg, bool heading);

	// 获取string格式的msg内容
	std::string get_msg_string(const MessageInfo& msg, bool append = true);
	std::string get_msg_string(const Message& msg, bool append = true);

	// 向cout打印方便阅读的消息内容
	void print_message(const MessageInfo& msg, bool append = true);
	void print_message(const Message& msg, bool append = true);
	void print_message(const MessageInfoVec& mset, bool append = true);

	// 向流os打印方便阅读的帧内容
	void write_frame_to_stream(std::ostream& os, const CanfdFrame& frame, bool heading);

	// 获取string格式的帧内容
	std::string get_frame_string(const CanfdFrame& frame, bool append = true);

	// 向cout打印方便阅读的帧内容
	void print_frame(const CanfdFrame& frame, bool append = true);
	void print_frame(const CanfdFrameMap& fmap, bool append = true);

	void write_msg_json_to_stream(std::ostream& os, const MessageInfoVec& mvec);
	// 向file文件写入消息集合，默认append为不追加写入
	void write_message(MessageInfoVec& mvec, const std::string& filename, bool append = false);
	// 从file读取消息集合
	void read_message(const std::string& file, MessageInfoVec& mset = MESSAGE_INFO_VEC);


	void write_frame_json_to_stream(std::ostream& os, const CanfdFrameMap& fmap);
	// 向file文件写入帧集合，默认append为不追加写入
	void write_frame(CanfdFrameMap& fmap, const std::string& file, bool append = false);
	// 从file读取帧集合
	void read_frame(CanfdFrameMap& fmap, const std::string& file);

	// 在folder_path下存储一对紧密相关的帧和消息集合，帧集合：frm_时间戳.txt 、消息集合：msg_时间戳.txt
	std::string store_frm_msg(MessageInfoVec& mset, CanfdFrameMap& fmap, const std::string& folder_path);
	std::string store_msg(const std::string& folder_path, MessageInfoVec& mset = MESSAGE_INFO_VEC);
	std::string store_frm(CanfdFrameMap& fmap, const std::string& folder_path);

	// 随机生成一组消息
	void generate_msg_info_set(MessageInfoVec& mset = MESSAGE_INFO_VEC, size_t num = SIZE_ORIGINAL_MESSAGE);

}
#endif // CANFDFRAME_H