#pragma once

namespace rm_terminal
{

    // Field size in metres: 28.0 x 15.0, per the 2027 rulebook 4.1 and measured in the
    // official RMUC2027_V1.0.0.stp, where the field spans model X [-14000, 14000] mm and
    // Y [-5875.5, 9124.5] mm. resources/images/minimap/minimap_bg.png is that rectangle
    // rendered top-down and turned 180 degrees, because the model has red on +X while the
    // protocol puts red's supply-zone fence corner at the origin: protocol
    // x = (14000 - X) / 1000, y = (9124.5 - Y) / 1000.
    //
    // Wrong dimensions here are a SILENT failure: every robot gets uniformly offset
    // or scaled while the map still looks perfectly tidy, so the operator has no cue
    // that anything is off. That is why the size lives in exactly one place, is
    // injected as a parameter rather than read from a global, and is captioned on the
    // map itself as a stated local assumption.
    struct FieldExtent
    {
        double width_m;
        double height_m;
    };

    FieldExtent default_field_extent();

    // Pixel rectangle inside the widget where the field is actually drawn, after
    // aspect-preserving fit and centring. Not the widget rect.
    struct MapViewport
    {
        int x, y, width, height;
    };

    struct PixelPoint
    {
        double x, y;
    };

    // `point` is always clamped inside the viewport so painting cannot escape it.
    // `in_bounds` is the honest answer to "was this coordinate actually on the field".
    //
    // These two must be used together: drawing a clamped point without an
    // out-of-bounds marker renders an off-field robot as a normal one resting on the
    // boundary, which reads as a legitimate tactical position.
    struct Projected
    {
        PixelPoint point;
        bool in_bounds;
    };

    // Largest aspect-preserving rectangle for `field` that fits the widget, centred.
    // Degenerate or negative widget sizes yield a zero-sized viewport rather than
    // dividing by zero.
    MapViewport fit_viewport(const FieldExtent& field, int widget_width, int widget_height);

    // Field metres -> viewport pixels, flipping Y because screen Y grows downward
    // while field Y grows upward.
    Projected project(const FieldExtent& field, const MapViewport& view,
                      double field_x_m, double field_y_m);

    // Protocol yaw (degrees) -> screen drawing angle, normalised to [0, 360).
    // Convention: 0 degrees points along +x (field east, screen right) and the angle
    // increases counter-clockwise in field space. Because the projection flips Y, a
    // counter-clockwise field rotation is clockwise on screen; the returned value is
    // already in screen terms, so callers pass it straight to a painter rotation.
    double project_yaw_deg(double yaw_deg);

} // namespace rm_terminal
