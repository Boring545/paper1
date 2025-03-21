#include"canfd_frame.h"


namespace cfd {
    MessageInfoVec message_info_vec;//全局维护一个message info 表


       


    void CanfdUtils::read_messages(MessageInfoVec& mset, const std::string& file)
    {
        std::ifstream ifs(file);
        if (!ifs) {
            throw std::ios_base::failure("Failed to open the file: " + file);
        }
        
        std::string line;
        std::getline(ifs, line); // 跳过表头

        int code=0,size = 0, period = 0, deadline = 0, src = 0, dst = 0, offset = 0, level = 0;
        int line_number = 2; // 从2开始，因为第1行是表头

        while (std::getline(ifs, line)) {
            std::istringstream iss(line);

            iss >> code >> size >> period >> deadline
                >> src >> dst
                >> offset >> level;
            // 输入合法性检查
            if (size < 0 || period < 0 || deadline < 0  || offset < 0 || offset >= deadline ||size>SIZE_MAX_CANFD_DATA) {
                std::cerr << "Error: 不合法的输入 位于 行 " << line_number << "\n(提示：data_size, period, deadline, offset 应非负, priority 应属于 [0 , 2047], data size 不应超过SIZE_MAX_CANFD_DATA)" << std::endl;
                continue; // 跳过当前行
            }

            if (deadline > period || src==dst) {
                std::cerr << "Error: 不合法的输入 位于 行 " << line_number << " \n(提示：deadline应等于period，src_ecu不应等于dst_ecu)" << std::endl;
                continue; // 跳过当前行
            }
            if (iss) {
                mset.emplace_back(code,size, period, deadline, src, dst, offset, level);
            }
            ++line_number;
        }

        return ;
    }



    void CanfdUtils::write_heading_to_stream(std::ostream& os) {
        os << std::left
            << std::setw(8) << "CODE"
            << std::setw(12) << "data_size"
            << std::setw(10) << "period"
            << std::setw(10) << "deadline"
            << std::setw(10) << "SrcECU"
            << std::setw(10) << "DstECU"
            << std::setw(10) << "offset"
            << std::setw(10) << "level"
            << std::endl;
    }

    void CanfdUtils::write_msg_to_stream(std::ostream& os, const MessageInfo& msg, bool heading)
    {
        if (heading) {
            // 输出表头
            write_heading_to_stream(os);
        }
        os << std::setw(8) << msg.code
            << std::setw(12) << msg.data_size
            << std::setw(10) << msg.period
            << std::setw(10) << msg.deadline
            << std::setw(10) << msg.ecu_pair.src_ecu
            << std::setw(10) << msg.ecu_pair.dst_ecu
            << std::setw(10) << msg.offset
            << std::setw(10) << msg.level
            << std::endl;
    }

    void CanfdUtils::write_mset_to_stream(std::ostream& os, const MessageInfoVec& mset, bool heading)
    {
        if (heading) {
            // 输出表头
            write_heading_to_stream(os);
        }

        // 输出每一条消息
        for (const auto& msg : mset) {
            write_msg_to_stream(os, msg, false);
        }
    }

    void CanfdUtils::write_messages(MessageInfoVec& mset, const std::string& file, bool append)
    {
        std::ofstream ofs(file, append ? std::ios::app : std::ios::out);

        if (!ofs) {
            throw std::ios_base::failure("Failed to open the file: " + file);
        }

        write_mset_to_stream(ofs, mset, append);
    }
    void CanfdUtils::print_message(const MessageInfo& msg, bool append){
        write_msg_to_stream(std::cout, msg, append);
    }
    void CanfdUtils::print_message(const Message& msg, bool append) {
        write_msg_to_stream(std::cout, message_info_vec[msg.get_id_message()], append);
    }

    void CanfdUtils::print_message(const MessageInfoVec& mset, bool append) {
        write_mset_to_stream(std::cout, mset, append);
    }


