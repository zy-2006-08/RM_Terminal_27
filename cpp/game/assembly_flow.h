#pragma once

#include "domain.h"
#include "role_commands.h"

#include <QObject>
#include <QString>
#include <QWidget>
#include <array>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rm_terminal {

// 重装装配。装配是操作手拿真的自定义控制器做的,终端只发三条指令(协议 2.2.21):开始兑换(带难度)、
// 确认装配、取消装配。照官方选手端(2026 界面手册第 14、15 页):按 H 打开的框里选难度,1–4 选、
// Enter 开始;装配中屏幕下方正中显示进度,Y 确认、L 退出;结果和失败原因来自 Event 15。
// 终端收不到「车在不在装配区」(只在发给车的 0x0209 里),所以开始以后看科技核心动没动来判断。

// 输入全部用 optional:协议没发和真实的 0 必须分开。
struct AssemblyView {
    std::optional<std::uint32_t> maximum_difficulty;  // TechCoreMotionStateSync 1:当前可选的最高难度
    std::optional<std::uint32_t> tech_level;          // GlobalLogisticsStatus:己方已经装成的最高一级
    std::optional<bool> modular_42mm;                 // RobotModuleStatus 11:42mm 安装权限
    std::optional<std::uint32_t> basic_state;         // 1 初始位置 2 运动中 3 已到达
    std::optional<bool> putin, move, rotate;
    std::optional<std::uint32_t> remain_time_step, remain_time_all;
    std::optional<std::uint32_t> enemy_core_status;   // 0 无 1 对方装非四级 2 对方装四级(己方四级可选时才有效)
    // 最近一条 Event 15 的文字(已是中文)、收到时刻和级别:1 成功,0 主动退出,2 失败。
    QString last_result;
    std::optional<MonotonicMs> last_result_at;
    std::uint32_t last_result_level = 0;
    // 最近一条 Event 14(重装战亡临时激活,进入强制退出缓冲期)的收到时刻。
    std::optional<MonotonicMs> buffer_at;
};

AssemblyView assembly_view(const Snapshot& snapshot);

// 规则手册表 5-10:一级只要放入,二级放入、平移,三级和四级放入、平移、旋转。
int assembly_step_count(std::uint32_t level);
QString assembly_level_name(std::uint32_t level);  // 「一级」…「四级」
// 规则手册表 5-12 的收益。first = 这一级还没装成过。
QString assembly_reward(std::uint32_t level, bool first);

// H 框里「装配难度」那一排。只有重装有。
struct AssemblyRow {
    bool shown = false;
    std::array<bool, 4> selectable{};
    std::optional<std::uint32_t> selected;
    bool blocked = false;  // 整排都选不了,note 写原因
    QString note;          // 选中那一级的收益,或者为什么选不了
};
bool operator==(const AssemblyRow& a, const AssemblyRow& b);
inline bool operator!=(const AssemblyRow& a, const AssemblyRow& b) { return !(a == b); }

// 屏幕下方正中的装配进度。visible = false 时不画。
struct AssemblyProgressView {
    enum class Tone { Normal, Good, Bad };
    bool visible = false;
    QString timer;   // 「装配倒计时 43s · 本步还剩 4s」「缓冲还剩 12s」
    QString title;   // 「运动中…」「请装配」「确认装配前请移出装配机构」「二级科技核心装配成功」
    QString detail;  // 「前往装配点」「还差：平移」「要站在装配区上…」
    std::vector<std::pair<QString, bool>> steps;  // 放入 / 平移 / 旋转,做完的为 true
    bool keys = false;         // 画「Y 确认」「L 退出装配」
    bool confirm_key = true;   // 临时激活时只能退出,不画 Y
    bool confirm_lit = false;
    QString footer;  // 「二级难度」
    Tone tone = Tone::Normal;
};
bool operator==(const AssemblyProgressView& a, const AssemblyProgressView& b);
inline bool operator!=(const AssemblyProgressView& a, const AssemblyProgressView& b) { return !(a == b); }

struct AssemblySend {
    std::uint32_t operation = 0;  // 0 开始兑换 1 确认装配 2 取消装配
    std::uint32_t difficulty = 0;
};

// 不碰界面的那一半:按键进来,算出要发什么、框里和进度条上写什么。单独拿出来好测。
class AssemblyState {
public:
    struct Inputs {
        AssemblyView view;
        bool link_up = false;
        bool in_match = false;  // 比赛阶段 4:装配只在比赛中有
        bool dead = false;
    };
    struct Key {
        bool consumed = false;  // 这一下按键归装配,不再交给别的(Y 不落到复活上)
        std::optional<AssemblySend> send;
    };

