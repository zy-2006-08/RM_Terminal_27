#include "store.h"

#include "match_event.h"

#include <algorithm>
#include <stdexcept>

namespace rm_terminal {

EventHistory::EventHistory(std::size_t capacity) : buffer_(capacity == 0 ? 1 : capacity) {}

void EventHistory::push(EventRecord record) {
    if (size_ == buffer_.size()) ++dropped_;
    buffer_[next_] = std::move(record);
    next_ = (next_ + 1) % buffer_.size();
    if (size_ < buffer_.size()) ++size_;
}

std::vector<EventRecord> EventHistory::recent(std::size_t max) const {
    const std::size_t count = max < size_ ? max : size_;
    std::vector<EventRecord> result;
    result.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        // Walk backwards from the most recently written slot so the newest record
        // comes out first. `+ buffer_.size()` keeps the modulo operand positive.
        const std::size_t index = (next_ + buffer_.size() - 1 - i) % buffer_.size();
        result.push_back(buffer_[index]);
    }
    return result;
}

namespace {
template<class T>
void age_field(Field<T>& field, MonotonicMs now, MonotonicMs threshold) {
    field.freshness = !field.last_valid ? Freshness::NeverReceived
                      : now - *field.last_valid >= threshold ? Freshness::Stale
                                                             : Freshness::Fresh;
}
template<class Group>
void age(Group& group, MonotonicMs now, MonotonicMs threshold) {
    std::apply([=](auto&... field) { (age_field(field, now, threshold), ...); }, group.fields());
}
template<class T>
void set_valid(Field<T>& field, T value, MonotonicMs now) {
    field.value = value;
    field.last_valid = now;
    field.quality = Quality::Valid;
    field.freshness = Freshness::Fresh;
}
bool valid_source(std::optional<RobotId> source) { return source && source->value > 0; }

// 兵种的最低上限血量(规则手册 5.3):重装开局 400,步兵功率优先 1 级 150,半自动哨兵 200。
// 只当下限用,真实上限靠「本局见过的最高血量」往上抬。
std::uint32_t hp_floor(std::uint32_t robot_id) {
    switch (robot_id > 100 ? robot_id - 100 : robot_id) {
    case 1: return 400;
    case 3:
    case 4:
    case 5: return 150;
    case 7: return 200;
    default: return 1;
    }
}
}

