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

struct SentryView {
    std::optional<std::uint32_t> posture;  // 1 进攻 2 防御 3 移动
    std::optional<bool> weakened, powered;
    std::optional<std::uint32_t> hp, max_hp;
    std::optional<std::int32_t> bullets;
    std::optional<std::uint32_t> economy;
    std::optional<SentryCtrlOutcome> result;
};

SentryView sentry_view(const Snapshot& snapshot);

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
    // 最近一条 SentryCtrlResult 的中文描述。和 statusText 分开:状态行讲「这次发送
    // 到哪一步了」,这一条讲「上一条指令的结果」,混在一行会在重发时自相矛盾。
    QString lastResultText() const;
    bool buttonsEnabled() const;
    bool postureCoolingDown() const;

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent*) override;

private:
    void refreshButtons();
    void send(std::uint32_t command_id);
    std::uint32_t lastPostureCommand() const;

    SentryView view_;
    bool link_up_ = false;
    std::function<bool(const char*, const std::string&)> sender_;

    MonotonicMs now_ = 0;
    CommandGate gate_;
    HoldGate gold_hold_;
    EchoWatch echo_;
    EchoResult echo_result_;
    // 规则手册 5.7.2:姿态切换冷却 5 秒。从「服务器确认成功」那一刻起算,
    // 而不是从点击起算 —— 失败的切换不该把按钮锁住。
    std::optional<MonotonicMs> posture_cooldown_from_;
    std::uint32_t last_seen_result_id_ = 0;
    std::optional<MonotonicMs> last_seen_result_at_;

    std::array<QPushButton*, 6> postures_{};
    QPushButton* ammo_ = nullptr;
    QPushButton* heal_ = nullptr;
    QPushButton* revive_ = nullptr;
    QPushButton* gold_revive_ = nullptr;
};

}  // namespace rm_terminal
