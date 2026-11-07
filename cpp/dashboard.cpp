#include "dashboard.h"

#include "layout_editor.h"
#include "logging.h"
#include "presentation.h"
#include "self_check_panel.h"
#include "ui_profile_store.h"
#include "tactical_reminder_dialog.h"
#include "theme.h"
#include "unit_grid.h"

#include <algorithm>
#include <initializer_list>

#include <QColor>
#include <QComboBox>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QPainter>
#include <QPushButton>
#include <QQuickItem>
#include <QRect>
#include <QResizeEvent>
#include <QTimer>
#include <QUrl>
#include <QPair>
#include <QMouseEvent>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSlider>
#include <QStackedLayout>
#include <QStringList>
#include <QVBoxLayout>
#include <QVector>

namespace rm_terminal {
namespace {

// 自动换行的 QLabel 只按「一行」上报 minimumSizeHint,父布局据此把格子定矮,控件却按
// 真实换行高度绘制,多出的行就向上盖住上方的图传画面。
//
// 上报真实换行高度需要宽度,而宽度要等布局算完才有 —— 于是首轮布局仍按一行定高。
// resizeEvent 里的 updateGeometry() 就是打破这个循环的那一步:宽度一旦确定就让父布局
// 带着正确的高度重算一遍。两者缺一都会静默复现重叠 bug。
class WrappedLabel final : public QLabel {
public:
    WrappedLabel(const QString& text, QWidget* parent) : QLabel(text, parent) {
        setWordWrap(true);
    }

    QSize minimumSizeHint() const override {
        const QSize base = QLabel::minimumSizeHint();
        const int w = width() > 0 ? width() : base.width();
        if (w <= 0) return base;
        return QSize(base.width(), std::max(base.height(), heightForWidth(w)));
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QLabel::resizeEvent(event);
        if (event->size().width() != event->oldSize().width()) updateGeometry();
    }
};

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
    case ModeReason::ManualVideo: return QStringLiteral("ManualVideo");
    }
    return QStringLiteral("Unknown");
}

QString game_result_name(GameResult result) {
    switch (result) {
        case GameResult::None:       return QStringLiteral("none");
        case GameResult::RedWin:     return QStringLiteral("red_win");
        case GameResult::BlueWin:    return QStringLiteral("blue_win");
        case GameResult::Draw:       return QStringLiteral("draw");
        case GameResult::Terminated: return QStringLiteral("terminated");
    }
    return QStringLiteral("none");
}

QString popup_name(Popup popup) {
    switch (popup) {
    case Popup::None: return QStringLiteral("None");
    case Popup::LinkLost: return QStringLiteral("LinkLost");
    case Popup::WaitingForData: return QStringLiteral("WaitingForData");
    case Popup::Settlement: return QStringLiteral("Settlement");
    case Popup::Eliminated: return QStringLiteral("Eliminated");
    case Popup::Paused: return QStringLiteral("Paused");
    case Popup::PreMatch: return QStringLiteral("PreMatch");
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

QString ratio(const Field<std::uint32_t>& value, const Field<std::uint32_t>& limit) {
    if (!value.value) return QStringLiteral("--");
    QString text = QString::number(*value.value);
    if (limit.value) text += QStringLiteral(" / ") + QString::number(*limit.value);
    return text + mark(value);
}

QString clock_text(const Field<std::int32_t>& field) {
    if (!field.value) return QStringLiteral("--:--");
    const int total = *field.value;
    return QStringLiteral("%1:%2")
               .arg(total / 60, 2, 10, QLatin1Char('0'))
               .arg(total % 60, 2, 10, QLatin1Char('0')) +
           mark(field);
}

}

QColor event_level_color(std::uint32_t level) { return theme::eventColor(level); }

QString alert_strip_text(const Snapshot& snapshot, const VideoReceiver* video) {
    QStringList alerts;

    // 遮挡由本机按 Event 9 推算,可信度跟着比赛数据流走,所以不再单列「致盲数据过期」:
    // 数据流断了,下面那条「比赛状态数据过期」已经说明了。
    if (snapshot.blind.self_base_blinded.value.value_or(false)) {
        const auto remaining = snapshot.blind.blind_remaining_ms.value;
        alerts << (remaining && *remaining > 0
                       ? QStringLiteral("我方被飞镖遮挡 · 预计剩余 %1s")
                             .arg((*remaining + 999) / 1000)
                       : QStringLiteral("我方被飞镖遮挡"));
    }

    if (snapshot.game.stage_countdown_sec.freshness == Freshness::Stale)
        alerts << QStringLiteral("比赛状态数据过期");
    if (video && !video->online()) alerts << QStringLiteral("图传离线");

    const std::uint32_t ours = self_faction(snapshot).value_or(kRed);
    for (const RobotHealth& robot : snapshot.robot_health) {
        if (robot.faction != ours) continue;
        if (robot.current_hp.value && *robot.current_hp.value == 0) {
            alerts << QStringLiteral("我方 %1 号阵亡")
                          .arg(display_robot_number(robot.id.value));
        }
    }

    return alerts.join(QStringLiteral("      "));
}

QString video_overlay_countdown_text(const Snapshot& snapshot) {
    return QStringLiteral("倒计时 %1").arg(clock_text(snapshot.game.stage_countdown_sec));
}

PopupInputs popup_inputs(const Snapshot& snapshot) {
    PopupInputs inputs;
    // 取阶段而不是任意字段做链路判活:阶段是每帧必发的,某个可选字段缺失是正常的
    // 协议行为,拿它当链路依据会把「这一项没发」误报成「链路断了」。
    inputs.match_freshness = snapshot.game.current_stage.freshness;
    inputs.stage = snapshot.game.current_stage.value;
    inputs.paused = snapshot.game.is_paused.value;
    inputs.countdown_sec = snapshot.game.stage_countdown_sec.value;

    if (snapshot.self) {
        const auto found = snapshot.robots.find(*snapshot.self);
        if (found != snapshot.robots.end()) inputs.self_hp = found->second.dynamic.current_hp.value;
    }
    return inputs;
}

QString popup_text(Popup popup, std::optional<std::int32_t> countdown_sec) {
    switch (popup) {
        case Popup::None:
            return QString();
        case Popup::LinkLost:
            return QStringLiteral("信号中断\n画面已冻结,数值非当前战况");
        case Popup::WaitingForData:
            return QStringLiteral("等待连接赛场");
        case Popup::Settlement:
            return QStringLiteral("本局结束");
        case Popup::Eliminated:
            return QStringLiteral("已阵亡\n等待复活");
        case Popup::Paused:
            return QStringLiteral("比赛暂停");
        case Popup::PreMatch:
            return countdown_sec && *countdown_sec > 0
                       ? QStringLiteral("比赛即将开始\n%1").arg(*countdown_sec)
                       : QStringLiteral("比赛即将开始");
    }
    return QString();
}

QString popup_title(Popup popup) {
    switch (popup) {
        case Popup::None:           return QString();
        case Popup::LinkLost:       return QStringLiteral("信号中断");
        case Popup::WaitingForData: return QStringLiteral("等待连接赛场");
        case Popup::Settlement:     return QStringLiteral("本局结束");
        case Popup::Eliminated:     return QStringLiteral("已阵亡");
        case Popup::Paused:         return QStringLiteral("比赛暂停");
        case Popup::PreMatch:       return QStringLiteral("比赛即将开始");
    }
    return QString();
}

QString popup_detail(Popup popup) {
    switch (popup) {
        case Popup::LinkLost:   return QStringLiteral("画面已冻结,数值非当前战况");
        case Popup::Eliminated: return QStringLiteral("等待复活");
        case Popup::None:
        case Popup::WaitingForData:
        case Popup::Settlement:
        case Popup::Paused:
        case Popup::PreMatch:
            return QString();
    }
    return QString();
}

QString popup_kind(Popup popup) {
    switch (popup) {
        case Popup::None:           return QString();
        case Popup::LinkLost:       return QStringLiteral("linkLost");
        case Popup::WaitingForData: return QStringLiteral("waiting");
        case Popup::Settlement:     return QStringLiteral("settlement");
        case Popup::Eliminated:     return QStringLiteral("eliminated");
        case Popup::Paused:         return QStringLiteral("paused");
        case Popup::PreMatch:       return QStringLiteral("preMatch");
    }
    return QString();
}

QString video_overlay_hp_text(const Snapshot& snapshot) {
    if (!snapshot.self) return QStringLiteral("血量 --");
    const auto found = snapshot.robots.find(*snapshot.self);
    if (found == snapshot.robots.end()) return QStringLiteral("血量 --");
    return QStringLiteral("血量 %1")
        .arg(ratio(found->second.dynamic.current_hp, found->second.statics.max_hp));
}

// 只在异常时浮现。计数器全零时它们不带信息量,却占掉图传格一半宽度;
// 但故障可见性是模块一的验收项,所以异常值一旦非零就必须显示,同时写日志。
VideoFaultCounts video_fault_counts(const VideoReceiver* video) {
    VideoFaultCounts counts;
    if (!video) return counts;
    const auto stats = video->snapshot();
    const auto count = [&stats](const char* key) {
        return stats.value(QLatin1String(key)).toInteger();
    };
    counts.missing = count("missing_packets");
    counts.out_of_order = count("out_of_order");
    counts.duplicates = count("duplicates");
    counts.incomplete = count("incomplete");
    counts.expired = count("expired");
    return counts;
}

QString VideoFaultTracker::faultLine(const VideoFaultCounts& counts, MonotonicMs now) {
    // 首次见到接收器时把当前累计值当作基线,而不是当作「刚刚发生的故障」。
    if (!seeded_) {
        baseline_ = counts;
        seeded_ = true;
        return QString();
    }

    QStringList faults;
    const auto delta = [&faults](qint64 current, qint64 base, const char* label) {
        const qint64 added = current - base;
        if (added > 0) faults << QString::fromUtf8(label) + QStringLiteral(" %1").arg(added);
        return added > 0;
    };

    bool any = false;
    any |= delta(counts.missing, baseline_.missing, "丢失");
    any |= delta(counts.out_of_order, baseline_.out_of_order, "乱序");
    any |= delta(counts.duplicates, baseline_.duplicates, "重复");
    any |= delta(counts.incomplete, baseline_.incomplete, "不完整");
    any |= delta(counts.expired, baseline_.expired, "超时");

    if (any) {
        held_ = faults.join(QStringLiteral("  "));
        visible_until_ = now + kVideoFaultHoldMs;
        baseline_ = counts;
        return held_;
    }

    if (now < visible_until_) return held_;
    held_.clear();
    return QString();
}

QString video_panel_text(const VideoReceiver* video, VideoFaultTracker* tracker,
                         MonotonicMs now) {
    if (!video) return QStringLiteral("图传未启用");
    // 一切正常时不复述状态:画面本身就是「在线」最好的证据,再顶一行 online 只是噪声。
    // 异常状态和故障计数仍要显示,那才是操作手需要读到的东西。
    const QString state = video->snapshot().value(QStringLiteral("state")).toString();
    const QString shown = video->online() ? QString() : state;
    if (!tracker) return shown;
    const QString faults = tracker->faultLine(video_fault_counts(video), now);
    if (faults.isEmpty()) return shown;
    if (shown.isEmpty()) return faults;
    return shown + QStringLiteral("\n") + faults;
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

void VideoPane::setScaling(Scaling scaling) {
    if (scaling_ == scaling) return;
    scaling_ = scaling;
    update();
}

void VideoPane::setCaption(bool caption) {
    if (caption_ == caption) return;
    caption_ = caption;
    update();
}

void VideoPane::setRotated(bool rotated) {
    if (rotated_ == rotated) return;
    rotated_ = rotated;
    update();
}

void Dashboard::setVideoRotated(bool rotated) {
    info_video_pane_->setRotated(rotated);
    video_full_pane_->setRotated(rotated);
}

void Dashboard::setPreMatchReminder(const QString& text) { pre_match_reminder_ = text; }

void Dashboard::setRuneEntry(const QString& text, bool lit, double progress, const QString& tooltip) {
    const bool shown = !text.isEmpty();
    if (shown == rune_entry_->isHidden()) {
        rune_entry_->setVisible(shown);
        placeAlertStrip();
    }
    if (!shown) return;
    if (rune_entry_->text() != text) {
        rune_entry_->setText(text);
        placeAlertStrip();
    }
    rune_entry_->setDot(lit ? theme::kOk : theme::kOffline);
    rune_entry_->setTextColour(lit ? theme::kTextPrimary : theme::kTextMuted);
    rune_entry_->setProgress(progress);
    if (rune_entry_->toolTip() != tooltip) rune_entry_->setToolTip(tooltip);
}

void Dashboard::setRuneEntryHold(std::function<void(bool down)> hold) {
    QObject::disconnect(rune_entry_, &QPushButton::pressed, this, nullptr);
    QObject::disconnect(rune_entry_, &QPushButton::released, this, nullptr);
    if (!hold) return;
    QObject::connect(rune_entry_, &QPushButton::pressed, this, [hold] { hold(true); });
    QObject::connect(rune_entry_, &QPushButton::released, this, [hold] { hold(false); });
}

void VideoPane::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        event->accept();
        Q_EMIT clicked();
        return;
    }
    QWidget::mousePressEvent(event);
}

