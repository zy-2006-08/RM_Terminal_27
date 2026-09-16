#pragma once

#include "domain.h"

#include <QString>
#include <QWidget>
#include <cstddef>
#include <vector>

namespace rm_terminal {

// 两侧花名册列的宽度。1280 宽下两列 250 + 盈余给地图仍成立,故取 250。定义在这里
// 而不是 dashboard.h:事件面板的宽度提示要用它,而 dashboard.h 反向依赖本头文件。
constexpr int kRosterColumnWidth = 250;

// 事件横幅的配色来源。faction 之外单列 Positive:能量机关一类正向事件无论由哪
// 方激活都走绿色横幅,不能按阵营染成红/蓝。
enum class EventBanner { Neutral, Red, Blue, Positive };

EventBanner event_banner(const EventRecord& record);
QString event_banner_asset(EventBanner banner);

// 终端自己下发指令后的回显(底盘模式、自瞄、紧急停止、云台回中)不是赛事事件,
// 不进这个面板。仍照常写入 EventHistory 与日志,只是不渲染。
bool is_local_control_echo(const EventRecord& record);

struct EventFeedRow {
    QString time;
    QString text;
    EventBanner banner;
};

std::vector<EventFeedRow> build_event_rows(const Snapshot& snapshot, std::size_t max);

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
