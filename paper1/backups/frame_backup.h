#pragma once

#include "canfd_frame.h"
#include "config.h"
#include "scheme.h"

namespace cfd::backups::frame {

// Homo-source frame backup based on per-frame fault probability.
PackingScheme homo_frame_backup(PackingScheme& scheme, double lambda = LAMBDA_CONFERENCE);

// Homo-source frame backup method 2:
// ASIL B/C uses bc_backup_num copies, ASIL D uses d_backup_num copies.
PackingScheme homo_frame_backup_method2(PackingScheme& scheme, int bc_backup_num = 1, int d_backup_num = 2);

// Hetero-source frame backup placeholder. 
PackingScheme hetero_frame_backup(PackingScheme& scheme, int redundancy_n = REDUNDANCY_N,
                                  double lambda = LAMBDA_CONFERENCE);

}  // namespace cfd::backups::frame
