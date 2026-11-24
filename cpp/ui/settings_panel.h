#pragma once

#include "terminal_settings.h"

#include <QFrame>
#include <QString>
#include <cstdint>
#include <functional>
#include <optional>

class QLabel;
class QPushButton;
class QSlider;

namespace rm_terminal {

// P 设置面板这一帧要显示的东西,主程序每帧填。
struct SettingsView {
    QString seat;                        // 「红方 3 号 步兵」
    QString login;                       // 「已登录 · 服务器核对过编号」
    bool login_problem = false;          // 没连上服务器、编号对不上:红字
    std::optional<std::uint32_t> stage;  // 0 未开始 1 准备 2 自检 3 倒计时 4 比赛中 5 结算
    bool has_performance = false;        // 只有步兵有性能体系
    bool performance_open = false;       // 准备、自检、倒计时阶段,而且连着服务器
    std::optional<std::uint32_t> shooter, chassis;  // 1 冷却优先 / 血量优先,2 爆发优先 / 功率优先
    QString performance_note;            // 性能设置下面那一行
    bool performance_warn = false;       // 还是默认、开赛就锁定:用警告色
    bool fullscreen = false;
    bool volume_adjustable = true;
    QString video;                       // 底部的图传状态
};

// 照官方选手端的 P 设置面板(2026 选手端界面说明手册第 21 页):按 P 打开,再按 P 关闭;
// 分块和官方一样:登录、一键重置、性能设置、图传、硬件设置 / 图形设置、UI 设置 / 操作设置、
// 底部图传状态。终端做不到的几项照样放在原来的位置,灰着并写原因,按官方的位置找得到。
class SettingsPanel final : public QFrame {
    Q_OBJECT
public:
    explicit SettingsPanel(QWidget* host);

    void setSettings(const TerminalSettings& settings);
    const TerminalSettings& settings() const { return settings_; }
    void setView(const SettingsView& view);
    const SettingsView& view() const { return view_; }

    void setOpen(bool open);
    bool isOpen() const { return !isHidden(); }
    void toggle() { setOpen(!isOpen()); }

    using SettingsChanged = std::function<void(const TerminalSettings&)>;
    using PerformancePicked =
        std::function<void(std::optional<std::uint32_t> shooter, std::optional<std::uint32_t> chassis)>;
    void setActions(SettingsChanged changed, PerformancePicked performance, std::function<void(bool)> display,
                    std::function<void()> reset);
    // 一键重置只能在自由阶段和三分钟准备阶段用,和官方一样。
    bool resetAllowed() const;

    QSize sizeHint() const override;

Q_SIGNALS:
    void openChanged(bool open);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void refresh();
    void place();
    void emitSettings();

    TerminalSettings settings_;
    SettingsView view_;
    SettingsChanged changed_;
    PerformancePicked performance_;
    std::function<void(bool)> display_;
    std::function<void()> reset_;

    QLabel* seat_ = nullptr;
    QLabel* login_ = nullptr;
    QPushButton* reset_button_ = nullptr;
    QPushButton* chassis_hp_ = nullptr;
    QPushButton* chassis_power_ = nullptr;
    QPushButton* shooter_cooling_ = nullptr;
    QPushButton* shooter_burst_ = nullptr;
    QLabel* performance_note_ = nullptr;
    QPushButton* video_forward_ = nullptr;
    QPushButton* video_reverse_ = nullptr;
    QSlider* sensitivity_ = nullptr;
    QLabel* sensitivity_value_ = nullptr;
    QSlider* volume_ = nullptr;
    QLabel* volume_value_ = nullptr;
    QPushButton* fullscreen_ = nullptr;
    QPushButton* windowed_ = nullptr;
    QLabel* video_status_ = nullptr;
};

}  // namespace rm_terminal
