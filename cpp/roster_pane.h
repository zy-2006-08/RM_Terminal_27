#pragma once

#include "domain.h"

#include <QString>
#include <QWidget>
#include <optional>
#include <vector>

namespace rm_terminal {

// 一台机器人的生存状态。三态而不是两态:「阵亡」和「服务器没报这台」是完全
// 不同的两件事 —— 前者是可以立刻利用的战机(对方少一台,可以压上去),后者是
// 数据缺失(对方可能满血站在你面前)。把两者都画成 OFFLINE 会让操作手据此做出
// 相反的决策,所以类型层面就把它们分开,不给渲染层「顺手合并」的机会。
enum class LiveState { NoData, Alive, Kia };

// One row in the ally/enemy column: id, faction, and whatever telemetry we
// actually receive for that robot.
//
// `hp` 和 `max_hp` 仍是 optional:全场血量来自 RobotHealthSet,而经济/热量一类
// 只有自机的 RobotDynamicStatus 才有。缺失一律不画,绝不用 0 冒充测量值。
struct RosterEntry {
    RobotId id;
    std::uint32_t faction = 0;
    bool is_self = false;
    bool has_position = false;
    bool position_stale = false;
    LiveState live = LiveState::NoData;
    std::optional<std::uint32_t> hp;
    std::optional<std::uint32_t> max_hp;

    // 官方状态卡还有等级角标、经济、模块灯排。协议里只有自机有 RobotModuleStatus,
    // 等级与经济根本没有字段 —— 所以这两项保持 optional 并在卡上标注,
    // 绝不用占位数字冒充真实 telemetry。
    std::optional<std::uint32_t> coin;
    std::optional<std::uint32_t> modules_online;
    std::optional<std::uint32_t> modules_total;

    // 只有自机会收到 RobotDynamicStatus/RobotTelemetry。缺失字段必须不画,
    // 画 0 会被当成真实测量值。
    std::optional<std::uint32_t> heat;
    std::optional<std::uint32_t> heat_limit;
    std::optional<std::uint32_t> ammo;
    std::optional<double> bullet_speed;
    std::optional<double> chassis_power;
    std::optional<double> buffer_energy;
    std::optional<bool> vision_online;
    std::optional<bool> autoaim_enabled;
    std::optional<bool> target_locked;
    std::optional<bool> fire_permit;
};

// Derived from the snapshot rather than stored: the roster is a view of
// map_robots + robots, and caching it would let the two disagree.
std::vector<RosterEntry> build_roster(const Snapshot& snapshot, std::uint32_t faction);

// 我方阵营,取自协议标记 is_self 的机器人 —— 本校每场可能被编在红方或蓝方。
// 自机缺失时返回 nullopt,调用方只能显示「红方/蓝方」:猜错阵营会让操作手读反全场。
std::optional<std::uint32_t> self_faction(const Snapshot& snapshot);

// A column of robot cards. Draws every row itself: a QLabel per field cannot
// produce the gradient health bar or the faction-tinted card edge, and stacking
// six nested layouts to fake it costs more than paintEvent.
class RosterPane final : public QWidget {
    Q_OBJECT
public:
    // `mirrored` right-aligns the card contents so the enemy column reads
    // outward from the map, the way the reference layout does.
    explicit RosterPane(bool mirrored, QWidget* parent = nullptr);

    void setEntries(const std::vector<RosterEntry>& entries);

    // Observation only, for tests and layout evidence.
    std::size_t rowCount() const { return entries_.size(); }

protected:
    void paintEvent(QPaintEvent* event) override;
    QSize sizeHint() const override;

private:
    void paintRow(QPainter& painter, const RosterEntry& entry, const QRect& box) const;
    void paintHealth(QPainter& painter, const RosterEntry& entry, const QRect& area,
                     const QColor& faction, const QColor& faction_dim) const;

    bool mirrored_;
    std::vector<RosterEntry> entries_;
};

}  // namespace rm_terminal
