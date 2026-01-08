#ifndef DEBUGTOOL_H
#define DEBUGTOOL_H

#include <iostream>
#include <ostream>
#include <chrono>
#include <ctime>
#include <sstream>
#include <string>

inline std::string get_time_stamp() {
    // 获取当前时间戳
    auto now = std::chrono::system_clock::now();
    auto now_time_t = std::chrono::system_clock::to_time_t(now);

    // 将时间戳转换为字符串，格式为 "yyyyMMdd_HHmmss"
    std::tm tm = *std::localtime(&now_time_t);
    std::ostringstream timestamp;
    timestamp << (tm.tm_year + 1900) << (tm.tm_mon + 1) << tm.tm_mday << "_"
        << tm.tm_hour << tm.tm_min << tm.tm_sec;
    return timestamp.str();
}

/*
==========================================================
通过定义 #define DEBUG_OUTPUT 和 DebugLevel 来控制输出等级
==========================================================
*/
#define DEBUG_OUTPUT DebugLevel::DEBUG1

// 枚举定义
enum class DebugLevel {
    INFO,
    DEBUG1,
    DEBUG2,
    DEBUG3,
    DEBUG4, // 最高 DEBUG4
};

// 基础函数声明（必须先声明！）
inline void debug_print(std::ostream& os) {
    // 空实现，用于终止递归
    os << std::endl;
}

// 模板函数
template<typename T, typename... Args>
inline void debug_print(std::ostream& os, T&& first, Args&&... args) {
    os << std::forward<T>(first); // 打印第一个参数
    debug_print(os, std::forward<Args>(args)...); // 递归调用打印剩余参数
}

// 如果用户定义了 DEBUG_OUTPUT
#ifdef DEBUG_OUTPUT

// 将 DEBUG_OUTPUT 转换为 DebugLevel 枚举值
constexpr DebugLevel CURRENT_DEBUG_LEVEL = \
(DEBUG_OUTPUT == DebugLevel::INFO) ? DebugLevel::INFO : \
(DEBUG_OUTPUT == DebugLevel::DEBUG1) ? DebugLevel::DEBUG1 : \
(DEBUG_OUTPUT == DebugLevel::DEBUG2) ? DebugLevel::DEBUG2 : \
(DEBUG_OUTPUT == DebugLevel::DEBUG3) ? DebugLevel::DEBUG3 : \
(DEBUG_OUTPUT == DebugLevel::DEBUG4) ? DebugLevel::DEBUG4 : \
DebugLevel::INFO; // 默认值

// 信息级别
#define DEBUG_MSG_INFO(os, ...) \
    do { \
        if (static_cast<int>(DebugLevel::INFO) <= static_cast<int>(CURRENT_DEBUG_LEVEL)) { \
            os << "[INFO] "; \
            debug_print(os, __VA_ARGS__); \
        } \
    } while (0)

// 调试级别1
#define DEBUG_MSG_DEBUG1(os, ...) \
    do { \
        if (static_cast<int>(DebugLevel::DEBUG1) <= static_cast<int>(CURRENT_DEBUG_LEVEL)) { \
            os << "[DEBUG1] "; \
            debug_print(os, __VA_ARGS__); \
        } \
    } while (0)

// 调试级别2
#define DEBUG_MSG_DEBUG2(os, ...) \
    do { \
        if (static_cast<int>(DebugLevel::DEBUG2) <= static_cast<int>(CURRENT_DEBUG_LEVEL)) { \
            os << "[DEBUG2] "; \
            debug_print(os, __VA_ARGS__); \
        } \
    } while (0)

// 调试级别3
#define DEBUG_MSG_DEBUG3(os, ...) \
    do { \
        if (static_cast<int>(DebugLevel::DEBUG3) <= static_cast<int>(CURRENT_DEBUG_LEVEL)) { \
            os << "[DEBUG3] "; \
            debug_print(os, __VA_ARGS__); \
        } \
    } while (0)

// 调试级别4
#define DEBUG_MSG_DEBUG4(os, ...) \
    do { \
        if (static_cast<int>(DebugLevel::DEBUG4) <= static_cast<int>(CURRENT_DEBUG_LEVEL)) { \
            os << "[DEBUG4] "; \
            debug_print(os, __VA_ARGS__); \
        } \
    } while (0)

#else
// 如果未定义 DEBUG_OUTPUT，则不定义任何日志宏
#define DEBUG_MSG_INFO(os, ...)
#define DEBUG_MSG_DEBUG1(os, ...)
#define DEBUG_MSG_DEBUG2(os, ...)
#define DEBUG_MSG_DEBUG3(os, ...)
#define DEBUG_MSG_DEBUG4(os, ...)
#endif

#endif // DEBUGTOOL_H