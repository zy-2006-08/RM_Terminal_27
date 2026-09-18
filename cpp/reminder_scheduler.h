#pragma once

#include "domain.h"
#include "tactical_reminder.h"

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace rm_terminal {

// 只有阶段 4 是「比赛中」。其余阶段(准备/自检/倒计时/结算)都不播报,
// 因为剩余时间在那些阶段指的不是比赛剩余时间。
constexpr std::uint32_t kMatchStage = 4;
constexpr std::uint32_t kMaxStage = 5;

// Pending 只取消待播和第二遍的重复定时器,让当前一句自然说完;All 连当前音频
// 一起停。区分这两级是因为「暂停」和「关总开关」的语义不同:前者不该把一句话
// 从中间切断,后者必须立刻安静。
enum class ReminderCancellation { None, Pending, All };

enum class ReminderStatus {
    Disabled,             // 总开关关闭
    Idle,                 // 不在比赛中
    WaitingForCountdown,  // 已进入比赛,但还没拿到属于这一局的倒计时
    WaitingForPause,      // 缺暂停信息,不猜测「未暂停」
    Suspended,            // 断线/字段失效后保守暂停本局提醒
    ContinuityUncertain,  // 倒计时回升,无法确认是否换局
    Paused,
    Active,
};

// 一次观察的全部输入。全部显式传入而不从全局读取:调度器因此可以被确定性地
// 测试,不需要真的比赛、真的时钟或真的音频。
struct ReminderInputs {
    bool connected = false;
    // 赛中操作手可以关总开关。与 ReminderConfig::master_enabled 是两回事:
    // 这个是本次进程内的即时开关,那个是已持久化的意愿。
    bool master_enabled = true;
    Field<std::uint32_t> stage;
    Field<std::int32_t> countdown;
    Field<bool> paused;
    AudioStatus audio;
};

// 调度器的输出。它不播放也不落盘,只说「该排哪些、该取消什么、能否播放」,
// 于是同一套时序规则可以在没有音频设备的测试里逐条验证。
struct ReminderDecision {
    std::vector<ReminderItem> enqueue;
    ReminderCancellation cancel = ReminderCancellation::None;
    // 比赛开始时必须掐掉赛前试听,否则试听声会盖住真正的战术播报。
    bool cancel_preview = false;
    bool settings_locked = false;
    bool allow_playback = false;
    ReminderStatus status = ReminderStatus::Idle;
    // 播放代次。取消一次就换一个值,于是取消前发出的「播完了」回调回来时
    // 能被认出已经过期,不会触发第二遍或下一条。
    std::uint32_t playback_generation = 0;
    AudioStatus audio;
};

// 纯确定性状态机:给定输入序列,输出逐字节可复现。没有 Qt、没有定时器、
// 没有真实时钟 —— 时间是参数。
class ReminderScheduler {
public:
    // `freshness_stale_ms` 沿用配置里的 stale_window_ms。非正值直接拒绝:
    // 零窗口会让每个字段永远过期,提醒于是永远不响,而且没有任何报错。
    ReminderScheduler(ReminderConfig config, int freshness_stale_ms);

    ReminderDecision step(const ReminderInputs& inputs, MonotonicMs now);

    // 赛中拒绝替换配置(返回 false),这是「赛中只允许查看和关闭」的落点。
    bool setConfig(ReminderConfig config);

    const ReminderConfig& config() const { return config_; }
    std::uint32_t configGeneration() const { return config_generation_; }
    bool settingsLocked() const { return locked_; }

private:
    void suspend(ReminderDecision* out);

    ReminderConfig config_;
    int freshness_ms_;
    std::uint32_t config_generation_ = 1;
    std::uint32_t playback_generation_ = 0;
    std::set<std::string> consumed_;
    std::optional<std::int32_t> baseline_;
    std::optional<MonotonicMs> last_now_;
    MonotonicMs match_entered_at_ = 0;
    // 最近一次确认观察到的新鲜合法非 4 阶段。下一局必须先经过它,
    // 才可能重新布防 —— 这是「不猜测换局」的唯一凭据。
    std::optional<MonotonicMs> saw_non4_at_;
    bool in_match_ = false;
    bool locked_ = false;
    bool suspended_ = false;
    bool master_off_ = false;
    // 已进入阶段 4,但还没拿到确实属于这一局的倒计时样本。此时不能用换局前
    // 缓存的倒计时布防,否则上一局的剩余时间会在新一局里立刻触发一串提醒。
    bool awaiting_countdown_ = false;
    // 进程启动时就已经在比赛中,只能以当时的倒计时为基线、只播之后的阈值。
    // 这个特例**不能**被断线重连复用,否则重连会被当成一次新开赛而重新布防。
    bool first_observation_ = true;
};

}
