#include "field_map.h"
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace rm_terminal;

namespace {

void check(bool condition, const char* name) {
    if (!condition) throw std::runtime_error(name);
    std::cout << "PASS " << name << '\n';
}

bool near(double a, double b, double tolerance = 0.001) {
    return std::fabs(a - b) <= tolerance;
}

// A square widget over the default 28x15 field: letterboxed, so the drawn area
// is narrower in height than the widget and vertically centred.
void fit_letterboxes_and_centres() {
    const FieldExtent field = default_field_extent();
    const MapViewport v = fit_viewport(field, 1000, 1000);

    check(v.width == 1000, "wide-limited fit uses the full widget width");
    const double expected_height = 1000.0 * (field.height_m / field.width_m);
    check(near(v.height, expected_height, 1.0), "height follows the field aspect ratio");
    check(v.x == 0, "no horizontal padding when width is the limiting axis");
    check(near(v.y, (1000 - v.height) / 2.0, 1.0), "vertical padding is centred");
}

// The other limiting axis: a very wide widget must pillarbox with symmetric
// left/right padding rather than stretching the field.
void fit_pillarboxes_symmetrically() {
    const FieldExtent field = default_field_extent();
    const MapViewport v = fit_viewport(field, 2000, 500);

    check(v.height == 500, "height-limited fit uses the full widget height");
    const double expected_width = 500.0 * (field.width_m / field.height_m);
    check(near(v.width, expected_width, 1.0), "width follows the field aspect ratio");
    const int right_padding = 2000 - (v.x + v.width);
    check(std::abs(v.x - right_padding) <= 1, "left and right padding are symmetric");
}

void aspect_ratio_is_preserved() {
    const FieldExtent field = default_field_extent();
    for (const auto& size : {std::pair<int, int>{800, 600}, {1920, 1080}, {640, 480}, {300, 900}}) {
        const MapViewport v = fit_viewport(field, size.first, size.second);
        const double field_aspect = field.width_m / field.height_m;
        const double view_aspect = static_cast<double>(v.width) / static_cast<double>(v.height);
        check(near(field_aspect, view_aspect, 0.02), "aspect preserved across widget sizes");
        check(v.x >= 0 && v.y >= 0, "viewport stays inside the widget");
        check(v.x + v.width <= size.first && v.y + v.height <= size.second,
              "viewport never overflows the widget");
    }
}

// Screen Y grows downward while field Y grows upward. Without a flip the whole
// map is upside down and every tactical read is mirrored.
void y_axis_is_flipped() {
    const FieldExtent field = default_field_extent();
    const MapViewport v = fit_viewport(field, 1400, 750);

    const Projected origin = project(field, v, 0.0, 0.0);
    check(near(origin.point.y, v.y + v.height, 1.0), "field y=0 lands at the viewport BOTTOM");
    check(origin.in_bounds, "field origin is in bounds");

    const Projected top = project(field, v, 0.0, field.height_m);
    check(near(top.point.y, v.y, 1.0), "field y=max lands at the viewport TOP");
    check(top.point.y < origin.point.y, "increasing field y moves UP the screen");
}

void corners_map_to_corners() {
    const FieldExtent field = default_field_extent();
    const MapViewport v = fit_viewport(field, 1400, 750);

    const Projected bottom_left = project(field, v, 0.0, 0.0);
    check(near(bottom_left.point.x, v.x, 1.0) && near(bottom_left.point.y, v.y + v.height, 1.0),
          "field (0,0) -> viewport bottom-left");

    const Projected bottom_right = project(field, v, field.width_m, 0.0);
    check(near(bottom_right.point.x, v.x + v.width, 1.0), "field (max,0) -> viewport bottom-right");

    const Projected top_right = project(field, v, field.width_m, field.height_m);
    check(near(top_right.point.x, v.x + v.width, 1.0) && near(top_right.point.y, v.y, 1.0),
          "field (max,max) -> viewport top-right");
}

void centre_maps_to_centre() {
    const FieldExtent field = default_field_extent();
    const MapViewport v = fit_viewport(field, 1400, 750);
    const Projected mid = project(field, v, field.width_m / 2.0, field.height_m / 2.0);
    check(near(mid.point.x, v.x + v.width / 2.0, 1.0), "field centre x -> viewport centre x");
    check(near(mid.point.y, v.y + v.height / 2.0, 1.0), "field centre y -> viewport centre y");
}

// Clamping keeps the paint inside the widget, but a clamped point drawn as a
// normal marker would show an off-field robot sitting plausibly on the boundary.
// in_bounds is what stops the map from lying about it.
void out_of_bounds_is_reported_and_clamped() {
    const FieldExtent field = default_field_extent();
    const MapViewport v = fit_viewport(field, 1400, 750);

    const Projected past_right = project(field, v, field.width_m + 5.0, 7.5);
    check(!past_right.in_bounds, "x beyond the field reports out of bounds");
    check(past_right.point.x <= v.x + v.width + 0.001, "x is clamped into the viewport");

    const Projected negative = project(field, v, -3.0, -2.0);
    check(!negative.in_bounds, "negative coordinates report out of bounds");
    check(negative.point.x >= v.x - 0.001 && negative.point.y <= v.y + v.height + 0.001,
          "negative coordinates are clamped into the viewport");

    check(project(field, v, 0.0, 0.0).in_bounds, "exact lower corner counts as in bounds");
    check(project(field, v, field.width_m, field.height_m).in_bounds,
          "exact upper corner counts as in bounds");
    check(project(field, v, 14.0, 7.5).in_bounds, "a mid-field point is in bounds");
}

// The sim's actual publish envelope, so a legitimate robot is never flagged.
void simulator_range_is_fully_in_bounds() {
    const FieldExtent field = default_field_extent();
    const MapViewport v = fit_viewport(field, 1400, 750);
    for (int i = 0; i < 360; ++i) {
        const double t = static_cast<double>(i);
        const double x = 14.0 + 5.5 * std::sin(t * 0.2);
        const double y = 7.5 + 3.5 * std::cos(t * 0.17);
        if (!project(field, v, x, y).in_bounds) throw std::runtime_error("simulator range in bounds");
    }
    std::cout << "PASS simulator range in bounds\n";
}

// Proves the extent is a real parameter, not decoration. If the field size were
// silently hardcoded elsewhere, every robot would be offset and the map would
// still look tidy - a silent failure worse than an error.
void extent_actually_parameterises_projection() {
    const MapViewport v{0, 0, 1000, 1000};
    const FieldExtent small{14.0, 15.0};
    const FieldExtent standard = default_field_extent();
    const Projected a = project(small, v, 7.0, 7.5);
    const Projected b = project(standard, v, 7.0, 7.5);
    check(!near(a.point.x, b.point.x, 1.0), "changing extent changes the projection");
}

void degenerate_sizes_do_not_divide_by_zero() {
    const FieldExtent field = default_field_extent();
    for (const auto& size : {std::pair<int, int>{0, 500}, {500, 0}, {0, 0}, {-10, -10}}) {
        const MapViewport v = fit_viewport(field, size.first, size.second);
        check(v.width >= 0 && v.height >= 0, "degenerate widget yields a non-negative viewport");
        const Projected p = project(field, v, 14.0, 7.5);
        check(std::isfinite(p.point.x) && std::isfinite(p.point.y),
              "degenerate viewport still projects to finite values");
    }
    const FieldExtent zero_field{0.0, 0.0};
    const MapViewport v = fit_viewport(zero_field, 800, 600);
    const Projected p = project(zero_field, v, 1.0, 1.0);
    check(std::isfinite(p.point.x) && std::isfinite(p.point.y),
          "zero-sized field does not produce NaN or inf");
}

void yaw_conversion_is_finite_and_wraps() {
    check(std::isfinite(project_yaw_deg(0.0)), "yaw 0 is finite");
    for (double yaw : {0.0, 90.0, 180.0, -90.0, 360.0, 720.0, -450.0}) {
        const double screen = project_yaw_deg(yaw);
        check(std::isfinite(screen), "yaw conversion stays finite");
        check(screen >= 0.0 && screen < 360.0, "yaw normalises into [0,360)");
    }
    check(near(project_yaw_deg(0.0), project_yaw_deg(360.0)),
          "yaw wraps: 0 and 360 agree");
    check(near(project_yaw_deg(10.0), project_yaw_deg(-350.0)),
          "yaw wraps: negative input normalises");
}

}  // namespace

int main() {
    try {
        fit_letterboxes_and_centres();
        fit_pillarboxes_symmetrically();
        aspect_ratio_is_preserved();
        y_axis_is_flipped();
        corners_map_to_corners();
        centre_maps_to_centre();
        out_of_bounds_is_reported_and_clamped();
        simulator_range_is_fully_in_bounds();
        extent_actually_parameterises_projection();
        degenerate_sizes_do_not_divide_by_zero();
        yaw_conversion_is_finite_and_wraps();
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    std::cout << "field_map: all checks passed\n";
    return 0;
}
