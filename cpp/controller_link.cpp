#include "controller_link.h"

#include "clock.h"
#include "logging.h"

#include <QDir>
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
// "auto" 时每个口听多久。控制器 25Hz:开口时多半截到半帧,下一整帧 80ms 内一定到。
constexpr int kProbeMs = 300;

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

QStringList device_files(const QString& dir, const QStringList& patterns) {
    const QDir folder(dir);
    QStringList out;
    for (const QString& name :
         folder.entryList(patterns, QDir::AllEntries | QDir::System | QDir::NoDotAndDotDot, QDir::Name))
        out << folder.filePath(name);
    return out;
}

// 插着的 USB 串口。Linux 用 /dev/serial/by-id 下的名字:带芯片型号和序列号,日志里认得出是哪块板。
QStringList usb_serial_ports() {
#if defined(Q_OS_MACOS)
    return device_files(QStringLiteral("/dev"), {QStringLiteral("cu.usbserial*"), QStringLiteral("cu.usbmodem*"),
                                                 QStringLiteral("cu.wchusbserial*")});
#else
    QStringList ports = device_files(QStringLiteral("/dev/serial/by-id"), {QStringLiteral("*")});
    if (ports.isEmpty())
        ports = device_files(QStringLiteral("/dev"), {QStringLiteral("ttyACM*"), QStringLiteral("ttyUSB*")});
    return ports;
#endif
}

}  // namespace

ControllerLink::ControllerLink(QString path, int baud, QObject* parent)
    : QObject(parent), path_(std::move(path)), baud_(baud) {
    stats_.configured = !path_.isEmpty();
    stats_.path = path_;
    auto_ = path_ == QLatin1String("auto");
    lister_ = usb_serial_ports;
    retry_.setSingleShot(true);
    connect(&retry_, &QTimer::timeout, this, &ControllerLink::tryOpen);
    probe_.setSingleShot(true);
    connect(&probe_, &QTimer::timeout, this, &ControllerLink::probeFailed);
}

ControllerLink::~ControllerLink() {
    delete notifier_;
    if (fd_ >= 0) ::close(fd_);
}

void ControllerLink::start() {
    if (stats_.configured) tryOpen();
}

void ControllerLink::setCandidateLister(std::function<QStringList()> lister) { lister_ = std::move(lister); }

ControllerLink::Stats ControllerLink::stats() const {
    Stats copy = stats_;
    copy.bad_frames = parser_.badFrames() - ignored_bad_;
    return copy;
}

void ControllerLink::tryOpen() {
    if (auto_) {  // 新一轮:重新列一遍插着的口
        candidates_ = lister_();
        next_candidate_ = 0;
        probeNext();
        return;
    }
    QString reason;
    const int fd = openPort(path_, &reason);
    if (fd < 0) {
        retryLater(reason);
        return;
    }
    attach(fd, path_);
    markOpen();
}