void VideoPane::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF box(rect());
    if (caption_) painter.setClipPath(theme::chamferPath(box, 6));
    else painter.setClipRect(rect());
    painter.fillRect(rect(), theme::kVideoBackground);
    if (!image_.isNull()) {
        // Cover 用 KeepAspectRatioByExpanding 铺满,再靠 clip 裁掉溢出画面的部分,
        // 溢出量左右/上下均分,所以画面中心仍是格子中心。
        const Qt::AspectRatioMode mode = scaling_ == Scaling::Cover
                                             ? Qt::KeepAspectRatioByExpanding
                                             : Qt::KeepAspectRatio;
        const QImage scaled = image_.scaled(size(), mode, Qt::SmoothTransformation);
        const QPoint origin((width() - scaled.width()) / 2, (height() - scaled.height()) / 2);
        if (rotated_) {
            // 图传方向「反向」:绕格子中心转 180°,相机倒着装时画面才是正的。
            painter.save();
            painter.translate(width() / 2.0, height() / 2.0);
            painter.rotate(180);
            painter.translate(-width() / 2.0, -height() / 2.0);
            painter.drawImage(origin, scaled);
            painter.restore();
        } else {
            painter.drawImage(origin, scaled);
        }
    } else {
        painter.setFont(theme::labelPx(13));
        painter.setPen(theme::kTextSecondary);
        painter.drawText(rect(), Qt::AlignCenter, status_);
    }
    if (!caption_ || height() < 40) return;
    // 底部一条渐暗,写「图传 / 点击放大」:小窗能点这件事要看得出来。
    QLinearGradient shade(0, height() - 34, 0, height());
    shade.setColorAt(0, QColor(0, 0, 0, 0));
    shade.setColorAt(1, QColor(0, 0, 0, 153));
    painter.fillRect(QRectF(0, height() - 34, width(), 34), shade);
    painter.setFont(theme::labelPx(13));
    const QRectF line(10, height() - 34, width() - 20, 26);
    painter.setPen(Qt::white);
    painter.drawText(line, Qt::AlignLeft | Qt::AlignBottom, QStringLiteral("图传"));
    painter.setPen(QColor(255, 255, 255, 191));
    painter.drawText(line, Qt::AlignRight | Qt::AlignBottom, QStringLiteral("点击放大"));
}

void apply_frame(VideoPane* thumbnail, VideoPane* full, const QImage& frame) {
    thumbnail->setFrame(frame);
    full->setFrame(frame);
}

