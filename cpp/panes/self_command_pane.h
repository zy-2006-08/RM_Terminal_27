#pragma once

#include "domain.h"
#include "next_step.h"
#include "purchase.h"
#include "role_commands.h"
#include "self_command_view.h"

#include <QString>
#include <QWidget>
#include <array>
#include <functional>
#include <optional>

class QPushButton;

namespace rm_terminal
{

    // 本车指令:兑换和远程兑换发弹量、远程兑换血量、开能量机关、赛前选性能体系。
    // 重装和步兵共用,重装没有性能体系和能量机关。复活照官方选手端,阵亡时才弹出复活框
    // (revive_popup.h),发送仍走这里的限频和回显。键鼠控制的开关在状态条上,这里只显示状态。
    // 「兑换」「远程补弹」「远程回血」照官方选手端先二次确认:点了交给 setPurchaseRequest 给的
    // 回调(主程序接到确认框,purchase_popups.h),确认后由 sendPurchase 发出。
    // 画面上的「本车」面板已经删掉:主程序只把它当成发指令的对象,藏着不显示,操作都走官方选手端
    // 的按键(H、O/I、P、F、Y/F1)。按钮还在,测试靠它们验限频和回显。
    class SelfCommandPane : public QWidget
    {
        Q_OBJECT
    public:
        explicit SelfCommandPane(QWidget* parent = nullptr);

        void setView(const SelfCommandView& view);
        void setLinkUp(bool up);
        void setSender(std::function<bool(const char* topic, const std::string& payload)> sender);
        void setPurchaseRequest(std::function<void(PurchaseKind kind, std::uint32_t rounds)> request);
        // 确认框确认后发兑换指令,和复活共用 CommonCommand 的限频。买不了或没发出去返回 false。
        bool sendPurchase(const PurchaseOffer& offer);
        // 兑换到没到账的那一句(PurchaseWatch),非空时占用状态行。problem = 没到账或作废,用警告色。
        void setPurchaseText(const QString& text, bool problem);
        // 键鼠控制由主程序的 KmCapture 管,面板只在状态行里显示它。
        void setKeyboardMouseState(bool active, const QString& detail);
        // 「下一步」推荐开能量机关时,按钮上标同一个编号并发光。空串去掉。
        void setRuneBadge(const QString& badge);
        void tick(MonotonicMs now);

        QString statusText() const;
        QString contextText() const;
        // 卡片标题栏右侧的状态(富文本):「● 就绪 · 交战中」、断线、回显。
        QString headerHtml() const;
        bool preMatch() const;
        // 复活框的两颗按钮和 Y 键调这两个。条件不满足(没阵亡、读条没满、金币不够、断线)
        // 时什么都不发,返回 false。
        bool confirmRevive();
        bool payRevive();
        // 长按 F 开能量机关(官方选手端手册第 12 页)。不满足条件(不是步兵、不在比赛中、能量机关
        // 不是未激活、断线)时什么都不发,返回 false。
        bool openRune();
        // 选性能体系,P 设置面板和这里赛前的四个按钮共用。没给的那一项沿用现在的选择。
        // 不能选的时候(不是步兵、不在准备到倒计时阶段、断线)什么都不发,返回 false。
        bool choosePerformance(std::optional<std::uint32_t> shooter, std::optional<std::uint32_t> chassis);
        bool performanceOpen() const;
        // 现在该显示的选择:刚点、服务器还没报回来时是点的那个,否则是服务器报的。
        std::optional<std::uint32_t> shownShooter() const;
        std::optional<std::uint32_t> shownChassis() const;
        // 这次准备阶段在终端里选过性能体系(不管选的是不是默认那一项)。
        bool performanceChosen() const { return performance_chosen_; }
        // 最近一次选性能体系到哪一步了:「等待回显」「已确认」「未确认」;没选过时为空。
        QString performanceEcho() const;
        // 最近一条复活指令的回显(「确认复活 · 已确认」),别的指令或还没发过时是空串。复活框用。
        QString reviveEchoText() const;
        std::uint32_t exchangeRounds() const;
        std::uint32_t exchangeCost() const;

        QSize sizeHint() const override;

    protected:
        void paintEvent(QPaintEvent*) override;

    private:
        enum class Kind : std::uint32_t
        {
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
        bool sendPerformance(std::uint32_t shooter, std::uint32_t chassis);
        std::uint32_t shooterChoice() const;
        std::uint32_t chassisChoice() const;
        bool sendRune();
        void afterSend(Kind kind);
        std::uint64_t fingerprint(Kind kind) const;
        const std::array<std::uint32_t, 6>& ladder() const;

        SelfCommandView view_;
        bool link_up_ = false;
        std::function<bool(const char*, const std::string&)> sender_;
        std::function<void(PurchaseKind, std::uint32_t)> purchase_request_;
        QString purchase_text_;
        bool purchase_problem_ = false;
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
        bool performance_chosen_ = false;
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

} // namespace rm_terminal
