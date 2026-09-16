#include "map_pane.h"

#include <QApplication>
#include <QImage>
#include <QPixmap>
#include <QRect>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace rm_terminal;

namespace {

void check(bool condition, const char* name) {
    if (!condition) throw std::runtime_error(name);
    std::cout << "PASS " << name << '\n';
}

constexpr int kWidth = 800;
constexpr int kHeight = 600;
const QColor kBackground(12, 12, 14);

Field<double> valid(double value, Freshness freshness = Freshness::Fresh) {
    Field<double> field;
    field.value = value;
    field.quality = Quality::Valid;
    field.freshness = freshness;
    return field;
}

Field<double> absent() { return Field<double>{}; }

MapRobot robot(std::uint32_t id, std::uint32_t faction, double x, double y,
               bool is_self = false, Freshness freshness = Freshness::Fresh) {
    MapRobot entry;
    entry.id = RobotId{id};
    entry.faction = faction;
    entry.is_self = is_self;
    entry.position.x = valid(x, freshness);
    entry.position.y = valid(y, freshness);
    entry.position.yaw = valid(0.0, freshness);
    return entry;
}

QImage render(const std::vector<MapRobot>& robots) {
    MapPane pane;
    pane.resize(kWidth, kHeight);
    pane.setRobots(robots);
    QImage image(kWidth, kHeight, QImage::Format_ARGB32);
    image.fill(Qt::black);
    pane.render(&image);
    return image;
}

bool is_background(QRgb pixel) {
    return qRed(pixel) == kBackground.red() && qGreen(pixel) == kBackground.green() &&
           qBlue(pixel) == kBackground.blue();
}

// Hue tests rather than exact matches: antialiased marker edges blend toward the
// background, so counting only exact faction colours would miss most of the shape.
bool is_reddish(QRgb p) { return qRed(p) > qGreen(p) + 40 && qRed(p) > qBlue(p) + 40; }
bool is_bluish(QRgb p) { return qBlue(p) > qRed(p) + 40 && qBlue(p) > qGreen(p) + 40; }

template <class Predicate>
int count_pixels(const QImage& image, const QRect& area, Predicate predicate) {
    int total = 0;
    for (int y = area.top(); y <= area.bottom(); ++y) {
        for (int x = area.left(); x <= area.right(); ++x) {
            if (predicate(image.pixel(x, y))) ++total;
        }
    }
    return total;
}

int count_drawn(const QImage& image, const QRect& area) {
    return count_pixels(image, area, [](QRgb p) { return !is_background(p); });
}

// Tight box around a marker centre, sized to hold the arrow (radius <= 13) but to
// EXCLUDE the "过期" label, which is drawn up and to the right at x+radius+4 /
// y-radius-2. Without that exclusion a stale test passes on the label alone and
// never checks whether the marker itself was degraded.
QRect marker_box(double field_x_m, double field_y_m) {
    const MapViewport view = map_viewport(default_field_extent(), kWidth, kHeight);
    const Projected at = project(default_field_extent(), view, field_x_m, field_y_m);
    return QRect(static_cast<int>(at.point.x) - 10, static_cast<int>(at.point.y) - 10, 21, 21);
}

bool regions_identical(const QImage& a, const QImage& b, const QRect& area) {
    for (int y = area.top(); y <= area.bottom(); ++y) {
        for (int x = area.left(); x <= area.right(); ++x) {
            if (a.pixel(x, y) != b.pixel(x, y)) return false;
        }
    }
    return true;
}

// 故障行现在叠在场地上,不再有独立状态带。三种故障(越界 / 缺失 / 自机不可用)
// 自底边向上叠加,所以这条底部条带既是状态区,也必须从场地比对区里排除 ——
// 否则「缺失坐标不画在场地上」会被那行缺失文字判成失败。
constexpr int kStatusSpan = 3 * kStatusLineHeight + 12;

QRect status_region() { return QRect(0, kHeight - kStatusSpan, kWidth, kStatusSpan); }

QRect field_region() {
    const MapViewport view = map_viewport(default_field_extent(), kWidth, kHeight);
    const QRect field(view.x, view.y, view.width, view.height);
    return field.intersected(QRect(0, 0, kWidth, kHeight - kStatusSpan));
}

std::vector<MapRobot> full_roster() {
    return {robot(3, 1, 8.0, 5.0, true), robot(4, 1, 10.0, 9.0), robot(5, 1, 6.0, 11.0),
            robot(103, 2, 20.0, 5.0),    robot(104, 2, 22.0, 9.0), robot(105, 2, 24.0, 11.0)};
}

// (1) Faction colouring must actually reach the canvas for both sides.
void factions_render_in_their_own_colours() {
    const QImage image = render(full_roster());
    const QRect field = field_region();
    check(count_pixels(image, field, is_reddish) > 0, "red faction renders red pixels");
    check(count_pixels(image, field, is_bluish) > 0, "blue faction renders blue pixels");
}

// (2) A missing coordinate must not be drawn at all. Compared over the field
// region only: the status band is REQUIRED to differ, because the robot has to be
// named in the missing list rather than silently vanishing.
void missing_position_is_not_drawn_but_is_named() {
    std::vector<MapRobot> with_missing = full_roster();
    with_missing[1].position.x = absent();

    std::vector<MapRobot> without;
    for (const MapRobot& entry : full_roster()) {
        if (entry.id.value != 4) without.push_back(entry);
    }

    const QImage missing_image = render(with_missing);
    const QImage absent_image = render(without);
    check(regions_identical(missing_image, absent_image, field_region()),
          "a Missing coordinate draws nothing on the field");
    check(!regions_identical(missing_image, absent_image, status_region()),
          "the missing robot still changes the status band");

    const MapStatus status = map_status(markers_from(with_missing), default_field_extent());
    check(status.missing.contains(QStringLiteral("红方4号")),
          "the missing robot is named in the missing list");
    check(status.received == 5, "the received count excludes the missing robot");
}

// Marker size is the thing that kept regressing by eye: the open-source formula
// carries a width/270 ratio term that yields a ~119px blob if copied verbatim onto
// our 880px map, and ~16px specks if the term is simply deleted. Both shipped once.
// So the on-screen diameter is asserted here rather than judged from a screenshot.
// All three factions must land on the same size despite red/blue using a 48px
// canvas and self using 96px.
void markers_render_at_a_legible_size() {
    struct Case {
        const char* what;
        std::vector<MapRobot> roster;
    };
    const std::vector<Case> cases{
        {"red ally", {robot(3, 1, 14.0, 7.5)}},
        {"blue enemy", {robot(103, 2, 14.0, 7.5)}},
        {"self", {robot(3, 1, 14.0, 7.5, true)}},
    };

    const MapViewport view = map_viewport(default_field_extent(), kWidth, kHeight);
    const Projected at = project(default_field_extent(), view, 14.0, 7.5);
    const QRect around(static_cast<int>(at.point.x) - 60, static_cast<int>(at.point.y) - 60, 120,
                       120);

    // 底图本身有地形纹理,is_background() 判不出「这里没有标记」。所以尺寸靠
    // 有标记/无标记两帧的差分求得:只有标记覆盖过的像素才会变化。
    const QImage bare = render({});
    for (const Case& entry : cases) {
        const QImage image = render(entry.roster);
        int min_x = around.right(), max_x = around.left();
        int min_y = around.bottom(), max_y = around.top();
        for (int y = around.top(); y <= around.bottom(); ++y) {
            for (int x = around.left(); x <= around.right(); ++x) {
                if (image.pixel(x, y) == bare.pixel(x, y)) continue;
                min_x = std::min(min_x, x);
                max_x = std::max(max_x, x);
                min_y = std::min(min_y, y);
                max_y = std::max(max_y, y);
            }
        }
        const int drawn_width = max_x - min_x + 1;
        // 宽度含箭头沿朝向外推的位移,所以比底座直径大;高度是去掉光晕后的实心
        // 底座本身。区间下限挡「缩成小点」,上限挡「119px 巨块」,两者都发生过。
        check(drawn_width >= 40 && drawn_width <= 68, entry.what);
        check(max_y - min_y + 1 >= 34, "the marker is as tall as it is wide, not a sliver");
    }
}

// Every robot must be identifiable, so the number/class badge has to actually reach
// the canvas on top of the base. Regression: the badge was scaled by the base's own
// factor, which rendered it wide enough to smear over the whole base and stop
// reading as a digit.
void markers_carry_a_visible_badge() {
    const MapViewport view = map_viewport(default_field_extent(), kWidth, kHeight);
    const Projected at = project(default_field_extent(), view, 14.0, 7.5);
    const QRect core(static_cast<int>(at.point.x) - 12, static_cast<int>(at.point.y) - 12, 25, 25);

    // 1..3 carry a digit badge; 4 is 空中 and 5 is 哨兵, which carry a class icon.
    for (std::uint32_t id : {1u, 2u, 3u, 4u, 5u}) {
        const QImage with_badge = render({robot(id, 1, 14.0, 7.5)});
        check(count_drawn(with_badge, core) > 0, "the marker centre carries a badge");
    }

    // The badge must differ between two robots that share a base, otherwise the
    // digits are not being drawn and every ally looks identical.
    const QImage one = render({robot(1, 1, 14.0, 7.5)});
    const QImage three = render({robot(3, 1, 14.0, 7.5)});
    check(!regions_identical(one, three, core),
          "robot 1 and robot 3 draw different badges on the same base");
}

// (3) Self must be findable at a glance, so its marker cannot render identically
// to an ally sitting at the same spot.
void self_is_distinct_from_an_ally() {
    const MapViewport view = map_viewport(default_field_extent(), kWidth, kHeight);
    const Projected at = project(default_field_extent(), view, 8.0, 5.0);
    const QRect around(static_cast<int>(at.point.x) - 30, static_cast<int>(at.point.y) - 30, 60, 60);

    const QImage self_image = render({robot(3, 1, 8.0, 5.0, true)});
    const QImage ally_image = render({robot(3, 1, 8.0, 5.0, false)});
    // 复刻开源后自机靠专用图标(self_map_robot.png)区分,不再靠额外画一圈黄环,
    // 而该图标比友军底座还略小 —— 所以断的是「看起来不一样」,不是「画得更多」。
    check(!regions_identical(self_image, ally_image, around),
          "the self marker is visually distinct from the same robot as an ally");
    check(count_drawn(self_image, around) > 0, "the self marker is actually drawn");
}

// (4) Stale data must look degraded, not current. Asserted on the marker body
// itself, not merely on the whole field region: the "过期" label alone makes the
// field differ, so a region-wide comparison passes even when the marker is drawn
// at full strength. Fault injection caught exactly that.
void stale_position_renders_differently_from_valid() {
    const QImage fresh = render({robot(4, 1, 10.0, 9.0, false, Freshness::Fresh)});
    const QImage stale = render({robot(4, 1, 10.0, 9.0, false, Freshness::Stale)});

    const QRect body = marker_box(10.0, 9.0);
    check(!regions_identical(fresh, stale, body),
          "a Stale marker body renders differently from a Fresh one");

    // 标记改成 PNG 合成后,画布上不再出现 faction_color() 那个精确 RGB,原来数
    // 精确色像素的写法只能测到自绘时代的实现细节。真正要守的性质是「过期更淡」,
    // 所以改成比较相对底图的偏离量:淡出后每个像素都更接近底图,总偏离必然更小。
    const auto ink_weight = [](const QImage& image, const QRect& area) {
        long long total = 0;
        for (int y = area.top(); y <= area.bottom(); ++y) {
            for (int x = area.left(); x <= area.right(); ++x) {
                const QRgb p = image.pixel(x, y);
                total += std::abs(qRed(p) - kBackground.red()) +
                         std::abs(qGreen(p) - kBackground.green()) +
                         std::abs(qBlue(p) - kBackground.blue());
            }
        }
        return total;
    };
    const long long fresh_ink = ink_weight(fresh, body);
    const long long stale_ink = ink_weight(stale, body);
    check(fresh_ink > 0, "a Fresh marker actually puts ink on the field");
    check(stale_ink < fresh_ink, "a Stale marker is drawn fainter than a Fresh one");
}

// (5) No data must say so, rather than presenting an empty field as a clear one.
void empty_roster_states_that_there_is_no_data() {
    const MapViewport view = map_viewport(default_field_extent(), kWidth, kHeight);
    const QRect centre(view.x + view.width / 2 - 100, view.y + view.height / 2 - 20, 200, 40);

    const QImage empty = render({});
    // Corner robot keeps the centre clear, so the difference is the notice itself
    // rather than a marker that happened to land in the compared region.
    const QImage corner = render({robot(4, 1, 1.0, 1.0)});
    // 底图铺满后场地任何位置都有像素,数「非背景像素」两边都是满值,分不出差别。
    // 提示文字的判据改为同一块区域在两种 roster 下是否不同 —— 有文字才会不同。
    check(!regions_identical(empty, corner, centre),
          "an empty roster writes a notice in the middle of the field");
    check(count_drawn(empty, centre) > 0, "the notice actually puts ink on the field");
}

// (6) An off-field coordinate is clamped by project(), so drawing it would place a
// robot on the boundary as if that were a real position.
void out_of_bounds_is_listed_instead_of_drawn() {
    std::vector<MapRobot> roster = full_roster();
    roster[1].position.x = valid(40.0);

    std::vector<MapRobot> without;
    for (const MapRobot& entry : full_roster()) {
        if (entry.id.value != 4) without.push_back(entry);
    }

    const QImage image = render(roster);
    check(regions_identical(image, render(without), field_region()),
          "an out-of-bounds robot draws no arrow on the field");

    const MapStatus status = map_status(markers_from(roster), default_field_extent());
    check(status.out_of_bounds.contains(QStringLiteral("红方4号")),
          "the out-of-bounds robot is named");
    check(!status.missing.contains(QStringLiteral("红方4号")),
          "out-of-bounds is reported as out-of-bounds, not as missing");
    check(status.received == 5, "the received count excludes the out-of-bounds robot");
}

// (7) Self is the most important entity on the map; its absence must be loud.
void missing_self_is_called_out_specifically() {
    std::vector<MapRobot> roster = full_roster();
    roster[0].position.x = absent();

    const MapStatus status = map_status(markers_from(roster), default_field_extent());
    check(status.self_position_unavailable, "missing self position raises its own flag");
    check(status.missing.contains(QStringLiteral("自机")), "the self entry is named as 自机");

    const QRect band = status_region();
    check(count_drawn(render(roster), band) > count_drawn(render(full_roster()), band),
          "the self-unavailable warning adds text to the status band");

    // Counted by alert colour, not by total ink: the grey "位置缺失: 自机" line
    // alone satisfies an ink-count assertion, so deleting the red self warning
    // outright still passed. This roster has no out-of-bounds robot, so alert
    // pixels in the band can only come from the self-unavailable warning.
    const int alert_pixels = count_pixels(render(roster), band, [](QRgb p) {
        return qRed(p) > qGreen(p) + 40 && qRed(p) > qBlue(p) + 40;
    });
    check(alert_pixels > 0, "the self-unavailable warning is drawn in the alert colour");
}

// Ordering must be stable so the map does not reshuffle between ticks.
void markers_are_sorted_by_id() {
    const std::vector<MapMarker> markers = markers_from(
        {robot(105, 2, 24.0, 11.0), robot(3, 1, 8.0, 5.0, true), robot(4, 1, 10.0, 9.0)});
    check(markers.size() == 3, "every robot is kept");
    check(markers[0].id.value == 3 && markers[1].id.value == 4 && markers[2].id.value == 105,
          "markers are ordered by ascending id");
}

// 素材没编进二进制时,paintEvent 会静默退回线框,而上面每条断言依旧全绿 ——
// 等于把「复刻开源观感」这件事悄悄丢掉。所以资源存在性必须单独断言。
void minimap_assets_are_compiled_into_the_binary() {
    const char* required[] = {
        ":/minimap/minimap_bg.png",            ":/minimap/red_map_robot_flat.png",
        ":/minimap/blue_map_robot_flat.png",   ":/minimap/self_map_robot_flat.png",
        ":/minimap/red_map_robot.png",
        ":/minimap/blue_map_robot.png",        ":/minimap/red_map_robot_lockbg.png",
        ":/minimap/blue_map_robot_lockbg.png", ":/minimap/self_map_robot.png",
        ":/minimap/red_map_arrow.png",       ":/minimap/blue_map_arrow.png",
        ":/minimap/self_map_arrow.png",      ":/minimap/map_robot_id_1.png",
        ":/minimap/map_robot_id_5.png",      ":/minimap/red_map_guard.png",
        ":/minimap/blue_map_guard.png",      ":/minimap/red_map_airplane.png",
        ":/minimap/blue_map_airplane.png",
    };
    for (const char* path : required) {
        const QPixmap pixmap(QString::fromLatin1(path));
        check(!pixmap.isNull(), path);
    }
}

// 底图必须真的落在场地区域内。只断言「有非背景像素」不够:状态栏文字也是非背景,
// 空roster 的提示文字同样是,两者都能让一个假的底图测试通过。
void field_background_image_covers_the_field() {
    const QImage empty_field = render({});
    const QRect field = field_region();
    const QRect inner(field.x() + field.width() / 4, field.y() + field.height() / 4,
                      field.width() / 2, field.height() / 8);
    // 取场地上四分之一的一条窄带,避开正中央的「无位置数据」文字。
    check(count_drawn(empty_field, inner) > inner.width(),
          "the field background image fills the field area even with no robots");
}

}  // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    try {
        minimap_assets_are_compiled_into_the_binary();
        field_background_image_covers_the_field();
        factions_render_in_their_own_colours();
        markers_render_at_a_legible_size();
        markers_carry_a_visible_badge();
        missing_position_is_not_drawn_but_is_named();
        self_is_distinct_from_an_ally();
        stale_position_renders_differently_from_valid();
        empty_roster_states_that_there_is_no_data();
        out_of_bounds_is_listed_instead_of_drawn();
        missing_self_is_called_out_specifically();
        markers_are_sorted_by_id();
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    std::cout << "map_pane: all checks passed\n";
    return 0;
}
