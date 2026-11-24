#include "purchase.h"

#include "next_step.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace rm_terminal {

namespace {

constexpr std::uint32_t kStageMatch = 4;
// CommonCommand.cmd_type(2.2.25)。
constexpr std::uint32_t kBuy17 = 1;
constexpr std::uint32_t kBuy42 = 2;
constexpr std::uint32_t kRemoteAmmo = 5;
constexpr std::uint32_t kRemoteHeal = 6;
// 规则手册表 5-8:非远程兑换 17mm 10 金/10 发,42mm 10 金/1 发。
constexpr std::uint32_t kSupplyPrice = 10;
constexpr double kRemoteHealFraction = 0.6;
// 远程兑换 6 秒后生效(规则手册 5.3.1、5.4.2),再多等 3 秒给 10Hz 的动态状态和网络留余量。
constexpr MonotonicMs kRemoteSettleMs = 9000;
// 补给区兑换当场到账。
constexpr MonotonicMs kSupplySettleMs = 3000;
constexpr MonotonicMs kShowResultMs = 5000;

QString match_block(const SelfCommandView& view, bool link_up) {
    if (!link_up) return QStringLiteral("断线，指令不会发送");
    if (!view.stage || *view.stage < kStageMatch) return QStringLiteral("比赛开始后才能兑换");
    if (*view.stage > kStageMatch) return QStringLiteral("本局已结束");
    if (view.dead.value_or(false)) return QStringLiteral("阵亡，复活后才能兑换");
    return QString();
}

// allowed 是服务器发的「现在能不能远程兑换」(RobotDynamicStatus,10Hz)。
QString remote_block(const SelfCommandView& view, std::optional<bool> allowed) {
    if (allowed.value_or(false)) return QString();
    if (view.out_of_combat == std::optional<bool>(false)) return QStringLiteral("交战中，脱战后才能远程兑换");
    return QStringLiteral("不满足远程兑换条件");
}

QString coin_block(const SelfCommandView& view, std::uint32_t cost) {
    if (!view.economy) return QStringLiteral("还没收到金币数");
    if (*view.economy < cost) return QStringLiteral("金币不足，无法兑换");
    return QString();
}

QString wrong_caliber(Caliber caliber) { return QStringLiteral("本车不用 %1 弹丸").arg(caliber_text(caliber)); }

QString shooter_offline(Caliber caliber) { return QStringLiteral("%1发射机构未连接").arg(caliber_text(caliber)); }

}  // namespace

Caliber own_caliber(const SelfCommandView& view) { return view.infantry ? Caliber::Small : Caliber::Large; }

QString caliber_text(Caliber caliber) {
    return caliber == Caliber::Small ? QStringLiteral("17mm") : QStringLiteral("42mm");
}

std::uint32_t remote_ammo_rounds(Caliber caliber) { return caliber == Caliber::Small ? 100 : 10; }

std::uint32_t supply_unit(Caliber caliber) { return caliber == Caliber::Small ? 10 : 1; }

std::uint32_t supply_cost(Caliber caliber, std::uint32_t rounds) {
    const std::uint32_t unit = supply_unit(caliber);
    return (rounds + unit - 1) / unit * kSupplyPrice;
}

std::array<std::uint32_t, 4> supply_steps(Caliber caliber) {
    if (caliber == Caliber::Small) return {10, 20, 50, 100};
    return {1, 2, 5, 10};
}

std::uint32_t supply_max_rounds(const SelfCommandView& view, Caliber caliber) {
    if (!view.economy) return 0;
    return *view.economy / kSupplyPrice * supply_unit(caliber);
}

std::optional<std::uint32_t> remote_heal_amount(const SelfCommandView& view) {
    if (!view.max_hp || !view.logistics) return std::nullopt;
    // 1e-9:0.6 不是精确的二进制小数,别让 200 × 0.6 落成 119。
    const auto amount =
        static_cast<std::uint32_t>(std::floor(*view.max_hp * kRemoteHealFraction * *view.logistics + 1e-9));
    if (!view.hp) return amount;
    const std::uint32_t room = *view.max_hp > *view.hp ? *view.max_hp - *view.hp : 0;
    return std::min(amount, room);
}

