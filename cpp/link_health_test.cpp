#include "link_health.h"

#include <cstdio>

namespace {

int failures = 0;

void check(bool condition, const char* label) {
    if (condition) return;
    std::fprintf(stderr, "FAIL %s\n", label);
    ++failures;
}

using rm_terminal::LinkLevel;

void referee_distinguishes_down_from_silent() {
    rm_terminal::RefereeSample sample;
    check(rm_terminal::referee_status(sample).level == LinkLevel::Down, "disconnected is down");
    check(rm_terminal::referee_status(sample).summary == QStringLiteral("重连中"),
          "disconnected says it is reconnecting");

    // 网线被拔:连接还没超时,但数据已经停了。
    sample.connected = true;
    sample.silent_ms = 2500;
    const auto silent = rm_terminal::referee_status(sample);
    check(silent.level == LinkLevel::Down, "connected but silent is down too");
    check(silent.summary == QStringLiteral("无数据"), "silent says no data, not reconnecting");
    check(silent.detail.contains(QStringLiteral("2.5 秒")), "silent says for how long");

    sample.silent_ms = 300;
    sample.messages_per_s = 98.6;
    const auto ok = rm_terminal::referee_status(sample);
    check(ok.level == LinkLevel::Ok && ok.summary.isEmpty(), "flowing data is ok with no summary");
    check(ok.detail.contains(QStringLiteral("99")), "ok reports the message rate");
}

void car_points_at_the_real_cause() {
    rm_terminal::CarSample sample;
    sample.freshness = rm_terminal::Freshness::Stale;
    sample.silent_ms = 3000;
    sample.referee_down = true;
    check(rm_terminal::car_status(sample).hint.contains(QStringLiteral("裁判数据")),
          "with the referee link down the car hint points at it");
    sample.referee_down = false;
    const auto stale = rm_terminal::car_status(sample);
    check(stale.level == LinkLevel::Down, "stale car data is down");
    check(stale.hint.contains(QStringLiteral("上电")), "stale car data suggests the power");
    sample.freshness = rm_terminal::Freshness::Fresh;
    check(rm_terminal::car_status(sample).level == LinkLevel::Ok, "fresh car data is ok");
}

void video_and_controller_states() {
    rm_terminal::VideoSample video;
    video.silent_ms = 12000;
    check(rm_terminal::video_status(video).detail.contains(QStringLiteral("12 秒")),
          "video says how long it has been dark");
    video.enabled = false;
    check(rm_terminal::video_status(video).level == LinkLevel::Off, "replay video is off");

    rm_terminal::ControllerSample arm;
    check(rm_terminal::controller_status(arm).level == LinkLevel::Off,
          "no serial configured is off, not an error");
    arm.configured = true;
    arm.error = QStringLiteral("找不到串口 /dev/ttyUSB0");
    const auto missing = rm_terminal::controller_status(arm);
    check(missing.level == LinkLevel::Down && missing.detail == arm.error,
          "a missing port is down and names the path");
    arm.open = true;
    arm.silent_ms = 250;
    check(rm_terminal::controller_status(arm).summary == QStringLiteral("无数据"),
          "an open but silent port is down after 200ms");
    arm.silent_ms = 30;
    arm.frames_per_s = 25;
    arm.joints = {0.0f, 1.5707964f, 0, 0, 0, 0};
    const auto blocked = rm_terminal::controller_status(arm);
    check(blocked.level == LinkLevel::Warn && blocked.summary == QStringLiteral("发不出"),
          "frames that cannot be forwarded are a warning");
    arm.can_forward = true;
    arm.bad_frames = 2;
    const auto ok = rm_terminal::controller_status(arm);
    check(ok.level == LinkLevel::Ok, "flowing and forwarding is ok");
    check(ok.detail.contains(QStringLiteral("90°")), "joints are shown in degrees");
    check(ok.detail.contains(QStringLiteral("坏帧 2")), "bad frames are disclosed");
}

void silence_restarts_at_reconnect() {
    check(!rm_terminal::silence(std::nullopt, std::nullopt, 5000).has_value(),
          "nothing yet is not a duration");
    check(rm_terminal::silence(1000, std::nullopt, 5000) == 4000, "since the last event");
    // 断线前最后一条在 1000,9000 时重连:不能报「沉默了 9 秒」。
    check(rm_terminal::silence(1000, 9000, 9200) == 200, "a reconnect restarts the clock");
}

void rate_meter_settles_per_second() {
    rm_terminal::RateMeter meter;
    check(meter.update(0, 0) == 0, "no rate before one second");
    check(meter.update(10, 500) == 0, "still no rate at half a second");
    const double rate = meter.update(25, 1000);
    check(rate > 24.9 && rate < 25.1, "25 frames in one second is 25/s");
    check(meter.update(30, 1250) == rate, "the rate holds between refreshes");
}

void self_check_counts_only_failures() {
    rm_terminal::SelfCheckInputs inputs;
    inputs.self_robot_id = 103;
    inputs.links.referee.level = LinkLevel::Ok;
    inputs.links.car.level = LinkLevel::Ok;
    inputs.links.video.level = LinkLevel::Ok;
    inputs.links.controller.level = LinkLevel::Off;
    inputs.recording = true;
    inputs.free_bytes = 120LL * 1000 * 1000 * 1000;
    inputs.speech_available = false;
    auto report = rm_terminal::build_self_check(inputs);
    check(report.rows.front().detail == QStringLiteral("蓝方 3 号 步兵"), "self robot label");
    check(report.failures == 0, "warnings and off rows do not block");
    check(report.verdict == QStringLiteral("可以上场"), "all clear says go");

    inputs.links.car.level = LinkLevel::Down;
    inputs.links.video.level = LinkLevel::Down;
    report = rm_terminal::build_self_check(inputs);
    check(report.failures == 2, "two down links are two failures");
    check(report.verdict == QStringLiteral("有 2 项异常，先别上场"), "failures say do not go");
}

}  // namespace

int main() {
    referee_distinguishes_down_from_silent();
    car_points_at_the_real_cause();
    video_and_controller_states();
    silence_restarts_at_reconnect();
    rate_meter_settles_per_second();
    self_check_counts_only_failures();
    if (failures != 0) {
        std::fprintf(stderr, "link_health_test: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("link_health_test: all checks passed\n");
    return 0;
}
