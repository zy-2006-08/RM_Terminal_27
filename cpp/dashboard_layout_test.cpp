#include "dart_pane.h"
#include "dashboard.h"
#include "layout_editor.h"
#include "sentry_pane.h"
#include "layout_ops.h"
#include "reminder_repository.h"
#include "speech_backend.h"
#include "tactical_reminder_controller.h"
#include "tactical_reminder_dialog.h"
#include "ui_profile.h"
#include "ui_profile_store.h"

#include "rm_terminal.pb.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QKeyEvent>
#include <QPushButton>
#include <QQuickItem>
#include <QQuickWidget>
#include <QTemporaryDir>
#include <QThread>
#include <algorithm>
#include <cstdlib>
#include <numeric>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <set>
#include <string>
#include <vector>

using namespace rm_terminal;

namespace {
bool qml_error = false;

void messages(QtMsgType type, const QMessageLogContext&, const QString& message) {
    if ((type == QtWarningMsg || type == QtCriticalMsg) &&
        (message.contains("qrc:/") || message.contains("QQml"))) qml_error = true;
    std::cerr << message.toStdString() << '\n';
}

void check(bool condition, const char* name) {
    if (!condition) throw std::runtime_error(name);
    std::cout << "PASS " << name << '\n';
}

constexpr int kWidth = 1280;
constexpr int kHeight = 720;

// Mirrors main.cpp's setMinimumSize - the smallest window the app can present, so
// the info page must lay out cleanly here, not only at 1280x720. Raised from
// 980x620 with the three-column roster layout; if these drift apart the test
// resizes to a size Qt then clamps, and every geometry assertion below is void.
constexpr int kMinWidth = 1280;
constexpr int kMinHeight = 760;

template <class T>
Field<T> present(T value, Freshness freshness = Freshness::Fresh) {
    Field<T> field;
    field.value = value;
    field.quality = Quality::Valid;
    field.freshness = freshness;
    return field;
}

Snapshot base_snapshot() {
    Snapshot snapshot;
    snapshot.self = RobotId{3};
    snapshot.game.stage_countdown_sec = present<std::int32_t>(154);
    snapshot.game.current_stage = present<std::uint32_t>(4);

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

Snapshot blinded_snapshot() {
    Snapshot snapshot = base_snapshot();
    snapshot.blind.self_base_blinded = present<bool>(true);
    return snapshot;
}

Snapshot clear_snapshot() {
    Snapshot snapshot = base_snapshot();
    snapshot.blind.self_base_blinded = present<bool>(false);
    return snapshot;
}

Snapshot pre_match_snapshot() {
    Snapshot snapshot = clear_snapshot();
    snapshot.game.current_stage = present<std::uint32_t>(1);
    snapshot.game.stage_countdown_sec = present<std::int32_t>(7);
    return snapshot;
}

Snapshot settled_snapshot() {
    Snapshot snapshot = clear_snapshot();
    snapshot.game.current_stage = present<std::uint32_t>(5);
    snapshot.game.winner = present<std::uint32_t>(1);
    return snapshot;
}

void settle(Dashboard& dashboard) {
    dashboard.show();
    QCoreApplication::processEvents();
}

// QWidget::render() 抵不到 QQuickWidget 里的 QML 内容：scene graph 是异步的，
// 单跑一次 processEvents() 就读像素实测约 9/40 抓到空白帧。grabFramebuffer()
// 同步算绘并回传当前帧，既不靠等待也不靠 sleep 赌时序。
QImage grab_rendered(const QWidget* host) {
    if (auto* quick = qobject_cast<QQuickWidget*>(const_cast<QWidget*>(host))) {
        return quick->grabFramebuffer();
    }
    QImage shot(host->size(), QImage::Format_ARGB32);
    shot.fill(Qt::transparent);
    const_cast<QWidget*>(host)->render(&shot);
    return shot;
}

// Modes are reached only by feeding snapshots through the real update() path.
// Exposing a mode setter for the test would both pollute the production surface
// and skip the very state-machine wiring under test.
void drive_to_video(Dashboard& dashboard) {
    dashboard.update(blinded_snapshot(), nullptr, 1000);
    settle(dashboard);
}

void drive_to_info(Dashboard& dashboard) {
    dashboard.update(blinded_snapshot(), nullptr, 1000);
    // Hysteresis needs continuously observed clear samples, so a single late tick
    // would not prove the exit path: feed the window, then cross it.
    dashboard.update(clear_snapshot(), nullptr, 1250);
    dashboard.update(clear_snapshot(), nullptr, 2500);
    dashboard.update(clear_snapshot(), nullptr, 4300);
    dashboard.update(clear_snapshot(), nullptr, 4550);
    settle(dashboard);
}

int area(const QWidget* widget) { return widget->width() * widget->height(); }

const QWidget* find(const Dashboard& dashboard, const char* name) {
    const QWidget* found = dashboard.findChild<QWidget*>(QString::fromLatin1(name));
    if (!found) throw std::runtime_error(std::string("widget not found: ") + name);
    return found;
}

const VideoPane* find_pane(const Dashboard& dashboard, const char* name) {
    const VideoPane* found = dashboard.findChild<VideoPane*>(QString::fromLatin1(name));
    if (!found) throw std::runtime_error(std::string("pane not found: ") + name);
    return found;
}

const QLabel* find_label(const Dashboard& dashboard, const char* name) {
    const QLabel* found = dashboard.findChild<QLabel*>(QString::fromLatin1(name));
    if (!found) throw std::runtime_error(std::string("label not found: ") + name);
    return found;
}

Config test_config() { return Config{}; }

// (1) and (2): info mode is map-dominant, and the thumbnail stays a thumbnail.
// 启动时的方案里没有「装配」:主程序照样能把装配面板放进去,加回画面时按钮都在。
void a_role_pane_placed_while_off_screen_shows_up_later() {
    Config config = test_config();
    config.self_robot_id = 6;
    Dashboard dashboard(config);
    dashboard.resize(1440, 900);
    const UiScheme standard = builtin_scheme(Role::Commander, QStringLiteral("standard")).value();
    UiScheme without = standard;
    without.cells = layout_remove(scheme_layout(Role::Commander, standard).cells, PaneId::Dart);
    dashboard.applyUiScheme(without);
    settle(dashboard);
    check(dashboard.paneCard(PaneId::Dart) == nullptr && dashboard.roleCard(PaneId::Dart) != nullptr,
          "a pane that is off screen still has its card for the seat");
    check(dashboard.roleCard(PaneId::Assembly) == nullptr, "a pane the seat does not have has no card");
    check(dashboard.roleCard(PaneId::SelfCommands) == nullptr, "「本车」 is gone: no seat has it any more");
    auto* dart = new QPushButton(QStringLiteral("开闸"));
    dashboard.placePane(PaneId::Dart, dart);
    dashboard.applyUiScheme(standard);
    settle(dashboard);
    check(dashboard.paneCard(PaneId::Dart) != nullptr && dart->isVisible() && dart->height() > 0,
          "when the pane comes back on screen its buttons are there");

    Config heavy_config = test_config();
    heavy_config.self_robot_id = 1;
    Dashboard heavy(heavy_config);
    check(heavy.roleCard(PaneId::Assembly) == nullptr,
          "重装 has no 装配 pane: assembly lives in the H box and the progress strip");
}

void info_mode_is_map_dominant_with_a_capped_thumbnail() {
    Dashboard dashboard(test_config());
    dashboard.resize(kWidth, kHeight);
    drive_to_info(dashboard);
    check(dashboard.mode() == UiMode::Info, "clear blind data settles into info mode");

    const QWidget* map = find(dashboard, "mapPane");
    const VideoPane* thumbnail = find_pane(dashboard, "infoVideoPane");

    check(map->isVisible(), "the map is visible in info mode");
    check(thumbnail->isVisible(), "the thumbnail is visible in info mode");
    check(area(map) > 4 * area(thumbnail), "the map outweighs the thumbnail by over 4x");
    check(thumbnail->width() <= 320 && thumbnail->height() <= 180,
          "the thumbnail is capped at the decoder's 320x180");
}

// The panels must fit the screen they are given. An earlier build stacked enough
// minimum height in the right-hand column that resize(1280,720) was silently
// overridden to 1280x896, which would clip on a 720p operator display while every
// ratio assertion still passed.
void the_layout_fits_the_requested_screen_size() {
    Dashboard dashboard(test_config());
    dashboard.resize(kWidth, kHeight);

    drive_to_info(dashboard);
    check(dashboard.height() == kHeight && dashboard.width() == kWidth,
          "info mode honours the requested 1280x720 without growing");
    check(dashboard.minimumSizeHint().height() <= kHeight,
          "the info layout's minimum height fits within 720p");

    drive_to_video(dashboard);
    check(dashboard.height() == kHeight && dashboard.width() == kWidth,
          "video mode honours the requested 1280x720 without growing");
}

// Regression: every ratio and visibility assertion passed while the video box was
// visibly broken - the unshrinkable 320x180 thumbnail overflowed its box at the
// minimum size and the stats label was painted on top of the picture.
void the_thumbnail_and_its_stats_never_overlap() {
    Dashboard dashboard(test_config());
    dashboard.resize(kMinWidth, kMinHeight);
    drive_to_info(dashboard);

    check(dashboard.width() == kMinWidth && dashboard.height() == kMinHeight,
          "info mode honours the minimum window size without growing");

    const VideoPane* thumbnail = find_pane(dashboard, "infoVideoPane");
    const QLabel* stats = find_label(dashboard, "videoStats");
    const QWidget* box = thumbnail->parentWidget();

    // update(..., nullptr, ...) leaves the one-line "图传未启用" placeholder. The tallest
    // shape video_panel_text() can now produce is state + one fault line, which is what
    // this box must still survive without painting over the picture.
    const_cast<QLabel*>(stats)->setText(
        QStringLiteral("online\n丢失 12  乱序 3  重复 1  不完整 2  超时 4"));
    settle(dashboard);

    check(box == stats->parentWidget(),
          "thumbnail and stats share the video box, so their geometry is comparable");
    check(thumbnail->isVisible() && stats->isVisible(),
          "both the thumbnail and its stats are visible at the minimum size");

    // Without these, the overlap check below passes vacuously: a zero-height picture
    // intersects nothing, and a clipped stats label shrinks out of the way. Both are
    // worse than the bug, so the useful area is asserted before the non-overlap.
    // 图传单独一块 2 格高,1280 × 760 下再加两行故障,画面还剩 72 像素以上。
    check(thumbnail->height() >= 72 && thumbnail->width() >= 160,
          "the thumbnail keeps a usable picture area instead of collapsing");
    check(stats->height() >= stats->minimumSizeHint().height(),
          "the stats label is tall enough to show every line it holds");

    check(!thumbnail->geometry().intersects(stats->geometry()),
          "the stats text does not overlap the thumbnail picture");
    check(box->rect().contains(thumbnail->geometry()),
          "the thumbnail fits inside its box instead of overflowing it");
    check(box->rect().contains(stats->geometry()),
          "the stats label fits inside its box instead of overflowing it");
}

void clicking_video_panes_switches_the_visible_page() {
    Dashboard dashboard(test_config());
    dashboard.resize(kMinWidth, kMinHeight);
    drive_to_info(dashboard);

    auto* thumbnail = const_cast<VideoPane*>(find_pane(dashboard, "infoVideoPane"));
    auto* full = const_cast<VideoPane*>(find_pane(dashboard, "videoFullPane"));
    const auto click = [&](VideoPane* pane, Qt::MouseButton button = Qt::LeftButton) {
        const QPoint local = pane->rect().center();
        check(dashboard.childAt(pane->mapTo(&dashboard, local)) == pane,
              "native hit testing reaches the video pane");
        QMouseEvent event(QEvent::MouseButtonPress, local,
                          pane->mapTo(pane->window(), local), pane->mapToGlobal(local),
                          button, button, Qt::NoModifier);
        QApplication::sendEvent(pane, &event);
        settle(dashboard);
    };
    const auto capture = [&](const QString& name) {
        const QString directory = qEnvironmentVariable("RM_UI_EVIDENCE_DIR");
        if (directory.isEmpty()) return;
        QImage shot = grab_rendered(&dashboard);
        QPainter painter(&shot);
        for (auto* quick : dashboard.findChildren<QQuickWidget*>()) {
            if (quick->isVisible() && quick->status() == QQuickWidget::Ready)
                painter.drawImage(QRect(quick->mapTo(&dashboard, QPoint()), quick->size()),
                                  quick->grabFramebuffer());
        }
        painter.end();
        check(QDir().mkpath(directory) && shot.save(directory + "/" + name + ".png"),
              "native dashboard PNG saved");
    };
    check(thumbnail->isVisible() && !full->isVisible(),
          "info mode starts with only the thumbnail visible");

    check(thumbnail->cursor().shape() == Qt::PointingHandCursor &&
              !thumbnail->toolTip().isEmpty() && !full->toolTip().isEmpty(),
          "video click affordances are present");
    capture("dashboard-info");
    click(thumbnail, Qt::RightButton);
    check(dashboard.mode() == UiMode::Info, "right click does not expand video");
    click(thumbnail);
    check(full->isVisible() && !thumbnail->isVisible(),
          "clicking the thumbnail opens the full video page");
    check(dashboard.mode() == UiMode::Video, "manual expansion reports the actual visible mode");
    check(dashboard.reason() == ModeReason::ManualVideo &&
              dashboard.layoutDump().find("ManualVideo") != std::string::npos &&
              find_label(dashboard, "modeBanner")->toolTip().contains("ManualVideo"),
          "manual mode reason, dump and banner tooltip agree");
    check(find_label(dashboard, "modeBanner")->text() == QStringLiteral("红方 3 号 · 步兵"),
          "the status bar shows the seat instead of the mode");
    capture("dashboard-expanded");
    dashboard.update(clear_snapshot(), nullptr, 4800);
    settle(dashboard);
    check(full->isVisible(), "manual expansion persists across updates");

    click(full);
    check(thumbnail->isVisible() && !full->isVisible(),
          "clicking the full video returns to the info page");
    dashboard.update(clear_snapshot(), nullptr, 4900);
    check(dashboard.mode() == UiMode::Info, "collapse persists across clear refresh");
    capture("dashboard-returned");
    dashboard.update(blinded_snapshot(), nullptr, 5050);
    settle(dashboard);
    check(full->isVisible(), "return does not pin Info over automatic blind Video");
    click(full);
    check(full->isVisible(), "full click cannot defeat automatic blind Video");
    capture("dashboard-blind");
    dashboard.forceMode(UiMode::Info);
    settle(dashboard);
    click(thumbnail);
    dashboard.update(blinded_snapshot(), nullptr, 5300);
    check(dashboard.mode() == UiMode::Info && dashboard.reason() == ModeReason::ForcedByCli,
          "CLI Info wins over click and blind refresh");
    dashboard.forceMode(UiMode::Video);
    settle(dashboard);
    click(full);
    dashboard.update(clear_snapshot(), nullptr, 5550);
    check(dashboard.mode() == UiMode::Video && dashboard.reason() == ModeReason::ForcedByCli,
          "CLI Video wins over collapse and clear refresh");
    dashboard.forceMode(std::nullopt);
    check(dashboard.mode() == UiMode::Video, "releasing CLI retains blind exit hysteresis");
    dashboard.update(clear_snapshot(), nullptr, 8800);
    check(dashboard.mode() == UiMode::Info, "automatic mode resumes after CLI release");
}

// 图传统计只在异常时浮现,但「浮现」必须真的发生 —— 否则等于把模块一已验证的
// 故障可见性悄悄删掉。没有接收器时不得伪造任何计数。
void video_stats_stay_quiet_until_something_goes_wrong() {
    check(video_panel_text(nullptr) == QStringLiteral("图传未启用"),
          "no receiver reports as disabled rather than as healthy");

    Dashboard dashboard(test_config());
    dashboard.resize(kWidth, kHeight);
    drive_to_info(dashboard);

    const QLabel* stats = find_label(dashboard, "videoStats");
    for (const char* noise : {"丢失", "乱序", "重复", "不完整", "超时"}) {
        check(!stats->text().contains(QString::fromUtf8(noise)),
              (std::string("a healthy feed does not show the ") + noise + " counter").c_str());
    }
}

// 累计计数器曾让开局一次丢包永久占住界面。故障行必须按新增量浮现,并在恢复后消失。
void video_faults_surface_on_change_and_clear_on_recovery() {
    VideoFaultTracker tracker;
    VideoFaultCounts counts;
    counts.missing = 42;

    check(tracker.faultLine(counts, 1000).isEmpty(),
          "a pre-existing total is taken as the baseline, not as a live fault");

    counts.missing = 47;
    const QString surfaced = tracker.faultLine(counts, 2000);
    check(surfaced.contains(QStringLiteral("丢失 5")),
          "a new loss surfaces the delta, not the cumulative total");

    check(tracker.faultLine(counts, 2000 + kVideoFaultHoldMs - 1) == surfaced,
          "the fault line is held long enough to be readable");
    check(tracker.faultLine(counts, 2000 + kVideoFaultHoldMs).isEmpty(),
          "a recovered feed clears the fault line instead of pinning it forever");
}

// (3): video mode hands the picture the screen, and the map goes away.
void video_mode_fills_the_window_and_hides_the_map() {
    Dashboard dashboard(test_config());
    dashboard.resize(kWidth, kHeight);
    drive_to_video(dashboard);
    check(dashboard.mode() == UiMode::Video, "an asserted blind switches to video mode");

    const VideoPane* full = find_pane(dashboard, "videoFullPane");
    const QWidget* map = find(dashboard, "mapPane");

    check(full->isVisible(), "the full pane is visible in video mode");
    check(area(full) * 100 > area(&dashboard) * 60,
          "the full pane covers over 60% of the window");
    check(!map->isVisible(), "the map is not visible in video mode");
}

// (5): during blinding these two are the only match state left to act on.
void video_mode_keeps_the_countdown_and_hp() {
    Dashboard dashboard(test_config());
    dashboard.resize(kWidth, kHeight);
    drive_to_video(dashboard);

    const QLabel* countdown = find_label(dashboard, "videoOverlayCountdown");
    const QLabel* hp = find_label(dashboard, "videoOverlayHp");

    check(countdown->isVisible(), "the countdown stays visible in video mode");
    check(hp->isVisible(), "the HP readout stays visible in video mode");
    check(countdown->text().contains(QStringLiteral("02:34")),
          "the countdown shows the real remaining time");
    check(hp->text().contains(QStringLiteral("412")), "the HP readout shows the real value");
}

// 能量机关事件(3、4)不带是哪一方的;增益推算分出来以后,事件栏写清是我方还是敌方。
void rune_events_name_the_side() {
    Snapshot snapshot = clear_snapshot();
    snapshot.self = RobotId{3};
    EventRecord ours;
    ours.event_id = 4;
    ours.text = "小能量机关已激活";
    ours.received_at = 1000;
    ours.rune_kind = 1;
    EventRecord theirs = ours;
    theirs.received_at = 2000;
    snapshot.events.push(ours);
    snapshot.events.push(theirs);
    const std::map<MonotonicMs, std::uint32_t> sides{{1000, kRed}, {2000, kBlue}};
    const std::vector<EventFeedRow> rows = build_event_rows(snapshot, kEventPanelRows, &sides);
    check(rows.size() == 2 && rows[0].text == QStringLiteral("敌方小能量机关已激活（推算）") &&
              rows[0].banner == EventBanner::Blue,
          "the enemy's rune event is named and painted in the enemy colour");
    check(rows.size() == 2 && rows[1].text == QStringLiteral("我方小能量机关已激活") &&
              rows[1].banner == EventBanner::Positive,
          "our rune event keeps the green banner");
    const std::vector<EventFeedRow> plain = build_event_rows(snapshot, kEventPanelRows);
    check(plain.size() == 2 && plain[0].text == QStringLiteral("小能量机关已激活"),
          "without attribution the text stays as the protocol says");
}

// (6): a critical event must be both present and unmistakably coloured.
void a_critical_event_is_listed_and_coloured() {
    Dashboard dashboard(test_config());
    dashboard.resize(kWidth, kHeight);

    Snapshot snapshot = clear_snapshot();
    snapshot.events.push(EventRecord{1000, 0, "常规事件", 900, 0});
    snapshot.events.push(EventRecord{2000, 3, "红方基地被击破", 950, 1});
    dashboard.update(snapshot, nullptr, 1000);
    settle(dashboard);

    const EventFeedPane* panel =
        dashboard.findChild<const EventFeedPane*>(QStringLiteral("eventPanel"));
    check(panel != nullptr, "the event panel is present");

    const std::vector<EventFeedRow> rows = build_event_rows(snapshot, kEventPanelRows);
    check(rows.size() == 2, "both events reach the feed");
    const bool has_critical =
        std::any_of(rows.begin(), rows.end(), [](const EventFeedRow& row) {
            return row.text == QStringLiteral("红方基地被击破") &&
                   row.banner == EventBanner::Red;
        });
    check(has_critical, "the critical red event carries the red banner");
    const bool has_ordinary =
        std::any_of(rows.begin(), rows.end(), [](const EventFeedRow& row) {
            return row.text == QStringLiteral("常规事件") &&
                   row.banner == EventBanner::Neutral;
        });
    check(has_ordinary, "the factionless event stays neutral");

    QImage shot(panel->size(), QImage::Format_ARGB32);
    shot.fill(Qt::black);
    const_cast<EventFeedPane*>(panel)->render(&shot);
    int alert_pixels = 0;
    for (int y = 0; y < shot.height(); ++y) {
        for (int x = 0; x < shot.width(); ++x) {
            const QRgb p = shot.pixel(x, y);
            if (qRed(p) > qGreen(p) + 40 && qRed(p) > qBlue(p) + 40) ++alert_pixels;
        }
    }
    check(alert_pixels > 0, "the red banner actually renders red pixels");
}

void the_event_time_fits_its_column() {
    const QFont time_font = event_time_font();
    check(QFontMetrics(time_font).horizontalAdvance(QStringLiteral("00:00")) <= kEventTimeWidth,
          "the event time fits its column instead of being cut");
    const auto pixels = [](const QFont& font) {
        return font.pixelSize() > 0 ? static_cast<double>(font.pixelSize()) : font.pointSizeF() * 96.0 / 72.0;
    };
    check(pixels(time_font) < pixels(EventFeedPane().font()),
          "the event time is drawn smaller than the event text");
}

// (7): both panes are handed the SAME frame every tick, including the hidden one.
// A per-pane latestFrame() call could straddle a frame advance, and a visibility
// short-circuit would leave the target page one frame stale at the switch.
void both_panes_receive_one_identical_frame() {
    VideoPane thumbnail;
    VideoPane full;
    thumbnail.resize(320, 180);
    full.resize(960, 540);

    QImage frame(VideoReceiver().frameWidth(), VideoReceiver().frameHeight(),
                 QImage::Format_BGR888);
    for (int y = 0; y < frame.height(); ++y) {
        for (int x = 0; x < frame.width(); ++x) {
            frame.setPixel(x, y, qRgb((x * 7) % 256, (y * 5) % 256, (x + y) % 256));
        }
    }

    apply_frame(&thumbnail, &full, frame);

    check(!thumbnail.currentFrame().isNull(), "the thumbnail received a frame");
    check(!full.currentFrame().isNull(), "the hidden full pane received the same tick's frame");
    check(thumbnail.currentFrame() == full.currentFrame(),
          "both panes hold a pixel-identical frame");
}

// The single-source rule is a property of the code, not of one recorded tick:
// latestFrame() must be called exactly once per update(), and neither pane may be
// skipped for being on the hidden page.
void the_frame_source_is_read_exactly_once() {
    QFile source(QStringLiteral(RM_TERMINAL_DASHBOARD_SOURCE));
    if (!source.open(QIODevice::ReadOnly | QIODevice::Text)) {
        throw std::runtime_error("cannot open dashboard.cpp to audit frame reads");
    }
    const QString text = QString::fromUtf8(source.readAll());

    int calls = 0;
    int from = 0;
    while ((from = text.indexOf(QStringLiteral("latestFrame()"), from)) != -1) {
        const int line_start = text.lastIndexOf(QLatin1Char('\n'), from) + 1;
        const QString line = text.mid(line_start, from - line_start).trimmed();
        if (!line.startsWith(QStringLiteral("//"))) ++calls;
        from += 1;
    }
    check(calls == 1, "dashboard.cpp reads latestFrame() exactly once per tick");
    check(text.contains(QStringLiteral("apply_frame(info_video_pane_, video_full_pane_")),
          "both panes are fed through the shared fan-out");

    const int body_start = text.indexOf(QStringLiteral("void apply_frame("));
    if (body_start == -1) throw std::runtime_error("cannot locate apply_frame definition");
    const int body_end = text.indexOf(QStringLiteral("\n}"), body_start);
    if (body_end == -1) throw std::runtime_error("cannot locate end of apply_frame");
    const QString body = text.mid(body_start, body_end - body_start);

    check(!body.contains(QStringLiteral("isVisible")) &&
              !body.contains(QStringLiteral("isHidden")) &&
              !body.contains(QStringLiteral("if ")),
          "apply_frame feeds both panes unconditionally, with no visibility short-circuit");
}

// The dump is the machine-readable half of the dual-mode evidence, so its contract
// is a regression target: same input must yield the same bytes, and a pane the
// stacked layout never showed must not publish the stale geometry() Qt left on it.
void the_layout_dump_is_reproducible_and_hides_unlaid_geometry() {
    Dashboard dashboard(test_config());
    dashboard.resize(kWidth, kHeight);
    drive_to_video(dashboard);

    const std::string dump = dashboard.layoutDump();
    check(dump == dashboard.layoutDump(),
          "two dumps of one unchanged layout are byte-identical");

    const QString text = QString::fromStdString(dump);
    check(text.contains(QStringLiteral("\"mode\": \"Video\"")),
          "the dump reports the mode actually on screen");

    // Field order is fixed so a textual diff means the layout moved, not that the
    // emitter reordered itself.
    const int mode_at = text.indexOf(QStringLiteral("\"mode\""));
    const int reason_at = text.indexOf(QStringLiteral("\"reason\""));
    const int index_at = text.indexOf(QStringLiteral("\"stacked_index\""));
    const int window_at = text.indexOf(QStringLiteral("\"window\""));
    const int panes_at = text.indexOf(QStringLiteral("\"panes\""));
    check(mode_at < reason_at && reason_at < index_at &&
              index_at < window_at && window_at < panes_at,
          "the dump keeps its documented field order");

    for (const char* name : {"map_pane", "info_video_pane", "video_full_pane",
                             "event_panel", "mode_banner"}) {
        check(text.contains(QStringLiteral("\"name\": \"%1\"").arg(QLatin1String(name))),
              (std::string("the dump covers ") + name).c_str());
    }

    // In video mode the map is on the hidden page: it must read as invisible with a
    // zeroed box rather than the leftover size from whenever it was last laid out.
    check(text.contains(QStringLiteral("{\"name\": \"map_pane\", \"visible\": false, \"x\": 0,"
                                      " \"y\": 0, \"width\": 0, \"height\": 0}")),
          "a pane on the hidden page reports zeroed geometry");
    check(!find(dashboard, "mapPane")->isVisible(),
          "that pane really is the hidden one, not merely reported so");
}

// (9) 弹窗必须真的画在屏幕里、盖在两页之上,而且**不改变布局**。纯逻辑测试
// 只能证明状态机选对了,证明不了它没被挤出窗口或被地图盖住。
void a_blocking_popup_renders_above_both_pages_without_moving_the_layout() {
    Dashboard dashboard(test_config());
    dashboard.resize(kWidth, kHeight);

    Snapshot healthy = clear_snapshot();
    dashboard.update(healthy, nullptr, 1000);
    settle(dashboard);

    const QWidget* popup = find(dashboard, "popupOverlay");
    check(!popup->isVisible(), "no popup during a healthy live match");

    // 记下健康状态下的几何,弹窗出现后必须逐一保持不变。
    const QRect map_before = find(dashboard, "mapPane")->geometry();
    const QRect ally_before = find(dashboard, "allyRoster")->geometry();
    const QRect enemy_before = find(dashboard, "enemyRoster")->geometry();

    Snapshot dead = clear_snapshot();
    dead.robots.at(RobotId{3}).dynamic.current_hp = present<std::uint32_t>(0);
    dashboard.update(dead, nullptr, 1250);
    settle(dashboard);

    check(popup->isVisible(), "hp=0 shows the popup overlay");
    check(dashboard.popup() == Popup::Eliminated, "the dashboard reports Eliminated");
    // 步兵、重装由复活框接管阵亡:状态照报 Eliminated,但不再画「已阵亡」盖在复活框上面。
    dashboard.setEliminatedOverlayReplaced(true);
    dashboard.update(dead, nullptr, 1300);
    settle(dashboard);
    check(!popup->isVisible() && dashboard.popup() == Popup::Eliminated,
          "with the revive popup attached, death keeps its state but not the old overlay");
    dashboard.setEliminatedOverlayReplaced(false);
    dashboard.update(dead, nullptr, 1350);
    settle(dashboard);

    // 文案断言落在纯函数上,而不是从控件里抓字符串:弹窗内容现在由 QML 排版,
    // 从宿主控件读不到文字,而「这个状态说了什么」仍然必须被钉住。
    check(popup_title(Popup::Eliminated).contains(QStringLiteral("已阵亡")),
          "the popup states the condition");

    // 布局不动是刻意的:弹窗一出现就推位整屏控件,会在最紧张的时刻废掉肌肉记忆。
    check(find(dashboard, "mapPane")->geometry() == map_before,
          "the popup does not move the map");
    check(find(dashboard, "allyRoster")->geometry() == ally_before,
          "the popup does not move the ally roster");
    check(find(dashboard, "enemyRoster")->geometry() == enemy_before,
          "the popup does not move the enemy roster");

    // 完整落在窗口内。限宽逻辑写错时弹窗会被裁掉一半,而文字恰好在被裁的那半边。
    check(dashboard.rect().contains(popup->geometry()),
          "the popup is fully inside the window");
    check(popup->width() > 0 && popup->height() > 0, "the popup has a real size");

    // 居中容差 2px:整数除法允许 1px 偏移,但不允许偏到一侧。
    const QPoint popup_center = popup->geometry().center();
    check(std::abs(popup_center.x() - dashboard.rect().center().x()) <= 2,
          "the popup is horizontally centred");
    check(std::abs(popup_center.y() - dashboard.rect().center().y()) <= 2,
          "the popup is vertically centred");

    // 真的画出了像素,而不是一个可见但透明的空盒子。
    const QImage shot = grab_rendered(popup);
    int opaque_pixels = 0;
    for (int y = 0; y < shot.height(); ++y) {
        for (int x = 0; x < shot.width(); ++x) {
            if (qAlpha(shot.pixel(x, y)) > 200) ++opaque_pixels;
        }
    }
    check(opaque_pixels > 0, "the popup renders opaque pixels over the page beneath it");

    // 恢复后必须消失,否则一次出局会把弹窗永久钉在屏幕上。dwell 窗口需要跨过。
    dashboard.update(healthy, nullptr, 1250 + 5000);
    settle(dashboard);
    check(!popup->isVisible(), "the popup clears once the condition is gone");
    check(dashboard.popup() == Popup::None, "the dashboard reports None after recovery");
}

// (10) 弹窗在图传全屏页同样有效。它是 Dashboard 的子控件而不是某一页的,
// 一份实现覆盖两种布局 —— 按页各放一份会在切换瞬间互相打断。
void the_popup_also_covers_the_video_page() {
    Dashboard dashboard(test_config());
    dashboard.resize(kWidth, kHeight);
    drive_to_video(dashboard);
    check(dashboard.mode() == UiMode::Video, "in video mode");

    Snapshot stale = blinded_snapshot();
    stale.game.current_stage = present<std::uint32_t>(4, Freshness::Stale);
    dashboard.update(stale, nullptr, 1250);
    settle(dashboard);

    const QWidget* popup = find(dashboard, "popupOverlay");
    check(popup->isVisible(), "the popup shows on the video page too");
    check(dashboard.popup() == Popup::LinkLost, "stale match data reports LinkLost");
    check(dashboard.rect().contains(popup->geometry()),
          "the popup stays inside the window in video mode");
}

// Regression: QPushButton 默认水平 Minimum，会跟两条横幅一起均分告警带隐藏时空出的
// 横向余量。实测 1280 宽下入口被拉到 419px（sizeHint 只有 80px）。宽度断言必须比
// sizeHint 收紧，否则按钮拉满半屏时本测试依旧全绿。
void the_reminder_entry_hugs_its_text_at_the_right_edge() {
    Dashboard dashboard(test_config());
    dashboard.resize(kMinWidth, kMinHeight);
    drive_to_info(dashboard);

    auto* entry = dashboard.findChild<QPushButton*>(QStringLiteral("reminderEntry"));
    if (!entry) throw std::runtime_error("reminderEntry not found");

    // 挂上控制器后的最长状态文本。构造真控制器要拖进音频后端，而这里要验的是布局对
    // 文本宽度的反应，直接设文本即可覆盖最坏情况。
    entry->setText(QStringLiteral("战术提醒 · 99 条"));
    settle(dashboard);

    const QWidget* alert = find(dashboard, "alertStrip");
    check(!alert->isVisible(), "the alert strip is hidden in the nominal case");
    check(entry->width() == entry->sizeHint().width(),
          "the reminder entry takes exactly its sizeHint, never the strip's slack");
    check(entry->geometry().right() < kMinWidth,
          "the reminder entry stays inside the minimum window width");
}

// 告警带(我方 X 号阵亡)在比分正上方的正中,不偏到一边;字太长时往空的那边让、截短,不压到状态条两边的牌子。
void the_alert_strip_sits_centred_above_the_score() {
    const auto with_dead = [](std::initializer_list<std::uint32_t> ids, bool blinded) {
        Snapshot snapshot = blinded ? blinded_snapshot() : clear_snapshot();
        for (const std::uint32_t id : ids) {
            RobotHealth robot;
            robot.id = RobotId{id};
            robot.faction = 1;
            robot.current_hp = present<std::uint32_t>(0);
            snapshot.robot_health.push_back(robot);
        }
        return snapshot;
    };
    const auto siblings_clear = [](const Dashboard& dashboard, const QWidget* strip) {
        for (const char* name : {"modeBanner", "linkLights", "recordingStatus", "selfCheckEntry", "keyboardMouseEntry",
                                 "reminderEntry", "uiSettingsEntry"}) {
            const QWidget* other = dashboard.findChild<QWidget*>(QString::fromLatin1(name));
            if (other && other->isVisible() && other->geometry().intersects(strip->geometry())) return false;
        }
        return true;
    };
    for (const QSize size : {QSize(1440, 900), QSize(kMinWidth, kMinHeight)}) {
        Dashboard dashboard(test_config());
        dashboard.resize(size);
        drive_to_info(dashboard);
        dashboard.update(with_dead({4}, false), nullptr, 4800);
        settle(dashboard);
        const auto* strip = static_cast<const QLabel*>(find(dashboard, "alertStrip"));
        const QWidget* top_bar = find(dashboard, "topBar");
        check(strip->isVisible() && strip->text() == QStringLiteral("我方 4 号阵亡"), "a dead robot of ours shows in the strip");
        check(std::abs(strip->geometry().center().x() - top_bar->geometry().center().x()) <= 2,
              "the strip is centred over the score, not pushed to one side");
        check(strip->geometry().bottom() < top_bar->geometry().top() && siblings_clear(dashboard, strip),
              "the strip sits in the status row above the score, clear of the chips on both sides");
    }
    // 最长的情况:被飞镖遮挡、数据过期、七台都阵亡,两边的牌子也都是最长的字。
    Dashboard dashboard(test_config());
    dashboard.resize(kMinWidth, kMinHeight);
    drive_to_info(dashboard);
    dashboard.setRecordingStatus(QStringLiteral("● REC 03:04 · 46.2 MB"));
    dashboard.findChild<QPushButton*>(QStringLiteral("reminderEntry"))->setText(QStringLiteral("战术提醒 · 99 条"));
    Snapshot worst = with_dead({1, 2, 3, 4, 5, 6, 7}, true);
    worst.game.stage_countdown_sec.freshness = Freshness::Stale;
    dashboard.update(worst, nullptr, 4800);
    settle(dashboard);
    const auto* strip = static_cast<const QLabel*>(find(dashboard, "alertStrip"));
    check(strip->isVisible() && siblings_clear(dashboard, strip) && dashboard.rect().contains(strip->geometry()),
          "a long strip moves over and stays clear of the other chips");
    check(strip->text().endsWith(QStringLiteral("…")) && strip->toolTip().contains(QStringLiteral("我方 7 号阵亡")),
          "text that does not fit is shortened and the whole of it is in the tooltip");
}

// Regression: 两层覆盖层都铺满整窗 + AlwaysStackOnTop,不穿透鼠标时会吞掉全屏点击。
// 备战阶段 PreMatch 弹窗常驻,操作手按顶部「战术提醒」毫无反应 —— 而那正是唯一
// 能改提醒的时机。断言落在 childAt() 上而不是 testAttribute():childAt() 跳过
// 穿透控件,等于真实命中测试,照抄属性名的测试证明不了点击真能到达按钮。
void the_overlays_never_swallow_clicks_on_the_reminder_entry() {
    Dashboard dashboard(test_config());
    dashboard.resize(kMinWidth, kMinHeight);
    drive_to_info(dashboard);

    auto* entry = dashboard.findChild<QPushButton*>(QStringLiteral("reminderEntry"));
    if (!entry) throw std::runtime_error("reminderEntry not found");
    const QWidget* popup = find(dashboard, "popupOverlay");
    const QWidget* victory = find(dashboard, "victoryOverlay");

    const QPoint target = entry->geometry().center();
    check(dashboard.childAt(target) == entry,
          "the reminder entry is hit-testable with no overlay up");

    dashboard.update(pre_match_snapshot(), nullptr, 5000);
    settle(dashboard);

    // 覆盖层真的在最上层盖住了那个点,否则下面的命中断言是空过的。
    check(popup->isVisible(), "the pre-match popup is up");
    check(dashboard.popup() == Popup::PreMatch, "the dashboard reports PreMatch");
    check(popup->geometry().contains(target),
          "the popup host really covers the reminder entry, so the check is not vacuous");
    check(dashboard.childAt(target) == entry,
          "the reminder entry stays clickable through the pre-match popup");

    dashboard.update(settled_snapshot(), nullptr, 5000 + 6000);
    settle(dashboard);

    check(victory->isVisible(), "the victory overlay is up");
    check(victory->geometry().contains(target),
          "the victory host really covers the reminder entry");
    check(dashboard.childAt(target) == entry,
          "the reminder entry stays clickable through the victory overlay");
}

// 结算动画现在是自绘的(自旧版逐帧素材换过来)。素材缺失时旧版只剩一行字,
// 所以这条断言落在**像素**上:覆盖层必须真的画出阵营色的亮像素,而不是
// 「属性设对了但画面只有一片压暗层」。
//
// 逐帧素材按设计不进仓库(红胜/蓝胜共 550MB),这台机器上也没有,因此一张
// 只有压暗层的图正是旧实现的真实表现 —— 这条测试守的就是那个退化。
void the_victory_overlay_draws_the_result_instead_of_only_dimming() {
    Dashboard dashboard(test_config());
    dashboard.resize(1440, 900);
    drive_to_info(dashboard);

    auto* victory = dashboard.findChild<QQuickWidget*>(QStringLiteral("victoryOverlay"));
    if (!victory) throw std::runtime_error("victoryOverlay not found");

    dashboard.update(settled_snapshot(), nullptr, 11000);
    settle(dashboard);
    check(victory->isVisible(), "the victory overlay is up for a decided round");

    QQuickItem* root = victory->rootObject();
    if (!root) throw std::runtime_error("victory overlay has no root object");

    check(root->property("title").toString() == QStringLiteral("红方胜利"),
          "the overlay carries the result title");
    check(root->property("outcome").toString() == QStringLiteral("red"),
          "the overlay carries the winning side for its colour scheme");
    // 逐帧播放器已经移除:frameUrls 不该再是覆盖层的输入。
    check(!root->property("frameUrls").isValid(),
          "the frame-sequence player is gone from the overlay");

    // 把动画时钟拨到爆发最盛的时刻,再抓一帧。按墙上时间 sleep 会抓到随机一帧。
    root->setProperty("autoPlay", false);
    root->setProperty("tMs", 900);
    settle(dashboard);
    QCoreApplication::processEvents();

    const QImage shot = grab_rendered(victory);
    int red_lit = 0;
    int bright = 0;
    for (int y = 0; y < shot.height(); y += 3) {
        for (int x = 0; x < shot.width(); x += 3) {
            const QColor c = shot.pixelColor(x, y);
            if (qAlpha(shot.pixel(x, y)) < 40) continue;
            if (c.red() > 90 && c.red() > c.blue() + 25) ++red_lit;
            if (c.red() + c.green() + c.blue() > 330) ++bright;
        }
    }
    // 旧实现(缺素材时)这两个计数都接近 0:只有一层 #B3000000 和一行白字。
    check(red_lit > 2000,
          "the burst paints a large field of red-dominant pixels for a red win");
    check(bright > 400, "the badge and title paint bright pixels, not just a dim layer");
}

// Esc 关掉结算面板,并且在同一次结算里不再弹回来 —— update() 每 250ms 跑一次,
// 一个会被下一跳覆盖的 bool 等于「关不掉」。换出新结果时必须自动恢复显示:
// 上一局按过的 Esc 不该把下一局的结算也吞掉。
void esc_dismisses_the_settlement_until_the_next_result() {
    Dashboard dashboard(test_config());
    dashboard.resize(1440, 900);
    drive_to_info(dashboard);

    auto* victory = dashboard.findChild<QQuickWidget*>(QStringLiteral("victoryOverlay"));
    if (!victory) throw std::runtime_error("victoryOverlay not found");

    dashboard.update(settled_snapshot(), nullptr, 11000);
    settle(dashboard);
    check(victory->isVisible(), "the settlement overlay is up");

    // 真键盘事件走到仪表盘上,而不是直接调一个私有方法:快捷键挂错 context
    // 或者没挂上,只有这样才看得出来。
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&dashboard, &press);
    QCoreApplication::processEvents();
    check(!victory->isVisible(), "Esc hides the settlement overlay");

    // 同一次结算继续送快照:面板不能自己回来。
    dashboard.update(settled_snapshot(), nullptr, 11250);
    settle(dashboard);
    dashboard.update(settled_snapshot(), nullptr, 11500);
    settle(dashboard);
    check(!victory->isVisible(), "the dismissed settlement stays closed while it is the same result");

    // 比赛继续(下一局备战):结果回到 None。
    dashboard.update(clear_snapshot(), nullptr, 20000);
    settle(dashboard);
    check(!victory->isVisible(), "no result means no overlay");

    // 下一局判出蓝胜:这是一次新的结算,必须显示。
    Snapshot next = settled_snapshot();
    next.game.winner = present<std::uint32_t>(2);
    dashboard.update(next, nullptr, 30000);
    settle(dashboard);
    check(victory->isVisible(), "a new result shows the overlay again after an earlier Esc");
    QQuickItem* root = victory->rootObject();
    if (!root) throw std::runtime_error("victory overlay has no root object");
    check(root->property("outcome").toString() == QStringLiteral("blue"),
          "the new result repaints in the new winner's colour");
}

// 四条链路全断时状态灯文字最长,自检按钮也最长:最小窗口不能因此被撑宽。赛前自检页
// 自己弹出且盖过弹窗,操作手关掉后本阶段不再弹,开赛自动收起。
void link_lights_and_self_check_fit_and_follow_the_stage() {
    Dashboard dashboard(test_config());
    dashboard.resize(kMinWidth, kMinHeight);
    drive_to_info(dashboard);

    LinkReport report;
    report.referee = {LinkLevel::Down, QStringLiteral("重连中"), QStringLiteral("断开了，正在重连"),
                      QStringLiteral("检查网线")};
    report.car = {LinkLevel::Down, QStringLiteral("无数据"), QStringLiteral("还没收到过车端数据"),
                  QString()};
    report.video = {LinkLevel::Down, QStringLiteral("无画面"), QStringLiteral("12 秒没画面"),
                    QString()};
    report.controller = {LinkLevel::Down, QStringLiteral("未接"),
                         QStringLiteral("找不到串口 /dev/serial/by-id/usb-arm"), QString()};
    SelfCheckInputs inputs;
    inputs.self_robot_id = 3;
    inputs.links = report;
    dashboard.setLinkReport(report);
    dashboard.setSelfCheck(build_self_check(inputs));
    dashboard.update(base_snapshot(), nullptr, 4000);
    settle(dashboard);

    const QLabel* lights = find_label(dashboard, "linkLights");
    check(lights->isVisible() && lights->text().contains(QStringLiteral("重连中")),
          "the link lights show the down reason");
    check(dashboard.width() == kMinWidth, "all-down link lights do not widen the minimum window");
    auto* reminder = dashboard.findChild<QPushButton*>(QStringLiteral("reminderEntry"));
    auto* entry = dashboard.findChild<QPushButton*>(QStringLiteral("selfCheckEntry"));
    if (!reminder || !entry) throw std::runtime_error("banner buttons not found");
    check(entry->text().contains(QStringLiteral("4 项异常")), "the self-check entry counts failures");
    check(reminder->geometry().right() < kMinWidth, "the reminder entry stays inside the window");
    check(!dashboard.selfCheckVisible(), "mid-match the self-check does not open by itself");

    const QWidget* popup = find(dashboard, "popupOverlay");
    dashboard.update(pre_match_snapshot(), nullptr, 5000);
    settle(dashboard);
    check(dashboard.selfCheckVisible(), "entering the pre-match phase opens the self-check");
    check(!popup->isVisible(), "the pre-match popup gives way to the self-check");
    const QWidget* panel = find(dashboard, "selfCheckPanel");
    check(dashboard.rect().contains(panel->geometry()), "the self-check stays inside the window");
    check(dashboard.childAt(entry->geometry().center()) == entry,
          "the self-check entry stays clickable while the page is open");

    dashboard.setSelfCheckVisible(false);
    dashboard.update(pre_match_snapshot(), nullptr, 5250);
    settle(dashboard);
    check(!dashboard.selfCheckVisible(), "a closed self-check stays closed within the phase");
    check(popup->isVisible(), "the pre-match popup returns once the self-check is closed");

    dashboard.setSelfCheckVisible(true);
    dashboard.update(base_snapshot(), nullptr, 6000);
    settle(dashboard);
    check(!dashboard.selfCheckVisible(), "the match starting closes the self-check");
}

// 事件横幅悬浮在比分栏下方:出现和消失都不能推动布局,严重的先显示,4 秒后换下一条。
void alert_banners_float_and_queue() {
    Dashboard dashboard(test_config());
    dashboard.resize(kMinWidth, kMinHeight);
    drive_to_info(dashboard);
    const QWidget* map = find(dashboard, "mapPane");
    const QRect map_before = map->geometry();

    dashboard.showAlert(QStringLiteral("小能量机关可以激活了"), AlertLevel::Info, 10000);
    dashboard.showAlert(QStringLiteral("基地被攻击"), AlertLevel::Critical, 10000);
    dashboard.flashBase(10000);
    dashboard.update(base_snapshot(), nullptr, 10000);
    settle(dashboard);
    check(dashboard.alertBannerText() == QStringLiteral("基地被攻击"), "the critical banner goes first");
    const QWidget* banner = find(dashboard, "alertBanner");
    const QWidget* top = find(dashboard, "topBar");
    check(banner->geometry().top() > top->geometry().bottom(), "the banner sits below the top bar");
    check(dashboard.rect().contains(banner->geometry()), "the banner stays inside the window");
    check(std::abs(banner->geometry().center().x() - dashboard.rect().center().x()) <= 1,
          "the banner is centred under the top bar, not pinned to the map");
    check(map->geometry() == map_before, "showing a banner does not move the layout");

    dashboard.update(base_snapshot(), nullptr, 14000);
    settle(dashboard);
    check(dashboard.alertBannerText() == QStringLiteral("小能量机关可以激活了"),
          "after 4 s the next banner shows");
    dashboard.update(base_snapshot(), nullptr, 18000);
    settle(dashboard);
    check(dashboard.alertBannerText().isEmpty(), "with nothing queued the banner hides");
    check(map->geometry() == map_before, "hiding the banner does not move the layout");
}

// 赛前卡片:倒计时下面一排大按钮(性能体系、键鼠控制、界面设置),点了和状态条上的小按钮一样;
// 有性能体系按钮时比分上方不再挂同一句提醒;P 面板开着时整张卡片让开。
void pre_match_card_offers_the_settings() {
    QTemporaryDir directory;
    UiProfileController profiles{UiProfileRepository(directory.path()), Role::Infantry};
    Config config;
    config.self_robot_id = 3;
    Dashboard dashboard(config);
    dashboard.resize(1440, 900);
    dashboard.attachUiProfiles(&profiles, {QStringLiteral("base")});
    int toggled = 0;
    int opened = 0;
    dashboard.setKeyboardMouseToggle([&toggled] { ++toggled; });
    dashboard.setPerformanceHandler([&opened] { ++opened; });
    dashboard.setPreMatchPerformance(QStringLiteral("还是默认，开赛就锁定"), true);
    const QString reminder = QStringLiteral("性能体系还是默认（血量优先 + 冷却优先）· 按 P 修改，开赛就锁定");
    dashboard.setPreMatchReminder(reminder);
    dashboard.update(pre_match_snapshot(), nullptr, 1000);
    dashboard.update(pre_match_snapshot(), nullptr, 1100);
    settle(dashboard);
    check(dashboard.preMatchButtons() ==
              QStringList({QStringLiteral("性能体系 · P"), QStringLiteral("键鼠控制 · F9"), QStringLiteral("界面设置")}),
          "the pre-match card lists the settings the seat has");
    const QLabel* strip = find_label(dashboard, "alertStrip");
    check(!strip->isVisible() || !strip->text().contains(QStringLiteral("性能体系")),
          "the performance reminder is on the card, not repeated above the score");

    auto* performance = dashboard.findChild<QAbstractButton*>(QStringLiteral("preMatchPerformance"));
    auto* keyboard = dashboard.findChild<QAbstractButton*>(QStringLiteral("preMatchKeyboardMouse"));
    auto* settings = dashboard.findChild<QAbstractButton*>(QStringLiteral("preMatchUiSettings"));
    if (!performance || !keyboard || !settings) throw std::runtime_error("pre-match buttons not found");
    const QWidget* popup = find(dashboard, "popupOverlay");
    check(popup->isVisible() && performance->isVisible() && keyboard->isVisible() && settings->isVisible(),
          "the card and its buttons are up before the match");
    check(performance->height() >= 64 && performance->width() >= 140 &&
              dashboard.rect().contains(settings->geometry()),
          "the buttons are big and inside the window");
    check(performance->geometry().top() > dashboard.height() / 2 - 150 &&
              performance->geometry().bottom() < dashboard.height() / 2 + 200,
          "the buttons sit in the card in the middle of the screen");
    check(dashboard.childAt(performance->geometry().center()) == performance,
          "the buttons are clickable through the popup layer");
    performance->click();
    keyboard->click();
    check(opened == 1 && toggled == 1, "the card buttons do what the status-bar entries do");
    settings->click();
    settle(dashboard);
    check(dashboard.uiSettingsVisible(), "the card's 界面设置 opens the settings page");
    dashboard.setUiSettingsVisible(false);

    dashboard.setSettingsPanelOpen(true);
    dashboard.update(pre_match_snapshot(), nullptr, 1200);
    settle(dashboard);
    check(!popup->isVisible() && !performance->isVisible(), "the card gives way while the P panel is open");
    dashboard.setSettingsPanelOpen(false);
    dashboard.update(pre_match_snapshot(), nullptr, 1300);
    settle(dashboard);
    check(popup->isVisible() && performance->isVisible(), "the card comes back when the P panel closes");

    dashboard.resize(2560, 1440);
    dashboard.update(pre_match_snapshot(), nullptr, 1400);
    settle(dashboard);
    check(performance->height() >= 100, "full screen makes the card buttons bigger");

    // 弹窗至少挂 1 秒,开赛 1 秒以后再看。
    dashboard.update(base_snapshot(), nullptr, 1500);
    dashboard.update(base_snapshot(), nullptr, 3000);
    settle(dashboard);
    check(!performance->isVisible() && dashboard.preMatchButtons().isEmpty(), "the card buttons go away when the match starts");
}

// 卡片上点「战术提醒」:提醒窗口开着时整张卡片让开,关掉窗口再回来,和 P 面板一样。
void pre_match_card_gives_way_to_the_reminder_window() {
    QTemporaryDir directory;
    UnavailableSpeechBackend speech(QStringLiteral("测试里没有语音"));
    ReminderController reminders(std::make_unique<ReminderRepository>(directory.filePath(QStringLiteral("cfg"))),
                                 &speech, 2200);
    reminders.start(directory.filePath(QStringLiteral("cache")));
    Config config;
    config.self_robot_id = 3;
    Dashboard dashboard(config);
    dashboard.resize(1440, 900);
    dashboard.attachReminders(&reminders);
    dashboard.update(pre_match_snapshot(), nullptr, 1000);
    dashboard.update(pre_match_snapshot(), nullptr, 1100);
    settle(dashboard);

    auto* entry = dashboard.findChild<QAbstractButton*>(QStringLiteral("preMatchReminders"));
    if (!entry) throw std::runtime_error("preMatchReminders not found");
    const QWidget* popup = find(dashboard, "popupOverlay");
    check(popup->isVisible() && entry->isVisible(), "the card offers 战术提醒 before the match");
    entry->click();
    auto* window = dashboard.findChild<ReminderDialog*>();
    check(window && window->isVisible(), "the card's 战术提醒 opens the reminder window");

    dashboard.update(pre_match_snapshot(), nullptr, 1200);
    settle(dashboard);
    check(!popup->isVisible() && !entry->isVisible(), "the card gives way while the reminder window is open");
    window->hide();
    dashboard.update(pre_match_snapshot(), nullptr, 1300);
    settle(dashboard);
    check(popup->isVisible() && entry->isVisible(), "the card comes back when the reminder window closes");
}

// 操作手把地图从布局里拿掉:横幅照样出现在顶栏下方,全屏时跟着变大。
void alert_banner_shows_without_a_map() {
    Dashboard dashboard(test_config());
    dashboard.resize(2560, 1440);
    UiScheme mapless = builtin_scheme(dashboard.role(), QStringLiteral("standard")).value();
    mapless.cells = layout_remove(scheme_layout(dashboard.role(), mapless).cells, PaneId::Map);
    dashboard.applyUiScheme(mapless);
    drive_to_info(dashboard);
    check(dashboard.paneCard(PaneId::Map) == nullptr, "the map is out of the layout");
    dashboard.showAlert(QStringLiteral("大能量机关可以激活了"), AlertLevel::Info, 10000);
    dashboard.update(base_snapshot(), nullptr, 10000);
    settle(dashboard);
    const QWidget* banner = find(dashboard, "alertBanner");
    const QWidget* top = find(dashboard, "topBar");
    check(banner->isVisible() && dashboard.alertBannerText() == QStringLiteral("大能量机关可以激活了"),
          "without a map the banner still shows");
    check(banner->geometry().top() > top->geometry().bottom() && dashboard.rect().contains(banner->geometry()),
          "without a map the banner sits below the top bar inside the window");
    check(banner->width() >= 1000 && banner->height() > GlowBanner::kHeight,
          "a full-screen window gets a wider and taller banner");
}

}  // namespace

