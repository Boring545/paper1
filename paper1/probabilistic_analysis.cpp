#include"probabilistic_analysis.h"
namespace cfd::schedule::paper2 {

	// 计算阶乘
	long long int factorial(int m) {
		//别用它计算太大的数
		if (m < 0) {
			throw std::invalid_argument("M 必须大于0");
		}
		if (m == 0) {
			return 1;
		}
		if (m > 20) {
			throw std::invalid_argument("M 大于20会导致越界");
		}
		long long int result = 1;
		while (m > 0) {
			result *= m;
			m--;
		}
		return result;
	}
	double calc_probability_fault(double t, int num)
	{
		return std::exp(-1 * LAMBDA * t) * std::pow(LAMBDA * t, num) / factorial(num);
	}

	double calc_interference()
	{
		return 0.0;
	}

	class StateNode {
	public:
		int rx_num = 0;	// 总重传次数
		double response_time = 0;		// 响应时间
		double p = 1;		// 概率
		double t_pre = 0;
		double t = 0;
		std::vector<std::shared_ptr<StateNode>>children;


		StateNode(int rx_num = 0, double response_time = 0.0, double p = 1.0)
			: rx_num(rx_num), response_time(response_time), p(p) {}

		void addChild(std::shared_ptr<StateNode> child) {
			children.emplace_back(child);
		}
	};

	ProbResult analyze_frame_probability(
		const CanfdFrame& frame,
		const std::vector<CanfdFrame>& sorted_frames,
		const std::unordered_map<FrameId, double>& max_fault_rate,
		int frame_index,
		double COST_MAX_ERROR_FRAME
	) {
		auto period = frame.get_period();
		auto trans_time = frame.get_trans_time();
		auto fid = frame.id;
		auto fault_thd = max_fault_rate.at(fid);

		// 计算高优先级帧和低优先级帧的最大传输时间
		double max_hep_trans_time = 0.0;
		double max_lp_trans_time = 0.0;

		for (int j = 0; j < sorted_frames.size(); j++) {
			auto ft = sorted_frames[j].get_trans_time();
			if (j <= frame_index) {  // 高优先级帧
				if (ft > max_hep_trans_time) {
					max_hep_trans_time = ft;
				}
			}
			else {  // 低优先级帧
				if (ft > max_lp_trans_time) {
					max_lp_trans_time = ft;
				}
			}
		}

		double single_error_cost = COST_MAX_ERROR_FRAME + max_hep_trans_time;
		auto root = std::make_shared<StateNode>();
		root->t_pre = 0;
		root->t = trans_time;

		std::queue<std::shared_ptr<StateNode>> que;
		que.push(root);

		double epsilon = std::pow(10, std::floor(std::log10(fault_thd / (3600000 / period))) - 1);

		// 层次遍历构建概率分析树
		while (!que.empty()) {
			auto node = que.front();
			que.pop();

			double t_pre = node->t_pre;
			double t = node->t;

			// 计算高优先级帧的干扰
			double interference = 0.0;
			double jitter = 0.0;  // 暂不考虑抖动
			for (int j = frame_index - 1; j >= 0; j--) {
				interference += std::ceil((t - trans_time + TIME_BIT_ARBITRATION + jitter) / sorted_frames[j].get_period()) * sorted_frames[j].get_trans_time();
			}

			// 考虑重传次数 m
			for (int m = 0;; ++m) {
				int err_num = node->rx_num + m;
				double block_time = max_lp_trans_time;
				double t_next = block_time + trans_time + interference + err_num * single_error_cost;
				double prob = node->p * calc_probability_fault(t - t_pre, m);

				auto child = std::make_shared<StateNode>(err_num, t_next, prob);
				child->t_pre = t;
				child->t = t_next;
				node->addChild(child);

				if (prob < epsilon) {
					break;  // 概率太低，剪枝
				}

				if (t_next > period) {
					break;  // 不可调度，终止
				}

				if (std::abs(t_next - t) < 1e-5) {
					continue;  // 收敛，终止(注意这里，即使收敛，但也可以探索更多的可能，所以continue)
				}

				que.push(child);  // 继续分支
			}
		}

		// 遍历所有叶子节点，计算统计量
		double E_R = 0.0;
		double prob_schedulable = 0.0;
		double prob_no_failure = 0.0;

		std::stack<std::shared_ptr<StateNode>> stk;
		stk.push(root);

		while (!stk.empty()) {
			auto node = stk.top();
			stk.pop();

			if (node->children.empty()) {
				if (node->response_time <= period) {
					E_R += node->response_time * node->p;
					prob_schedulable += node->p;
					if (node->rx_num == 0) {
						prob_no_failure += node->p;
					}
				}
			}
			else {
				for (const auto& child : node->children) {
					stk.push(child);
				}
			}
		}

		double prob_unschedulable = 1.0 - prob_schedulable;

		return { E_R, prob_unschedulable };
	}




