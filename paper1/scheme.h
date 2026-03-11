#ifndef PACKINGSCHEME_H
#define PACKINGSCHEME_H

#include <unordered_set>
#include"config.h"
#include "canfd_frame.h"

namespace cfd {
constexpr double LIMIT_BANDWIDTH = 0.9;  // 带宽利用率上限
class PackingScheme;

// 一个打包方案，包含一组装载了全部消息的CANFD帧
class PackingScheme {
 private:
  using PeriodFrameeMap =
      std::map<int, std::vector<int>>;  // 相同ECUpair下，不同周期对应的frame索引分表,通过frame_set[索引]选取frame
  using EcuToFrameMap =
      std::unordered_map<EcuPair, PeriodFrameeMap, EcuPairHash>;  // 根据【ECUpair，period】选择PeriodFrameeMap分表

  using PeriodMessageMap =
      std::map<int,
               std::vector<size_t>>;  // 相同ECUpair下，不同周期对应的message索引分表,通过message_set[索引]选取message
  using EcuPeriodMessageMap =
      std::unordered_map<EcuPair, PeriodMessageMap, EcuPairHash>;  // 根据【ECUpair，period】选择PeriodMessageMap分表

  std::unordered_set<int>
      free_ids;  // 可复用的ID池,给帧标号用,加入新帧时从池里选择id，帧清空时id返回池，池为空则分配frame_map.size()作为id

  // 根据消息集合生成初始化的帧集合，作为生成初始打包方案
  bool init_frames();

 public:
  MessageVec message_set;  // 信号集合

  CanfdFrameMap frame_map;  // key为 frame的id，id应该唯一，分配id时注意使用get_free_id()获取唯一的id

  // 获取一个可用帧id,取必用！
  int get_free_id();
  // 回收帧的id
  void recover_id(int id);

  // 重新初始化，生成初始打包方案
  bool re_init_frames();

  // 增加一个新帧，其只包含一个基准msg，返回新帧id
  int new_frame(Message& msg);

  // 增加一个新帧，其只包含一个基准msg，返回新帧id
  int new_frame(int _period, int _deadline, const EcuPair& _ecu_pair, int _offset);

  // 计算带宽利用率
  double calc_bandwidth_utilization() const;
  void print_frame() { cfd::utils::print_frame(this->frame_map); }

  PackingScheme(bool backup = true, MessageInfoVec& vec = cfd::MESSAGE_INFO_VEC) {
    int original_size = vec.size();

    for (size_t i = 0; i < vec.size(); i++) {
      message_set.emplace_back(i);
    }
    init_frames();
  }

  // 用于从fmap表示的打包方案初始化一个PackingScheme类
  PackingScheme(const CanfdFrameMap& fmap);
  PackingScheme(const PackingScheme& other);
  PackingScheme(PackingScheme&& other) noexcept;

  PackingScheme& operator=(const PackingScheme& other) {
    this->message_set = other.message_set;
    this->frame_map = other.frame_map;
    this->free_ids = other.free_ids;
    return *this;
  }
};
}  // namespace cfd

namespace cfd::packing {
// 对初始化后的scheme进行打包
double frame_pack(PackingScheme& scheme, PACK_METHOD method);
}  // namespace cfd::packing

#endif  // !FRAMEPACKING_H