// 三种席位的布局 × 两档屏幕:档案里的面板都在、互不重叠、不出窗口;不在档案里的
// 面板藏起来;地图保持 28:15;花名册两列等宽。RM_UI_EVIDENCE_DIR 设了就顺手出图。
// 界面设置里排出来的布局(包括内置的「图传为主」)在主界面上:面板互不重叠、都在窗口里,
// 而且把信息页铺满 —— 缩小地图以后空出来的格子由旁边的面板补上。
void dragged_layouts_fill_the_page() {
    for (const int seat : {3, 1}) {
        Config config;
        config.self_robot_id = seat;
        Dashboard dashboard(config);
        dashboard.resize(1440, 900);
        drive_to_info(dashboard);
        const Role role = dashboard.role();
        UiScheme shrunk = builtin_scheme(role, QStringLiteral("standard")).value();
        shrunk.cells = layout_profile(role).cells;
        for (PaneSlot& slot : shrunk.cells)
            if (slot.pane == PaneId::Map) slot.column_span = 5;
        shrunk.cells.push_back({PaneId::Timeline, 6, 9, 3, 3});
        // 地图和一个花名册也删掉:剩下的照样铺满。
        UiScheme mapless = builtin_scheme(role, QStringLiteral("standard")).value();
        mapless.cells = layout_remove(layout_remove(scheme_layout(role, mapless).cells, PaneId::Map), PaneId::AllyRoster);
        for (const UiScheme& scheme : {builtin_scheme(role, QStringLiteral("video")).value(), shrunk, mapless}) {
            dashboard.applyUiScheme(scheme);
            drive_to_info(dashboard);
            const LayoutProfile profile = scheme_layout(role, scheme);
            std::vector<QRect> boxes;
            QRect all;
            for (const PaneSlot& cell : profile.cells) {
                const QWidget* card = dashboard.paneCard(cell.pane);
                check(card && card->isVisible(), "every dragged pane is visible");
                const QRect box(card->mapTo(&dashboard, QPoint(0, 0)), card->size());
                for (const QRect& other : boxes) check(!other.intersects(box), "dragged panes never overlap");
                boxes.push_back(box);
                all = all.united(box);
            }
            check(dashboard.rect().contains(all), "dragged panes stay inside the window");
            const double step_x = all.width() / static_cast<double>(kGridColumns);
            const double step_y = all.height() / static_cast<double>(kGridRows);
            for (int r = 0; r < kGridRows; ++r)
                for (int c = 0; c < kGridColumns; ++c) {
                    const QPoint centre(all.left() + static_cast<int>((c + 0.5) * step_x),
                                        all.top() + static_cast<int>((r + 0.5) * step_y));
                    check(std::any_of(boxes.begin(), boxes.end(), [&](const QRect& b) { return b.contains(centre); }),
                          "no empty cell is left on the info page");
                }
        }
    }
}

