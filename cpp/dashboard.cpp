#include "dashboard.h"

#include "logging.h"
#include "presentation.h"
#include "tactical_reminder_dialog.h"
#include "theme.h"

#include <algorithm>

#include <QColor>
#include <QDateTime>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QPainter>
#include <QPushButton>
#include <QQuickItem>
#include <QRect>
#include <QResizeEvent>
#include <QUrl>
#include <QPair>
#include <QScrollArea>
#include <QSizePolicy>
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

// Formats the timestamp carried in the feed's payload, not a local reading —— 故
// 不受 assert_monotonic_clock.cmake 对 MonotonicMs 来源的限制。
QString event_time_text(std::uint64_t timestamp_ms) {
    if (timestamp_ms == 0) return QStringLiteral("--");
    return QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(timestamp_ms))
        .toString(QStringLiteral("HH:mm:ss"));
}

}

QString event_panel_text(const Snapshot& snapshot) {
    const auto& event = snapshot.event;
    if (!event.text.value) return QStringLiteral("暂无赛事事件");
    QString line = QStringLiteral("[%1] %2")
                       .arg(event.level.value ? level_name(*event.level.value) : QStringLiteral("--"))
                       .arg(*event.text.value);
    return line + mark(event.text);
}

QColor event_level_color(std::uint32_t level) { return theme::eventColor(level); }

