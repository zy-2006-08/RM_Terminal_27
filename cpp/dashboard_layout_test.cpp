#include "dashboard.h"

#include <QApplication>
#include <QCoreApplication>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace rm_terminal;

namespace {

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
    state.dynamic.max_hp = present<std::uint32_t>(600);
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

void settle(Dashboard& dashboard) {
    dashboard.show();
    QCoreApplication::processEvents();
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
    check(thumbnail->height() >= 90 && thumbnail->width() >= 160,
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

// (4): the read-only guarantee is not a per-mode decision.
void the_read_only_banner_survives_both_modes() {
    Dashboard dashboard(test_config());
    dashboard.resize(kWidth, kHeight);

    drive_to_info(dashboard);
    check(find(dashboard, "readOnlyBanner")->isVisible(),
          "the read-only banner is visible in info mode");

    drive_to_video(dashboard);
    check(find(dashboard, "readOnlyBanner")->isVisible(),
          "the read-only banner is visible in video mode");
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

// (6): a critical event must be both present and unmistakably coloured.
void a_critical_event_is_listed_and_coloured() {
    Dashboard dashboard(test_config());
    dashboard.resize(kWidth, kHeight);

    Snapshot snapshot = clear_snapshot();
    snapshot.events.push(EventRecord{1000, 0, "常规事件", 900});
    snapshot.events.push(EventRecord{2000, 3, "基地被击破", 950});
    dashboard.update(snapshot, nullptr, 1000);
    settle(dashboard);

    const QLabel* panel = find_label(dashboard, "eventPanel");
    check(panel->text().contains(QStringLiteral("基地被击破")),
          "the critical event appears in the panel");
    check(panel->text().contains(QStringLiteral("常规事件")),
          "the ordinary event is still listed");

    QImage shot(panel->size(), QImage::Format_ARGB32);
    shot.fill(Qt::black);
    const_cast<QLabel*>(panel)->render(&shot);
    int alert_pixels = 0;
    for (int y = 0; y < shot.height(); ++y) {
        for (int x = 0; x < shot.width(); ++x) {
            const QRgb p = shot.pixel(x, y);
            if (qRed(p) > qGreen(p) + 60 && qRed(p) > qBlue(p) + 60) ++alert_pixels;
        }
    }
    check(alert_pixels > 0, "the critical event renders in the alert colour");
}

// (7): both panes are handed the SAME frame every tick, including the hidden one.
// A per-pane latestFrame() call could straddle a frame advance, and a visibility
// short-circuit would leave the target page one frame stale at the switch.
void both_panes_receive_one_identical_frame() {
    VideoPane thumbnail;
    VideoPane full;
    thumbnail.resize(320, 180);
    full.resize(960, 540);

    QImage frame(VideoReceiver::frameWidth(), VideoReceiver::frameHeight(),
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
    const int banner_at = text.indexOf(QStringLiteral("\"readonly_banner_visible\""));
    const int index_at = text.indexOf(QStringLiteral("\"stacked_index\""));
    const int window_at = text.indexOf(QStringLiteral("\"window\""));
    const int panes_at = text.indexOf(QStringLiteral("\"panes\""));
    check(mode_at < reason_at && reason_at < banner_at && banner_at < index_at &&
              index_at < window_at && window_at < panes_at,
          "the dump keeps its documented field order");

    for (const char* name : {"map_pane", "info_video_pane", "video_full_pane",
                             "event_panel", "mode_banner", "readonly_banner"}) {
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

}  // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    try {
        info_mode_is_map_dominant_with_a_capped_thumbnail();
        the_layout_fits_the_requested_screen_size();
        the_thumbnail_and_its_stats_never_overlap();
        video_stats_stay_quiet_until_something_goes_wrong();
        video_faults_surface_on_change_and_clear_on_recovery();
        video_mode_fills_the_window_and_hides_the_map();
        the_read_only_banner_survives_both_modes();
        video_mode_keeps_the_countdown_and_hp();
        a_critical_event_is_listed_and_coloured();
        both_panes_receive_one_identical_frame();
        the_frame_source_is_read_exactly_once();
        the_layout_dump_is_reproducible_and_hides_unlaid_geometry();
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    std::cout << "dashboard_layout: all checks passed\n";
    return 0;
}