void every_role_layout_fits_its_screens() {
    const std::vector<PaneId> all_panes{PaneId::AllyRoster, PaneId::EnemyRoster, PaneId::Map,
                                        PaneId::Events,     PaneId::Analysis,    PaneId::Video,
                                        PaneId::Assembly,   PaneId::Dart,        PaneId::Sentry};
    for (const int seat : {3, 1, 6}) {
        for (const QSize size : {QSize(kMinWidth, kMinHeight), QSize(1920, 1080)}) {
            Config config;
            config.self_robot_id = seat;
            Dashboard dashboard(config);
            dashboard.resize(size);
            drive_to_info(dashboard);
            // 内置方案会藏的那几块(事件、分析、图传、下一步)的每一种组合都在同一个窗口里依次重排一遍,
            // 最后回到默认布局:既验证每种藏法都排得下,也验证运行中重排不会留下上一次的拉伸或钉宽。
            // 花名册、地图也能关,那是在界面设置页里删,见 dragged_layouts_fill_the_page。
            const std::set<PaneId> optional{PaneId::Events, PaneId::Analysis, PaneId::Video, PaneId::NextStep};
            std::vector<PaneId> hideable;
            for (const PaneSlot& slot : layout_profile(dashboard.role()).cells)
                if (optional.count(slot.pane)) hideable.push_back(slot.pane);
            const unsigned combos = 1u << hideable.size();
            const std::string base_tag = role_name(dashboard.role()).toStdString() + " " +
                                         std::to_string(size.width()) + "x" + std::to_string(size.height());
            for (unsigned step = 1; step <= combos; ++step) {
                const unsigned mask = step % combos;
                UiScheme scheme;
                for (std::size_t i = 0; i < hideable.size(); ++i)
                    if (mask & (1u << i)) scheme.hidden.insert(hideable[i]);
                dashboard.applyUiScheme(scheme);
                drive_to_info(dashboard);
                const LayoutProfile profile = scheme_layout(dashboard.role(), scheme);
                const std::string tag = base_tag + " hidden=" + std::to_string(mask);
                check(dashboard.size() == size, ("the window keeps the requested size: " + tag).c_str());

                std::vector<QRect> boxes;
                for (const PaneSlot& cell : profile.cells) {
                    const QWidget* card = dashboard.paneCard(cell.pane);
                    check(card && card->isVisible(), ("every profile pane is visible: " + tag).c_str());
                    const QRect box(card->mapTo(&dashboard, QPoint(0, 0)), card->size());
                    check(dashboard.rect().contains(box), ("every pane stays inside the window: " + tag).c_str());
                    check(box.width() >= 150 && box.height() >= 100,
                          ("no pane collapses to a sliver: " + tag).c_str());
                    for (const QRect& other : boxes)
                        check(!other.intersects(box), ("panes never overlap: " + tag).c_str());
                    boxes.push_back(box);
                }
                for (const PaneId pane : all_panes) {
                    const bool listed = std::any_of(profile.cells.begin(), profile.cells.end(),
                                                    [pane](const PaneSlot& cell) { return cell.pane == pane; });
                    if (!listed)
                        check(dashboard.paneCard(pane) == nullptr, ("panes outside the profile stay hidden: " + tag).c_str());
                }
                const QWidget* map_card = dashboard.paneCard(PaneId::Map);
                int used = 0;
                for (const QRect& box : boxes) used += box.width() * box.height();
                const QRect grid_area = std::accumulate(boxes.begin(), boxes.end(), QRect(),
                                                        [](QRect a, const QRect& b) { return a | b; });
                check(map_card && used * 100 >= grid_area.width() * grid_area.height() * 95,
                      ("hidden panes leave no hole in the grid: " + tag).c_str());
                const QWidget* ally = find(dashboard, "allyRoster");
                const QWidget* enemy = find(dashboard, "enemyRoster");
                check(ally->width() == enemy->width(), ("both rosters are equally wide: " + tag).c_str());
                // 场地按比例画在格子里:格子不比场地需要的更高(上下没有黑带),画出来的场地是 28:15,
                // 而且至少占格子宽或高的 90%。
                const QWidget* map = find(dashboard, "mapPane");
                const FieldExtent field = default_field_extent();
                const MapViewport drawn = map_viewport(field, map->width(), map->height());
                check(map->height() <= map_height_for_width(field, map->width()) + 2,
                      ("the map cell is not taller than the field: " + tag).c_str());
                check(std::abs(static_cast<double>(drawn.width) / drawn.height - field.width_m / field.height_m) < 0.02,
                      ("the field is drawn at 28:15: " + tag).c_str());
                check(drawn.width >= map->width() * 0.9 || drawn.height >= map->height() * 0.9,
                      ("the field fills its cell: " + tag).c_str());
            }
            const std::string dump = dashboard.layoutDump();
            check(dump.find("\"role\": \"" + role_name(dashboard.role()).toStdString() + "\"") != std::string::npos,
                  ("the layout dump names the role: " + base_tag).c_str());

            const QString directory = qEnvironmentVariable("RM_UI_EVIDENCE_DIR");
            if (!directory.isEmpty()) {
                QImage shot = grab_rendered(&dashboard);
                QPainter painter(&shot);
                for (auto* quick : dashboard.findChildren<QQuickWidget*>()) {
                    if (quick->isVisible() && quick->status() == QQuickWidget::Ready)
                        painter.drawImage(QRect(quick->mapTo(&dashboard, QPoint()), quick->size()),
                                          quick->grabFramebuffer());
                }
                painter.end();
                QDir().mkpath(directory);
                shot.save(QStringLiteral("%1/layout-%2-%3x%4.png")
                              .arg(directory, role_name(dashboard.role()))
                              .arg(size.width())
                              .arg(size.height()));
            }
        }
    }
    check(Dashboard(Config{}).role() == Role::Infantry, "the default seat is the infantry layout");
}