int ControllerLink::openPort(const QString& path, QString* reason) const {
    const QByteArray native = QFile::encodeName(path);
    const int fd = ::open(native.constData(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        *reason = open_error_text(errno, path);
        return -1;
    }
    const auto fail = [fd, reason](const QString& why) {
        ::close(fd);
        *reason = why;
        return -1;
    };

    termios tio{};
    if (::tcgetattr(fd, &tio) != 0) return fail(QStringLiteral("%1 不是串口设备").arg(path));
    const auto speed = termios_speed(baud_);
    if (!speed) return fail(QStringLiteral("不支持的波特率 %1").arg(baud_));
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
    if (::tcsetattr(fd, TCSANOW, &tio) != 0)
        return fail(QStringLiteral("串口设置失败：%1").arg(QString::fromLocal8Bit(std::strerror(errno))));
    // 打开前积在驱动里的字节是上一次连接的残留,喂给解析器只会多出几帧旧姿态。
    ::tcflush(fd, TCIFLUSH);
    return fd;
}

void ControllerLink::attach(int fd, const QString& path) {
    fd_ = fd;
    port_ = path;
    parser_.clearBuffer();
    notifier_ = new QSocketNotifier(fd_, QSocketNotifier::Read, this);
    connect(notifier_, &QSocketNotifier::activated, this, &ControllerLink::onReadable);
}

void ControllerLink::detach() {
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
}

void ControllerLink::markOpen() {
    stats_.open = true;
    stats_.path = port_;
    stats_.error.clear();
    logged_error_.clear();
    StructuredLog::write(LogLevel::info, QStringLiteral("controller_link"),
                         {QStringLiteral("state=open"), QStringLiteral("path=%1").arg(port_),
                          QStringLiteral("baud=%1").arg(baud_)});
}

void ControllerLink::retryLater(const QString& reason) {
    stats_.error = reason;
    if (reason != logged_error_) {
        StructuredLog::write(LogLevel::warning, QStringLiteral("controller_link"),
                             {QStringLiteral("state=open_failed"), QStringLiteral("path=%1").arg(path_),
                              QStringLiteral("reason=%1").arg(reason)});
        logged_error_ = reason;
    }
    retry_.start(kRetryMs);
}

// 试这一轮的下一个口。一轮试完都不是控制器就等 500ms,由 tryOpen 重新列口再试。
void ControllerLink::probeNext() {
    while (next_candidate_ < candidates_.size()) {
        const QString path = candidates_.at(next_candidate_++);
        QString reason;
        const int fd = openPort(path, &reason);
        if (fd < 0) continue;  // 别的程序占着、没权限的口跳过
        attach(fd, path);
        probing_ = true;
        probe_bad_at_ = parser_.badFrames();
        if (stats_.error.isEmpty()) stats_.error = QStringLiteral("正在找控制器");
        probe_.start(kProbeMs);
        return;
    }
    retryLater(candidates_.isEmpty()
                   ? QStringLiteral("没找到 USB 串口：插上控制器的串口线")
                   : QStringLiteral("插着 %1 个串口，都没收到控制器的数据").arg(candidates_.size()));
}

// 这个口听满 kProbeMs 也没有控制器的帧,或者听的时候断了:不是它,换下一个。
void ControllerLink::probeFailed() {
    probe_.stop();
    probing_ = false;
    ignored_bad_ += parser_.badFrames() - probe_bad_at_;
    detach();
    probeNext();
}

bool ControllerLink::send(const QByteArray& frame) {
    if (fd_ < 0 || probing_) return false;
    const ssize_t wrote = ::write(fd_, frame.constData(), static_cast<size_t>(frame.size()));
    if (wrote != frame.size()) return false;
    ++stats_.sent;
    return true;
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
        if (probing_) probeFailed();
        else closePort(reason);
        return;
    }
    drainFrames();
    // 还没认定是控制器:这些字节可能来自别的设备,坏帧不记日志。
    if (probing_) return;

    const std::uint64_t bad = parser_.badFrames() - ignored_bad_;
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
        if (probing_) {  // 收到第一帧控制器数据:就是这个口
            probing_ = false;
            probe_.stop();
            markOpen();
        }
        ++stats_.frames;
        stats_.last_frame_at = monotonic_now();
        for (std::size_t index = 0; index < stats_.joints.size(); ++index)
            stats_.joints[index] =
                qFromLittleEndian<float>(frame->data.constData() + index * sizeof(float));
        Q_EMIT frameReceived(frame->data);
    }
}

void ControllerLink::closePort(const QString& reason) {
    detach();
    stats_.open = false;
    stats_.error = reason;
    logged_error_ = reason;
    StructuredLog::write(LogLevel::warning, QStringLiteral("controller_link"),
                         {QStringLiteral("state=closed"), QStringLiteral("path=%1").arg(port_),
                          QStringLiteral("reason=%1").arg(reason)});
    retry_.start(kRetryMs);
}

}  // namespace rm_terminal