Dashboard::Dashboard(const Config& config, QWidget* parent)
    : QWidget(parent),
      machine_(config.mode_exit_hysteresis_ms, config.blind_stale_fallback_ms) {
    setObjectName(QStringLiteral("dashboardRoot"));
    theme::ensureFonts();
    if (const auto seat = Seat::fromRobotId(config.self_robot_id)) {
        role_ = seat->role();
        seat_label_ = seat->label();
    } else {
        seat_label_ = QStringLiteral("席位 %1").arg(config.self_robot_id);
    }
    own_faction_ = config.self_robot_id > 100 ? 2u : 1u;
    self_robot_id_ = config.self_robot_id > 0 ? static_cast<std::uint32_t>(config.self_robot_id) : 3u;
    mark_lifetime_ms_ = config.map_mark_ms;
    setStyleSheet(theme::styleSheet());

    auto* root = new QVBoxLayout(this);
    // 效果图的外边距:左右下 8、上 6;状态条和顶栏之间 2,顶栏和面板区之间 8。
    root->setContentsMargins(8, 6, 8, 8);
    root->setSpacing(0);

    // 状态条一行 28 高。左边只用来看(席位、链路灯、录制),右边是按钮,计时器正上方放告警带。
    mode_banner_ = new ChipLabel(this);
    mode_banner_->setObjectName(QStringLiteral("modeBanner"));
    QFont seat_font = theme::labelPx(13, QFont::Bold);
    seat_font.setLetterSpacing(QFont::AbsoluteSpacing, 0.5);
    mode_banner_->setFont(seat_font);
    mode_banner_->setTick(own_faction_ == 2 ? theme::kBlue : theme::kRed);
    mode_banner_->setFixedHeight(kStatusHeight);
    mode_banner_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    // 常态隐藏。一条永远在屏幕上的告警带等于没有告警带 —— 操作手会在三十秒内
    // 学会无视它,而它要传达的恰恰是「现在必须立刻看一眼」。
    // 不进状态条的布局,由 placeAlertStrip() 摆在比分正上方。
    alert_strip_ = new ChipLabel(this);
    alert_strip_->setObjectName(QStringLiteral("alertStrip"));
    alert_strip_->setTone(ChipLabel::Tone::Alert);
    alert_strip_->setFont(theme::labelPx(13, QFont::Bold));
    alert_strip_->setFixedHeight(kStatusHeight);
    alert_strip_->setVisible(false);

    const auto chip_button = [this](const QString& text, const char* name) {
        auto* button = new GlassButton(text, this, GlassButton::Kind::Chip);
        button->setObjectName(QString::fromLatin1(name));
        button->setFixedHeight(kStatusHeight);
        // 水平锁 Fixed。QPushButton 默认是 Minimum,会跟告警带一起均分状态条的横向余量,
        // 实测 1280 宽下按钮被拉到 419px。Fixed 让它只占 sizeHint,始终贴住右端。
        button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        return button;
    };
    reminder_entry_ = chip_button(QStringLiteral("战术提醒"), "reminderEntry");
    reminder_entry_->setDimTail(true);
    // 未挂控制器时不可用。可点但什么都不弹会让操作手以为功能坏了。
    reminder_entry_->setEnabled(false);

    recording_status_ = new ChipLabel(this);
    recording_status_->setObjectName(QStringLiteral("recordingStatus"));
    recording_status_->setTextFormat(Qt::RichText);
    recording_status_->setFixedHeight(kStatusHeight);
    recording_status_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    recording_status_->setVisible(false);

    link_lights_ = new ChipLabel(this);
    link_lights_->setObjectName(QStringLiteral("linkLights"));
    link_lights_->setTextFormat(Qt::RichText);
    link_lights_->setFixedHeight(kStatusHeight);
    link_lights_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    link_lights_->setVisible(false);

    self_check_entry_ = chip_button(QStringLiteral("自检"), "selfCheckEntry");
    self_check_entry_->setVisible(false);
    QObject::connect(self_check_entry_, &QPushButton::clicked, this,
                     [this] { setSelfCheckVisible(!selfCheckVisible()); });

    // 键鼠控制开关放在状态条而不是本车面板:它在信息页和图传全屏页都点得到。
    // 只有重装、步兵席位会挂上开关,其他席位一直隐藏。
    keyboard_mouse_entry_ = chip_button(QStringLiteral("键鼠控制 F9"), "keyboardMouseEntry");
    keyboard_mouse_entry_->setDimTail(true);
    keyboard_mouse_entry_->setVisible(false);

    ui_settings_entry_ = chip_button(QStringLiteral("界面设置"), "uiSettingsEntry");
    ui_settings_entry_->setVisible(false);

    rune_entry_ = chip_button(QString(), "runeEntry");
    rune_entry_->setVisible(false);
    QObject::connect(ui_settings_entry_, &QPushButton::clicked, this,
                     [this] { setUiSettingsVisible(!uiSettingsVisible()); });

    auto* banner_row = new QHBoxLayout();
    banner_row->setContentsMargins(0, 0, 0, 0);
    banner_row->setSpacing(6);
    banner_row->addWidget(mode_banner_);
    // 状态灯紧跟席位。告警带不在这一行里排,出现、消失都不会让状态灯挪位置。
    banner_row->addWidget(link_lights_);
    banner_row->addWidget(recording_status_);
    banner_row->addWidget(rune_entry_);
    banner_row->addStretch(1);
    banner_row->addWidget(self_check_entry_);
    banner_row->addWidget(keyboard_mouse_entry_);
    banner_row->addWidget(reminder_entry_);
    banner_row->addWidget(ui_settings_entry_);
    root->addLayout(banner_row);
    root->addSpacing(2);

    top_bar_ = new TopBar(this);
    top_bar_->setObjectName(QStringLiteral("topBar"));
    root->addWidget(top_bar_);
    root->addSpacing(8);

    stack_ = new QStackedLayout();
    info_page_ = new QWidget(this);
    video_page_ = new QWidget(this);

    // 信息页按 16 × 9 格排(layout_profile.h),格子之间 8 像素。
    grid_ = new UnitGridLayout(info_page_, kGridColumns, kGridRows, 8);

    // 花名册是本终端相对官方选手端的核心增量 —— 官方端看不到对方血量,这两列是唯一来源。
    // 标题、卡面、边框都由 Panel.qml 画,QML 面板直接进 grid。
    ally_roster_ = new RosterPanel(true, info_page_);
    ally_roster_->setObjectName(QStringLiteral("allyRoster"));
    enemy_roster_ = new RosterPanel(false, info_page_);
    enemy_roster_->setObjectName(QStringLiteral("enemyRoster"));

    auto* map_box = new GlassCard(QStringLiteral("战术地图"), info_page_);
    map_box->setObjectName(QStringLiteral("mapCard"));
    // 地图紧贴标题栏,左右下各留 8(效果图)。
    map_box->body()->setContentsMargins(8, GlassCard::kHeaderHeight, 8, 8);
    map_box->setHeaderRight(QStringLiteral(
        "<span style='color:#d64a4a'>●</span>&nbsp;红方&nbsp;&nbsp;&nbsp;"
        "<span style='color:#2f7fd8'>●</span>&nbsp;蓝方&nbsp;&nbsp;&nbsp;"
        "<span style='color:#12b76a'>●</span>&nbsp;本机"));
    map_ = new MapPane();
    map_->setObjectName(QStringLiteral("mapPane"));
    const FieldExtent field = default_field_extent();
    // 场地永远按 28:15 画:格子比场地宽就左右居中,比场地高就上下居中,不拉伸。
    auto* map_frame = new AspectFrame(map_, field.width_m / field.height_m, map_box);
    map_box->body()->addWidget(map_frame, 1);
    // 指挥手:点地图下标记,工具条紧贴地图下沿。
    if (role_ == Role::Commander) {
        mark_toolbar_ = new MarkToolbar(map_box);
        map_frame->setFooter(mark_toolbar_);
        map_->setClickHandler(
            [this](double x_m, double y_m, std::optional<std::uint32_t> enemy_id) {
                sendMark(x_m, y_m, enemy_id);
            },
            own_faction_);
    }

    // 图传小窗和它下面的故障行直接放进同一张卡片:多套一层盒子,故障行变成两行时卡片要
    // 晚一轮才知道自己该长高,那一轮里故障行会压到画面上。
    info_video_pane_ = new VideoPane(info_page_);
    info_video_pane_->setObjectName(QStringLiteral("infoVideoPane"));
    // 不设宽高上限:paintEvent 已按比例居中缩放,让它铺满整格即可。只保留下限:窗口变矮时
    // 画面缩小,而不是压到故障行上。
    info_video_pane_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    info_video_pane_->setScaling(VideoPane::Scaling::Cover);
    info_video_pane_->setCaption(true);
    // 最矮 72:图传单独一块 2 格高时,1280 × 760 的小窗口里再加两行故障也放得下。
    info_video_pane_->setMinimumSize(160, 72);
    info_video_pane_->setCursor(Qt::PointingHandCursor);
    info_video_pane_->setToolTip(QStringLiteral("点击放大图传"));
    QObject::connect(info_video_pane_, &VideoPane::clicked, this, &Dashboard::showVideoPage);

    // 故障行在画面下方而不是右侧:异常时那行故障计数很长,并排放不下就会压到画面上。
    // 常态是空串,WrappedLabel 负责把换行后的真实高度报给布局,所以空行不占高度。
    video_stats_ = new WrappedLabel(QString(), info_page_);
    video_stats_->setObjectName(QStringLiteral("videoStats"));
    video_stats_->setFont(theme::numericPx(12, QFont::Medium));
    video_stats_->setStyleSheet(QStringLiteral("color:%1;").arg(theme::css(theme::kWarn)));
    video_stats_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    // 纵向 Minimum 而不是 MinimumExpanding:后者会让这个常态为空串的标签把盈余高度
    // 吃掉,画面被顶到上方。盈余要留给画面。
    QSizePolicy stats_policy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    stats_policy.setHeightForWidth(true);
    video_stats_->setSizePolicy(stats_policy);

    // 图传卡片不要标题栏:画面左下角自己写着「图传」「点击放大」,省下的高度都给画面。
    auto* video_card = new GlassCard(QString(), info_page_);
    video_card->setObjectName(QStringLiteral("videoCard"));

    auto* event_box = new GlassCard(QStringLiteral("战场事件"), info_page_);
    event_box->setObjectName(QStringLiteral("eventCard"));
    event_ = new EventFeedPane(event_box);
    event_->setObjectName(QStringLiteral("eventPanel"));
    // Ignored 纵向策略让面板既不撑高窗口最小高度,也不给自己设上限 —— 高度完全由那一格
    // 分到多少决定,画满为止。
    event_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Ignored);
    event_box->body()->addWidget(event_, 1);

    auto* analysis_box = new GlassCard(QStringLiteral("战场数据分析"), info_page_);
    analysis_box->setObjectName(QStringLiteral("analysisCard"));
    analysis_ = new BattleAnalysisPane(analysis_box);
    analysis_->setObjectName(QStringLiteral("battleAnalysisPane"));
    analysis_->setOwnFaction(own_faction_);
    analysis_box->body()->addWidget(analysis_, 1);

    // 「下一步」三张卡:中间一列最上面。「有急事才弹出」时格子里不放,急事来了浮在战场数据
    // 分析上面,处理完自动收起。
    auto* next_box = new GlassCard(QStringLiteral("下一步"), info_page_);
    next_box->setObjectName(QStringLiteral("nextStepCard"));
    next_box->setHeaderRight(QStringLiteral("对我方最有利的排最前"));
    next_pane_ = new NextStepPane(next_box);
    next_pane_->setObjectName(QStringLiteral("nextStepPane"));
    next_box->body()->addWidget(next_pane_, 1);
    next_popup_ = new GlassCard(QStringLiteral("下一步 · 有急事"), this);
    next_popup_->setObjectName(QStringLiteral("nextStepPopup"));
    next_popup_->setAccent(theme::kDanger);
    next_popup_->setFill(theme::kOverlay);
    next_popup_->setHeaderRight(QStringLiteral("处理完自动收起"));
    next_popup_pane_ = new NextStepPane(next_popup_);
    next_popup_->body()->addWidget(next_popup_pane_, 1);
    next_popup_->setAttribute(Qt::WA_TransparentForMouseEvents);
    next_popup_->setVisible(false);

    // 角色面板的卡片。面板本身由主程序放进来(placePane()),这里只给卡片和标题,
    // 让三种布局在没有数据时也是完整的骨架。
    const auto role_card = [this](const QString& heading, const char* name) {
        auto* box = new GlassCard(heading, info_page_);
        box->setObjectName(QString::fromLatin1(name));
        return box;
    };

    // 组件库里能加的两块(默认画面里没有)。
    auto* self_status_box = new GlassCard(QStringLiteral("本机状态"), info_page_);
    self_status_box->setObjectName(QStringLiteral("selfStatusCard"));
    self_status_ = new SelfStatusPane(self_status_box);
    self_status_box->body()->addWidget(self_status_, 1);
    auto* timeline_box = new GlassCard(QStringLiteral("接下来的时间点"), info_page_);
    timeline_box->setObjectName(QStringLiteral("timelineCard"));
    timeline_ = new TimelinePane(timeline_box);
    timeline_box->body()->addWidget(timeline_, 1);

    pane_cards_ = {{PaneId::AllyRoster, ally_roster_},
                   {PaneId::EnemyRoster, enemy_roster_},
                   {PaneId::Map, map_box},
                   {PaneId::Events, event_box},
                   {PaneId::Analysis, analysis_box},
                   {PaneId::Video, video_card},
                   {PaneId::Assembly, role_card(QStringLiteral("装配"), "assemblyCard")},
                   {PaneId::Dart, role_card(QStringLiteral("飞镖"), "dartCard")},
                   {PaneId::Sentry, role_card(QStringLiteral("哨兵"), "sentryCard")},
                   {PaneId::SelfCommands, role_card(QStringLiteral("本车"), "selfCommandsCard")},
                   {PaneId::NextStep, next_box},
                   {PaneId::SelfStatus, self_status_box},
                   {PaneId::Timeline, timeline_box}};

    pane_cards_.at(PaneId::SelfCommands)->installEventFilter(this);
    // 没挂界面设置(测试、回放截图)时按标准方案排:「下一步」不开。
    applyUiScheme(builtin_scheme(role_, QStringLiteral("standard")).value_or(UiScheme{}));

    auto* video_layout = new QVBoxLayout(video_page_);
    video_layout->setContentsMargins(0, 0, 0, 0);
    video_layout->setSpacing(6);
    auto* overlay = new QWidget(video_page_);
    overlay->setObjectName(QStringLiteral("videoOverlay"));
    overlay->setStyleSheet(QStringLiteral("QWidget#videoOverlay{background:%1; border:1px solid %2;}")
                               .arg(theme::css(theme::kChip), theme::css(theme::kRowBorder)));
    auto* overlay_row = new QHBoxLayout(overlay);
    overlay_row->setContentsMargins(10, 3, 10, 3);
    // Countdown and HP stay on screen in video mode: during blinding they are the
    // only match state left that the operator can still act on.
    video_countdown_ = new QLabel(overlay);
    video_countdown_->setFont(theme::labelPx(14, QFont::Bold));
    video_countdown_->setObjectName(QStringLiteral("videoOverlayCountdown"));
    video_hp_ = new QLabel(overlay);
    video_hp_->setFont(theme::labelPx(14, QFont::Bold));
    video_hp_->setObjectName(QStringLiteral("videoOverlayHp"));
    overlay_row->addWidget(video_countdown_);
    overlay_row->addStretch();
    overlay_row->addWidget(video_hp_);
    video_layout->addWidget(overlay);

    video_full_pane_ = new VideoPane(video_page_);
    video_full_pane_->setObjectName(QStringLiteral("videoFullPane"));
    video_full_pane_->setCursor(Qt::PointingHandCursor);
    video_full_pane_->setToolTip(QStringLiteral("点击返回自动视图（致盲或强制图传时保持全屏）"));
    video_layout->addWidget(video_full_pane_, 1);
    QObject::connect(video_full_pane_, &VideoPane::clicked, this, &Dashboard::showInfoPage);

    stack_->addWidget(info_page_);
    stack_->addWidget(video_page_);
    root->addLayout(stack_, 1);

    // 播放器式控制条。只存在于回放,实况下隐藏,不占布局高度。
    replay_bar_ = new QWidget(this);
    replay_bar_->setObjectName(QStringLiteral("replayBar"));
    replay_bar_->setStyleSheet(
        QStringLiteral("QWidget#replayBar{background:%1;border-top:1px solid %2;}"
                       "QWidget#replayBar QLabel{color:%3;}"
                       "QWidget#replayBar QPushButton{background:%4;color:%5;"
                       "border:1px solid %2;padding:2px 10px;font-weight:bold;}"
                       "QWidget#replayBar QComboBox{background:%4;color:%5;"
                       "border:1px solid %2;padding:2px 6px;}")
            .arg(theme::css(theme::kChip), theme::css(theme::kBorder), theme::css(theme::kTextBright),
                 theme::css(theme::kSurfaceRaised), theme::css(theme::kTextPrimary)));
    replay_bar_->setMinimumHeight(40);
    auto* replay_row = new QHBoxLayout(replay_bar_);
    replay_row->setContentsMargins(8, 4, 8, 4);
    replay_row->setSpacing(8);

    replay_play_ = new QPushButton(QStringLiteral("⏸"), replay_bar_);
    replay_play_->setObjectName(QStringLiteral("replayPlay"));
    replay_play_->setFixedWidth(40);
    replay_row->addWidget(replay_play_);

    replay_slider_ = new QSlider(Qt::Horizontal, replay_bar_);
    replay_slider_->setObjectName(QStringLiteral("replaySlider"));
    replay_slider_->setRange(0, 0);
    replay_slider_->setSingleStep(1000);
    replay_slider_->setPageStep(10000);
    replay_slider_->setStyleSheet(
        QStringLiteral("QSlider::groove:horizontal{height:4px;background:%1;}"
                       "QSlider::sub-page:horizontal{background:%2;}"
                       "QSlider::handle:horizontal{width:12px;margin:-5px 0;background:%3;}")
            .arg(theme::css(QColor(255, 255, 255, 31)), theme::css(theme::kCyan), theme::css(theme::kTextPrimary)));
    replay_row->addWidget(replay_slider_, 1);

    replay_time_ = new QLabel(QStringLiteral("00:00 / 00:00"), replay_bar_);
    replay_time_->setObjectName(QStringLiteral("replayTime"));
    replay_row->addWidget(replay_time_);

    auto* replay_speed_label = new QLabel(QStringLiteral("倍速"), replay_bar_);
    replay_speed_label->setObjectName(QStringLiteral("replaySpeedLabel"));
    replay_row->addWidget(replay_speed_label);

    replay_speed_ = new QComboBox(replay_bar_);
    replay_speed_->setObjectName(QStringLiteral("replaySpeed"));
    for (const double speed : {0.5, 1.0, 2.0, 4.0})
        replay_speed_->addItem(QStringLiteral("%1x").arg(speed), speed);
    replay_speed_->setCurrentIndex(1);
    replay_speed_->setFixedWidth(72);
    replay_speed_->setToolTip(QStringLiteral("播放速度：0.5x / 1x / 2x / 4x"));
    replay_row->addWidget(replay_speed_);

    QObject::connect(replay_play_, &QPushButton::clicked, this, [this] {
        if (replay_pause_) replay_pause_();
    });
    QObject::connect(replay_slider_, &QSlider::sliderReleased, this, [this] {
        if (replay_seek_) replay_seek_(replay_slider_->value());
    });
    QObject::connect(replay_slider_, &QSlider::valueChanged, this, [this](int value) {
        if (replay_slider_->isSliderDown()) return;
        if (replay_seek_) replay_seek_(value);
    });
    QObject::connect(replay_speed_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                     [this](int index) {
                         if (replay_speed_handler_)
                             replay_speed_handler_(replay_speed_->itemData(index).toDouble());
                     });

    replay_bar_->setVisible(false);
    root->addWidget(replay_bar_);

    // 不进任何 layout:一个会改变布局的弹窗等于在最紧张的时刻把整屏控件推位,
    // 操作手的肌肉记忆随之失效。它悬浮在最上层,几何由 resizeEvent 单独维护。
    popup_ = new QQuickWidget(this);
    popup_->setObjectName(QStringLiteral("popupOverlay"));
    popup_->setResizeMode(QQuickWidget::SizeRootObjectToView);
    // 铺满整窗但要能透出下面的地图/图传:压暗层是 QML 自己画的半透明矩形,
    // 宿主必须真透明,否则整屏会被 Quick 场景的背景色糊死。
    popup_->setClearColor(Qt::transparent);
    popup_->setAttribute(Qt::WA_TranslucentBackground);
    popup_->setAttribute(Qt::WA_AlwaysStackOnTop);
    // 必需:铺满整窗 + AlwaysStackOnTop 会吞掉全屏点击,备战阶段弹窗可见时顶部按钮全部失效。
    popup_->setAttribute(Qt::WA_TransparentForMouseEvents);
    popup_->setSource(QUrl(QStringLiteral("qrc:/qml/PopupOverlay.qml")));
    popup_->setVisible(false);

    victory_ = new QQuickWidget(this);
    victory_->setObjectName(QStringLiteral("victoryOverlay"));
    victory_->setResizeMode(QQuickWidget::SizeRootObjectToView);
    victory_->setClearColor(Qt::transparent);
    victory_->setAttribute(Qt::WA_TranslucentBackground);
    victory_->setAttribute(Qt::WA_AlwaysStackOnTop);
    victory_->setAttribute(Qt::WA_TransparentForMouseEvents);
    victory_->setSource(QUrl(QStringLiteral("qrc:/qml/VictoryOverlay.qml")));
    victory_->setVisible(false);

    // 我方基地在地图哪一侧。场地图红方在左。
    our_base_left_ = config.self_robot_id <= 100;

    // 提醒横幅盖在地图上方(效果图),不进 layout:出现和消失都不能推动别的控件。
    alert_banner_ = new GlowBanner(this);
    alert_banner_->setObjectName(QStringLiteral("alertBanner"));

    // 和弹窗一样不进 layout:它出现时不能把整屏控件推位。
    self_check_ = new SelfCheckPanel(this);
    self_check_->setVisible(false);
    QObject::connect(self_check_, &SelfCheckPanel::closeRequested, this,
                     [this] { setSelfCheckVisible(false); });
}

