#include "dart_pane.h"

#include "theme.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QPainter>
#include <QPaintEvent>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

namespace rm_terminal {

namespace {

constexpr int kRowHeight = 17;
constexpr int kTileHeight = 40;
constexpr int kGap = 6;
constexpr std::uint32_t kGateOpened = 2;
// 开闸和确认发射共用 DartCommand,回显判定要分得开:tag 0 给选目标和开闸,
// 10 给确认发射,不然一次发射的失败会被归到上一次选目标上。
constexpr std::uint32_t kTagLaunch = 10;

const QString kDash = QStringLiteral("--");

template <class T>
std::optional<T> present(const Field<T>& field) {
    if (field.quality != Quality::Valid || !field.value) return std::nullopt;
    return field.value;
}

QString gate_text(std::optional<std::uint32_t> gate) {
    if (!gate) return kDash;
    switch (*gate) {
        case 0: return QStringLiteral("关闭");
        case 1: return QStringLiteral("开启中");
        case 2: return QStringLiteral("已开启");
        default: return QStringLiteral("未知（%1）").arg(*gate);
    }
}

QColor gate_colour(std::optional<std::uint32_t> gate) {
    if (!gate) return theme::kTextMuted;
    switch (*gate) {
        case 0: return theme::kTextSecondary;
        case 1: return theme::kWarn;
        case 2: return theme::kOk;
        default: return theme::kTextMuted;
    }
}

QPushButton* make_button(QWidget* parent, const QString& text, const QString& name) {
    auto* button = new QPushButton(text, parent);
    button->setObjectName(name);
    button->setStyleSheet(command_button_style());
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::NoFocus);
    return button;
}

}  // namespace

QString dart_target_text(std::uint32_t target_id) {
    switch (target_id) {
        case 1: return QStringLiteral("前哨站");
        case 2: return QStringLiteral("基地固定");
        case 3: return QStringLiteral("基地随机固定");
        case 4: return QStringLiteral("基地随机移动");
        case 5: return QStringLiteral("基地末端移动");
        default: return kDash;
    }
}

DartView dart_view(const Snapshot& snapshot) {
    DartView view;
    view.gate = present(snapshot.dart.gate);
    view.target_id = present(snapshot.dart.target_id);
    for (const EventRecord& record : snapshot.events.recent(snapshot.events.capacity())) {
        if (record.event_id != 9 && record.event_id != 10) continue;
        view.event_lines.push_back(QString::fromStdString(record.text));
        if (view.event_lines.size() >= 2) break;
    }
    return view;
}

DartPane::DartPane(QWidget* parent) : QWidget(parent) {
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(kGap);
    column->addStretch(1);

    // 五个目标按两行摆(3 + 2)。一行五个在 380 宽下每个只有 70 像素,
    // 「基地随机固定」会被挤成省略号。
    auto* grid = new QGridLayout;
    grid->setSpacing(4);
    for (std::size_t i = 0; i < targets_.size(); ++i) {
        const auto id = static_cast<std::uint32_t>(i + 1);
        targets_[i] = make_button(this, dart_target_text(id),
                                  QStringLiteral("dartTarget%1").arg(id));
        targets_[i]->setCheckable(true);
        grid->addWidget(targets_[i], static_cast<int>(i / 3), static_cast<int>(i % 3));
        QObject::connect(targets_[i], &QPushButton::clicked, this,
                         [this, id] { send(id, false, false, id); });
    }
    column->addLayout(grid);

    auto* actions = new QHBoxLayout;
    actions->setSpacing(4);
    open_ = make_button(this, QStringLiteral("开闸"), QStringLiteral("dartOpenGate"));
    launch_ = make_button(this, QStringLiteral("确认发射 按住"), QStringLiteral("dartLaunch"));
    actions->addWidget(open_, 2);
    actions->addWidget(launch_, 3);
    column->addLayout(actions);

    QObject::connect(open_, &QPushButton::clicked, this, [this] {
        send(view_.target_id.value_or(0), true, false, 0);
    });
    QObject::connect(launch_, &QPushButton::pressed, this, [this] {
        launch_hold_.press(now_);
        refreshButtons();
    });
    QObject::connect(launch_, &QPushButton::released, this, [this] {
        const bool fired = launch_hold_.release(now_);
        refreshButtons();
        if (fired) send(view_.target_id.value_or(0), true, true, kTagLaunch);
    });

    setMinimumHeight(190);
    refreshButtons();
}

void DartPane::setView(const DartView& view) {
    view_ = view;
    refreshButtons();
    update();
}

void DartPane::setLinkUp(bool up) {
    if (link_up_ == up) return;
    link_up_ = up;
    if (!up) launch_hold_.cancel();
    refreshButtons();
    update();
}

void DartPane::setSender(
    std::function<bool(const char* topic, const std::string& payload)> sender) {
    sender_ = std::move(sender);
}

void DartPane::tick(MonotonicMs now) {
    now_ = now;
    echo_result_ = echo_.update(now, echoFingerprint());
    refreshButtons();
    update();
}

