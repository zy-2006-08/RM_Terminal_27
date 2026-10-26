#include "controller_link.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <unistd.h>

namespace {

int failures = 0;

void check(bool condition, const char* label) {
    if (condition) return;
    std::fprintf(stderr, "FAIL %s\n", label);
    ++failures;
}

bool wait_until(const std::function<bool()>& done, int timeout_ms) {
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < timeout_ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (done()) return true;
        QThread::msleep(5);
    }
    return done();
}

// 一对 pty 模拟 USB 串口线:测试写 master,ControllerLink 读 slave。关掉 master
// 并删掉符号链接,就等于拔掉 USB(设备读错、路径消失)。
struct FakeCable {
    int master = -1;
    QString link;

    bool plug(const QString& at) {
        master = ::posix_openpt(O_RDWR | O_NOCTTY);
        if (master < 0 || ::grantpt(master) != 0 || ::unlockpt(master) != 0) return false;
        const char* slave = ::ptsname(master);
        if (!slave) return false;
        link = at;
        QFile::remove(link);
        return QFile::link(QString::fromLocal8Bit(slave), link);
    }
    void unplug() {
        if (master >= 0) ::close(master);
        master = -1;
        QFile::remove(link);
    }
    void send(const QByteArray& bytes) const {
        if (::write(master, bytes.constData(), static_cast<size_t>(bytes.size())) != bytes.size())
            std::fprintf(stderr, "short write to pty\n");
    }
};

QByteArray joint_frame(float first, std::uint8_t seq) {
    const float joints[6] = {first, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f};
    QByteArray data(rm_terminal::kCustomControllerDataBytes, '\0');
    std::memcpy(data.data(), joints, sizeof(joints));
    return rm_terminal::encode_referee_frame(rm_terminal::kCustomControllerCmd, data, seq);
}

void an_unconfigured_link_stays_off() {
    rm_terminal::ControllerLink link(QString(), 115200);
    link.start();
    const auto stats = link.stats();
    check(!stats.configured, "an empty path means no controller on this seat");
    check(!stats.open, "an unconfigured link never opens");
}

void frames_flow_unplug_closes_and_replug_recovers(const QString& path) {
    FakeCable cable;
    check(cable.plug(path), "fake cable plugged");

    rm_terminal::ControllerLink link(path, 115200);
    int signalled = 0;
    QByteArray last;
    QObject::connect(&link, &rm_terminal::ControllerLink::frameReceived,
                     [&](const QByteArray& data) {
                         ++signalled;
                         last = data;
                     });
    link.start();
    check(link.stats().open, "the link opens an existing serial path");

    for (std::uint8_t seq = 0; seq < 3; ++seq) cable.send(joint_frame(0.1f * (seq + 1), seq));
    check(wait_until([&] { return signalled == 3; }, 1000), "three frames are forwarded");
    auto stats = link.stats();
    check(stats.frames == 3, "three frames are counted");
    check(stats.last_frame_at.has_value(), "the last frame time is recorded");
    check(stats.joints[0] > 0.29f && stats.joints[0] < 0.31f, "joints come from the newest frame");
    check(last.size() == rm_terminal::kCustomControllerDataBytes, "the 30-byte payload is forwarded");

    // 坏帧:丢掉、计数,不转发。
    QByteArray corrupt = joint_frame(9.0f, 9);
    corrupt[20] = static_cast<char>(corrupt[20] ^ 0x40);
    cable.send(corrupt);
    check(wait_until([&] { return link.stats().bad_frames >= 1; }, 1000), "a corrupt frame is counted");
    // 回归:VMIN=0 时「暂时没数据」的 read 返回 0,被当成拔线,串口每读一批就关掉重开。
    check(link.stats().open && link.stats().error.isEmpty(),
          "an idle line is not mistaken for an unplugged cable");
    check(signalled == 3, "a corrupt frame is never forwarded");

    // 拔线:设备读错 + 路径消失。
    cable.unplug();
    check(wait_until([&] { return !link.stats().open; }, 1000), "unplugging closes the link");
    check(!link.stats().error.isEmpty(), "the close reason is reported");
    check(wait_until([&] { return link.stats().error.contains(QStringLiteral("找不到串口")); }, 1500),
          "retries report the missing path");
    const int before = signalled;
    QCoreApplication::processEvents();
    check(signalled == before, "nothing is forwarded while unplugged");

    // 插回:同一路径重新出现,自动重开,不用重启终端。
    check(cable.plug(path), "fake cable plugged back");
    check(wait_until([&] { return link.stats().open; }, 2000), "replugging reopens the link");
    cable.send(joint_frame(0.7f, 10));
    check(wait_until([&] { return signalled == before + 1; }, 1000), "frames flow again after replug");
    cable.unplug();
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    if (!dir.isValid()) {
        std::fprintf(stderr, "FAIL temporary directory unavailable\n");
        return 1;
    }
    an_unconfigured_link_stays_off();
    frames_flow_unplug_closes_and_replug_recovers(dir.filePath(QStringLiteral("arm")));
    if (failures != 0) {
        std::fprintf(stderr, "controller_link_test: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("controller_link_test: all checks passed\n");
    return 0;
}
