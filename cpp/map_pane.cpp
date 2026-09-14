#include "map_pane.h"

#include <QFont>
#include <QPaintEvent>
#include <QPainter>
#include <QPolygonF>
#include <algorithm>

namespace rm_terminal {

namespace {

const QColor kBackground(12, 12, 14);
const QColor kFieldLine(70, 74, 82);
const QColor kStatusText(170, 172, 178);
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

}  // namespace

MapViewport map_viewport(const FieldExtent& field, int widget_width, int widget_height) {
    return fit_viewport(field, widget_width, std::max(0, widget_height - kStatusBandHeight));
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

MapPane::MapPane(QWidget* parent) : QWidget(parent) {
    setMinimumSize(360, 260);
    setAutoFillBackground(false);
}

void MapPane::setRobots(const std::vector<MapRobot>& robots) {
    markers_ = markers_from(robots);
    update();
}

void MapPane::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), kBackground);

    QFont label_font = painter.font();
    label_font.setPointSize(9);
    painter.setFont(label_font);

    const MapViewport view = map_viewport(field_, width(), height());

    if (view.width > 0 && view.height > 0) {
        painter.setPen(kFieldLine);
        painter.drawRect(view.x, view.y, view.width - 1, view.height - 1);
        const int middle = view.x + view.width / 2;
        painter.drawLine(middle, view.y, middle, view.y + view.height - 1);
    }

    const MapStatus status = map_status(markers_, field_);

    if (markers_.empty()) {
        painter.setPen(kStatusText);
        painter.drawText(QRect(view.x, view.y, view.width, view.height), Qt::AlignCenter,
                         QStringLiteral("无位置数据"));
    }

    for (const MapMarker& marker : markers_) {
        if (!marker.has_position) continue;
        const Projected projected = project(field_, view, marker.x_m, marker.y_m);
        // An off-field coordinate is reported in the status band instead: drawing
        // the clamped point would render it as a robot resting on the boundary,
        // which reads as a legitimate tactical position.
        if (!projected.in_bounds) continue;

        QColor color = faction_color(marker.faction);
        if (marker.stale) color.setAlpha(kStaleAlpha);

        const double radius = marker.is_self ? 13.0 : 9.0;
        QPolygonF arrow;
        arrow << QPointF(radius, 0.0) << QPointF(-radius * 0.7, radius * 0.65)
              << QPointF(-radius * 0.7, -radius * 0.65);

        painter.save();
        painter.translate(projected.point.x, projected.point.y);
        painter.rotate(marker.has_yaw ? project_yaw_deg(marker.yaw_deg) : 0.0);
        painter.setBrush(color);
        QColor outline = marker.is_self ? QColor(255, 244, 170) : color.darker(160);
        if (marker.stale) outline.setAlpha(kStaleAlpha);
        painter.setPen(QPen(outline, marker.is_self ? 3.0 : 1.0));
        painter.drawPolygon(arrow);
        painter.restore();

        if (marker.is_self) {
            QColor ring(255, 244, 170);
            if (marker.stale) ring.setAlpha(kStaleAlpha);
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(ring, 2.0));
            const double ring_radius = radius + 6.0;
            painter.drawEllipse(QPointF(projected.point.x, projected.point.y), ring_radius,
                                ring_radius);
        }

        if (marker.stale) {
            painter.setPen(kAlertText);
            painter.drawText(QPointF(projected.point.x + radius + 4.0,
                                     projected.point.y - radius - 2.0),
                             QStringLiteral("过期"));
        }
    }

    int line_y = height() - kStatusBandHeight + 14;
    painter.setPen(kStatusText);
    painter.drawText(
        QPoint(8, line_y),
        QStringLiteral("已收到 %1 台 / 预期 %2 台 · 场地 %3×%4m（本地假设）")
            .arg(status.received)
            .arg(kExpectedRobotCount)
            .arg(field_.width_m)
            .arg(field_.height_m));

    if (status.self_position_unavailable) {
        line_y += 16;
        painter.setPen(kAlertText);
        painter.drawText(QPoint(8, line_y),
                         QStringLiteral("自机位置不可用 · 单机 RobotPosition 未收到"));
    }
    if (!status.missing.isEmpty()) {
        line_y += 16;
        painter.setPen(kStatusText);
        painter.drawText(QPoint(8, line_y),
                         QStringLiteral("位置缺失: %1").arg(status.missing.join(
                             QStringLiteral(" / "))));
    }
    if (!status.out_of_bounds.isEmpty()) {
        line_y += 16;
        painter.setPen(kAlertText);
        painter.drawText(QPoint(8, line_y),
                         QStringLiteral("越界: %1").arg(status.out_of_bounds.join(
                             QStringLiteral(" / "))));
    }
}

}  // namespace rm_terminal
