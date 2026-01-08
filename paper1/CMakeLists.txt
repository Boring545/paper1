# CMakeList.txt: paper1 的 CMake 项目，在此处包括源代码并定义
# 项目特定的逻辑。
#
cmake_minimum_required (VERSION 3.8...4.3)

# 将源代码添加到此项目的可执行文件。
add_executable (paper1 
    paper1.cpp 
    canfd_frame.cpp 
    packing_scheme.cpp 
    priority_allocation.cpp 
    probabilistic_analysis.cpp 
    packing_algorithms/sa_algorithm.cpp)

include_directories(${PROJECT_SOURCE_DIR}/paper1)

# 链接 nlohmann_json 库
target_link_libraries(paper1 PRIVATE nlohmann_json::nlohmann_json)
