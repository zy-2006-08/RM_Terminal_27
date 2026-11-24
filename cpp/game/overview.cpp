#include "overview.h"

#include "match_alerts.h"

#include <algorithm>
#include <cmath>

namespace rm_terminal {
namespace {

constexpr std::uint32_t kStageMatch = 4;
constexpr std::uint32_t kStageSettlement = 5;
constexpr std::int32_t kMatchSec = 420;
constexpr std::int32_t kRateWindowSec = 60;
constexpr std::int32_t kRateMinSec = 10;
// 阵地数组的下标:红前哨、蓝前哨、红基地、蓝基地。
constexpr std::size_t kRedOutpost = 0, kBlueOutpost = 1, kRedBase = 2, kBlueBase = 3;

template <class T>
std::optional<T> fresh(const Field<T>& field) {
    if (field.quality != Quality::Valid || field.freshness != Freshness::Fresh || !field.value) return std::nullopt;
    return field.value;
}

template <class T>
std::optional<T> valid(const Field<T>& field) {
    return field.quality == Quality::Valid ? field.value : std::nullopt;
}

bool held_by(std::optional<std::uint32_t> code, std::uint32_t faction) {
    if (!code) return false;
    return faction == kRed ? (*code == 1 || *code == 2) : (*code == 3 || *code == 4);
}

std::uint32_t temp_code(std::uint32_t faction) { return faction == kRed ? 1 : 3; }
std::uint32_t full_code(std::uint32_t faction) { return faction == kRed ? 2 : 4; }

QString clock(std::int64_t seconds) {
    seconds = std::max<std::int64_t>(0, seconds);
    return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

QString leader_text(Leader leader) {
    switch (leader) {
    case Leader::Ours: return QStringLiteral("我方赢");
    case Leader::Theirs: return QStringLiteral("敌方赢");
    case Leader::Even: return QStringLiteral("打平");
    case Leader::Unknown: break;
    }
    return QStringLiteral("等待数据");
}

using SampleField = std::optional<std::uint32_t> MatchSample::*;

SampleField pick(std::uint32_t faction, SampleField red, SampleField blue) { return faction == kRed ? red : blue; }

// 最近 60 秒里某个量平均每秒变多少;样本跨度不到 10 秒时为空。
std::optional<double> per_second(const std::vector<MatchSample>& samples, SampleField field) {
    if (samples.empty() || !(samples.back().*field)) return std::nullopt;
    const MatchSample& last = samples.back();
    for (const MatchSample& sample : samples) {
        if (last.elapsed - sample.elapsed > kRateWindowSec || !(sample.*field)) continue;
        const std::int32_t span = last.elapsed - sample.elapsed;
        if (span < kRateMinSec) return std::nullopt;
        return (static_cast<double>(*(last.*field)) - static_cast<double>(*(sample.*field))) / span;
    }
    return std::nullopt;
}

struct Change {
    QString what;
    std::int64_t deadline = 0;  // 倒计时还剩这么多秒之前做到
    int gain = 0;               // 之后每秒控制度差多出多少
};

// side 这一方要在 remaining 秒内追上 gap(对方最后比它多的控制度),换手哪一个阵地最宽裕。
// mine 为真时按我方的说法写阵地名,否则按敌方的说法(「拿下我方前哨阵地」)。
std::optional<Change> best_change(const std::array<std::optional<std::uint32_t>, 4>& zones, std::uint32_t side,
                                  double gap, std::int64_t remaining, bool mine) {
    const auto rates = control_rates(zones);
    if (!rates) return std::nullopt;
    const std::uint32_t other = opponent_of(side);
    const auto diff = [side](std::pair<int, int> r) { return side == kRed ? r.first - r.second : r.second - r.first; };
    const std::size_t own_outpost = side == kRed ? kRedOutpost : kBlueOutpost;
    const std::size_t their_outpost = side == kRed ? kBlueOutpost : kRedOutpost;
    const std::size_t own_base = side == kRed ? kRedBase : kBlueBase;
    const std::size_t their_base = side == kRed ? kBlueBase : kRedBase;
    struct Option { std::size_t zone; std::uint32_t code; QString ours, theirs; };
    std::vector<Option> options;
    if (!held_by(zones[their_outpost], side))
        options.push_back({their_outpost, temp_code(side), QStringLiteral("拿下敌方前哨阵地"), QStringLiteral("拿下我方前哨阵地")});
    if (held_by(zones[own_outpost], other))
        options.push_back({own_outpost, full_code(side), QStringLiteral("拿回我方前哨阵地"), QStringLiteral("拿回它的前哨阵地")});
    if (!held_by(zones[their_base], side))
        options.push_back({their_base, temp_code(side), QStringLiteral("临时控制敌方基地阵地"), QStringLiteral("临时控制我方基地阵地")});
    if (held_by(zones[own_base], other))
        options.push_back({own_base, full_code(side), QStringLiteral("拿回我方基地阵地"), QStringLiteral("拿回它的基地阵地")});

    std::optional<Change> best;
    for (const Option& option : options) {
        auto changed = zones;
        changed[option.zone] = option.code;
        const auto after = control_rates(changed);
        const int gain = diff(*after) - diff(*rates);
        if (gain <= 0) continue;
        // 换手以后剩下的秒数 × 每秒多出的 要大过差距。
        const std::int64_t need = static_cast<std::int64_t>(std::floor(std::max(0.0, gap) / gain)) + 1;
        if (need > remaining) continue;
        if (!best || need < best->deadline) best = Change{mine ? option.ours : option.theirs, need, gain};
    }
    return best;
}

}  // namespace

std::optional<std::pair<int, int>> control_rates(const std::array<std::optional<std::uint32_t>, 4>& zones) {
    for (const auto& zone : zones)
        if (!zone) return std::nullopt;
    const auto rate = [&zones](std::uint32_t faction) {
        int r = 0;
        if (held_by(zones[kRedOutpost], faction) && held_by(zones[kBlueOutpost], faction)) r += 1;
        if (held_by(zones[kRedBase], faction) && held_by(zones[kBlueBase], faction)) r += 3;
        return r;
    };
    return std::make_pair(rate(kRed), rate(kBlue));
}

Verdict judge(const MatchSample& s, std::uint32_t ours) {
    const std::uint32_t theirs = opponent_of(ours);
    const auto get = [&s](std::uint32_t faction, SampleField red, SampleField blue) { return s.*pick(faction, red, blue); };
    const auto line = [](Leader leader, const QString& rest) {
        return Verdict{leader, leader_text(leader) + QStringLiteral(" · ") + rest};
    };
    const auto compare = [&](const QString& label, SampleField red, SampleField blue) -> std::optional<Verdict> {
        const auto a = get(ours, red, blue);
        const auto b = get(theirs, red, blue);
        if (!a || !b || *a == *b) return std::nullopt;
        return line(*a > *b ? Leader::Ours : Leader::Theirs, QStringLiteral("%1 %2:%3").arg(label).arg(*a).arg(*b));
    };
    const auto my_base = get(ours, &MatchSample::red_base, &MatchSample::blue_base);
    const auto their_base = get(theirs, &MatchSample::red_base, &MatchSample::blue_base);
    if (my_base && their_base && (*my_base == 0 || *their_base == 0)) {
        if (*my_base == *their_base) return line(Leader::Even, QStringLiteral("双方基地都没了"));
        return line(*my_base > *their_base ? Leader::Ours : Leader::Theirs,
                    *my_base == 0 ? QStringLiteral("我方基地被打掉") : QStringLiteral("敌方基地被打掉"));
    }
    const auto my_control = get(ours, &MatchSample::red_control, &MatchSample::blue_control);
    const auto their_control = get(theirs, &MatchSample::red_control, &MatchSample::blue_control);
    if (!my_control || !their_control) return Verdict{Leader::Unknown, leader_text(Leader::Unknown)};
    if (auto v = compare(QStringLiteral("控制度"), &MatchSample::red_control, &MatchSample::blue_control)) return *v;
    if (auto v = compare(QStringLiteral("总伤害"), &MatchSample::red_damage, &MatchSample::blue_damage)) return *v;
    if (auto v = compare(QStringLiteral("剩余血量"), &MatchSample::red_hp, &MatchSample::blue_hp)) return *v;
    return line(Leader::Even, QStringLiteral("控制度 %1:%2").arg(*my_control).arg(*their_control));
}

void MatchLog::reset() {
    samples_.clear();
    notes_.clear();
    winner_.reset();
    end_reason_.reset();
    reported_ = false;
}

bool MatchLog::observe(const Snapshot& snapshot) {
    const GameFields<Field>& g = snapshot.game;
    if (snapshot.self) self_ = snapshot.self->value;
    if (const auto round = valid(g.current_round)) {
        if (round_ && *round != *round_) reset();
        round_ = round;
    }
    bool finished = false;
    const auto stage = fresh(g.current_stage);
    if (stage) {
        if (*stage < kStageMatch && stage_ && *stage_ >= kStageMatch) reset();
        if (*stage == kStageSettlement && stage_ == kStageMatch && !reported_ && !samples_.empty()) {
            winner_ = valid(g.winner);
            end_reason_ = valid(g.end_reason);
            reported_ = true;
            finished = true;
        }
        stage_ = stage;
    }
    if (stage != kStageMatch) return finished;
    const auto elapsed = fresh(g.stage_elapsed_sec);
    if (!elapsed) return finished;
    countdown_ = fresh(g.stage_countdown_sec);

    const std::vector<EventRecord> events = snapshot.events.recent(snapshot.events.size());
    for (auto it = events.rbegin(); it != events.rend(); ++it)
        if (it->received_at > last_event_at_)
            notes_.push_back({*elapsed, it->countdown_sec ? it->countdown_sec : countdown_, QString::fromStdString(it->text),
                              recording_ms_});
    if (!events.empty()) last_event_at_ = std::max(last_event_at_, events.front().received_at);

    if (!samples_.empty() && samples_.back().elapsed >= *elapsed) return finished;
    MatchSample sample;
    sample.elapsed = *elapsed;
    sample.countdown = countdown_;
    sample.red_control = fresh(g.red_control_points);
    sample.blue_control = fresh(g.blue_control_points);
    sample.red_base = fresh(g.red_base_hp);
    sample.blue_base = fresh(g.blue_base_hp);
    sample.red_outpost = fresh(g.red_outpost_hp);
    sample.blue_outpost = fresh(g.blue_outpost_hp);
    sample.red_damage = fresh(g.red_total_damage);
    sample.blue_damage = fresh(g.blue_total_damage);
    sample.economy = fresh(g.economy);
    if (snapshot.radar_intel && snapshot.radar_intel->field_fresh) sample.enemy_gold = snapshot.radar_intel->gold;
    for (const RobotHealth& robot : snapshot.robot_health) {
        const auto hp = fresh(robot.current_hp);
        if (!hp) continue;
        std::optional<std::uint32_t>& total = robot.faction == kBlue ? sample.blue_hp : sample.red_hp;
        total = total.value_or(0) + *hp;
    }
    sample.zones = {fresh(g.red_outpost_zone), fresh(g.blue_outpost_zone), fresh(g.red_base_zone),
                    fresh(g.blue_base_zone)};
    samples_.push_back(sample);
    return finished;
}

void MatchLog::note(const QString& text) {
    if (stage_ != kStageMatch || samples_.empty()) return;
    notes_.push_back({samples_.back().elapsed, countdown_, text, recording_ms_});
}

Overview build_overview(const Snapshot& snapshot, const BuffBoard& buffs, const MatchLog& log, MonotonicMs now) {
    Overview o;
    const GameFields<Field>& g = snapshot.game;
    const std::vector<MatchSample>& samples = log.samples();
    if (!snapshot.self || fresh(g.current_stage) != kStageMatch || samples.empty()) {
        o.now.text = QStringLiteral("开赛后开始算");
        return o;
    }
    o.in_match = true;
    const std::uint32_t ours = faction_of(snapshot.self->value);
    const std::uint32_t theirs = opponent_of(ours);
    const bool red = ours == kRed;
    const MatchSample& last = samples.back();
    o.elapsed = last.elapsed;
    const std::int64_t remaining = fresh(g.stage_countdown_sec).value_or(kMatchSec - last.elapsed);
    o.match_sec = static_cast<std::int32_t>(last.elapsed + remaining);
    o.now = judge(last, ours);

    for (const MatchSample& sample : samples)
        if (sample.red_control && sample.blue_control) {
            const std::int64_t diff = static_cast<std::int64_t>(*sample.red_control) - *sample.blue_control;
            o.trend.emplace_back(sample.elapsed, red ? diff : -diff);
        }

    // 照这样打到 7:00:控制度按阵地算的速度往后推,基地和总伤害按最近 60 秒的速度推。
    const auto rates = control_rates(last.zones);
    MatchSample end = last;
    std::optional<std::pair<std::uint32_t, double>> destroyed;  // 哪一方的基地、几秒后
    for (const std::uint32_t side : {ours, theirs}) {
        const SampleField base = pick(side, &MatchSample::red_base, &MatchSample::blue_base);
        const auto drop = per_second(samples, base);
        if (!(last.*base) || !drop || *drop >= -0.5) continue;
        const double after = *(last.*base) / -*drop;
        if (after < remaining && (!destroyed || after < destroyed->second)) destroyed = std::make_pair(side, after);
    }
    if (rates) {
        const int mine_rate = red ? rates->first : rates->second;
        const int their_rate = red ? rates->second : rates->first;
        o.projection = mine_rate - their_rate;
        o.rates = QStringLiteral("控制度每秒 我 +%1 · 敌 +%2").arg(mine_rate).arg(their_rate);
        if (end.red_control) *end.red_control += static_cast<std::uint32_t>(rates->first * remaining);
        if (end.blue_control) *end.blue_control += static_cast<std::uint32_t>(rates->second * remaining);
    }
    for (const auto field : {&MatchSample::red_damage, &MatchSample::blue_damage})
        if (const auto grow = per_second(samples, field); grow && *grow > 0 && end.*field)
            *(end.*field) += static_cast<std::uint32_t>(*grow * remaining);
    if (destroyed) {
        const bool mine_falls = destroyed->first == ours;
        o.at_end = Verdict{mine_falls ? Leader::Theirs : Leader::Ours,
                           QStringLiteral("%1 · 倒计时 %2 %3基地被打掉")
                               .arg(leader_text(mine_falls ? Leader::Theirs : Leader::Ours))
                               .arg(clock(remaining - static_cast<std::int64_t>(destroyed->second)))
                               .arg(mine_falls ? QStringLiteral("我方") : QStringLiteral("敌方"))};
    } else if (rates) {
        o.at_end = judge(end, ours);
    } else {
        o.at_end = Verdict{Leader::Unknown, QStringLiteral("等阵地数据")};
    }

    // 翻盘点:照这样输就找我方能翻的那一步;照这样赢就提醒敌方能翻的那一步。
    const auto control_of = [&end](std::uint32_t side) {
        const auto value = end.*pick(side, &MatchSample::red_control, &MatchSample::blue_control);
        return static_cast<double>(value.value_or(0));
    };
    if (destroyed && destroyed->first == ours) {
        o.flip = QStringLiteral("守基地：照现在 %1 秒后被打掉").arg(static_cast<int>(destroyed->second));
        if (last.zones[red ? kRedOutpost : kBlueOutpost] != full_code(ours))
            o.flip += QStringLiteral("，拿回我方前哨阵地基地就有 100% 防御");
        o.flip_urgent = true;
    } else if (rates && (o.at_end.leader == Leader::Theirs || o.at_end.leader == Leader::Even)) {
        const double gap = control_of(theirs) - control_of(ours);
        if (const auto change = best_change(last.zones, ours, gap, remaining, true))
            o.flip = QStringLiteral("翻盘：%1，倒计时 %2 前拿下（之后每秒多 %3）")
                         .arg(change->what, clock(change->deadline))
                         .arg(change->gain);
        else
            o.flip = QStringLiteral("换一个阵地也追不上控制度，只能打掉敌方基地");
        o.flip_urgent = o.at_end.leader == Leader::Theirs;
    } else if (rates && o.at_end.leader == Leader::Ours) {
        const double gap = control_of(ours) - control_of(theirs);
        if (const auto change = best_change(last.zones, theirs, gap, remaining, false))
            o.flip = QStringLiteral("守住：敌方%1，倒计时 %2 前拿下就能翻").arg(change->what, clock(change->deadline));
        else
            o.flip = QStringLiteral("照这样敌方换一个阵地也翻不了");
    }

    QStringList parts;
    for (const std::uint32_t side : {ours, theirs})
        if (const auto& rune = buffs.side(side).rune)
            if (const QString text = rune_buff_text(*rune, now); !text.isEmpty())
                parts << (side == ours ? QStringLiteral("我 ") : QStringLiteral("敌 ")) + text;
    if (const auto stolen = fresh(red ? g.red_fortress_activation_sec : g.blue_fortress_activation_sec); stolen && *stolen > 0)
        parts << QStringLiteral("我方堡垒被偷 %1/10秒").arg(*stolen);
    if (const auto stealing = fresh(red ? g.blue_fortress_activation_sec : g.red_fortress_activation_sec); stealing && *stealing > 0)
        parts << QStringLiteral("在偷敌方堡垒 %1/10秒").arg(*stealing);
    o.buffs = parts.isEmpty() ? QStringLiteral("双方都没有能量机关增益") : parts.join(QStringLiteral(" · "));

    for (const TimelineMark& mark : match_timeline())
        if (mark.at_sec > last.elapsed) {
            o.next_mark = QStringLiteral("下一个：%1（还有 %2 秒）").arg(mark.text).arg(mark.at_sec - last.elapsed);
            break;
        }
    return o;
}

}  // namespace rm_terminal