    void update(const Inputs& inputs, MonotonicMs now);
    // 终端开始的这次装配:科技核心在动或已经到位,而且还没出结果。
    bool active() const;
    // 按了 Enter、还没看到科技核心动起来。
    bool waiting() const { return pending_.has_value(); }
    // L 有用:等开始、装配中,或者失败了科技核心还没回初始位置(规则手册 5.4.3:战亡后要手动取消)。
    bool canExit() const { return started_at_.has_value() || pending_.has_value(); }
    AssemblyRow row() const;
    AssemblyProgressView progress() const;

    // H 打开框时调:默认选中下一级(还没装成的最低一级)。
    void boxOpened();
    void choose(std::uint32_t level);
    // Enter。能开始就给出要发的指令,发出去以后调 sent()。
    std::optional<AssemblySend> start();
    // Y:装配中归装配;步骤都做完才给出确认指令,没做完写还差哪步。阵亡时 Y 留给复活。
    Key confirm();
    // L:canExit() 时给出取消指令。
    Key exit();
    // 装配中按了 H:不开框,进度条上写怎么退出。
    void busy();
    void sent(const AssemblySend& send);

private:
    struct Pending {
        std::uint32_t level = 0;
        MonotonicMs at = 0;
    };
    struct Result {
        QString text;
        std::uint32_t level_code = 0;
        std::optional<std::uint32_t> level;
        MonotonicMs until = 0;
    };

    QString blockReason() const;
    bool selectable(std::uint32_t level) const;
    std::optional<std::uint32_t> recommended() const;
    int stepCount() const;
    std::vector<std::pair<QString, bool>> steps() const;
    bool stepsDone() const;
    void hint(const QString& text, MonotonicMs ms);
    void note(const QString& text);

    Inputs in_;
    MonotonicMs now_ = 0;
    bool primed_ = false;
    MonotonicMs seen_result_at_ = -1;
    std::optional<std::uint32_t> selected_;
    std::optional<Pending> pending_;
    std::optional<std::uint32_t> level_;
    std::optional<MonotonicMs> started_at_;
    // 终端开始的那次刚结束(科技核心回到初始位置):稍晚到的结果还记在它头上。
    std::optional<std::uint32_t> ended_level_;
    MonotonicMs ended_until_ = 0;
    bool finished_ = false;
    std::optional<Result> result_;
    QString hint_;
    MonotonicMs hint_until_ = 0;
    MonotonicMs not_started_until_ = 0;
    QString note_;
    MonotonicMs note_until_ = 0;
};

// 进度条本身:只画 AssemblyProgressView,不管逻辑。
class AssemblyProgress final : public QWidget {
    Q_OBJECT
public:
    explicit AssemblyProgress(QWidget* host);
    void setView(const AssemblyProgressView& view);
    const AssemblyProgressView& view() const { return view_; }
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    AssemblyProgressView view_;
};

// 主程序用的这一层:把 AssemblyState 接上发送、进度条和窗口位置。
class AssemblyFlow final : public QObject {
    Q_OBJECT
public:
    using Sender = std::function<bool(const char* topic, const std::string& payload)>;

    explicit AssemblyFlow(QWidget* host);

    void setSender(Sender sender);
    // 远程兑换框(关着时也可能露出底下那条结果):进度条摆在它上面,不压住它。
    void setNeighbor(QWidget* neighbor);
    void update(const AssemblyState::Inputs& inputs, MonotonicMs now);

    bool active() const { return state_.active(); }
    bool canExit() const { return state_.canExit(); }
    AssemblyRow row() const { return state_.row(); }
    const AssemblyState& state() const { return state_; }
    AssemblyProgress* progressWidget() const { return progress_; }

    void boxOpened();
    void choose(std::uint32_t level);
    // Enter:指令发出去了返回 true,主程序把框关掉。
    bool start();
    // Y:这一下归装配时返回 true。
    bool confirm();
    // L:这一下归装配时返回 true。
    bool exit();
    void busy();
    // 主界面每帧会把自检页、比赛弹窗提到最上面;进度条要再盖回去。
    void raiseOpen();

Q_SIGNALS:
    void canExitChanged(bool can_exit);
    // H 框里装配难度那一排变了(选了别的难度、能不能选变了)。
    void rowChanged();
    // 每条发出去的指令(验收脚本和复盘看)。ok = 交给了发送链路。
    void commandSent(std::uint32_t operation, std::uint32_t difficulty, bool ok);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    bool send(const AssemblySend& send);
    void refresh();
    void place();

    QWidget* host_;
    QWidget* neighbor_ = nullptr;
    AssemblyProgress* progress_;
    AssemblyState state_;
    Sender sender_;
    CommandGate gate_;
    bool link_up_ = false;
    MonotonicMs now_ = 0;
    bool can_exit_ = false;
    AssemblyRow row_;
};

}  // namespace rm_terminal
