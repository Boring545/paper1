# CMakeList.txt: paper1 的 CMake 项目，在此处包括源代码并定义
# 项目特定的逻辑。
#
cmake_minimum_required (VERSION 3.8)

# 将源代码添加到此项目的可执行文件。
add_executable (paper1 "paper1.cpp"  "canfd_frame.h" "canfd_frame.cpp" "frame_packing.h" "frame_packing.cpp"    "priority_allocation.h" "priority_allocation.cpp" "debug_tool.h" "sa_algorithm.h" "sa_algorithm.cpp"   "probabilistic_analysis.cpp" "probabilistic_analysis.h")

# 链接 nlohmann_json 库
target_link_libraries(paper1 PRIVATE nlohmann_json::nlohmann_json)
