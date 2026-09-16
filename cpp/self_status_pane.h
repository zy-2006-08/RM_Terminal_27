#pragma once

#include "domain.h"
#include "roster_pane.h"

#include <QString>
#include <QWidget>
#include <optional>

namespace rm_terminal {

// 自机作战状态。存在的理由是数据不对称:热量、弹速、允许发弹量、经济、底盘功率
// 和自瞄状态都只有自机的 RobotDynamicStatus / RobotTelemetry 携带,协议不下发
// 他机的这些字段。花名册按编号平铺十台,放这些格子会有九台永远是 "--"。
std::optional<RosterEntry> self_entry(const Snapshot& snapshot);

class SelfStatusPane final : public QWidget {
    Q_OBJECT
public:
    explicit SelfStatusPane(QWidget* parent = nullptr);

    void setEntry(const std::optional<RosterEntry>& entry);

    bool hasEntry() const { return entry_.has_value(); }

protected:
    void paintEvent(QPaintEvent* event) override;
    QSize sizeHint() const override;

private:
    std::optional<RosterEntry> entry_;
};

}  // namespace rm_terminal