void Dashboard::layoutCells(const LayoutProfile& profile) {
    current_profile_ = profile;
    // 运行中也会重排(操作手换方案、在界面设置里拖):先把所有卡片摘下来,再按格子放回去。
    grid_->clearCells();
    for (auto& [pane, card] : pane_cards_) card->setVisible(false);
    for (const PaneSlot& slot : profile.cells) {
        const auto found = pane_cards_.find(slot.pane);
        if (found == pane_cards_.end()) continue;
        QWidget* card = found->second;
        grid_->addWidget(card, slot.units());
        // 贴着右边的面板切角镜像(效果图)。
        const bool mirrored = slot.column + slot.column_span == kGridColumns && slot.column > 0;
        if (auto* glass = qobject_cast<GlassCard*>(card)) glass->setMirrored(mirrored);
        if (auto* roster = qobject_cast<RosterPanel*>(card)) roster->setMirrored(mirrored);
        card->setVisible(true);
    }
    placeVideo();
    grid_->invalidate();
}

void Dashboard::placeVideo() {
    // 图传跟着布局走:放进「图传」卡片。席位表里写了要并进别的卡片(现在没有席位这样用)才放进那张卡片的下半截。
    const std::optional<PaneId> host_pane = embedded_host(current_profile_, PaneId::Video);
    auto* host = static_cast<GlassCard*>(pane_cards_.at(host_pane.value_or(PaneId::Video)));
    if (info_video_pane_->parentWidget() != host) {
        for (QWidget* widget : {static_cast<QWidget*>(info_video_pane_), static_cast<QWidget*>(video_stats_)}) {
            if (auto* old = qobject_cast<GlassCard*>(widget->parentWidget())) old->body()->removeWidget(widget);
            widget->setParent(host);
        }
        host->body()->addWidget(info_video_pane_, 1);
        host->body()->addWidget(video_stats_, 0);
    }
    // 「本车」里的指令面板:和小窗合住时只要自己那么高,单独住时铺满。
    auto* commands = static_cast<GlassCard*>(pane_cards_.at(PaneId::SelfCommands));
    const bool shares = info_video_pane_->parentWidget() == commands;
    if (commands->body()->count() > 0)
        if (QWidget* first = commands->body()->itemAt(0)->widget(); first && first != info_video_pane_) {
            commands->body()->setStretch(0, shares ? 0 : 1);
            // 单独住时按钮靠上排,多出来的高度留在下面,不把几排按钮拉得稀稀拉拉。
            commands->body()->setAlignment(first, shares ? Qt::Alignment() : Qt::AlignTop);
        }
    const bool show = !video_hidden_;
    info_video_pane_->setVisible(show);
    video_stats_->setVisible(show);
    capVideo();
}

void Dashboard::capVideo() {
    // 小窗最高按 16:9 算:卡片里指令少(赛前只有两行)时多出来的高度留在下面,不把小窗拉成竖条。
    QWidget* host = info_video_pane_->parentWidget();
    const bool shares = host == pane_cards_.at(PaneId::SelfCommands);
    const int cap = shares ? std::max(90, (host->width() - 16) * 9 / 16) : QWIDGETSIZE_MAX;
    if (info_video_pane_->maximumHeight() != cap) info_video_pane_->setMaximumHeight(cap);
    if (!shares || video_hidden_) return;
    // 「本车」被拖得太矮,指令下面放不下小窗:先不放,别压在按钮上。拉高了自己回来。
    auto* card = static_cast<GlassCard*>(host);
    QWidget* commands = card->body()->count() > 0 ? card->body()->itemAt(0)->widget() : nullptr;
    if (!commands || commands == info_video_pane_) return;
    // 按卡片自己的高度算:布局刚失效时它记的区域是空的。
    const QMargins margins = card->body()->contentsMargins();
    const int room = card->height() - margins.top() - margins.bottom() - commands->sizeHint().height() -
                     video_stats_->sizeHint().height() - 2 * card->body()->spacing();
    const bool fits = room >= 72;
    if (info_video_pane_->isVisibleTo(card) != fits) {
        info_video_pane_->setVisible(fits);
        video_stats_->setVisible(fits);
    }
}

bool Dashboard::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::Resize && watched == pane_cards_.at(PaneId::SelfCommands)) capVideo();
    return QWidget::eventFilter(watched, event);
}