// 指挥手点地图:点空地发点位标记、点敌方车发目标标记;0.5 秒内第二次被拦;断线时不发。
// 其他席位地图不响应点击,但照样画收到的标记。
// 标记工具条必须整条待在地图卡片里。1280x760 是程序的最小窗口(setMinimumSize),地图
// 那一列只有约 550 宽;工具条排成一行、最小宽度又比它大的话,最右边那颗开关会被卡片边
// 裁掉,甚至只剩一个空框。右列按 main.cpp 的做法把飞镖和哨兵面板放进去,宽度才算得真。
void the_mark_toolbar_stays_inside_the_map_card() {
    for (const QSize size : {QSize(1280, 760), QSize(1440, 900), QSize(1920, 1080)}) {
        Config config;
        config.self_robot_id = 6;
        Dashboard dashboard(config);
        if (QWidget* card = dashboard.roleCard(PaneId::Dart)) {
            auto* dart = new DartPane(card);
            dashboard.placePane(PaneId::Dart, dart);
            dart->setView(DartView{});
        }
        if (QWidget* card = dashboard.roleCard(PaneId::Sentry)) {
            auto* sentry = new SentryPane(card);
            dashboard.placePane(PaneId::Sentry, sentry);
            sentry->setView(SentryView{});
        }
        dashboard.resize(size);
        Snapshot snapshot = clear_snapshot();
        snapshot.self = RobotId{6};
        dashboard.update(snapshot, nullptr, 1000);
        settle(dashboard);

        const std::string tag = std::to_string(size.width()) + "x" + std::to_string(size.height());
        MarkToolbar* toolbar = dashboard.markToolbar();
        const QWidget* card = find(dashboard, "mapCard");
        check(toolbar != nullptr && card != nullptr, ("commander has a toolbar and a map card: " + tag).c_str());
        const QRect bar = QRect(toolbar->mapTo(card, QPoint()), toolbar->size());
        check(card->rect().contains(bar),
              ("the mark toolbar sits fully inside the map card: " + tag).c_str());
        check(toolbar->width() >= toolbar->minimumSizeHint().width(),
              ("the mark toolbar is never squeezed below its minimum width: " + tag).c_str());
    }
}