PurchaseOffer supply_offer(const SelfCommandView& view, bool link_up, Caliber caliber, std::uint32_t rounds) {
    PurchaseOffer offer;
    offer.kind = PurchaseKind::Supply;
    offer.caliber = caliber;
    offer.rounds = rounds;
    offer.cost = supply_cost(caliber, rounds);
    offer.cmd_type = caliber == Caliber::Small ? kBuy17 : kBuy42;
    offer.param = rounds;
    offer.question = QStringLiteral("此次兑换将消耗 %1 金币，请确认是否兑换").arg(offer.cost);
    offer.reason = [&]() -> QString {
        if (caliber != own_caliber(view)) return wrong_caliber(caliber);
        if (QString block = match_block(view, link_up); !block.isEmpty()) return block;
        if (!view.shooter_online.value_or(false)) return shooter_offline(caliber);
        // 车站在补给区、己方基地增益点或前哨站增益点上时,服务器发 Buff 类型 6(规则手册 5.4.2)。
        if (!view.at_exchange_point.value_or(false)) return QStringLiteral("未在兑换点");
        if (QString coins = coin_block(view, supply_cost(caliber, supply_unit(caliber))); !coins.isEmpty())
            return coins;
        if (rounds == 0) return QStringLiteral("先选兑换数量");
        if (rounds % supply_unit(caliber) != 0)
            return QStringLiteral("%1 要按 %2 发的整数倍兑换").arg(caliber_text(caliber)).arg(supply_unit(caliber));
        return coin_block(view, offer.cost);
    }();
    offer.available = offer.reason.isEmpty();
    return offer;
}

PurchaseOffer remote_ammo_offer(const SelfCommandView& view, bool link_up, Caliber caliber) {
    PurchaseOffer offer;
    offer.kind = PurchaseKind::RemoteAmmo;
    offer.caliber = caliber;
    offer.rounds = remote_ammo_rounds(caliber);
    offer.cost = kRemoteAmmoPrice;
    offer.cmd_type = kRemoteAmmo;
    // 协议没写远程补弹的 param 是什么。补给区兑换填的是发数,这里也填一份的发数,要连真服务器确认。
    offer.param = offer.rounds;
    offer.question = QStringLiteral("是否消耗 %1 金币，兑换 %2 发 %3 弹丸？")
                         .arg(offer.cost)
                         .arg(offer.rounds)
                         .arg(caliber_text(caliber));
    offer.reason = [&]() -> QString {
        if (caliber != own_caliber(view)) return wrong_caliber(caliber);
        if (QString block = match_block(view, link_up); !block.isEmpty()) return block;
        if (!view.can_remote_ammo.value_or(false) && view.shooter_online == std::optional<bool>(false))
            return shooter_offline(caliber);
        if (QString block = remote_block(view, view.can_remote_ammo); !block.isEmpty()) return block;
        return coin_block(view, offer.cost);
    }();
    offer.available = offer.reason.isEmpty();
    return offer;
}

PurchaseOffer remote_heal_offer(const SelfCommandView& view, bool link_up) {
    PurchaseOffer offer;
    offer.kind = PurchaseKind::RemoteHeal;
    offer.caliber = own_caliber(view);
    offer.cmd_type = kRemoteHeal;
    offer.cost = view.remaining ? remote_heal_cost(*view.remaining) : 0;
    offer.heal = remote_heal_amount(view).value_or(0);
    offer.question = offer.heal > 0
                         ? QStringLiteral("是否消耗 %1 金币，兑换回血（回 %2 血）？").arg(offer.cost).arg(offer.heal)
                         : QStringLiteral("是否消耗 %1 金币，兑换回血？").arg(offer.cost);
    offer.reason = [&]() -> QString {
        if (QString block = match_block(view, link_up); !block.isEmpty()) return block;
        if (QString block = remote_block(view, view.can_remote_heal); !block.isEmpty()) return block;
        if (view.hp && view.max_hp && *view.hp >= *view.max_hp) return QStringLiteral("满血，不用回血");
        if (!view.remaining) return QStringLiteral("还没收到比赛时间，算不出价钱");
        return coin_block(view, offer.cost);
    }();
    offer.available = offer.reason.isEmpty();
    return offer;
}

PurchaseOffer purchase_offer(PurchaseKind kind, Caliber caliber, std::uint32_t rounds, const SelfCommandView& view,
                             bool link_up) {
    switch (kind) {
        case PurchaseKind::Supply: return supply_offer(view, link_up, caliber, rounds);
        case PurchaseKind::RemoteAmmo: return remote_ammo_offer(view, link_up, caliber);
        case PurchaseKind::RemoteHeal: return remote_heal_offer(view, link_up);
    }
    return supply_offer(view, link_up, caliber, rounds);
}

