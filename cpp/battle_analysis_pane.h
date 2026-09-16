#pragma once

#include "battle_analysis.h"

#include <QWidget>
#include <vector>

namespace rm_terminal {

class BattleAnalysisPane : public QWidget {
public:
    explicit BattleAnalysisPane(QWidget* parent = nullptr);

    void setMetrics(const std::vector<AnalysisMetric>& metrics);
    void setFortressHold(FortressHold hold);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent*) override;

private:
    std::vector<AnalysisMetric> metrics_;
    FortressHold hold_ = FortressHold::Unknown;
};

}
