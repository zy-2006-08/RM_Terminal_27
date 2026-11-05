#include "map_pane.h"

#include "presentation.h"
#include "theme.h"

#include <QFont>
#include <QHash>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QRadialGradient>
#include <algorithm>
#include <cmath>

// terminal_map 是静态库,链接器看不到任何指向 qrc_minimap.cpp.o 的符号引用,会把
// 整块目标文件丢掉 —— 结果 :/minimap/* 全部加载失败,地图静默退回线框。
// Q_INIT_RESOURCE 是 Qt 对这种情况的官方解法。它展开成一个 extern 函数声明,所以
// 必须留在全局作用域:放进匿名命名空间会把声明变成内部链接,链接时找不到真符号。
void ensure_minimap_resources() {
    [[maybe_unused]] static const bool once = [] {
        Q_INIT_RESOURCE(minimap);
        return true;
    }();
}

namespace rm_terminal {

namespace {

// 地图底色:效果图里是 rgba(4,8,14,.55) 压在玻璃面板上,这里直接取合成后的颜色画成不透明的,
// 场地图的透明处和信箱边都是这个颜色。
const QColor kBackground(8, 13, 23);
const QColor kFieldLine(150, 180, 230, 41);
const QColor kStatusText(170, 180, 196);
const QColor kAlertText(255, 138, 138);

// Drawn at 40% alpha so a stale marker reads as degraded at a glance while its
// position stays legible; pairing it with the "过期" label keeps the reason
// explicit rather than relying on the operator noticing the transparency.
constexpr int kStaleAlpha = 102;

double value_or(const Field<double>& field, double fallback) {
    return field.value ? *field.value : fallback;
}

bool usable(const Field<double>& field) {
    return field.value.has_value() && field.quality != Quality::Missing &&
           field.freshness != Freshness::NeverReceived;
}

// 场地图按 0.9 不透明度铺在深色底上(效果图是 0.92)。操作手可以在界面设置里调暗。
constexpr double kBackgroundOpacity = 0.9;

// 机器人标记(效果图):直径 22 的圆点,本机 26,白色细边加一圈暗影,中间写编号。
// 标记是给操作手辨认的图元,尺寸恒定,不随面板膨胀。
constexpr double kMarkerDiameter = 22.0;
constexpr double kSelfDiameter = 26.0;

// QPixmap 的构造要求 QGuiApplication 已就位,所以不能是文件级静态对象;函数内
// 静态表在首次绘制时才建,同时保证整个进程只解码一次 PNG。
const QPixmap& asset(const QString& name) {
    ensure_minimap_resources();
    static QHash<QString, QPixmap> cache;
    auto it = cache.find(name);
    if (it == cache.end()) {
        it = cache.insert(name, QPixmap(QStringLiteral(":/minimap/%1").arg(name)));
    }
    return *it;
}

QColor marker_fill(const MapMarker& marker) {
    if (marker.is_self) return QColor(18, 183, 106);
    switch (marker.faction) {
    case 1: return QColor(214, 74, 74);
    case 2: return QColor(47, 127, 216);
    default: return QColor(142, 142, 148);
    }
}

QColor arrow_fill(const MapMarker& marker) {
    if (marker.is_self) return QColor(46, 227, 138);
    return marker.faction == 2 ? QColor(132, 194, 255) : QColor(255, 143, 150);
}

// 空中/哨兵用官方小图标,其余画编号圆点。
QString overlay_asset(const MapMarker& marker) {
    const QString robot_class = robot_class_name(marker.id.value);
    const bool red = marker.faction == 1;
    if (robot_class == QStringLiteral("空中"))
        return red ? QStringLiteral("red_map_airplane.png") : QStringLiteral("blue_map_airplane.png");
    if (robot_class == QStringLiteral("哨兵"))
        return red ? QStringLiteral("red_map_guard.png") : QStringLiteral("blue_map_guard.png");
    return QString();
}

double marker_diameter(const MapMarker& marker) { return marker.is_self ? kSelfDiameter : kMarkerDiameter; }

void draw_marker(QPainter& painter, const MapMarker& marker, QPointF centre, bool low_health) {
    const double d = marker_diameter(marker);
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    if (low_health) {
        QRadialGradient glow(centre, d * 1.1);
        glow.setColorAt(0, QColor(255, 100, 100, 160));
        glow.setColorAt(1, QColor(255, 100, 100, 0));
        painter.setPen(Qt::NoPen);
        painter.setBrush(glow);
        painter.drawEllipse(centre, d * 1.1, d * 1.1);
    }
    const QString overlay = overlay_asset(marker);
    if (!overlay.isEmpty() && !marker.is_self) {
        const QPixmap& icon = asset(overlay);
        if (!icon.isNull()) {
            const QSizeF size = QSizeF(icon.size()).scaled(24, 22, Qt::KeepAspectRatio);
            painter.drawPixmap(QRectF(centre.x() - size.width() / 2, centre.y() - size.height() / 2, size.width(),
                                      size.height()),
                               icon, QRectF(icon.rect()));
            painter.restore();
            return;
        }
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 64));
    painter.drawEllipse(centre + QPointF(0, 2), d / 2 + 2, d / 2 + 2);
    painter.setBrush(QColor(0, 0, 0, 89));
    painter.drawEllipse(centre, d / 2 + 2, d / 2 + 2);
    painter.setBrush(marker_fill(marker));
    painter.setPen(QPen(marker.is_self ? QColor(234, 255, 243) : QColor(255, 255, 255, 235), 1.5));
    painter.drawEllipse(centre, d / 2, d / 2);
    const std::uint32_t number = display_robot_number(marker.id.value);
    if (number >= 1 && number <= 9) {
        painter.setFont(theme::numericPx(marker.is_self ? 14 : 13, QFont::Bold));
        painter.setPen(Qt::white);
        painter.drawText(QRectF(centre.x() - d / 2, centre.y() - d / 2, d, d), Qt::AlignCenter,
                         QString::number(number));
    }
    painter.restore();
}

// 朝向:圆点外侧一个小三角,尖朝车头方向。
void draw_heading(QPainter& painter, const MapMarker& marker, QPointF centre) {
    const double d = marker_diameter(marker);
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.translate(centre);
    painter.rotate(project_yaw_deg(marker.yaw_deg));
    QPainterPath arrow;
    const double tip = d / 2 + 11;
    arrow.moveTo(0, -tip);
    arrow.lineTo(5, -tip + 9);
    arrow.lineTo(-5, -tip + 9);
    arrow.closeSubpath();
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 110));
    painter.drawPath(arrow.translated(0, 1));
    painter.setBrush(arrow_fill(marker));
    painter.drawPath(arrow);
    painter.restore();
}

}  // namespace

