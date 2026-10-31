#include "seat_picker.h"

#include "logging.h"

#include <QCloseEvent>
#include <QColor>
#include <QEventLoop>
#include <QKeyEvent>
#include <QQuickItem>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

namespace rm_terminal {

namespace {

// 最小窗口。1280x760 是程序的验收尺寸,960x600 是这一页还能把四张席位卡横排
// 摆下、按钮仍然点得中的下限。
constexpr int kMinWidth = 960;
constexpr int kMinHeight = 600;

// 席位卡用的兵种名要能被 Theme.avatarFor 认出来:指挥手坐的是空中系统席位,
// 所以它取「空中」那张头像,而界面上的职务名仍然写「指挥手」。
QString avatar_class(Role role) {
    switch (role) {
    case Role::Hero: return QStringLiteral("重装");
    case Role::Commander: return QStringLiteral("空中");
    case Role::Infantry: return QStringLiteral("步兵");
    }
    return QString();
}

// 可选席位表直接由 Seat::all() 推出,只取红方那一半 —— 蓝方是同样的四个编号 +100,
// 在这里再列一遍就等于把「蓝方 +100」这条规则抄进了界面层。
QVariantList seat_models() {
    QVariantList out;
    for (const int id : Seat::all()) {
        if (id > 100) continue;
        const std::optional<Seat> seat = Seat::fromRobotId(id);
        if (!seat) continue;
        QVariantMap map;
        map[QStringLiteral("number")] = id;
        map[QStringLiteral("title")] = role_title(seat->role());
        map[QStringLiteral("cls")] = avatar_class(seat->role());
        out.append(map);
    }
    return out;
}

}  // namespace

SeatPicker::SeatPicker(QWidget* parent) : QQuickWidget(parent) {
    setResizeMode(QQuickWidget::SizeRootObjectToView);
    // Quick 场景默认白底,深色 HUD 下会在面板圆角外沿露出白边。
    setClearColor(QColor(7, 9, 13));
    setMinimumSize(kMinWidth, kMinHeight);
    setWindowTitle(QStringLiteral("RM Terminal · 选择席位"));
    setSource(QUrl(QStringLiteral("qrc:/qml/SeatPicker.qml")));

    QQuickItem* root = rootObject();
    // 加载失败不崩:此时页面是空的,但键盘的 Enter/Esc 仍然可用,操作手至少能退出,
    // 而不是对着一块黑屏束手无策。
    if (!root) return;
    root->setProperty("seats", seat_models());
    connect(root, SIGNAL(factionRequested(int)), this, SLOT(onFactionRequested(int)));
    connect(root, SIGNAL(seatRequested(int)), this, SLOT(onSeatRequested(int)));
    connect(root, SIGNAL(confirmRequested()), this, SLOT(onConfirmRequested()));
    connect(root, SIGNAL(cancelRequested()), this, SLOT(onCancelRequested()));
    publish();
}

void SeatPicker::onFactionRequested(int faction) {
    chooseFaction(static_cast<std::uint32_t>(faction));
}

void SeatPicker::onSeatRequested(int number) { chooseSeat(number); }

void SeatPicker::onConfirmRequested() { confirm(); }

void SeatPicker::onCancelRequested() { cancel(); }

std::optional<Seat> SeatPicker::seat() const {
    if (faction_ == 0 || seat_number_ == 0) return std::nullopt;
    return Seat::fromRobotId(faction_ == 2 ? seat_number_ + 100 : seat_number_);
}

void SeatPicker::publish() {
    QQuickItem* root = rootObject();
    if (!root) return;
    root->setProperty("factionChoice", static_cast<int>(faction_));
    root->setProperty("seatChoice", seat_number_);
    root->setProperty("confirmEnabled", canConfirm());
    // 摘要整句在 C++ 拼好再灌进去:登录名换算只有 Seat 一份实现,测试也就能直接
    // 断言界面上那行字,而不是断言 QML 的拼接表达式。
    const std::optional<Seat> chosen = seat();
    root->setProperty("summaryText",
                      chosen ? QStringLiteral("将以 %1 进入，MQTT 登录名 %2")
                                   .arg(chosen->label(), chosen->mqttClientId())
                             : QString());
}

void SeatPicker::chooseFaction(std::uint32_t f) {
    if (f != 1 && f != 2) return;
    faction_ = f;
    // 阵营换边不清掉席位:操作手点错红蓝改回来时,已经选好的席位号不该跟着丢。
    publish();
}

void SeatPicker::chooseSeat(int number) {
    // 第二步选的是「几号席位」,不是车号 —— 蓝方 103 要走 chooseFaction(2) + chooseSeat(3),
    // 所以这里只认红方那一半编号。7 号(哨兵没有操作手)和 103 一样原地拒绝,
    // 不能等到确认那一步才发现。
    if (number > 100 || !Seat::fromRobotId(number)) return;
    seat_number_ = number;
    publish();
}

void SeatPicker::confirm() {
    const std::optional<Seat> chosen = seat();
    if (!chosen) return;
    result_ = chosen;
    finished_ = true;
    emit_log(LogLevel::info, QStringLiteral("seat_picked"),
             {QStringLiteral("seat=%1").arg(chosen->robotId()),
              QStringLiteral("role=%1").arg(role_name(chosen->role()))});
    Q_EMIT accepted(static_cast<int>(chosen->robotId()));
}

void SeatPicker::cancel() {
    result_.reset();
    finished_ = true;
    emit_log(LogLevel::info, QStringLiteral("seat_cancelled"));
    Q_EMIT cancelled();
}

std::optional<Seat> SeatPicker::exec() {
    finished_ = false;
    result_.reset();
    show();
    // 取消和确认都只置位 finished_,循环退出统一在这里 —— 两条出口各自 quit()
    // 一个局部 loop 会在「测试直接调 confirm()」时留下一个没人退出的循环。
    QEventLoop loop;
    const auto stop = [&loop] { loop.quit(); };
    const QMetaObject::Connection on_accept = connect(this, &SeatPicker::accepted, &loop, stop);
    const QMetaObject::Connection on_cancel = connect(this, &SeatPicker::cancelled, &loop, stop);
    if (!finished_) loop.exec();
    disconnect(on_accept);
    disconnect(on_cancel);
    hide();
    return result_;
}

void SeatPicker::keyPressEvent(QKeyEvent* event) {
    switch (event->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        // 没选完时 Enter 什么都不做,和按钮置灰一致。
        confirm();
        return;
    case Qt::Key_Escape:
        cancel();
        return;
    default:
        break;
    }
    QQuickWidget::keyPressEvent(event);
}

void SeatPicker::closeEvent(QCloseEvent* event) {
    // 关窗等于取消:默默关掉窗口然后带着一个没选的席位继续启动是最坏的结果。
    if (!finished_) cancel();
    QQuickWidget::closeEvent(event);
}

}  // namespace rm_terminal
