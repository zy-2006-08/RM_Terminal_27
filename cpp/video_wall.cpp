#include "video_wall.h"

#include "dashboard.h"
#include "presentation.h"
#include "theme.h"
#include "video_receiver.h"

#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>

#include <algorithm>

namespace rm_terminal {

std::vector<std::uint32_t> wall_robots(std::uint32_t self_robot_id) {
    const std::uint32_t side = self_robot_id > 100 ? 100 : 0;
    return {side + 1, side + 3, side + 4};
}

QString wall_label(std::uint32_t robot_id) {
    return QStringLiteral("%1 号 · %2").arg(display_robot_number(robot_id)).arg(robot_class_name(robot_id));
}

WallFeed wall_feed(std::uint32_t robot_id, const VideoReceiver* receiver) {
    WallFeed feed;
    feed.robot_id = robot_id;
    feed.label = wall_label(robot_id);
    if (!receiver) {
        feed.status = QStringLiteral("录像里没有队友图传");
    } else if (receiver->online()) {
        feed.frame = frame_to_image(receiver->latestFrame(), VideoReceiver::frameWidth(), VideoReceiver::frameHeight())
                         .copy();
    } else if (receiver->state() == QLatin1String("bind_failed")) {
        feed.status = QStringLiteral("收不了：端口被占用");
    } else if (receiver->msSinceLastFrame() >= 0) {
        feed.status = QStringLiteral("%1 秒没画面").arg(receiver->msSinceLastFrame() / 1000);
    } else if (receiver->packetsReceived() > 0) {
        feed.status = QStringLiteral("收到转发，还没解出画面");
    } else {
        feed.status = QStringLiteral("还没收到转发");
    }
    return feed;
}

std::vector<QRect> wall_tiles(QSize area, int count, int gap) {
    if (count <= 0 || area.isEmpty()) return {};
    struct Option {
        int columns;
        int rows;
        int empty;
        double width;
    };
    std::vector<Option> options;
    double best = 0;
    for (int columns = 1; columns <= count; ++columns) {
        const int rows = (count + columns - 1) / columns;
        const double by_width = (area.width() - (columns - 1) * gap) / static_cast<double>(columns);
        const double by_height = (area.height() - (rows - 1) * gap) / static_cast<double>(rows) * 16.0 / 9.0;
        const double width = std::max(0.0, std::min(by_width, by_height));
        options.push_back({columns, rows, columns * rows - count, width});
        best = std::max(best, width);
    }
    const Option* chosen = nullptr;
    for (const Option& option : options) {
        if (option.width < best * 0.9) continue;
        if (!chosen || option.empty < chosen->empty || (option.empty == chosen->empty && option.width > chosen->width))
            chosen = &option;
    }
    const int width = static_cast<int>(chosen->width);
    const int height = width * 9 / 16;
    if (width < 2 || height < 2) return {};
    const int left = (area.width() - (chosen->columns * width + (chosen->columns - 1) * gap)) / 2;
    const int top = (area.height() - (chosen->rows * height + (chosen->rows - 1) * gap)) / 2;
    std::vector<QRect> tiles;
    for (int i = 0; i < count; ++i) {
        const int row = i / chosen->columns;
        const int in_row = std::min(chosen->columns, count - row * chosen->columns);
        const int shift = (chosen->columns - in_row) * (width + gap) / 2;
        tiles.emplace_back(left + shift + (i % chosen->columns) * (width + gap), top + row * (height + gap), width,
                           height);
    }
    return tiles;
}

VideoWall::VideoWall(Mode mode, QWidget* parent) : QWidget(parent), mode_(mode) {
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumSize(mode == Mode::Full ? QSize(320, 180) : QSize(160, 72));
    setCursor(Qt::PointingHandCursor);
    setToolTip(mode == Mode::Full ? QStringLiteral("点击返回自动视图（致盲时保持全屏）")
                                  : QStringLiteral("点击放大，看所有队友的图传"));
}

void VideoWall::setFeeds(std::vector<WallFeed> feeds) {
    feeds_ = std::move(feeds);
    update();
}

bool VideoWall::captioned() const { return mode_ == Mode::Compact && height() >= 90; }

QRect VideoWall::tileArea() const {
    constexpr int kCaption = 22;
    return captioned() ? rect().adjusted(0, 0, 0, -kCaption) : rect();
}

QRect VideoWall::tileRect(int index) const {
    const QRect area = tileArea();
    const std::vector<QRect> tiles = wall_tiles(area.size(), static_cast<int>(feeds_.size()), gap());
    if (index < 0 || index >= static_cast<int>(tiles.size())) return {};
    return tiles[static_cast<std::size_t>(index)].translated(area.topLeft());
}

void VideoWall::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        event->accept();
        Q_EMIT clicked();
        return;
    }
    QWidget::mousePressEvent(event);
}

void VideoWall::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    const bool full = mode_ == Mode::Full;
    const qreal cut = full ? 8 : 5;
    const QFont label_font = theme::labelPx(full ? 14 : 12, QFont::DemiBold);
    const QFontMetrics label_metrics(label_font);
    int live = 0;
    for (std::size_t i = 0; i < feeds_.size(); ++i) {
        const QRectF box(tileRect(static_cast<int>(i)));
        if (box.isEmpty()) continue;
        const WallFeed& feed = feeds_[i];
        painter.save();
        painter.setClipPath(theme::chamferPath(box, cut));
        painter.fillRect(box, theme::kVideoBackground);
        if (!feed.frame.isNull()) {
            ++live;
            painter.drawImage(box, feed.frame);
        } else {
            painter.setFont(theme::labelPx(full ? 15 : 12));
            painter.setPen(theme::kTextSecondary);
            painter.drawText(box.adjusted(6, 6, -6, -6), Qt::AlignCenter | Qt::TextWordWrap, feed.status);
        }
        // 左上角写是哪台车,左边一道阵营色。格子太矮就不写,免得盖住半个画面。
        const int pad = full ? 8 : 5;
        const QRectF chip(box.left() + pad, box.top() + pad, label_metrics.horizontalAdvance(feed.label) + 2 * pad + 3,
                          label_metrics.height() + (full ? 6 : 2));
        if (box.height() >= chip.height() * 2.5) {
            painter.fillRect(chip, QColor(0, 0, 0, 150));
            painter.fillRect(QRectF(chip.left(), chip.top(), 3, chip.height()),
                             feed.robot_id > 100 ? theme::kBlue : theme::kRed);
            painter.setFont(label_font);
            painter.setPen(Qt::white);
            painter.drawText(chip.adjusted(3, 0, 0, 0), Qt::AlignCenter, feed.label);
        }
        painter.restore();
        painter.setPen(QPen(theme::kBorder, 1));
        painter.drawPath(theme::chamferPath(box.adjusted(0.5, 0.5, -0.5, -0.5), cut));
    }
    if (!captioned()) return;
    const QRectF line(4, height() - 22, width() - 8, 22);
    painter.setFont(theme::labelPx(13));
    painter.setPen(theme::kTextPrimary);
    painter.drawText(line, Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("队友图传 · %1/%2 路有画面").arg(live).arg(feeds_.size()));
    painter.setPen(theme::kTextSecondary);
    painter.drawText(line, Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("点击放大"));
}

}  // namespace rm_terminal
