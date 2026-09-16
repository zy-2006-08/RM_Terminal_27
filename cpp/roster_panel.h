#pragma once

#include "roster_pane.h"

#include <QQuickWidget>
#include <QVariantList>
#include <vector>

namespace rm_terminal {

// RosterEntry → QML 属性。导出为自由函数以便单测直接断言,不必构造 QQuickWidget。
QVariantList roster_entry_models(const std::vector<RosterEntry>& entries);

// 花名册面板的 QML 宿主。取代原先 352 行的 QWidget 自绘:自绘做不出顶栏那套
// 渐变/圆角/描边质感,两套渲染体系并存正是上下割裂的原因。
class RosterPanel final : public QQuickWidget {
    Q_OBJECT
public:
    // `is_ally` 只决定这一列站在布局的哪边(左=我方),阵营配色由 setFactionKnown 决定。
    explicit RosterPanel(bool is_ally, QWidget* parent = nullptr);

    // `known` 为假时标题退回「红方/蓝方」:没有自机就无法断定哪边是我方。
    void setFactionKnown(bool known, bool is_blue);
    void setEntries(const std::vector<RosterEntry>& entries);

    std::size_t rowCount() const { return count_; }

private:
    void applyFaction();

    bool is_ally_;
    bool is_blue_ = false;
    bool faction_known_ = false;
    std::size_t count_ = 0;
};

}  // namespace rm_terminal
