#include "map_marks.h"

#include "rm_terminal.pb.h"

#include <algorithm>
#include <cmath>

namespace rm_terminal {

bool valid_mark_letter(char letter) {
    return (letter >= 'C' && letter <= 'L') || letter == 'N' || letter == 'O' ||
           (letter >= 'Q' && letter <= 'Z');
}

QString mark_rejection_text(MarkRejection rejection) {
    switch (rejection) {
    case MarkRejection::None: return QString();
    case MarkRejection::TooSoon: return QStringLiteral("两次标记至少隔 0.5 秒");
    case MarkRejection::BadLetter: return QStringLiteral("自定义字母只能用 C–L、N、O、Q–Z");
    case MarkRejection::OffField: return QStringLiteral("点在场地外");
    case MarkRejection::NoRecipient: return QStringLiteral("指定发送时至少选一台车");
    }
    return QString();
}

MarkEncoding encode_map_click(const MarkDraft& draft, const FieldExtent& field) {
    MarkEncoding out;
    if (draft.kind == MarkKind::Custom &&
        (draft.ascii > 127 || !valid_mark_letter(static_cast<char>(draft.ascii)))) {
        out.rejection = MarkRejection::BadLetter;
        return out;
    }
    if (!std::isfinite(draft.x_m) || !std::isfinite(draft.y_m) || draft.x_m < 0 ||
        draft.y_m < 0 || draft.x_m > field.width_m || draft.y_m > field.height_m) {
        out.rejection = MarkRejection::OffField;
        return out;
    }
    const bool any_recipient = std::any_of(draft.recipients.begin(), draft.recipients.end(),
                                           [](std::uint32_t id) { return id != 0; });
    if (draft.scope == MarkScope::Specific && !any_recipient) {
        out.rejection = MarkRejection::NoRecipient;
        return out;
    }
    rm::MapClickCmd message;
    message.set_is_send_all(static_cast<std::uint32_t>(draft.scope));
    std::string ids(7, '\0');
    for (std::size_t i = 0; i < ids.size(); ++i)
        ids[i] = static_cast<char>(static_cast<std::uint8_t>(draft.recipients[i]));
    message.set_robot_id(ids);
    message.set_mode(static_cast<std::uint32_t>(draft.kind));
    message.set_enemy_id(draft.target == MarkTarget::EnemyRobot ? draft.enemy_id : 0);
    message.set_ascii(draft.kind == MarkKind::Custom ? draft.ascii : 0);
    message.set_type(static_cast<std::uint32_t>(draft.target));
    message.set_map_x(static_cast<float>(draft.x_m));
    message.set_map_y(static_cast<float>(draft.y_m));
    out.bytes = message.SerializeAsString();
    return out;
}

bool MarkGate::allow(MonotonicMs now) {
    if (last_ && now - *last_ < kMarkIntervalMs) return false;
    last_ = now;
    return true;
}

std::optional<std::pair<double, double>> unproject(const FieldExtent& field, const MapViewport& view,
                                                   double pixel_x, double pixel_y) {
    if (view.width <= 0 || view.height <= 0) return std::nullopt;
    const double fx = (pixel_x - view.x) / view.width;
    const double fy = 1.0 - (pixel_y - view.y) / view.height;
    if (fx < 0.0 || fx > 1.0 || fy < 0.0 || fy > 1.0) return std::nullopt;
    return std::make_pair(fx * field.width_m, fy * field.height_m);
}

std::vector<DrawnMark> visible_marks(const std::vector<MapMark>& received,
                                     const std::vector<std::pair<MarkDraft, MonotonicMs>>& sent,
                                     MonotonicMs now, MonotonicMs lifetime_ms) {
    constexpr MonotonicMs kFadeMs = 5000;
    const auto opacity = [&](MonotonicMs at) -> std::optional<double> {
        const MonotonicMs age = now - at;
        if (age < 0 || age >= lifetime_ms) return std::nullopt;
        const MonotonicMs left = lifetime_ms - age;
        return left >= kFadeMs ? 1.0 : static_cast<double>(left) / kFadeMs;
    };
    std::vector<DrawnMark> marks;
    for (const MapMark& mark : received) {
        const auto alpha = opacity(mark.received_at);
        if (!alpha) continue;
        DrawnMark drawn;
        drawn.kind = mark.mode >= 1 && mark.mode <= 4 ? static_cast<MarkKind>(mark.mode) : MarkKind::Alert;
        drawn.ascii = mark.ascii;
        drawn.target = mark.type == 2 ? MarkTarget::EnemyRobot : MarkTarget::Map;
        drawn.enemy_id = mark.enemy_id;
        drawn.x_m = mark.x;
        drawn.y_m = mark.y;
        drawn.opacity = *alpha;
        marks.push_back(drawn);
    }
    for (const auto& [draft, at] : sent) {
        const auto alpha = opacity(at);
        if (!alpha) continue;
        DrawnMark drawn;
        drawn.kind = draft.kind;
        drawn.ascii = draft.ascii;
        drawn.target = draft.target;
        drawn.enemy_id = draft.enemy_id;
        drawn.x_m = draft.x_m;
        drawn.y_m = draft.y_m;
        drawn.sent_by_us = true;
        drawn.opacity = *alpha;
        marks.push_back(drawn);
    }
    return marks;
}

QString mark_glyph(MarkKind kind, std::uint32_t ascii) {
    switch (kind) {
    case MarkKind::Attack: return QStringLiteral("攻");
    case MarkKind::Defend: return QStringLiteral("防");
    case MarkKind::Alert: return QStringLiteral("警");
    case MarkKind::Custom:
        return ascii > 0 && ascii < 128 ? QString(QChar(static_cast<char16_t>(ascii)))
                                        : QStringLiteral("?");
    }
    return QStringLiteral("?");
}

}  // namespace rm_terminal
