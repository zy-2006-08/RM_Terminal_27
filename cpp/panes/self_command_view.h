#pragma once

#include "domain.h"

#include <cstdint>
#include <optional>

namespace rm_terminal {

// 本车面板、复活框、兑换面板共用的一份本车状态,每帧从快照取。缺数据是 nullopt,和真实的 0 分开。
struct SelfCommandView {
    // false = 重装:42mm、没有性能体系、不能开能量机关(规则手册 5.5.2、5.6.3)。
    bool infantry = true;
    std::optional<std::uint32_t> stage;      // 0 未开始 1 准备 2 自检 3 倒计时 4 比赛中 5 结算
    std::optional<std::int32_t> remaining;   // 比赛中:本局剩余秒数
    std::optional<std::uint32_t> economy;
    std::optional<std::uint32_t> ammo;       // 剩余允许发弹量
    std::optional<std::uint32_t> fired;      // 累计已发弹量:判断兑换到没到账时,把等待期间打出去的补回来
    std::optional<std::uint32_t> hp, max_hp, level;
    std::optional<bool> out_of_combat, can_remote_heal, can_remote_ammo;
    // 站在能兑换允许发弹量的增益点上(服务器的 Buff 类型 6 还在时效内)。nullopt = 本次开机没收到过。
    std::optional<bool> at_exchange_point;
    // 本车口径的测速模块在不在线(步兵看 17mm,重装看 42mm),当作发射机构连没连上。
    std::optional<bool> shooter_online;
    // 己方后勤系数 λ(规则手册表 5-19),远程回血的比例要乘它。
    std::optional<double> logistics;
    std::optional<bool> dead;
    std::optional<std::uint32_t> revive_total, revive_current, revive_cost;
    std::optional<bool> can_free_revive, can_pay_revive;
    std::optional<std::uint32_t> rune_status, rune_arms;
    std::optional<double> rune_rings;  // 大能量机关正在激活时的平均环数(2.2.27)
    std::optional<std::uint32_t> shooter, chassis;
};

SelfCommandView self_command_view(const Snapshot& snapshot);

}  // namespace rm_terminal
