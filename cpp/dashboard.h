#pragma once

#include "config.h"
#include "domain.h"
#include "map_pane.h"
#include "roster_pane.h"
#include "roster_panel.h"
#include "battle_analysis_pane.h"
#include "top_bar.h"
#include "ui_mode.h"
#include "video_receiver.h"

#include <QImage>
#include <QLabel>
#include <QStackedLayout>
#include <QString>
#include <QWidget>
#include <cstddef>
#include <optional>
#include <string>

namespace rm_terminal {

// Shared by both layout pages. A second set of these for the video page would
// let the two pages disagree about the same underlying values.
QString event_panel_text(const Snapshot& snapshot);

// VideoReceiver::snapshot() 的计数器是**累计值**。
struct VideoFaultCounts {
    qint64 missing = 0;
    qint64 out_of_order = 0;
    qint64 duplicates = 0;
    qint64 incomplete = 0;
    qint64 expired = 0;
};

VideoFaultCounts video_fault_counts(const VideoReceiver* video);

constexpr MonotonicMs kVideoFaultHoldMs = 3000;

// 累计计数器用 `>0` 判定,会让开局的一次丢包在整场比赛里永久挂在界面上,操作手
// 无法分辨「正在丢包」和「三十分钟前丢过一次」。这里只报告相对基线的新增量,并
// 让它可见 kVideoFaultHoldMs —— 否则单帧抖动会一闪而过看不见。恢复后自动消失。
class VideoFaultTracker {
public:
    QString faultLine(const VideoFaultCounts& counts, MonotonicMs now);

private:
    VideoFaultCounts baseline_;
    bool seeded_ = false;
    MonotonicMs visible_until_ = 0;
    QString held_;
};

// 图传状态。正常返回单行状态;仅在**新增**故障时追加故障行。tracker 为空时不做
// 增量判定,只返回状态文字。
QString video_panel_text(const VideoReceiver* video, VideoFaultTracker* tracker = nullptr,
                         MonotonicMs now = 0);

constexpr std::size_t kEventPanelRows = 14;

QColor event_level_color(std::uint32_t level);

// 全宽告警带的文字。空字符串表示一切正常,调用方据此隐藏整条 —— 常态占位会让
// 操作手习惯性忽略它,而这条带子存在的唯一目的就是打破这种忽略。
QString alert_strip_text(const Snapshot& snapshot, const VideoReceiver* video);

// Newest first, one row per record, coloured by level. Truncation is disclosed
// rather than implied away: a panel that silently drops records reads as a quiet
// match.
QString event_history_html(const Snapshot& snapshot, std::size_t max);

// Countdown and HP survive into the video overlay because during blinding they
// are the only match state the operator can still rely on.
QString video_overlay_countdown_text(const Snapshot& snapshot);
QString video_overlay_hp_text(const Snapshot& snapshot);

// BGR24 from the decoder maps to Format_BGR888, and the QImage borrows the
// caller's bytes, so the returned image must be consumed before the next frame.
QImage frame_to_image(const QByteArray& frame, int width, int height);

// Q_OBJECT so findChild<VideoPane*> resolves through qobject_cast. Looking these
// up by name as a bare QWidget and casting would be unchecked.
class VideoPane final : public QWidget {
    Q_OBJECT
public:
    explicit VideoPane(QWidget* parent = nullptr);
    void setFrame(const QImage& image);
    void setStatus(const QString& status);

    // Observation only. Returns the owned copy this pane is painting, so a caller
    // can confirm both panes received the same frame.
    const QImage& currentFrame() const { return image_; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QImage image_;
    QString status_ = QStringLiteral("waiting for video");
};

// Hands ONE already-owned frame to both panes.
//
// Both panes are fed unconditionally, including the one on the hidden page. A
// visibility short-circuit would leave the target pane holding the previous frame
// at the instant of a switch, so the operator would see a stale picture for one
// tick exactly when the layout changed.
void apply_frame(VideoPane* thumbnail, VideoPane* full, const QImage& frame);

class Dashboard final : public QWidget {
public:
    explicit Dashboard(const Config& config, QWidget* parent = nullptr);

    // `now` is injected, never read inside this class: the caller's snapshot and
    // this mode decision must come from the same instant, and a second clock
    // source here would diverge from the one feeding Store::snapshot.
    void update(const Snapshot& snapshot, const VideoReceiver* video, MonotonicMs now);

    UiMode mode() const { return machine_.mode(); }
    ModeReason reason() const { return machine_.reason(); }
    void forceMode(std::optional<UiMode> forced) { machine_.forceMode(forced); }

    // Structured layout evidence: mode, reason, and the geometry of every pane that
    // distinguishes the two layouts. Deliberately carries no match data or robot
    // coordinates - it is evidence about the LAYOUT, and mixing live values in would
    // make a byte-for-byte diff fail for reasons that have nothing to do with layout.
    std::string layoutDump() const;

private:
    // Two VideoPane instances, one per page, sharing the single resident
    // VideoReceiver. The same pane is deliberately NOT reparented between pages:
    // the reason is engineering determinism, avoiding a simultaneous
    // parent/geometry/repaint change at the moment of a switch, so each page's
    // layout ownership is fixed and each page always holds its own copy of the
    // latest frame. The cost is one extra lightweight widget.
    QStackedLayout* stack_;
    QWidget* info_page_;
    QWidget* video_page_;

    QLabel* banner_;
    QLabel* mode_banner_;
    QLabel* alert_strip_;

    TopBar* top_bar_;
    RosterPanel* ally_roster_;
    RosterPanel* enemy_roster_;
    BattleAnalysisPane* analysis_;

    MapPane* map_;
    QLabel* event_;
    QLabel* video_stats_;
    VideoPane* info_video_pane_;

    VideoPane* video_full_pane_;
    QLabel* video_countdown_;
    QLabel* video_hp_;

    UiModeMachine machine_;
    UiMode logged_mode_ = UiMode::Info;
    VideoFaultTracker video_faults_;
};

}
