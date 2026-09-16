#include "roster_panel.h"

#include "presentation.h"

#include <QColor>
#include <QQmlContext>
#include <QQuickItem>
#include <QUrl>
#include <QVariantMap>

namespace rm_terminal {

namespace {

int live_code(LiveState live) {
    switch (live) {
    case LiveState::Alive: return 1;
    case LiveState::Kia: return 2;
    case LiveState::NoData: return 0;
    }
    return 0;
}

}  // namespace

QVariantList roster_entry_models(const std::vector<RosterEntry>& entries) {
    QVariantList out;
    for (const RosterEntry& entry : entries) {
        QVariantMap map;
        map[QStringLiteral("id")] = entry.id.value;
        map[QStringLiteral("live")] = live_code(entry.live);
        // hp/maxHp 缺失时报 0,并靠 hasHp=false 表达「没有数据」。QML 侧在 hasHp 为假时
        // 走「无血量数据」分支,所以这个 0 不会被画成空血条(即阵亡)。
        map[QStringLiteral("hasHp")] = entry.hp.has_value() && entry.max_hp.has_value();
        map[QStringLiteral("hp")] = entry.hp ? *entry.hp : 0u;
        map[QStringLiteral("maxHp")] = entry.max_hp ? *entry.max_hp : 0u;
        map[QStringLiteral("cls")] = robot_class_name(entry.id.value);
        map[QStringLiteral("hasPosition")] = entry.has_position;
        map[QStringLiteral("positionStale")] = entry.position_stale;
        out.append(map);
    }
    return out;
}

RosterPanel::RosterPanel(bool is_ally, QWidget* parent)
    : QQuickWidget(parent), is_ally_(is_ally) {
    setResizeMode(QQuickWidget::SizeRootObjectToView);
    // Quick 场景默认白底,深色 HUD 下会在圆角外沿露出白边。
    setClearColor(QColor(7, 9, 13));
    setSource(QUrl(QStringLiteral("qrc:/qml/RosterPanel.qml")));
    applyFaction();
}

void RosterPanel::setFactionKnown(bool known, bool is_blue) {
    faction_known_ = known;
    is_blue_ = is_blue;
    applyFaction();
}

void RosterPanel::applyFaction() {
    QQuickItem* root = rootObject();
    if (!root) return;
    root->setProperty("isBlue", is_blue_);
    root->setProperty("isAlly", is_ally_);
    root->setProperty("factionKnown", faction_known_);
}

void RosterPanel::setEntries(const std::vector<RosterEntry>& entries) {
    count_ = entries.size();
    QQuickItem* root = rootObject();
    // 加载失败时静默返回而不是崩:血量数据在顶栏仍然可见,加载失败由空面板暴露。
    if (!root) return;
    applyFaction();
    root->setProperty("entries", roster_entry_models(entries));
}

}  // namespace rm_terminal