MapViewport map_viewport(const FieldExtent& field, int widget_width, int widget_height) {
    return fit_viewport(field, widget_width, std::max(0, widget_height));
}

int map_height_for_width(const FieldExtent& field, int width) {
    if (width <= 0 || field.width_m <= 0.0 || field.height_m <= 0.0) return 0;
    const double field_height = width * (field.height_m / field.width_m);
    return static_cast<int>(std::lround(field_height));
}

int MapPane::heightForWidth(int width) const {
    return map_height_for_width(field_, width);
}

QSize MapPane::sizeHint() const {
    const int width = std::max(minimumWidth(), 360);
    return QSize(width, map_height_for_width(field_, width));
}

QColor faction_color(std::uint32_t faction) {
    switch (faction) {
    case 1: return QColor(214, 74, 74);
    case 2: return QColor(78, 132, 226);
    default: return QColor(142, 142, 148);
    }
}

std::vector<MapMarker> markers_from(const std::vector<MapRobot>& robots) {
    std::vector<MapMarker> markers;
    markers.reserve(robots.size());
    for (const MapRobot& robot : robots) {
        MapMarker marker;
        marker.id = robot.id;
        marker.faction = robot.faction;
        marker.is_self = robot.is_self;
        marker.has_position = usable(robot.position.x) && usable(robot.position.y);
        marker.has_yaw = usable(robot.position.yaw);
        marker.x_m = value_or(robot.position.x, 0.0);
        marker.y_m = value_or(robot.position.y, 0.0);
        marker.yaw_deg = value_or(robot.position.yaw, 0.0);
        marker.highlight = robot.highlight;
        // Only a drawn marker can be stale. Staleness of an undrawn robot is not
        // reported as "aging position", it is reported as missing.
        marker.stale = marker.has_position &&
                       (robot.position.x.freshness == Freshness::Stale ||
                        robot.position.y.freshness == Freshness::Stale);
        markers.push_back(marker);
    }
    std::sort(markers.begin(), markers.end(),
              [](const MapMarker& a, const MapMarker& b) { return a.id < b.id; });
    return markers;
}

