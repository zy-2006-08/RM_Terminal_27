#pragma once

#include "battle_analysis.h"

#include <QWidget>
#include <cstdint>
#include <vector>

namespace rm_terminal {

// 战场数据分析(效果图 v5):控制度大数字和对拉条、四个阵地色块、堡垒一行、经济/伤害/击杀/
// 能量机关四格统计。按高度分档,每档都整块画得下:完整 → 中等(控制度一行 + 五个色块 + 统计一行)
// → 紧凑(统计挪到卡片标题栏右侧的小字里)→ 迷你 → 只剩控制度一行。
class BattleAnalysisPane : public QWidget {
public:
    explicit BattleAnalysisPane(QWidget* parent = nullptr);

    void setMetrics(const std::vector<AnalysisMetric>& metrics);
    void setFortress(const FortressStatus& status);
    void setZoneBoard(const ZoneBoard& board);
    // 我方阵营(1 红 2 蓝),决定「我方领先」怎么说、统计条哪边算我方。
    void setOwnFaction(std::uint32_t faction);

    // 只画控制度,画得大(齿轮里的「只看控制度」)。
    void setControlOnly(bool control_only);
    // 卡片标题栏右侧该写什么:面板里有统计时写后勤,没有时把统计也写上。富文本。
    QString headerText() const;
    bool compact() const;
    enum class Level { Full, Medium, Compact, Mini, Tiny };
    Level level() const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent*) override;

private:
    void paintControl(QPainter& painter, const QRectF& row, bool compact) const;
    void paintBigControl(QPainter& painter, const QRectF& area) const;
    void paintZones(QPainter& painter, const QRectF& area, bool with_fortress) const;
    void paintFortress(QPainter& painter, const QRectF& row) const;
    void paintStats(QPainter& painter, const QRectF& row, bool compact) const;

    std::vector<AnalysisMetric> metrics_;
    FortressStatus fortress_;
    ZoneBoard board_;
    std::uint32_t own_faction_ = 1;
    bool control_only_ = false;
};

}  // namespace rm_terminal
