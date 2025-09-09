#include"canfd_frame.h"

namespace cfd {
	MessageInfoVec MESSAGE_INFO_VEC;//全局维护一个message info 表

	int CanfdFrame::payload_size_trans(int size)
	{
		int byte_size = (size + 7) / 8;  // 向上取整
		// 使用二分查找寻找合适的 payload size
		auto it = std::lower_bound(OPTION_CANFD_PAYLOAD_SIZE, OPTION_CANFD_PAYLOAD_SIZE + NUM_CANFD_PAYLOAD_SIZE, byte_size);

		// 如果找到适当的值，返回
		if (it != std::end(OPTION_CANFD_PAYLOAD_SIZE)) {
			return *it;
		}

		// 如果没有找到合适的值，返回 -1
		return -1;
	}

	double CanfdFrame::calc_wctt(int paylaod_size)
	{
		int p = paylaod_size;
		double wctt = 32 * TIME_BIT_ARBITRATION + (28 + 5 * ceil((p - 16) / 64.0) + 10.0 * p) * TIME_BIT_TRANSMISSION;
		return wctt;
	}

	//根据data_size更新data_size、payload_size,exec_time

	bool CanfdFrame::set_data_size(int v) {
		if (v < 0 || v > SIZE_MAX_CANFD_DATA) {
			return false;
		}
		else {
			this->data_size = v;
			this->payload_size = payload_size_trans(v);
			this->trans_time = calc_wctt(this->payload_size);
			return true;
		}
	}

	// 序列化 CanfdFrame 对象为 JSON
	json CanfdFrame::to_json() const {
		json j;
		j["data_size"] = data_size;
		j["payload_size"] = payload_size;
		j["priority"] = priority;
		j["period"] = period;
		j["deadline"] = deadline;
		j["offset"] = offset;
		j["trans_time"] = trans_time;
		j["id"] = id;
		j["ecu_pair"] = ecu_pair.to_json();

		// 序列化 msg_set
		std::vector<json> msg_set_json;
		for (const auto& msg : msg_set) {
			msg_set_json.push_back(msg.to_json()); // 假设 Message 类有 to_json 方法
		}
		j["msg_set"] = msg_set_json;

		return j;
	}


	// 从 JSON 反序列化为 CanfdFrame 对象
	CanfdFrame CanfdFrame::from_json(const json& j) {
		
		FrameId id = j.at("id").get<FrameId>();
		int period = j.at("period").get<int>();
		int deadline = j.at("deadline").get<int>();
		EcuPair ecu_pair = EcuPair::from_json(j.at("ecu_pair"));
		int offset = j.at("offset").get<int>();
		CanfdFrame frame(id, period, deadline, ecu_pair, offset);

		frame.data_size = j.at("data_size").get<int>();
		frame.payload_size = j.at("payload_size").get<int>();
		frame.priority = j.at("priority").get<int>();
		frame.trans_time = j.at("trans_time").get<double>();

		// 反序列化 msg_set
		for (const auto& msg_json : j.at("msg_set")) {
			Message msg = Message::from_json(msg_json); // 假设 Message 类有 from_json 方法
			frame.msg_set.insert(msg);
		}

		return frame;
	}

	int CanfdFrame::get_max_offset()
	{
		return 0;


		// 如果 loaded_msgs 为空，抛出异常
		if (msg_set.empty()) {
			return 0;
		}
		int max_offset = 0;
		for (auto msg : msg_set) {
			if (max_offset < msg.get_offset() % this->period) {
				max_offset = msg.get_offset() % this->period;
			}
		}
		return max_offset;
	}

	// TODO 应该修改
	bool CanfdFrame::set_offset(int v) {
		if (v == offset) {
			return true;	// 无改变
		}
		if (v < 0 || v >= period) {
			return false;
		}
		
		offset = v;
		return true;
	}

	bool CanfdFrame::set_priority(int v) {
		if (v >= 0 && v < NUM_MAX_FRAME) {
			this->priority = v;
			return true;
		}
		else {
			return false;
		}
	}




