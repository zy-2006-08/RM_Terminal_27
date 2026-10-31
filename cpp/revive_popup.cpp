#include "revive_popup.h"

#include "theme.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

namespace rm_terminal {

namespace {

constexpr int kWidth = 460;
constexpr int kTitleHeight = 40;
constexpr int kBarHeight = 8;
constexpr int kLineHeight = 18;
constexpr int kPad = 16;

QPushButton* make_button(QWidget* parent, const char* name) {
    auto* button = new QPushButton(parent);
    button->setObjectName(QString::fromLatin1(name));
    button->setStyleSheet(command_button_style());
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::NoFocus);
    return button;
}

}  // namespace

RevivePopup::RevivePopup(QWidget* host) : QWidget(host) {
    setObjectName(QStringLiteral("revivePopup"));
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(kPad, kPad + kTitleHeight + kBarHeight + 10, kPad, kPad);
    column->setSpacing(8);
    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    pay_button_ = make_button(this, "revivePay");
    free_button_ = make_button(this, "reviveFree");
    buttons->addWidget(pay_button_, 1);
    buttons->addWidget(free_button_, 1);
    column->addLayout(buttons);
    column->addSpacing(kLineHeight * 2 + 4);

    QObject::connect(pay_button_, &QPushButton::clicked, this, [this] {
        if (pay_) pay_();
    });
    QObject::connect(free_button_, &QPushButton::clicked, this, [this] {
        if (confirm_free_) confirm_free_();
    });
    if (host) host->installEventFilter(this);
    setVisible(false);
    refresh();
}

void RevivePopup::setView(const SelfCommandView& view) {
    view_ = view;
    refresh();
}

void RevivePopup::setLinkUp(bool up) {
    link_up_ = up;
    refresh();
}

void RevivePopup::setActions(std::function<void()> confirm_free, std::function<void()> pay) {
    confirm_free_ = std::move(confirm_free);
    pay_ = std::move(pay);
}

void RevivePopup::setStatus(const QString& status) {
    if (status_ == status) return;
    status_ = status;
    update();
}

QString RevivePopup::titleText() const {
    if (view_.can_free_revive.value_or(false)) return QStringLiteral("可以复活");
    if (view_.revive_total && view_.revive_current && *view_.revive_total >= *view_.revive_current)
        return QStringLiteral("复活中 %1s").arg(*view_.revive_total - *view_.revive_current);
    return QStringLiteral("复活中");
}

void RevivePopup::refresh() {
    const bool dead = view_.dead.value_or(false);
    const bool can_pay = view_.can_pay_revive.value_or(true);
    const QString cost = view_.revive_cost ? QString::number(*view_.revive_cost) : QStringLiteral("--");
    pay_button_->setText(can_pay ? QStringLiteral("金币复活 · %1金").arg(cost)
                                 : QStringLiteral("%1 金币不足").arg(cost));
    pay_button_->setEnabled(link_up_ && can_pay);
    free_button_->setText(QStringLiteral("免费复活 [Y]"));
    free_button_->setEnabled(link_up_ && view_.can_free_revive.value_or(false));
    if (dead != isVisible()) setVisible(dead);
    if (dead) {
        place();
        raise();
    }
    update();
}

void RevivePopup::place() {
    QWidget* host = parentWidget();
    if (!host) return;
    const int width = std::min(kWidth, host->width() - 40);
    const int height = sizeHint().height();
    setGeometry((host->width() - width) / 2, std::max(0, host->height() * 2 / 5 - height / 2), width,
                height);
}

QSize RevivePopup::sizeHint() const {
    return QSize(kWidth, kPad * 2 + kTitleHeight + kBarHeight + 10 + 42 + 8 + kLineHeight * 2 + 4);
}

bool RevivePopup::eventFilter(QObject* watched, QEvent* event) {
    if (watched == parentWidget() && event->type() == QEvent::Resize && isVisible()) place();
    return false;
}

void RevivePopup::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF box = QRectF(rect()).adjusted(1, 1, -1, -1);
    painter.setPen(QPen(theme::kOk, 2));
    painter.setBrush(QColor(theme::kSurface.red(), theme::kSurface.green(), theme::kSurface.blue(), 240));
    painter.drawPath(theme::skewPath(box, 8));

    const int inner = width() - kPad * 2;
    painter.setFont(theme::labelFont(18, true));
    painter.setPen(theme::kTextPrimary);
    painter.drawText(QRect(kPad, kPad, inner, kTitleHeight), Qt::AlignCenter, titleText());

    const QRect track(kPad + 30, kPad + kTitleHeight, inner - 60, kBarHeight);
    painter.setPen(Qt::NoPen);
    painter.setBrush(theme::kSurfaceRaised);
    painter.drawRect(track);
    if (view_.revive_total && *view_.revive_total > 0) {
        const double ratio =
            std::clamp(static_cast<double>(view_.revive_current.value_or(0)) / *view_.revive_total, 0.0, 1.0);
        painter.setBrush(theme::kOk);
        painter.drawRect(QRect(track.left(), track.top(), static_cast<int>(track.width() * ratio),
                               track.height()));
    }

    const int text_top = pay_button_->geometry().bottom() + 10;
    painter.setFont(theme::labelFont(10));
    painter.setPen(theme::kTextSecondary);
    painter.drawText(QRect(kPad, text_top, inner, kLineHeight), Qt::AlignLeft | Qt::AlignVCenter,
                     painter.fontMetrics().elidedText(
                         QStringLiteral("免费复活后 10% 血量、进入虚弱(发射机构锁定),占增益点解除"),
                         Qt::ElideRight, inner));
    const QString second = !link_up_ ? QStringLiteral("断线，指令不会发送")
                           : !status_.isEmpty()
                               ? status_
                               : QStringLiteral("金币复活满血，3 秒后解除虚弱");
    painter.setPen(!link_up_ ? theme::kDanger : theme::kTextMuted);
    painter.drawText(QRect(kPad, text_top + kLineHeight, inner, kLineHeight), Qt::AlignLeft | Qt::AlignVCenter,
                     painter.fontMetrics().elidedText(second, Qt::ElideRight, inner));
}

}  // namespace rm_terminal
