#pragma once

#include "self_command_pane.h"

#include <QString>
#include <QWidget>
#include <functional>

class QPushButton;

namespace rm_terminal {

// 阵亡时弹出的复活框,照官方选手端:读条中显示倒计时和进度;「免费复活 [Y]」读条满了
// 才能点(或按 Y);「金币复活」随时点一下就花钱复活,金币不够时写「N 金币不足」。
// 活着时整个框藏起来。叠在主窗口正中,信息页和图传全屏页都盖得到。
class RevivePopup final : public QWidget {
    Q_OBJECT
public:
    explicit RevivePopup(QWidget* host);

    void setView(const SelfCommandView& view);
    void setLinkUp(bool up);
    void setActions(std::function<void()> confirm_free, std::function<void()> pay);
    // 最近一条复活指令到哪一步了(等待回显/已确认/未确认),空串不显示。
    void setStatus(const QString& status);

    QString titleText() const;
    QSize sizeHint() const override;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void refresh();
    void place();

    SelfCommandView view_;
    bool link_up_ = false;
    QString status_;
    std::function<void()> confirm_free_;
    std::function<void()> pay_;
    QPushButton* pay_button_ = nullptr;
    QPushButton* free_button_ = nullptr;
};

}  // namespace rm_terminal
