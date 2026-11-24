#pragma once

#include "buff_ledger.h"
#include "domain.h"

#include <QString>
#include <array>
#include <cstdint>
#include <optional>

namespace rm_terminal {

constexpr std::size_t kTopBarSlotsPerSide = 4;

// 一侧固定 4 个槽位(1、3、4、7),保持左右对称。缺席的槽位是**占位**:online 为假、hp 为空。
// 绝不把「协议没报这台」补成在线或满血 —— 那会让界面谎报一台不存在的机器人。
struct TopBarSlot {
    std::uint32_t id = 0;
    std::uint32_t display_number = 0;
    QString robot_class;
    std::optional<std::uint32_t> hp;
    std::optional<std::uint32_t> max_hp;
    bool online = false;

    bool has_hp() const { return hp.has_value() && max_hp.has_value(); }
};

struct TopBarSide {
    std::optional<std::uint32_t> base_hp;
    std::optional<std::uint32_t> base_max_hp;
    std::optional<std::uint32_t> outpost_hp;
    std::optional<std::uint32_t> outpost_max_hp;
    // 前哨站归谁(打掉重建才换主人,见 outpost_owner),牌子按它上色。
    bool outpost_owned_by_blue = false;
    // 基地防御状态:0 有 100% 防御增益,1 无增益且护甲未展开,2 无增益且护甲展开。
    std::optional<std::uint32_t> base_status;
    // 基地没有 100% 防御时血条下面那一行:「防御 25% · 护甲未展开」。空串时 QML 照旧按 base_status 写。
    QString defense_text;
    std::optional<std::uint32_t> control_points;
    QString team_name;
    QString score;
    std::array<TopBarSlot, kTopBarSlotsPerSide> cards{};

    bool has_base_hp() const { return base_hp.has_value() && base_max_hp.has_value(); }
    bool has_outpost_hp() const {
        return outpost_hp.has_value() && outpost_max_hp.has_value();
    }
};

struct TopBarModel {
    TopBarSide red;
    TopBarSide blue;
    QString clock;
    QString round;
    QString stage;
    bool clock_critical = false;
};

// 纯函数,不碰 Qt Quick,因此可以脱离 GUI 单测。
TopBarModel top_bar_model(const Snapshot& snapshot, const BuffBoard* buffs = nullptr);

}  // namespace rm_terminal
