#include "seat_picker.h"

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>
#include <iostream>

using namespace rm_terminal;

namespace {

// 1280x760 是程序的最小验收尺寸,席位页在这里不挤就在任何屏上都不挤。
constexpr int kWidth = 1280;
constexpr int kHeight = 760;

// QQuickWidget 在**渲染线程**上出帧,setProperty 不会同步反映到下一次 grab()。
// 只 processEvents 两轮会截到上一状态的旧帧(文案和选中框对不上),所以要泵到
// 真的过了一段时间为止。做法与 popup_shots 一致。
void flush_frames(int ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(5);
    }
}

void shoot(SeatPicker& picker, const QString& path, const char* label) {
    flush_frames(300);
    if (!picker.grab().save(path)) {
        std::cout << "FAILED to write " << path.toStdString() << '\n';
        return;
    }
    std::cout << label << " -> " << path.toStdString()
              << "  确认可用=" << (picker.canConfirm() ? "yes" : "no") << '\n';
}

}  // namespace

// 验收用出图工具,不注册为测试:它只产出图片,不做断言。三个状态各一张 ——
// 「什么都没选」和「只选了阵营」是操作手真正会看到的中间态,
// 只截最终态就看不出禁用的确认按钮和未定阵营时的灰头像长什么样。
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    const QString out = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("/tmp");

    SeatPicker picker;
    picker.resize(kWidth, kHeight);
    picker.show();

    shoot(picker, out + "/picker-0-empty.png", "两步都没选");

    picker.chooseFaction(1);
    shoot(picker, out + "/picker-1-faction.png", "只选了红方");

    picker.chooseSeat(3);
    shoot(picker, out + "/picker-2-both.png", "红方 3 号已选全");

    return 0;
}