void Dashboard::applyUiScheme(const UiScheme& scheme, bool editing) {
    const UiOptions& options = scheme.options;
    LayoutProfile profile = scheme_layout(role_, scheme);
    const bool next_placed = std::any_of(profile.cells.begin(), profile.cells.end(),
                                         [](const PaneSlot& slot) { return slot.pane == PaneId::NextStep; });
    next_enabled_ = next_placed;
    next_popup_mode_ = next_enabled_ && options.next_mode == 1;
    next_options_ = options;
    // 「有急事才弹出」:格子里不放,平时画面和标准方案一样。设置页里照样画出来,好摆位置。
    if (next_popup_mode_ && !editing) {
        std::set<PaneId> hidden = scheme.hidden;
        hidden.insert(PaneId::NextStep);
        UiScheme without = scheme;
        without.hidden = hidden;
        profile = scheme_layout(role_, without);
    }
    video_hidden_ = scheme.hidden.count(PaneId::Video) > 0;
    layoutCells(profile);
    for (NextStepPane* pane : {next_pane_, next_popup_pane_}) {
        pane->setCardSize(options.next_size);
        pane->setCapacity(options.next_count);
    }
    if (!next_enabled_) {
        next_cards_.clear();
        rune_badge_.clear();
        rune_entry_->setBadge(QString());
        rune_entry_->setGlow(false);
        enemy_roster_->setRanks({});
        next_popup_->setVisible(false);
    }
    map_->setDisplayOptions(options.map_background_percent / 100.0, options.map_show_enemies,
                            options.map_show_marks, own_faction_);
    analysis_->setControlOnly(options.analysis_control_only);
    setPaneHeader(PaneId::Analysis, analysis_->headerText());
    ally_roster_->setLowHealthPulse(options.enemy_low_hp_pulse);
    enemy_roster_->setLowHealthPulse(options.enemy_low_hp_pulse);
}

void Dashboard::updateNextStep(const Snapshot& snapshot, MonotonicMs now) {
    if (!next_enabled_) return;
    std::vector<NextGroup> order;
    for (const QString& key : next_options_.next_order)
        if (const auto group = next_group_from_key(key)) order.push_back(*group);
    if (order.empty()) order = default_next_order();
    next_cards_ = next_board_.update(snapshot, now, order, static_cast<std::size_t>(next_options_.next_count),
                                     static_cast<MonotonicMs>(next_options_.next_dwell_sec) * 1000);
    const bool urgent = std::any_of(next_cards_.begin(), next_cards_.end(), [](const NextCard& c) { return c.urgent; });
    const bool showing = next_popup_mode_ ? urgent : true;
    if (next_popup_mode_) {
        next_popup_pane_->setCards(next_cards_);
        next_popup_->setVisible(urgent && mode() == UiMode::Info && !uiSettingsVisible() && !selfCheckVisible());
        if (next_popup_->isVisible()) placeNextPopup();
    } else {
        next_pane_->setCards(next_cards_);
    }
    // 卡片指向的地方标同一个编号:集火的那台敌方车、「能量机关 · 按住」按钮。
    QVariantMap ranks;
    rune_badge_.clear();
    if (showing)
        for (std::size_t i = 0; i < next_cards_.size(); ++i) {
            const QString rank = QString::number(i + 1);
            if (next_cards_[i].enemy_id) ranks.insert(QString::number(*next_cards_[i].enemy_id), rank);
            if (next_cards_[i].rune_button) rune_badge_ = rank;
        }
    // 推荐开能量机关时,状态条上「F · 长按开启」那一格标同一个编号并发光。
    rune_entry_->setBadge(rune_badge_);
    rune_entry_->setGlow(!rune_badge_.isEmpty());
    enemy_roster_->setRanks(ranks);
    // 第 1 件换成了另一件事才念,同一件事数字变了不念。
    const std::optional<NextRule> top =
        showing && !next_cards_.empty() ? std::optional<NextRule>(next_cards_.front().rule) : std::nullopt;
    if (top != spoken_rule_) {
        spoken_rule_ = top;
        if (top && next_options_.next_voice) pending_speech_ = next_cards_.front().speech;
    }
}

void Dashboard::placeNextPopup() {
    // 浮在中间一列的上半截(战场数据分析那里),不挡花名册和地图。
    const QWidget* anchor = pane_cards_.at(PaneId::Analysis);
    if (!anchor->isVisible()) anchor = map_;
    const QPoint at = anchor->mapTo(this, QPoint(0, 0));
    const int height = std::min(anchor->height(), 168);
    next_popup_->setGeometry(at.x(), at.y(), anchor->width(), std::max(120, height));
    next_popup_->raise();
}

QString Dashboard::takeNextSpeech() {
    QString speech;
    std::swap(speech, pending_speech_);
    return speech;
}

void Dashboard::attachUiProfiles(UiProfileController* controller, const QStringList& config_disabled_alerts) {
    ui_profiles_ = controller;
    if (!controller) return;
    layout_editor_ = new LayoutEditor(controller, this);
    layout_editor_->setConfigDisabledAlerts(config_disabled_alerts);
    layout_editor_->setVisible(false);
    layout_editor_->setPaneImageSource([this](PaneId pane, QSize size) { return paneImage(pane, size); });
    // 主界面按设置页正在改的那一份排,排完截图给设置页当真画面。拖缝时一秒要排很多次,截图攒一下再截;
    // 平时每秒截一次,画面里的数据是活的。
    preview_grab_ = new QTimer(this);
    preview_grab_->setSingleShot(true);
    preview_grab_->setInterval(120);
    QObject::connect(preview_grab_, &QTimer::timeout, this, &Dashboard::refreshEditorPreview);
    preview_timer_ = new QTimer(this);
    preview_timer_->setInterval(1000);
    QObject::connect(preview_timer_, &QTimer::timeout, this, &Dashboard::refreshEditorPreview);
    QObject::connect(layout_editor_, &LayoutEditor::previewRequested, this, [this] {
        if (!uiSettingsVisible()) return;
        applyUiScheme(layout_editor_->working(), true);
        // 拖缝时一直在要:最多每 120ms 截一次,不是等停下来才截。
        if (!preview_grab_->isActive()) preview_grab_->start();
    });
    QObject::connect(layout_editor_, &LayoutEditor::doneRequested, this, [this] { setUiSettingsVisible(false); });
    QObject::connect(controller, &UiProfileController::changed, this, [this] {
        // 设置页开着时主界面跟着设置页排(previewRequested),关着时按存盘的方案排。
        if (!uiSettingsVisible()) applyUiScheme(ui_profiles_->activeScheme());
    });
    ui_settings_entry_->setVisible(true);
    applyUiScheme(controller->activeScheme());
}

bool Dashboard::uiSettingsVisible() const { return layout_editor_ && layout_editor_->isVisible(); }

void Dashboard::setUiSettingsVisible(bool visible) {
    if (!layout_editor_ || !ui_profiles_ || layout_editor_->isVisible() == visible) return;
    if (visible && ui_locked_) return;
    if (!visible) {
        preview_timer_->stop();
        preview_grab_->stop();
        layout_editor_->setVisible(false);
        setEditingLook(false);
        applyUiScheme(ui_profiles_->activeScheme());
        return;
    }
    // 和自检页、弹窗、横幅不同时出现:设置页盖住整个窗口,下面那层弹出来谁都看不清。
    setSelfCheckVisible(false);
    showInfoPage();
    if (popup_) popup_->setVisible(false);
    next_popup_->setVisible(false);
    alert_banner_->setVisible(false);
    // 先截一张现在的主画面:组件库里花名册这类 QML 面板只能从这里取缩略图。
    rememberPaneShots(windowShot());
    setEditingLook(true);
    layout_editor_->begin();
    layout_editor_->setGeometry(rect());
    layout_editor_->setVisible(true);
    layout_editor_->raise();
    layout_editor_->setFocus();
    applyUiScheme(layout_editor_->working(), true);
    preview_grab_->start();
    preview_timer_->start();
}

QPixmap Dashboard::windowShot() {
    const qreal dpr = devicePixelRatioF();
    QPixmap shot(size() * dpr);
    shot.setDevicePixelRatio(dpr);
    QPainter painter(&shot);
    painter.drawImage(QPoint(0, 0), theme::backdrop(size(), dpr));
    // 一块一块画到同一张背景上(不铺各自的底色),半透明的玻璃面板才和主界面上一样深。设置页自己不画。
    for (QObject* object : children()) {
        auto* child = qobject_cast<QWidget*>(object);
        if (!child || child == layout_editor_ || child->isWindow() || !child->isVisible()) continue;
        child->render(&painter, child->pos(), QRegion(), QWidget::DrawChildren);
    }
    return shot;
}

void Dashboard::refreshEditorPreview() {
    if (!uiSettingsVisible()) return;
    const QPixmap shot = windowShot();
    rememberPaneShots(shot);
    QRect grid = grid_->contentsRect();
    grid.translate(info_page_->mapTo(this, QPoint(0, 0)));
    layout_editor_->setPreview(shot, grid);
}

void Dashboard::rememberPaneShots(const QPixmap& shot) {
    const qreal dpr = shot.devicePixelRatio();
    for (const PaneSlot& slot : current_profile_.cells) {
        const auto card = pane_cards_.find(slot.pane);
        if (card == pane_cards_.end() || !card->second->isVisible()) continue;
        const QRect box(card->second->mapTo(this, QPoint(0, 0)), card->second->size());
        QPixmap piece = shot.copy(QRect(box.topLeft() * dpr, box.size() * dpr));
        piece.setDevicePixelRatio(dpr);
        pane_shots_[slot.pane] = piece;
    }
}

void Dashboard::setEditingLook(bool on) {
    if (on == editing_look_) return;
    editing_look_ = on;
    // 设置页在每块右上角画齿轮和叉(缩小后约 60 像素宽):标题栏右边的字往左让,照样显示。
    constexpr int kToolInset = 76;
    for (const auto& [pane, widget] : pane_cards_)
        if (auto* card = qobject_cast<GlassCard*>(widget)) card->setHeaderInset(on ? kToolInset : 0);
    ally_roster_->setToolInset(on ? kToolInset : 0);
    enemy_roster_->setToolInset(on ? kToolInset : 0);
}

namespace {

// 组件库缩略图:先铺和主界面一样的窗口背景,再把卡片画上去(不铺它自己的底色),玻璃才不会发灰。
QPixmap over_backdrop(QWidget* card, QSize size, qreal dpr) {
    QPixmap out(size * dpr);
    out.setDevicePixelRatio(dpr);
    QPainter painter(&out);
    painter.drawImage(QPoint(0, 0), theme::backdrop(size, dpr));
    card->render(&painter, QPoint(), QRegion(), QWidget::DrawChildren);
    return out;
}

// 设置页预览用的三张卡,和效果图 7 情况 1 一样。
std::vector<NextCard> sample_next_cards() {
    std::vector<NextCard> cards(3);
    cards[0].category = QStringLiteral("进攻");
    cards[0].title = QStringLiteral("集火敌方 4 号步兵");
    cards[0].reason = QStringLiteral("只剩 46 血，离你约 3 米");
    cards[1].category = QStringLiteral("增益");
    cards[1].title = QStringLiteral("开大能量机关");
    cards[1].reason = QStringLiteral("刚开放，按住「能量机关」1 秒");
    cards[2].category = QStringLiteral("阵地");
    cards[2].title = QStringLiteral("守住蓝方前哨区");
    cards[2].reason = QStringLiteral("临时控制，5 秒没人激活就丢");
    return cards;
}

RosterEntry sample_self_entry(std::uint32_t robot) {
    RosterEntry entry;
    entry.id = RobotId{robot};
    entry.is_self = true;
    entry.live = LiveState::Alive;
    entry.hp = 135;
    entry.max_hp = 200;
    entry.level = 2;
    entry.ammo = 145;
    entry.bullet_speed = 23.5;
    entry.economy = 800;
    entry.heat = 120;
    entry.heat_limit = 200;
    entry.buffer_energy = 52;
    entry.buffer_limit = 60;
    entry.modules_online = 6;
    entry.modules_total = 6;
    entry.out_of_combat = true;
    entry.can_remote_heal = false;
    entry.can_remote_ammo = false;
    entry.pending_respawn = false;
    return entry;
}

}  // namespace

