#ifndef CONFIG_H
#define CONFIG_H

#include <string>
#include <xutility>

#define V0 1
#define V1 0
#define V2 0
#define OFFSET_TEST

namespace cfd::packing {
enum class PACK_METHOD {
  SIMULATED_ANNEALING = 0,
};
}  // namespace cfd::packing

namespace cfd::schedule {
enum class PRIORITY_ASSIGN_METHOD {
  OPTIMAL = 0,
  BY_PERIOD = 1,
};
}  // namespace cfd::schedule

namespace cfd {
const std::string TEST_INFO_PATH = "D:/document/CODE/paper1/storage/";
const std::string DEFAULT_MSG_FILE = "msg_2026312_21355_tab.txt";
const std::string DEFAULT_FRM_FILE = "frm_2025611_132913.txt";

const cfd::packing::PACK_METHOD DEFAULT_PACK_METHOD = cfd::packing::PACK_METHOD::SIMULATED_ANNEALING;
const cfd::schedule::PRIORITY_ASSIGN_METHOD DEFAULT_PRIORITY_ASSIGN_METHOD =
    cfd::schedule::PRIORITY_ASSIGN_METHOD::BY_PERIOD;

constexpr int REDUNDANCY_N = 3;
constexpr double LAMBDA_CONFERENCE = 1e-3;
constexpr int FACTOR_M_F_PERIOD = 2;

constexpr int OPTION_MESSAGE_SIZE[] = {1, 2, 4, 8, 16, 32, 64};
constexpr int NUM_MESSAGE_SIZE = std::size(OPTION_MESSAGE_SIZE);
constexpr double PROBABILITY_MESSAGE_SIZE[] = {0.35, 0.49, 0.13, 0.008, 0.013, 0.005, 0.004};

constexpr int OPTION_MESSAGE_LEVEL[] = {0, 1, 2, 3};
constexpr int NUM_MESSAGE_LEVEL = std::size(OPTION_MESSAGE_LEVEL);
constexpr double PROBABILITY_MESSAGE_LEVEL[] = {0.75, 0.1, 0.1, 0.05};

constexpr double THRESHOLD_RELIABILITY[] = {1e10, 1e-7, 1e-7, 1e-8};
constexpr int NUM_THRESHOLD_RELIABILITY = sizeof(THRESHOLD_RELIABILITY) / sizeof(THRESHOLD_RELIABILITY[0]);

#if V0
constexpr int SIZE_ORIGINAL_MESSAGE = 100;
constexpr int OPTION_MESSAGE_PERIOD[] = {1, 2, 5, 10, 20, 50, 100, 1000};
constexpr int NUM_MESSAGE_PERIOD = std::size(OPTION_MESSAGE_PERIOD);
constexpr double PROBABILITY_MESSAGE_PERIOD[] = {0.04, 0.03, 0.03, 0.31, 0.31, 0.03, 0.2, 0.05};

constexpr int OPTION_ECU[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
constexpr int NUM_ECU = std::size(OPTION_ECU);

constexpr int OPTION_MESSAGE_TYPE[] = {0, 1, 2};
constexpr int NUM_MESSAGE_TYPE = std::size(OPTION_MESSAGE_TYPE);
constexpr double PROBABILITY_MESSAGE_TYPE[] = {0.99, 0.01, 0};

#elif V1
constexpr int SIZE_ORIGINAL_MESSAGE = 1000;
constexpr int OPTION_MESSAGE_PERIOD[] = {2, 5, 10, 20, 50, 100};
constexpr int NUM_MESSAGE_PERIOD = std::size(OPTION_MESSAGE_PERIOD);
constexpr double PROBABILITY_MESSAGE_PERIOD[] = {0.02, 0.02, 0.26, 0.26, 0.04, 0.4};

constexpr int OPTION_ECU[] = {0, 1, 2, 3};
constexpr int NUM_ECU = std::size(OPTION_ECU);

constexpr int OPTION_MESSAGE_TYPE[] = {0, 1, 2};
constexpr int NUM_MESSAGE_TYPE = std::size(OPTION_MESSAGE_TYPE);
constexpr double PROBABILITY_MESSAGE_TYPE[] = {0.99, 0.01, 0};

#elif V2
constexpr int SIZE_ORIGINAL_MESSAGE = 700;
constexpr int OPTION_MESSAGE_PERIOD[] = {2, 5, 10, 20, 50, 100};
constexpr int NUM_MESSAGE_PERIOD = std::size(OPTION_MESSAGE_PERIOD);
constexpr double PROBABILITY_MESSAGE_PERIOD[] = {0.02, 0.02, 0.26, 0.26, 0.04, 0.4};

constexpr int OPTION_ECU[] = {0, 1, 2, 3, 4, 5, 6, 7};
constexpr int NUM_ECU = std::size(OPTION_ECU);

constexpr int OPTION_MESSAGE_TYPE[] = {0, 1, 2};
constexpr int NUM_MESSAGE_TYPE = std::size(OPTION_MESSAGE_TYPE);
constexpr double PROBABILITY_MESSAGE_TYPE[] = {0.99, 0.01, 0};
#endif
}  // namespace cfd

#endif  // CONFIG_H
