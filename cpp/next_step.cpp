#include "next_step.h"

#include "presentation.h"

#include <algorithm>
#include <cmath>

namespace rm_terminal {

namespace {

// 阵地状态:0 未控 1 红临时 2 红完全 3 蓝临时 4 蓝完全(proto 注释)。
constexpr std::uint32_t kRedTemp = 1, kRedFull = 2, kBlueTemp = 3, kBlueFull = 4;
constexpr std::uint32_t kStageMatch = 4;
// 本机血量低于这个比例才推荐回血或撤出。
constexpr double kLowSelfRatio = 0.3;
// 敌方血量不超过四分之一算残血,和花名册的残血提示同一条线。
constexpr double kLowEnemyRatio = 0.25;
// 弹少的线:17mm 不到 50 发,42mm 不到 5 发。一次兑换 17mm 50 发 50 金,42mm 5 发 50 金。
constexpr std::uint32_t kLowAmmo17 = 50, kLowAmmo42 = 5;
constexpr std::uint32_t kBuy17 = 50, kBuy42 = 5, kBuyCost = 50;
// 能量机关开放后多久之内算「刚开放」。
constexpr std::int32_t kRuneFreshSec = 45;
// 基地掉血后多久之内算「正在被打」。
constexpr MonotonicMs kBaseHitHoldMs = 5000;

template <class T>
std::optional<T> fresh(const Field<T>& field) {
    if (field.quality != Quality::Valid || field.freshness != Freshness::Fresh || !field.value) return std::nullopt;
    return field.value;
}

QString faction_word(std::uint32_t faction) { return faction == kBlue ? QStringLiteral("蓝方") : QStringLiteral("红方"); }

QString robot_name(std::uint32_t id) {
    return QStringLiteral("%1 号%2").arg(display_robot_number(id)).arg(robot_class_name(id));
}

struct Zone {
    QString name;
    std::uint32_t side;  // 这块阵地在谁家:红方前哨区在红方
    std::optional<std::uint32_t> state;
};

std::vector<Zone> zones_of(const GameFields<Field>& game) {
    return {{QStringLiteral("红方前哨区"), kRed, fresh(game.red_outpost_zone)},
            {QStringLiteral("蓝方前哨区"), kBlue, fresh(game.blue_outpost_zone)},
            {QStringLiteral("红方基地区"), kRed, fresh(game.red_base_zone)},
            {QStringLiteral("蓝方基地区"), kBlue, fresh(game.blue_base_zone)}};
}

std::optional<std::pair<double, double>> position_of(const Snapshot& snapshot, std::uint32_t id) {
    for (const MapRobot& robot : snapshot.map_robots) {
        if (robot.id.value != id) continue;
        const auto x = fresh(robot.position.x);
        const auto y = fresh(robot.position.y);
        if (x && y) return std::make_pair(*x, *y);
    }
    return std::nullopt;
}

int rule_rank(NextRule rule) { return static_cast<int>(rule); }

bool same_card(const NextCard& a, const NextCard& b) { return a.rule == b.rule && a.enemy_id == b.enemy_id; }

}  // namespace

std::vector<NextGroup> default_next_order() {
    return {NextGroup::Survive, NextGroup::Defend, NextGroup::Opportunity, NextGroup::Supply, NextGroup::Score};
}

QString next_group_name(NextGroup group) {
    switch (group) {
    case NextGroup::Survive: return QStringLiteral("保命");
    case NextGroup::Defend: return QStringLiteral("守家");
    case NextGroup::Opportunity: return QStringLiteral("抓机会");
    case NextGroup::Supply: return QStringLiteral("补给");
    case NextGroup::Score: return QStringLiteral("抢分");
    }
    return QString();
}

QString next_group_hint(NextGroup group) {
    switch (group) {
    case NextGroup::Survive: return QStringLiteral("阵亡先复活，血少先回血");
    case NextGroup::Defend: return QStringLiteral("基地被打、堡垒被偷、阵地快丢");
    case NextGroup::Opportunity: return QStringLiteral("能量机关开放、附近有残血敌人");
    case NextGroup::Supply: return QStringLiteral("弹少补弹");
    case NextGroup::Score: return QStringLiteral("控制度落后，去占区");
    }
    return QString();
}

QString next_group_key(NextGroup group) {
    switch (group) {
    case NextGroup::Survive: return QStringLiteral("survive");
    case NextGroup::Defend: return QStringLiteral("defend");
    case NextGroup::Opportunity: return QStringLiteral("opportunity");
    case NextGroup::Supply: return QStringLiteral("supply");
    case NextGroup::Score: return QStringLiteral("score");
    }
    return QString();
}

std::optional<NextGroup> next_group_from_key(const QString& key) {
    for (const NextGroup group : default_next_order())
        if (next_group_key(group) == key) return group;
    return std::nullopt;
}

std::uint32_t remote_heal_cost(std::int32_t remaining_sec) {
    const double elapsed = 420.0 - std::clamp(remaining_sec, 0, 420);
    return 50u + static_cast<std::uint32_t>(std::ceil(elapsed / 60.0 * 20.0 - 1e-9));
}

std::vector<NextCard> next_candidates(const Snapshot& snapshot, bool base_hit) {
    std::vector<NextCard> cards;
    const GameFields<Field>& game = snapshot.game;
    if (fresh(game.current_stage) != kStageMatch || !snapshot.self) return cards;
    const std::uint32_t self_id = snapshot.self->value;
    const std::uint32_t ours = faction_of(self_id);
    const std::uint32_t theirs = opponent_of(ours);
    const std::uint32_t number = self_id % 100;
    const bool ground = number == 1 || number == 3 || number == 4;
    const bool infantry = number == 3 || number == 4;
    const std::optional<std::uint32_t> economy = fresh(game.economy);
    const std::optional<std::int32_t> remaining = fresh(game.stage_countdown_sec);

    const RobotState* self = nullptr;
    if (const auto found = snapshot.robots.find(*snapshot.self); found != snapshot.robots.end()) self = &found->second;
    const bool dead = self && fresh(self->respawn.pending).value_or(false);

    // 保命:阵亡先复活,血少先回血。
    if (ground && self && dead) {
        NextCard card;
        card.group = NextGroup::Survive;
        card.category = QStringLiteral("复活");
        card.urgent = true;
        const auto total = fresh(self->respawn.total_progress);
        const auto current = fresh(self->respawn.current_progress);
        const QString bar = QStringLiteral("读条 %1/%2")
                                .arg(current ? QString::number(*current) : QStringLiteral("--"),
                                     total ? QString::number(*total) : QStringLiteral("--"));
        const auto cost = fresh(self->respawn.gold_cost);
        if (fresh(self->respawn.can_free).value_or(false)) {
            card.rule = NextRule::FreeRevive;
            card.title = QStringLiteral("按 Y 免费复活");
            card.reason = QStringLiteral("读条已满，不用花金币");
            card.speech = QStringLiteral("按Y免费复活");
        } else if (fresh(self->respawn.can_pay).value_or(false) && cost && economy && *economy >= *cost) {
            card.rule = NextRule::PaidRevive;
            card.title = QStringLiteral("花 %1 金立即复活").arg(*cost);
            card.reason = bar + QStringLiteral("，不想等就花金币");
            card.speech = QStringLiteral("可以花金币复活");
        } else {
            card.rule = NextRule::WaitRevive;
            card.title = QStringLiteral("等读条复活");
            card.reason = bar + QStringLiteral("，满了按 Y");
            card.speech = QStringLiteral("等读条复活");
        }
        cards.push_back(card);
    }
    if (ground && self && !dead) {
        const auto hp = fresh(self->dynamic.current_hp);
        const auto max_hp = fresh(self->statics.max_hp);
        if (hp && max_hp && *max_hp > 0 && *hp > 0 && static_cast<double>(*hp) / *max_hp < kLowSelfRatio) {
            const int percent = static_cast<int>(std::lround(100.0 * *hp / *max_hp));
            const std::uint32_t cost = remaining ? remote_heal_cost(*remaining) : 0;
            NextCard card;
            card.group = NextGroup::Survive;
            if (fresh(self->dynamic.can_remote_heal).value_or(false) && remaining && economy && *economy >= cost) {
                card.rule = NextRule::RemoteHeal;
                card.category = QStringLiteral("回血");
                card.title = QStringLiteral("先远程回血");
                card.reason = QStringLiteral("本机只剩 %1%，花 %2 金").arg(percent).arg(cost);
                card.speech = QStringLiteral("血量低，先远程回血");
                cards.push_back(card);
            } else if (!fresh(self->dynamic.out_of_combat).value_or(true)) {
                card.rule = NextRule::LeaveCombat;
                card.category = QStringLiteral("保命");
                card.title = QStringLiteral("先撤出交战");
                card.reason = QStringLiteral("本机只剩 %1%，脱战后才能远程回血").arg(percent);
                card.speech = QStringLiteral("血量低，先撤出交战");
                cards.push_back(card);
            }
        }
    }

    // 守家:堡垒被偷、基地被打、我方阵地被敌方临时控制。
    const auto fortress = fresh(ours == kRed ? game.red_fortress_activation_sec : game.blue_fortress_activation_sec);
    if (fortress && *fortress > 0) {
        NextCard card;
        card.rule = NextRule::DefendFortress;
        card.group = NextGroup::Defend;
        card.category = QStringLiteral("防守");
        card.title = QStringLiteral("回防我方堡垒");
        card.reason = QStringLiteral("敌方正在激活：%1 秒 / 10 秒").arg(*fortress);
        card.speech = QStringLiteral("敌方在偷堡垒，回防");
        card.urgent = true;
        cards.push_back(card);
    }
    if (base_hit) {
        const auto base_hp = fresh(ours == kRed ? game.red_base_hp : game.blue_base_hp);
        NextCard card;
        card.rule = NextRule::DefendBase;
        card.group = NextGroup::Defend;
        card.category = QStringLiteral("防守");
        card.title = QStringLiteral("回防我方基地");
        card.reason = base_hp ? QStringLiteral("基地正在掉血，还剩 %1").arg(*base_hp) : QStringLiteral("基地正在掉血");
        card.speech = QStringLiteral("基地被攻击，回防");
        card.urgent = true;
        cards.push_back(card);
    }
    const std::uint32_t enemy_temp = theirs == kRed ? kRedTemp : kBlueTemp;
    const std::uint32_t our_temp = ours == kRed ? kRedTemp : kBlueTemp;
    const std::uint32_t our_full = ours == kRed ? kRedFull : kBlueFull;
    const std::vector<Zone> zones = zones_of(game);
    for (const Zone& zone : zones) {
        if (zone.side != ours || zone.state != enemy_temp) continue;
        NextCard card;
        card.rule = NextRule::DefendZone;
        card.group = NextGroup::Defend;
        card.category = QStringLiteral("阵地");
        card.title = QStringLiteral("回防%1").arg(zone.name);
        card.reason = QStringLiteral("敌方正在临时控制");
        card.speech = QStringLiteral("阵地被敌方临时控制，回防");
        cards.push_back(card);
        break;
    }

    // 抓机会:残血敌人(雷达看得到才推荐)、能量机关刚开放、我方临时控制的阵地。
    if (!dead) {
        std::optional<NextCard> focus;
        double best_ratio = 2.0;
        const auto self_at = position_of(snapshot, self_id);
        for (const RobotHealth& robot : snapshot.robot_health) {
            if (robot.faction != theirs) continue;
            const auto hp = fresh(robot.current_hp);
            const auto max_hp = fresh(robot.max_hp);
            if (!hp || !max_hp || *max_hp == 0 || *hp == 0) continue;
            const double ratio = static_cast<double>(*hp) / *max_hp;
            if (ratio > kLowEnemyRatio || ratio >= best_ratio) continue;
            const auto at = position_of(snapshot, robot.id.value);
            if (!at) continue;
            best_ratio = ratio;
            NextCard card;
            card.rule = NextRule::FocusEnemy;
            card.group = NextGroup::Opportunity;
            card.category = QStringLiteral("进攻");
            card.title = QStringLiteral("集火敌方 %1").arg(robot_name(robot.id.value));
            card.reason = QStringLiteral("只剩 %1 血").arg(*hp);
            if (self_at) {
                const double metres = std::hypot(at->first - self_at->first, at->second - self_at->second);
                card.reason += QStringLiteral("，离你约 %1 米").arg(std::max(1L, std::lround(metres)));
            }
            card.enemy_id = robot.id.value;
            card.speech = QStringLiteral("集火残血敌人");
            focus = card;
        }
        if (focus) cards.push_back(*focus);
    }
    if (infantry && !dead && fresh(game.rune_status) == 1u) {
        if (const auto elapsed = fresh(game.stage_elapsed_sec)) {
            struct Window { std::int32_t at; bool big; };
            static const Window windows[] = {{0, false}, {90, false}, {180, true}, {255, true}, {330, true}};
            for (const Window& window : windows) {
                if (*elapsed < window.at || *elapsed - window.at > kRuneFreshSec) continue;
                NextCard card;
                card.rule = NextRule::OpenRune;
                card.group = NextGroup::Opportunity;
                card.category = QStringLiteral("增益");
                card.title = window.big ? QStringLiteral("开大能量机关") : QStringLiteral("开小能量机关");
                card.reason = QStringLiteral("刚开放，长按 F 1 秒开启");
                card.rune_button = true;
                card.speech = QStringLiteral("去开能量机关");
                cards.push_back(card);
                break;
            }
        }
    }
    for (const Zone& zone : zones) {
        if (zone.state != our_temp) continue;
        NextCard card;
        card.rule = NextRule::HoldZone;
        card.group = NextGroup::Opportunity;
        card.category = QStringLiteral("阵地");
        card.title = QStringLiteral("守住%1").arg(zone.name);
        card.reason = QStringLiteral("临时控制，5 秒没人激活就丢");
        card.speech = QStringLiteral("守住临时阵地");
        cards.push_back(card);
        break;
    }

    // 补给:弹少补弹。阵亡时也提醒,复活后第一件事做它。
    if (ground && self) {
        const auto ammo = fresh(self->dynamic.remaining_ammo);
        const std::uint32_t low = number == 1 ? kLowAmmo42 : kLowAmmo17;
        const std::uint32_t pack = number == 1 ? kBuy42 : kBuy17;
        if (ammo && *ammo < low && economy && *economy >= kBuyCost) {
            NextCard card;
            card.rule = NextRule::BuyAmmo;
            card.group = NextGroup::Supply;
            card.category = QStringLiteral("补给");
            card.title = dead ? QStringLiteral("复活后先补弹") : QStringLiteral("兑换弹丸");
            card.reason = QStringLiteral("只剩 %1 发，%2 金换 %3 发").arg(*ammo).arg(kBuyCost).arg(pack);
            card.speech = QStringLiteral("弹少，先补弹");
            cards.push_back(card);
        }
    }

    // 抢分:控制度落后,去占一块还不是我方完全控制的阵地,先占自家的。
    const auto red_points = fresh(game.red_control_points);
    const auto blue_points = fresh(game.blue_control_points);
    if (red_points && blue_points) {
        const long our_points = static_cast<long>(ours == kRed ? *red_points : *blue_points);
        const long their_points = static_cast<long>(ours == kRed ? *blue_points : *red_points);
        if (our_points < their_points) {
            const Zone* target = nullptr;
            for (const bool home : {true, false})
                for (const Zone& zone : zones)
                    if (!target && (zone.side == ours) == home && zone.state && *zone.state != our_full &&
                        *zone.state != our_temp)
                        target = &zone;
            if (target) {
                NextCard card;
                card.rule = NextRule::TakeZone;
                card.group = NextGroup::Score;
                card.category = QStringLiteral("抢分");
                card.title = QStringLiteral("去占%1").arg(target->name);
                card.reason = QStringLiteral("控制度落后 %1 分").arg(their_points - our_points);
                card.speech = QStringLiteral("控制度落后，去占阵地");
                cards.push_back(card);
            }
        }
    }
    return cards;
}

std::vector<NextCard> rank_next(std::vector<NextCard> cards, const std::vector<NextGroup>& order, std::size_t count) {
    const auto group_rank = [&order](NextGroup group) {
        const auto it = std::find(order.begin(), order.end(), group);
        return it == order.end() ? static_cast<int>(order.size()) : static_cast<int>(it - order.begin());
    };
    std::stable_sort(cards.begin(), cards.end(), [&](const NextCard& a, const NextCard& b) {
        const int ga = group_rank(a.group), gb = group_rank(b.group);
        if (ga != gb) return ga < gb;
        return rule_rank(a.rule) < rule_rank(b.rule);
    });
    if (cards.size() > count) cards.resize(count);
    return cards;
}

std::vector<NextCard> NextStepBoard::update(const Snapshot& snapshot, MonotonicMs now,
                                            const std::vector<NextGroup>& order, std::size_t count,
                                            MonotonicMs dwell_ms) {
    const std::uint32_t ours = snapshot.self ? faction_of(snapshot.self->value) : kRed;
    const auto base_hp = fresh(ours == kRed ? snapshot.game.red_base_hp : snapshot.game.blue_base_hp);
    if (base_hp) {
        if (last_base_hp_ && *base_hp < *last_base_hp_) base_hit_until_ = now + kBaseHitHoldMs;
        last_base_hp_ = base_hp;
    }
    const std::vector<NextCard> all = next_candidates(snapshot, now < base_hit_until_);
    const std::vector<NextCard> ranked = rank_next(all, order, count);

    const auto present = [&all](const NextCard& card) {
        return std::any_of(all.begin(), all.end(), [&card](const NextCard& c) { return same_card(c, card); });
    };
    const auto rank_of = [&order](NextGroup group) {
        const auto it = std::find(order.begin(), order.end(), group);
        return static_cast<int>(it - order.begin());
    };
    const bool stale = std::any_of(shown_.begin(), shown_.end(), [&](const NextCard& card) { return !present(card); });
    const bool preempt = !ranked.empty() && !shown_.empty() && rank_of(ranked.front().group) < rank_of(shown_.front().group);
    const bool same_set = ranked.size() == shown_.size() &&
                          std::equal(ranked.begin(), ranked.end(), shown_.begin(), same_card);
    if (same_set || (!stale && !preempt && !shown_.empty() && now - shown_at_ < dwell_ms && shown_.size() >= std::min(count, ranked.size()))) {
        // 同一件事的数字照常更新。
        for (NextCard& card : shown_)
            for (const NextCard& latest : all)
                if (same_card(card, latest)) card = latest;
        return shown_;
    }
    shown_ = ranked;
    shown_at_ = now;
    return shown_;
}

QStringList next_step_phrases() {
    return {QStringLiteral("按Y免费复活"),       QStringLiteral("可以花金币复活"),     QStringLiteral("等读条复活"),
            QStringLiteral("血量低，先远程回血"), QStringLiteral("血量低，先撤出交战"), QStringLiteral("敌方在偷堡垒，回防"),
            QStringLiteral("基地被攻击，回防"),   QStringLiteral("阵地被敌方临时控制，回防"),
            QStringLiteral("集火残血敌人"),       QStringLiteral("去开能量机关"),       QStringLiteral("守住临时阵地"),
            QStringLiteral("弹少，先补弹"),       QStringLiteral("控制度落后，去占阵地")};
}

}  // namespace rm_terminal
