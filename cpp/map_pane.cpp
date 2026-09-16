#include "map_pane.h"

#include "presentation.h"

#include <QFont>
#include <QHash>
#include <QPaintEvent>
#include <QPainter>
#include <QPixmap>
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

// 开源底图是 0.7 不透明度铺在深色面板上,直接画满会盖掉自绘的越界/过期提示对比度。
constexpr double kBackgroundOpacity = 0.7;

// 底座在屏幕上的目标直径。开源公式含 root.width/270 比例项,在它 270px 的控件上
// 底座约 37px;照抄到我们 880px 的地图会得到 119px 巨块,整条删掉又会缩成小点。
// 所以这里直接声明目标像素:标记是给操作手辨认的图元,尺寸恒定,不随面板膨胀。
constexpr double kMarkerDiameter = 44.0;

// 素材画布尺寸不一(普通底座 48px,自机与 lockbg 96px)。按各自宽度归一到同一目标
// 直径,自机才不会大出一倍 —— 自机靠专属配色区分,不靠体积。
double scale_for(const QPixmap& pixmap, double diameter = kMarkerDiameter) {
    if (pixmap.isNull() || pixmap.width() <= 0) return 1.0;
    return diameter / pixmap.width();
}

// 箭头沿朝向外推的距离,必须大于底座半径,否则箭头落在底座内部并压住编号。
// 开源在 270px 空间里推 10px、底座 37px —— 但它的底座带发光外圈,视觉半径比实心
// 部分大;我们裁掉了光晕,所以按实心半径 + 半个箭头高来定,箭尖正好贴在边缘外。
constexpr double kArrowOffset = kMarkerDiameter * 0.5 + 5.0;

// 箭头素材是 20x22,开源按 1/3 系数画;这里按底座直径折算成同等观感的宽度。
constexpr double kArrowWidth = kMarkerDiameter * 0.42;

// 兵种图标(空中/哨兵)沿用 PNG,按底座直径折算。编号不走 PNG:见 draw_number()。
constexpr double kOverlayWidth = kMarkerDiameter * 0.5;

// 编号字号。map_robot_id_*.png 只有 11x23~16x24,放大到底座尺寸会插值发虚,边缘
// 泛白看着像在发光 —— 那是位图拉伸的产物,不是设计。改用矢量字形绘制:任意尺寸
// 都清晰,且 1 号和 3 号自动等宽。
constexpr double kNumberPointSize = 13.0;

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

void draw_centered(QPainter& painter, const QPixmap& pixmap, QPointF centre, double scale,
                   double rotation_deg = 0.0, QPointF offset = QPointF()) {
    if (pixmap.isNull()) return;
    const double w = pixmap.width() * scale;
    const double h = pixmap.height() * scale;
    painter.save();
    painter.translate(centre);
    if (rotation_deg != 0.0) painter.rotate(rotation_deg);
    painter.translate(offset);
    painter.drawPixmap(QRectF(-w / 2.0, -h / 2.0, w, h), pixmap, QRectF(pixmap.rect()));
    painter.restore();
}

// *_robot.png 是常态底座;*_robot_lockbg.png 是开源专门给「被锁定目标」用的版本,
// 带一圈外发光。之前误用了 lockbg,于是每台车都在发光 —— 那本该是锁定提示。
// *_flat.png 是从开源素材裁掉外发光环后的版本。开源那圈渐隐光晕(实心圆止于
// r=15,外圈还有 alpha 56→1 一直铺到 r=24)在 QML 的小地图里是装饰,但在这里
// 每台车都糊着一团光,既显脏又让相邻单位的光晕叠在一起。原素材保留可回退。
QString base_asset(const MapMarker& marker) {
    if (marker.is_self) return QStringLiteral("self_map_robot_flat.png");
    return marker.faction == 1 ? QStringLiteral("red_map_robot_flat.png")
                               : QStringLiteral("blue_map_robot_flat.png");
}

QString arrow_asset(const MapMarker& marker) {
    if (marker.is_self) return QStringLiteral("self_map_arrow.png");
    return marker.faction == 1 ? QStringLiteral("red_map_arrow.png")
                               : QStringLiteral("blue_map_arrow.png");
}

