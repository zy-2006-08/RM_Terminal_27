#include "link_health.h"

#include "logging.h"
#include "presentation.h"

#include <QStringList>

#include <algorithm>
#include <cmath>

namespace rm_terminal {
namespace {

QString seconds_text(MonotonicMs ms) {
    if (ms < 10000) return QStringLiteral("%1 秒").arg(ms / 1000.0, 0, 'f', 1);
    return QStringLiteral("%1 秒").arg(ms / 1000);
}

QString per_second(double rate) {
    return QString::number(static_cast<long long>(std::lround(rate)));
}

LinkStatus make(LinkLevel level, QString summary, QString detail, QString hint = QString()) {
    return LinkStatus{level, std::move(summary), std::move(detail), std::move(hint)};
}

CheckLevel check_level(LinkLevel level) {
    switch (level) {
    case LinkLevel::Ok: return CheckLevel::Ok;
    case LinkLevel::Warn: return CheckLevel::Warn;
    case LinkLevel::Down: return CheckLevel::Fail;
    case LinkLevel::Off: return CheckLevel::Off;
    }
    return CheckLevel::Off;
}

}  // namespace

QString link_level_name(LinkLevel level) {
    switch (level) {
    case LinkLevel::Ok: return QStringLiteral("Ok");
    case LinkLevel::Warn: return QStringLiteral("Warn");
    case LinkLevel::Down: return QStringLiteral("Down");
    case LinkLevel::Off: return QStringLiteral("Off");
    }
    return QStringLiteral("Off");
}

std::optional<MonotonicMs> silence(std::optional<MonotonicMs> last_event,
                                   std::optional<MonotonicMs> since, MonotonicMs now) {
    std::optional<MonotonicMs> start = last_event;
    if (since && (!start || *since > *start)) start = since;
    if (!start) return std::nullopt;
    return std::max<MonotonicMs>(0, now - *start);
}

LinkStatus referee_status(const RefereeSample& sample) {
    const QString hint = QStringLiteral("检查网线、服务器地址和端口");
    if (!sample.connected) return make(LinkLevel::Down, QStringLiteral("重连中"),
                                       QStringLiteral("断开了，正在重连"), hint);
    if (sample.silent_ms && *sample.silent_ms >= kRefereeSilentMs)
        return make(LinkLevel::Down, QStringLiteral("无数据"),
                    QStringLiteral("连着，但 %1没收到数据").arg(seconds_text(*sample.silent_ms)),
                    hint);
    return make(LinkLevel::Ok, QString(),
                QStringLiteral("已连上，每秒约 %1 条").arg(per_second(sample.messages_per_s)));
}

LinkStatus car_status(const CarSample& sample) {
    if (sample.freshness == Freshness::Fresh)
        return make(LinkLevel::Ok, QString(), QStringLiteral("正常"));
    if (sample.referee_down)
        return make(LinkLevel::Down, QStringLiteral("无数据"),
                    QStringLiteral("裁判数据断了，车端数据也收不到"),
                    QStringLiteral("先查裁判数据"));
    const QString hint = QStringLiteral("检查车有没有上电、车上程序有没有在发");
    if (sample.freshness == Freshness::NeverReceived || !sample.silent_ms)
        return make(LinkLevel::Down, QStringLiteral("无数据"),
                    QStringLiteral("还没收到过车端数据"), hint);
    return make(LinkLevel::Down, QStringLiteral("无数据"),
                QStringLiteral("%1没收到车端数据").arg(seconds_text(*sample.silent_ms)), hint);
}

LinkStatus video_status(const VideoSample& sample) {
    if (!sample.enabled) return make(LinkLevel::Off, QString(), QStringLiteral("未启用"));
    if (sample.online)
        return make(LinkLevel::Ok, QString(),
                    QStringLiteral("有画面，每秒 %1 帧").arg(per_second(sample.frames_per_s)));
    const QString hint = QStringLiteral("检查图传接收端和网线");
    if (!sample.silent_ms)
        return make(LinkLevel::Down, QStringLiteral("无画面"), QStringLiteral("还没收到画面"), hint);
    return make(LinkLevel::Down, QStringLiteral("无画面"),
                QStringLiteral("%1没画面").arg(seconds_text(*sample.silent_ms)), hint);
}

LinkStatus controller_status(const ControllerSample& sample) {
    if (!sample.configured)
        return make(LinkLevel::Off, QString(),
                    QStringLiteral("未启用（配置里没写 controller_serial）"));
    if (!sample.open)
        return make(LinkLevel::Down, QStringLiteral("未接"),
                    sample.error.isEmpty() ? QStringLiteral("串口没打开") : sample.error,
                    QStringLiteral("检查机械臂的 USB 线"));
    const QString bad = sample.bad_frames > 0
                            ? QStringLiteral(" · 坏帧 %1").arg(sample.bad_frames)
                            : QString();
    if (!sample.silent_ms || *sample.silent_ms >= kControllerSilentMs) {
        const QString what = sample.silent_ms
                                 ? QStringLiteral("串口开着，但 %1没收到数据")
                                       .arg(seconds_text(*sample.silent_ms))
                                 : QStringLiteral("串口开着，还没收到过数据");
        return make(LinkLevel::Down, QStringLiteral("无数据"), what + bad,
                    QStringLiteral("检查机械臂有没有上电、波特率是不是 115200"));
    }
    QStringList degrees;
    for (const float joint : sample.joints)
        degrees << QStringLiteral("%1°").arg(static_cast<double>(joint) * 180.0 / M_PI, 0, 'f', 0);
    const QString rate = QStringLiteral("每秒 %1 帧").arg(per_second(sample.frames_per_s));
    if (!sample.can_forward)
        return make(LinkLevel::Warn, QStringLiteral("发不出"),
                    rate + QStringLiteral("，但裁判数据断了，发不出去") + bad,
                    QStringLiteral("先查裁判数据"));
    return make(LinkLevel::Ok, QString(),
                rate + QStringLiteral("，正在转发 · 关节 ") + degrees.join(QLatin1Char(' ')) + bad);
}

double RateMeter::update(std::uint64_t total, MonotonicMs now) {
    if (!base_at_ || total < base_total_) {
        base_at_ = now;
        base_total_ = total;
        return rate_;
    }
    const MonotonicMs elapsed = now - *base_at_;
    if (elapsed >= 1000) {
        rate_ = static_cast<double>(total - base_total_) * 1000.0 / static_cast<double>(elapsed);
        base_at_ = now;
        base_total_ = total;
    }
    return rate_;
}

void LinkStateLog::observe(const LinkReport& report) {
    const auto entry = [this](const char* name, const LinkStatus& now, const LinkStatus* before) {
        if (before && before->level == now.level && before->summary == now.summary) return;
        StructuredLog::write(
            now.level == LinkLevel::Down ? LogLevel::warning : LogLevel::info,
            QStringLiteral("link_state"),
            {QStringLiteral("link=%1").arg(QLatin1String(name)),
             QStringLiteral("from=%1").arg(before ? link_level_name(before->level)
                                                  : QStringLiteral("None")),
             QStringLiteral("to=%1").arg(link_level_name(now.level)),
             QStringLiteral("summary=%1").arg(now.summary.isEmpty() ? QStringLiteral("-")
                                                                    : now.summary),
             QStringLiteral("detail=%1").arg(now.detail)});
    };
    entry("referee", report.referee, last_ ? &last_->referee : nullptr);
    entry("car", report.car, last_ ? &last_->car : nullptr);
    entry("video", report.video, last_ ? &last_->video : nullptr);
    entry("controller", report.controller, last_ ? &last_->controller : nullptr);
    last_ = report;
}

QString self_robot_label(int robot_id) {
    const auto id = static_cast<std::uint32_t>(robot_id);
    return QStringLiteral("%1 %2 号 %3")
        .arg(robot_id > 100 ? QStringLiteral("蓝方") : QStringLiteral("红方"))
        .arg(display_robot_number(id))
        .arg(robot_class_name(id));
}

SelfCheck build_self_check(const SelfCheckInputs& inputs) {
    SelfCheck check;
    check.rows.push_back({QStringLiteral("自机"), CheckLevel::Ok,
                          self_robot_label(inputs.self_robot_id), QString()});
    const auto link_row = [&check](const QString& name, const LinkStatus& status) {
        check.rows.push_back({name, check_level(status.level), status.detail, status.hint});
    };
    link_row(QStringLiteral("裁判数据"), inputs.links.referee);
    link_row(QStringLiteral("车端数据"), inputs.links.car);
    link_row(QStringLiteral("图传"), inputs.links.video);
    link_row(QStringLiteral("控制器"), inputs.links.controller);

    constexpr std::int64_t kLowDiskBytes = 2LL * 1000 * 1000 * 1000;
    if (inputs.recording) {
        const std::int64_t free_bytes = inputs.free_bytes.value_or(-1);
        const QString gb = QString::number(free_bytes / 1000 / 1000 / 1000);
        if (free_bytes >= 0 && free_bytes < kLowDiskBytes)
            check.rows.push_back({QStringLiteral("录像"), CheckLevel::Warn,
                                  QStringLiteral("正在录，硬盘只剩 %1 GB").arg(gb),
                                  QStringLiteral("清理硬盘，一场比赛的录像约 1 GB")});
        else
            check.rows.push_back({QStringLiteral("录像"), CheckLevel::Ok,
                                  free_bytes >= 0 ? QStringLiteral("正在录，硬盘还剩 %1 GB").arg(gb)
                                                  : QStringLiteral("正在录"),
                                  QString()});
    } else if (!inputs.recording_error.isEmpty()) {
        check.rows.push_back({QStringLiteral("录像"), CheckLevel::Fail,
                              QStringLiteral("录像失败：%1").arg(inputs.recording_error),
                              QStringLiteral("检查录像目录能不能写、硬盘满没满")});
    } else {
        check.rows.push_back({QStringLiteral("录像"), CheckLevel::Warn,
                              QStringLiteral("这次没开录像"),
                              QStringLiteral("去掉启动参数 --no-record")});
    }

    if (inputs.speech_available)
        check.rows.push_back(
            {QStringLiteral("语音提醒"), CheckLevel::Ok, QStringLiteral("可以用"), QString()});
    else
        check.rows.push_back({QStringLiteral("语音提醒"), CheckLevel::Warn,
                              QStringLiteral("这台电脑没有语音，只有横幅"), QString()});

    for (const CheckRow& row : check.rows)
        if (row.level == CheckLevel::Fail) ++check.failures;
    check.verdict = check.failures == 0
                        ? QStringLiteral("可以上场")
                        : QStringLiteral("有 %1 项异常，先别上场").arg(check.failures);
    return check;
}

}  // namespace rm_terminal
