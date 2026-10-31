#pragma once

#include "domain.h"
#include "role_commands.h"

#include <QString>
#include <QWidget>
#include <array>
#include <functional>
#include <optional>
#include <vector>

class QPushButton;

namespace rm_terminal {

struct DartView {
    std::optional<std::uint32_t> gate;       // 0 关闭 1 开启中 2 已开启
    std::optional<std::uint32_t> target_id;  // 1..5
    // Event 9(飞镖命中)和 Event 10(敌方闸门开启)的最近两条,文字已是中文。
    std::vector<QString> event_lines;
};

DartView dart_view(const Snapshot& snapshot);

// 五个目标的中文名(协议 2.2.29 target_id 1..5)。
QString dart_target_text(std::uint32_t target_id);

class DartPane : public QWidget {
    Q_OBJECT
public:
    explicit DartPane(QWidget* parent = nullptr);

    void setView(const DartView& view);
    void setLinkUp(bool up);
    void setSender(std::function<bool(const char* topic, const std::string& payload)> sender);
    void tick(MonotonicMs now);

    QString statusText() const;
    bool buttonsEnabled() const;

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent*) override;

private:
    void refreshButtons();
    void send(std::uint32_t target_id, bool open, bool launch_confirm, std::uint32_t tag);
    std::uint64_t echoFingerprint() const;

    DartView view_;
    bool link_up_ = false;
    std::function<bool(const char*, const std::string&)> sender_;

    MonotonicMs now_ = 0;
    CommandGate gate_;
    HoldGate launch_hold_;
    EchoWatch echo_;
    EchoResult echo_result_;

    std::array<QPushButton*, 5> targets_{};
    QPushButton* open_ = nullptr;
    QPushButton* launch_ = nullptr;
};

}  // namespace rm_terminal