QString robot_label(const MapMarker& marker) {
    if (marker.is_self) return QStringLiteral("自机");
    switch (marker.faction) {
    case 1: return QStringLiteral("红方%1号").arg(marker.id.value);
    case 2: return QStringLiteral("蓝方%1号").arg(marker.id.value);
    default: return QStringLiteral("未知%1号").arg(marker.id.value);
    }
}

MapStatus map_status(const std::vector<MapMarker>& markers, const FieldExtent& field) {
    MapStatus status;
    for (const MapMarker& marker : markers) {
        if (!marker.has_position) {
            status.missing.append(robot_label(marker));
            if (marker.is_self) status.self_position_unavailable = true;
            continue;
        }
        const bool inside = marker.x_m >= 0.0 && marker.x_m <= field.width_m &&
                            marker.y_m >= 0.0 && marker.y_m <= field.height_m;
        if (!inside) {
            status.out_of_bounds.append(robot_label(marker));
            continue;
        }
        ++status.received;
    }
    return status;
}

MapPane::MapPane(QWidget* parent) : QWidget(parent), background_opacity_(kBackgroundOpacity) {
    setMinimumSize(160, 86);
    setAutoFillBackground(false);
}

void MapPane::setRobots(const std::vector<MapRobot>& robots) {
    markers_ = markers_from(robots);
    update();
}

