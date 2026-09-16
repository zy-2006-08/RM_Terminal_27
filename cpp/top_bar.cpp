#include "top_bar.h"

#include <QQmlContext>
#include <QQuickItem>
#include <QUrl>
#include <QVariantMap>

namespace rm_terminal {

namespace {

// 必须与 TopBar.qml 的 root.height 一致。两处一旦不同,较大的那个会在卡片下面空出
// 纯黑,叠进顶栏与面板区之间的缝。128 = 血条 y22 + 高30 + 间距4 + 卡片高72。
constexpr int kTopBarHeight = 128;

QVariantMap card_of(const TopBarSlot& slot) {
    QVariantMap map;
    map[QStringLiteral("id")] = slot.id;
    // 缺席槽位报 0,并靠 hasHp=false 表达「没有数据」。QML 侧不画填充,所以 0 不会
    // 被误画成空血条。
    map[QStringLiteral("hp")] = slot.hp ? *slot.hp : 0u;
    map[QStringLiteral("maxHp")] = slot.max_hp ? *slot.max_hp : 0u;
    map[QStringLiteral("hasHp")] = slot.has_hp();
    map[QStringLiteral("online")] = slot.online;
    map[QStringLiteral("cls")] = slot.robot_class;
    return map;
}

}  // namespace

QVariantList top_bar_cards(const TopBarSide& side) {
    QVariantList out;
    for (const TopBarSlot& slot : side.cards) out.append(card_of(slot));
    return out;
}

TopBar::TopBar(QWidget* parent) : QQuickWidget(parent) {
    setResizeMode(QQuickWidget::SizeRootObjectToView);
    setFixedHeight(kTopBarHeight);
    // 素材是深色 HUD,背景必须同色,否则 Quick 场景默认白底会在边缘露出白框。
    setClearColor(QColor(8, 9, 12));
    setSource(QUrl(QStringLiteral("qrc:/qml/TopBar.qml")));
}

void TopBar::setSnapshot(const Snapshot& snapshot) { apply(top_bar_model(snapshot)); }

void TopBar::apply(const TopBarModel& model) {
    QQuickItem* root = rootObject();
    // 加载失败时静默返回而不是崩:顶部栏是显示层,协议数据仍在别处可见。加载失败
    // 由 dashboard 的占位文字暴露,不在这里伪装成正常。
    if (!root) return;

    root->setProperty("redHp", model.red.base_hp ? *model.red.base_hp : 0u);
    root->setProperty("redMaxHp", model.red.base_max_hp ? *model.red.base_max_hp : 0u);
    root->setProperty("redHasHp", model.red.has_base_hp());
    root->setProperty("blueHp", model.blue.base_hp ? *model.blue.base_hp : 0u);
    root->setProperty("blueMaxHp", model.blue.base_max_hp ? *model.blue.base_max_hp : 0u);
    root->setProperty("blueHasHp", model.blue.has_base_hp());

    root->setProperty("redOutpostHp", model.red.outpost_hp ? *model.red.outpost_hp : 0u);
    root->setProperty("redOutpostMaxHp",
                      model.red.outpost_max_hp ? *model.red.outpost_max_hp : 0u);
    root->setProperty("redOutpostHasHp", model.red.has_outpost_hp());
    root->setProperty("blueOutpostHp", model.blue.outpost_hp ? *model.blue.outpost_hp : 0u);
    root->setProperty("blueOutpostMaxHp",
                      model.blue.outpost_max_hp ? *model.blue.outpost_max_hp : 0u);
    root->setProperty("blueOutpostHasHp", model.blue.has_outpost_hp());

    root->setProperty("redTeamName", model.red.team_name);
    root->setProperty("blueTeamName", model.blue.team_name);

    root->setProperty("redScore", model.red.score);
    root->setProperty("blueScore", model.blue.score);
    root->setProperty("clockText", model.clock);
    root->setProperty("roundText", model.round);
    root->setProperty("stageText", model.stage);
    root->setProperty("clockCritical", model.clock_critical);

    root->setProperty("redRobots", top_bar_cards(model.red));
    root->setProperty("blueRobots", top_bar_cards(model.blue));
}

}  // namespace rm_terminal
