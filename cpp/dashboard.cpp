#include "dashboard.h"

#include "logging.h"
#include "presentation.h"

#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QPainter>
#include <QVBoxLayout>

namespace rm_terminal {
namespace {

QString stage_name(std::uint32_t stage) {
    switch (stage) {
    case 0: return QStringLiteral("未开始");
    case 1: return QStringLiteral("准备阶段");
    case 2: return QStringLiteral("自检阶段");
    case 3: return QStringLiteral("倒计时");
    case 4: return QStringLiteral("比赛中");
    case 5: return QStringLiteral("比赛结束");
    default: return QStringLiteral("未知");
    }
}

QString level_name(std::uint32_t level) {
    switch (level) {
    case 0: return QStringLiteral("INFO");
    case 1: return QStringLiteral("NOTICE");
    case 2: return QStringLiteral("WARNING");
    case 3: return QStringLiteral("CRITICAL");
    default: return QStringLiteral("UNKNOWN");
    }
}

QString mode_name(UiMode mode) {
    switch (mode) {
    case UiMode::Info: return QStringLiteral("Info");
    case UiMode::Video: return QStringLiteral("Video");
    }
    return QStringLiteral("Unknown");
}

QString reason_name(ModeReason reason) {
    switch (reason) {
    case ModeReason::Startup: return QStringLiteral("Startup");
    case ModeReason::BlindAsserted: return QStringLiteral("BlindAsserted");
    case ModeReason::BlindClearedHysteresis: return QStringLiteral("BlindClearedHysteresis");
    case ModeReason::ForcedByCli: return QStringLiteral("ForcedByCli");
    case ModeReason::BlindDataStale: return QStringLiteral("BlindDataStale");
    case ModeReason::BlindSignalLost: return QStringLiteral("BlindSignalLost");
    }
    return QStringLiteral("Unknown");
}

QString freshness_name(Freshness freshness) {
    switch (freshness) {
    case Freshness::NeverReceived: return QStringLiteral("NeverReceived");
    case Freshness::Fresh: return QStringLiteral("Fresh");
    case Freshness::Stale: return QStringLiteral("Stale");
    }
    return QStringLiteral("Unknown");
}

// A field that is present but stale must not read as current, so every value
// carries its own freshness marker instead of one banner for the whole panel.
template<class T>
QString mark(const Field<T>& field) {
    if (!field.value) return QString();
    switch (field.freshness) {
    case Freshness::Stale: return QStringLiteral(" (过期)");
    case Freshness::Fresh: return QString();
    case Freshness::NeverReceived: return QString();
    }
    return QString();
}

QString number(const Field<std::uint32_t>& field) {
    if (!field.value) return QStringLiteral("--");
    return QString::number(*field.value) + mark(field);
}

QString number(const Field<std::int32_t>& field) {
    if (!field.value) return QStringLiteral("--");
    return QString::number(*field.value) + mark(field);
}

QString number(const Field<double>& field, int precision) {
    if (!field.value) return QStringLiteral("--");
    return QString::number(*field.value, 'f', precision) + mark(field);
}

QString flag(const Field<bool>& field, const QString& yes, const QString& no) {
    if (!field.value) return QStringLiteral("--");
    return (*field.value ? yes : no) + mark(field);
}

QString clock_text(const Field<std::int32_t>& field) {
    if (!field.value) return QStringLiteral("--:--");
    const int total = *field.value;
    return QStringLiteral("%1:%2")
               .arg(total / 60, 2, 10, QLatin1Char('0'))
               .arg(total % 60, 2, 10, QLatin1Char('0')) +
           mark(field);
}

QString ratio(const Field<std::uint32_t>& value, const Field<std::uint32_t>& limit) {
    if (!value.value) return QStringLiteral("--");
    QString text = QString::number(*value.value);
    if (limit.value) text += QStringLiteral(" / ") + QString::number(*limit.value);
    return text + mark(value);
}

}

QString game_panel_text(const Snapshot& snapshot) {
    const auto& game = snapshot.game;
    QString stage = QStringLiteral("--");
    if (game.current_stage.value) {
        stage = QStringLiteral("%1 (%2)")
                    .arg(stage_name(*game.current_stage.value))
                    .arg(*game.current_stage.value) +
                mark(game.current_stage);
    }
    return QStringLiteral(
               "阶段    %1\n"
               "剩余    %2      已进行  %3 s\n"
               "轮次    %4 / %5\n"
               "比分    红 %6  :  蓝 %7\n"
               "暂停    %8\n"
               "数据    quality=%9  freshness=%10")
        .arg(stage)
        .arg(clock_text(game.stage_countdown_sec))
        .arg(number(game.stage_elapsed_sec))
        .arg(number(game.current_round))
        .arg(number(game.total_rounds))
        .arg(number(game.red_score))
        .arg(number(game.blue_score))
        .arg(flag(game.is_paused, QStringLiteral("是"), QStringLiteral("否")))
        .arg(quality_text(game.current_stage.quality))
        .arg(freshness_text(game.current_stage.freshness));
}

QString robot_panel_text(const Snapshot& snapshot) {
    if (snapshot.robots.empty()) return QStringLiteral("未收到任何机器人数据");
    QStringList blocks;
    for (const auto& entry : snapshot.robots) {
        const auto& dyn = entry.second.dynamic;
        const auto& tel = entry.second.telemetry;
        const auto& pos = entry.second.position;
        blocks.append(
            QStringLiteral(
                "机器人 %1\n"
                "  血量    %2        装甲电源 %3\n"
                "  热量    %4        弹速    %5 m/s\n"
                "  弹量    %6        金币    %7\n"
                "  底盘功率 %8 W      缓冲能量 %9 J\n"
                "  云台    yaw %10   pitch %11\n"
                "  底盘    %12        视觉    %13\n"
                "  自瞄    %14        锁定    %15\n"
                "  目标    id=%16 距离 %17 m 置信 %18\n"
                "  摩擦轮  %19 rpm     允许开火 %20\n"
                "  位置    x %21  y %22  yaw %23")
                .arg(entry.first.value)
                .arg(ratio(dyn.current_hp, dyn.max_hp))
                .arg(number(entry.second.modules.power_manager))
                .arg(ratio(dyn.shooter_heat_17mm, dyn.shooter_heat_limit))
                .arg(number(dyn.bullet_speed, 2))
                .arg(number(dyn.remaining_ammo))
                .arg(number(dyn.coin))
                .arg(number(dyn.chassis_power, 1))
                .arg(number(dyn.buffer_energy, 1))
                .arg(number(tel.gimbal_yaw, 2))
                .arg(number(tel.gimbal_pitch, 2))
                .arg(tel.chassis_mode.value ? chassis_name(*tel.chassis_mode.value) + mark(tel.chassis_mode)
                                            : QStringLiteral("--"))
                .arg(flag(tel.vision_online, QStringLiteral("在线"), QStringLiteral("离线")))
                .arg(flag(tel.autoaim_enabled, QStringLiteral("开"), QStringLiteral("关")))
                .arg(flag(tel.target_locked, QStringLiteral("是"), QStringLiteral("否")))
                .arg(number(tel.target_id))
                .arg(number(tel.target_distance, 2))
                .arg(number(tel.confidence, 2))
                .arg(number(tel.friction_rpm, 0))
                .arg(flag(tel.fire_permit, QStringLiteral("是"), QStringLiteral("否")))
                .arg(number(pos.x, 2))
                .arg(number(pos.y, 2))
                .arg(number(pos.yaw, 2)));
    }
    return blocks.join(QStringLiteral("\n\n"));
}

QString event_panel_text(const Snapshot& snapshot) {
    const auto& event = snapshot.event;
    if (!event.text.value) return QStringLiteral("暂无赛事事件");
    QString line = QStringLiteral("[%1] %2")
                       .arg(event.level.value ? level_name(*event.level.value) : QStringLiteral("--"))
                       .arg(*event.text.value);
    return line + mark(event.text);
}

QString video_panel_text(const VideoReceiver* video) {
    if (!video) return QStringLiteral("图传未启用");
    const auto stats = video->snapshot();
    return QStringLiteral(
               "状态 %1    已解码 %2 帧\n"
               "包 %3   丢失 %4   乱序 %5   重复 %6\n"
               "整帧 %7   不完整 %8   超时 %9")
        .arg(stats.value(QStringLiteral("state")).toString())
        .arg(stats.value(QStringLiteral("decoded_frames")).toInteger())
        .arg(stats.value(QStringLiteral("packets")).toInteger())
        .arg(stats.value(QStringLiteral("missing_packets")).toInteger())
        .arg(stats.value(QStringLiteral("out_of_order")).toInteger())
        .arg(stats.value(QStringLiteral("duplicates")).toInteger())
        .arg(stats.value(QStringLiteral("completed")).toInteger())
        .arg(stats.value(QStringLiteral("incomplete")).toInteger())
        .arg(stats.value(QStringLiteral("expired")).toInteger());
}

QImage frame_to_image(const QByteArray& frame, int width, int height) {
    const qsizetype expected = qsizetype(width) * height * 3;
    if (frame.size() != expected) return QImage();
    return QImage(reinterpret_cast<const uchar*>(frame.constData()), width, height,
                  width * 3, QImage::Format_BGR888);
}

VideoPane::VideoPane(QWidget* parent) : QWidget(parent) {
    setMinimumSize(320, 180);
    setAutoFillBackground(false);
}

void VideoPane::setFrame(const QImage& image) {
    image_ = image;
    update();
}

void VideoPane::setStatus(const QString& status) {
    if (status_ == status) return;
    status_ = status;
    update();
}

void VideoPane::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor(12, 12, 14));
    if (!image_.isNull()) {
        const QImage scaled =
            image_.scaled(size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        const QPoint origin((width() - scaled.width()) / 2, (height() - scaled.height()) / 2);
        painter.drawImage(origin, scaled);
        return;
    }
    painter.setPen(QColor(150, 150, 155));
    painter.drawText(rect(), Qt::AlignCenter, status_);
}

