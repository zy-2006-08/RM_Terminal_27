#include "km_control.h"

#include "rm_terminal.pb.h"

#include <QApplication>
#include <QCursor>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QStringList>
#include <QWheelEvent>
#include <QWidget>
#include <QWindow>

#include <algorithm>
#include <array>
#include <cmath>

namespace rm_terminal {

namespace {

struct KeyBit {
    int qt_key;
    const char* name;
};

// 协议 2.2.1 的位序。macOS 上 Qt::Key_Control 是 Command 键,物理 Ctrl 是 Key_Meta。
#if defined(Q_OS_MACOS)
constexpr int kCtrlKey = Qt::Key_Meta;
#else
constexpr int kCtrlKey = Qt::Key_Control;
#endif

constexpr std::array<KeyBit, 16> kKeyBits{{
    {Qt::Key_W, "W"}, {Qt::Key_S, "S"}, {Qt::Key_A, "A"}, {Qt::Key_D, "D"},
    {Qt::Key_Shift, "Shift"}, {kCtrlKey, "Ctrl"}, {Qt::Key_Q, "Q"}, {Qt::Key_E, "E"},
    {Qt::Key_R, "R"}, {Qt::Key_F, "F"}, {Qt::Key_G, "G"}, {Qt::Key_Z, "Z"},
    {Qt::Key_X, "X"}, {Qt::Key_C, "C"}, {Qt::Key_V, "V"}, {Qt::Key_B, "B"},
}};

constexpr int kWheelStep = 120;
constexpr qint64 kWarpTimeoutMs = 200;

}  // namespace

std::uint32_t km_key_bit(int qt_key) {
    for (std::size_t bit = 0; bit < kKeyBits.size(); ++bit)
        if (kKeyBits[bit].qt_key == qt_key) return 1u << bit;
    return 0;
}

QString km_keys_text(std::uint32_t keys) {
    QStringList names;
    for (std::size_t bit = 0; bit < kKeyBits.size(); ++bit)
        if (keys & (1u << bit)) names << QString::fromLatin1(kKeyBits[bit].name);
    return names.join(QLatin1Char(' '));
}

std::string encode_keyboard_mouse(const KmFrame& frame) {
    rm::KeyboardMouseControl message;
    message.set_mouse_x(frame.mouse_x);
    message.set_mouse_y(frame.mouse_y);
    message.set_mouse_z(frame.mouse_z);
    message.set_left_button_down(frame.left);
    message.set_right_button_down(frame.right);
    message.set_keyboard_value(frame.keys);
    message.set_mid_button_down(frame.middle);
    return message.SerializeAsString();
}

void KmState::key(int qt_key, bool down) {
    const std::uint32_t bit = km_key_bit(qt_key);
    if (down) keys_ |= bit;
    else keys_ &= ~bit;
}

void KmState::button(KmButton which, bool down) {
    switch (which) {
        case KmButton::Left: left_ = down; break;
        case KmButton::Right: right_ = down; break;
        case KmButton::Middle: middle_ = down; break;
    }
}

void KmState::move(double dx, double dy) {
    dx_ += dx;
    dy_ += dy;
}

void KmState::wheel(int angle_delta) { wheel_ += angle_delta; }

KmFrame KmState::take() {
    KmFrame frame;
    const double x = std::trunc(dx_);
    const double y = std::trunc(dy_);
    dx_ -= x;
    dy_ -= y;
    frame.mouse_x = static_cast<std::int32_t>(x);
    frame.mouse_y = static_cast<std::int32_t>(-y);
    frame.mouse_z = wheel_ / kWheelStep;
    wheel_ -= frame.mouse_z * kWheelStep;
    frame.left = left_;
    frame.right = right_;
    frame.middle = middle_;
    frame.keys = keys_;
    return frame;
}

void KmState::releaseAll() { *this = KmState{}; }

std::int64_t KmClock::dueIn(std::int64_t now_ns) const { return std::max<std::int64_t>(next_ns_ - now_ns, 0); }

void KmClock::sent(std::int64_t now_ns) {
    next_ns_ += kPeriodNs;
    next_ns_ = std::max(next_ns_, now_ns + kMinGapNs);
}

KmCapture::KmCapture(QWidget* window, QObject* parent) : QObject(parent), window_(window) {
    const QString platform = QGuiApplication::platformName();
    pointer_lock_ = platform == QLatin1String("xcb") || platform == QLatin1String("cocoa") ||
                    platform == QLatin1String("windows");
    timer_.setSingleShot(true);
    timer_.setTimerType(Qt::PreciseTimer);
    QObject::connect(&timer_, &QTimer::timeout, this, [this] { tick(); });
    elapsed_.start();
}

KmCapture::~KmCapture() { stop(QStringLiteral("程序退出")); }

void KmCapture::setSender(Sender sender) { sender_ = std::move(sender); }

void KmCapture::setLinkUp(bool up) {
    link_up_ = up;
    if (!up && active_) stop(QStringLiteral("断线"));
}

bool KmCapture::start() {
    if (active_) return true;
    if (!link_up_ || !sender_ || !window_) return false;
    active_ = true;
    suspended_ = false;
    state_.releaseAll();
    frames_ = 0;
    grab();
    clock_.reset(elapsed_.nsecsElapsed());
    tick();
    Q_EMIT activeChanged(true, QString());
    return active_;
}

void KmCapture::stop(const QString& reason) {
    if (!active_) return;
    active_ = false;
    suspended_ = false;
    timer_.stop();
    ungrab();
    state_.releaseAll();
    if (link_up_) send(state_.take());
    Q_EMIT activeChanged(false, reason);
}

void KmCapture::setSuspended(bool suspended) {
    if (!active_ || suspended == suspended_) return;
    if (suspended) {
        suspended_ = true;
        ungrab();
        state_.releaseAll();
        Q_EMIT suspendedChanged(true);
        return;
    }
    if (!window_ || !window_->isActiveWindow()) {
        stop(QStringLiteral("复活时窗口不在前台"));
        return;
    }
    suspended_ = false;
    state_.releaseAll();
    grab();
    Q_EMIT suspendedChanged(false);
}

void KmCapture::grab() {
    if (grabbed_ || !window_) return;
    grabbed_ = true;
    qApp->installEventFilter(this);
    window_->activateWindow();
    window_->grabMouse();
    QGuiApplication::setOverrideCursor(QCursor(Qt::BlankCursor));
    last_ = QCursor::pos();
    center_ = QPointF(window_->mapToGlobal(window_->rect().center()));
    awaiting_warp_ = false;
    if (pointer_lock_) {
        QCursor::setPos(center_.toPoint());
        awaiting_warp_ = true;
        warp_at_ms_ = elapsed_.elapsed();
    }
}

void KmCapture::ungrab() {
    if (!grabbed_) return;
    grabbed_ = false;
    qApp->removeEventFilter(this);
    if (window_) window_->releaseMouse();
    QGuiApplication::restoreOverrideCursor();
}

void KmCapture::toggle() {
    if (active_) stop(QStringLiteral("再按一次开关"));
    else start();
}

bool KmCapture::send(const KmFrame& frame) {
    if (!sender_) return false;
    if (!sender_("KeyboardMouseControl", encode_keyboard_mouse(frame))) return false;
    ++frames_;
    return true;
}

void KmCapture::tick() {
    if (!active_) return;
    const qint64 now = elapsed_.nsecsElapsed();
    if (clock_.dueIn(now) > 0) {
        schedule();
        return;
    }
    if (!send(state_.take())) {
        stop(QStringLiteral("发送失败"));
        return;
    }
    clock_.sent(now);
    schedule();
}

void KmCapture::schedule() {
    const std::int64_t wait_ns = clock_.dueIn(elapsed_.nsecsElapsed());
    timer_.start(static_cast<int>((wait_ns + 999'999) / 1'000'000));
}

void KmCapture::move(const QPointF& global) {
    if (!pointer_lock_ || !window_) {
        state_.move(global.x() - last_.x(), global.y() - last_.y());
        last_ = global;
        return;
    }
    const double near = std::max(8.0, std::min(window_->width(), window_->height()) / 8.0);
    const double far = std::max(near * 2.0, std::min(window_->width(), window_->height()) / 4.0);
    const QPointF from_center = global - center_;
    if (awaiting_warp_) {
        if (std::abs(from_center.x()) <= near && std::abs(from_center.y()) <= near) {
            state_.move(from_center.x(), from_center.y());
            last_ = global;
            awaiting_warp_ = false;
            return;
        }
        if (elapsed_.elapsed() - warp_at_ms_ > kWarpTimeoutMs) awaiting_warp_ = false;
    }
    state_.move(global.x() - last_.x(), global.y() - last_.y());
    last_ = global;
    if (!awaiting_warp_ && (std::abs(from_center.x()) > far || std::abs(from_center.y()) > far)) {
        QCursor::setPos(center_.toPoint());
        awaiting_warp_ = true;
        warp_at_ms_ = elapsed_.elapsed();
    }
}

bool KmCapture::eventFilter(QObject* watched, QEvent* event) {
    if (!active_ || suspended_) return false;
    switch (event->type()) {
        case QEvent::KeyPress:
        case QEvent::KeyRelease: {
            const auto* key = static_cast<QKeyEvent*>(event);
            const bool press = event->type() == QEvent::KeyPress;
            if (key->key() == Qt::Key_Escape || key->key() == Qt::Key_F9) {
                if (press && !key->isAutoRepeat())
                    stop(key->key() == Qt::Key_Escape ? QStringLiteral("按 Esc 退出")
                                                      : QStringLiteral("按 F9 退出"));
                return true;
            }
            if (!key->isAutoRepeat()) state_.key(key->key(), press);
            return true;
        }
        case QEvent::ShortcutOverride:
            event->accept();
            return true;
        case QEvent::MouseMove:
            move(static_cast<QMouseEvent*>(event)->globalPosition());
            return true;
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick:
        case QEvent::MouseButtonRelease: {
            const bool down = event->type() != QEvent::MouseButtonRelease;
            switch (static_cast<QMouseEvent*>(event)->button()) {
                case Qt::LeftButton: state_.button(KmButton::Left, down); break;
                case Qt::RightButton: state_.button(KmButton::Right, down); break;
                case Qt::MiddleButton: state_.button(KmButton::Middle, down); break;
                default: break;
            }
            return true;
        }
        case QEvent::Wheel:
            state_.wheel(static_cast<QWheelEvent*>(event)->angleDelta().y());
            return true;
        case QEvent::ContextMenu:
            return true;
        case QEvent::WindowDeactivate:
            if (watched == window_.data() ||
                (window_ && watched == static_cast<QObject*>(window_->windowHandle())))
                stop(QStringLiteral("窗口失去焦点"));
            return false;
        case QEvent::ApplicationStateChange:
            if (QGuiApplication::applicationState() != Qt::ApplicationActive)
                stop(QStringLiteral("窗口失去焦点"));
            return false;
        default:
            return false;
    }
}

}  // namespace rm_terminal
