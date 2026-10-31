#include "match_event.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>

namespace rm_terminal {
namespace {

std::vector<std::string> split(const std::string& text) {
    std::vector<std::string> parts;
    std::string current;
    for (const char c : text) {
        if (c == ',') {
            parts.push_back(current);
            current.clear();
        } else if (c != ' ') {
            current.push_back(c);
        }
    }
    parts.push_back(current);
    return parts;
}

std::optional<long> parse_int(const std::string& text) {
    if (text.empty()) return std::nullopt;
    char* end = nullptr;
    errno = 0;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0') return std::nullopt;
    return value;
}

std::optional<double> parse_double(const std::string& text) {
    if (text.empty()) return std::nullopt;
    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(text.c_str(), &end);
    if (errno != 0 || end == text.c_str() || *end != '\0') return std::nullopt;
    return value;
}

std::string side(std::uint32_t faction, std::uint32_t self) {
    return faction == self ? "我方" : "敌方";
}

std::string unit_class(std::uint32_t number) {
    switch (number) {
    case 1: return "重装";
    case 3:
    case 4:
    case 5: return "步兵";
    case 6: return "空中";
    case 7: return "哨兵";
    case 8: return "飞镖";
    case 9: return "雷达";
    case 10: return "基地";
    case 11: return "前哨站";
    default: return "机器人";
    }
}

std::string robot(std::uint32_t id, std::uint32_t self) {
    const std::uint32_t number = id > 100 ? id - 100 : id;
    return side(faction_of(id), self) + " " + std::to_string(number) + " 号" + unit_class(number);
}

std::string dart_target_name(std::uint32_t target) {
    switch (target) {
    case 1: return "前哨站";
    case 2: return "基地固定目标";
    case 3: return "基地随机固定目标";
    case 4: return "基地随机移动目标";
    case 5: return "基地末端移动目标";
    default: return "未知目标";
    }
}

std::string assembly_result(long code) {
    switch (code) {
    case 0: return "装配成功";
    case 1: return "装配失败：能量单元被拔出";
    case 2: return "装配失败：超时";
    case 3: return "装配失败：离开装配区过久";
    case 4: return "装配失败：重装战亡";
    case 5: return "装配失败：四级难度未在协作时限内完成";
    case 6: return "装配已主动退出";
    case 7: return "装配失败：结算时没检测到能量单元";
    case 8: return "装配失败：缓冲期到期，流程强制结束";
    default: return "装配结果 " + std::to_string(code);
    }
}

MatchEvent raw(std::int32_t event_id, const std::string& param, const char* why) {
    MatchEvent event;
    event.text = std::string(why) + " " + std::to_string(event_id) + "：" + param;
    event.understood = false;
    return event;
}

// 阵地易手。owner 是这块阵地开局时的归属方,controller/full 是事件给出的新状态。
MatchEvent zone_change(std::size_t index, std::uint32_t owner, std::uint32_t controller,
                       bool full, bool outpost, std::uint32_t self) {
    MatchEvent event;
    event.zone_index = index;
    event.zone_code = controller == kRed ? (full ? 2u : 1u) : (full ? 4u : 3u);
    event.faction = controller;
    event.text = side(owner, self) + (outpost ? "前哨阵地" : "基地阵地") + "被" +
                 side(controller, self) + (full ? "完全控制" : "临时控制");
    const bool lost = owner == self && controller != self;
    event.level = lost ? (full || !outpost ? 3u : 2u) : 1u;
    return event;
}

}  // namespace

MatchEvent describe_event(std::int32_t event_id, const std::string& param,
                          std::uint32_t self_faction) {
    const std::uint32_t self = self_faction == kBlue ? kBlue : kRed;
    const std::uint32_t enemy = opponent_of(self);
    const std::vector<std::string> args = split(param);
    const auto arg = [&args](std::size_t index) -> std::optional<long> {
        return index < args.size() ? parse_int(args[index]) : std::nullopt;
    };

    MatchEvent event;
    switch (event_id) {
    case 1: {
        const auto victim = arg(0);
        const auto killer = arg(1);
        if (!victim || *victim <= 0) return raw(event_id, param, "事件参数无法解析");
        const auto victim_id = static_cast<std::uint32_t>(*victim);
        event.level = faction_of(victim_id) == self ? 2u : 1u;
        if (killer && *killer > 0) {
            const auto killer_id = static_cast<std::uint32_t>(*killer);
            event.killer_faction = faction_of(killer_id);
            event.faction = faction_of(killer_id);
            event.text = robot(killer_id, self) + "击毁" + robot(victim_id, self);
        } else {
            event.faction = faction_of(victim_id);
            event.text = robot(victim_id, self) + "战亡";
        }
        return event;
    }
    case 2: {
        const auto target = arg(0);
        if (!target || (*target != 11 && *target != 111))
            return raw(event_id, param, "事件参数无法解析");
        const std::uint32_t owner = *target == 11 ? kRed : kBlue;
        event.faction = owner;
        event.level = owner == self ? 3u : 1u;
        event.text = side(owner, self) + "前哨站被击毁";
        return event;
    }
    case 3: {
        const auto arms = arg(0);
        const auto rings = args.size() > 1 ? parse_double(args[1]) : std::nullopt;
        if (!arms) return raw(event_id, param, "事件参数无法解析");
        char rings_text[32] = "--";
        if (rings) std::snprintf(rings_text, sizeof(rings_text), "%.1f", *rings);
        event.level = 1;
        event.text = "大能量机关激活 " + std::to_string(*arms) + " 臂，平均 " + rings_text + " 环";
        return event;
    }
    case 4: {
        const auto kind = arg(0);
        if (!kind || (*kind != 1 && *kind != 2)) return raw(event_id, param, "事件参数无法解析");
        event.level = 1;
        event.text = *kind == 1 ? "小能量机关已激活" : "大能量机关已激活";
        return event;
    }
    case 5:
    case 6: {
        const auto state = arg(0);
        if (!state || *state < 1 || *state > 4) return raw(event_id, param, "事件参数无法解析");
        const std::uint32_t owner = event_id == 5 ? kRed : kBlue;
        const std::uint32_t controller = *state <= 2 ? kRed : kBlue;
        const bool full = *state == 2 || *state == 4;
        return zone_change(event_id == 5 ? 0 : 1, owner, controller, full, true, self);
    }
    case 7:
    case 8: {
        const auto state = arg(0);
        if (!state || (*state != 1 && *state != 2)) return raw(event_id, param, "事件参数无法解析");
        const std::uint32_t owner = event_id == 7 ? kRed : kBlue;
        const bool full = *state == 1;
        return zone_change(event_id == 7 ? 2 : 3, owner, full ? owner : opponent_of(owner), full,
                           false, self);
    }
    case 9: {
        const auto hitter = arg(0);
        const auto target = arg(1);
        if (!hitter || (*hitter != 1 && *hitter != 2) || !target || *target < 1 || *target > 5)
            return raw(event_id, param, "事件参数无法解析");
        const auto hitter_faction = static_cast<std::uint32_t>(*hitter);
        event.dart_hitter = hitter_faction;
        event.dart_target = static_cast<std::uint32_t>(*target);
        event.faction = hitter_faction;
        event.level = hitter_faction == self ? 1u : 3u;
        event.text = side(hitter_faction, self) + "飞镖命中" + side(opponent_of(hitter_faction), self) +
                     dart_target_name(event.dart_target);
        return event;
    }
    case 10:
        event.level = 2;
        event.faction = enemy;
        event.text = "敌方飞镖闸门开启";
        return event;
    case 11:
        event.level = 2;
        event.faction = self;
        event.text = "我方基地遭到攻击";
        return event;
    case 12:
        event.level = 1;
        event.faction = enemy;
        event.text = "敌方前哨站停转";
        return event;
    case 13:
        event.level = 1;
        event.faction = enemy;
        event.text = "敌方基地护甲展开";
        return event;
    case 14:
        event.level = 2;
        event.faction = self;
        event.text = "我方重装战亡，装配进入强制退出缓冲期";
        return event;
    case 15: {
        const auto code = arg(0);
        if (!code) return raw(event_id, param, "事件参数无法解析");
        event.level = *code == 0 ? 1u : 2u;
        event.faction = self;
        event.text = assembly_result(*code);
        return event;
    }
    case 16:
        event.level = 1;
        event.faction = self;
        event.text = "我方重装装上了模块化 42mm 发射机构";
        return event;
    default:
        return raw(event_id, param, "未知事件");
    }
}

MonotonicMs dart_blind_ms(std::uint32_t target, int prior_fixed_hits) {
    if (target == 4 || target == 5) return 10000;
    if (target < 1 || target > 3) return 0;
    static constexpr MonotonicMs kFixed[] = {10000, 5000, 3000, 2000};
    const int index = prior_fixed_hits < 0 ? 0 : (prior_fixed_hits > 3 ? 3 : prior_fixed_hits);
    return kFixed[index];
}

}  // namespace rm_terminal