Dashboard::Dashboard(const Config& config, QWidget* parent)
    : QWidget(parent),
      machine_(config.mode_exit_hysteresis_ms, config.blind_stale_fallback_ms) {
    auto* root = new QVBoxLayout(this);
    banner_ = new QLabel(QStringLiteral("只读模拟 · READ-ONLY SIMULATION SAFE · 无控制下发通道"), this);
    banner_->setStyleSheet(QStringLiteral(
        "background:#173d17; color:#8ef58e; padding:6px; font-weight:bold;"));
    root->addWidget(banner_);

    auto* grid = new QGridLayout();
    auto make_section = [this](const QString& title, QLabel** target) {
        auto* box = new QFrame(this);
        box->setFrameShape(QFrame::StyledPanel);
        auto* column = new QVBoxLayout(box);
        auto* heading = new QLabel(title, box);
        heading->setStyleSheet(QStringLiteral("color:#7fb7ff; font-weight:bold;"));
        column->addWidget(heading);
        *target = new QLabel(QStringLiteral("--"), box);
        QFont mono(QStringLiteral("Menlo"));
        mono.setStyleHint(QFont::Monospace);
        mono.setPointSize(11);
        (*target)->setFont(mono);
        (*target)->setTextInteractionFlags(Qt::TextSelectableByMouse);
        (*target)->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        column->addWidget(*target);
        column->addStretch();
        return box;
    };

    grid->addWidget(make_section(QStringLiteral("赛事状态"), &game_), 0, 0);
    grid->addWidget(make_section(QStringLiteral("机器人状态"), &robot_), 0, 1, 2, 1);

    auto* video_box = new QFrame(this);
    video_box->setFrameShape(QFrame::StyledPanel);
    auto* video_column = new QVBoxLayout(video_box);
    auto* video_heading = new QLabel(QStringLiteral("图传"), video_box);
    video_heading->setStyleSheet(QStringLiteral("color:#7fb7ff; font-weight:bold;"));
    video_column->addWidget(video_heading);
    pane_ = new VideoPane(video_box);
    video_column->addWidget(pane_, 1);
    video_stats_ = new QLabel(QStringLiteral("--"), video_box);
    QFont mono(QStringLiteral("Menlo"));
    mono.setStyleHint(QFont::Monospace);
    mono.setPointSize(10);
    video_stats_->setFont(mono);
    video_column->addWidget(video_stats_);
    grid->addWidget(video_box, 1, 0);

    grid->addWidget(make_section(QStringLiteral("赛事事件"), &event_), 2, 0, 1, 2);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
    root->addLayout(grid, 1);
}

