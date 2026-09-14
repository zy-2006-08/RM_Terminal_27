#pragma once

#include "domain.h"
#include "video_receiver.h"

#include <QImage>
#include <QLabel>
#include <QString>
#include <QWidget>

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
    explicit Dashboard(QWidget* parent = nullptr);
    void update(const Snapshot& snapshot, const VideoReceiver* video);

private:
    QLabel* game_;
    QLabel* robot_;
    QLabel* event_;
    QLabel* video_stats_;
    QLabel* banner_;
    VideoPane* pane_;
};

}
