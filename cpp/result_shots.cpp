#include "dashboard.h"
#include "game_result.h"

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QQuickItem>
#include <QQuickWidget>
#include <QThread>
#include <iostream>

using namespace rm_terminal;

namespace {

template <typename T>
Field<T> present(T value) {
    Field<T> field;
    field.value = value;
    field.quality = Quality::Valid;
    field.freshness = Freshness::Fresh;
    return field;
}

RobotHealth health(std::uint32_t id, std::uint32_t faction, std::optional<std::uint32_t> hp,
                   std::uint32_t max_hp) {
    RobotHealth robot;
    robot.id = RobotId{id};
    robot.faction = faction;
    if (hp) robot.current_hp = present<std::uint32_t>(*hp);
    robot.max_hp = present<std::uint32_t>(max_hp);
    return robot;
}

// 结算快照:阶段 5 + 可信链路 + 具体 winner,这三者齐了 resolve_game_result 才出结果。
// 数值刻意不对称(基地、前哨、伤害、击杀各有高低),这样对比条的两侧长度不同,
// 一张图就能看出条有没有按量程定标。
Snapshot settlement(std::uint32_t winner, std::uint32_t end_reason) {
    Snapshot snapshot;
    snapshot.self = RobotId{3};
    snapshot.game.current_stage = present<std::uint32_t>(5);
    snapshot.game.stage_countdown_sec = present<std::int32_t>(12);
    snapshot.game.winner = present<std::uint32_t>(winner);
    snapshot.game.end_reason = present<std::uint32_t>(end_reason);
    snapshot.game.current_round = present<std::uint32_t>(2);
    snapshot.game.total_rounds = present<std::uint32_t>(3);
    snapshot.game.red_score = present<std::uint32_t>(winner == 1 ? 1u : 0u);
    snapshot.game.blue_score = present<std::uint32_t>(winner == 2 ? 1u : 0u);

    snapshot.game.red_base_hp = present<std::uint32_t>(winner == 2 ? 0u : 4200u);
    snapshot.game.red_base_max_hp = present<std::uint32_t>(10000);
    snapshot.game.blue_base_hp = present<std::uint32_t>(winner == 1 ? 0u : 6800u);
    snapshot.game.blue_base_max_hp = present<std::uint32_t>(10000);
    snapshot.game.red_outpost_hp = present<std::uint32_t>(0);
    snapshot.game.red_outpost_max_hp = present<std::uint32_t>(1000);
    snapshot.game.blue_outpost_hp = present<std::uint32_t>(640);
    snapshot.game.blue_outpost_max_hp = present<std::uint32_t>(1000);
    snapshot.game.red_total_damage = present<std::uint32_t>(14820);
    snapshot.game.blue_total_damage = present<std::uint32_t>(11360);
    snapshot.game.red_control_points = present<std::uint32_t>(412);
    snapshot.game.blue_control_points = present<std::uint32_t>(377);
    snapshot.game.rune_status = present<std::uint32_t>(3);
    snapshot.game.rune_activated_arms = present<std::uint32_t>(5);
    snapshot.kills = KillTally{7, 5};

    // 一方缺一台的数据(蓝 4 号),好在图上确认缺失显示的是「--」而不是 0。
    snapshot.robot_health.push_back(health(1, kRed, 0, 400));
    snapshot.robot_health.push_back(health(3, kRed, 412, 600));
    snapshot.robot_health.push_back(health(4, kRed, 168, 600));
    snapshot.robot_health.push_back(health(7, kRed, 580, 600));
    snapshot.robot_health.push_back(health(101, kBlue, 205, 400));
    snapshot.robot_health.push_back(health(103, kBlue, 0, 600));
    snapshot.robot_health.push_back(health(104, kBlue, std::nullopt, 600));
    snapshot.robot_health.push_back(health(107, kBlue, 332, 600));

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
    state.statics.max_hp = present<std::uint32_t>(600);
    snapshot.robots.emplace(RobotId{3}, state);
    return snapshot;
}

void flush_frames(int ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
    }
}

// 非黑像素占比。自绘的爆发层亮起来时这个值明显抬升;只有压暗层时接近 0,
// 所以它是「动画真的画出来了」的廉价证据,而不是只证明属性设对了。
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

QQuickItem* overlay_root(const Dashboard& dashboard) {
    auto* host = dashboard.findChild<QQuickWidget*>(QStringLiteral("victoryOverlay"));
    return host ? host->rootObject() : nullptr;
}

// 定时截图:关掉 QML 自己的 ticker,直接把动画时钟拨到 t_ms。
//
// 必须这样做而不是 sleep(t):NumberAnimation/Timer 按墙上时间推进,grab() 抓到
// 哪一帧取决于调度抖动,同一条命令两次跑出来的图不一样,既没法逐像素比对,
// 也没法在报告里说「这是 t=1.0s 的那一帧」。
void shoot(Dashboard& dashboard, const Snapshot& snapshot, int t_ms, const QString& path,
           const char* label) {
    dashboard.update(snapshot, nullptr, 1000);
    dashboard.show();
    flush_frames(120);

    QQuickItem* root = overlay_root(dashboard);
    if (!root) {
        std::cout << "FAILED: no victory overlay root\n";
        return;
    }
    root->setProperty("autoPlay", false);
    root->setProperty("tMs", t_ms);
    // Canvas 的重画是排队的:拨完时钟要再放几轮事件循环,否则抓到的是上一帧。
    flush_frames(260);

    const QPixmap grabbed = dashboard.grab();
    if (!grabbed.save(path)) {
        std::cout << "FAILED to write " << path.toStdString() << '\n';
        return;
    }
    std::cout << label << " t=" << t_ms << "ms -> " << path.toStdString()
              << "  点亮率=" << lit_ratio(grabbed.toImage()) << '\n';
}

struct Case {
    const char* name;
    std::uint32_t winner;
    std::uint32_t end_reason;
};

struct Size {
    int w;
    int h;
};

}  // namespace

// 验收工具:三种结果 × 四个时刻 × 两种窗口尺寸。
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    const QString out = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("/tmp");

    // end_reason 各取一个有代表性的编码:1 基地摧毁、4 总剩余血量、5 平局。
    const Case cases[] = {
        {"red-win", 1, 1},
        {"blue-win", 2, 4},
        {"draw", 0, 5},
    };
    const Size sizes[] = {{1440, 900}, {2560, 1440}};
    const int times[] = {300, 1000, 2000, 3500};

    for (const Size& size : sizes) {
        Dashboard dashboard(Config{});
        dashboard.resize(size.w, size.h);
        for (const Case& item : cases) {
            const Snapshot snapshot = settlement(item.winner, item.end_reason);
            const SettlementModel model = settlement_model(snapshot);
            std::cout << "== " << item.name << " " << size.w << "x" << size.h
                      << "  标题=[" << model.title.toStdString() << "]"
                      << " 原因=[" << model.reason.toStdString() << "]\n";
            for (const int t : times) {
                const QString path = QStringLiteral("%1/%2-%3x%4-t%5.png")
                                         .arg(out, QString::fromLatin1(item.name))
                                         .arg(size.w)
                                         .arg(size.h)
                                         .arg(t);
                shoot(dashboard, snapshot, t, path, item.name);
            }
            // 下一种结果要让 resolve_game_result 真的换值,否则 Dashboard 认为
            // 结果没变,不会重设 QML 属性也不会重置动画时钟。
            Snapshot clear;
            dashboard.update(clear, nullptr, 1000);
            flush_frames(60);
        }
    }
    return 0;
}
