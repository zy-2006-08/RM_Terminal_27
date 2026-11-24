#include "field_zones.h"

#include "domain.h"

#include <array>
#include <cmath>
#include <vector>

namespace rm_terminal {
namespace {

constexpr double kFieldLength = 28.0;
constexpr double kFieldWidth = 15.0;

struct Rect {
    double x0, y0, x1, y1;
    bool contains(FieldPoint p) const { return p.x >= x0 && p.x <= x1 && p.y >= y0 && p.y <= y1; }
};

bool inside(const std::vector<FieldPoint>& polygon, FieldPoint p) {
    bool in = false;
    for (std::size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        const FieldPoint& a = polygon[i];
        const FieldPoint& b = polygon[j];
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) in = !in;
    }
    return in;
}

// 以下全是红方那一侧,米。来源见 场地坐标.md。
// 红资源区(补给区)外包盒。
constexpr Rect kSupply{0.0, 0.0, 3.32, 3.23};
// 基地增益点:基地外面那圈尖顶六边形(小地图上量的),中心是基地底座的中心 (2.46, 7.5)。
const std::vector<FieldPoint> kBaseBuff{{2.46, 9.53}, {4.18, 8.52}, {4.18, 6.48},
                                        {2.46, 5.47}, {0.74, 6.48}, {0.74, 8.52}};
// 堡垒_新 的外包盒。
constexpr Rect kFortress{5.29, 6.31, 7.86, 8.69};
// 制高点_红色_上侧 的外包盒。
constexpr Rect kHighPoint{0.0, 10.76, 2.8, 13.56};
// 中央高地增益点:中央高地红方这一侧那条高起来的走道(内外两道墙之间),照小地图和图 5-23 描的。
const std::vector<FieldPoint> kCentralStrip{{10.0, 4.9}, {10.0, 9.6}, {12.6, 13.0}, {13.1, 12.5},
                                            {11.2, 9.2}, {11.2, 5.6}, {12.3, 3.1}, {11.6, 2.7}};
// 前哨站底座中心,增益点按半径 1 米算。
constexpr FieldPoint kOutpost{10.92, 3.64};
// 装配区外包盒,在场地正中,不分红蓝。
constexpr Rect kAssembly{11.96, 5.31, 16.04, 9.70};
// 中央高地总装在中线上的上下边:中间这一段过中线算中央高地,两头是隧道。
constexpr double kHighlandLow = 1.94;
constexpr double kHighlandHigh = 13.07;

FieldPoint to_red_side(FieldPoint p, std::uint32_t faction) {
    if (faction != kBlue) return p;
    return {kFieldLength - p.x, kFieldWidth - p.y};
}

}  // namespace

bool in_half(FieldPoint point, std::uint32_t faction, double margin) {
    return faction == kBlue ? point.x > kFieldMidX + margin : point.x < kFieldMidX - margin;
}

std::optional<FieldZone> zone_at(FieldPoint point, std::uint32_t faction) {
    if (kAssembly.contains(point)) return FieldZone::AssemblyZone;
    const FieldPoint p = to_red_side(point, faction);
    if (kSupply.contains(p)) return FieldZone::SupplyZone;
    if (inside(kBaseBuff, p)) return FieldZone::BaseBuff;
    if (kFortress.contains(p)) return FieldZone::Fortress;
    if (kHighPoint.contains(p)) return FieldZone::HighPoint;
    if (inside(kCentralStrip, p)) return FieldZone::CentralHighland;
    if (std::hypot(p.x - kOutpost.x, p.y - kOutpost.y) <= 1.0) return FieldZone::OutpostBuff;
    return std::nullopt;
}

FieldPoint landmark(Landmark which, std::uint32_t faction) {
    FieldPoint red;
    switch (which) {
    case Landmark::DartLauncher: red = {1.31, 14.19}; break;  // 红飞镖发射井外包盒中心
    case Landmark::Outpost: red = kOutpost; break;
    case Landmark::Base: red = {2.46, 7.5}; break;
    case Landmark::Fortress: red = {(kFortress.x0 + kFortress.x1) / 2, (kFortress.y0 + kFortress.y1) / 2}; break;
    case Landmark::Rune: return {kFieldMidX, kFieldWidth / 2};
    }
    return to_red_side(red, faction);
}

std::vector<FieldPoint> zone_outline(FieldZone zone, std::uint32_t faction) {
    std::vector<FieldPoint> red;
    switch (zone) {
    case FieldZone::BaseBuff: red = kBaseBuff; break;
    case FieldZone::CentralHighland: red = kCentralStrip; break;
    case FieldZone::Fortress:
        red = {{kFortress.x0, kFortress.y0}, {kFortress.x1, kFortress.y0}, {kFortress.x1, kFortress.y1},
               {kFortress.x0, kFortress.y1}};
        break;
    default: return {};
    }
    for (FieldPoint& p : red) p = to_red_side(p, faction);
    return red;
}

FieldRoute route_at_midline(double y) {
    return y >= kHighlandLow && y <= kHighlandHigh ? FieldRoute::Highland : FieldRoute::Tunnel;
}

QString route_name(FieldRoute route) {
    switch (route) {
    case FieldRoute::Highland: return QStringLiteral("中央高地坡");
    case FieldRoute::Tunnel: return QStringLiteral("隧道");
    case FieldRoute::Road: return QStringLiteral("起伏路段");
    case FieldRoute::Unknown: break;
    }
    return QString();
}

}  // namespace rm_terminal
