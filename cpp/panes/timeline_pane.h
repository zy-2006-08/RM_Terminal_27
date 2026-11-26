#pragma once

#include "match_alerts.h"

#include <QWidget>
#include <optional>
#include <vector>

namespace rm_terminal
{

    // 「接下来的时间点」:能量机关、飞镖闸门什么时候开放,还有几秒。比赛刚开始战场事件是空的,
    // 这块告诉操作手接下来会发生什么。时间点来自 match_timeline(),和事件提醒是同一张表。
    class TimelinePane : public QWidget
    {
    public:
        explicit TimelinePane(QWidget* parent = nullptr);

        // 开赛后第几秒和本局还剩几秒;不在比赛中时传空,面板写「开赛后显示」。
        void setClock(std::optional<std::int32_t> elapsed_sec, std::optional<std::int32_t> remaining_sec);

        QSize sizeHint() const override;
        QSize minimumSizeHint() const override;

    protected:
        void paintEvent(QPaintEvent* event) override;

    private:
        std::vector<TimelineMark> marks_;
        std::optional<std::int32_t> elapsed_;
        std::optional<std::int32_t> remaining_;
    };

} // namespace rm_terminal
