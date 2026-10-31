#include "seat_picker.h"

#include <QApplication>
#include <QEventLoop>
#include <QQuickItem>
#include <QTimer>
#include <functional>
#include <iostream>
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

// QML 的属性绑定在事件循环里求值,setProperty 之后立刻读派生属性会读到旧值。
void settle() {
    QEventLoop loop;
    QTimer::singleShot(30, &loop, &QEventLoop::quit);
    loop.exec();
}

// 不用 QSignalSpy:那是 Qt6::Test 里的类,为了数两个信号把整个测试库拉进链接
// 不值得。自己接一个计数器连的是同一个信号,断言力度一样。
struct SignalLog {
    std::vector<int> accepted;
    int cancelled = 0;

    explicit SignalLog(SeatPicker& picker) {
        QObject::connect(&picker, &SeatPicker::accepted, &picker,
                         [this](int id) { accepted.push_back(id); });
        QObject::connect(&picker, &SeatPicker::cancelled, &picker,
                         [this] { ++cancelled; });
    }
};

QString summary(const SeatPicker& picker) {
    QQuickItem* root = picker.rootObject();
    return root ? root->property("summaryText").toString() : QString();
}

// exec() 自己转事件循环,所以选择动作必须排到循环开始之后才投递 —— 否则 confirm()
// 在 exec() 之前就跑完,测的就不是「页面里点确认」那条路。
std::optional<Seat> run(SeatPicker& picker, const std::function<void()>& actions) {
    QTimer::singleShot(0, &picker, actions);
    return picker.exec();
}

void loads_without_qml_errors() {
    SeatPicker picker;
    check(picker.rootObject() != nullptr, "SeatPicker.qml root object loaded");
    check(picker.status() == QQuickWidget::Ready, "SeatPicker.qml reached Ready");
    check(picker.windowTitle() == QStringLiteral("RM Terminal · 选择席位"),
          "window title names the picker page");
    check(picker.minimumWidth() == 960 && picker.minimumHeight() == 600,
          "minimum size is 960x600");
    // 四张卡就是规则允许的四个席位:多一张意味着 7 号哨兵混进了列表。
    check(picker.rootObject()->property("seats").toList().size() == 4,
          "exactly four selectable seats are offered");
}

void confirm_needs_both_steps() {
    SeatPicker nothing;
    check(!nothing.canConfirm(), "confirm is impossible before either step");
    nothing.confirm();
    check(!nothing.rootObject()->property("confirmEnabled").toBool(),
          "QML confirm button stays disabled with nothing chosen");

    SeatPicker faction_only;
    faction_only.chooseFaction(1);
    settle();
    check(!faction_only.canConfirm(), "confirm is impossible after only the faction");
    check(summary(faction_only).isEmpty(), "no summary before the seat is chosen");

    SeatPicker seat_only;
    seat_only.chooseSeat(3);
    settle();
    check(!seat_only.canConfirm(), "confirm is impossible after only the seat");
    check(summary(seat_only).isEmpty(), "no summary before the faction is chosen");

    // 确认不可用时点下去必须完全没有副作用,而不是「发了信号但返回了空」。
    SignalLog signals_(seat_only);
    seat_only.confirm();
    check(signals_.accepted.empty(), "confirm emits nothing while it is unavailable");
}

void rejects_illegal_seats() {
    SeatPicker picker;
    picker.chooseFaction(1);
    // 7 号哨兵没有操作手;0 和 2 号不在选手端列表里。
    for (const int illegal : {0, 2, 5, 7, 103}) picker.chooseSeat(illegal);
    settle();
    check(!picker.canConfirm(), "illegal seat numbers are refused on the spot");
    picker.chooseFaction(7);
    settle();
    check(picker.rootObject()->property("factionChoice").toInt() == 1,
          "an illegal faction code leaves the choice untouched");
}

void red_three_returns_three() {
    SeatPicker picker;
    SignalLog signals_(picker);
    const std::optional<Seat> seat = run(picker, [&picker] {
        picker.chooseFaction(1);
        picker.chooseSeat(3);
        check(picker.canConfirm(), "both steps enable confirm");
        picker.confirm();
    });
    check(seat.has_value() && seat->robotId() == 3u, "red seat 3 returns robot id 3");
    check(seat->mqttClientId() == QStringLiteral("3"), "red seat 3 logs in as 3");
    check(signals_.accepted == std::vector<int>{3}, "accepted(3) is emitted exactly once");
}

void blue_six_returns_one_hundred_six() {
    SeatPicker picker;
    SignalLog signals_(picker);
    const std::optional<Seat> seat = run(picker, [&picker] {
        picker.chooseFaction(2);
        picker.chooseSeat(6);
        picker.confirm();
    });
    check(seat.has_value() && seat->robotId() == 106u, "blue seat 6 returns robot id 106");
    check(seat->role() == Role::Commander, "blue seat 6 is the commander");
    check(signals_.accepted == std::vector<int>{106}, "accepted(106) is emitted exactly once");
}

void cancel_returns_nothing() {
    SeatPicker picker;
    SignalLog signals_(picker);
    const std::optional<Seat> seat = run(picker, [&picker] {
        // 选完两步再取消:取消必须压过已经做出的选择。
        picker.chooseFaction(1);
        picker.chooseSeat(4);
        picker.cancel();
    });
    check(!seat.has_value(), "cancel returns no seat");
    check(signals_.cancelled == 1, "cancelled() is emitted exactly once");
    check(signals_.accepted.empty(), "cancel never reports an accepted seat");
}

void changing_faction_keeps_the_seat() {
    SeatPicker picker;
    picker.chooseFaction(1);
    picker.chooseSeat(3);
    settle();
    check(picker.canConfirm(), "red 3 is confirmable");
    check(summary(picker) == QStringLiteral("将以 红方 3 号 · 步兵 进入，MQTT 登录名 3"),
          "summary names the red seat and its login");

    picker.chooseFaction(2);
    settle();
    check(picker.rootObject()->property("seatChoice").toInt() == 3,
          "switching faction keeps the chosen seat");
    check(picker.canConfirm(), "the seat stays confirmable across a faction switch");
    check(summary(picker) == QStringLiteral("将以 蓝方 3 号 · 步兵 进入，MQTT 登录名 103"),
          "summary follows the new faction and its +100 login");

    const std::optional<Seat> seat = run(picker, [&picker] { picker.confirm(); });
    check(seat.has_value() && seat->robotId() == 103u, "the switched faction decides the id");
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    qInstallMessageHandler(messages);
    try {
        loads_without_qml_errors();
        confirm_needs_both_steps();
        rejects_illegal_seats();
        red_three_returns_three();
        blue_six_returns_one_hundred_six();
        cancel_returns_nothing();
        changing_faction_keeps_the_seat();
        check(!qml_error, "no QML load or runtime warnings occurred");
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
    return 0;
}
