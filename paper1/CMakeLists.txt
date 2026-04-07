# CMakeList.txt: paper1 的 CMake 项目，在此处包括源代码并定义
# 项目特定的逻辑。
#
cmake_minimum_required (VERSION 3.8...4.3)
project("paper1")

add_executable(paper1
    paper1.cpp
    canfd_frame.cpp
    scheme.cpp
    priority_allocation.cpp

    packing_algorithms/sa_algorithm.cpp

    probabilistic_analysis/retry.cpp
    probabilistic_analysis/normal.cpp
    probabilistic_analysis/no_retry.cpp

    backups/signal_backup.cpp
    backups/frame_backup.cpp
)

target_include_directories(paper1
    PRIVATE
        ${PROJECT_SOURCE_DIR}
)

target_link_libraries(paper1
    PRIVATE
        nlohmann_json::nlohmann_json
)



