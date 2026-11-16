#pragma once

#include "buff_ledger.h"
#include "domain.h"
#include "role_commands.h"

#include <QString>
#include <QWidget>
#include <array>
#include <functional>
#include <optional>

class QPushButton;
class QResizeEvent;

namespace rm_terminal {

struct SentryView {
    std::optional<std::uint32_t> posture;  // 1 进攻 2 防御 3 移动
    std::optional<bool> weakened, powered;
    std::optional<std::uint32_t> hp, max_hp;
    std::optional<std::int32_t> bullets;
    std::optional<std::uint32_t> economy;
    std::optional<SentryCtrlOutcome> result;
    // 按钮上的真实总价要这两项(sentry_cost)。比赛中才有剩余秒数:远程回血和金币复活
    // 按它涨价,赛外没有价可报,按钮就不标价。
    std::optional<std::int32_t> remaining_sec;
    // 规则 5.7.2:自动哨兵每次干预加 50,半自动不加。协议没发时按规则默认值「自动」。
    bool automatic = true;
    // 姿态的实际效果(表 5-28)、这个姿态本局累计多久(满 3 分钟效果下降)、强化还能用几秒(每种最多 15 秒)。
    // 后两项从终端打开时数起。
    QString posture_effect;
    std::optional<std::int32_t> posture_held_sec;
    std::optional<std::int32_t> powered_left_sec;
};

SentryView sentry_view(const Snapshot& snapshot, const BuffBoard* buffs = nullptr);

QString sentry_posture_text(std::uint32_t posture);
QString sentry_command_text(std::uint32_t command_id);

class SentryPane : public QWidget {
    Q_OBJECT
public:
    explicit SentryPane(QWidget* parent = nullptr);

    void setView(const SentryView& view);
    void setLinkUp(bool up);
    void setSender(std::function<bool(const char* topic, const std::string& payload)> sender);
    void tick(MonotonicMs now);

    QString statusText() const;
    // 卡片标题右侧的小字。六颗姿态按钮的单价都一样,在这里说一次,不在每颗按钮上重复。
    QString headerText() const;
    // 最近一条 SentryCtrlResult 的中文描述。和 statusText 分开:状态行讲「这次发送
    // 到哪一步了」,这一条讲「上一条指令的结果」,混在一行会在重发时自相矛盾。
    QString lastResultText() const;
    bool buttonsEnabled() const;
    bool postureCoolingDown() const;

    // 二次确认:第一次点亮按钮并报价,约 3 秒内再点同一颗才真的发。
    static constexpr MonotonicMs kArmedMs = 3000;
    std::optional<std::uint32_t> armedCommand() const { return armed_; }
    // 按钮上的总价。赛外、或协议没给涨价依据时为空,按钮不标价。
    std::optional<std::uint32_t> priceOf(std::uint32_t command_id) const;

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent*) override;
    // 按钮字号按按钮的实际高度算,而高度只有布局跑完才知道,所以每次改尺寸都要重算一遍。
    void resizeEvent(QResizeEvent* event) override;

private:
    // 成功结果只闪一下就收走,不常驻。
    static constexpr MonotonicMs kResultFlashMs = 4000;
    struct Banner {
        QString text;
        QColor ink;
    };
    Banner bannerNow() const;

    void refreshButtons();
    // 一次点击:没武装过就武装这一颗,已武装同一颗就发送。别的按钮一律先解除武装。
    void arm(std::uint32_t command_id);
    void disarm();
    void send(std::uint32_t command_id);
    void dress(QPushButton* button, std::uint32_t command_id, const QString& name);
    std::uint32_t lastPostureCommand() const;
    QPushButton* buttonFor(std::uint32_t command_id) const;

    SentryView view_;
    bool link_up_ = false;
    std::function<bool(const char*, const std::string&)> sender_;

    MonotonicMs now_ = 0;
    CommandGate gate_;
    std::optional<std::uint32_t> armed_;
    MonotonicMs armed_at_ = 0;
    EchoWatch echo_;
    EchoResult echo_result_;
    // 规则手册 5.7.2:姿态切换冷却 5 秒。从「服务器确认成功」那一刻起算,
    // 而不是从点击起算 —— 失败的切换不该把按钮锁住。
    std::optional<MonotonicMs> posture_cooldown_from_;
    std::uint32_t last_seen_result_id_ = 0;
    std::optional<MonotonicMs> last_seen_result_at_;

    // 自绘区的占位控件:布局用它分高度,三格和血量画在它的矩形里。
    QWidget* canvas_ = nullptr;
    std::array<QPushButton*, 6> postures_{};
    QPushButton* ammo_ = nullptr;
    QPushButton* heal_ = nullptr;
    QPushButton* revive_ = nullptr;
    QPushButton* gold_revive_ = nullptr;
};

}  // namespace rm_terminal
