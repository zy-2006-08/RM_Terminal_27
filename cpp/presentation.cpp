#include "presentation.h"

#include "logging.h"

#include <QStringList>
#include <tuple>

namespace rm_terminal {
QString freshness_text(Freshness freshness) {
    switch (freshness) {
    case Freshness::Fresh: return QStringLiteral("fresh");
    case Freshness::Stale: return QStringLiteral("stale");
    case Freshness::NeverReceived: return QStringLiteral("never_received");
    }
    return QStringLiteral("unknown");
}
// Value-by-value mirror of core/constants.py:80-86 CHASSIS_MODES (authoritative,
// aligned to the 2026 protocol). Python is the source of truth; if these ever
// disagree, fix this table, never constants.py.
//   0 停机          1 手动驾驶      2 底盘跟随云台
//   3 小陀螺        4 自瞄模式      other 未知
QString chassis_name(std::uint32_t mode) {
    switch (mode) {
    case 0: return QStringLiteral("停机");
    case 1: return QStringLiteral("手动驾驶");
    case 2: return QStringLiteral("底盘跟随云台");
    case 3: return QStringLiteral("小陀螺");
    case 4: return QStringLiteral("自瞄模式");
    default: return QStringLiteral("未知");
    }
}

QString quality_text(Quality quality) {
    switch (quality) {
    case Quality::Missing: return QStringLiteral("missing");
    case Quality::Valid: return QStringLiteral("valid");
    case Quality::Invalid: return QStringLiteral("invalid");
    }
    return QStringLiteral("unknown");
}
namespace {
template<class Group>
int count_stale(Group group) {
    int stale = 0;
    std::apply([&stale](auto&... field) { ((stale += field.freshness == Freshness::Stale ? 1 : 0), ...); },
               group.fields());
    return stale;
}
}

void StaleReporter::inspect(const Snapshot& snapshot) {
    std::set<QString> stale;
    auto note = [&stale](const QString& group, int count) {
        if (count > 0) stale.insert(QStringLiteral("%1:%2").arg(group).arg(count));
    };
    Snapshot copy = snapshot;
    note(QStringLiteral("game"), count_stale(copy.game));
    note(QStringLiteral("event"), count_stale(copy.event));
    for (auto& entry : copy.robots) {
        const auto id = QString::number(entry.first.value);
        note(QStringLiteral("robot%1.dynamic").arg(id), count_stale(entry.second.dynamic));
        note(QStringLiteral("robot%1.modules").arg(id), count_stale(entry.second.modules));
        note(QStringLiteral("robot%1.position").arg(id), count_stale(entry.second.position));
        note(QStringLiteral("robot%1.telemetry").arg(id), count_stale(entry.second.telemetry));
    }
    if (stale == stale_) return;
    stale_ = stale;
    if (stale.empty()) return;
    QStringList groups;
    for (const QString& item : stale) groups.append(item);
    emit_log(LogLevel::warning, QStringLiteral("stale_data"),
             {QStringLiteral("groups=%1").arg(groups.join(QLatin1Char(','))),
              QStringLiteral("robots=%1").arg(static_cast<qulonglong>(snapshot.robots.size()))});
}

QString snapshot_text(const Snapshot& snapshot) {
    const auto& stage = snapshot.game.current_stage;
    return QStringLiteral("LOCAL SIMULATION\nstage=%1 quality=%2 freshness=%3\nrobots=%4")
        .arg(stage.value ? QString::number(*stage.value) : QStringLiteral("missing"))
        .arg(quality_text(stage.quality)).arg(freshness_text(stage.freshness))
        .arg(static_cast<qulonglong>(snapshot.robots.size()));
}
}
