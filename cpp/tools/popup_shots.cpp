#include "dashboard.h"

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QPixmap>
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

EventRecord event(std::int32_t id, std::uint32_t level, const char* text, MonotonicMs at,
                  std::uint32_t faction, std::int32_t countdown) {
    EventRecord record;
    record.event_id = id;
    record.level = level;
    record.text = text;
    record.received_at = at;
    record.faction = faction;
    record.countdown_sec = countdown;
    return record;
}

Snapshot live_match() {
    Snapshot snapshot;
    snapshot.self = RobotId{3};
    snapshot.game.stage_countdown_sec = present<std::int32_t>(154);
    snapshot.game.current_stage = present<std::uint32_t>(4);
    snapshot.blind.self_base_blinded = present<bool>(false);

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

    snapshot.game.economy = present<std::uint32_t>(1240);
    snapshot.game.total_economy = present<std::uint64_t>(3600);
    snapshot.game.red_total_damage = present<std::uint32_t>(5200);
    snapshot.game.blue_total_damage = present<std::uint32_t>(4350);
    snapshot.game.red_fortress_activation_sec = present<std::int32_t>(0);
    snapshot.game.blue_fortress_activation_sec = present<std::int32_t>(4);
    snapshot.game.rune_status = present<std::uint32_t>(3);
    snapshot.kills = KillTally{3, 2};

    snapshot.events.push(event(4, 1, "小能量机关已激活", 900, 0, 290));
    snapshot.events.push(event(2, 1, "敌方前哨站被击毁", 904, 2, 250));
    snapshot.events.push(event(1, 2, "敌方 3 号步兵击毁我方 4 号步兵", 908, 2, 230));
    snapshot.events.push(event(9, 3, "敌方飞镖命中我方基地随机移动目标，遮挡 10 秒", 916, 2, 200));
    snapshot.events.push(event(6, 1, "敌方前哨阵地被我方临时控制", 920, 1, 160));
    return snapshot;
}

// 弹窗内容由 QQuickWidget 在**渲染线程**上出帧,属性写入不会同步反映到下一次 grab()。
// 只 processEvents 两轮会截到上一状态的旧帧(表现为文案与图框对不上),所以这里必须
// 泵事件泵到真的过了一段时间为止。
void flush_frames(int ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(5);
    }
}

void shoot(Dashboard& dashboard, const Snapshot& snapshot, MonotonicMs now,
           const QString& path, const char* label) {
    dashboard.update(snapshot, nullptr, now);
    dashboard.show();
    flush_frames(300);
    if (!dashboard.grab().save(path)) {
        std::cout << "FAILED to write " << path.toStdString() << '\n';
        return;
    }
    const QString body = popup_text(dashboard.popup(), std::nullopt).replace(QChar('\n'), QStringLiteral(" / "));
    std::cout << label << " -> " << path.toStdString()
              << "  文案=[" << body.toStdString() << "]\n";
}

}  // namespace

// 一次性验收工具:把每个比赛弹窗渲染成图。模拟器里自机血量归零是低概率随机事件,
// 等它自然发生不现实,所以这里直接构造快照。
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    const QString out = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("/tmp");

    Dashboard dashboard(Config{});
    dashboard.resize(kWidth, kHeight);

    // dwell 窗口要求弹窗停留足够久才允许降级,所以每一枪都往后推 6 秒,
    // 否则后一个弹窗会被前一个压住,截出来全是同一张。
    MonotonicMs now = 1000;
    const auto next = [&now] { return now += 6000; };

    shoot(dashboard, live_match(), now, out + "/popup-0-healthy.png", "健康比赛(无弹窗)");

    Snapshot dead = live_match();
    dead.robots.at(RobotId{3}).dynamic.current_hp = present<std::uint32_t>(0);
    shoot(dashboard, dead, next(), out + "/popup-1-eliminated.png", "已阵亡");

    Snapshot paused = live_match();
    paused.game.is_paused = present<bool>(true);
    shoot(dashboard, paused, next(), out + "/popup-2-paused.png", "比赛暂停");

    Snapshot settlement = live_match();
    settlement.game.current_stage = present<std::uint32_t>(5);
    shoot(dashboard, settlement, next(), out + "/popup-3-settlement.png", "本局结束");

    Snapshot pre = live_match();
    pre.game.current_stage = present<std::uint32_t>(3);
    pre.game.stage_countdown_sec = present<std::int32_t>(7);
    shoot(dashboard, pre, next(), out + "/popup-4-prematch.png", "比赛即将开始");

    Snapshot stale = live_match();
    stale.game.current_stage.freshness = Freshness::Stale;
    shoot(dashboard, stale, next(), out + "/popup-5-linklost.png", "信号中断");

    return 0;
}
