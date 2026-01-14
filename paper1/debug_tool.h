#ifndef DEBUGTOOL_H
#define DEBUGTOOL_H

#include <chrono>
#include <ctime>
#include <iostream>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>  // std::forward

inline std::string get_time_stamp() {
    auto now = std::chrono::system_clock::now();
    auto now_time_t = std::chrono::system_clock::to_time_t(now);

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

enum class DebugLevel {
    INFO,
    DEBUG1,
    DEBUG2,
    DEBUG3,
    DEBUG4,
};

inline void debug_print(std::ostream& os) { os << std::endl; }

template <typename T, typename... Args>
inline void debug_print(std::ostream& os, T&& first, Args&&... args) {
    os << std::forward<T>(first);
    debug_print(os, std::forward<Args>(args)...);
}

#ifdef DEBUG_OUTPUT

constexpr DebugLevel CURRENT_DEBUG_LEVEL = DEBUG_OUTPUT;

// 下面全部改成“单行宏”，避免多行宏续行符在 Windows 上被空白字符坑到。
#define DEBUG_MSG_INFO(os, ...) \
  do { \
    if (static_cast<int>(DebugLevel::INFO) <= static_cast<int>(CURRENT_DEBUG_LEVEL)) { \
      (os) << "[INFO] "; \
      debug_print((os), __VA_ARGS__); \
    } \
  } while (0)

#define DEBUG_MSG_DEBUG1(os, ...) \
  do { \
    if (static_cast<int>(DebugLevel::DEBUG1) <= static_cast<int>(CURRENT_DEBUG_LEVEL)) { \
      (os) << "[DEBUG1] "; \
      debug_print((os), __VA_ARGS__); \
    } \
  } while (0)

#define DEBUG_MSG_DEBUG2(os, ...) \
  do { \
    if (static_cast<int>(DebugLevel::DEBUG2) <= static_cast<int>(CURRENT_DEBUG_LEVEL)) { \
      (os) << "[DEBUG2] "; \
      debug_print((os), __VA_ARGS__); \
    } \
  } while (0)

#define DEBUG_MSG_DEBUG3(os, ...) \
  do { \
    if (static_cast<int>(DebugLevel::DEBUG3) <= static_cast<int>(CURRENT_DEBUG_LEVEL)) { \
      (os) << "[DEBUG3] "; \
      debug_print((os), __VA_ARGS__); \
    } \
  } while (0)

#define DEBUG_MSG_DEBUG4(os, ...) \
  do { \
    if (static_cast<int>(DebugLevel::DEBUG4) <= static_cast<int>(CURRENT_DEBUG_LEVEL)) { \
      (os) << "[DEBUG4] "; \
      debug_print((os), __VA_ARGS__); \
    } \
  } while (0)

#else

#define DEBUG_MSG_INFO(os, ...)   do {} while (0)
#define DEBUG_MSG_DEBUG1(os, ...) do {} while (0)
#define DEBUG_MSG_DEBUG2(os, ...) do {} while (0)
#define DEBUG_MSG_DEBUG3(os, ...) do {} while (0)
#define DEBUG_MSG_DEBUG4(os, ...) do {} while (0)

#endif

#endif  // DEBUGTOOL_H
