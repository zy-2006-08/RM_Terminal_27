#pragma once

#include "self_command_view.h"

#include <QString>
#include <array>
#include <cstdint>
#include <optional>

namespace rm_terminal {

// 本车花金币的三种兑换。价格和数量按 2027 规则手册表 5-7、5-8、5-9,条件按 5.3.1、5.4.2;
// 服务器还会再查一遍,终端先挡住明知买不了的,并写明原因。
enum class PurchaseKind { Supply, RemoteAmmo, RemoteHeal };
// 步兵用 17mm,重装用 42mm。官方选手端 O 键是 17mm 补给面板,I 键是 42mm。
enum class Caliber { Small, Large };

struct PurchaseOffer {
    PurchaseKind kind = PurchaseKind::Supply;
    Caliber caliber = Caliber::Small;
    bool available = false;
    QString reason;              // 买不了时给操作手看的原因;能买时为空
    std::uint32_t rounds = 0;    // 补给区兑换、远程补弹:发数
    std::uint32_t cost = 0;      // 金币;远程回血不知道比赛剩余时间时为 0
    std::uint32_t heal = 0;      // 远程回血:预计回多少血,算不出时为 0
    std::uint32_t cmd_type = 0;  // CommonCommand(2.2.25)
    std::uint32_t param = 0;
    QString question;            // 确认框里的那句话
};

constexpr std::uint32_t kRemoteAmmoPrice = 150;
Caliber own_caliber(const SelfCommandView& view);
QString caliber_text(Caliber caliber);
std::uint32_t remote_ammo_rounds(Caliber caliber);
std::uint32_t supply_unit(Caliber caliber);
std::uint32_t supply_cost(Caliber caliber, std::uint32_t rounds);
// 补弹面板的加减档位:17mm 照官方 10/20/50/100;42mm 一发起买,缩成 1/2/5/10。
std::array<std::uint32_t, 4> supply_steps(Caliber caliber);
// 现在的钱最多能换多少发。全队兑换上限协议里没有,算不进来。
std::uint32_t supply_max_rounds(const SelfCommandView& view, Caliber caliber);
// 远程回血预计回多少:上限血量的 60% 乘后勤系数,加完不超过上限。缺上限血量或后勤系数时为空。
std::optional<std::uint32_t> remote_heal_amount(const SelfCommandView& view);

PurchaseOffer supply_offer(const SelfCommandView& view, bool link_up, Caliber caliber, std::uint32_t rounds);
PurchaseOffer remote_ammo_offer(const SelfCommandView& view, bool link_up, Caliber caliber);
PurchaseOffer remote_heal_offer(const SelfCommandView& view, bool link_up);
// 按种类重新算一遍(确认框开着时每帧刷新价钱和条件)。rounds 只对补给区兑换有用。
PurchaseOffer purchase_offer(PurchaseKind kind, Caliber caliber, std::uint32_t rounds,
                             const SelfCommandView& view, bool link_up);
QString purchase_name(PurchaseKind kind);

// 兑换到没到账。协议不回「买成了没有」:补给区兑换看允许发弹量当场涨没涨,远程兑换 6 秒后
// 看弹量或血量涨没涨。等待期间打出去的弹按累计已发弹量补回来,边打边等也不会误判。
class PurchaseWatch {
public:
    enum class State { Idle, Waiting, Arrived, Missing, Voided };

    void sent(const PurchaseOffer& offer, const SelfCommandView& view, MonotonicMs now);
    void update(const SelfCommandView& view, MonotonicMs now);
    State state() const { return state_; }
    std::optional<PurchaseKind> kind() const;
    // 「远程补弹 · 已兑换，6 秒后生效」;没买过或结果已经显示够久时为空。
    QString text(MonotonicMs now) const;
    bool problem() const { return state_ == State::Missing || state_ == State::Voided; }

private:
    void settle(State state, MonotonicMs now);

    State state_ = State::Idle;
    PurchaseKind kind_ = PurchaseKind::Supply;
    std::uint32_t rounds_ = 0;
    std::uint32_t heal_ = 0;
    std::uint32_t max_hp_ = 0;
    std::optional<std::uint32_t> ammo_before_;
    std::uint32_t fired_before_ = 0;
    std::uint32_t lowest_hp_ = 0;
    std::uint32_t gained_ = 0;
    MonotonicMs sent_at_ = 0;
    MonotonicMs settled_at_ = 0;
};

}  // namespace rm_terminal
