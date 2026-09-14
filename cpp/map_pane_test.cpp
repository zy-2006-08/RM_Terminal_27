#include "map_pane.h"

#include <QApplication>
#include <QImage>
#include <QRect>
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

int exact_color_count(const QImage& image, const QRect& area, const QColor& want) {
    return count_pixels(image, area, [&want](QRgb p) {
        return qRed(p) == want.red() && qGreen(p) == want.green() && qBlue(p) == want.blue();
    });
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

QRect field_region() {
    const MapViewport view = map_viewport(default_field_extent(), kWidth, kHeight);
    return QRect(view.x, view.y, view.width, view.height);
}

QRect status_region() {
    return QRect(0, kHeight - kStatusBandHeight, kWidth, kStatusBandHeight);
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

// (3) Self must be findable at a glance, so its marker carries more ink than an
// identical ally. Compared by local footprint, which survives antialiasing.
void self_is_highlighted_more_than_an_ally() {
    const MapViewport view = map_viewport(default_field_extent(), kWidth, kHeight);
    const Projected at = project(default_field_extent(), view, 8.0, 5.0);
    const QRect around(static_cast<int>(at.point.x) - 30, static_cast<int>(at.point.y) - 30, 60, 60);

    const QImage self_image = render({robot(3, 1, 8.0, 5.0, true)});
    const QImage ally_image = render({robot(3, 1, 8.0, 5.0, false)});
    check(count_drawn(self_image, around) > count_drawn(ally_image, around),
          "the self marker draws more than the same robot as an ally");
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

    const QColor full = faction_color(1);
    const int fresh_solid = exact_color_count(fresh, body, full);
    const int stale_solid = exact_color_count(stale, body, full);
    check(fresh_solid > 0, "a Fresh marker is filled with the undimmed faction colour");
    check(stale_solid < fresh_solid,
          "a Stale marker keeps less undimmed faction colour than a Fresh one");
}

// (5) No data must say so, rather than presenting an empty field as a clear one.
void empty_roster_states_that_there_is_no_data() {
    const MapViewport view = map_viewport(default_field_extent(), kWidth, kHeight);
    const QRect centre(view.x + view.width / 2 - 100, view.y + view.height / 2 - 20, 200, 40);

    const QImage empty = render({});
    // Corner robot keeps the centre clear, so the difference is the notice itself
    // rather than a marker that happened to land in the compared region.
    const QImage corner = render({robot(4, 1, 1.0, 1.0)});
    check(count_drawn(empty, centre) > count_drawn(corner, centre),
          "an empty roster writes a notice in the middle of the field");
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

}  // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    try {
        factions_render_in_their_own_colours();
        missing_position_is_not_drawn_but_is_named();
        self_is_highlighted_more_than_an_ally();
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
