#pragma once

#include "domain.h"
#include "field_map.h"

#include <QColor>
#include <QString>
#include <QStringList>
#include <QWidget>
#include <cstdint>
#include <vector>

namespace rm_terminal {

// The simulated roster size (sim/match_server.py publishes 1 self + 2 ally + 3
// enemy). It is a LOCAL simulation figure, not an RM2027 rule, and exists so the
// operator can see "received 4 of 6" rather than a plausible-looking partial map.
constexpr std::size_t kExpectedRobotCount = 6;

constexpr int kStatusBandHeight = 74;

// Where the field is actually drawn inside a widget of this size, i.e.
// fit_viewport applied to the area left over above the status band. Exposed so
// callers and tests can locate the field region without duplicating the layout.
MapViewport map_viewport(const FieldExtent& field, int widget_width, int widget_height);

QColor faction_color(std::uint32_t faction);

// A Snapshot field group reduced to the values the painter needs. The pane keeps
// these by value: a Snapshot is a per-tick temporary, so holding a reference to
// one would dangle.
struct MapMarker {
    RobotId id{0};
    std::uint32_t faction = 0;
    bool is_self = false;
    double x_m = 0.0;
    double y_m = 0.0;
    double yaw_deg = 0.0;
    bool has_position = false;
    bool has_yaw = false;
    bool stale = false;
};

std::vector<MapMarker> markers_from(const std::vector<MapRobot>& robots);

QString robot_label(const MapMarker& marker);

// An empty spot on the map cannot distinguish "no data" from "not in this match"
// from "renderer broke", and under time pressure reads as "that robot is not
// here". So whatever is not drawn must be named in text instead.
struct MapStatus {
    QStringList missing;
    QStringList out_of_bounds;
    std::size_t received = 0;
    bool self_position_unavailable = false;
};

MapStatus map_status(const std::vector<MapMarker>& markers, const FieldExtent& field);

class MapPane final : public QWidget {
public:
    explicit MapPane(QWidget* parent = nullptr);

    void setRobots(const std::vector<MapRobot>& robots);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    std::vector<MapMarker> markers_;
    FieldExtent field_ = default_field_extent();
};

}  // namespace rm_terminal
