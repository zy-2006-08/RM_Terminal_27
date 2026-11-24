#pragma once

#include "map_marks.h"

#include <QWidget>
#include <array>

class QButtonGroup;
class QLabel;
class QPushButton;
class QResizeEvent;

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

    // 照官方选手端的「发送地图标记」:默认不转发给哨兵(规则 5.7.2 每条要花钱),
    // 开启要二次确认并看到单价,关闭不用确认。
    void setSentryAutomatic(bool automatic);
    bool sentryForwarding() const { return forward_sentry_; }
    bool sentryArmed() const { return arm_sentry_; }

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void dressSentry();
    void clickSentry();

    QButtonGroup* kinds_;
    QButtonGroup* scopes_;
    QPushButton* letter_;
    QPushButton* sentry_;
    QLabel* status_;
    bool link_up_ = false;
    bool automatic_ = true;
    bool forward_sentry_ = false;
    bool arm_sentry_ = false;
    std::uint32_t letter_index_ = 0;
    int chip_height_ = 0;
    int chip_px_ = 0;
};

}  // namespace rm_terminal
