#include "video_relay.h"

#include "logging.h"

namespace rm_terminal
{

    quint16 wall_port(int base, int robot_id)
    {
        return static_cast<quint16>(base + (robot_id > 100 ? robot_id - 100 : robot_id));
    }

    VideoRelay::VideoRelay(const QHostAddress& host, quint16 port) : host_(host), port_(port)
    {
        emit_log(LogLevel::info, QStringLiteral("video_relay"),
                 {QStringLiteral("state=on"), QStringLiteral("target=%1:%2").arg(host_.toString()).arg(port_)});
    }

    void VideoRelay::forward(const QByteArray& datagram)
    {
        const bool ok = socket_.writeDatagram(datagram, host_, port_) == datagram.size();
        if (ok)
            ++sent_;
        else
            ++failed_;
        if (ok != failing_)
            return;
        failing_ = !ok;
        emit_log(ok ? LogLevel::info : LogLevel::warning, QStringLiteral("video_relay"),
                 {QStringLiteral("state=%1").arg(ok ? QStringLiteral("sending") : QStringLiteral("failing")),
                  QStringLiteral("target=%1:%2").arg(host_.toString()).arg(port_),
                  QStringLiteral("error=%1").arg(ok ? QStringLiteral("-") : socket_.errorString()),
                  QStringLiteral("sent=%1").arg(sent_), QStringLiteral("failed=%1").arg(failed_)});
    }

} // namespace rm_terminal