void MapPane::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    // 地图框切 6 的角,和别的卡片、行是同一种形状。
    painter.setClipPath(theme::chamferPath(QRectF(rect()), 6));
    painter.fillRect(rect(), kBackground);

    QFont label_font = theme::labelPx(12);
    painter.setFont(label_font);

    const MapViewport view = map_viewport(field_, width(), height());

    if (view.width > 0 && view.height > 0) {
        const QRect field_rect(view.x, view.y, view.width, view.height);
        const QPixmap& background = asset(QStringLiteral("minimap_bg.png"));
        if (background.isNull()) {
            // 资源没编进来时退回线框,而不是留一片纯黑:纯黑和「没收到数据」无法区分。
            painter.setPen(kFieldLine);
            painter.drawRect(view.x, view.y, view.width - 1, view.height - 1);
            const int middle = view.x + view.width / 2;
            painter.drawLine(middle, view.y, middle, view.y + view.height - 1);
        } else {
            painter.save();
            painter.setOpacity(background_opacity_);
            painter.drawPixmap(field_rect, background);
            painter.restore();
        }
    }

    const MapStatus status = map_status(markers_, field_);

    if (markers_.empty()) {
        painter.setPen(kStatusText);
        painter.drawText(QRect(view.x, view.y, view.width, view.height), Qt::AlignCenter,
                         QStringLiteral("无位置数据"));
    }

    // 哨兵规划路径画在机器人下面:它是意图,不是位置。
    if (sentry_path_.size() >= 2 && view.width > 0) {
        QPainterPath path;
        for (std::size_t i = 0; i < sentry_path_.size(); ++i) {
            const Projected point = project(field_, view, sentry_path_[i].first, sentry_path_[i].second);
            if (i == 0) path.moveTo(point.point.x, point.point.y);
            else path.lineTo(point.point.x, point.point.y);
        }
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(theme::kCyan, 2.0, Qt::DashLine));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);
        painter.restore();
    }

    for (const MapMarker& marker : markers_) {
        if (!marker.has_position) continue;
        if (!show_enemies_ && !marker.is_self && marker.faction != 0 && marker.faction != display_faction_) continue;
        const Projected projected = project(field_, view, marker.x_m, marker.y_m);
        // An off-field coordinate is reported in the status band instead: drawing
        // the clamped point would render it as a robot resting on the boundary,
        // which reads as a legitimate tactical position.
        if (!projected.in_bounds) continue;

        const QPointF centre(projected.point.x, projected.point.y);
        painter.save();
        // 过期标记整体半透明,和箭头一起淡出,避免只有圆点变淡而箭头仍然实心。
        if (marker.stale) painter.setOpacity(kStaleAlpha / 255.0);
        const bool low = !marker.is_self && marker.faction != display_faction_ &&
                         std::find(low_health_.begin(), low_health_.end(), marker.id.value) != low_health_.end();
        if (marker.has_yaw && overlay_asset(marker).isEmpty()) draw_heading(painter, marker, centre);
        draw_marker(painter, marker, centre, low);
        painter.restore();

        // 雷达特殊标识:外面套一圈;定位模块离线(2)用虚线。
        if (marker.highlight != 0) {
            painter.save();
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setPen(QPen(theme::kWarn, 2.0, marker.highlight == 2 ? Qt::DashLine : Qt::SolidLine));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(centre, kMarkerDiameter * 0.85, kMarkerDiameter * 0.85);
            painter.restore();
        }

        if (marker.stale) {
            const double radius = marker_diameter(marker) / 2;
            painter.setPen(kAlertText);
            painter.drawText(QPointF(projected.point.x + radius + 4.0,
                                     projected.point.y - radius - 2.0),
                             QStringLiteral("过期"));
        }
    }

    // 地图标记压在机器人上面:标记是指挥手要队友看的东西。点到敌方车的标记画在那台车上。
    for (const DrawnMark& mark : show_marks_ ? marks_ : std::vector<DrawnMark>{}) {
        double x = mark.x_m;
        double y = mark.y_m;
        if (mark.target == MarkTarget::EnemyRobot) {
            const auto robot = std::find_if(markers_.begin(), markers_.end(), [&](const MapMarker& m) {
                return m.id.value == mark.enemy_id && m.has_position;
            });
            if (robot != markers_.end()) {
                x = robot->x_m;
                y = robot->y_m;
            }
        }
        const Projected projected = project(field_, view, x, y);
        if (!projected.in_bounds) continue;
        QColor ink = theme::kTextPrimary;
        if (mark.kind == MarkKind::Attack) ink = theme::kDanger;
        if (mark.kind == MarkKind::Defend) ink = theme::kOk;
        if (mark.kind == MarkKind::Alert) ink = theme::kWarn;
        const QPointF centre(projected.point.x, projected.point.y - kMarkerDiameter * 1.4);
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setOpacity(mark.opacity);
        QPainterPath pin;
        pin.moveTo(centre + QPointF(0, 15));
        pin.lineTo(centre + QPointF(-11, -2));
        pin.lineTo(centre + QPointF(0, -15));
        pin.lineTo(centre + QPointF(11, -2));
        pin.closeSubpath();
        painter.setPen(QPen(ink.darker(150), 1.5));
        painter.setBrush(ink);
        painter.drawPath(pin);
        QFont glyph = painter.font();
        glyph.setPointSize(10);
        glyph.setBold(true);
        painter.setFont(glyph);
        painter.setPen(theme::kVoid);
        painter.drawText(QRectF(centre.x() - 11, centre.y() - 12, 22, 20), Qt::AlignCenter,
                         mark_glyph(mark.kind, mark.ascii));
        if (mark.target == MarkTarget::EnemyRobot) {
            painter.setPen(QPen(ink, 2.0));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(QPointF(projected.point.x, projected.point.y), kMarkerDiameter * 0.9,
                                kMarkerDiameter * 0.9);
        }
        painter.restore();
    }
    painter.setFont(label_font);

    // 自下而上叠在场地上:故障行是例外情况,给它预留常驻高度会在常态下留一条黑缝。
    // 底图是浅色的,所以每行先垫一块半透明暗底,否则告警文字在浅色地形上不可读。
    int line_y = height() - 9;
    const auto line = [&](const QColor& ink, const QString& text) {
        const QRect box = painter.fontMetrics().boundingRect(text).adjusted(-4, -2, 4, 2);
        painter.fillRect(box.translated(8, line_y), QColor(6, 10, 18, 200));
        painter.setPen(ink);
        painter.drawText(QPoint(8, line_y), text);
        line_y -= kStatusLineHeight;
    };

    if (!status.out_of_bounds.isEmpty())
        line(kAlertText, QStringLiteral("越界: %1").arg(
                             status.out_of_bounds.join(QStringLiteral(" / "))));
    if (!status.missing.isEmpty())
        line(kStatusText, QStringLiteral("位置缺失: %1").arg(
                              status.missing.join(QStringLiteral(" / "))));
    if (status.self_position_unavailable)
        line(kAlertText, QStringLiteral("自机位置不可用 · 单机 RobotPosition 未收到"));

    if (base_alert_) {
        // 场地图还是 2026 的,画不准 2027 基地的确切位置,所以闪我方那一半的边框并标在
        // 我方一侧;换成 2027 场地图后再改成在基地处闪。
        const QColor red(255, 100, 100, base_alert_bright_ ? 235 : 100);
        painter.setPen(QPen(red, 3));
        painter.setBrush(QColor(255, 100, 100, base_alert_bright_ ? 28 : 10));
        const int half = view.width / 2;
        const QRect side(base_alert_left_ ? view.x : view.x + half, view.y, half, view.height);
        painter.drawPath(theme::chamferPath(QRectF(side.adjusted(3, 3, -3, -3)), 6));
        painter.setFont(theme::labelPx(18, QFont::Bold));
        const QString text = QStringLiteral("基地被攻击");
        const QRect box = painter.fontMetrics().boundingRect(text).adjusted(-12, -6, 12, 6);
        const QRect tag = box.translated(side.center() - box.center());
        theme::paintGlass(painter, QRectF(tag), 4, false, QColor(70, 14, 22, 230), QColor(255, 100, 100, 150));
        painter.setPen(theme::kDangerText);
        painter.drawText(tag, Qt::AlignCenter, text);
    }
}

