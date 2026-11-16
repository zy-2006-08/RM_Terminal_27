#pragma once

#include <QString>

#include <cstdint>
#include <optional>
#include <vector>

namespace rm_terminal {

// 场地上和增益、来路有关的几块地方,协议坐标(米,原点在红方补给区那一角)。
// 数据来自官方场地模型 RMUC2027_V1.0.0.stp(scripts/stp_landmarks.py 读出的零件范围)和规则手册
// 图 5-23 的增益点分布,对照见 场地坐标.md。场地按中心点中心对称:红方这边写一份,蓝方的转 180°。
// 都是外包盒或照图描的多边形,边上差个二三十厘米;雷达定位本身也有这么大的误差。
struct FieldPoint {
    double x = 0;
    double y = 0;
};

enum class FieldZone {
    SupplyZone,       // 补给区(规则 5.6.4.8):回血,重装无敌
    BaseBuff,         // 基地增益点(5.6.4.2):防御 50%
    HighPoint,        // 制高点增益点(5.6.4.3):防御 50%
    Fortress,         // 堡垒增益点(5.6.4.9):自己的防御 50%,对方的易伤 100%
    CentralHighland,  // 中央高地增益点(5.6.4.4):防御 25%,两侧各一条,不相连
    AssemblyZone,     // 装配区(5.6.4.7):重装装配中无敌
    OutpostBuff,      // 前哨站增益点(5.6.4.6)
};

// 中线 x = 14 米。红方半场是 x < 14。margin > 0 时要越过中线这么多才算进了这一方的半场。
constexpr double kFieldMidX = 14.0;
bool in_half(FieldPoint point, std::uint32_t faction, double margin = 0);

// 这个点在 faction 那一方的哪块地方。装配区在场地正中,两方都算。
std::optional<FieldZone> zone_at(FieldPoint point, std::uint32_t faction);

// 地图动画要用的几个点:faction 那一方的飞镖发射井、前哨站、基地、堡垒的中心;能量机关在正中。
enum class Landmark { DartLauncher, Outpost, Base, Fortress, Rune };
FieldPoint landmark(Landmark which, std::uint32_t faction);
// 增益区的轮廓(多边形顶点),只有基地增益点、堡垒、中央高地增益点有;别的返回空。
std::vector<FieldPoint> zone_outline(FieldZone zone, std::uint32_t faction);

// 敌人过中线走的哪条路:中线上中央高地占着中间那一大段,上下两头是隧道。
enum class FieldRoute { Unknown, Highland, Tunnel, Road };
FieldRoute route_at_midline(double y);
// 横幅里的说法:「中央高地坡」「隧道」「起伏路段」,不知道时为空。
QString route_name(FieldRoute route);

}  // namespace rm_terminal