	//不知道为什么有时候会和extrect不对偶,刚移出来的消息可能放不进去
	bool CanfdFrame::add_message(Message& m)
	{
		// message已分配
		if (m.get_id_frame() != -1) {
			DEBUG_MSG_DEBUG2(std::cout, "CanfdFrame::add_message::message已分配");
			std::cout << "已分配\n";
			return false;
		}
		//放得下
		if (SIZE_MAX_CANFD_DATA - data_size < m.get_data_size()) {
			DEBUG_MSG_DEBUG2(std::cout, "CanfdFrame::add_message::frame剩余空间不足 ");
			return false;
		}
		//周期合适,信号周期为CAN-FD帧周期1~N倍时才可装载
		if (m.get_period() % this->period != 0 || m.get_period() > this->period * FACTOR_M_F_PERIOD) {
			DEBUG_MSG_DEBUG2(std::cout, "CanfdFrame::add_message::frame和message周期不匹配");
			return false;
		}
		//源目ECU一致
		if (!(m.get_ecu_pair() == this->ecu_pair)) {
			DEBUG_MSG_DEBUG2(std::cout, "CanfdFrame::add_message::frame和message的源目ecu不匹配");
			return false;
		}

		// 不允许存储复数个同code的消息
		for (auto& msg : this->msg_set) {
			if (m.get_code() == msg.get_code()) {
				DEBUG_MSG_DEBUG2(std::cout, "CanfdFrame::add_message::frame内已存在messagecode");
				return false;
			}
		}

		int min_tolerance_time = m.get_deadline();;
		for (auto &msg: this->msg_set) {
			auto td = msg.get_deadline();
			if (td < min_tolerance_time) {
				min_tolerance_time = td;
			}
		}
		int tolerance_time = 0;		// 指一个消息 在offset=v的帧 到达后 还有多久超时


		this->set_data_size(this->data_size + m.get_data_size());
		this->set_offset(0);// TODO 也许应该修改,这里帧默认offset为0
		this->deadline = min_tolerance_time;

		m.assign_frame(this->id);
		msg_set.insert(m);


		return true;
	}

	bool CanfdFrame::extract_message(Message& m)
	{
		if (msg_set.empty()) {
			std::cout << "9797\n";
			return false;
		}
		// 删除message
		auto it = msg_set.find(m);
		if (it == msg_set.end()) {
			std::cout << "10\n";
			return false;   // 不存在这个消息
		}
		else {
			msg_set.erase(it);
		}
		DEBUG_MSG_DEBUG2(std::cout, "取出消息 ", m.get_id_message(), " 从帧", this->id);
		m.clear_frame();

		// 有的帧可能啥也不剩，可以通过empty确定该帧是否为空，注意执行clear使其归零 
		if (msg_set.empty()) {
			this->clear();
			return true;
		}


		// 重算offset、deadline，wctt，datasize
		int frame_offset = get_max_offset();

		int min_tolerance_time = INT_MAX;
		int tolerance_time = 0;		// 指一个消息 在offset=v的帧 到达后 还有多久超时

		for (auto& msg : msg_set) {
			// tolerance_time是一个消息在帧到达后，剩余的可等待时间
			tolerance_time = msg.get_deadline() - (frame_offset - msg.get_offset());
			if (tolerance_time < min_tolerance_time) {
				min_tolerance_time = tolerance_time;
			}
		}

		this->set_data_size(this->data_size - m.get_data_size());
		this->set_offset(frame_offset);
		this->deadline = min_tolerance_time;// 帧deadline就是所有消息最小的tolerance_time

		return true;
	}

	bool CanfdFrame::move_message(CanfdFrame& other, Message& m)
	{
		int x = m.get_id_frame();
		bool flag = false;
		auto frame_temp = *this;
		if (this->extract_message(m)) {
			if (other.add_message(m)) {
				return true;
			}
			else {
				//*this = frame_temp;//恢复 
				// m.assign_frame(this->id);
				flag = this->add_message(m); //恢复 
				if (flag == false)
					std::cout << 456;
			}
		}
		return false;
	}


	// 将 Message 对象转换为 JSON 对象

	json Message::to_json() const {
		json j;
		j["message_index"] = message_index;
		j["frame_index"] = frame_index;
		return j;
	}

	// 从 JSON 对象中反序列化 Message 对象

	Message Message::from_json(const json& j) {
		size_t message_idx = j.value("message_index", SIZE_MAX);
		size_t frame_idx = j.value("frame_index", SIZE_MAX);
		return Message(message_idx, frame_idx);
	}




	// 序列化到 JSON

	json EcuPair::to_json() const {
		json j;
		j["src_ecu"] = src_ecu;
		j["dst_ecu"] = dst_ecu;
		return j;
	}

	// 从 JSON 反序列化

	EcuPair EcuPair::from_json(const json& j) {
		int src = j.at("src_ecu").get<int>();
		int dst = j.at("dst_ecu").get<int>();
		return EcuPair(src, dst);
	}


	json MessageInfo::to_json() const {
		json j;
		j["code"] = code;
		j["data_size"] = data_size;
		j["period"] = period;
		j["deadline"] = deadline;
		j["ecu_pair"] = ecu_pair.to_json();
		j["offset"] = offset;
		j["level"] = level;
		j["type"] = type;
		return j;
	}


