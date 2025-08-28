# **README - 使用 vcpkg 安装并集成 nlohmann/json 库**

## **1. 安装 vcpkg**
安装 `vcpkg` 包管理工具。

1. **合适的地方，克隆 `vcpkg` 仓库：**
   ```sh
    git clone https://github.com/microsoft/vcpkg.git
    cd vcpkg
   ```

2. **运行 `bootstrap` 脚本进行安装：**
   - **Windows（PowerShell）：**
     ```sh
     .\bootstrap-vcpkg.bat
     ```
   - **Linux/macOS：**
     ```sh
     ./bootstrap-vcpkg.sh
     ```
## **2. 安装 nlohmann/json 库**
安装 `nlohmann/json` 库，使用 `vcpkg` 来下载、编译并安装它。

1. **运行安装命令：**
   ```sh
   ./vcpkg install nlohmann-json
   ```

2. 安装完成后，库会被安装到 `vcpkg/installed/<platform>` 目录中。

---

## **3. 在 CMake 项目中使用 nlohmann/json**

### **步骤：**
1. **在顶层 CMakeLists.txt 文件中配置 `vcpkg` 工具链：**

   修改 `CMakeLists.txt` 文件，在文件顶部加入如下代码，设置 `vcpkg` 的工具链文件路径：

   ```cmake
   # 配置 vcpkg 工具链文件
    set(CMAKE_TOOLCHAIN_FILE "D:/tool/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
    set(CMAKE_PREFIX_PATH "D:/tool/vcpkg/vcpkg/installed/x64-windows/share" ${CMAKE_PREFIX_PATH})
   ```

   **注意：**
   - 将 `"D:/tool/vcpkg"` 替换为你本地的 `vcpkg` 安装路径。
   - 将 替换自己的的`"/share"`文件位置

2. **查找并链接 nlohmann/json 库：**
   

   ```cmake
   find_package(nlohmann_json CONFIG REQUIRED)
   target_link_libraries(your_target_name PRIVATE nlohmann_json::nlohmann_json)
   ```

3. **在 C++ 代码中使用 JSON 库：**

   现在，才可以在代码中直接使用 `nlohmann/json` 库：

   ```cpp
   #include <nlohmann/json.hpp>
   ```

---