void the_commander_marks_the_map() {
    Config config;
    config.self_robot_id = 6;
    Dashboard dashboard(config);
    dashboard.resize(1920, 1080);
    Snapshot snapshot = clear_snapshot();
    snapshot.self = RobotId{6};
    MapRobot enemy;
    enemy.id = RobotId{103};
    enemy.faction = 2;
    enemy.from_radar = true;
    enemy.position.x = present<double>(20.0);
    enemy.position.y = present<double>(7.0);
    snapshot.map_robots.push_back(enemy);
    dashboard.update(snapshot, nullptr, 1000);
    settle(dashboard);

    std::vector<std::string> sent;
    std::vector<std::string> payloads;
    dashboard.setCommandSender([&sent, &payloads](const char* topic, const std::string& payload) {
        sent.push_back(std::string(topic) + ":" + std::to_string(payload.size()));
        payloads.push_back(payload);
        return true;
    });
    // 最近一条 MapClickCmd 的 is_send_all:0 指定 1 除哨兵 2 含哨兵。
    const auto last_scope = [&payloads] {
        rm::MapClickCmd message;
        if (payloads.empty() || !message.ParseFromString(payloads.back())) return -1;
        return static_cast<int>(message.is_send_all());
    };
    const auto last_recipients = [&payloads] {
        rm::MapClickCmd message;
        if (payloads.empty() || !message.ParseFromString(payloads.back())) return std::string();
        return message.robot_id();
    };
    MapPane* map = const_cast<MapPane*>(static_cast<const MapPane*>(find(dashboard, "mapPane")));
    const MapViewport view = map_viewport(default_field_extent(), map->width(), map->height());
    const Projected empty_spot = project(default_field_extent(), view, 8.0, 4.0);
    const Projected on_enemy = project(default_field_extent(), view, 20.0, 7.0);
    check(dashboard.markToolbar() != nullptr, "the commander has a mark toolbar");

    map->clickAt(QPointF(empty_spot.point.x, empty_spot.point.y));
    check(sent.empty() && dashboard.markToolbar()->statusText().contains(QStringLiteral("断线")),
          "while the link is down a mark is dropped, not queued");
    dashboard.setLinkUp(true);
    map->clickAt(QPointF(empty_spot.point.x, empty_spot.point.y));
    check(sent.size() == 1 && sent[0].rfind("MapClickCmd:", 0) == 0, "a click on open ground sends one MapClickCmd");
    map->clickAt(QPointF(on_enemy.point.x, on_enemy.point.y));
    check(sent.size() == 1 && dashboard.markToolbar()->statusText().contains(QStringLiteral("0.5")),
          "a second mark within 0.5 s is blocked");
    dashboard.update(snapshot, nullptr, 1600);
    map->clickAt(QPointF(on_enemy.point.x, on_enemy.point.y));
    check(sent.size() == 2 && dashboard.markToolbar()->statusText().contains(QStringLiteral("敌方 3 号")),
          "a click on an enemy robot marks that robot");

    // 默认不转发给哨兵:自动哨兵每条标记要花 50 金(规则 5.7.2),不能点一下地图就悄悄花掉。
    MarkToolbar* toolbar = dashboard.markToolbar();
    check(last_scope() == 1, "by default a mark goes to everyone except the sentry");
    check(!toolbar->sentryForwarding(), "sentry forwarding starts off");

    // 开启要二次确认:点一下只报价,再点才真的开。
    auto* sentry_toggle = toolbar->findChild<QPushButton*>(QStringLiteral("markSentry"));
    check(sentry_toggle != nullptr, "the toolbar has a sentry forwarding toggle");
    sentry_toggle->click();
    check(!toolbar->sentryForwarding() && toolbar->sentryArmed(),
          "one click only arms the toggle and shows the price");
    check(sentry_toggle->text().contains(QStringLiteral("50")),
          "the armed toggle names the price per mark for an automatic sentry");
    sentry_toggle->click();
    check(toolbar->sentryForwarding() && !toolbar->sentryArmed(),
          "confirming turns sentry forwarding on");
    dashboard.update(snapshot, nullptr, 2200);
    map->clickAt(QPointF(empty_spot.point.x, empty_spot.point.y));
    check(sent.size() == 3 && last_scope() == 2, "while forwarding is on a mark includes the sentry");

    // 只发某一台车时,哨兵只能靠写进收件人列表:指定 + 哨兵席位。
    for (QPushButton* chip : toolbar->findChildren<QPushButton*>())
        if (chip->text() == QStringLiteral("重装")) chip->click();
    dashboard.update(snapshot, nullptr, 2800);
    map->clickAt(QPointF(empty_spot.point.x, empty_spot.point.y));
    check(sent.size() == 4 && last_scope() == 0, "one named robot encodes the specific scope");
    const std::string list = last_recipients();
    check(list.size() == 7 && static_cast<unsigned char>(list[0]) == 1 &&
              static_cast<unsigned char>(list[1]) == 7,
          "a specific mark with forwarding on carries that robot and the sentry");

    // 关掉不用确认。
    sentry_toggle->click();
    check(!toolbar->sentryForwarding() && !toolbar->sentryArmed(),
          "turning sentry forwarding off takes a single click, no confirmation");
    for (QPushButton* chip : toolbar->findChildren<QPushButton*>())
        if (chip->text() == QStringLiteral("全体")) chip->click();
    dashboard.update(snapshot, nullptr, 3400);
    map->clickAt(QPointF(empty_spot.point.x, empty_spot.point.y));
    check(sent.size() == 5 && last_scope() == 1, "with forwarding off marks skip the sentry again");

    // 半自动哨兵不收干预费,开关要报「免费」而不是 50。
    snapshot.game.sentry_control = present<std::uint32_t>(1);
    dashboard.update(snapshot, nullptr, 4000);
    sentry_toggle->click();
    check(!sentry_toggle->text().contains(QStringLiteral("50")),
          "a semi-automatic sentry is not quoted the 50 coin fee");
    sentry_toggle->click();

    Dashboard infantry(test_config());
    infantry.resize(1440, 900);
    check(infantry.markToolbar() == nullptr, "other seats have no mark toolbar");
    MapPane* infantry_map = const_cast<MapPane*>(static_cast<const MapPane*>(find(infantry, "mapPane")));
    check(!infantry_map->clickAt(QPointF(100, 100)), "other seats' maps ignore clicks");
}

