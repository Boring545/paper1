# CMakeList.txt: paper1 的 CMake 项目，在此处包括源代码并定义
# 项目特定的逻辑。
#
cmake_minimum_required (VERSION 3.8)

# 将源代码添加到此项目的可执行文件。
add_executable (paper1 "paper1.cpp" "paper1.h" "canfd_frame.h" "canfd_frame.cpp" "frame_packing.h" "frame_packing.cpp")

# TODO: 如有需要，请添加测试并安装目标。