QPixmap Dashboard::paneImage(PaneId pane, QSize size) {
    const qreal dpr = devicePixelRatioF();
    // 赛前没有数据、画出来是空的几块,用示例内容画(组件库是看「长什么样」的)。
    const auto sample = [&](const QString& title, QWidget* content) {
        GlassCard card(title);
        content->setParent(&card);
        card.body()->addWidget(content, 1);
        card.resize(size);
        card.layout()->activate();
        return over_backdrop(&card, size, dpr);
    };
    switch (pane) {
    case PaneId::NextStep: {
        auto* pane_widget = new NextStepPane;
        pane_widget->setCards(sample_next_cards());
        return sample(QStringLiteral("下一步"), pane_widget);
    }
    case PaneId::Events: {
        auto* feed = new EventFeedPane;
        feed->setRows({{QStringLiteral("03:56"), QStringLiteral("敌方4号步兵残血 46/200"), EventBanner::Neutral},
                       {QStringLiteral("04:00"), QStringLiteral("大能量机关可以激活了"), EventBanner::Positive},
                       {QStringLiteral("04:09"), QStringLiteral("蓝方4号步兵 击毁 红方4号步兵"), EventBanner::Blue},
                       {QStringLiteral("04:33"), QStringLiteral("我方基地遭到攻击"), EventBanner::Red},
                       {QStringLiteral("05:12"), QStringLiteral("红方3号步兵 击毁 蓝方4号步兵"), EventBanner::Red},
                       {QStringLiteral("05:30"), QStringLiteral("小能量机关可以激活了"), EventBanner::Positive}});
        return sample(QStringLiteral("战场事件"), feed);
    }
    case PaneId::Timeline: {
        auto* timeline = new TimelinePane;
        timeline->setClock(96, 324);
        return sample(QStringLiteral("接下来的时间点"), timeline);
    }
    case PaneId::SelfStatus:
        if (!self_status_->hasEntry()) {
            auto* status = new SelfStatusPane;
            status->setEntry(sample_self_entry(self_robot_id_));
            return sample(QStringLiteral("本机状态"), status);
        }
        break;
    case PaneId::Video: {
        // 图传单独一块时的样子:卡片里放现在这一帧。
        GlassCard card(QStringLiteral("图传"));
        card.resize(size);
        QPixmap out = over_backdrop(&card, size, dpr);
        const QRect body(8, GlassCard::kHeaderHeight + 8, size.width() - 16, size.height() - GlassCard::kHeaderHeight - 16);
        // 直接画现在这一帧(小窗可能正藏着,截它会截到旧的大小)。按铺满裁边,和主界面的小窗一样。
        const QImage& frame = info_video_pane_->currentFrame();
        {
            QPainter painter(&out);
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            painter.setClipPath(theme::chamferPath(QRectF(body), 6));
            painter.fillRect(body, theme::kVoid);
            if (!frame.isNull()) {
                const QSizeF cover = QSizeF(frame.size()).scaled(QSizeF(body.size()), Qt::KeepAspectRatioByExpanding);
                const QRectF target(body.left() + (body.width() - cover.width()) / 2,
                                    body.top() + (body.height() - cover.height()) / 2, cover.width(), cover.height());
                painter.drawImage(target, frame);
            } else {
                painter.setFont(theme::labelPx(14));
                painter.setPen(theme::kTextMuted);
                painter.drawText(body, Qt::AlignCenter, QStringLiteral("还没有画面"));
            }
        }
        return out;
    }
    default: break;
    }
    const auto found = pane_cards_.find(pane);
    if (found == pane_cards_.end()) return {};
    QWidget* card = found->second;
    // QML 面板(或者里面有 QML 的)藏起来以后画不出来,用最后一次在主画面上截到的样子。
    if (qobject_cast<QQuickWidget*>(card) || card->findChild<QQuickWidget*>()) {
        if (const auto shot = pane_shots_.find(pane); shot != pane_shots_.end()) return shot->second;
        return {};
    }
    // 别的面板没放进画面时是藏着的,按组件库的大小画一张。
    if (card->isVisible()) {
        if (const auto shot = pane_shots_.find(pane); shot != pane_shots_.end()) return shot->second;
    }
    card->resize(size);
    if (card->layout()) card->layout()->activate();
    return over_backdrop(card, size, dpr);
}

void Dashboard::placeSelfCheck() {
    if (!self_check_) return;
    const int width = std::min(780, this->width() - 40);
    const int height = self_check_->hasHeightForWidth() ? self_check_->heightForWidth(width)
                                                        : self_check_->sizeHint().height();
    const int top = top_bar_ ? top_bar_->geometry().bottom() + 16 : 80;
    self_check_->setGeometry((this->width() - width) / 2, top, width, height);
    self_check_->raise();
}

void Dashboard::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.drawImage(QPoint(0, 0), theme::backdrop(size(), devicePixelRatioF()));
}

void Dashboard::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    placeVideo();
    if (layout_editor_) layout_editor_->setGeometry(rect());
    placeSelfCheck();
    placeBanner();
    placeAlertStrip();
    if (!popup_) return;
    // 宿主铺满整窗是为了压暗层;对话框自身的居中与限宽在 QML 内部,
    // 所以两侧花名册照旧可读 —— 它是本终端的核心增量,不能被弹窗盖掉。
    popup_->setGeometry(rect());
    popup_->raise();
    if (victory_) {
        victory_->setGeometry(rect());
        victory_->raise();
    }
}

void Dashboard::update(const Snapshot& snapshot, const VideoReceiver* video, MonotonicMs now) {
    const bool blind_asserted =
        snapshot.blind.self_base_blinded.value.value_or(false);
    machine_.step(blind_asserted, snapshot.blind.self_base_blinded.freshness, now);
    const ModeDecision decision{mode(), reason()};

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

    refreshPage();

    top_bar_->setSnapshot(snapshot);
    const std::optional<std::uint32_t> mine = self_faction(snapshot);
    const std::uint32_t ally_faction = mine.value_or(1);
    const std::uint32_t enemy_faction = ally_faction == 2 ? 1 : 2;
    ally_roster_->setFactionKnown(mine.has_value(), ally_faction == 2);
    enemy_roster_->setFactionKnown(mine.has_value(), enemy_faction == 2);
    ally_roster_->setEntries(build_roster(snapshot, ally_faction));
    const std::vector<RosterEntry> enemies = build_roster(snapshot, enemy_faction);
    enemy_roster_->setEntries(enemies);
    // 地图上残血敌人的红光和花名册的残血提示是同一条规则:活着、有血量、不超过四分之一。
    std::vector<std::uint32_t> low_health;
    if (mine)
        for (const RosterEntry& enemy : enemies)
            if (enemy.live == LiveState::Alive && enemy.hp && enemy.max_hp && *enemy.max_hp > 0 && *enemy.hp > 0 &&
                *enemy.hp * 4 <= *enemy.max_hp)
                low_health.push_back(enemy.id.value);
    map_->setLowHealth(low_health);
    updateNextStep(snapshot, now);
    analysis_->setMetrics(build_analysis_metrics(snapshot));
    self_status_->setEntry(self_entry(snapshot));
    {
        const auto elapsed = snapshot.game.stage_elapsed_sec;
        const bool playing = snapshot.game.current_stage.value == std::optional<std::uint32_t>(4);
        timeline_->setClock(playing && elapsed.value ? elapsed.value : std::nullopt,
                            playing ? snapshot.game.stage_countdown_sec.value : std::nullopt);
    }
    analysis_->setFortress(fortress_status(snapshot));
    analysis_->setZoneBoard(zone_board(snapshot));

    alert_text_ = alert_strip_text(snapshot, video);
    if (!pre_match_reminder_.isEmpty())
        alert_text_ = alert_text_.isEmpty() ? pre_match_reminder_
                                            : pre_match_reminder_ + QStringLiteral("      ") + alert_text_;
    alert_strip_->setVisible(!alert_text_.isEmpty());
    placeAlertStrip();

    const std::vector<EventFeedRow> event_rows = build_event_rows(snapshot, kEventPanelRows);
    event_->setRows(event_rows);
    event_->setDroppedCount(snapshot.events.droppedCount());
    setPaneHeader(PaneId::Events,
                  event_rows.empty() ? QString()
                                     : QStringLiteral("<span style='font-family:\"%1\"; font-weight:600; font-size:14px; "
                                                      "color:%2'>%3</span>&nbsp;条")
                                           .arg(theme::numericFamily(), theme::kTextBright.name())
                                           .arg(event_rows.size()));
    setPaneHeader(PaneId::Analysis, analysis_->headerText());
    video_stats_->setText(video_panel_text(video, &video_faults_, now));
    map_->setRobots(snapshot.map_robots);
    last_now_ = now;
    sent_marks_.erase(std::remove_if(sent_marks_.begin(), sent_marks_.end(),
                                     [&](const auto& sent) { return now - sent.second >= mark_lifetime_ms_; }),
                      sent_marks_.end());
    map_->setMarks(visible_marks(snapshot.map_marks, sent_marks_, now, mark_lifetime_ms_));
    // 哨兵规划路径(只有指挥手收得到):10 秒没有新路径就不再画,旧意图比没有更误导。
    constexpr MonotonicMs kPathLifetimeMs = 10000;
    map_->setSentryPath(snapshot.sentry_path && now - snapshot.sentry_path->received_at < kPathLifetimeMs
                            ? snapshot.sentry_path->points
                            : std::vector<std::pair<double, double>>{});
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

    const PopupDecision popup = popups_.step(popup_inputs(snapshot), now);
    if (popup.popup != logged_popup_) {
        StructuredLog::write(
            LogLevel::info, QStringLiteral("popup_switch"),
            {QStringLiteral("from=%1").arg(popup_name(logged_popup_)),
             QStringLiteral("to=%1").arg(popup_name(popup.popup)),
             QStringLiteral("match_freshness=%1")
                 .arg(freshness_name(snapshot.game.current_stage.freshness))});
        logged_popup_ = popup.popup;
    }
    // 结算动画一旦接管屏幕就压住普通弹窗:两者都会画在最上层,同时出现会互相遮挡,
    // 而「本局结束」这句话已经被动画本身说得更清楚。
    const GameResult result = resolve_game_result(game_result_inputs(snapshot));
    if (result != shown_result_) {
        StructuredLog::write(LogLevel::info, QStringLiteral("game_result"),
                             {QStringLiteral("result=%1").arg(game_result_name(result))});
        shown_result_ = result;
        if (QQuickItem* overlay = victory_->rootObject()) {
            overlay->setProperty("title", game_result_title(result));
            overlay->setProperty("frameUrls", game_result_frame_urls(result));
            overlay->setProperty("shown", result != GameResult::None);
        }
        victory_->setVisible(result != GameResult::None);
    }
    if (result != GameResult::None) victory_->raise();

    // 界面设置比赛中锁定。只认新鲜可信的阶段:断线时保持原状,比赛中断线不会解锁。
    if (const Field<std::uint32_t>& stage = snapshot.game.current_stage;
        stage.value && stage.quality == Quality::Valid && stage.freshness == Freshness::Fresh) {
        const bool locked = *stage.value == 4;
        if (locked != ui_locked_) {
            ui_locked_ = locked;
            if (ui_profiles_) ui_profiles_->setLocked(locked);
            ui_settings_entry_->setEnabled(!locked);
            ui_settings_entry_->setText(locked ? QStringLiteral("界面设置 · 比赛中锁定") : QStringLiteral("界面设置"));
            if (locked) setUiSettingsVisible(false);
        }
    }

    if (self_check_enabled_) {
        const Field<std::uint32_t>& stage = snapshot.game.current_stage;
        // 0 赛外 1 准备 2 自检:都还来得及修。还没收到过比赛数据也算赛前 —— 那正是
        // 最需要自检页说清「裁判数据没连上」的时候。
        const bool pre_match = !stage.value || *stage.value <= 2;
        if (pre_match && !was_pre_match_) setSelfCheckVisible(true);
        if (!pre_match && was_pre_match_) setSelfCheckVisible(false);
        was_pre_match_ = pre_match;
    }

    const bool replaced = popup.popup == Popup::Eliminated && eliminated_overlay_replaced_;
    const bool popup_shown =
        popup.popup != Popup::None && !replaced && result == GameResult::None && !selfCheckVisible() &&
        !uiSettingsVisible();
    popup_->setVisible(popup_shown);
    if (QQuickItem* overlay = popup_->rootObject()) {
        overlay->setProperty("kind", popup_kind(popup.popup));
        overlay->setProperty("title", popup_title(popup.popup));
        overlay->setProperty("detail", popup_detail(popup.popup));
        // 倒计时只在备战阶段有意义,其余状态传 -1 让 QML 整块隐藏而不是显示 00:00。
        overlay->setProperty("countdownSec",
                             popup.popup == Popup::PreMatch && popup.countdown_sec
                                 ? *popup.countdown_sec
                                 : -1);
        overlay->setProperty("shown", popup_shown);
    }
    if (popup_shown) popup_->raise();

    const auto banner = banners_.current(now);
    alert_banner_->setVisible(banner.has_value() && !uiSettingsVisible());
    if (banner) {
        const GlowBanner::Tone tone = banner->level == AlertLevel::Critical  ? GlowBanner::Tone::Critical
                                      : banner->level == AlertLevel::Warning ? GlowBanner::Tone::Warning
                                                                            : GlowBanner::Tone::Info;
        const double shown_for = static_cast<double>(now - banners_.shownAt());
        alert_banner_->show(banner->text, tone, 1.0 - shown_for / kBannerShowMs);
        placeBanner();
        alert_banner_->raise();
    }
    map_->setBaseAlert(now < base_flash_until_, our_base_left_, (now / 300) % 2 == 0);
}

