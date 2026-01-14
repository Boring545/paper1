// paper1.cpp: 定义应用程序的入口点。
//

#include "packing_scheme.h"
#include "probabilistic_analysis.h"


#ifdef _WIN32
extern "C" __declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int);
extern "C" __declspec(dllimport) int __stdcall SetConsoleCP(unsigned int);
const std::string  folder_path = "D:/document/CODE/paper1/storage/";		// 存储测试数据的文件
#else
const std::string folder_path = "../../storage/"; // 存储测试数据的文件
#endif

#ifndef CP_UTF8
#define CP_UTF8 65001
#endif



//
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

	for (double lambda = 0; lambda <= 0.03; lambda += 0.002)
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


// 读取消息集合并单次测试
void task3()
{
	read_data_1("msg_202599_15323.txt");		 // 读取特定信号集合
	cfd::PackingScheme scheme_origin{};			 // 生成打包方案
	cfd::packing::frame_pack(scheme_origin, sa); // sa优化初始打包方案

	cfd::utils::store_frm(scheme_origin.frame_map, folder_path); // 存储打包方案

	std::string timestamp = get_time_stamp();

	//double lambda = 1e-2; // 按照论文描述，建议选择3e-2，但测试可以选择更大
	//double eilen = 1e-2; // 不要太大，因为一个报文传输时间差不多0.04ms或者更大，干扰太大直接全覆盖了，备份无法提高概率，推荐1E-2这个量级或者更小

	std::vector<double> lambda_vec = { 0.0,1e-3, 3e-3,1e-2, 3e-2,1e-1,3e-1,1 };

	std::vector<double> len_vec = { 0.0,1e-4, 3e-4,1e-3, 3e-3,1e-2, 3e-2,1e-1 };
	bool flag = true;

	// 只变 lambda
	cfd::schedule::paper2::EI_LEN = 3e-3;
	for (auto lambda : lambda_vec) {
		cfd::schedule::paper2::LAMBDA = lambda;
		DEBUG_MSG_DEBUG1(std::cout, "LAMBDA: ", cfd::schedule::paper2::LAMBDA);
		DEBUG_MSG_DEBUG1(std::cout, "EI_LEN: ", cfd::schedule::paper2::EI_LEN);

		DEBUG_MSG_DEBUG1(std::cout, "1. 概率分析原打包方案");
		auto res_origin = cfd::schedule::paper2::probabilistic_analysis(scheme_origin, BACKUP_OFF, timestamp); // 仅分析
		if (flag) {
			DEBUG_MSG_DEBUG1(std::cout, "2. 概率分析重打包方案方案");
			cfd::PackingScheme scheme_repack = scheme_origin;														  // 未来对信号重打包
			auto res_repack = cfd::schedule::paper2::probabilistic_analysis(scheme_repack, BACKUP_REPACK, timestamp); // 重打包
			if (res_repack.empty()) {
				continue;
			}
			std::string cmppath = folder_path + "cmp_result_" + timestamp + ".txt";

			cfd::schedule::paper2::compare_prob_result(cmppath, res_origin, res_repack, scheme_origin, scheme_repack); // 保存分析结果
		}
	}


	timestamp = get_time_stamp();

	// 只变eilen
	cfd::schedule::paper2::LAMBDA = 3e-2;
	for (auto eilen : len_vec) {
		cfd::schedule::paper2::EI_LEN = eilen;
		DEBUG_MSG_DEBUG1(std::cout, "LAMBDA: ", cfd::schedule::paper2::LAMBDA);
		DEBUG_MSG_DEBUG1(std::cout, "EI_LEN: ", cfd::schedule::paper2::EI_LEN);

		DEBUG_MSG_DEBUG1(std::cout, "1. 概率分析原打包方案");
		auto res_origin = cfd::schedule::paper2::probabilistic_analysis(scheme_origin, BACKUP_OFF, timestamp); // 仅分析
		if (flag) {
			DEBUG_MSG_DEBUG1(std::cout, "2. 概率分析重打包方案方案");
			cfd::PackingScheme scheme_repack = scheme_origin;														  // 未来对信号重打包
			auto res_repack = cfd::schedule::paper2::probabilistic_analysis(scheme_repack, BACKUP_REPACK, timestamp); // 重打包
			if (res_repack.empty()) {
				continue;
			}
			std::string cmppath = folder_path + "cmp_result_" + timestamp + ".txt";

			cfd::schedule::paper2::compare_prob_result(cmppath, res_origin, res_repack, scheme_origin, scheme_repack); // 保存分析结果k
			cfd::schedule::paper2::compare_prob_result(cmppath, res_origin, res_repack, scheme_origin, scheme_repack); // 保存分析结果k

		}

	}


	//TODO 
	//已经做了变lambda，还有变EI_LEN和变mat

	return;
}
int main() {
#ifdef _WIN32
	// 让 Windows 控制台按 UTF-8 显示（否则中文经常变成“浼樺寲”这种乱码）
	SetConsoleOutputCP(CP_UTF8);
	SetConsoleCP(CP_UTF8);
#endif

	// task1();
	//task2();
	task3();

	return 0;
}
