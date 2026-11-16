#pragma once

#include "domain.h"
#include "role_commands.h"

#include <QString>
#include <QWidget>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace rm_terminal {

struct DartView {
    std::optional<std::uint32_t> gate;       // 0 关闭 1 开启中 2 已开启
    std::optional<std::uint32_t> target_id;  // 1..5
    std::optional<std::int32_t> match_sec;   // 比赛进行了几秒;不在比赛中为空
    // 本局我方飞镖命中几次(Event 9,Snapshot.dart_hits)。协议不报发射和脱靶,面板拿它当
    // 用掉了几枚:模拟器里每发都命中;真比赛里打偏的那枚看不到,剩余枚数只会多算不会少算。
    std::uint32_t hits = 0;
};

DartView dart_view(const Snapshot& snapshot);

// 五个目标的中文名(协议 2.2.29 target_id 1..5)。
QString dart_target_text(std::uint32_t target_id);

// 协议不发开闸次数、关闸倒计时和冷却,终端看闸门状态什么时候变,按规则 5.7.3 自己算。
// 所以次数从终端打开时数起;比赛时间往回走(换局、回放跳转)或离开比赛就清零。
struct DartGateLog {
    int openings = 0;
    std::optional<std::int32_t> opened_sec;
    std::optional<std::int32_t> closed_sec;
    std::optional<std::uint32_t> last_gate;
    std::optional<std::int32_t> last_sec;
};

// 官方选手端 F 键飞镖界面的几步。Spent:本局 4 枚飞镖都已命中,不再开闸、不再发射。
enum class DartStep { NoData, Ready, Asking, Opening, Open, Cooldown, NextChance, Exhausted, Spent };

struct DartPrompt {
    DartStep step = DartStep::NoData;
    // Open:离关闸还有几秒;Cooldown:冷却还剩几秒;NextChance:离下一次开闸机会还有几秒。
    std::optional<std::int32_t> seconds;
};

class DartPane : public QWidget {
    Q_OBJECT
public:
    explicit DartPane(QWidget* parent = nullptr);

    void setView(const DartView& view);
    void setLinkUp(bool up);
    void setSender(std::function<bool(const char* topic, const std::string& payload)> sender);
    void tick(MonotonicMs now);

    // 官方选手端的飞镖键位:J 切换目标,F 开启飞镖闸门,Y 确定,N 取消,L 发射飞镖。
    void cycleTarget();
    void requestOpen();
    void confirmOpen();
    void cancelOpen();
    void launch();

    DartPrompt prompt() const;

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent*) override;

private:
    bool live() const;
    bool send(std::uint32_t target_id, bool open, bool launch_confirm);

    DartView view_;
    DartGateLog log_;
    bool link_up_ = false;
    bool asking_ = false;
    std::function<bool(const char*, const std::string&)> sender_;

    MonotonicMs now_ = 0;
    CommandGate gate_;
};

}  // namespace rm_terminal