// 告警带(我方 X 号阵亡、图传离线……)摆在比分正上方的正中。夹在状态条两边的牌子中间排会偏:
// 左右两边一样宽才居中,实际左边的席位、链路灯和右边的按钮宽度不一样。
// 两边的牌子挡着时往空的那边让;还放不下就截短,鼠标指着看全文。
void Dashboard::placeAlertStrip() {
    if (alert_strip_->isHidden()) return;
    constexpr int kSpacing = 6;
    constexpr int kGap = 12;
    // 状态条两边的牌子都是 Fixed,宽度就是 sizeHint;用它算,不用等布局排完。
    const auto span = [](std::initializer_list<QWidget*> widgets) {
        int total = 0;
        int shown = 0;
        for (QWidget* widget : widgets) {
            if (widget->isHidden()) continue;
            total += widget->sizeHint().width();
            ++shown;
        }
        return total + std::max(0, shown - 1) * kSpacing;
    };
    const QMargins margins = layout()->contentsMargins();
    const int left = margins.left() + span({mode_banner_, link_lights_, recording_status_, rune_entry_}) + kGap;
    const int right = width() - margins.right() -
                      span({self_check_entry_, keyboard_mouse_entry_, reminder_entry_, ui_settings_entry_}) - kGap;
    const int centre = margins.left() + (width() - margins.left() - margins.right()) / 2;
    const QMargins text_margins = alert_strip_->contentsMargins();
    const int padding = text_margins.left() + text_margins.right() + 4;
    const QFontMetrics metrics(alert_strip_->font());
    const int room = std::max(padding, right - left);
    QString shown = alert_text_;
    int width = metrics.horizontalAdvance(alert_text_) + padding;
    if (width > room) {
        shown = metrics.elidedText(alert_text_, Qt::ElideRight, room - padding);
        width = room;
    }
    const int x = std::clamp(centre - width / 2, left, std::max(left, right - width));
    alert_strip_->setText(shown);
    const QString tip = shown == alert_text_ ? QString() : alert_text_;
    if (alert_strip_->toolTip() != tip) alert_strip_->setToolTip(tip);
    alert_strip_->setGeometry(x, margins.top(), width, kStatusHeight);
}

void Dashboard::placeBanner() {
    if (!alert_banner_ || !alert_banner_->isVisible() || uiSettingsVisible()) return;
    // 信息页盖在地图最上面一条(离地图上沿 16);图传全屏页没有地图,放在顶栏下方居中。
    if (map_->isVisible() && map_->width() > 0) {
        const QPoint at = map_->mapTo(this, QPoint(0, 0));
        const int height = map_->height() >= 240 ? GlowBanner::kHeight : 48;
        alert_banner_->setGeometry(at.x(), at.y() + std::min(16, map_->height() / 20), map_->width(), height);
    } else {
        const int width = std::min(720, this->width() - 80);
        alert_banner_->setGeometry((this->width() - width) / 2, top_bar_->geometry().bottom() + 8, width,
                                   GlowBanner::kHeight);
    }
}

void Dashboard::showAlert(const QString& text, AlertLevel level, MonotonicMs now) {
    banners_.push(text, level, now);
}

void Dashboard::flashBase(MonotonicMs now) { base_flash_until_ = now + 3000; }

QString Dashboard::alertBannerText() const {
    return alert_banner_->isVisible() ? alert_banner_->text() : QString();
}

void Dashboard::showVideoPage() {
    if (machine_.reason() != ModeReason::ForcedByCli) video_expanded_ = true;
    refreshPage();
}

void Dashboard::showInfoPage() {
    video_expanded_ = false;
    refreshPage();
}

UiMode Dashboard::mode() const {
    if (machine_.reason() == ModeReason::ForcedByCli) return machine_.mode();
    return video_expanded_ ? UiMode::Video : machine_.mode();
}

ModeReason Dashboard::reason() const {
    if (video_expanded_ && machine_.mode() == UiMode::Info &&
        machine_.reason() != ModeReason::ForcedByCli) return ModeReason::ManualVideo;
    return machine_.reason();
}

void Dashboard::forceMode(std::optional<UiMode> forced) {
    machine_.forceMode(forced);
    if (forced) video_expanded_ = false;
    refreshPage();
}

void Dashboard::refreshPage() {
    stack_->setCurrentWidget(mode() == UiMode::Video ? video_page_ : info_page_);
    // 状态条最左边固定显示席位;模式和原因照写日志、照进 layoutDump,悬停可看。
    // 键鼠控制中时跟在席位后面:它在信息页和图传全屏页都看得见,而光标此时是藏起来的。
    mode_banner_->setText(keyboard_mouse_status_.isEmpty()
                              ? seat_label_
                              : seat_label_ + QStringLiteral(" · ") + keyboard_mouse_status_);
    mode_banner_->setToolTip(QStringLiteral("界面 %1 · 原因 %2")
                                 .arg(mode_name(mode()))
                                 .arg(reason_name(reason())));
}

void Dashboard::setCommandSender(CommandSender sender) { command_sender_ = std::move(sender); }

void Dashboard::setEliminatedOverlayReplaced(bool replaced) { eliminated_overlay_replaced_ = replaced; }

void Dashboard::setKeyboardMouseToggle(std::function<void()> toggle) {
    keyboard_mouse_entry_->setVisible(static_cast<bool>(toggle));
    QObject::disconnect(keyboard_mouse_entry_, &QPushButton::clicked, this, nullptr);
    if (toggle)
        QObject::connect(keyboard_mouse_entry_, &QPushButton::clicked, this,
                         [toggle = std::move(toggle)] { toggle(); });
}

void Dashboard::setKeyboardMouseStatus(const QString& text) {
    if (keyboard_mouse_status_ == text) return;
    keyboard_mouse_status_ = text;
    keyboard_mouse_entry_->setText(text.isEmpty() ? QStringLiteral("键鼠控制 F9")
                                                  : QStringLiteral("键鼠控制 · 开"));
    keyboard_mouse_entry_->setDot(text.isEmpty() ? QColor(Qt::transparent) : theme::kCyan);
    keyboard_mouse_entry_->setTextColour(text.isEmpty() ? QColor(Qt::transparent) : theme::kAccentText);
    refreshPage();
}

void Dashboard::setLinkUp(bool up) {
    link_up_ = up;
    if (mark_toolbar_) mark_toolbar_->setLinkUp(up);
}

void Dashboard::sendMark(double x_m, double y_m, std::optional<std::uint32_t> enemy_id) {
    if (!mark_toolbar_) return;
    MarkDraft draft = mark_toolbar_->draft(own_faction_);
    draft.x_m = x_m;
    draft.y_m = y_m;
    if (enemy_id) {
        draft.target = MarkTarget::EnemyRobot;
        draft.enemy_id = *enemy_id;
    }
    const MarkEncoding encoded = encode_map_click(draft, default_field_extent());
    if (encoded.rejection != MarkRejection::None) {
        mark_toolbar_->setStatus(mark_rejection_text(encoded.rejection), true);
        return;
    }
    // 断线时直接丢:等重连后再补发的是过时的意图。
    if (!link_up_ || !command_sender_) {
        mark_toolbar_->setStatus(QStringLiteral("断线，标记不会发送"), true);
        return;
    }
    if (!mark_gate_.allow(last_now_)) {
        mark_toolbar_->setStatus(mark_rejection_text(MarkRejection::TooSoon), true);
        return;
    }
    if (!command_sender_("MapClickCmd", encoded.bytes)) {
        mark_toolbar_->setStatus(QStringLiteral("发送失败"), true);
        return;
    }
    sent_marks_.emplace_back(draft, last_now_);
    StructuredLog::write(LogLevel::info, QStringLiteral("map_mark"),
                         {QStringLiteral("kind=%1").arg(static_cast<int>(draft.kind)),
                          QStringLiteral("target=%1").arg(enemy_id ? QString::number(*enemy_id)
                                                                   : QStringLiteral("map")),
                          QStringLiteral("x=%1").arg(x_m, 0, 'f', 2),
                          QStringLiteral("y=%1").arg(y_m, 0, 'f', 2)});
    mark_toolbar_->setStatus(
        enemy_id ? QStringLiteral("已标记敌方 %1 号").arg(display_robot_number(*enemy_id))
                 : QStringLiteral("已标记 (%1, %2) m").arg(x_m, 0, 'f', 1).arg(y_m, 0, 'f', 1),
        false);
}

