#include "field_map.h"

#include <algorithm>
#include <cmath>

namespace rm_terminal {

namespace {
constexpr double kFieldWidthM = 28.0;
constexpr double kFieldHeightM = 15.0;

double clamp_double(double value, double low, double high) {
    return std::max(low, std::min(high, value));
}
}  // namespace

FieldExtent default_field_extent() { return FieldExtent{kFieldWidthM, kFieldHeightM}; }

MapViewport fit_viewport(const FieldExtent& field, int widget_width, int widget_height) {
    if (widget_width <= 0 || widget_height <= 0 || field.width_m <= 0.0 || field.height_m <= 0.0) {
        return MapViewport{0, 0, 0, 0};
    }

    const double field_aspect = field.width_m / field.height_m;
    const double widget_aspect = static_cast<double>(widget_width) / static_cast<double>(widget_height);

    int width = widget_width;
    int height = widget_height;
    if (widget_aspect > field_aspect) {
        width = static_cast<int>(std::lround(widget_height * field_aspect));
    } else {
        height = static_cast<int>(std::lround(widget_width / field_aspect));
    }
    width = std::min(width, widget_width);
    height = std::min(height, widget_height);

    return MapViewport{(widget_width - width) / 2, (widget_height - height) / 2, width, height};
}

Projected project(const FieldExtent& field, const MapViewport& view,
                  double field_x_m, double field_y_m) {
    const bool in_bounds = field.width_m > 0.0 && field.height_m > 0.0 &&
                           field_x_m >= 0.0 && field_x_m <= field.width_m &&
                           field_y_m >= 0.0 && field_y_m <= field.height_m;

    if (field.width_m <= 0.0 || field.height_m <= 0.0) {
        return Projected{PixelPoint{static_cast<double>(view.x), static_cast<double>(view.y)}, false};
    }

    const double fx = clamp_double(field_x_m, 0.0, field.width_m) / field.width_m;
    const double fy = clamp_double(field_y_m, 0.0, field.height_m) / field.height_m;

    // Y is inverted: field y=0 is the near edge and must land at the viewport
    // bottom. Dropping this flip mirrors the entire map vertically.
    return Projected{PixelPoint{view.x + fx * view.width,
                                view.y + (1.0 - fy) * view.height},
                     in_bounds};
}

double project_yaw_deg(double yaw_deg) {
    if (!std::isfinite(yaw_deg)) return 0.0;
    // Negate to convert the field's counter-clockwise rotation into the screen's
    // clockwise direction, a consequence of the Y flip in project().
    double screen = std::fmod(-yaw_deg, 360.0);
    if (screen < 0.0) screen += 360.0;
    return screen;
}

}  // namespace rm_terminal
