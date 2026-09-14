#include "dashboard.h"

#include "logging.h"
#include "presentation.h"

#include <QColor>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QPainter>
#include <QRect>
#include <QScrollArea>
#include <QStackedLayout>
#include <QStringList>
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

QColor event_level_color(std::uint32_t level) {
    switch (level) {
    case 0: return QColor(198, 200, 206);
    case 1: return QColor(127, 183, 255);
    case 2: return QColor(240, 200, 90);
    case 3: return QColor(255, 107, 107);
    default: return QColor(198, 200, 206);
    }
}

QString event_history_html(const Snapshot& snapshot, std::size_t max) {
    const std::vector<EventRecord> records = snapshot.events.recent(max);
    if (records.empty()) return QStringLiteral("<span>暂无赛事事件</span>");

    QStringList rows;
    for (const EventRecord& record : records) {
        // Escaped because the text arrives from the MQTT feed: an unescaped '<'
        // would be swallowed by the rich-text parser, so a hostile or merely
        // malformed payload could hide or restyle the alert it is reporting.
        const QString text = QString::fromStdString(record.text).toHtmlEscaped();
        rows.append(QStringLiteral("<span style=\"color:%1\">[%2] %3</span>")
                        .arg(event_level_color(record.level).name())
                        .arg(level_name(record.level))
                        .arg(text));
    }

    // Overwritten records are disclosed rather than dropped in silence, which
    // would render a truncated history as a quiet match.
    if (snapshot.events.droppedCount() > 0) {
        rows.append(QStringLiteral("<span style=\"color:#8e8e94\">… 更早 %1 条已滚出缓冲</span>")
                        .arg(snapshot.events.droppedCount()));
    }
    return rows.join(QStringLiteral("<br/>"));
}

QString video_overlay_countdown_text(const Snapshot& snapshot) {
    return QStringLiteral("倒计时 %1").arg(clock_text(snapshot.game.stage_countdown_sec));
}

