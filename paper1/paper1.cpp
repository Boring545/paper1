// paper1.cpp: 定义应用程序的入口点。
//

#include "packing_scheme.h"
#include "probabilistic_analysis.h"

const std::string folder_path = "../../storage/"; // 存储测试数据的文件
auto sa = cfd::packing::PACK_METHOD::SIMULATED_ANNEALING;

// 返回时间戳
std::string create_msg()
{
	cfd::utils::generate_msg_info_set();
	return cfd::utils::store_msg(folder_path);
}

void read_data_1(std::string msg_file_name)
{
	// 读取消息
	cfd::utils::read_message(folder_path + msg_file_name);
	return;
}
cfd::PackingScheme read_data_2()
{
	// 读取消息
	cfd::utils::read_message(folder_path + "msg_2025611_13614.txt");

	// 读取对应的打包方案
	cfd::CanfdFrameMap fmap;
	cfd::utils::read_frame(fmap, folder_path + "frm_2025611_132913.txt");
	return cfd::PackingScheme(fmap);
}
// 单次测试
void task1()
{
	DEBUG_MSG_DEBUG1(std::cout, "生成信号集合,数量：", cfd::SIZE_ORIGINAL_MESSAGE);
	create_msg(); // 生成信号集合

	DEBUG_MSG_DEBUG1(std::cout, "生成打包方案");
	cfd::PackingScheme scheme_origin{}; // 生成打包方案

	cfd::packing::frame_pack(scheme_origin, sa); // sa优化初始打包方案

	cfd::utils::store_frm(scheme_origin.frame_map, folder_path); // 存储打包方案

	cfd::PackingScheme scheme_repack = scheme_origin; // 未来对信号重打包

	std::string timestamp = get_time_stamp();

	DEBUG_MSG_DEBUG1(std::cout, "1. 概率分析原打包方案");
	auto res_origin = cfd::schedule::paper2::probabilistic_analysis(scheme_origin, BACKUP_OFF, timestamp); // 仅分析

	DEBUG_MSG_DEBUG1(std::cout, "2. 概率分析重打包方案方案");
	auto res_repack = cfd::schedule::paper2::probabilistic_analysis(scheme_repack, BACKUP_REPACK, timestamp); // 重打包
	std::string cmppath = folder_path + "cmp_result_" + timestamp + ".txt";

	cfd::schedule::paper2::compare_prob_result(cmppath, res_origin, res_repack, scheme_origin, scheme_repack); // 保存分析结果

	return;
}

// 读取消息集合并单次测试
void task2()
{
	read_data_1("msg_202599_15323.txt");		 // 读取特定信号集合
	cfd::PackingScheme scheme_origin{};			 // 生成打包方案
	cfd::packing::frame_pack(scheme_origin, sa); // sa优化初始打包方案

	cfd::utils::store_frm(scheme_origin.frame_map, folder_path); // 存储打包方案

	std::string timestamp = get_time_stamp();

	for (double lambda = 0.002; lambda <= 0.05; lambda += 0.002)
	{
		cfd::schedule::paper2::LAMBDA = lambda;
		DEBUG_MSG_DEBUG1(std::cout, "LAMBDA: ", lambda);

		DEBUG_MSG_DEBUG1(std::cout, "1. 概率分析原打包方案");
		auto res_origin = cfd::schedule::paper2::probabilistic_analysis(scheme_origin, BACKUP_OFF, timestamp); // 仅分析

		DEBUG_MSG_DEBUG1(std::cout, "2. 概率分析重打包方案方案");
		

		cfd::PackingScheme scheme_repack = scheme_origin;														  // 未来对信号重打包
		auto res_repack = cfd::schedule::paper2::probabilistic_analysis(scheme_repack, BACKUP_REPACK, timestamp); // 重打包
		std::string cmppath = folder_path + "cmp_result_" + timestamp + ".txt";

		cfd::schedule::paper2::compare_prob_result(cmppath, res_origin, res_repack, scheme_origin, scheme_repack); // 保存分析结果
	}
	//TODO 
	//已经做了变lambda，还有变EI_LEN和变mat

	return;
}

// 多轮循环测试
void task3()
{

	double res = 0;
	double trynum = 20;
	for (int i = 0; i < trynum; i++)
	{
		DEBUG_MSG_DEBUG1(std::cout, "生成信号集合,数量：", cfd::SIZE_ORIGINAL_MESSAGE);
		create_msg(); // 生成信号集合

		DEBUG_MSG_DEBUG1(std::cout, "元启发算法 生成打包方案");
		cfd::PackingScheme scheme_origin{false};			// 生成打包方案
		res += cfd::packing::frame_pack(scheme_origin, sa); // sa优化初始打包方案
	}
	res /= trynum;
	DEBUG_MSG_DEBUG1(std::cout, "平均带宽利用率：", res);
}

int main()
{
	// task1();
	task2();
	// task3()

	return 0;
}