void MapPane::setDisplayOptions(double background_opacity, bool show_enemies, bool show_marks,
                                std::uint32_t own_faction) {
    background_opacity_ = std::clamp(background_opacity, 0.2, 1.0);
    show_enemies_ = show_enemies;
    show_marks_ = show_marks;
    display_faction_ = own_faction;
    update();
}

void MapPane::setLowHealth(const std::vector<std::uint32_t>& ids) {
    if (ids == low_health_) return;
    low_health_ = ids;
    update();
}

void MapPane::setMarks(const std::vector<DrawnMark>& marks) {
    marks_ = marks;
    update();
}

void MapPane::setSentryPath(const std::vector<std::pair<double, double>>& points) {
    if (points == sentry_path_) return;
    sentry_path_ = points;
    update();
}

void MapPane::setClickHandler(ClickHandler handler, std::uint32_t own_faction) {
    click_handler_ = std::move(handler);
    own_faction_ = own_faction;
    setCursor(click_handler_ ? Qt::CrossCursor : Qt::ArrowCursor);
}

bool MapPane::clickAt(QPointF pixel) {
    if (!click_handler_) return false;
    const MapViewport view = map_viewport(field_, width(), height());
    const auto field_point = unproject(field_, view, pixel.x(), pixel.y());
    if (!field_point) return false;
    std::optional<std::uint32_t> enemy;
    double best = 22.0;
    for (const MapMarker& marker : markers_) {
        if (!marker.has_position || marker.faction == own_faction_ || marker.faction == 0) continue;
        const Projected projected = project(field_, view, marker.x_m, marker.y_m);
        const double distance = std::hypot(projected.point.x - pixel.x(), projected.point.y - pixel.y());
        if (distance <= best) {
            best = distance;
            enemy = marker.id.value;
        }
    }
    click_handler_(field_point->first, field_point->second, enemy);
    return true;
}

void MapPane::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && clickAt(event->position())) {
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void MapPane::setBaseAlert(bool active, bool our_side_left, bool bright) {
    if (active == base_alert_ && our_side_left == base_alert_left_ && bright == base_alert_bright_)
        return;
    base_alert_ = active;
    base_alert_left_ = our_side_left;
    base_alert_bright_ = bright;
    update();
}

}  // namespace rm_terminal