QString video_overlay_hp_text(const Snapshot& snapshot) {
    for (const MapRobot& robot : snapshot.map_robots) {
        if (!robot.is_self) continue;
        const auto found = snapshot.robots.find(robot.id);
        if (found == snapshot.robots.end()) break;
        return QStringLiteral("血量 %1")
            .arg(ratio(found->second.dynamic.current_hp, found->second.dynamic.max_hp));
    }
    return QStringLiteral("血量 --");
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
    // Half the decode resolution at 16:9. A 320x180 floor here forced the info page
    // to 696px, silently overriding the app's own 620px window minimum.
    setMinimumSize(160, 90);
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

void apply_frame(VideoPane* thumbnail, VideoPane* full, const QImage& frame) {
    thumbnail->setFrame(frame);
    full->setFrame(frame);
}

Dashboard::Dashboard(const Config& config, QWidget* parent)
    : QWidget(parent),
      machine_(config.mode_exit_hysteresis_ms, config.blind_stale_fallback_ms) {
    auto* root = new QVBoxLayout(this);
    banner_ = new QLabel(QStringLiteral("只读模拟 · READ-ONLY SIMULATION SAFE · 无控制下发通道"), this);
    banner_->setStyleSheet(QStringLiteral(
        "background:#173d17; color:#8ef58e; padding:6px; font-weight:bold;"));
    banner_->setObjectName(QStringLiteral("readOnlyBanner"));
    root->addWidget(banner_);

    // Sits alongside the read-only banner and never replaces it: the read-only
    // guarantee has to stay on screen in every mode.
    mode_banner_ = new QLabel(this);
    mode_banner_->setStyleSheet(QStringLiteral(
        "background:#1b2333; color:#9ec5ff; padding:4px; font-weight:bold;"));
    mode_banner_->setObjectName(QStringLiteral("modeBanner"));
    root->addWidget(mode_banner_);

    stack_ = new QStackedLayout();
    info_page_ = new QWidget(this);
    video_page_ = new QWidget(this);

    auto* grid = new QGridLayout(info_page_);
    auto make_section = [this](const QString& title, QLabel** target) {
        auto* box = new QFrame(info_page_);
        box->setFrameShape(QFrame::StyledPanel);
        auto* column = new QVBoxLayout(box);
        auto* heading = new QLabel(title, box);
        heading->setStyleSheet(QStringLiteral("color:#7fb7ff; font-weight:bold;"));
        column->addWidget(heading);
        *target = new QLabel(QStringLiteral("--"));
        QFont mono(QStringLiteral("Menlo"));
        mono.setStyleHint(QFont::Monospace);
        // 9pt, not 11pt: at 11pt the robot panel's ~13 telemetry rows do not fit the
        // right-hand column at 720p, so rows got sliced mid-glyph behind a scrollbar.
        mono.setPointSize(9);
        (*target)->setFont(mono);
        (*target)->setTextInteractionFlags(Qt::TextSelectableByMouse);
        (*target)->setAlignment(Qt::AlignTop | Qt::AlignLeft);

        // Scrolled, not stretched: these panels are dense enough that their
        // combined minimum height forced the info page to 896px, which silently
        // overrode resize(1280,720) and would clip on a 720p operator screen.
        auto* scroll = new QScrollArea(box);
        scroll->setWidget(*target);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setMinimumHeight(0);
        scroll->setStyleSheet(QStringLiteral("background:transparent;"));
        column->addWidget(scroll, 1);
        return box;
    };

    auto* map_box = new QFrame(info_page_);
    map_box->setFrameShape(QFrame::StyledPanel);
    auto* map_column = new QVBoxLayout(map_box);
    auto* map_heading = new QLabel(QStringLiteral("战术地图"), map_box);
    map_heading->setStyleSheet(QStringLiteral("color:#7fb7ff; font-weight:bold;"));
    map_column->addWidget(map_heading);
    map_ = new MapPane(map_box);
    map_->setObjectName(QStringLiteral("mapPane"));
    map_column->addWidget(map_, 1);

    // The map owns the entire left column rather than sharing rows with the
    // thumbnail box. Sharing rows pins it to its minimum height, because a
    // fixed-size 320x180 pane and the map would compete for the same vertical
    // space and grid stretch only distributes what is left over after minimums.
    grid->addWidget(map_box, 0, 0, 3, 1);
    grid->addWidget(make_section(QStringLiteral("赛事状态"), &game_), 0, 1);
    grid->addWidget(make_section(QStringLiteral("机器人状态"), &robot_), 1, 1);

    auto* video_box = new QFrame(info_page_);
    video_box->setFrameShape(QFrame::StyledPanel);
    auto* video_column = new QVBoxLayout(video_box);
    auto* video_heading = new QLabel(QStringLiteral("图传"), video_box);
    video_heading->setStyleSheet(QStringLiteral("color:#7fb7ff; font-weight:bold;"));
    video_column->addWidget(video_heading);
    info_video_pane_ = new VideoPane(video_box);
    info_video_pane_->setObjectName(QStringLiteral("infoVideoPane"));
    // A cap, not a fixed size: scaling a 320x180 frame up buys no detail, but
    // setFixedSize also pinned the floor, so the box could not shrink on a short
    // window and overflowed onto the stats label. paintEvent keeps the aspect.
    info_video_pane_->setMaximumSize(320, 180);
    // Half the cap, so a short window shrinks the picture instead of overlapping the
    // stats text with it. paintEvent keeps the aspect ratio at whatever height it gets.
    info_video_pane_->setMinimumSize(160, 90);

    video_stats_ = new QLabel(QStringLiteral("--"), video_box);
    video_stats_->setObjectName(QStringLiteral("videoStats"));
    QFont mono(QStringLiteral("Menlo"));
    mono.setStyleHint(QFont::Monospace);
    mono.setPointSize(10);
    video_stats_->setFont(mono);
    video_stats_->setAlignment(Qt::AlignTop | Qt::AlignLeft);

    // Picture beside the numbers, not above them. This box is wide and short (374x158
    // at the 980x620 floor), so stacking heading + 90px picture + 3 lines of stats
    // needed 196px, and QVBoxLayout resolved the 38px deficit by painting the text on
    // top of the picture. Side by side the box needs only the taller of the two.
    auto* video_row = new QHBoxLayout();
    video_row->addWidget(info_video_pane_, 0, Qt::AlignTop);
    video_row->addWidget(video_stats_, 1, Qt::AlignTop);
    video_column->addLayout(video_row);
    grid->addWidget(video_box, 2, 1);

    auto* event_box = new QFrame(info_page_);
    event_box->setFrameShape(QFrame::StyledPanel);
    auto* event_column = new QVBoxLayout(event_box);
    auto* event_heading = new QLabel(QStringLiteral("赛事事件"), event_box);
    event_heading->setStyleSheet(QStringLiteral("color:#7fb7ff; font-weight:bold;"));
    event_column->addWidget(event_heading);
    event_ = new QLabel(QStringLiteral("--"), event_box);
    event_->setObjectName(QStringLiteral("eventPanel"));
    event_->setTextFormat(Qt::RichText);
    event_->setFont(mono);
    event_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    event_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    event_column->addWidget(event_);
    event_column->addStretch();
    grid->addWidget(event_box, 3, 0, 1, 2);

    grid->setColumnStretch(0, 3);
    grid->setColumnStretch(1, 2);
    // Rows 0-2 carry the map column; the event row stays unstretched so the map
    // keeps the growth rather than the text panel below it.
    grid->setRowStretch(0, 3);
    grid->setRowStretch(1, 3);
    // Row 2 takes no stretch share at all. The telemetry scroll areas above can shrink
    // to nothing, so any share here let the grid hand row 2 less than the video box
    // needs, and QVBoxLayout then overlapped the picture with the stats text. At zero
    // stretch the row is sized from its own minimum and the spare height goes to the
    // map column instead, which is where it was wanted anyway.
    grid->setRowStretch(2, 0);
    grid->setRowStretch(3, 0);

    auto* video_layout = new QVBoxLayout(video_page_);
    video_layout->setContentsMargins(0, 0, 0, 0);
    auto* overlay = new QWidget(video_page_);
    overlay->setStyleSheet(QStringLiteral("background:#101014;"));
    auto* overlay_row = new QHBoxLayout(overlay);
    overlay_row->setContentsMargins(8, 2, 8, 2);
    // Countdown and HP stay on screen in video mode: during blinding they are the
    // only match state left that the operator can still act on.
    video_countdown_ = new QLabel(overlay);
    video_countdown_->setStyleSheet(QStringLiteral("color:#e8e8ee; font-weight:bold;"));
    video_countdown_->setObjectName(QStringLiteral("videoOverlayCountdown"));
    video_hp_ = new QLabel(overlay);
    video_hp_->setStyleSheet(QStringLiteral("color:#e8e8ee; font-weight:bold;"));
    video_hp_->setObjectName(QStringLiteral("videoOverlayHp"));
    overlay_row->addWidget(video_countdown_);
    overlay_row->addStretch();
    overlay_row->addWidget(video_hp_);
    video_layout->addWidget(overlay);

    video_full_pane_ = new VideoPane(video_page_);
    video_full_pane_->setObjectName(QStringLiteral("videoFullPane"));
    video_layout->addWidget(video_full_pane_, 1);

    stack_->addWidget(info_page_);
    stack_->addWidget(video_page_);
    root->addLayout(stack_, 1);
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

    stack_->setCurrentWidget(decision.mode == UiMode::Video ? video_page_ : info_page_);
    mode_banner_->setText(QStringLiteral("模式 %1 · 原因 %2")
                              .arg(mode_name(decision.mode))
                              .arg(reason_name(decision.reason)));

    game_->setText(game_panel_text(snapshot));
    robot_->setText(robot_panel_text(snapshot));
    event_->setText(event_history_html(snapshot, kEventPanelRows));
    video_stats_->setText(video_panel_text(video));
    map_->setRobots(snapshot.map_robots);
    video_countdown_->setText(video_overlay_countdown_text(snapshot));
    video_hp_->setText(video_overlay_hp_text(snapshot));

    if (video) {
        // latestFrame() is called EXACTLY ONCE per tick. Calling it per pane could
        // straddle a frame advance and leave the two pages showing different
        // moments. The copy() is what detaches from the decoder buffer that
        // frame_to_image only borrows; QImage is copy-on-write, so handing the same
        // copy to both panes costs no second deep copy.
        const QByteArray frame = video->latestFrame();
        const QImage owned = frame_to_image(frame, VideoReceiver::frameWidth(),
                                            VideoReceiver::frameHeight())
                                 .copy();
        apply_frame(info_video_pane_, video_full_pane_, owned);

        const QString status = video->online()
                                   ? QStringLiteral("等待画面")
                                   : QStringLiteral("图传 %1").arg(video->state());
        info_video_pane_->setStatus(status);
        video_full_pane_->setStatus(status);
    } else {
        info_video_pane_->setStatus(QStringLiteral("图传未启用"));
        video_full_pane_->setStatus(QStringLiteral("图传未启用"));
    }
}

std::string Dashboard::layoutDump() const {
    const auto pane_entry = [](const QString& name, const QWidget* widget) {
        const bool shown = widget->isVisible();
        // A pane the stacked layout never showed was never laid out, so its geometry()
        // is default/leftover junk that varies between identical runs.
        const QRect box = shown ? widget->geometry() : QRect();
        return QStringLiteral("    {\"name\": \"%1\", \"visible\": %2, \"x\": %3, \"y\": %4,"
                              " \"width\": %5, \"height\": %6}")
            .arg(name)
            .arg(shown ? QStringLiteral("true") : QStringLiteral("false"))
            .arg(box.x())
            .arg(box.y())
            .arg(box.width())
            .arg(box.height());
    };

    // Fixed order, integers only: the whole point is that identical input yields a
    // byte-identical file, so a diff means the layout really changed.
    QStringList panes;
    panes << pane_entry(QStringLiteral("map_pane"), map_)
          << pane_entry(QStringLiteral("info_video_pane"), info_video_pane_)
          << pane_entry(QStringLiteral("video_full_pane"), video_full_pane_)
          << pane_entry(QStringLiteral("game_panel"), game_)
          << pane_entry(QStringLiteral("robot_panel"), robot_)
          << pane_entry(QStringLiteral("event_panel"), event_)
          << pane_entry(QStringLiteral("mode_banner"), mode_banner_)
          << pane_entry(QStringLiteral("readonly_banner"), banner_);

    // The window box is included because the "video fills the screen" criterion is a
    // ratio against it; without it that check would need a hard-coded size.
    return QStringLiteral("{\n"
                          "  \"mode\": \"%1\",\n"
                          "  \"reason\": \"%2\",\n"
                          "  \"readonly_banner_visible\": %3,\n"
                          "  \"stacked_index\": %4,\n"
                          "  \"window\": {\"width\": %5, \"height\": %6},\n"
                          "  \"panes\": [\n%7\n  ]\n"
                          "}\n")
        .arg(mode_name(machine_.mode()))
        .arg(reason_name(machine_.reason()))
        .arg(banner_->isVisible() ? QStringLiteral("true") : QStringLiteral("false"))
        .arg(stack_->currentIndex())
        .arg(width())
        .arg(height())
        .arg(panes.join(QStringLiteral(",\n")))
        .toStdString();
}

}
