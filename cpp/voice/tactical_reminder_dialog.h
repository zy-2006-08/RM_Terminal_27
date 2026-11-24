#pragma once

#include "tactical_reminder.h"
#include "tactical_reminder_controller.h"

#include <QDialog>
#include <QString>
#include <optional>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTableWidgetItem;

namespace rm_terminal {

// 操作手填的是「剩余 1 分 30 秒」,不是 90。两个方向都放在这里并成对测试:
// 只写一半会让界面显示的格式和它能读回的格式悄悄分叉。
QString reminder_time_text(std::int32_t remaining_sec);
std::optional<std::int32_t> parse_reminder_time(const QString& text);

// 两条提醒挨得太近时,后一条会排在前一条的两遍播报之后 —— 到点时它不会响,
// 而是迟几秒才响。这是排队的必然结果,不是缺陷,但操作手必须在保存前知道,
// 否则他会以为自己设的时间点没生效。
constexpr std::int32_t kReminderCrowdedSec = 6;
QString reminder_crowding_warning(const ReminderConfig& config);

// 赛前配置界面。持有草稿:操作手改到一半按取消必须什么都不留下,所以表格里的
// 内容在按下保存之前绝不进入控制器。
class ReminderDialog final : public QDialog {
    Q_OBJECT
public:
    explicit ReminderDialog(ReminderController* controller, QWidget* parent = nullptr);

    // 表格 → 配置。保存走的就是这条路径,测试因此能在不点按钮的情况下断言映射。
    ReminderConfig draft() const;
    void loadDraft(const ReminderConfig& config);

private Q_SLOTS:
    void onAdd();
    void onRemove();
    void onPreview();
    void onSave();
    void onMasterToggled(bool checked);
    void refreshFromController();

private:
    void applyLock(bool locked);
    void showMessage(const QString& text, bool error);
    void appendRow(const ReminderItem& item);

    // 剩余时间和开赛后时间是同一触发点的两种写法。改一列即刻换算写入另一列,
    // 保证界面上不会出现两个互相矛盾的读数。
    void mirrorTimeColumns(QTableWidgetItem* source);

    ReminderController* controller_;
    QCheckBox* master_;
    QComboBox* voice_;
    QSpinBox* rate_;
    QTableWidget* table_;
    QPushButton* add_;
    QPushButton* remove_;
    QPushButton* preview_;
    QPushButton* save_;
    QPushButton* close_;
    QLabel* status_;
    QLabel* message_;
    // 赛中锁定时 master_ 仍然可点(关闭是赛中唯一允许的写操作),而这会让
    // 程序化的状态同步触发 toggled 信号。同步期间置位,避免把界面刷新
    // 误当成操作手的一次点击。
    bool syncing_ = false;

    // 本机一个可用中文声音都没有。此时声音下拉里放的是一条占位文字,必须始终禁用 ——
    // 光靠 applyLock 的 !locked 会在赛后把这个选不出有效声音的下拉打开。
    bool voice_no_usable_ = false;
};

}
