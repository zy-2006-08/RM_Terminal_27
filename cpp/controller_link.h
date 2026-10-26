#pragma once

#include "domain.h"
#include "referee_frame.h"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTimer>

#include <array>
#include <cstdint>
#include <optional>

class QSocketNotifier;

namespace rm_terminal {

// 自定义控制器(小机械臂)的串口链路:打开串口、读 0x0302 帧、断了每 500ms 重开一次。
// 不知道 MQTT 的存在 —— 转不转发由上层决定,所以单元测试用一个 pty 就能跑全部行为。
//
// 用 POSIX termios 而不是 QSerialPort:后者要多装一个 Qt 模块(apt/brew/CI 三处都要
// 改),而这里只需要 115200 8N1 读写,macOS 和 Linux 的 termios 都够用。
class ControllerLink final : public QObject {
    Q_OBJECT
public:
    // path 为空表示本席位没接控制器:不开口、不重试,状态报「未启用」。
    ControllerLink(QString path, int baud, QObject* parent = nullptr);
    ~ControllerLink() override;

    void start();

    struct Stats {
        bool configured = false;
        bool open = false;
        QString path;
        // 最近一次打开失败或断开的原因;串口正常打开后清空。
        QString error;
        std::uint64_t frames = 0;
        std::uint64_t bad_frames = 0;
        std::optional<MonotonicMs> last_frame_at;
        // 弧度。固件的顺序:0-3 机械臂关节,4 夹爪,5 臂 yaw。
        std::array<float, 6> joints{};
    };
    Stats stats() const;

Q_SIGNALS:
    // 一帧校验通过的 0x0302 数据段,固定 30 字节。只在真的收到新帧时发,从不补发旧帧:
    // 线一断就再也没有这个信号,下游自然停发 —— 车上的机械臂不会按最后一帧一直动。
    void frameReceived(const QByteArray& data);

private:
    void tryOpen();
    void onReadable();
    void closePort(const QString& reason);
    void drainFrames();

    QString path_;
    int baud_;
    int fd_ = -1;
    QSocketNotifier* notifier_ = nullptr;
    QTimer retry_;
    RefereeFrameParser parser_;
    Stats stats_;
    // 打不开时每 500ms 重试一次;只在原因变化时写日志,否则一场没接控制器的比赛会
    // 刷几千行一模一样的失败。
    QString logged_error_;
    // 坏帧最多每秒记一条累计数:线缆接触不良时每秒可能几十个坏帧。
    std::uint64_t logged_bad_frames_ = 0;
    std::optional<MonotonicMs> bad_logged_at_;
};

}  // namespace rm_terminal
