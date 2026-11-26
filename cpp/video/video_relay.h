#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QUdpSocket>
#include <QtGlobal>

namespace rm_terminal
{

    // 指挥手图传墙每一路的端口:video_wall_port 加上车号(1 重装,3、4 步兵;蓝方 101 也是 +1)。
    quint16 wall_port(int base, int robot_id);

    // 地面席位把收到的每个图传 UDP 包原样转发给指挥手的终端。包不改、不重组、不补发,指挥手那边
    // 照样用 VideoReceiver 重组解码。组网网线断了只是发不出去,本机的图传和录像照常;状态变了才写日志,
    // 断着时每秒几千个包不会写几千行。
    class VideoRelay final
    {
    public:
        VideoRelay(const QHostAddress& host, quint16 port);
        void forward(const QByteArray& datagram);
        quint64 sent() const { return sent_; }
        quint64 failed() const { return failed_; }

    private:
        QUdpSocket socket_;
        QHostAddress host_;
        quint16 port_;
        quint64 sent_ = 0;
        quint64 failed_ = 0;
        bool failing_ = false;
    };

} // namespace rm_terminal
