// paper1.cpp: 定义应用程序的入口点。
//

#include "frame_packing.h"
#include"sa_algorithm.h"
#include"probabilistic_analysis.h"


const std::string  folder_path= "D:/document/CODE/paper1/storage/";		// 存储测试数据的文件


// 返回时间戳
std::string create_msg() {
	cfd::utils::generate_msg_info_set();
	return cfd::utils::store_msg(folder_path);
}


cfd::PackingScheme read_data() {
	// 读取消息
	cfd::utils::read_message(folder_path+ "msg_2025611_13614.txt");

	//读取对应的打包方案
	cfd::CanfdFrameMap fmap;
	cfd::utils::read_frame(fmap, folder_path + "frm_2025611_132913.txt");
	return cfd::PackingScheme(fmap);
}

void task1() {
	DEBUG_MSG_DEBUG1(std::cout, "生成信号集合,数量：", cfd::SIZE_ORIGINAL_MESSAGE);
	create_msg();	//生成信号集合

	DEBUG_MSG_DEBUG1(std::cout, "元启发算法 生成打包方案");
	cfd::PackingScheme scheme_origin{};// 生成打包方案

	cfd::packing::heuristics::simulated_annealing(scheme_origin);// sa优化初始打包方案

	cfd::utils::store_frm(scheme_origin.frame_map, folder_path);// 存储打包方案

	cfd::PackingScheme scheme_repack = scheme_origin;						// 未来对信号重打包

	std::string timestamp = get_time_stamp();

	DEBUG_MSG_DEBUG1(std::cout, "1. 概率分析原打包方案");
	auto res_origin = cfd::schedual::paper2::probabilistic_analysis(scheme_origin,BACKUP_OFF, timestamp);		//仅分析



	DEBUG_MSG_DEBUG1(std::cout, "2. 概率分析重打包方案方案");
	auto res_repack = cfd::schedual::paper2::probabilistic_analysis(scheme_repack, BACKUP_REPACK, timestamp);	//重打包
	std::string cmppath = folder_path + "cmp_result_" + timestamp + ".txt";
	
	cfd::schedual::paper2::compare_prob_result(cmppath, res_origin, res_repack, scheme_origin, scheme_repack);// 保存分析结果

	return;
}

void task2() {
	cfd::PackingScheme scheme_origin = read_data(); // 从文件读取原打包方案
	auto scheme_repack = scheme_origin;					  // 未来对信号重打包

	std::string timestamp = get_time_stamp();

	auto res_origin = cfd::schedual::paper2::probabilistic_analysis(scheme_origin, BACKUP_OFF, timestamp);		//仅分析
	auto res_repack = cfd::schedual::paper2::probabilistic_analysis(scheme_repack, BACKUP_REPACK, timestamp);	//重打包

	std::string cmppath = folder_path + "cmp_result_" + timestamp + ".txt";
	cfd::schedual::paper2::compare_prob_result(cmppath, res_origin, res_repack, scheme_origin, scheme_repack);// 保存分析结果

	return;
}

void task3() {

	double res = 0;
	double trynum = 20;
	for (int i = 0; i < trynum; i++) {
		DEBUG_MSG_DEBUG1(std::cout, "生成信号集合,数量：", cfd::SIZE_ORIGINAL_MESSAGE);
		create_msg();	//生成信号集合

		DEBUG_MSG_DEBUG1(std::cout, "元启发算法 生成打包方案");
		cfd::PackingScheme scheme_origin{ false };// 生成打包方案
		res += cfd::packing::heuristics::simulated_annealing(scheme_origin);// sa优化初始打包方案
	}
	res /= trynum;
	DEBUG_MSG_DEBUG1(std::cout, "平均带宽利用率：", res);
}



int main()
{
	task1();
	//task2();
	//task3()

	
	return 0;
}