// 指挥手没有自己的图传:图传格子里是队友图传的小墙,点开是铺满图传页的大墙,再点回来。其他席位没有墙。
void the_commander_watches_a_wall_of_team_video() {
    Config config;
    config.self_robot_id = 6;
    Dashboard dashboard(config);
    dashboard.resize(1920, 1080);
    drive_to_info(dashboard);
    auto* compact = dashboard.findChild<VideoWall*>(QStringLiteral("videoWallCompact"));
    auto* full = dashboard.findChild<VideoWall*>(QStringLiteral("videoWallFull"));
    check(compact && full, "the commander has both walls");
    const QWidget* card = dashboard.paneCard(PaneId::Video);
    check(card && compact->isVisible() && compact->parentWidget() == card &&
              !find_pane(dashboard, "infoVideoPane")->isVisible(),
          "the video cell holds the small wall instead of a single picture");
    check(compact->feeds().size() == 3 && compact->feeds()[1].label == QStringLiteral("3 号 · 步兵") &&
              compact->feeds()[1].status == QStringLiteral("还没收到转发"),
          "before any relay the wall lists 1, 3 and 4 and says it is waiting");
    check(compact->tileRect(0).y() == compact->tileRect(2).y(), "the small wall is one row in the default cell");

    QImage red(VideoReceiver().frameWidth(), VideoReceiver().frameHeight(), QImage::Format_RGB32);
    red.fill(Qt::red);
    std::vector<WallFeed> feeds = compact->feeds();
    feeds[1].frame = red;
    dashboard.setWallFeeds(feeds);
    check(!compact->feeds()[1].frame.isNull() && !full->feeds()[1].frame.isNull(), "both walls get the same frames");

    const QPoint local = compact->rect().center();
    check(dashboard.childAt(compact->mapTo(&dashboard, local)) == compact, "native hit testing reaches the wall");
    QMouseEvent press(QEvent::MouseButtonPress, local, compact->mapTo(compact->window(), local),
                      compact->mapToGlobal(local), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(compact, &press);
    settle(dashboard);
    check(dashboard.mode() == UiMode::Video && full->isVisible() && !find_pane(dashboard, "videoFullPane")->isVisible(),
          "clicking the small wall opens the big one in place of the single picture");
    check(full->height() > dashboard.height() / 2 && full->tileRect(0).width() > 600,
          "the big wall fills the video page");
    check(!find_label(dashboard, "videoOverlayHp")->isVisible() &&
              find_label(dashboard, "videoOverlayCountdown")->isVisible(),
          "the commander's video page keeps the countdown but has no HP of its own");
    const std::string dump = dashboard.layoutDump();
    check(dump.find("video_wall_full\", \"visible\": true") != std::string::npos, "the layout dump shows the big wall");

    const QPoint back = full->rect().center();
    QMouseEvent again(QEvent::MouseButtonPress, back, full->mapTo(full->window(), back), full->mapToGlobal(back),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(full, &again);
    settle(dashboard);
    check(dashboard.mode() == UiMode::Info && compact->isVisible(), "clicking the big wall goes back");

    VideoReceiver silent;
    dashboard.update(clear_snapshot(), &silent, 5000);
    check(!dashboard.findChild<QLabel*>(QStringLiteral("alertStrip"))->text().contains(QStringLiteral("图传离线")),
          "the commander is not told its own (non-existent) video is offline");

    Dashboard infantry(test_config());
    check(infantry.findChild<VideoWall*>() == nullptr, "ground seats have no wall");
}

// 界面设置:勾掉一块面板,主界面立刻少一块、文件里记下;换方案;比赛中锁定、赛前解锁;
// 重开终端(新建一个读同一目录的控制器)以后设置还在。
void click_on(QWidget* widget, const QPoint& pos) {
    for (const QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
        QMouseEvent event(type, QPointF(pos), widget->mapToGlobal(QPointF(pos)), Qt::LeftButton,
                          type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(widget, &event);
    }
}

// 界面设置(效果图 10):点状态条的「界面设置」直接进一页,盖住整个窗口;中间是主界面的真画面,
// 删掉一块主界面马上跟着变、马上存盘;点方案整个换;比赛中锁定并关掉这一页。
void the_ui_settings_page_hides_panes_saves_and_locks() {
    QTemporaryDir directory;
    UiProfileController profiles{UiProfileRepository(directory.path()), Role::Infantry};
    Config config;
    config.self_robot_id = 3;
    Dashboard dashboard(config);
    dashboard.resize(1440, 900);
    dashboard.attachUiProfiles(&profiles, {QStringLiteral("base")});
    dashboard.update(pre_match_snapshot(), nullptr, 1000);
    settle(dashboard);
    auto* entry = dashboard.findChild<QPushButton*>(QStringLiteral("uiSettingsEntry"));
    check(entry && entry->isVisible() && entry->isEnabled(), "the settings entry is in the status bar before the match");
    entry->click();
    settle(dashboard);
    LayoutEditor* editor = dashboard.layoutEditor();
    check(dashboard.uiSettingsVisible() && editor->geometry() == dashboard.rect(),
          "the settings page opens straight away and covers the window");
    // 主界面排好以后约 120ms 截一张给设置页。
    for (int i = 0; i < 30 && !editor->hasPreview(); ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(10);
    }
    check(editor->hasPreview() && editor->canvasRect().width() > 900, "the page shows the whole screen in the middle");

    click_on(editor, editor->closeRect(PaneId::Events).center());
    settle(dashboard);
    check(dashboard.paneCard(PaneId::Events) == nullptr, "removing a pane takes it off the screen at once");
    const UiProfileLoad saved = UiProfileRepository(directory.path()).load();
    const std::vector<PaneSlot> saved_cells =
        scheme_layout(Role::Infantry, active_scheme(saved.book.operators.front().roles.at(Role::Infantry))).cells;
    check(saved.result == UiProfileLoadResult::Loaded &&
              std::none_of(saved_cells.begin(), saved_cells.end(),
                           [](const PaneSlot& slot) { return slot.pane == PaneId::Events; }),
          "the change is already on disk");

    QPushButton* novice = editor->schemeButton(QStringLiteral("novice"));
    check(novice != nullptr && editor->schemeButton(QStringLiteral("quiet")) == nullptr,
          "the built-in schemes are listed, without the old quiet one");
    novice->click();
    QCoreApplication::processEvents();
    settle(dashboard);
    check(profiles.activeScheme().id == QStringLiteral("novice") && dashboard.paneCard(PaneId::Events) != nullptr &&
              dashboard.paneCard(PaneId::NextStep) != nullptr,
          "choosing another scheme switches the whole screen");

    dashboard.update(clear_snapshot(), nullptr, 2000);
    settle(dashboard);
    check(!dashboard.uiSettingsVisible() && !entry->isEnabled() && profiles.locked(),
          "the match locks the settings and closes the page");
    entry->click();
    check(!dashboard.uiSettingsVisible(), "a locked entry does not open the page");
    dashboard.update(pre_match_snapshot(), nullptr, 3000);
    check(entry->isEnabled() && !profiles.locked(), "the next pre-match stage unlocks the settings");

    UiProfileController restarted{UiProfileRepository(directory.path()), Role::Infantry};
    const RoleSetup& setup = restarted.setup();
    const std::vector<PaneSlot> standard = scheme_layout(Role::Infantry, setup.schemes.front()).cells;
    check(restarted.activeScheme().id == QStringLiteral("novice") &&
              std::none_of(standard.begin(), standard.end(), [](const PaneSlot& slot) { return slot.pane == PaneId::Events; }),
          "after a restart the scheme and the edit to the standard scheme are both back");
}

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    qInstallMessageHandler(messages);
    try {
        a_role_pane_placed_while_off_screen_shows_up_later();
        the_alert_strip_sits_centred_above_the_score();
        info_mode_is_map_dominant_with_a_capped_thumbnail();
        every_role_layout_fits_its_screens();
        dragged_layouts_fill_the_page();
        the_mark_toolbar_stays_inside_the_map_card();
        the_commander_marks_the_map();
        the_commander_watches_a_wall_of_team_video();
        the_layout_fits_the_requested_screen_size();
        the_thumbnail_and_its_stats_never_overlap();
        clicking_video_panes_switches_the_visible_page();
        video_stats_stay_quiet_until_something_goes_wrong();
        video_faults_surface_on_change_and_clear_on_recovery();
        video_mode_fills_the_window_and_hides_the_map();
        the_reminder_entry_hugs_its_text_at_the_right_edge();
        the_overlays_never_swallow_clicks_on_the_reminder_entry();
        the_victory_overlay_draws_the_result_instead_of_only_dimming();
        esc_dismisses_the_settlement_until_the_next_result();
        video_mode_keeps_the_countdown_and_hp();
        a_critical_event_is_listed_and_coloured();
        rune_events_name_the_side();
        the_event_time_fits_its_column();
        both_panes_receive_one_identical_frame();
        the_frame_source_is_read_exactly_once();
        the_layout_dump_is_reproducible_and_hides_unlaid_geometry();
        a_blocking_popup_renders_above_both_pages_without_moving_the_layout();
        the_popup_also_covers_the_video_page();
        link_lights_and_self_check_fit_and_follow_the_stage();
        alert_banners_float_and_queue();
        alert_banner_shows_without_a_map();
        pre_match_card_offers_the_settings();
        pre_match_card_gives_way_to_the_reminder_window();
        the_ui_settings_page_hides_panes_saves_and_locks();
        check(!qml_error, "all dashboard QML loads and runs without warnings");
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    std::cout << "dashboard_layout: all checks passed\n";
    return 0;
}