	struct bak_msg_rem {
		Message backup_msg;
		double remain_prob;

		// 带参构造函数
		bak_msg_rem(const Message& msg, double prob)
			: backup_msg(msg), remain_prob(prob) {}
	};
	// 尝试在高优先级帧中插入备份
	int try_insert_backups(PackingScheme& scheme, std::vector<bak_msg_rem>& bak_msgs,
		CanfdFrameVec& sorted_frames,
		std::unordered_map<FrameId, ProbResult>& result,
		const CanfdFrame& frame, size_t frame_index) {

		int cnt = 0;
		for (int k = 0; k < bak_msgs.size(); k++) {
			if (bak_msgs[k].remain_prob >= 1.0) {
				cnt++;
				continue;
			}

			auto& m = bak_msgs[k].backup_msg;
			for (int j = 0; j < frame_index; j++) {
				auto& hp_frame = sorted_frames[j];
				if (frame.ecu_pair == hp_frame.ecu_pair && m.get_period() % hp_frame.get_period() == 0 && m.get_data_size() <= hp_frame.get_free_size()) {

					Message mbackup(m.get_id_message());
					if (hp_frame.add_message(mbackup)) {
						scheme.message_set.emplace_back(mbackup);
						bak_msgs[k].remain_prob /= result[hp_frame.get_id()].p_timeout;// 更新剩余概率
						cnt++;
						break;
					}
				}
			}
		}
		return cnt;
	}

	






	// 存储概率分析的结果signal_p_timeout到address，额外写入带宽利用率 append=true 追加写 false 清空写
	void save_prob_result(const std::string& address, std::unordered_map<MessageCode, ProbData>& signal_p_timeout, double b_u, bool append = false) {
		std::ofstream ofs(address, append ? std::ios::app : std::ios::trunc);
		if (!ofs) {
			std::cerr << "Failed to open output file\n";
		}
		ofs << "MessageCode\tperiod\tP_Timeout\tp_threshold\n";
		for (const auto& [code, prob_pair] : signal_p_timeout) {
			ofs << code << '\t' << prob_pair.period << '\t' << prob_pair.p_timeout << '\t' << prob_pair.p_threshold << '\n';
		}
		ofs << "bandwidth_utilization: " << b_u << '\n';
		ofs.close();
	}