	MessageInfo MessageInfo::from_json(const json& j) {

		int data_size = j.at("data_size").get<int>();

		MessageCode code = j.at("code").get<MessageCode>();
		int period = j.at("period").get<int>();
		int deadline = j.at("deadline").get<int>();
		EcuPair ecu_pair = EcuPair::from_json(j.at("ecu_pair"));
		int offset = j.at("offset").get<int>();
		int level = j.at("level").get<int>();
		int type = j.at("type").get<int>();
		return MessageInfo(code, data_size, period, deadline, ecu_pair.src_ecu, ecu_pair.dst_ecu, offset, level, type);
	}

}

namespace cfd::utils {




	void write_msg_heading_to_stream(std::ostream& os) {
		os << "\n";
		os << std::left
			<< std::setw(22) << "CODE"
			<< std::setw(12) << "data_size"
			<< std::setw(10) << "period"
			<< std::setw(10) << "deadline"
			<< std::setw(10) << "SrcECU"
			<< std::setw(10) << "DstECU"
			<< std::setw(10) << "offset"
			<< std::setw(10) << "level"
			<< std::setw(10) << "type"
			<< std::endl;
	}

	void write_frame_heading_to_stream(std::ostream& os) {
		os << "\n";
		os << std::left
			<< std::setw(9) << "FrameID"
			<< std::setw(14) << "payload_size"
			<< std::setw(10) << "priority"
			<< std::setw(10) << "period"
			<< std::setw(10) << "deadline"
			<< std::setw(10) << "SrcECU"
			<< std::setw(10) << "DstECU"
			<< std::setw(10) << "offset"
			<< std::endl;
	}



	void write_msg_to_stream(std::ostream& os, const MessageInfo& msg, bool heading)
	{
		if (heading) {
			// 输出表头
			write_msg_heading_to_stream(os);
		}
		os << std::left
			<< std::setw(22) << msg.code
			<< std::setw(12) << msg.data_size
			<< std::setw(10) << msg.period
			<< std::setw(10) << msg.deadline
			<< std::setw(10) << msg.ecu_pair.src_ecu
			<< std::setw(10) << msg.ecu_pair.dst_ecu
			<< std::setw(10) << msg.offset
			<< std::setw(10) << msg.level
			<< std::setw(10) << msg.type
			<< std::endl;
	}

	std::string get_msg_string(const MessageInfo& msg, bool append) {
		std::ostringstream oss;
		write_msg_to_stream(oss, msg, append);
		return oss.str();
	}
	std::string get_msg_string(const Message& msg, bool append) {
		std::ostringstream oss;
		write_msg_to_stream(std::cout, MESSAGE_INFO_VEC[msg.get_id_message()], append);
		return oss.str();
	}


	void print_message(const MessageInfo& msg, bool append) {
		write_msg_to_stream(std::cout, msg, append);
	}
	void print_message(const Message& msg, bool append) {
		write_msg_to_stream(std::cout, MESSAGE_INFO_VEC[msg.get_id_message()], append);
	}
	void print_message(const MessageInfoVec& mset, bool append) {
		write_msg_heading_to_stream(std::cout);
		for (auto& msg : mset) {
			write_msg_to_stream(std::cout, msg, false);
		}
	}


	void write_frame_to_stream(std::ostream& os, const CanfdFrame& frame, bool heading)
	{
		if (heading) {
			// 输出帧表头
			write_frame_heading_to_stream(os);
		}
		os << std::left
			<< std::setw(9) << frame.id
			<< std::setw(14) << frame.get_paylaod_size()
			<< std::setw(10) << frame.get_priority()
			<< std::setw(10) << frame.get_period()
			<< std::setw(10) << frame.get_deadline()
			<< std::setw(10) << frame.ecu_pair.src_ecu
			<< std::setw(10) << frame.ecu_pair.dst_ecu
			<< std::setw(10) << frame.get_offset()
			<< std::endl;
		// 输出消息表头
		write_msg_heading_to_stream(os);
		for (const auto& msg : frame.msg_set) {
			write_msg_to_stream(os, MESSAGE_INFO_VEC[msg.get_id_message()], false);
		}
		if (heading)
			os << "===============================================================================================\n";
	}
	std::string get_frame_string(const CanfdFrame& frame, bool append) {
		std::ostringstream oss;
		write_frame_to_stream(oss, frame, append);
		return oss.str();
	}


	void print_frame(const CanfdFrame& frame, bool append) {
		write_frame_to_stream(std::cout, frame, append);
	}