    void CanfdUtils::generate_msg_info_set(MessageInfoVec& mset, size_t num){
        std::random_device rd;
        std::mt19937 gen(rd());

        mset.reserve(num);

        // 生成随机的 period、 data_size、源ECU、目ECU 分布、offset、level

        std::discrete_distribution<> dist_size(PROBABILITY_MESSAGE_SIZE, PROBABILITY_MESSAGE_SIZE + NUM_MESSAGE_SIZE);
  
        std::discrete_distribution<>dist_period(PROBABILITY_MESSAGE_PERIOD, PROBABILITY_MESSAGE_PERIOD+ NUM_MESSAGE_PERIOD);

        std::uniform_int_distribution<>dist_ecu1(0, NUM_MESSAGE_ECU - 1);
        std::uniform_int_distribution<>dist_ecu2(0, NUM_MESSAGE_ECU - 2);

        std::uniform_real_distribution<> dist_offset(0.0, 1.0);

        std::discrete_distribution<>dist_level(PROBABILITY_MESSAGE_LEVEL, PROBABILITY_MESSAGE_LEVEL+ NUM_MESSAGE_LEVEL);

        int size = 0, period = 0, deadline = 0, src = 0, dst = 0, offset = 0, level = 0;

        // 获取一个ecu数组的备份
        std::array<int, NUM_MESSAGE_ECU> option_ecu_copy;
        for (size_t i = 0; i < NUM_MESSAGE_ECU; ++i) {
            option_ecu_copy[i] = OPTION_MESSAGE_ECU[i];
        }

        std::hash<std::string> hash_fn;

        for (size_t i = 0; i < num; i++) {
            size = OPTION_MESSAGE_SIZE[dist_size(gen)];

            period = OPTION_MESSAGE_PERIOD[dist_period(gen)];
            deadline = period;

            src = option_ecu_copy[dist_ecu1(gen)];
            std::swap(option_ecu_copy[NUM_MESSAGE_ECU - 1], option_ecu_copy[src]);
            dst = option_ecu_copy[dist_ecu2(gen)];
            
            offset = dist_offset(gen) * period;

            level = OPTION_MESSAGE_LEVEL[dist_level(gen)];

            auto timestamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
            // 拼接所有字段（包括时间戳）
            std::string code_str = std::to_string(size) + "-" + std::to_string(period) + "-" +
                std::to_string(deadline) + "-" + std::to_string(src) + "-" +
                std::to_string(dst) + "-" + std::to_string(offset) + "-" +
                std::to_string(level) + "-" + std::to_string(timestamp);

            // 使用哈希生成唯一的code
            size_t code = hash_fn(code_str);

            mset.emplace_back(code,size, period, deadline, src, dst, offset, level);
        }

        return;
    }





    int CanfdFrame::payload_size_trans(int size)
    {
        int payload_sizes[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64 };
        int num_sizes = sizeof(payload_sizes) / sizeof(payload_sizes[0]);//16
        double byte_size = double(size / 8.0);
        for (int i = 0; i < num_sizes; ++i) {
            if (byte_size <= payload_sizes[i]) {
                return payload_sizes[i];
            }
        }
        return -1;
    }

    double CanfdFrame::calc_wctt(int paylaod_size)
    {
        int p = paylaod_size;
        double wctt = 32 * TIME_BIT_ARBITRATION + (28 + 5 * ceil(p - 16 / 64.0) + 10.0 * p) * TIME_BIT_TRANSMISSION;
        return wctt;
    }

    int CanfdFrame::get_min_msg_offset()
    {
        // 如果 loaded_msgs 为空，抛出异常
        if (msg_set.empty()) {
            throw std::runtime_error("错误: 调用 get_min_msg_offset() 时 loaded_msgs 为空");
        }
        return (*msg_set.begin()).get_offset();
    }

    int CanfdFrame::get_max_msg_offset()
    {
        // 如果 loaded_msgs 为空，抛出异常
        if (msg_set.empty()) {
            throw std::runtime_error("错误: 调用 get_max_msg_offset() 时 loaded_msgs 为空");
        }
        return (*std::prev(msg_set.end())).get_offset();
    }



    bool CanfdFrame::add_message(Message& m,int limit_size)
    {
        //放得下
        if (limit_size - data_size < m.get_data_size()) {
            return false;
        }
        //周期合适,信号周期为CAN-FD帧周期1~N倍时才可装载
        if (m.get_period() % this->period != 0 || m.get_period() > this->period * FACTOR_M_F_PERIOD) {
            return false;
        }
        //源目ECU一致
        if ( m.get_ecu_pair() != this->ecu_pair) {
            return false;
        }
        // 待装载消息的offset 不可相对已装载最小消息的offset相差过多
        if (m.get_offset() >= this->period* FACTOR_TIME_MSG_USABLE_WAIT+get_min_msg_offset()) {
            return false;
        }
        double temp_wctt = calc_wctt(this->data_size + m.get_data_size());

        int temp_offset = m.get_offset();
        //if()
        for (const auto& msg : msg_set) {
            if (msg.get_offset() > temp_offset)
                temp_offset = msg.get_offset();
        }
        int temp_deadline = this->period;

        //int wait_delay = 0;//消息等待帧的 等待延迟
        //for (const MessagePtr& mp : loaded_msgs) {
        //    wait_delay = (this->offset - mp->offset + this->period)% this->period;
        //    if (wait_delay + temp_wctt > deadline) return false;
        //    //获取最短的截止日期
        //    if (mp->deadline - wait_delay < temp_deadline) {
        //        temp_deadline = deadline - wait_delay;
        //    }
        //}
        //CANFD帧截止时间太紧张则舍弃
        if (temp_deadline < temp_wctt) {
            return false;
        }

        this->set_data_size(this->data_size + m.get_data_size());
        this->set_offset(temp_offset);
        this->set_deadline(temp_deadline);
        msg_set.insert(m);
        m.assign_frame(this->id);
        return true;
    }

    bool CanfdFrame::add_message(MessageVec& s)
    {
        bool flag = true;
        for (auto msg : s) {
            if (!add_message(msg)) {
                std::cout << "WARNNING::帧无法插入" << std::endl;
                CanfdUtils::print_message(msg, true);
            }
        }
        return true;
    }


}
