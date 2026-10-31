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

struct SelfCommandView {
    // false = 重装:42mm、没有性能体系、不能开能量机关(规则手册 5.5.2、5.6.3)。
    bool infantry = true;
    std::optional<std::uint32_t> stage;      // 0 未开始 1 准备 2 自检 3 倒计时 4 比赛中 5 结算
    std::optional<std::int32_t> remaining;   // 比赛中:本局剩余秒数
    std::optional<std::uint32_t> economy;
    std::optional<std::uint32_t> ammo;       // 剩余允许发弹量
    std::optional<std::uint32_t> hp, max_hp, level;
    std::optional<bool> out_of_combat, can_remote_heal, can_remote_ammo;
    std::optional<bool> dead;
    std::optional<std::uint32_t> revive_total, revive_current, revive_cost;
    std::optional<bool> can_free_revive, can_pay_revive;
    std::optional<std::uint32_t> rune_status, rune_arms;
    std::optional<std::uint32_t> shooter, chassis;
};

SelfCommandView self_command_view(const Snapshot& snapshot);

// 规则手册表 5-7:50 + ROUNDUP((420 - 剩余秒数) / 60 × 20)。
std::uint32_t remote_heal_cost(std::int32_t remaining_sec);

// 本车指令:兑换和远程兑换发弹量、远程兑换血量、开能量机关、赛前选性能体系。
// 重装和步兵共用,重装没有性能体系和能量机关。复活照官方选手端,阵亡时才弹出复活框
// (revive_popup.h),发送仍走这里的限频和回显。键鼠控制的开关在状态条上,这里只显示状态。
class SelfCommandPane : public QWidget {
    Q_OBJECT
public:
    explicit SelfCommandPane(QWidget* parent = nullptr);

    void setView(const SelfCommandView& view);
    void setLinkUp(bool up);
    void setSender(std::function<bool(const char* topic, const std::string& payload)> sender);
    // 键鼠控制由主程序的 KmCapture 管,面板只在状态行里显示它。
    void setKeyboardMouseState(bool active, const QString& detail);
    void tick(MonotonicMs now);

    QString statusText() const;
    QString contextText() const;
    bool preMatch() const;
    // 复活框的两颗按钮和 Y 键调这两个。条件不满足(没阵亡、读条没满、金币不够、断线)
    // 时什么都不发,返回 false。
    bool confirmRevive();
    bool payRevive();
    // 最近一条复活指令的回显(「确认复活 · 已确认」),别的指令或还没发过时是空串。复活框用。
    QString reviveEchoText() const;
    std::uint32_t exchangeRounds() const;
    std::uint32_t exchangeCost() const;

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent*) override;

private:
    enum class Kind : std::uint32_t {
        None,
        Exchange,
        RemoteAmmo,
        RemoteHeal,
        ConfirmRevive,
        PayRevive,
        Rune,
        Performance
    };

    void refreshButtons();
    void placeRune();
    bool sendCommon(Kind kind, std::uint32_t cmd_type, std::uint32_t param);
    void sendPerformance(std::uint32_t shooter, std::uint32_t chassis);
    std::uint32_t shooterChoice() const;
    std::uint32_t chassisChoice() const;
    void sendRune();
    void afterSend(Kind kind);
    std::uint64_t fingerprint(Kind kind) const;
    const std::array<std::uint32_t, 6>& ladder() const;

    SelfCommandView view_;
    bool link_up_ = false;
    std::function<bool(const char*, const std::string&)> sender_;
    bool km_active_ = false;
    QString km_detail_;

    MonotonicMs now_ = 0;
    CommandGate common_gate_;
    CommandGate rune_gate_;
    CommandGate performance_gate_;
    HoldGate rune_hold_;
    EchoWatch echo_;
    EchoResult echo_result_;
    Kind last_kind_ = Kind::None;
    // 刚发出去、同步(1Hz)还没回来的性能体系选择。连点两项时第二条要带上第一条的
    // 选择,否则会用旧的同步值把刚选的那一项改回去。同步对上或回显超时就清掉。
    std::optional<std::uint32_t> wanted_shooter_, wanted_chassis_;
    std::size_t rounds_index_ = 2;

    QWidget* pre_page_ = nullptr;
    QWidget* match_page_ = nullptr;
    QPushButton* chassis_hp_ = nullptr;
    QPushButton* chassis_power_ = nullptr;
    QPushButton* shooter_cooling_ = nullptr;
    QPushButton* shooter_burst_ = nullptr;
    QPushButton* minus_ = nullptr;
    QPushButton* exchange_ = nullptr;
    QPushButton* plus_ = nullptr;
    QPushButton* remote_ammo_ = nullptr;
    QPushButton* remote_heal_ = nullptr;
    QPushButton* rune_ = nullptr;
};

}  // namespace rm_terminal