	void print_frame(const CanfdFrameMap& fmap, bool append) {
		for (const auto& [key, frame] : fmap) {
			print_frame(frame, append);
		}

	}


	// 将多个 MessageInfo 对象转换成 JSON 数组写入流
	void write_msg_json_to_stream(std::ostream& os, const MessageInfoVec& mvec) {
		json j;
		for (size_t i = 0; i < mvec.size(); ++i) {
			j.push_back(mvec[i].to_json());
		}
		os << j.dump(4);
	}
	// 将多个 MessageInfo 对象以 TAB 分隔格式写入文本文件
	void write_msg_txt_to_stream(std::ostream& os, const MessageInfoVec& mvec) {
		// 写表头
		os << "code\tdata_size\tperiod\tdeadline\tsrc_ecu\tdst_ecu\toffset\tlevel\ttype\n";
		for (const auto& msg : mvec) {
			os << msg.code << '\t'
				<< msg.data_size << '\t'
				<< msg.period << '\t'
				<< msg.deadline << '\t'
				<< msg.ecu_pair.src_ecu << '\t'
				<< msg.ecu_pair.dst_ecu << '\t'
				<< msg.offset << '\t'
				<< msg.level << '\t'
				<< msg.type << '\n';
				;

		}
	}
	void write_message(MessageInfoVec& mvec, const std::string& filename, bool append)
	{
		auto fjname = filename + ".txt";
		auto ftname = filename + "_tab.txt";
		std::ofstream ofs_j(fjname, append ? std::ios::app : std::ios::out);

		if (!ofs_j) {
			throw std::ios_base::failure("Failed to open the file: " + fjname);
		}
		write_msg_json_to_stream(ofs_j, mvec);

		std::ofstream ofs_t(ftname, append ? std::ios::app : std::ios::out);
		if (!ofs_t) {
			throw std::ios_base::failure("Failed to open the file: " + ftname);
		}
		write_msg_txt_to_stream(ofs_t, mvec);
	}


	void read_message(const std::string& file, MessageInfoVec& mset)
	{
		std::ifstream ifs(file);
		if (!ifs) {
			throw std::ios_base::failure("无法打开文件: " + file);
		}

		try {
			// 读取整个文件内容
			json j;
			ifs >> j;

			int line_number = 0;
			// 将 JSON 反序列化为 MessageInfoVec
			for (const auto& item : j) {
				auto m = MessageInfo::from_json(item);

				// 输入合法性检查
				if (m.data_size < 0 || m.period < 0 || m.deadline < 0 || m.offset < 0 || m.offset >= m.deadline || m.data_size > SIZE_MAX_CANFD_DATA) {
					throw std::invalid_argument("数据大小、周期、时限或偏移无效: " + std::to_string(m.code));
				}

				if (m.deadline > m.period || m.ecu_pair.src_ecu == m.ecu_pair.dst_ecu) {
					throw std::invalid_argument("消息 " + std::to_string(m.code) + " 的 deadline 应等于 period，且 src_ecu 不应等于 dst_ecu");
				}

				mset.emplace_back(m);
				line_number++;
			}
		}
		catch (const json::parse_error& e) {
			throw std::runtime_error("JSON 解析失败: " + std::string(e.what()));
		}
		catch (const std::invalid_argument& e) {
			throw std::runtime_error("无效的输入: " + std::string(e.what()));
		}
		catch (const std::exception& e) {
			std::cerr << "运行时错误: " << e.what() << std::endl;
			throw std::runtime_error("发生错误: " + std::string(e.what()));
		}
	}



	//向 流os 写入 帧集合fset，heading=true表示输出表头
	void write_frame_json_to_stream(std::ostream& os, const CanfdFrameMap& fmap) {
		json j;
		for (const auto& [key, frame] : fmap) {
			if (!frame.empty()) {
				j.push_back(frame.to_json());
			}
		}
		os << j.dump(4);
	}
	void write_frame(CanfdFrameMap& fmap, const std::string& filename, bool append)
	{
		std::ofstream ofs(filename, append ? std::ios::app : std::ios::out);

		if (!ofs) {
			throw std::ios_base::failure("Failed to open the file: " + filename);
		}

		write_frame_json_to_stream(ofs, fmap);
	}


	void read_frame(CanfdFrameMap& fmap, const std::string& file)
	{
		std::ifstream ifs(file);
		if (!ifs) {
			throw std::ios_base::failure("无法打开文件: " + file);
		}

		try {
			json j;
			ifs >> j;
			fmap.clear();// 清空现有的 fmap

			for (const auto& item : j) {
				auto frame = CanfdFrame::from_json(item);
				fmap.emplace(frame.id, frame);
			}
		}
		catch (const json::parse_error& e) {
			throw std::runtime_error("JSON 解析失败: " + std::string(e.what()));
		}
		catch (const std::exception& e) {
			throw std::runtime_error("读取文件时发生错误: " + std::string(e.what()));
		}
	}