QString alert_strip_text(const Snapshot& snapshot, const VideoReceiver* video) {
    QStringList alerts;

    if (snapshot.blind.self_base_blinded.value.value_or(false)) {
        const auto remaining = snapshot.blind.blind_remaining_ms.value;
        alerts << (remaining && *remaining > 0
                       ? QStringLiteral("基地致盲中 · 预计剩余 %1s")
                             .arg((*remaining + 999) / 1000)
                       : QStringLiteral("基地致盲中"));
    }

    if (snapshot.blind.self_base_blinded.freshness == Freshness::Stale)
        alerts << QStringLiteral("致盲状态数据过期");
    if (snapshot.game.stage_countdown_sec.freshness == Freshness::Stale)
        alerts << QStringLiteral("比赛状态数据过期");
    if (video && !video->online()) alerts << QStringLiteral("图传离线");

    for (const RobotHealth& robot : snapshot.robot_health) {
        if (robot.faction != 1) continue;
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

    for (const MapRobot& robot : snapshot.map_robots) {
        if (!robot.is_self) continue;
        const auto found = snapshot.robots.find(robot.id);
        if (found == snapshot.robots.end()) break;
        inputs.self_hp = found->second.dynamic.current_hp.value;
        break;
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
    for (const MapRobot& robot : snapshot.map_robots) {
        if (!robot.is_self) continue;
        const auto found = snapshot.robots.find(robot.id);
        if (found == snapshot.robots.end()) break;
        return QStringLiteral("血量 %1")
            .arg(ratio(found->second.dynamic.current_hp, found->second.dynamic.max_hp));
    }
    return QStringLiteral("血量 --");
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

void VideoPane::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor(12, 12, 14));
    if (!image_.isNull()) {
        // Cover 用 KeepAspectRatioByExpanding 铺满,再靠 clip 裁掉溢出画面的部分,
        // 溢出量左右/上下均分,所以画面中心仍是格子中心。
        const Qt::AspectRatioMode mode = scaling_ == Scaling::Cover
                                             ? Qt::KeepAspectRatioByExpanding
                                             : Qt::KeepAspectRatio;
        const QImage scaled = image_.scaled(size(), mode, Qt::SmoothTransformation);
        const QPoint origin((width() - scaled.width()) / 2, (height() - scaled.height()) / 2);
        painter.setClipRect(rect());
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
    setObjectName(QStringLiteral("dashboardRoot"));
    setStyleSheet(theme::styleSheet());

    auto* root = new QVBoxLayout(this);
    // 根层边距/间距纯装饰却吃掉约 30px 垂直预算,而 720p 下内容区正好差这一截。
    root->setContentsMargins(6, 4, 6, 4);
    root->setSpacing(4);
    banner_ = new QLabel(QStringLiteral("只读模拟 · READ-ONLY SIMULATION SAFE · 无控制下发通道"), this);
    banner_->setStyleSheet(QStringLiteral(
        "background:#0d2818; color:#40d68a; padding:5px; font-weight:bold;"
        " border:1px solid #1b5236; border-radius:3px;"));
    banner_->setObjectName(QStringLiteral("readOnlyBanner"));

    // Sits alongside the read-only banner and never replaces it: the read-only
    // guarantee has to stay on screen in every mode.
    mode_banner_ = new QLabel(this);
    mode_banner_->setStyleSheet(QStringLiteral(
        "background:#101a26; color:#40d0e8; padding:4px; font-weight:bold;"
        " border:1px solid #263042; border-radius:3px;"));
    mode_banner_->setObjectName(QStringLiteral("modeBanner"));

    // 常态隐藏。一条永远在屏幕上的告警带等于没有告警带 —— 操作手会在三十秒内
    // 学会无视它,而它要传达的恰恰是「现在必须立刻看一眼」。
    alert_strip_ = new QLabel(this);
    alert_strip_->setObjectName(QStringLiteral("alertStrip"));
    alert_strip_->setStyleSheet(QStringLiteral(
        "background:#3a0d12; color:#ff9aa2; padding:1px 8px; font-weight:bold;"
        " border:1px solid #f85c5c; border-radius:3px;"));
    alert_strip_->setAlignment(Qt::AlignCenter);
    alert_strip_->setFont(theme::labelFont(11, true));
    alert_strip_->setVisible(false);

    // 三者高度锁死在同一个值:告警带天生比另外两条高,不锁的话它一出现就顶高整行,
    // 在 720p 恰好把窗口挤过屏幕高度。删掉 setFixedHeight 会静默复现该 bug。
    mode_banner_->setText(QStringLiteral("模式"));
    alert_strip_->setText(QStringLiteral("告警"));
    // max 而不是 min:三条锁同一高度是为了让告警带出现时不顶高整行,而 min 会把内容
    // 最长的只读横幅压到比它 sizeHint 更矮,中英混排的字直接垂直重叠。max 同样锁住
    // 行高,且没有任何一条被压扁。
    const int banner_row_height = std::max({banner_->sizeHint().height(),
                                            mode_banner_->sizeHint().height(),
                                            alert_strip_->sizeHint().height()});
    banner_->setFixedHeight(banner_row_height);
    mode_banner_->setFixedHeight(banner_row_height);
    alert_strip_->setFixedHeight(banner_row_height);
    alert_strip_->clear();

    // 与模式横幅同一行而不是自成一行:独占一行时它出现/消失会把整个页面上下顶动,
    // 而它恰好在最需要稳定视线的时刻出现。同行还省下 720p 下不够用的垂直预算。
    reminder_entry_ = new QPushButton(QStringLiteral("战术提醒"), this);
    reminder_entry_->setObjectName(QStringLiteral("reminderEntry"));
    reminder_entry_->setFixedHeight(banner_row_height);
    // 水平锁 Fixed。QPushButton 默认是 Minimum,会跟两条横幅一起均分告警带隐藏时
    // 空出来的横向余量 —— 实测 1280 宽下按钮被拉到 419px(sizeHint 只有 80px),
    // 一个半屏宽的按钮。Fixed 让它只占 sizeHint,余量归横幅,按钮始终贴住右端。
    reminder_entry_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    // 未挂控制器时不可用。可点但什么都不弹会让操作手以为功能坏了。
    reminder_entry_->setEnabled(false);

    auto* banner_row = new QHBoxLayout();
    banner_row->setContentsMargins(0, 0, 0, 0);
    banner_row->setSpacing(6);
    banner_row->addWidget(banner_, 0);
    banner_row->addWidget(mode_banner_, 0);
    banner_row->addWidget(alert_strip_, 1);
    // 告警带拿 stretch,入口按钮跟在它后面且不拉伸:按钮固定贴住右端,而告警带
    // 的出现/消失不会把按钮左右推动。
    banner_row->addWidget(reminder_entry_, 0);
    root->addLayout(banner_row);

    top_bar_ = new TopBar(this);
    top_bar_->setObjectName(QStringLiteral("topBar"));
    root->addWidget(top_bar_);

    stack_ = new QStackedLayout();
    info_page_ = new QWidget(this);
    video_page_ = new QWidget(this);

    auto* grid = new QGridLayout(info_page_);
    // 顶栏和面板区之间原本叠了三层留白(顶栏内余 4 + root spacing 4 + grid 默认上边距 9),
    // 合起来是一道 17px 黑缝。上边距收到 2 把这道缝减半,左右下仍用默认值。
    grid->setContentsMargins(9, 2, 9, 9);

    // Roster columns flank the map. Built as cards so they read as the same kind
    // of surface as the telemetry panels.
    // 标题、卡面、边框都由 Panel.qml 画,所以这里不再套 QFrame+QLabel —— 套了会出现
    // 双重标题和两层边框。QML 面板直接进 grid。
    ally_roster_ = new RosterPanel(true, info_page_);
    ally_roster_->setObjectName(QStringLiteral("allyRoster"));
    enemy_roster_ = new RosterPanel(false, info_page_);
    enemy_roster_->setObjectName(QStringLiteral("enemyRoster"));
    QWidget* ally_box = ally_roster_;
    QWidget* enemy_box = enemy_roster_;

    auto* map_box = new QFrame(info_page_);
    map_box->setProperty("card", true);
    auto* map_column = new QVBoxLayout(map_box);
    auto* map_heading = new QLabel(QStringLiteral("战术地图"), map_box);
    map_heading->setProperty("heading", true);
    map_column->addWidget(map_heading);
    map_ = new MapPane(map_box);
    map_->setObjectName(QStringLiteral("mapPane"));
    // stretch 归 addStretch 而不是地图:给地图拉伸权会把差额画成上下黑边。
    map_column->addWidget(map_, 0);
    map_column->addStretch(1);

    // 三列上区:我方花名册 / 地图 / 敌方花名册。花名册是本终端相对官方选手端的
    // 核心增量 —— 官方端看不到对方血量,这两列是唯一来源,所以给固定宽度而不是
    // 让地图把它们挤成窄条。
    grid->addWidget(ally_box, 0, 0);
    grid->addWidget(map_box, 0, 1);
    grid->addWidget(enemy_box, 0, 2);

    auto* video_box = new QFrame(info_page_);
    video_box->setProperty("card", true);
    auto* video_column = new QVBoxLayout(video_box);
    // 标题 19 + 画面 90 + 统计 35 几乎填满这一格,默认边距/间距会让整列高出约 8px,
    // 溢出部分就是压在画面上的统计文字。恢复默认值会静默复现该重叠 bug。
    video_column->setContentsMargins(9, 4, 9, 4);
    video_column->setSpacing(2);
    auto* video_heading = new QLabel(QStringLiteral("图传"), video_box);
    video_heading->setProperty("heading", true);
    video_column->addWidget(video_heading);
    info_video_pane_ = new VideoPane(video_box);
    info_video_pane_->setObjectName(QStringLiteral("infoVideoPane"));
    // 不设宽高上限:paintEvent 已按 KeepAspectRatio 居中缩放,让它铺满整格即可。
    // 320x180 放大后画质偏软,但留一大片黑边更难看。曾经用 setFixedSize 把下限也
    // 钉死,导致窗口变矮时画面压到统计文字上,所以这里只放开上限、保留下限。
    info_video_pane_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    info_video_pane_->setScaling(VideoPane::Scaling::Cover);
    // Half the cap, so a short window shrinks the picture instead of overlapping the
    // stats text with it. paintEvent keeps the aspect ratio at whatever height it gets.
    info_video_pane_->setMinimumSize(160, 90);

    // 状态在画面下方而不是右侧:异常时那行故障计数很长,并排放不下就会压到画面上。
    // 常态是空串,WrappedLabel 负责把换行后的真实高度报给布局,所以空行不占高度。
    video_stats_ = new WrappedLabel(QString(), video_box);
    video_stats_->setObjectName(QStringLiteral("videoStats"));
    QFont mono(QStringLiteral("Menlo"));
    mono.setStyleHint(QFont::Monospace);
    mono.setPointSize(10);
    video_stats_->setFont(mono);
    video_stats_->setAlignment(Qt::AlignTop | Qt::AlignLeft);

    // 纵向 Minimum 而不是 MinimumExpanding:后者会让这个常态为空串的标签把盈余高度
    // 吃掉,画面被顶到上方,标签下方留出一条黑带。盈余要留给画面。
    QSizePolicy stats_policy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    stats_policy.setHeightForWidth(true);
    video_stats_->setSizePolicy(stats_policy);
    // 画面直接吃掉这一列的盈余高度。原先靠上下两个 addStretch 把定高画面夹在正中,
    // 那是画面不能变大时的折中;现在画面自己会铺满,再加 stretch 只会重新挤出黑边。
    video_column->addWidget(info_video_pane_, 1);
    video_column->addWidget(video_stats_, 0);

    auto* event_box = new QFrame(info_page_);
    event_box->setProperty("card", true);
    auto* event_column = new QVBoxLayout(event_box);
    event_column->setContentsMargins(9, 4, 9, 4);
    event_column->setSpacing(2);
    auto* event_heading = new QLabel(QStringLiteral("战场事件"), event_box);
    event_heading->setProperty("heading", true);
    event_column->addWidget(event_heading);
    event_ = new EventFeedPane(event_box);
    event_->setObjectName(QStringLiteral("eventPanel"));
    // Ignored 纵向策略让面板既不撑高窗口最小高度(720p 断言守的就是这条),也不给
    // 自己设上限 —— 高度完全由下区那一格分到多少决定,画满为止。封顶会让分到的
    // 高度重新变成卡片内的空白。
    event_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Ignored);
    event_column->addWidget(event_, 1);

    auto* analysis_box = new QFrame(info_page_);
    analysis_box->setProperty("card", true);
    auto* analysis_column = new QVBoxLayout(analysis_box);
    auto* analysis_heading = new QLabel(QStringLiteral("战场数据分析"), analysis_box);
    analysis_heading->setProperty("heading", true);
    analysis_column->addWidget(analysis_heading);
    analysis_ = new BattleAnalysisPane(analysis_box);
    analysis_->setObjectName(QStringLiteral("battleAnalysisPane"));
    analysis_column->addWidget(analysis_, 1);

    // 下区三列与上区列一一对齐:事件在我方一侧、战场分析居中、图传在敌方一侧,
    // 视线不需要横跨整屏找对应关系。
    grid->addWidget(event_box, 1, 0);
    grid->addWidget(analysis_box, 1, 1);
    grid->addWidget(video_box, 1, 2);

    // 花名册按内容定宽,地图吃掉盈余。中列同时承载地图和战场分析,所以它是唯一
    // 有拉伸权的列。
    grid->setColumnStretch(0, 0);
    grid->setColumnStretch(1, 5);
    grid->setColumnStretch(2, 0);
    // 两侧花名册列必须等宽。列宽取该列最宽控件的 sizeHint,左列下方是事件面板、
    // 右列下方是图传,两者宽度提示不同就会让画面左右不对称(事件面板报 460 时左列
    // 明显更宽)。这里钉住列宽本身,而不是给花名册 setFixedWidth —— 后者会让花名册
    // 窄于列宽,差额就是花名册右侧那道黑缝。
    for (int column : {0, 2}) {
        grid->setColumnMinimumWidth(column, kRosterColumnWidth);
    }
    // 上区内容是定高的(花名册满 5 台即到顶、地图按比例缩放后用 addStretch 吸差额),
    // 所以把全部盈余压给上区会在花名册/地图下方画出一道纯黑带 —— 全屏越宽越明显。
    // 两区都给拉伸权,盈余按 3:2 分,下区的事件与战场分析随窗口一起长高。
    grid->setRowStretch(0, 5);
    grid->setRowStretch(1, 3);

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
}

void Dashboard::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
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

    top_bar_->setSnapshot(snapshot);
    const std::optional<std::uint32_t> mine = self_faction(snapshot);
    const std::uint32_t ally_faction = mine.value_or(1);
    const std::uint32_t enemy_faction = ally_faction == 2 ? 1 : 2;
    ally_roster_->setFactionKnown(mine.has_value(), ally_faction == 2);
    enemy_roster_->setFactionKnown(mine.has_value(), enemy_faction == 2);
    ally_roster_->setEntries(build_roster(snapshot, ally_faction));
    enemy_roster_->setEntries(build_roster(snapshot, enemy_faction));
    analysis_->setMetrics(build_analysis_metrics(snapshot));
    analysis_->setFortressHold(fortress_hold(snapshot));

    const QString alerts = alert_strip_text(snapshot, video);
    alert_strip_->setVisible(!alerts.isEmpty());
    if (!alerts.isEmpty()) alert_strip_->setText(alerts);

    event_->setRows(build_event_rows(snapshot, kEventPanelRows));
    event_->setDroppedCount(snapshot.events.droppedCount());
    video_stats_->setText(video_panel_text(video, &video_faults_, now));
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

    const bool popup_shown = popup.popup != Popup::None && result == GameResult::None;
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