QString purchase_name(PurchaseKind kind) {
    switch (kind) {
        case PurchaseKind::Supply: return QStringLiteral("兑换弹丸");
        case PurchaseKind::RemoteAmmo: return QStringLiteral("远程补弹");
        case PurchaseKind::RemoteHeal: return QStringLiteral("远程回血");
    }
    return QString();
}

void PurchaseWatch::sent(const PurchaseOffer& offer, const SelfCommandView& view, MonotonicMs now) {
    state_ = State::Waiting;
    kind_ = offer.kind;
    rounds_ = offer.rounds;
    heal_ = offer.heal;
    max_hp_ = view.max_hp.value_or(0);
    ammo_before_ = view.ammo;
    fired_before_ = view.fired.value_or(0);
    lowest_hp_ = view.hp.value_or(std::numeric_limits<std::uint32_t>::max());
    gained_ = 0;
    sent_at_ = now;
    settled_at_ = 0;
}

void PurchaseWatch::update(const SelfCommandView& view, MonotonicMs now) {
    if (state_ != State::Waiting) return;
    if (kind_ == PurchaseKind::RemoteHeal) {
        // 6 秒内阵亡,这次回血作废、金币不退(规则手册 5.3.1)。
        if (view.dead.value_or(false)) {
            settle(State::Voided, now);
            return;
        }
        if (view.hp) {
            lowest_hp_ = std::min(lowest_hp_, *view.hp);
            gained_ = std::max(gained_, *view.hp - lowest_hp_);
        }
        // 等的时候又掉了血就从最低点算;λ 这 6 秒里可能变了,涨到预计的一半就算到账。
        const std::uint32_t room = max_hp_ > lowest_hp_ ? max_hp_ - lowest_hp_ : 0;
        const std::uint32_t expected = heal_ > 0 ? std::min(heal_, room) : room;
        if (gained_ > 0 && gained_ * 2 >= expected) {
            settle(State::Arrived, now);
            return;
        }
    } else if (view.ammo) {
        if (!ammo_before_) {
            ammo_before_ = view.ammo;
            fired_before_ = view.fired.value_or(0);
        }
        const std::uint32_t fired = view.fired.value_or(fired_before_);
        // 换局时累计已发弹量清零,那时不往回补。
        const std::uint32_t shot = fired >= fired_before_ ? fired - fired_before_ : 0;
        const std::uint64_t have = static_cast<std::uint64_t>(*view.ammo) + shot;
        gained_ = have > *ammo_before_ ? static_cast<std::uint32_t>(have - *ammo_before_) : 0;
        if (gained_ >= rounds_) {
            settle(State::Arrived, now);
            return;
        }
    }
    const MonotonicMs limit = kind_ == PurchaseKind::Supply ? kSupplySettleMs : kRemoteSettleMs;
    if (now - sent_at_ >= limit) settle(State::Missing, now);
}

void PurchaseWatch::settle(State state, MonotonicMs now) {
    state_ = state;
    settled_at_ = now;
}

std::optional<PurchaseKind> PurchaseWatch::kind() const {
    if (state_ == State::Idle) return std::nullopt;
    return kind_;
}

QString PurchaseWatch::text(MonotonicMs now) const {
    const QString name = purchase_name(kind_);
    if (state_ != State::Idle && state_ != State::Waiting && now - settled_at_ >= kShowResultMs) return QString();
    switch (state_) {
        case State::Idle: return QString();
        case State::Waiting:
            if (kind_ == PurchaseKind::Supply)
                return QStringLiteral("%1 · 已兑换 %2 发，等待到账").arg(name).arg(rounds_);
            return QStringLiteral("%1 · 已兑换，6 秒后生效").arg(name);
        case State::Arrived:
            if (kind_ == PurchaseKind::RemoteHeal) return QStringLiteral("%1 · 到账 +%2 血").arg(name).arg(gained_);
            return QStringLiteral("%1 · 到账 +%2 发").arg(name).arg(gained_);
        case State::Missing: return QStringLiteral("%1 · 没到账").arg(name);
        case State::Voided: return QStringLiteral("%1 · 6 秒内阵亡，这次回血作废，金币不退").arg(name);
    }
    return QString();
}

}  // namespace rm_terminal
