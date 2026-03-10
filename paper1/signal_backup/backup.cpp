#include"signal_backup/backup.h"
#include"probabilistic_analysis/normal.h"
namespace cfd::backups {
	// 同源备份
	PackingScheme homo_signal_backup(PackingScheme& scheme, double lambda = LAMBDA_CONFERENCE) {
		auto result = analysis::probabilistic_analysis(scheme, lambda);
		//1. 检查结果，过程中收集问题信号，估算备份数量，没问题则退出
		//2. 添加同源信号备份
		//3. 重打包，不可调度则异常退出
		//4. 回到1
	}

	// 异源备份
	PackingScheme hetero_signal_backup(PackingScheme& scheme, double lambda = LAMBDA_CONFERENCE) {
		auto result = analysis::probabilistic_analysis(scheme, lambda);
		// 1. 对特定信号，选择三个ECU。至少有四个ECU
		//       首先尝试将其副本直接插入现有报文中发送；
		//       若无法满足，则优先选择当前带宽利用率较低的 ECU 作为冗余信号源，插入后检查可调度性。
	}




}