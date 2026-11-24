#pragma once

#include "domain.h"
#include "roster_pane.h"

#include <QString>
#include <QWidget>
#include <optional>

namespace rm_terminal {

// 自机作战状态。存在的理由是数据不对称:热量、弹速、允许发弹量、缓冲能量、等级只有
// 自机的 RobotDynamicStatus / RobotStaticStatus 携带,协议不下发他机的这些字段。
// self_entry() 在 roster_pane.h。
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
