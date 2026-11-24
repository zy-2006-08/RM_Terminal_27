#pragma once

#include <QImage>
#include <QRect>
#include <QSize>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <vector>

class QMouseEvent;

namespace rm_terminal {

class VideoReceiver;

// 指挥手图传墙上的一格。
struct WallFeed {
    std::uint32_t robot_id = 0;
    QString label;
    // 断了就是空的,不留最后一帧:冻住的画面会被当成那台车停着不动。
    QImage frame;
    // 没画面时格子中间写的原因。
    QString status;
};

// 指挥手看己方重装和两台步兵(蓝方 +100)。哨兵没有操作手,也就没有选手端收它的图传。
std::vector<std::uint32_t> wall_robots(std::uint32_t self_robot_id);
QString wall_label(std::uint32_t robot_id);
// 接收器这一拍的样子。receiver 为空是回放:录像里只有本席位收到的图传,指挥手那里没有。
WallFeed wall_feed(std::uint32_t robot_id, const VideoReceiver* receiver);
// count 路 16:9 的画面在 area 里怎么排得最大。小一成以内选空格少的(3 路在扁长的格子里排一行,
// 不排成 2 × 2 空一格);最后一行不满时居中。
std::vector<QRect> wall_tiles(QSize area, int count, int gap);

// 指挥手的图传墙,像监控墙。Compact 放在信息页的图传格子里,底下一行写几路有画面;点一下切到图传页,
// 那里是 Full,铺满整页,再点一下回去。
class VideoWall final : public QWidget {
    Q_OBJECT
public:
    enum class Mode { Compact, Full };
    explicit VideoWall(Mode mode, QWidget* parent = nullptr);
    void setFeeds(std::vector<WallFeed> feeds);
    const std::vector<WallFeed>& feeds() const { return feeds_; }
    QRect tileRect(int index) const;

Q_SIGNALS:
    void clicked();

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    bool captioned() const;
    QRect tileArea() const;
    int gap() const { return mode_ == Mode::Full ? 8 : 6; }
    Mode mode_;
    std::vector<WallFeed> feeds_;
};

}  // namespace rm_terminal
