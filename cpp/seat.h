#pragma once

#include <QString>
#include <array>
#include <cstdint>
#include <optional>

namespace rm_terminal {

// 终端坐在哪个席位上。2027 协议附录二的选手端只有重装 1、步兵 3/4、空中 6(蓝方 +100);
// 规则手册表 3-1:指挥手操作空中系统和飞镖系统的选手端。哨兵 7 号没有操作手,不能选。
enum class Role { Infantry, Hero, Commander };

class Seat {
public:
    static std::optional<Seat> fromRobotId(int robot_id);
    static const std::array<int, 8>& all();

    std::uint32_t robotId() const { return robot_id_; }
    std::uint32_t faction() const { return robot_id_ > 100 ? 2u : 1u; }
    Role role() const;
    // 「红方 3 号 · 步兵」,状态条和日志里用。
    QString label() const;
    // MQTT 登录名:协议要求填所连机器人的编号,红 3 填 "3",蓝 3 填 "103"。
    QString mqttClientId() const { return QString::number(robot_id_); }

private:
    explicit Seat(std::uint32_t robot_id) : robot_id_(robot_id) {}
    std::uint32_t robot_id_;
};

// 日志和布局 JSON 里的英文名:infantry / hero / commander。
QString role_name(Role role);
// 界面上的中文名:步兵 / 重装 / 指挥手。
QString role_title(Role role);

}  // namespace rm_terminal
