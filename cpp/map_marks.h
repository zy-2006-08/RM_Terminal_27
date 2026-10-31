#pragma once

#include "domain.h"
#include "field_map.h"

#include <QString>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rm_terminal {

// 指挥手地图标记(协议 2.2.17 MapClickInfo / 2.2.18 MapClickCmd)。字段名照协议:
// mode 是「标记类型」,type 是「标记模式」,两个名字容易弄反,这里用枚举把含义钉住。
enum class MarkKind : std::uint32_t { Attack = 1, Defend = 2, Alert = 3, Custom = 4 };  // -> mode
enum class MarkTarget : std::uint32_t { Map = 1, EnemyRobot = 2 };                    // -> type
enum class MarkScope : std::uint32_t { Specific = 0, ExceptSentry = 1, IncludeSentry = 2 };  // -> is_send_all

struct MarkDraft {
    MarkScope scope = MarkScope::IncludeSentry;
    // 发送范围为「指定客户端」时的收件人,固定 7 个位置,用不到的填 0。
    std::array<std::uint32_t, 7> recipients{};
    MarkKind kind = MarkKind::Attack;
    std::uint32_t ascii = 0;  // 只在 Custom 时有效
    MarkTarget target = MarkTarget::Map;
    std::uint32_t enemy_id = 0;
    double x_m = 0;
    double y_m = 0;
};

// 1.3.1:官方选手端只认 C–L、N、O、Q–Z。
bool valid_mark_letter(char letter);

// 协议规定两次标记间隔不低于 0.5 秒。
constexpr MonotonicMs kMarkIntervalMs = 500;

enum class MarkRejection { None, TooSoon, BadLetter, OffField, NoRecipient };
QString mark_rejection_text(MarkRejection rejection);

// 校验后编码成 MapClickCmd 的字节。被拒时返回原因,不产生字节。
struct MarkEncoding {
    MarkRejection rejection = MarkRejection::None;
    std::string bytes;
};
MarkEncoding encode_map_click(const MarkDraft& draft, const FieldExtent& field);

class MarkGate {
public:
    bool allow(MonotonicMs now);

private:
    std::optional<MonotonicMs> last_;
};

// 屏幕像素 -> 场地坐标(米),和 project() 互逆;点在场地外返回 nullopt。
std::optional<std::pair<double, double>> unproject(const FieldExtent& field, const MapViewport& view,
                                                   double pixel_x, double pixel_y);

// 地图上要画的一个标记:自己发出的和收到的都在这里,age 决定淡出。
struct DrawnMark {
    MarkKind kind = MarkKind::Attack;
    std::uint32_t ascii = 0;
    MarkTarget target = MarkTarget::Map;
    std::uint32_t enemy_id = 0;
    double x_m = 0;
    double y_m = 0;
    bool sent_by_us = false;
    double opacity = 1.0;
};

// 收到的(snapshot.map_marks)和本机发出的标记合在一起,去掉超过 lifetime_ms 的,
// 最后 5 秒线性淡出。
std::vector<DrawnMark> visible_marks(const std::vector<MapMark>& received,
                                     const std::vector<std::pair<MarkDraft, MonotonicMs>>& sent,
                                     MonotonicMs now, MonotonicMs lifetime_ms);

// 标记上写的字:攻 / 防 / 警 / 自定义字母。
QString mark_glyph(MarkKind kind, std::uint32_t ascii);

}  // namespace rm_terminal
