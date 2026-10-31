#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QString>
#include <QTimer>

#include <cstdint>
#include <functional>
#include <string>

class QWidget;

namespace rm_terminal {

// 协议 2.2.1 KeyboardMouseControl 的一帧。按键和鼠标键是「这一刻按着没有」;
// 鼠标位移和滚轮是上一帧到这一帧之间的累积量。
struct KmFrame {
    std::int32_t mouse_x = 0;  // 负值向左
    std::int32_t mouse_y = 0;  // 负值向下
    std::int32_t mouse_z = 0;  // 滚轮格数,负值向后
    bool left = false;
    bool right = false;
    bool middle = false;
    std::uint32_t keys = 0;    // bit0 W ... bit15 B
};

// Qt 键值转协议位。表外的键返回 0:它们不发给车,Esc 和 F9 留给终端退出键鼠控制。
std::uint32_t km_key_bit(int qt_key);
// 按下的键,按协议位序写成 "W A Shift",面板和日志用。
QString km_keys_text(std::uint32_t keys);

std::string encode_keyboard_mouse(const KmFrame& frame);

enum class KmButton { Left, Right, Middle };

// 键鼠状态的纯逻辑,不碰 Qt 事件,测试直接喂数。
class KmState {
public:
    void key(int qt_key, bool down);
    void button(KmButton which, bool down);
    // Qt 坐标:x 向右、y 向下为正。协议的 y 向下为负,翻转在 take() 里做。
    void move(double dx, double dy);
    // QWheelEvent::angleDelta().y():向前滚为正,一格 120。
    void wheel(int angle_delta);
    // 取出这一帧。位移取整后清零,小数部分和不满一格的滚轮留给下一帧,慢慢移也不丢。
    KmFrame take();
    // 全部抬起、累积量清零。退出键鼠控制前用它发最后一帧,车不会按着最后的键一直走。
    void releaseAll();
    std::uint32_t keys() const { return keys_; }
    bool left() const { return left_; }
    bool right() const { return right_; }

private:
    std::uint32_t keys_ = 0;
    bool left_ = false;
    bool right_ = false;
    bool middle_ = false;
    double dx_ = 0.0;
    double dy_ = 0.0;
    int wheel_ = 0;
};

// 75Hz 节拍(协议 2.1)。按理想时间表排帧,平均 75Hz;定时器晚到时下一帧最早也要隔
// 10ms,两帧不会挤在一起,落下的帧也不补发。
class KmClock {
public:
    static constexpr std::int64_t kPeriodNs = 13'333'334;  // 1e9 / 75,向上取整
    static constexpr std::int64_t kMinGapNs = 10'000'000;

    void reset(std::int64_t now_ns) { next_ns_ = now_ns; }
    std::int64_t dueIn(std::int64_t now_ns) const;
    void sent(std::int64_t now_ns);

private:
    std::int64_t next_ns_ = 0;
};

// 把终端变成键鼠来源:接管整个窗口的键盘和鼠标,按 75Hz 发 KeyboardMouseControl。
// 协议要求操作手赛前在官方选手端选定键鼠来源,终端不知道选的是谁,所以只在操作手
// 明确按下开关后才发。退出条件:Esc / F9、窗口失去焦点、断线、发送失败。每次退出都
// 补发一帧「全部抬起」。
class KmCapture final : public QObject {
    Q_OBJECT
public:
    using Sender = std::function<bool(const char* topic, const std::string& payload)>;

    explicit KmCapture(QWidget* window, QObject* parent = nullptr);
    ~KmCapture() override;

    void setSender(Sender sender);
    void setLinkUp(bool up);
    // 断线或没有发送链路时不进入,返回 false。
    bool start();
    void stop(const QString& reason);
    void toggle();
    // 阵亡时暂停,照官方选手端:放开鼠标、显示光标、不再拦键盘,操作手才能点复活框、按 Y。
    // 暂停期间照常按 75Hz 发「全部松开」。复活后恢复接管;那时窗口不在前台就直接退出,
    // 不去抢别的程序的鼠标。
    void setSuspended(bool suspended);

    bool active() const { return active_; }
    bool suspended() const { return suspended_; }
    // 平台能不能把光标拉回窗口中心。Wayland 不能:光标到屏幕边就再也转不动了。
    bool pointerLock() const { return pointer_lock_; }
    std::uint32_t keysDown() const { return state_.keys(); }
    bool leftDown() const { return state_.left(); }
    bool rightDown() const { return state_.right(); }
    qint64 framesSent() const { return frames_; }

Q_SIGNALS:
    void activeChanged(bool active, const QString& reason);
    void suspendedChanged(bool suspended);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void tick();
    void schedule();
    void grab();
    void ungrab();
    bool send(const KmFrame& frame);
    void move(const QPointF& global);

    QPointer<QWidget> window_;
    Sender sender_;
    bool link_up_ = false;
    bool active_ = false;
    bool suspended_ = false;
    bool grabbed_ = false;
    bool pointer_lock_ = false;
    KmState state_;
    KmClock clock_;
    QElapsedTimer elapsed_;
    QTimer timer_;
    QPointF center_;
    QPointF last_;
    // 刚把光标拉回中心、还没收到中心附近那一帧。期间到的事件是拉回之前排队的,
    // 照常按上一点算位移;中心附近的第一帧按中心算,拉回本身不算操作手的移动。
    bool awaiting_warp_ = false;
    qint64 warp_at_ms_ = 0;
    qint64 frames_ = 0;
};

}  // namespace rm_terminal
