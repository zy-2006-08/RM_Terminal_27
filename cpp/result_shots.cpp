#include "dashboard.h"
#include "game_result.h"

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QThread>
#include <iostream>

using namespace rm_terminal;

namespace {

constexpr int kWidth = 1440;
constexpr int kHeight = 900;

template <typename T>
Field<T> present(T value) {
    Field<T> field;
    field.value = value;
    field.quality = Quality::Valid;
    field.freshness = Freshness::Fresh;
    return field;
}

// 结算快照:阶段 5 + 可信链路 + 具体 winner,这三者齐了 resolve_game_result 才出结果。
Snapshot settlement(std::uint32_t winner) {
    Snapshot snapshot;
    snapshot.game.current_stage = present<std::uint32_t>(5);
    snapshot.game.stage_countdown_sec = present<std::int32_t>(12);
    snapshot.game.winner = present<std::uint32_t>(winner);

    MapRobot self;
    self.id = RobotId{3};
    self.faction = 1;
    self.is_self = true;
    self.position.x = present<double>(6.0);
    self.position.y = present<double>(7.5);
    self.position.yaw = present<double>(0.0);
    snapshot.map_robots.push_back(self);

    RobotState state;
    state.dynamic.current_hp = present<std::uint32_t>(412);
    state.dynamic.max_hp = present<std::uint32_t>(600);
    snapshot.robots.emplace(RobotId{3}, state);
    return snapshot;
}

void flush_frames(int ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(5);
    }
}

// 非黑像素占比。逐帧素材真的贴上来时画面会亮起一大片;只有压暗层时这个值接近 0。
// 「帧清单非空」只证明磁盘有文件,证明不了 QML 真的把它画出来了,所以这里看像素。
double lit_ratio(const QImage& image) {
    long long lit = 0;
    long long total = 0;
    for (int y = 0; y < image.height(); y += 4) {
        for (int x = 0; x < image.width(); x += 4) {
            ++total;
            const QColor c = image.pixelColor(x, y);
            if (c.red() + c.green() + c.blue() > 120) ++lit;
        }
    }
    return total > 0 ? static_cast<double>(lit) / static_cast<double>(total) : 0.0;
}

void shoot(Dashboard& dashboard, std::uint32_t winner, MonotonicMs now,
           const QString& path, const char* label, int settle_ms = 1500) {
    const Snapshot snapshot = settlement(winner);
    const GameResult result = resolve_game_result(game_result_inputs(snapshot));
    const QStringList urls = game_result_frame_urls(result);

    dashboard.update(snapshot, nullptr, now);
    dashboard.show();
    // 800ms 放大 + 10ms/帧:等够 1.5s 才能确认动画推进到末帧并停住。
    flush_frames(settle_ms);

    const QPixmap grabbed = dashboard.grab();
    if (!grabbed.save(path)) {
        std::cout << "FAILED to write " << path.toStdString() << '\n';
        return;
    }
    std::cout << label << " -> " << path.toStdString()
              << "  标题=[" << game_result_title(result).toStdString() << "]"
              << " 帧数=" << urls.size()
              << " 点亮率=" << lit_ratio(grabbed.toImage()) << '\n';
    if (!urls.isEmpty()) {
        std::cout << "    首帧=" << urls.front().toStdString() << '\n'
                  << "    末帧=" << urls.back().toStdString() << '\n';
    }
}

}  // namespace

// 验收工具:把三种结算结果各渲一张图,并报告帧数与画面点亮率。
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    const QString out = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("/tmp");

    Dashboard dashboard(Config{});
    dashboard.resize(kWidth, kHeight);

    MonotonicMs now = 1000;
    const auto next = [&now] { return now += 6000; };

    shoot(dashboard, 1, now, out + "/result-1-red-win.png", "红方胜利");
    shoot(dashboard, 2, next(), out + "/result-2-blue-win.png", "蓝方胜利");
    shoot(dashboard, 0, next(), out + "/result-3-draw.png", "平局");
    return 0;
}
