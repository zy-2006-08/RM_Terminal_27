#include "role_commands.h"

#include "rm_terminal.pb.h"
#include "theme.h"

#include <QPainter>

#include <algorithm>

namespace rm_terminal {

std::string encode_assembly(std::uint32_t operation, std::uint32_t difficulty) {
    rm::AssemblyCommand message;
    message.set_operation(operation);
    message.set_difficulty(difficulty);
    return message.SerializeAsString();
}

std::string encode_dart(std::uint32_t target_id, bool open, bool launch_confirm) {
    rm::DartCommand message;
    message.set_target_id(target_id);
    message.set_open(open);
    message.set_launch_confirm(launch_confirm);
    return message.SerializeAsString();
}

std::string encode_sentry(std::uint32_t command_id) {
    rm::SentryCtrlCommand message;
    message.set_command_id(command_id);
    return message.SerializeAsString();
}

std::string encode_common(std::uint32_t cmd_type, std::uint32_t param) {
    rm::CommonCommand message;
    message.set_cmd_type(cmd_type);
    message.set_param(param);
    return message.SerializeAsString();
}

std::string encode_performance(std::uint32_t shooter, std::uint32_t chassis) {
    rm::RobotPerformanceSelectionCommand message;
    message.set_shooter(shooter);
    message.set_chassis(chassis);
    return message.SerializeAsString();
}

std::string encode_rune_activate() {
    rm::RuneActivateCommand message;
    message.set_activate(1);
    return message.SerializeAsString();
}

CommandGate::CommandGate(MonotonicMs min_interval_ms)
    : interval_ms_(std::max<MonotonicMs>(min_interval_ms, 0)) {}

bool CommandGate::allow(MonotonicMs now) {
    // 时钟倒退本不该发生(monotonic_now 保证单调),但快照回放会把 now 重置。
    // 当成「间隔已过」处理,而不是把闸门永久锁死。
    if (sent_once_ && now >= last_sent_ && now - last_sent_ < interval_ms_) return false;
    last_sent_ = now;
    sent_once_ = true;
    return true;
}

void CommandGate::reset() {
    sent_once_ = false;
    last_sent_ = 0;
}

HoldGate::HoldGate(MonotonicMs hold_ms) : hold_ms_(std::max<MonotonicMs>(hold_ms, 1)) {}

void HoldGate::press(MonotonicMs now) {
    pressed_ = true;
    pressed_at_ = now;
}

bool HoldGate::release(MonotonicMs now) {
    if (!pressed_) return false;
    const bool fired = now - pressed_at_ >= hold_ms_;
    pressed_ = false;
    pressed_at_ = 0;
    return fired;
}

void HoldGate::cancel() {
    pressed_ = false;
    pressed_at_ = 0;
}

double HoldGate::progress(MonotonicMs now) const {
    if (!pressed_) return 0.0;
    const double held = static_cast<double>(now - pressed_at_);
    return std::clamp(held / static_cast<double>(hold_ms_), 0.0, 1.0);
}

bool HoldGate::ready(MonotonicMs now) const {
    return pressed_ && now - pressed_at_ >= hold_ms_;
}

EchoWatch::EchoWatch(MonotonicMs timeout_ms)
    : timeout_ms_(std::max<MonotonicMs>(timeout_ms, 0)) {}

void EchoWatch::sent(MonotonicMs now, std::uint32_t tag, std::uint64_t baseline) {
    sent_at_ = now;
    tag_ = tag;
    baseline_ = baseline;
    result_ = EchoResult{EchoStatus::Pending, 0};
}

EchoResult EchoWatch::update(MonotonicMs now, std::uint64_t state) {
    if (result_.status != EchoStatus::Pending) return result_;
    if (state != baseline_) {
        result_ = EchoResult{EchoStatus::Confirmed, 0};
        return result_;
    }
    if (now - sent_at_ >= timeout_ms_) result_ = EchoResult{EchoStatus::Unconfirmed, 0};
    return result_;
}

EchoResult EchoWatch::applyResult(MonotonicMs now, std::uint32_t tag, std::uint32_t code) {
    (void)now;
    // 超时后迟到的结果照样收:操作手要知道那条指令最终成了还是败了。
    // 但不接受别的 command_id 的回复 —— 那会把失败归到无关的按钮上。
    if (result_.status == EchoStatus::Idle || result_.status == EchoStatus::Confirmed ||
        result_.status == EchoStatus::Failed)
        return result_;
    if (tag != tag_) return result_;
    result_ = code == 0 ? EchoResult{EchoStatus::Confirmed, 0} : EchoResult{EchoStatus::Failed, code};
    return result_;
}

void EchoWatch::clear() {
    result_ = EchoResult{};
    tag_ = 0;
    sent_at_ = 0;
    baseline_ = 0;
}

QString echo_status_text(const EchoResult& echo) {
    switch (echo.status) {
        case EchoStatus::Idle: return QString();
        case EchoStatus::Pending: return QStringLiteral("等待回显");
        case EchoStatus::Confirmed: return QStringLiteral("已确认");
        case EchoStatus::Unconfirmed: return QStringLiteral("未确认");
        case EchoStatus::Failed: return QStringLiteral("失败（码 %1）").arg(echo.code);
    }
    return QString();
}

namespace {

// 操作手是坐着看 1080p 屏的,按钮不能小:高度 32、字号 11pt 是 role-layouts
// 「风格规定」里可读性的下限,三个面板共用同一份,避免逐个面板漂移。
constexpr int kButtonHeight = 32;
constexpr int kButtonPoint = 11;

QString base_rules(const QColor& idle_bg, const QColor& hover_bg, const QColor& border) {
    return QStringLiteral(R"(
QPushButton {
    background: %1;
    color: %2;
    border: 1px solid %3;
    border-radius: 0px;
    padding: 4px 8px;
    min-height: %7px;
    font-family: "PingFang SC";
    font-size: %8pt;
}
QPushButton:hover:enabled { background: %4; border-color: %5; }
QPushButton:pressed:enabled { background: %5; }
QPushButton:checked { background: %5; color: %6; border-color: %6; }
QPushButton:disabled { background: %9; color: %10; border-color: %9; }
)")
        .arg(idle_bg.name(), theme::kTextPrimary.name(), border.name(), hover_bg.name(),
             theme::kCyanDim.name(), theme::kCyan.name())
        .arg(kButtonHeight)
        .arg(kButtonPoint)
        .arg(theme::kOffline.name(), theme::kTextMuted.name());
}

}  // namespace

QString command_button_style() {
    return base_rules(theme::kSurfaceRaised, theme::kBorder, theme::kBorder);
}

QString hold_button_style(double progress) {
    const double stop = std::clamp(progress, 0.0, 1.0);
    // 已按住的比例用斜切不方便(样式表没有路径),改成横向渐变:左侧实心到 stop,
    // 右侧仍是普通底色,读起来就是一条正在填满的进度。
    const QString fill =
        QStringLiteral("qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 %1, stop:%2 %1, "
                       "stop:%3 %4, stop:1 %4)")
            .arg(theme::kCyanDim.name())
            .arg(stop, 0, 'f', 4)
            .arg(std::min(stop + 0.001, 1.0), 0, 'f', 4)
            .arg(theme::kSurfaceRaised.name());
    return base_rules(theme::kSurfaceRaised, theme::kBorder, theme::kBorderBright) +
           QStringLiteral("QPushButton:enabled { background: %1; }").arg(fill);
}

namespace role_paint {

void tile(QPainter& painter, const QRect& box, const QString& label, const QString& value,
          const QColor& ink) {
    painter.setPen(Qt::NoPen);
    painter.setBrush(theme::kSurfaceRaised);
    painter.drawPath(theme::skewPath(QRectF(box), 5));

    const int pad = 8;
    painter.setFont(theme::labelFont(9));
    painter.setPen(theme::kTextMuted);
    painter.drawText(QRect(box.left() + pad, box.top() + 3, box.width() - pad * 2, 13),
                     Qt::AlignLeft | Qt::AlignVCenter, label);

    painter.setFont(theme::labelFont(12, true));
    painter.setPen(ink);
    // 值可能是「等待 TechCoreMotionStateSync」这种长串。省略号而不是裁断:
    // 裁断看起来像一个短的、真的值,省略号明确说明「这里还有」。
    const QRect value_box(box.left() + pad, box.top() + 15, box.width() - pad * 2,
                          box.height() - 17);
    painter.drawText(value_box, Qt::AlignLeft | Qt::AlignVCenter,
                     painter.fontMetrics().elidedText(value, Qt::ElideRight, value_box.width()));
}

void row(QPainter& painter, const QRect& box, const QString& label, const QString& value,
         const QColor& ink) {
    painter.setFont(theme::labelFont(10));
    painter.setPen(theme::kTextMuted);
    const int label_w = painter.fontMetrics().horizontalAdvance(label) + 6;
    painter.drawText(box, Qt::AlignLeft | Qt::AlignVCenter, label);

    painter.setFont(theme::labelFont(10, true));
    painter.setPen(ink);
    const QRect value_box(box.left() + label_w, box.top(), box.width() - label_w, box.height());
    painter.drawText(value_box, Qt::AlignRight | Qt::AlignVCenter,
                     painter.fontMetrics().elidedText(value, Qt::ElideRight, value_box.width()));
}

void chip(QPainter& painter, const QRect& box, const QString& text, bool done,
          const QColor& accent) {
    const QPainterPath shape = theme::skewPath(QRectF(box), 4);
    painter.setPen(Qt::NoPen);
    painter.setBrush(done ? accent : theme::kSurfaceRaised);
    painter.drawPath(shape);
    if (!done) {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(theme::kBorder, 1));
        painter.drawPath(shape);
    }
    painter.setFont(theme::labelFont(10, done));
    painter.setPen(done ? theme::kTextPrimary : theme::kTextMuted);
    painter.drawText(box, Qt::AlignCenter, text);
}

}  // namespace role_paint

}  // namespace rm_terminal
