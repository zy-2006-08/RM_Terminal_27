#include "roster_panel.h"

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QTimer>
#include <algorithm>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace rm_terminal;

namespace {
bool qml_error = false;

void messages(QtMsgType type, const QMessageLogContext&, const QString& message) {
    if ((type == QtWarningMsg || type == QtCriticalMsg) &&
        (message.contains("qrc:/") || message.contains("QQml"))) qml_error = true;
    std::cerr << message.toStdString() << '\n';
}

void check(bool ok, const char* name) {
    if (!ok) throw std::runtime_error(name);
    std::cout << "PASS " << name << '\n';
}

void wait(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

QQuickItem* row(QQuickItem* item) {
    if (item->metaObject()->indexOfProperty("lowHealthAlert") >= 0) return item;
    for (auto* child : item->childItems()) {
        if (auto* found = row(child)) return found;
    }
    return nullptr;
}

void boundaries() {
    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl("qrc:/qml/RosterRow.qml"));
    if (component.isError()) std::cerr << component.errorString().toStdString();
    std::unique_ptr<QObject> item(component.create());
    check(item != nullptr, "actual RosterRow QML creates without errors");
    const auto eligible = [&](int hp, int max, int live, bool has, bool ally, bool known) {
        item->setProperty("currentHp", hp);
        item->setProperty("maxHp", max);
        item->setProperty("liveState", live);
        item->setProperty("hasHp", has);
        item->setProperty("isAlly", ally);
        item->setProperty("factionKnown", known);
        return item->property("lowHealthAlert").toBool();
    };
    check(eligible(25, 100, 1, true, false, true), "25 percent enemy alerts");
    check(eligible(1, 100, 1, true, false, true), "positive low HP alerts");
    check(!eligible(26, 100, 1, true, false, true), "above 25 percent does not alert");
    check(!eligible(0, 100, 1, true, false, true), "zero HP does not alert");
    check(!eligible(-1, 100, 1, true, false, true), "negative HP does not alert");
    check(!eligible(1, 0, 1, true, false, true), "zero maximum does not alert");
    check(!eligible(1, -1, 1, true, false, true), "negative maximum does not alert");
    check(!eligible(25, 100, 2, true, false, true), "dead robot does not alert");
    check(!eligible(25, 100, 0, true, false, true), "no-data robot does not alert");
    check(!eligible(25, 100, 3, true, false, true), "invalid live state does not alert");
    check(!eligible(25, 100, 1, false, false, true), "missing HP does not alert");
    check(!eligible(25, 100, 1, true, true, true), "ally does not alert");
    check(!eligible(25, 100, 1, true, false, false), "unknown allegiance does not alert");
    for (bool blue : {false, true}) {
        item->setProperty("isBlue", blue);
        check(eligible(25, 100, 1, true, false, true), "either faction can be the enemy");
        check(!eligible(25, 100, 1, true, true, true), "neither faction alerts when allied");
    }
}

// 花名册的行把面板的高度均分,不留空白:以前行高封在 104、行间距封在 12,余量留在
// 面板底部,四行的花名册在高面板里下面空掉一大半(570x1040 的格子里空四百多像素)。
void rows_share_the_whole_height() {
    RosterPanel panel(true);
    panel.setFactionKnown(true, false);
    std::vector<RosterEntry> entries;
    for (const std::uint32_t id : {1u, 3u, 4u, 7u}) {
        RosterEntry entry;
        entry.id = RobotId{id};
        entry.faction = 1;
        entry.live = LiveState::Alive;
        entry.hp = 200;
        entry.max_hp = 200;
        entry.is_self = id == 3;
        entries.push_back(entry);
    }
    panel.setEntries(entries);
    panel.show();
    wait(30);

    QQuickItem* root = panel.rootObject();
    check(root != nullptr, "the roster panel root object exists");
    const auto rows = [&] {
        std::vector<QQuickItem*> found;
        const std::function<void(QQuickItem*)> walk = [&](QQuickItem* item) {
            if (item->metaObject()->indexOfProperty("lowHealthAlert") >= 0) {
                found.push_back(item);
                return;
            }
            for (QQuickItem* child : item->childItems()) walk(child);
        };
        walk(root);
        return found;
    };

    double previous_row_height = 0;
    for (const auto [w, h] : {std::pair{300, 150}, std::pair{560, 420}, std::pair{570, 1040}}) {
        panel.resize(w, h);
        QCoreApplication::processEvents();
        wait(30);
        const std::vector<QQuickItem*> found = rows();
        check(found.size() == entries.size(), "every robot still gets a row");
        // 四行加上三道间隔要填满面板的内容区(Panel.qml 的 body:上边 48、四周 8)。
        const double body = h - 48 - 8;
        double covered = 0;
        double bottom = 0;
        for (QQuickItem* row : found) {
            covered += row->height();
            bottom = std::max(bottom, row->y() + row->height());
        }
        check(covered >= body * 0.85, "the rows between them cover almost the whole body height");
        check(bottom >= body - 2, "the last row reaches the bottom of the body, leaving no blank band");
        // 面板越高,每行越高:余量进行高,不进底部的空白。
        check(found.front()->height() > previous_row_height, "a taller panel gives every row more height");
        previous_row_height = found.front()->height();
    }
    // 行高不再封在 104:1040 高的面板里四行每行都比 104 高。
    check(previous_row_height > 104, "row height is no longer capped at the old 104 pixels");

    // 字跟着行高长,上限 2.5 倍,且由宽度兜住不溢出。
    panel.resize(570, 1040);
    QCoreApplication::processEvents();
    wait(30);
    const std::vector<QQuickItem*> tall = rows();
    const double scale = tall.front()->property("fk").toDouble();
    check(scale > 1.25, "fonts grow past the old 1.25 ceiling when the row is tall");
    check(scale <= 2.5, "font growth stops at 2.5x");
    panel.resize(240, 1040);
    QCoreApplication::processEvents();
    wait(30);
    const double narrow = rows().front()->property("fk").toDouble();
    check(narrow < scale, "a narrow panel shrinks the fonts even though the rows are just as tall");
}

void updates_and_rendering() {
    RosterPanel panel(false);
    panel.resize(300, 160);
    check(panel.status() == QQuickWidget::Ready, "actual RosterPanel QML is ready");
    panel.setFactionKnown(true, true);
    RosterEntry entry;
    entry.id = RobotId{103};
    entry.live = LiveState::Alive;
    entry.hp = 25;
    entry.max_hp = 100;
    panel.setEntries({entry});
    panel.show();
    wait(30);
    QPointer<QQuickItem> first = row(panel.rootObject());
    check(first && first->property("lowHealthAlert").toBool(), "enemy panel forwards allegiance");
    auto* ring = first->findChild<QQuickItem*>("lowHealthGlow");
    check(ring != nullptr, "one observable low-health glow exists");
    check(first->findChildren<QQuickItem*>("lowHealthGlow").size() == 1,
          "exactly one low-health glow exists");
    const QString directory = qEnvironmentVariable("RM_UI_EVIDENCE_DIR");
    const auto capture = [&](const QString& name) {
        const QImage shot = panel.grabFramebuffer();
        check(!shot.isNull(), "native QML framebuffer is rendered");
        if (!directory.isEmpty()) {
            check(QDir().mkpath(directory), "evidence directory exists");
            check(shot.save(directory + "/" + name + ".png"), "PNG evidence saved");
        }
        return shot;
    };
    const auto initial = capture("roster-start");
    double low = 1, high = 0;
    for (int tick = 0; tick < 8; ++tick) {
        wait(250);
        entry.hp = 24 + tick % 2;
        panel.setEntries({entry});
        check(first && first == row(panel.rootObject()), "250ms changing snapshots preserve delegate identity");
        const double opacity = ring->opacity();
        low = std::min(low, opacity);
        high = std::max(high, opacity);
        std::cout << "pulse tick=" << tick << " opacity=" << opacity << '\n';
        if (tick == 1) check(initial != capture("roster-bright"), "the breathing glow changes rendered pixels");
        if (tick == 4) capture("roster-dim");
    }
    // 效果图的呼吸是 1.0 ↔ 0.55,半周期 650ms。
    check(high > 0.9 && low < 0.7, "pulse reaches both halves despite repeated updates");
    panel.setFactionKnown(false, true);
    check(!first->property("lowHealthAlert").toBool() && !ring->isVisible(), "unknown panel immediately hides alert");
    capture("roster-unknown");
    panel.setFactionKnown(true, true);
    entry.hp = 26;
    panel.setEntries({entry});
    check(!first->property("lowHealthAlert").toBool() && !ring->isVisible(), "recovery immediately hides alert");
    capture("roster-recovered");
    panel.setEntries({});
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    check(!first, "empty roster removes old delegate");
}
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    qInstallMessageHandler(messages);
    try {
        boundaries();
        rows_share_the_whole_height();
        updates_and_rendering();
        check(!qml_error, "no QML load or runtime warnings occurred");
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
    return 0;
}
