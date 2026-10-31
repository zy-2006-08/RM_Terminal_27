#include "seat.h"

namespace rm_terminal {

std::optional<Seat> Seat::fromRobotId(int robot_id) {
    for (const int legal : all())
        if (legal == robot_id) return Seat(static_cast<std::uint32_t>(robot_id));
    return std::nullopt;
}

const std::array<int, 8>& Seat::all() {
    static const std::array<int, 8> seats{1, 3, 4, 6, 101, 103, 104, 106};
    return seats;
}

Role Seat::role() const {
    switch (robot_id_ > 100 ? robot_id_ - 100 : robot_id_) {
    case 1: return Role::Hero;
    case 6: return Role::Commander;
    default: return Role::Infantry;
    }
}

QString Seat::label() const {
    return QStringLiteral("%1 %2 号 · %3")
        .arg(faction() == 1 ? QStringLiteral("红方") : QStringLiteral("蓝方"))
        .arg(robot_id_ > 100 ? robot_id_ - 100 : robot_id_)
        .arg(role_title(role()));
}

QString role_name(Role role) {
    switch (role) {
    case Role::Infantry: return QStringLiteral("infantry");
    case Role::Hero: return QStringLiteral("hero");
    case Role::Commander: return QStringLiteral("commander");
    }
    return QString();
}

QString role_title(Role role) {
    switch (role) {
    case Role::Infantry: return QStringLiteral("步兵");
    case Role::Hero: return QStringLiteral("重装");
    case Role::Commander: return QStringLiteral("指挥手");
    }
    return QString();
}

}  // namespace rm_terminal
