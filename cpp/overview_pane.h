#pragma once

#include "overview.h"

#include <QWidget>

namespace rm_terminal {

// 指挥手的「大局」(大局判断表.md):上面一行现在结束和照这样到 7:00 谁赢,中间是控制度差的
// 趋势线(实线是已经发生的,虚线照现在的速度画到 7:00),下面是翻盘点、增益和下一个时间点。
// 矮的时候先省掉趋势线,字都留着。
class OverviewPane : public QWidget {
public:
    explicit OverviewPane(QWidget* parent = nullptr);

    void setOverview(Overview overview);
    void setOwnFaction(std::uint32_t faction);
    const Overview& overview() const { return overview_; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent*) override;

private:
    void paintTrend(QPainter& painter, const QRectF& area) const;

    Overview overview_;
    std::uint32_t own_faction_ = 1;
};

}  // namespace rm_terminal