Store::Store(MonotonicMs stale_after_ms, std::size_t event_capacity)
    : threshold_(stale_after_ms), event_capacity_(event_capacity) {
    if (threshold_ <= 0) throw std::invalid_argument("freshness threshold must be positive");
    state_.events = EventHistory(event_capacity);
}
void Store::setSelf(RobotId self) {
    std::lock_guard<std::mutex> lock(mutex_);
    self_ = self;
}
std::uint32_t Store::selfFaction() const { return self_ ? faction_of(self_->value) : kRed; }
void Store::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    Snapshot fresh;
    state_ = fresh;
    state_.events = EventHistory(event_capacity_);
    latest_ = 0;
    round_.reset();
    stage_.reset();
    resetRound();
}
void Store::resetRound() {
    state_.kills = KillTally{};
    enemy_fixed_dart_hits_ = 0;
    blind_until_.reset();
    blind_target_ = 0;
    max_hp_seen_.clear();
}
bool Store::advance(MonotonicMs now) {
    if (now < latest_) return false;
    latest_ = now;
    return true;
}
bool Store::apply(const inbound::GameStatus& patch, MonotonicMs now) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!advance(now)) return false;
    const bool ok = convert(state_.game, patch, now);
    // 换局:局号变了,或者从比赛中/结算回到赛前阶段。击杀数、飞镖命中次数、遮挡和上限血量
    // 都是按局算的。
    const auto& round = state_.game.current_round;
    const auto& stage = state_.game.current_stage;
    if (patch.current_round && round.quality == Quality::Valid) {
        if (round_ && *round_ != *round.value) resetRound();
        round_ = round.value;
    }
    if (patch.current_stage && stage.quality == Quality::Valid) {
        if (stage_ && *stage_ >= 4 && *stage.value < 4) resetRound();
        stage_ = stage.value;
    }
    return ok;
}
bool Store::apply(const inbound::RobotHealthSet& patch, MonotonicMs now) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!advance(now)) return false;
    std::vector<RobotHealth> health;
    health.reserve(patch.entries.size());
    for (const auto& entry : patch.entries) {
        RobotHealth robot;
        robot.id = RobotId{entry.robot_id};
        robot.faction = faction_of(entry.robot_id);
        std::uint32_t& seen = max_hp_seen_[entry.robot_id];
        if (entry.current_hp) {
            set_valid(robot.current_hp, *entry.current_hp, now);
            seen = std::max(seen, *entry.current_hp);
        }
        if (entry.bullets) set_valid(robot.bullets, *entry.bullets, now);
        std::uint32_t max_hp = std::max(seen, hp_floor(entry.robot_id));
        if (self_ && entry.robot_id == self_->value) {
            const auto self = state_.robots.find(*self_);
            if (self != state_.robots.end() && self->second.statics.max_hp.value)
                max_hp = *self->second.statics.max_hp.value;
        }
        set_valid(robot.max_hp, max_hp, now);
        health.push_back(std::move(robot));
    }
    std::stable_sort(health.begin(), health.end(),
                     [](const RobotHealth& left, const RobotHealth& right) { return left.id < right.id; });
    state_.robot_health = std::move(health);
    return true;
}
bool Store::apply(const inbound::Event& patch, MonotonicMs now) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!advance(now)) return false;
    const std::uint32_t self = selfFaction();
    MatchEvent event = describe_event(patch.event_id, patch.param, self);

    if (event.killer_faction) {
        if (*event.killer_faction == kRed) ++state_.kills.red;
        else ++state_.kills.blue;
    }
    // 对方飞镖命中:按规则推算我方操作界面被遮挡到什么时候。连续命中时间叠加。
    if (event.dart_hitter && *event.dart_hitter != self) {
        const MonotonicMs duration = dart_blind_ms(event.dart_target, enemy_fixed_dart_hits_);
        if (event.dart_target <= 3) ++enemy_fixed_dart_hits_;
        const MonotonicMs from = blind_until_ && *blind_until_ > now ? *blind_until_ : now;
        blind_until_ = from + duration;
        blind_target_ = event.dart_target;
        event.text += "，遮挡 " + std::to_string(duration / 1000) + " 秒";
    }
    // 阵地易手比 1Hz 的 GlobalUnitStatus 先到,先更新,下一帧全局状态会再确认一次。
    if (event.zone_index) {
        Field<std::uint32_t>* zones[4] = {&state_.game.red_outpost_zone, &state_.game.blue_outpost_zone,
                                          &state_.game.red_base_zone, &state_.game.blue_base_zone};
        set_valid(*zones[*event.zone_index], event.zone_code, now);
    }

    EventRecord record;
    record.event_id = patch.event_id;
    record.level = event.level;
    record.text = std::move(event.text);
    record.received_at = now;
    record.faction = event.faction;
    if (state_.game.current_stage.value == 4u && state_.game.stage_countdown_sec.value)
        record.countdown_sec = state_.game.stage_countdown_sec.value;
    state_.events.push(std::move(record));
    return event.understood;
}
bool Store::apply(const inbound::RobotDynamicStatus& patch, std::optional<RobotId> source, MonotonicMs now) {
    std::lock_guard<std::mutex> lock(mutex_);
    return valid_source(source) && advance(now) && convert(state_.robots[*source].dynamic, patch, now);
}
bool Store::apply(const inbound::RobotStaticStatus& patch, std::optional<RobotId> source, MonotonicMs now) {
    std::lock_guard<std::mutex> lock(mutex_);
    return valid_source(source) && advance(now) && convert(state_.robots[*source].statics, patch, now);
}
bool Store::apply(const inbound::RobotModuleStatus& patch, std::optional<RobotId> source, MonotonicMs now) {
    std::lock_guard<std::mutex> lock(mutex_);
    return valid_source(source) && advance(now) && convert(state_.robots[*source].modules, patch, now);
}
bool Store::apply(const inbound::RobotPosition& patch, std::optional<RobotId> source, MonotonicMs now) {
    std::lock_guard<std::mutex> lock(mutex_);
    return valid_source(source) && advance(now) && convert(state_.robots[*source].position, patch, now);
}
bool Store::apply(const inbound::RobotRespawnStatus& patch, std::optional<RobotId> source, MonotonicMs now) {
    std::lock_guard<std::mutex> lock(mutex_);
    return valid_source(source) && advance(now) && convert(state_.robots[*source].respawn, patch, now);
}
bool Store::apply(const inbound::RadarSet& patch, MonotonicMs now) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!advance(now)) return false;
    bool ok = true;
    std::vector<MapRobot> robots;
    robots.reserve(patch.entries.size());
    for (const auto& entry : patch.entries) {
        if (entry.robot_id == 0) {
            ++state_.map_invalid_entries;
            ok = false;
            continue;
        }
        MapRobot robot;
        robot.id = RobotId{entry.robot_id};
        robot.faction = faction_of(entry.robot_id);
        robot.is_self = self_ && self_->value == entry.robot_id;
        robot.from_radar = true;
        robot.highlight = entry.highlight;
        // 雷达这一帧没看到这台(0,0)时保留上一次的位置,让它在地图上按时效变旧,
        // 而不是凭空消失。自机坐标只认 RobotPosition,这里一律不写。
        const auto previous = std::find_if(state_.map_robots.begin(), state_.map_robots.end(),
                                           [&](const MapRobot& old) { return old.id.value == entry.robot_id; });
        if (previous != state_.map_robots.end()) robot.position = previous->position;
        if (!robot.is_self && entry.x && entry.y) {
            inbound::RobotPosition position;
            position.x = entry.x;
            position.y = entry.y;
            if (!convert(robot.position, position, now)) {
                ++state_.map_invalid_entries;
                ok = false;
            }
        }
        robots.push_back(std::move(robot));
    }
    std::stable_sort(robots.begin(), robots.end(),
                     [](const MapRobot& left, const MapRobot& right) { return left.id < right.id; });
    state_.map_robots = std::move(robots);
    return ok;
}
bool Store::apply(const inbound::TechCoreStatus& patch, MonotonicMs now) {
    std::lock_guard<std::mutex> lock(mutex_);
    return advance(now) && convert(state_.tech_core, patch, now);
}
bool Store::apply(const inbound::DartStatus& patch, MonotonicMs now) {
    std::lock_guard<std::mutex> lock(mutex_);
    return advance(now) && convert(state_.dart, patch, now);
}
bool Store::apply(const inbound::SentryStatus& patch, MonotonicMs now) {
    std::lock_guard<std::mutex> lock(mutex_);
    return advance(now) && convert(state_.sentry, patch, now);
}
bool Store::apply(const inbound::SentryCtrlResult& patch, MonotonicMs now) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!advance(now)) return false;
    state_.sentry_result = SentryCtrlOutcome{patch.command_id, patch.result_code, now};
    return true;
}
bool Store::apply(const inbound::PathPlan& patch, MonotonicMs now) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!advance(now)) return false;
    state_.sentry_path = PathPlan{patch.intention, patch.sender_id, patch.points, now};
    return true;
}
bool Store::apply(const inbound::MapClick& patch, MonotonicMs now) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!advance(now)) return false;
    MapMark mark = patch;
    mark.received_at = now;
    state_.map_marks.push_back(mark);
    if (state_.map_marks.size() > kMapMarkCapacity)
        state_.map_marks.erase(state_.map_marks.begin());
    return true;
}
Snapshot Store::snapshot(MonotonicMs now) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (now < latest_) throw std::invalid_argument("snapshot precedes latest update");
    auto copy = state_;
    copy.self = self_;

    // 服务器给的坐标(自机;指挥手还有哨兵)盖过雷达,雷达里没有的就补一行。
    for (const auto& [id, robot] : state_.robots) {
        if (!robot.position.x.last_valid && !robot.position.y.last_valid) continue;
        auto found = std::find_if(copy.map_robots.begin(), copy.map_robots.end(),
                                  [&](const MapRobot& candidate) { return candidate.id.value == id.value; });
        if (found == copy.map_robots.end()) {
            MapRobot added;
            added.id = id;
            added.faction = faction_of(id.value);
            copy.map_robots.push_back(added);
            found = copy.map_robots.end() - 1;
        }
        found->is_self = self_ && self_->value == id.value;
        found->from_radar = false;
        found->position = robot.position;
    }
    std::stable_sort(copy.map_robots.begin(), copy.map_robots.end(),
                     [](const MapRobot& left, const MapRobot& right) { return left.id < right.id; });

    age(copy.game, now, threshold_);
    for (auto& robot : copy.map_robots) age(robot.position, now, threshold_);
    for (auto& robot : copy.robot_health) {
        age_field(robot.current_hp, now, threshold_);
        age_field(robot.max_hp, now, threshold_);
        age_field(robot.bullets, now, threshold_);
    }
    for (auto& entry : copy.robots) {
        auto& robot = entry.second;
        age(robot.dynamic, now, threshold_);
        age(robot.statics, now, threshold_);
        age(robot.modules, now, threshold_);
        age(robot.position, now, threshold_);
        age(robot.respawn, now, threshold_);
    }
    age(copy.tech_core, now, threshold_);
    age(copy.dart, now, threshold_);
    age(copy.sentry, now, threshold_);

    // 遮挡是本机按事件推算的,不随某条消息老化:正在遮挡时一定可信;没在遮挡时,
    // 可信度跟着服务器数据流走(拿 5Hz 的 GameStatus 当代理)——流断了就可能漏掉命中事件。
    auto& blind = copy.blind;
    const bool blinded = blind_until_ && now < *blind_until_;
    const auto& stage = copy.game.current_stage;
    if (blinded || stage.last_valid) {
        const MonotonicMs as_of = blinded ? now : *stage.last_valid;
        const Freshness freshness = blinded ? Freshness::Fresh : stage.freshness;
        const auto stamp = [&](auto& field, auto value) {
            field.value = value;
            field.quality = Quality::Valid;
            field.last_valid = as_of;
            field.freshness = freshness;
        };
        stamp(blind.self_base_blinded, blinded);
        stamp(blind.blind_remaining_ms,
              blinded ? static_cast<std::int32_t>(*blind_until_ - now) : std::int32_t{0});
        if (blind_target_ != 0) stamp(blind.target, blind_target_);
    }
    return copy;
}
}
