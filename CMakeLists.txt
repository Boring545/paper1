# CMakeList.txt: 顶层 CMake 项目文件，在此处执行全局配置
# 并包含子项目。
#
cmake_minimum_required(VERSION 3.8)

project("paper1")

set(CMAKE_CXX_STANDARD 17)

# 针对 MSVC 编译器，禁用优化并启用调试信息，同时启用性能分析
if (MSVC)
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Od /Zi /PROFILE") # 禁用优化，启用调试信息并启用性能分析
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /Od /Zi /PROFILE")     # 禁用优化，启用调试信息并启用性能分析
endif()

# 配置 vcpkg 工具链文件【按照自己的路径修改】
set(CMAKE_TOOLCHAIN_FILE "D:/tool/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
set(CMAKE_PREFIX_PATH "D:/tool/vcpkg/vcpkg/installed/x64-windows/share" ${CMAKE_PREFIX_PATH})


# 查找 nlohmann_json 库
find_package(nlohmann_json CONFIG REQUIRED)

# 包含子项目
add_subdirectory("paper1")



