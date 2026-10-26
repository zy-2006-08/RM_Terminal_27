#include "controller_link.h"

#include "clock.h"
#include "logging.h"

#include <QFile>
#include <QSocketNotifier>
#include <QtEndian>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

namespace rm_terminal {
namespace {

constexpr int kRetryMs = 500;

std::optional<speed_t> termios_speed(int baud) {
    switch (baud) {
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
#ifdef B460800
    case 460800: return B460800;
#endif
#ifdef B921600
    case 921600: return B921600;
#endif
    default: return std::nullopt;
    }
}

QString open_error_text(int code, const QString& path) {
    if (code == ENOENT) return QStringLiteral("找不到串口 %1").arg(path);
    // Linux 上 /dev/ttyUSB* 默认只给 dialout 组读写,第一次接真机几乎必然撞上。
    if (code == EACCES)
        return QStringLiteral("没有串口权限 %1（Linux 要把用户加进 dialout 组后重新登录）").arg(path);
    return QStringLiteral("串口打不开：%1").arg(QString::fromLocal8Bit(std::strerror(code)));
}

}  // namespace

ControllerLink::ControllerLink(QString path, int baud, QObject* parent)
    : QObject(parent), path_(std::move(path)), baud_(baud) {
    stats_.configured = !path_.isEmpty();
    stats_.path = path_;
    retry_.setSingleShot(true);
    connect(&retry_, &QTimer::timeout, this, &ControllerLink::tryOpen);
}

ControllerLink::~ControllerLink() {
    delete notifier_;
    if (fd_ >= 0) ::close(fd_);
}

void ControllerLink::start() {
    if (stats_.configured) tryOpen();
}

ControllerLink::Stats ControllerLink::stats() const {
    Stats copy = stats_;
    copy.bad_frames = parser_.badFrames();
    return copy;
}

void ControllerLink::tryOpen() {
    const auto fail = [this](const QString& reason, int fd) {
        if (fd >= 0) ::close(fd);
        stats_.error = reason;
        if (reason != logged_error_) {
            StructuredLog::write(LogLevel::warning, QStringLiteral("controller_link"),
                                 {QStringLiteral("state=open_failed"),
                                  QStringLiteral("path=%1").arg(path_),
                                  QStringLiteral("reason=%1").arg(reason)});
            logged_error_ = reason;
        }
        retry_.start(kRetryMs);
    };

    const QByteArray native = QFile::encodeName(path_);
    const int fd = ::open(native.constData(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        fail(open_error_text(errno, path_), -1);
        return;
    }

    termios tio{};
    if (::tcgetattr(fd, &tio) != 0) {
        fail(QStringLiteral("%1 不是串口设备").arg(path_), fd);
        return;
    }
    const auto speed = termios_speed(baud_);
    if (!speed) {
        fail(QStringLiteral("不支持的波特率 %1").arg(baud_), fd);
        return;
    }
    ::cfmakeraw(&tio);
    tio.c_cflag &= static_cast<tcflag_t>(~(CSIZE | CSTOPB | PARENB | CRTSCTS));
    tio.c_cflag |= CS8 | CLOCAL | CREAD;
    // VMIN 必须是 1。VMIN=0/VTIME=0 时 Linux 在「暂时没数据」时让 read 返回 0 而不是
    // EAGAIN(O_NONBLOCK 排在后面才检查),而 0 在这里的意思是对端断开 —— 每读完一批
    // 数据就会误判拔线、关口重开,重开时的 tcflush 还会把刚到的帧冲掉。
    tio.c_cc[VMIN] = 1;
    tio.c_cc[VTIME] = 0;
    ::cfsetispeed(&tio, *speed);
    ::cfsetospeed(&tio, *speed);
    if (::tcsetattr(fd, TCSANOW, &tio) != 0) {
        fail(QStringLiteral("串口设置失败：%1").arg(QString::fromLocal8Bit(std::strerror(errno))),
             fd);
        return;
    }
    // 打开前积在驱动里的字节是上一次连接的残留,喂给解析器只会多出几帧旧姿态。
    ::tcflush(fd, TCIFLUSH);

    fd_ = fd;
    parser_.clearBuffer();
    stats_.open = true;
    stats_.error.clear();
    logged_error_.clear();
    notifier_ = new QSocketNotifier(fd_, QSocketNotifier::Read, this);
    connect(notifier_, &QSocketNotifier::activated, this, &ControllerLink::onReadable);
    StructuredLog::write(LogLevel::info, QStringLiteral("controller_link"),
                         {QStringLiteral("state=open"), QStringLiteral("path=%1").arg(path_),
                          QStringLiteral("baud=%1").arg(baud_)});
}

void ControllerLink::onReadable() {
    char buffer[4096];
    while (fd_ >= 0) {
        const ssize_t got = ::read(fd_, buffer, sizeof(buffer));
        if (got > 0) {
            parser_.feed(buffer, got);
            continue;
        }
        if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (got < 0 && errno == EINTR) continue;
        // read 返回 0(对端关了)或 EIO/ENXIO(USB 被拔):设备已经没了,只能关掉重开。
        const QString reason =
            got == 0 ? QStringLiteral("串口断开")
                     : QStringLiteral("串口断开：%1")
                           .arg(QString::fromLocal8Bit(std::strerror(errno)));
        drainFrames();
        closePort(reason);
        return;
    }
    drainFrames();

    const std::uint64_t bad = parser_.badFrames();
    const MonotonicMs now = monotonic_now();
    if (bad != logged_bad_frames_ && (!bad_logged_at_ || now - *bad_logged_at_ >= 1000)) {
        StructuredLog::write(LogLevel::warning, QStringLiteral("controller_link"),
                             {QStringLiteral("state=bad_frames"),
                              QStringLiteral("total=%1").arg(bad)});
        logged_bad_frames_ = bad;
        bad_logged_at_ = now;
    }
}

void ControllerLink::drainFrames() {
    while (const auto frame = parser_.next()) {
        if (frame->cmd_id != kCustomControllerCmd ||
            frame->data.size() != kCustomControllerDataBytes)
            continue;
        ++stats_.frames;
        stats_.last_frame_at = monotonic_now();
        for (std::size_t index = 0; index < stats_.joints.size(); ++index)
            stats_.joints[index] =
                qFromLittleEndian<float>(frame->data.constData() + index * sizeof(float));
        Q_EMIT frameReceived(frame->data);
    }
}

void ControllerLink::closePort(const QString& reason) {
    if (notifier_) {
        // 正处在这个 notifier 的 activated 槽里,不能当场 delete。
        notifier_->setEnabled(false);
        notifier_->deleteLater();
        notifier_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    stats_.open = false;
    stats_.error = reason;
    logged_error_ = reason;
    StructuredLog::write(LogLevel::warning, QStringLiteral("controller_link"),
                         {QStringLiteral("state=closed"), QStringLiteral("path=%1").arg(path_),
                          QStringLiteral("reason=%1").arg(reason)});
    retry_.start(kRetryMs);
}

}  // namespace rm_terminal
