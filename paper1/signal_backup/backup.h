#pragma once
#include "../canfd_frame.h"
#include "../config.h"
#include "../scheme.h"

namespace cfd::backups {

// 同源备份
PackingScheme homo_signal_backup(PackingScheme& scheme, double lambda = LAMBDA_CONFERENCE);

// 异源备份：N模冗余（N为奇数，默认3）
// 仅对type==1的信号操作，且要求可用源ECU数量 >= N
PackingScheme hetero_signal_backup(PackingScheme& scheme, int redundancy_n = 3,
                                   double lambda = LAMBDA_CONFERENCE);

}  // namespace cfd::backups
