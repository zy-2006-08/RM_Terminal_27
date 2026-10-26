#pragma once

#include "link_health.h"

#include <QFrame>

#include <array>
#include <vector>

class QGridLayout;
class QLabel;
class QPushButton;

namespace rm_terminal {

// 上场前自检页。悬浮在主窗口里而不是单独开一个窗口:比赛机是全屏的,独立窗口可能
// 被窗口管理器压到全屏窗口后面,操作手以为它没弹出来。
class SelfCheckPanel final : public QFrame {
    Q_OBJECT
public:
    explicit SelfCheckPanel(QWidget* parent = nullptr);
    void setCheck(const SelfCheck& check);

Q_SIGNALS:
    void closeRequested();

private:
    QLabel* verdict_;
    QGridLayout* grid_;
    QPushButton* close_;
    std::vector<std::array<QLabel*, 3>> rows_;
};

}  // namespace rm_terminal
