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
#include <iostream>
#include <memory>
#include <stdexcept>

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
    auto* ring = first->findChild<QQuickItem*>("lowHealthRing");
    check(ring != nullptr, "one observable alert border exists");
    check(first->findChildren<QQuickItem*>("lowHealthRing").size() == 1,
          "exactly one alert border exists");
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
        if (tick == 1) check(initial != capture("roster-bright"), "border animation changes rendered pixels");
        if (tick == 4) capture("roster-dim");
    }
    check(high > 0.8 && low < 0.4, "pulse reaches both halves despite repeated updates");
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
        updates_and_rendering();
        check(!qml_error, "no QML load or runtime warnings occurred");
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
    return 0;
}