std::uint64_t DartPane::echoFingerprint() const {
    // 飞镖没有显式回执,回显就是 DartSelectTargetStatusSync 的 gate/target 变化。
    const std::uint64_t gate = view_.gate ? *view_.gate + 1u : 0u;
    const std::uint64_t target = view_.target_id ? *view_.target_id + 1u : 0u;
    return gate * 16u + target;
}

void DartPane::send(std::uint32_t target_id, bool open, bool launch_confirm, std::uint32_t tag) {
    if (!link_up_ || !sender_) return;
    if (!gate_.allow(now_)) return;
    if (!sender_("DartCommand", encode_dart(target_id, open, launch_confirm))) return;
    echo_.sent(now_, tag, echoFingerprint());
    echo_result_ = echo_.result();
    refreshButtons();
    update();
}

void DartPane::refreshButtons() {
    const bool live = link_up_ && static_cast<bool>(sender_);
    for (std::size_t i = 0; i < targets_.size(); ++i) {
        targets_[i]->setEnabled(live);
        const bool chosen = view_.target_id.value_or(0) == static_cast<std::uint32_t>(i + 1);
        if (targets_[i]->isChecked() != chosen) targets_[i]->setChecked(chosen);
    }
    open_->setEnabled(live);
    // 闸门没开就发确认发射是无效指令,按钮灰掉比发出去等一个必然的失败更清楚。
    launch_->setEnabled(live && view_.gate.value_or(0) == kGateOpened);

    const double held = launch_hold_.progress(now_);
    launch_->setStyleSheet(held > 0.0 ? hold_button_style(held) : command_button_style());
    launch_->setText(launch_hold_.ready(now_) ? QStringLiteral("确认发射 松手发送")
                                              : QStringLiteral("确认发射 按住"));
}

bool DartPane::buttonsEnabled() const { return open_->isEnabled() && targets_[0]->isEnabled(); }

QString DartPane::statusText() const {
    if (!link_up_) return QStringLiteral("断线，指令不会发送");
    if (!sender_) return QStringLiteral("未接入发送链路");
    const QString echo = echo_status_text(echo_result_);
    if (!echo.isEmpty()) return echo;
    if (!view_.gate) return QStringLiteral("等待 DartSelectTargetStatusSync");
    return QStringLiteral("就绪");
}

QSize DartPane::sizeHint() const { return QSize(380, 250); }

void DartPane::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const int w = width();
    const int canvas_h = std::max(targets_[0]->geometry().top() - kGap, 0);
    if (canvas_h <= 0 || w <= 0) return;

    const QRect status(0, canvas_h - kRowHeight, w, kRowHeight);
    painter.setFont(theme::labelFont(10, !link_up_));
    painter.setPen(!link_up_ ? theme::kDanger
                             : (echo_result_.status == EchoStatus::Unconfirmed ||
                                        echo_result_.status == EchoStatus::Failed
                                    ? theme::kWarn
                                    : theme::kTextSecondary));
    painter.drawText(status, Qt::AlignLeft | Qt::AlignVCenter, statusText());

    int y = 0;
    const int limit = status.top() - 2;
    const auto fits = [&](int h) { return y + h <= limit; };

    if (fits(kTileHeight)) {
        const int tile_w = (w - kGap) / 2;
        role_paint::tile(painter, QRect(0, y, tile_w, kTileHeight), QStringLiteral("闸门"),
                         gate_text(view_.gate), gate_colour(view_.gate));
        role_paint::tile(painter, QRect(tile_w + kGap, y, w - tile_w - kGap, kTileHeight),
                         QStringLiteral("已选目标"),
                         view_.target_id ? dart_target_text(*view_.target_id) : kDash,
                         view_.target_id ? theme::kTextPrimary : theme::kTextMuted);
        y += kTileHeight + 4;
    }

    if (!view_.gate && fits(kRowHeight)) {
        role_paint::row(painter, QRect(0, y, w, kRowHeight), QStringLiteral("数据源"),
                        QStringLiteral("等待 DartSelectTargetStatusSync"), theme::kTextMuted);
        y += kRowHeight;
    }

    if (fits(kRowHeight)) {
        painter.setFont(theme::labelFont(9));
        painter.setPen(theme::kTextMuted);
        painter.drawText(QRect(0, y, w, kRowHeight), Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("飞镖事件"));
        y += kRowHeight - 2;
    }
    if (view_.event_lines.empty() && fits(kRowHeight)) {
        painter.setFont(theme::labelFont(10));
        painter.setPen(theme::kTextMuted);
        painter.drawText(QRect(0, y, w, kRowHeight), Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("-- 还没有 Event 9/10"));
        y += kRowHeight;
    }
    for (const QString& text : view_.event_lines) {
        if (!fits(kRowHeight)) break;
        painter.setFont(theme::labelFont(10));
        painter.setPen(theme::kTextPrimary);
        const QRect box(0, y, w, kRowHeight);
        painter.drawText(box, Qt::AlignLeft | Qt::AlignVCenter,
                         painter.fontMetrics().elidedText(text, Qt::ElideRight, w));
        y += kRowHeight;
    }
}

}  // namespace rm_terminal
