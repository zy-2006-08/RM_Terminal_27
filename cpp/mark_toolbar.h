#pragma once

#include "map_marks.h"

#include <QWidget>

class QButtonGroup;
class QComboBox;
class QLabel;

namespace rm_terminal {

// 指挥手地图下面的标记工具条:选标记类型和发送范围,点地图时由 Dashboard 取 draft()
// 填上坐标发出去。只有指挥手布局里有它。
class MarkToolbar final : public QWidget {
public:
    explicit MarkToolbar(QWidget* parent = nullptr);

    // 当前选择的类型/范围/字母,坐标和目标由调用方填。own_faction 决定收件人编号(蓝方 +100)。
    MarkDraft draft(std::uint32_t own_faction) const;
    void setStatus(const QString& text, bool error);
    void setLinkUp(bool up);
    QString statusText() const;

private:
    QButtonGroup* kinds_;
    QComboBox* letter_;
    QComboBox* scope_;
    QLabel* status_;
    bool link_up_ = false;
};

}  // namespace rm_terminal
