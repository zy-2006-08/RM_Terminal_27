#pragma once

#include "layout_profile.h"
#include "ui_profile.h"

#include <QFrame>
#include <QStringList>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

class QButtonGroup;
class QCheckBox;
class QLabel;
class QListWidget;
class QPushButton;
class QSlider;

namespace rm_terminal {

class UiProfileController;

// 界面设置页里点齿轮弹出来的细项(效果图 12)。改了马上存;改之前发 aboutToChange,
// 设置页据此记一步「撤销」。
class PaneDetails final : public QFrame {
    Q_OBJECT
public:
    enum class Page { Map, EnemyRoster, NextStep, Analysis, OwnVideo, Video, Alerts };

    explicit PaneDetails(UiProfileController* controller, QWidget* parent = nullptr);

    // 这块面板有没有细项。video_cell:图传单独占了一块(不在「本车」里)。
    static std::optional<Page> page_for(PaneId pane, bool video_cell, bool video_embeddable);
    void showPage(Page page);
    Page page() const { return page_; }
    void setConfigDisabledAlerts(const QStringList& kinds);
    void refresh();

Q_SIGNALS:
    void aboutToChange();
    // 「下一步」选了「关掉」:从画面里拿掉它,由设置页来做(能撤销)。
    void removePaneRequested(PaneId pane);
    // 图传「放回『本车』里」。
    void videoBackRequested();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void editOptions(const std::function<void(UiOptions&)>& edit);

    UiProfileController* controller_;
    Page page_ = Page::Map;
    QStringList config_disabled_;
    QLabel* title_ = nullptr;
    // 每块面板一页,只显示当前那页。不用 QStackedWidget:它按最高的那页算高度,短的页下面会空一大截。
    std::vector<QWidget*> pages_;
    QSlider* map_background_ = nullptr;
    QLabel* map_background_value_ = nullptr;
    QCheckBox* map_enemies_ = nullptr;
    QCheckBox* map_marks_ = nullptr;
    QCheckBox* low_hp_pulse_ = nullptr;
    QButtonGroup* next_mode_ = nullptr;
    QButtonGroup* next_count_ = nullptr;
    QButtonGroup* next_size_ = nullptr;
    QCheckBox* next_voice_ = nullptr;
    QSlider* next_dwell_ = nullptr;
    QLabel* next_dwell_value_ = nullptr;
    QButtonGroup* analysis_mode_ = nullptr;
    QCheckBox* own_video_ = nullptr;
    QPushButton* video_back_ = nullptr;
    QCheckBox* alert_voice_ = nullptr;
    std::vector<std::pair<QString, QCheckBox*>> alert_kinds_;
};

}  // namespace rm_terminal