	std::unordered_map<MessageCode, ProbData> probabilistic_analysis(PackingScheme& scheme, int enable_backup, std::string timestamp) {
		constexpr double COST_MAX_ERROR_FRAME = 0.405;

		std::unordered_map<FrameId, ProbResult> result;
		result.reserve(scheme.frame_map.size());

		// 1. 提取并排序所有帧
		CanfdFrameVec sorted_frames;
		for (auto& [id, frame] : scheme.frame_map) {
			sorted_frames.push_back(frame);
		}
		std::sort(sorted_frames.begin(), sorted_frames.end(),
			[](const CanfdFrame& f1, const CanfdFrame& f2) {
				return f1.get_priority() < f2.get_priority();
			});

		// 2. 计算最大故障率
		std::unordered_map<FrameId, double> max_fault_rate;
		max_fault_rate.reserve(sorted_frames.size());
		for (auto& frame : sorted_frames) {
			int max_level = 0;
			for (auto& msg : frame.msg_set) {
				max_level = std::max(max_level, msg.get_level());
			}
			max_fault_rate[frame.get_id()] = THRESHOLD_RELIABILITY[max_level];
		}

		// 3. 分析每个帧
		for (size_t i = 0; i < sorted_frames.size(); i++) {
			const auto& frame = sorted_frames[i];
			FrameId fid = frame.get_id();

			// 分析当前帧
			ProbResult res = analyze_frame_probability(
				frame, sorted_frames, max_fault_rate, i, COST_MAX_ERROR_FRAME
			);

			result[fid] = res;



			// 调试输出
			DEBUG_MSG_DEBUG2(std::cout, cfd::utils::get_frame_string(frame));
			//DEBUG_MSG_DEBUG1(std::cout, "响应时间期望: ", res.e_response_time);
			DEBUG_MSG_DEBUG2(std::cout, "不可调度的概率: ", res.p_timeout, "\n");
		}

		// 收集分析结果，下标为信号的code，即为每种类型的信号（原始信号）收集分析结果
		std::unordered_map<MessageCode, ProbData> signal_p_timeout;

		for (const auto& [fid, prob_res] : result) {
			const auto& frame = scheme.frame_map.at(fid);
			for (const auto& msg : frame.msg_set) {
				auto code = msg.get_code();

				auto it = signal_p_timeout.find(code);
				if (it == signal_p_timeout.end()) {
					signal_p_timeout[code] = {
						msg.get_id_message(),
						msg.get_period(),
						msg.get_level(),
						msg.get_type(),
						THRESHOLD_RELIABILITY[msg.get_level()], 
						prob_res.p_timeout // 信号的故障概率 = 报文的故障概率 只在信号无offset，信号周期等于deadline时
						//TODO frame的不可调度概率不完全等于所装载的每个帧的不可调度概率，但理论上，同周期的信号，deadline和帧deadline一致，那些周期为帧周期倍数的信号，是特殊点
					};
				}
				else {
					it->second.p_timeout *= prob_res.p_timeout;
				}
			}
		}


		const std::string address_origin = "D:\\document\\CODE\\paper1\\storage\\signal_fault_probabilities_origin_" + timestamp + ".txt";
		const std::string address_repack = "D:\\document\\CODE\\paper1\\storage\\signal_fault_probabilities_repack_" + timestamp + ".txt";


		if (enable_backup == BACKUP_OFF) {

			save_prob_result(address_origin, signal_p_timeout, scheme.calc_bandwidth_utilization());
			return signal_p_timeout;
			
		}
		else if (enable_backup == BACKUP_REPACK) {

			// 统计每个code对应的信号的故障概率，不满足的添加估算数量的备份
			int bcnt = 0; // 添加副本信号的总数
			for (const auto& [code, prob_pair] : signal_p_timeout) {
				if (prob_pair.p_timeout <= prob_pair.p_threshold) {
					continue; // 已满足要求，不添加副本
				}

				double log_p_total = std::log(prob_pair.p_timeout);    // 当前已有副本的信号的总超时概率（乘积）取对数
				double log_p_thresh = std::log(prob_pair.p_threshold); // 安全等级要求的最大容忍超时概率也取对数

				int total_required = (int)(std::ceil(log_p_thresh / log_p_total));
				int backup_num = std::max(0, total_required - 1); // 减去原本，确定需要添加的备份的数量

				// 添加同源副本
				for (int i = 0; i < backup_num; ++i) {
					scheme.message_set.emplace_back(prob_pair.id);
					bcnt++;
				}
			}

			
			//DEBUG_MSG_DEBUG1(std::cout, cfd::utils::get_frame_string(frame));
			if (bcnt != 0) {
				DEBUG_MSG_DEBUG1(std::cout, "本次总计新增 ", bcnt, " 个同源信号副本");
				DEBUG_MSG_DEBUG1(std::cout, "元启发算法重新初始化打包方案");
				scheme.re_init_frames();//重新初始化帧打包方案
				auto sa = cfd::packing::PACK_METHOD::SIMULATED_ANNEALING;
				cfd::packing::frame_pack(scheme, sa);// sa优化
				

				DEBUG_MSG_DEBUG1(std::cout, "对添加副本信号的信打包方案进行概率分析");
				return probabilistic_analysis(scheme, enable_backup, timestamp);//递归分析直至收敛
			}
			else {
				//递归迭代终止
				save_prob_result(address_repack, signal_p_timeout, scheme.calc_bandwidth_utilization(), true);
				return signal_p_timeout;
			}

		}

		return signal_p_timeout;
	}





	void compare_prob_result(
		const std::string& filename,
		const std::unordered_map<MessageCode, ProbData>& origin_res,
		const std::unordered_map<MessageCode, ProbData>& repack_res,
		const PackingScheme& scheme_origin,
		const PackingScheme& scheme_repack)
	{
		std::ofstream ofs(filename);
		if (!ofs.is_open()) {
			throw std::runtime_error("无法打开文件: " + filename);
		}

		ofs << std::scientific << std::setprecision(10); // 科学计数法输出概率

		// 输出表头，用\t分隔
		ofs << "MessageCode\t周期\t安全等级\t异源备份\t故障概率上限\t故障概率(原本)\t故障概率(重打包)\n";

		for (const auto& [code, data_origin] : origin_res) {
			double p_repack = 1.0;
			auto it = repack_res.find(code);
			if (it != repack_res.end()) {
				p_repack = it->second.p_timeout;
			}
			else {
				DEBUG_MSG_DEBUG1(std::cout, "compare_prob_result::原本副本code不匹配");
			}

			// level映射到字母
			char level_char = 'A' + data_origin.level; // 0->A, 1->B, 2->C, 3->D
			// type映射到中文“否/是”
			std::string type_str = (data_origin.type == 0) ? "否" : "是";

			ofs << code << "\t"
				<< data_origin.period << "\t"
				<< level_char << "\t"
				<< type_str << "\t"
				<< data_origin.p_threshold << "\t"
				<< data_origin.p_timeout << "\t"
				<< p_repack << "\n";
		}


		// 输出带宽利用率，固定小数点
		ofs << std::fixed << std::setprecision(10);
		ofs << "U 未添加副本:\t" << scheme_origin.calc_bandwidth_utilization() << "\n";
		ofs << "U 添加副本:\t" << scheme_repack.calc_bandwidth_utilization() << "\n";
	}




}