#pragma once

#include "domain.h"
#include "role_commands.h"

#include <QString>
#include <QWidget>
#include <array>
#include <functional>
#include <optional>

class QPushButton;

namespace rm_terminal {

// 重装「装配」面板的输入。全部用 optional:协议没发和真实的 0 必须分开,
// 缺数据一律画成「--」并写明在等哪条消息,画成 0 会被读成「难度上限是 0」。
struct AssemblyView {
    std::optional<std::uint32_t> maximum_difficulty;  // TechCoreMotionStateSync
    std::optional<std::uint32_t> tech_level;          // GlobalLogisticsStatus
    std::optional<bool> modular_42mm;                 // RobotModuleStatus
    std::optional<std::uint32_t> basic_state;         // 1 初始位置 2 运动中 3 已到达
    std::optional<bool> putin, move, rotate;
    std::optional<std::uint32_t> remain_time_step, remain_time_all;
    std::optional<std::uint32_t> enemy_core_status;  // 0 无 1 对方非四级 2 对方四级
    // 最近一条 Event 15 的文字(已是中文)和收到时刻。时刻用于回显判定。
    QString last_result;
    std::optional<MonotonicMs> last_result_at;
};

AssemblyView assembly_view(const Snapshot& snapshot);

class AssemblyPane : public QWidget {
    Q_OBJECT
public:
    explicit AssemblyPane(QWidget* parent = nullptr);

    void setView(const AssemblyView& view);
    void setLinkUp(bool up);
    void setSender(std::function<bool(const char* topic, const std::string& payload)> sender);
    // 机械臂链路单独一条:它不来自 Snapshot(走 0x0309 控制器链路),所以不进
    // AssemblyView,由主程序另外喂进来。
    void setArm(std::optional<double> fps, std::optional<std::array<float, 6>> joints);
    void tick(MonotonicMs now);

    QString statusText() const;
    bool buttonsEnabled() const;
    std::uint32_t selectedDifficulty() const { return difficulty_; }

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent*) override;

private:
    void refreshButtons();
    void send(std::uint32_t operation, std::uint32_t difficulty);
    std::uint64_t echoFingerprint() const;

    AssemblyView view_;
    bool link_up_ = false;
    std::function<bool(const char*, const std::string&)> sender_;
    std::optional<double> arm_fps_;
    std::optional<std::array<float, 6>> arm_joints_;

    MonotonicMs now_ = 0;
    std::uint32_t difficulty_ = 1;
    CommandGate gate_;
    HoldGate cancel_hold_;
    EchoWatch echo_;
    EchoResult echo_result_;

    QPushButton* start_ = nullptr;
    QPushButton* confirm_ = nullptr;
    QPushButton* cancel_ = nullptr;
    QPushButton* minus_ = nullptr;
    QPushButton* plus_ = nullptr;
};

}  // namespace rm_terminal
