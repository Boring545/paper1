#include"canfd_frame.h"
#include"frame_packing.h"
#ifndef PRIORITY_ALLOCATION_H
#define PRIORITY_ALLOCATION_H

namespace cfd::schedual::paper1{
    /*
        paper1 方法来自OPTIMAL PRIORITY ASSIGNMENT AND FEASIBILITY OF STATIC PRIORITY TASKS WITH ARBITRARY START TIMES
        可以为无关键期的帧集合分配理论最优的优先级，该方法将优先级分配与可调度性分析合二为一
        可分配优先级的帧集合即代表可调度，不能分配则不可调度
        主要方法为assign_priority
        注明：calc_remain_interf方法和论文不一致，论文提供的伪代码不能计算出论文例子同样的结果，修改为我的写法则可以。
    */


    int offset_trans(int target, int basis, int T);
    //确定每个任务的分析上界和下界
    bool find_interval(const std::vector<CanfdFrame*>& frame_set, std::vector<int>& lower_bound, std::vector<int>& upper_bound);
    class betaset {
    public:
        double C;   //执行时间
        int tr;  //release时间
        betaset(double C, int tr) :C(C), tr(tr) {}
    };
    //生成β集合
    bool create_beta(const std::vector<CanfdFrame*>& frame_set, const CanfdFrame& frame, int lower_bound, std::vector<betaset>& beta);
    //计算剩余干涉R_t_i：在时刻upper时 release的优先级 i 任务所面对的之前高优先级任务未完成的剩余执行时间,lower=upper-T_i+D_i
    double calc_remain_interf(const CanfdFrame& frame, int t, std::vector<betaset>& beta);
    //生成η集合
    bool  create_eta(const std::vector<CanfdFrame*>& frame_set, const CanfdFrame& frame, int t, double response_time, std::vector<betaset>& eta);
    //计算在t时刻优先级i 任务release后release的新任务产生的影响时间
    double calc_create_interf(const CanfdFrame& frame, const int t, const int response_time, const std::vector<betaset>& eta);


    // 检测数据帧集合的可行性，可行返回true
    bool feasibility_check(std::vector<CanfdFrame*>& frame_set, int taski, int pri, const std::vector<int>& lower, const std::vector<int>& upper);

    //为无关键期帧集合分配优先级，成功分配也表示可调度，返回true
    bool assign_priority(CanfdFrameMap& frame_map);

    bool assign_priority(PackingScheme& scheme);
}

#endif // !PRIORITY_ALLOCATION_H
