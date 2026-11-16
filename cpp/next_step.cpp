#include "next_step.h"

#include "match_event.h"
#include "presentation.h"

#include <algorithm>
#include <cmath>

namespace rm_terminal {

namespace {

// 阵地状态:0 未控 1 红临时 2 红完全 3 蓝临时 4 蓝完全(proto 注释)。
constexpr std::uint32_t kRedTemp = 1, kRedFull = 2, kBlueTemp = 3, kBlueFull = 4;
constexpr std::uint32_t kStageMatch = 4;
// 本机血量低于这个比例才推荐补血;敌方不超过这个比例算残血。
constexpr double kLowSelfRatio = 0.25;
constexpr double kLowEnemyRatio = 0.25;
// 弹少的线:17mm 不到 50 发,42mm 不到 5 发(都打不掉一座 1000 血的前哨站)。
constexpr std::uint32_t kLowAmmo17 = 50, kLowAmmo42 = 5;
// 规则手册表 5-7:兑换点上 10 金换 17mm 10 发、42mm 1 发;远程 150 金换 17mm 100 发、42mm 10 发。
constexpr std::uint32_t kRemoteAmmoPrice = 150;
// 能量机关开放后多久之内算「刚开放」。
constexpr std::int32_t kRuneFreshSec = 45;
// 基地掉血后多久之内算「正在被打」。
constexpr MonotonicMs kBaseHitHoldMs = 5000;
// 最后多少秒算收尾:这时候才提「现在结束是输是赢」,也才考虑远程回血。
constexpr std::int32_t kEndgameSec = 60;
// 打完一个目标最多被热量卡几次。17mm 一级 40 热、一发 10 点,一轮 4 发;4 轮够打掉 300 血的基地。
constexpr std::uint32_t kMaxHeatCycles = 4;
// 表 5-2:无增益时 17mm 一发 20 血,42mm 一发 200 血;5.1.3 单发热量 10 / 100。
constexpr std::uint32_t kDamage17 = 20, kDamage42 = 200;
constexpr std::uint32_t kHeat17 = 10, kHeat42 = 100;
// GlobalUnitStatus.base_status:0 有 100% 防御增益,2 护甲已展开。
constexpr std::uint32_t kBaseDefended = 0;
constexpr std::uint32_t kArmorDeployed = 2;

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

// 规则手册表 5-12:每一级第一次装成的收益。只推荐还没装成过的那一级,所以总是第一次。
QString first_assembly_gain(std::uint32_t level) {
    switch (level) {
    case 1: return QStringLiteral("升 5 级，每 10 秒 +40 金");
    case 2: return QStringLiteral("升 7 级，全队等级上限到 7");
    case 3: return QStringLiteral("解锁 42mm，全队 25% 防御");
    default: return QStringLiteral("全队 50% 防御，每 10 秒 +40 金");
    }
}

// 打掉 hp 要几发、会不会被热量拖太久。打完最多被热量卡 kMaxHeatCycles 轮才算「一时打得掉」。
struct KillPlan {
    std::uint32_t shots = 0;
    bool finishable = false;
};

KillPlan plan_for(std::uint32_t hp, bool heavy, std::optional<std::uint32_t> max_heat, double damage_scale) {
    const std::uint32_t damage = heavy ? kDamage42 : kDamage17;
    const std::uint32_t heat = heavy ? kHeat42 : kHeat17;
    const std::uint32_t cap = max_heat.value_or(heavy ? 300u : 40u);
    const std::uint32_t per_cycle = std::max(1u, cap / heat);
    const double per_shot = std::max(1.0, damage * damage_scale);
    KillPlan plan;
    plan.shots = static_cast<std::uint32_t>(std::ceil(hp / per_shot - 1e-9));
    plan.finishable = plan.shots <= per_cycle * kMaxHeatCycles;
    return plan;
}

bool same_card(const NextCard& a, const NextCard& b) { return a.rule == b.rule && a.enemy_id == b.enemy_id; }

int rule_rank(NextRule rule) { return static_cast<int>(rule); }

}  // namespace

std::uint32_t remote_heal_cost(std::int32_t remaining_sec) {
    const double elapsed = 420.0 - std::clamp(remaining_sec, 0, 420);
    return 50u + static_cast<std::uint32_t>(std::ceil(elapsed / 60.0 * 20.0 - 1e-9));
}

std::vector<NextCard> next_candidates(const Snapshot& snapshot, bool base_hit, const NextBuffs& buffs) {
    std::vector<NextCard> cards;
    const GameFields<Field>& game = snapshot.game;
    if (fresh(game.current_stage) != kStageMatch || !snapshot.self) return cards;
    const std::uint32_t self_id = snapshot.self->value;
    const std::uint32_t ours = faction_of(self_id);
    const std::uint32_t theirs = opponent_of(ours);
    const std::uint32_t number = self_id % 100;
    const bool heavy = number == 1;
    const bool infantry = number == 3 || number == 4;

    const RobotState* self = nullptr;
    if (const auto found = snapshot.robots.find(*snapshot.self); found != snapshot.robots.end()) self = &found->second;
    if (!self) return cards;

    const auto ammo = fresh(self->dynamic.remaining_ammo);
    const auto economy = fresh(game.economy);
    const auto remaining = fresh(game.stage_countdown_sec);
    const bool dead = fresh(self->respawn.pending).value_or(false);
    const bool mounted = heavy && fresh(self->modules.big_shooter).value_or(0) == 1;
    const bool shooter_ok = !heavy || snapshot.launcher_installed;
    // 虚弱时发射机构锁定(规则 5.3.2),但买弹不受影响。
    const bool can_shoot = shooter_ok && !buffs.self_weak;
    const auto max_heat = fresh(self->statics.max_heat);
    const std::uint32_t rounds = ammo.value_or(0);
    const double damage_scale =
        buffs.self_attack * (1.0 - std::clamp(buffs.enemy_structure_defense, 0, 99) / 100.0);

    const auto our_base_hp = fresh(ours == kRed ? game.red_base_hp : game.blue_base_hp);
    const auto their_base_hp = fresh(ours == kRed ? game.blue_base_hp : game.red_base_hp);
    const auto their_base_status = fresh(ours == kRed ? game.blue_base_status : game.red_base_status);
    const auto our_base_status = fresh(ours == kRed ? game.red_base_status : game.blue_base_status);
    // 0 = 有 100% 防御(规则 5.6.1),车打不动。
    const bool their_base_defended = their_base_status && *their_base_status == kBaseDefended;
    const bool our_base_defended = our_base_status && *our_base_status == kBaseDefended;

    // 1、2、15:敌方基地没防御时,打不打得掉。
    if (!dead && their_base_hp && *their_base_hp > 0 && their_base_status && !their_base_defended && can_shoot) {
        const KillPlan plan = plan_for(*their_base_hp, heavy, max_heat, damage_scale);
        if (plan.finishable && rounds >= plan.shots) {
            NextCard card;
            card.rule = NextRule::AttackBase;
            card.category = QStringLiteral("进攻");
            card.title = QStringLiteral("打敌方基地");
            card.reason = QStringLiteral("还剩 %1 血，约 %2 发").arg(*their_base_hp).arg(plan.shots);
            card.speech = QStringLiteral("打敌方基地");
            cards.push_back(card);
        } else if (plan.finishable) {
            NextCard card;
            card.rule = NextRule::AttackBaseAfterAmmo;
            card.category = QStringLiteral("进攻");
            card.title = QStringLiteral("先补弹，补完打基地");
            card.reason = QStringLiteral("敌方基地还剩 %1 血，现在 %2 发不够").arg(*their_base_hp).arg(rounds);
            card.speech = QStringLiteral("先补弹再打基地");
            cards.push_back(card);
        } else if (rounds > 0) {
            NextCard card;
            card.rule = NextRule::AttackBaseLongShot;
            card.category = QStringLiteral("进攻");
            card.title = QStringLiteral("打敌方基地");
            card.reason = QStringLiteral("还剩 %1 血，一时打不掉").arg(*their_base_hp);
            card.speech = QStringLiteral("打敌方基地");
            cards.push_back(card);
        }
    }

    // 3 守家:堡垒被偷。
    const auto fortress = fresh(ours == kRed ? game.red_fortress_activation_sec : game.blue_fortress_activation_sec);
    if (fortress && *fortress > 0) {
        NextCard card;
        card.rule = NextRule::DefendFortress;
        card.category = QStringLiteral("防守");
        card.title = QStringLiteral("回防我方堡垒");
        card.reason = QStringLiteral("敌方正在激活：%1 秒 / 10 秒").arg(*fortress);
        // 规则 5.6.4.9:在激活对方堡垒的车 100% 易伤,对方基地护甲展开后就没有了。
        if (our_base_status && *our_base_status != kArmorDeployed) card.reason += QStringLiteral("，它 100% 易伤");
        card.speech = QStringLiteral("敌方在偷堡垒，回防");
        card.urgent = true;
        cards.push_back(card);
    }
    // 4 基地在掉血,而且没有 100% 防御。有防御时掉血只可能是飞镖或判罚,回防没用。
    if (base_hit && our_base_status && !our_base_defended) {
        NextCard card;
        card.rule = NextRule::DefendBase;
        card.category = QStringLiteral("防守");
        card.title = QStringLiteral("回防我方基地");
        card.reason = our_base_hp ? QStringLiteral("基地正在掉血，还剩 %1").arg(*our_base_hp)
                                  : QStringLiteral("基地正在掉血");
        card.speech = QStringLiteral("基地被攻击，回防");
        card.urgent = true;
        cards.push_back(card);
    }
    // 5 血少补血。平时回补给区;只有最后 60 秒、金币够买弹又够远程回血,才提远程。
    const auto hp = fresh(self->dynamic.current_hp);
    const auto max_hp = fresh(self->statics.max_hp);
    if (!dead && hp && max_hp && *max_hp > 0 && *hp > 0 &&
        static_cast<double>(*hp) / *max_hp < kLowSelfRatio) {
        const int percent = static_cast<int>(std::lround(100.0 * *hp / *max_hp));
        const std::uint32_t cost = remaining ? remote_heal_cost(*remaining) : 0;
        const bool remote_ok = fresh(self->dynamic.can_remote_heal).value_or(false) && remaining &&
                               *remaining <= kEndgameSec && economy && *economy >= cost + kRemoteAmmoPrice;
        NextCard card;
        card.rule = NextRule::Heal;
        card.category = QStringLiteral("保命");
        card.title = remote_ok ? QStringLiteral("远程回血") : QStringLiteral("补血");
        card.reason = remote_ok ? QStringLiteral("本机只剩 %1%，花 %2 金").arg(percent).arg(cost)
                                : QStringLiteral("本机只剩 %1%").arg(percent);
        card.speech = remote_ok ? QStringLiteral("血量低，远程回血") : QStringLiteral("血量低，先补血");
        cards.push_back(card);
    }
    // 6 弹少补弹。只看弹数,不看死活;重装没装上 42mm 之前买不了,所以不出。
    if (shooter_ok && ammo && *ammo < (heavy ? kLowAmmo42 : kLowAmmo17)) {
        NextCard card;
        card.rule = NextRule::BuyAmmo;
        card.category = QStringLiteral("补给");
        card.title = QStringLiteral("补弹");
        card.reason = QStringLiteral("只剩 %1 发").arg(*ammo);
        card.speech = QStringLiteral("弹少，先补弹");
        cards.push_back(card);
    }
    // 7、8 重装 42mm:有权限才能装;装上后还要打一发验证弹才算成功安装(5.7.1)。
    if (heavy && !dead) {
        const auto permission = fresh(self->modules.modular_42mm);
        if (!snapshot.launcher_installed) {
            if (mounted && permission == 1u && !buffs.self_weak) {
                NextCard card;
                card.rule = NextRule::VerifyLauncher;
                card.category = QStringLiteral("装备");
                card.title = QStringLiteral("打一发验证弹");
                card.reason = QStringLiteral("打出一发被测到才算成功安装，没弹先买 1 发");
                card.speech = QStringLiteral("打一发验证弹");
                cards.push_back(card);
            } else if (!mounted && permission == 1u) {
                NextCard card;
                card.rule = NextRule::InstallLauncher;
                card.category = QStringLiteral("装备");
                card.title = QStringLiteral("去工作台装 42mm 发射机构");
                card.reason = QStringLiteral("装上才有 42mm");
                card.speech = QStringLiteral("去工作台装发射机构");
                cards.push_back(card);
            }
        }
    }

    // 9 打敌方前哨站:找敌方拥有的那座。
    if (can_shoot && !dead) {
        for (int index = 0; index < 2; ++index) {
            const auto zone = fresh(index == 0 ? game.red_outpost_zone : game.blue_outpost_zone);
            const auto hp = fresh(index == 0 ? game.red_outpost_hp : game.blue_outpost_hp);
            if (!hp || *hp == 0) continue;
            const std::uint32_t home = index == 0 ? kRed : kBlue;
            if (outpost_owner(zone, hp, home) != theirs) continue;
            const KillPlan plan = plan_for(*hp, heavy, max_heat, damage_scale);
            if (rounds >= plan.shots && plan.finishable) {
                NextCard card;
                card.rule = NextRule::AttackOutpost;
                card.category = QStringLiteral("进攻");
                card.title = QStringLiteral("打敌方前哨站");
                card.reason = QStringLiteral("还剩 %1 血，约 %2 发").arg(*hp).arg(plan.shots);
                card.speech = QStringLiteral("打敌方前哨站");
                cards.push_back(card);
            }
            break;
        }
    }
    // 10 集火残血敌人:雷达看得到才推荐,也不写距离。
    if (!dead && can_shoot) {
        std::optional<NextCard> focus;
        double best_ratio = 2.0;
        const auto self_at = position_of(snapshot, self_id);
        for (const RobotHealth& robot : snapshot.robot_health) {
            if (robot.faction != theirs) continue;
            const auto robot_hp = fresh(robot.current_hp);
            const auto robot_max = fresh(robot.max_hp);
            if (!robot_hp || !robot_max || *robot_max == 0 || *robot_hp == 0) continue;
            const double ratio = static_cast<double>(*robot_hp) / *robot_max;
            if (ratio > kLowEnemyRatio || ratio >= best_ratio) continue;
            const auto at = position_of(snapshot, robot.id.value);
            if (!at) continue;
            best_ratio = ratio;
            NextCard card;
            card.rule = NextRule::FocusEnemy;
            card.category = QStringLiteral("进攻");
            card.title = QStringLiteral("集火敌方 %1").arg(robot_name(robot.id.value));
            card.reason = QStringLiteral("只剩 %1 血").arg(*robot_hp);
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
    // 11 能量机关:只有步兵能开(5.6.3)。
    if (infantry && !dead && !buffs.self_weak && fresh(game.rune_status) == 1u) {
        if (const auto elapsed = fresh(game.stage_elapsed_sec)) {
            struct Window { std::int32_t at; bool big; };
            static const Window windows[] = {{0, false}, {90, false}, {180, true}, {255, true}, {330, true}};
            for (const Window& window : windows) {
                if (*elapsed < window.at || *elapsed - window.at > kRuneFreshSec) continue;
                NextCard card;
                card.rule = NextRule::OpenRune;
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
    // 12 重装装配:能选的难度比已经装成的高就去装。必须先装成上一级才能选下一级(5.4.3)。
    if (heavy && !dead) {
        const auto unlocked = fresh(snapshot.tech_core.maximum_difficulty);
        const auto done = fresh(game.tech_level);
        const auto core = fresh(snapshot.tech_core.basic_state);
        if (unlocked && done && *unlocked > *done && !(core && *core != 1)) {
            const std::uint32_t level = std::min(*unlocked, *done + 1);
            NextCard card;
            card.rule = NextRule::Assemble;
            card.category = QStringLiteral("装配");
            card.title = QStringLiteral("去装配 %1 级").arg(level);
            card.reason = first_assembly_gain(level);
            card.speech = QStringLiteral("新的装配难度开放，去装配");
            cards.push_back(card);
        }
    }
    // 13 我方阵地被敌方临时控制。
    const std::uint32_t enemy_temp = theirs == kRed ? kRedTemp : kBlueTemp;
    const std::uint32_t our_temp = ours == kRed ? kRedTemp : kBlueTemp;
    const std::uint32_t our_full = ours == kRed ? kRedFull : kBlueFull;
    const std::vector<Zone> zones = zones_of(game);
    for (const Zone& zone : zones) {
        if (zone.side != ours || zone.state != enemy_temp) continue;
        NextCard card;
        card.rule = NextRule::DefendZone;
        card.category = QStringLiteral("阵地");
        card.title = QStringLiteral("回防%1").arg(zone.name);
        card.reason = QStringLiteral("敌方正在临时控制");
        card.speech = QStringLiteral("阵地被敌方临时控制，回防");
        cards.push_back(card);
        break;
    }
    // 14 我方临时控制的阵地。自家前哨站已经倒了时,站着拿不回完全控制,要写清楚让开。
    for (std::size_t index = 0; index < zones.size(); ++index) {
        const Zone& zone = zones[index];
        if (zone.state != our_temp) continue;
        NextCard card;
        card.rule = NextRule::HoldZone;
        card.category = QStringLiteral("阵地");
        const bool own_outpost = index < 2 && zone.side == ours;
        const auto outpost_hp = index == 0 ? fresh(game.red_outpost_hp)
                                           : index == 1 ? fresh(game.blue_outpost_hp) : std::nullopt;
        if (own_outpost && outpost_hp && *outpost_hp == 0 && !our_base_defended) {
            card.title = QStringLiteral("让开 5 秒，等它重建再抢回来");
            card.reason = QStringLiteral("前哨站已经倒了，站着拿不回我方完全控制");
            card.speech = QStringLiteral("让开五秒，等前哨站重建再抢回来");
        } else {
            card.title = QStringLiteral("守住%1").arg(zone.name);
            card.reason = QStringLiteral("临时控制，5 秒没人激活就丢");
            card.speech = QStringLiteral("守住临时阵地");
        }
        cards.push_back(card);
        break;
    }
    // 16 控制度落后:去占一块还不是我方完全控制的阵地,先占自家的。
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
                card.category = QStringLiteral("抢分");
                card.title = QStringLiteral("去占%1").arg(target->name);
                card.reason = QStringLiteral("控制度落后 %1 分").arg(their_points - our_points);
                card.speech = QStringLiteral("控制度落后，去占阵地");
                cards.push_back(card);
            }
        }
    }
    // 17 最后 60 秒:按 5.8 的顺序算现在结束是输是赢(基地都还在时比控制度)。
    if (remaining && *remaining <= kEndgameSec && red_points && blue_points) {
        const long our_points = static_cast<long>(ours == kRed ? *red_points : *blue_points);
        const long their_points = static_cast<long>(ours == kRed ? *blue_points : *red_points);
        const long gap = our_points - their_points;
        NextCard card;
        card.rule = NextRule::Endgame;
        card.category = QStringLiteral("收尾");
        if (gap > 0) {
            card.title = QStringLiteral("现在结束就赢");
            card.reason = QStringLiteral("控制度领先 %1 分，守住").arg(gap);
            card.speech = QStringLiteral("现在结束就赢，守住");
        } else if (gap < 0) {
            card.title = QStringLiteral("现在结束就输");
            card.reason = QStringLiteral("控制度落后 %1 分，去占区追").arg(-gap);
            card.speech = QStringLiteral("现在结束就输，去占区");
        } else {
            card.title = QStringLiteral("现在结束是平局");
            card.reason = QStringLiteral("控制度打平，去占区");
            card.speech = QStringLiteral("现在打平，去占区");
        }
        cards.push_back(card);
    }
    // 阵亡时只剩补弹有意义(复活后要用);别的动作现在做不了,不占格子。
    if (dead) cards.erase(std::remove_if(cards.begin(), cards.end(),
                                         [](const NextCard& card) { return card.rule != NextRule::BuyAmmo; }),
                          cards.end());
    return cards;
}

std::vector<NextCard> rank_next(std::vector<NextCard> cards, std::size_t count) {
    std::stable_sort(cards.begin(), cards.end(),
                     [](const NextCard& a, const NextCard& b) { return rule_rank(a.rule) < rule_rank(b.rule); });
    if (cards.size() > count) cards.resize(count);
    return cards;
}

std::vector<NextCard> NextStepBoard::update(const Snapshot& snapshot, MonotonicMs now, std::size_t count,
                                            MonotonicMs dwell_ms, const NextBuffs& buffs) {
    const std::uint32_t ours = snapshot.self ? faction_of(snapshot.self->value) : kRed;
    const auto base_hp = fresh(ours == kRed ? snapshot.game.red_base_hp : snapshot.game.blue_base_hp);
    if (base_hp) {
        if (last_base_hp_ && *base_hp < *last_base_hp_) base_hit_until_ = now + kBaseHitHoldMs;
        last_base_hp_ = base_hp;
    }
    const std::vector<NextCard> all = next_candidates(snapshot, now < base_hit_until_, buffs);
    const std::vector<NextCard> ranked = rank_next(all, count);

    const auto present = [&all](const NextCard& card) {
        return std::any_of(all.begin(), all.end(), [&card](const NextCard& c) { return same_card(c, card); });
    };
    const bool stale = std::any_of(shown_.begin(), shown_.end(), [&](const NextCard& card) { return !present(card); });
    const bool preempt = !ranked.empty() && !shown_.empty() && rule_rank(ranked.front().rule) < rule_rank(shown_.front().rule);
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
    return {QStringLiteral("打敌方基地"),        QStringLiteral("先补弹再打基地"),   QStringLiteral("敌方在偷堡垒，回防"),
            QStringLiteral("基地被攻击，回防"),   QStringLiteral("血量低，远程回血"), QStringLiteral("血量低，先补血"),
            QStringLiteral("弹少，先补弹"),       QStringLiteral("去工作台装发射机构"), QStringLiteral("打一发验证弹"),
            QStringLiteral("打敌方前哨站"),       QStringLiteral("集火残血敌人"),     QStringLiteral("去开能量机关"),
            QStringLiteral("新的装配难度开放，去装配"),
            QStringLiteral("阵地被敌方临时控制，回防"), QStringLiteral("让开五秒，等前哨站重建再抢回来"),
            QStringLiteral("守住临时阵地"), QStringLiteral("控制度落后，去占阵地"),
            QStringLiteral("现在结束就赢，守住"), QStringLiteral("现在结束就输，去占区"),
            QStringLiteral("现在打平，去占区")};
}

}  // namespace rm_terminal