	std::string  store_frm_msg(MessageInfoVec& mset, CanfdFrameMap& fmap, const std::string& folder_path)
	{
		// 获取当前时间戳
		std::string timestamp = get_time_stamp();

		// 构造文件名
		std::string message_filename = folder_path + "/msg_" + timestamp + ".txt";
		std::string frame_filename = folder_path + "/frm_" + timestamp + ".txt";

		// 写入消息文件
		write_message(mset, message_filename, false);

		// 写入帧文件
		write_frame(fmap, frame_filename, false);

		return timestamp;
	}
	std::string store_msg(const std::string& folder_path, MessageInfoVec& mset){

		// 获取当前时间戳
		std::string timestamp=get_time_stamp();

		// 构造文件名
		std::string message_filename = folder_path + "/msg_" + timestamp;

		// 写入消息文件
		write_message(mset, message_filename, false);

		return timestamp;
	}
	std::string store_frm(CanfdFrameMap& fmap, const std::string& folder_path) {

		// 获取当前时间戳
		std::string timestamp = get_time_stamp();

		// 构造文件名
		std::string frame_filename = folder_path + "/frm_" + timestamp + ".txt";


		// 写入帧文件
		write_frame(fmap, frame_filename, false);

		return timestamp;
	}




	void generate_msg_info_set(MessageInfoVec& mset , size_t num) {
		std::random_device rd;
		std::mt19937 gen(rd());
		mset.clear();
		mset.reserve(num);

		// 生成随机的 period、 data_size、源ECU、目ECU 分布、offset、level

		std::discrete_distribution<> dist_size(PROBABILITY_MESSAGE_SIZE, PROBABILITY_MESSAGE_SIZE + NUM_MESSAGE_SIZE);

		std::discrete_distribution<>dist_period(PROBABILITY_MESSAGE_PERIOD, PROBABILITY_MESSAGE_PERIOD + NUM_MESSAGE_PERIOD);

		std::uniform_int_distribution<>dist_ecu1(0, NUM_ECU - 1);
		std::uniform_int_distribution<>dist_ecu2(0, NUM_ECU - 2);

		std::uniform_real_distribution<> dist_offset(0.0, 1.0);

		std::discrete_distribution<>dist_level(PROBABILITY_MESSAGE_LEVEL, PROBABILITY_MESSAGE_LEVEL + NUM_MESSAGE_LEVEL);

		std::discrete_distribution<> dist_type(PROBABILITY_MESSAGE_TYPE, PROBABILITY_MESSAGE_TYPE + NUM_MESSAGE_TYPE);


		int size = 0, period = 0, deadline = 0, src = 0, dst = 0, offset = 0, level = 0,type=0;

		// 获取一个ecu数组的备份
		std::array<int, NUM_ECU> option_ecu_copy;
		for (size_t i = 0; i < NUM_ECU; ++i) {
			option_ecu_copy[i] = OPTION_ECU[i];
		}

		std::hash<std::string> hash_fn;

		for (size_t i = 0; i < num; i++) {
			size = OPTION_MESSAGE_SIZE[dist_size(gen)];

			period = OPTION_MESSAGE_PERIOD[dist_period(gen)];
			deadline = period;

			int ecu1_index = dist_ecu1(gen);
			src = option_ecu_copy[ecu1_index];
			std::swap(option_ecu_copy[NUM_ECU - 1], option_ecu_copy[ecu1_index]);
			dst = option_ecu_copy[dist_ecu2(gen)];

			// offset = dist_offset(gen) * period;
			offset = 0;	// 信号不设offset


			level = OPTION_MESSAGE_LEVEL[dist_level(gen)];

			type = OPTION_MESSAGE_TYPE[dist_type(gen)];

			auto timestamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
			// 拼接所有字段（包括时间戳）
			std::string code_str = std::to_string(size) + "-" + std::to_string(period) + "-" +
				std::to_string(deadline) + "-" + std::to_string(src) + "-" +
				std::to_string(dst) + "-" + std::to_string(offset) + "-" +
				std::to_string(level) + "-" + std::to_string(timestamp);

			// 使用哈希生成唯一的code
			MessageCode code = hash_fn(code_str);

			mset.emplace_back(code, size, period, deadline, src, dst, offset, level, type);
		}

		return;
	}
}

