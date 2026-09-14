#pragma once

#include "config.h"
#include "domain.h"
#include "ui_mode.h"
#include "video_receiver.h"

#include <QImage>
#include <QLabel>
#include <QString>
#include <QWidget>
#include <optional>

namespace rm_terminal {

QString game_panel_text(const Snapshot& snapshot);
QString robot_panel_text(const Snapshot& snapshot);
QString event_panel_text(const Snapshot& snapshot);
QString video_panel_text(const VideoReceiver* video);

// BGR24 from the decoder maps to Format_BGR888, and the QImage borrows the
// caller's bytes, so the returned image must be consumed before the next frame.
QImage frame_to_image(const QByteArray& frame, int width, int height);

class VideoPane final : public QWidget {
public:
    explicit VideoPane(QWidget* parent = nullptr);
    void setFrame(const QImage& image);
    void setStatus(const QString& status);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QImage image_;
    QString status_ = QStringLiteral("waiting for video");
};

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

private:
    QLabel* game_;
    QLabel* robot_;
    QLabel* event_;
    QLabel* video_stats_;
    QLabel* banner_;
    VideoPane* pane_;
    UiModeMachine machine_;
    UiMode logged_mode_ = UiMode::Info;
};

}
