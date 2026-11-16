#pragma once

#include "domain.h"

#include <QFont>
#include <QString>
#include <QWidget>
#include <cstddef>
#include <map>
#include <vector>

namespace rm_terminal {

// 两侧花名册列的最小宽度。实际列宽是窗口宽度的四分之一(dashboard.cpp),窗口再窄也不低于
// 这个值。定义在这里而不是 dashboard.h:事件面板的宽度提示要用它,而 dashboard.h 反向依赖
// 本头文件。
constexpr int kRosterColumnWidth = 250;

// 玻璃卡片内容区的左右内边距之和(8 + 8)。面板的宽度提示要减掉它,否则卡片加上边距后
// 会把所在列顶宽。
constexpr int kCardHorizontalMargins = 16;

constexpr int kEventTimeWidth = 32;

// 时间列显示收到事件时的比赛倒计时「MM:SS」,用数字字体,比事件正文小一号。
QFont event_time_font();

// 事件左侧色条的配色来源。faction 之外单列 Positive:能量机关一类正向事件无论由哪
// 方激活都走绿色,不能按阵营染成红/蓝。
enum class EventBanner { Neutral, Red, Blue, Positive };

EventBanner event_banner(const EventRecord& record);
QString event_banner_asset(EventBanner banner);


struct EventFeedRow {
    QString time;
    QString text;
    EventBanner banner;
};

// rune_sides:增益推算分出来的能量机关事件归属(收到时刻 -> 阵营),给了就在这两条前面写「我方/敌方」。
std::vector<EventFeedRow> build_event_rows(const Snapshot& snapshot, std::size_t max,
                                           const std::map<MonotonicMs, std::uint32_t>* rune_sides = nullptr);

class EventFeedPane : public QWidget {
    Q_OBJECT

public:
    explicit EventFeedPane(QWidget* parent = nullptr);

    void setRows(const std::vector<EventFeedRow>& rows);
    void setDroppedCount(std::size_t dropped);

    QSize sizeHint() const override;
    static int rowHeight();

protected:
    void paintEvent(QPaintEvent*) override;

private:
    std::vector<EventFeedRow> rows_;
    std::size_t dropped_ = 0;
};

}