void Dashboard::update(const Snapshot& snapshot, const VideoReceiver* video, MonotonicMs now) {
    const bool blind_asserted =
        snapshot.blind.self_base_blinded.value.value_or(false);
    const ModeDecision decision =
        machine_.step(blind_asserted, snapshot.blind.self_base_blinded.freshness, now);

    // One record per actual transition, not per tick: a 250ms tick would write
    // 240 lines a minute and bury the switches the operator needs to find.
    if (decision.mode != logged_mode_) {
        StructuredLog::write(
            LogLevel::info, QStringLiteral("ui_mode_switch"),
            {QStringLiteral("from=%1").arg(mode_name(logged_mode_)),
             QStringLiteral("to=%1").arg(mode_name(decision.mode)),
             QStringLiteral("reason=%1").arg(reason_name(decision.reason)),
             QStringLiteral("blind_freshness=%1")
                 .arg(freshness_name(snapshot.blind.self_base_blinded.freshness))});
        logged_mode_ = decision.mode;
    }

    game_->setText(game_panel_text(snapshot));
    robot_->setText(robot_panel_text(snapshot));
    event_->setText(event_panel_text(snapshot));
    video_stats_->setText(video_panel_text(video));
    if (video) {
        const QByteArray frame = video->latestFrame();
        pane_->setFrame(frame_to_image(frame, VideoReceiver::frameWidth(),
                                      VideoReceiver::frameHeight())
                            .copy());
        pane_->setStatus(video->online() ? QStringLiteral("等待画面")
                                         : QStringLiteral("图传 %1").arg(video->state()));
    } else {
        pane_->setStatus(QStringLiteral("图传未启用"));
    }
}

}