// 兵种图标沿用我们自己的 robot_class_name(),而不是照抄开源的 key 1..14 分支:
// 两边的编号语义不同(我们敌方是 101..105),照抄会把兵种认错。
QString overlay_asset(const MapMarker& marker) {
    const QString robot_class = robot_class_name(marker.id.value);
    const bool red = marker.faction == 1;
    if (robot_class == QStringLiteral("空中"))
        return red ? QStringLiteral("red_map_airplane.png")
                   : QStringLiteral("blue_map_airplane.png");
    if (robot_class == QStringLiteral("哨兵"))
        return red ? QStringLiteral("red_map_guard.png") : QStringLiteral("blue_map_guard.png");
    return QString();
}

// 编号画在底座正中。白字压深色底座已有足够对比,不描边:描边在 44px 上会糊成一团。
void draw_number(QPainter& painter, const MapMarker& marker, QPointF centre) {
    const std::uint32_t number = display_robot_number(marker.id.value);
    if (number < 1 || number > 5) return;

    QFont font = painter.font();
    font.setPointSizeF(kNumberPointSize);
    font.setBold(true);
    painter.save();
    painter.setFont(font);
    painter.setPen(QColor(245, 246, 250));
    const QRectF box(centre.x() - kMarkerDiameter / 2.0, centre.y() - kMarkerDiameter / 2.0,
                     kMarkerDiameter, kMarkerDiameter);
    painter.drawText(box, Qt::AlignCenter, QString::number(number));
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
            painter.setOpacity(kBackgroundOpacity);
            painter.drawPixmap(field_rect, background);
            painter.restore();
            painter.setPen(kFieldLine);
            painter.drawRect(view.x, view.y, view.width - 1, view.height - 1);
        }
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

        const QPointF centre(projected.point.x, projected.point.y);
        const QPixmap& base = asset(base_asset(marker));

        painter.save();
        // 过期标记整体半透明,和图标一起淡出,避免只有底座变淡而箭头仍然实心。
        if (marker.stale) painter.setOpacity(kStaleAlpha / 255.0);

        if (base.isNull()) {
            QColor color = faction_color(marker.faction);
            const double radius = marker.is_self ? 13.0 : 9.0;
            painter.setBrush(color);
            painter.setPen(QPen(color.darker(160), 1.0));
            painter.drawEllipse(centre, radius, radius);
        } else {
            draw_centered(painter, base, centre, scale_for(base));
            if (marker.has_yaw) {
                // 开源把箭头沿朝向外推 10px 再旋转,所以箭头贴在底座边缘而不是压在
                // 正中心。位移必须发生在旋转之后的坐标系里,否则方向会错。
                const QPixmap& arrow = asset(arrow_asset(marker));
                const double yaw = project_yaw_deg(marker.yaw_deg);
                draw_centered(painter, arrow, centre, scale_for(arrow, kArrowWidth), yaw,
                              QPointF(0.0, -kArrowOffset));
            }
            // 空中/哨兵有专属图形,画图标;其余画编号。两者互斥:同时画会互相压住。
            const QString overlay = overlay_asset(marker);
            if (overlay.isEmpty()) {
                draw_number(painter, marker, centre);
            } else {
                const QPixmap& icon = asset(overlay);
                draw_centered(painter, icon, centre, scale_for(icon, kOverlayWidth));
            }
        }
        painter.restore();

        if (marker.stale) {
            const double radius = marker.is_self ? 13.0 : 9.0;
            painter.setPen(kAlertText);
            painter.drawText(QPointF(projected.point.x + radius + 4.0,
                                     projected.point.y - radius - 2.0),
                             QStringLiteral("过期"));
        }
    }

    // 自下而上叠在场地上:故障行是例外情况,给它预留常驻高度会在常态下留一条黑缝。
    // 底图是浅色的,所以每行先垫一块半透明暗底,否则告警文字在浅色地形上不可读。
    int line_y = height() - 9;
    const auto line = [&](const QColor& ink, const QString& text) {
        const QRect box = painter.fontMetrics().boundingRect(text).adjusted(-4, -2, 4, 2);
        painter.fillRect(box.translated(8, line_y), QColor(12, 12, 14, 190));
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
}

}  // namespace rm_terminal