void Dashboard::placePane(PaneId pane, QWidget* widget) {
    auto* card = qobject_cast<GlassCard*>(pane_cards_.at(pane));
    if (!card) return;
    // 指令排在最上面。卡片里已经装着图传小窗时,小窗在它下面吃掉剩下的高度;否则面板自己铺满。
    const bool shares = info_video_pane_->parentWidget() == card;
    card->body()->insertWidget(0, widget, shares ? 0 : 1);
}

void Dashboard::setPaneHeader(PaneId pane, const QString& rich_text) {
    if (auto* card = qobject_cast<GlassCard*>(pane_cards_.at(pane))) card->setHeaderRight(rich_text);
}

QWidget* Dashboard::roleCard(PaneId pane) const {
    const std::vector<PaneId> panes = available_panes(role_);
    if (std::find(panes.begin(), panes.end(), pane) == panes.end()) return nullptr;
    const auto found = pane_cards_.find(pane);
    return found == pane_cards_.end() ? nullptr : found->second;
}

QWidget* Dashboard::paneCard(PaneId pane) const {
    const auto found = pane_cards_.find(pane);
    if (found == pane_cards_.end() || !found->second->isVisibleTo(info_page_)) return nullptr;
    return found->second;
}

void Dashboard::setRecordingStatus(const QString& text) {
    if (recording_status_->property("plainText").toString() == text) return;
    recording_status_->setProperty("plainText", text);
    recording_status_->setVisible(!text.isEmpty());
    // 「● REC 03:04 · 46.2 MB」:红点 + 红字的 REC 时长,后面的大小用暗一档的字。
    // 回放和回放失败的提示照原样显示,回放用点缀色。
    const bool recording = text.startsWith(QStringLiteral("● REC"));
    const bool failed = text.startsWith(QStringLiteral("回放失败"));
    QString html;
    if (recording) {
        QString body = text.mid(2);
        QString tail;
        const int cut = body.indexOf(QStringLiteral(" · "));
        if (cut > 0) {
            tail = body.mid(cut);
            body = body.left(cut);
        }
        html = QStringLiteral("<span style='color:#ff5c5c; font-size:10px'>●</span>&nbsp;"
                              "<span style='font-family:\"%1\"; font-weight:600; font-size:14px; color:%2'>%3</span>"
                              "<span style='color:%4'>%5</span>")
                   .arg(theme::numericFamily(), theme::kDangerText.name(), body.toHtmlEscaped(),
                        theme::kTextSecondary.name(), tail.toHtmlEscaped());
    } else {
        html = QStringLiteral("<span style='color:%1'>%2</span>")
                   .arg((failed ? theme::kDangerText : theme::kAccentText).name(), text.toHtmlEscaped());
    }
    recording_status_->setText(html);
}

QString link_lights_html(const LinkReport& report) {
    const auto light = [](const QString& name, const LinkStatus& status) {
        QString color = QStringLiteral("#3ecf8e");
        if (status.level == LinkLevel::Warn) color = QStringLiteral("#e0a43a");
        if (status.level == LinkLevel::Down) color = QStringLiteral("#ff6b6b");
        if (status.level == LinkLevel::Off) color = QStringLiteral("#5b6474");
        const QString name_color =
            status.level == LinkLevel::Off ? theme::kTextSecondary.name() : theme::kTextPrimary.name();
        QString html = QStringLiteral("<span style='color:%1; font-size:10px'>●</span>"
                                      "<span style='color:%2'>&nbsp;%3</span>")
                           .arg(color, name_color, name);
        if (!status.summary.isEmpty())
            html += QStringLiteral("<span style='color:%1'>&nbsp;%2</span>")
                        .arg(color, status.summary.toHtmlEscaped());
        return html;
    };
    return QStringList{light(QStringLiteral("裁判"), report.referee),
                       light(QStringLiteral("车端"), report.car),
                       light(QStringLiteral("图传"), report.video),
                       light(QStringLiteral("控制器"), report.controller)}
        .join(QStringLiteral("&nbsp;&nbsp;"));
}

void Dashboard::setLinkReport(const LinkReport& report) {
    const QString html = link_lights_html(report);
    if (link_lights_->text() != html) link_lights_->setText(html);
    const auto line = [](const QString& name, const LinkStatus& status) {
        QString text = name + QStringLiteral("：") + status.detail;
        if (!status.hint.isEmpty()) text += QStringLiteral(" → ") + status.hint;
        return text;
    };
    link_lights_->setToolTip(QStringList{line(QStringLiteral("裁判数据"), report.referee),
                                         line(QStringLiteral("车端数据"), report.car),
                                         line(QStringLiteral("图传"), report.video),
                                         line(QStringLiteral("控制器"), report.controller)}
                                 .join(QLatin1Char('\n')));
    link_lights_->setVisible(true);
}

void Dashboard::setSelfCheck(const SelfCheck& check) {
    self_check_enabled_ = true;
    self_check_->setCheck(check);
    const QString text = check.failures == 0
                             ? QStringLiteral("自检 · 正常")
                             : QStringLiteral("自检 · %1 项异常").arg(check.failures);
    if (self_check_entry_->text() != text) self_check_entry_->setText(text);
    self_check_entry_->setDot(check.failures == 0 ? theme::kOk : theme::kDanger);
    self_check_entry_->setTextColour(check.failures == 0 ? QColor(Qt::transparent) : theme::kDangerText);
    self_check_entry_->setVisible(true);
    if (self_check_->isVisible()) placeSelfCheck();
}

bool Dashboard::selfCheckVisible() const { return self_check_ && self_check_->isVisible(); }

void Dashboard::setSelfCheckVisible(bool visible) {
    if (!self_check_ || self_check_->isVisible() == visible) return;
    self_check_->setVisible(visible);
    if (!visible) return;
    setUiSettingsVisible(false);
    placeSelfCheck();
    // 不等下一个 tick:弹窗多压在自检页上 250ms 也是在挡字。
    if (popup_) popup_->setVisible(false);
}

void Dashboard::showReplayControls(bool visible) {
    replay_bar_->setVisible(visible);
}

void Dashboard::setReplayProgress(qint64 position_ms, qint64 duration_ms) {
    const int maximum = static_cast<int>(std::max<qint64>(duration_ms, 0));
    // 程序化更新不能反过来触发 seek:那会把一次进度刷新变成一次重开录像。
    const QSignalBlocker blocker(replay_slider_);
    if (replay_slider_->maximum() != maximum) replay_slider_->setRange(0, maximum);
    if (!replay_slider_->isSliderDown())
        replay_slider_->setValue(static_cast<int>(std::clamp<qint64>(position_ms, 0, maximum)));

    const auto clock = [](qint64 milliseconds) {
        const qint64 seconds = std::max<qint64>(0, milliseconds) / 1000;
        return QStringLiteral("%1:%2")
            .arg(seconds / 60, 2, 10, QLatin1Char('0'))
            .arg(seconds % 60, 2, 10, QLatin1Char('0'));
    };
    replay_time_->setText(
        QStringLiteral("%1 / %2").arg(clock(position_ms), clock(duration_ms)));
}

void Dashboard::setReplayPaused(bool paused) {
    replay_play_->setText(paused ? QStringLiteral("▶") : QStringLiteral("⏸"));
    replay_play_->setToolTip(paused ? QStringLiteral("继续播放") : QStringLiteral("暂停"));
}

void Dashboard::setReplaySeekHandler(std::function<void(qint64)> handler) {
    replay_seek_ = std::move(handler);
}

void Dashboard::setReplayPauseHandler(std::function<void()> handler) {
    replay_pause_ = std::move(handler);
}

void Dashboard::setReplaySpeedHandler(std::function<void(double)> handler) {
    replay_speed_handler_ = std::move(handler);
}

void Dashboard::attachReminders(ReminderController* controller) {
    reminders_ = controller;
    reminder_entry_->setEnabled(controller != nullptr);
    if (!controller) return;

    QObject::connect(reminder_entry_, &QPushButton::clicked, this, [this] {
        // 复用同一个对话框实例。每次新建会让「取消」和「再次打开」之间丢掉控制器
        // 连接,也会在反复开关时堆积隐藏窗口。
        if (!reminder_dialog_) reminder_dialog_ = new ReminderDialog(reminders_, this);
        reminder_dialog_->loadDraft(reminders_->config());
        reminder_dialog_->show();
        reminder_dialog_->raise();
        reminder_dialog_->activateWindow();
    });

    const auto sync = [this] {
        const auto state = reminders_->uiState();
        reminder_entry_->setText(state.master_enabled
                                     ? QStringLiteral("战术提醒 · %1 条").arg(state.enabled_count)
                                     : QStringLiteral("战术提醒 · 关"));
    };
    QObject::connect(controller, &ReminderController::stateChanged, this, sync);
    sync();
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
          << pane_entry(QStringLiteral("event_panel"), event_)
          << pane_entry(QStringLiteral("mode_banner"), mode_banner_);
    // 布局档案里的每一格,按 PaneId 的固定顺序。
    for (const auto& [pane, card] : pane_cards_)
        panes << pane_entry(QStringLiteral("card_") + pane_name(pane), card);

    // The window box is included because the "video fills the screen" criterion is a
    // ratio against it; without it that check would need a hard-coded size.
    return QStringLiteral("{\n"
                          "  \"role\": \"%7\",\n"
                          "  \"mode\": \"%1\",\n"
                          "  \"reason\": \"%2\",\n"
                          "  \"stacked_index\": %3,\n"
                          "  \"window\": {\"width\": %4, \"height\": %5},\n"
                          "  \"panes\": [\n%6\n  ]\n"
                          "}\n")
        .arg(mode_name(mode()))
        .arg(reason_name(reason()))
        .arg(stack_->currentIndex())
        .arg(width())
        .arg(height())
        .arg(panes.join(QStringLiteral(",\n")))
        .arg(role_name(role_))
        .toStdString();
}

}
