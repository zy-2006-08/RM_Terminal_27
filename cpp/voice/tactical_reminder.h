#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rm_terminal {

// 战术提醒的共享契约。调度器、持久化、语音准备和设置界面都只依赖这个文件,
// 所以四个方向可以并行开发而不会各自发明一套字段名。

// 一条已保存的提醒。触发条件是**比赛剩余秒数**而不是墙上时钟:操作手赛前填的是
// 「剩余 1 分 30 秒时提醒我」,和这局什么时候开打无关。`id` 独立于文本,改文字
// 不会让这条提醒被当成新的一条重新播报。
struct ReminderItem {
    std::string id;
    std::int32_t remaining_sec = 0;
    std::string text;
    bool enabled = true;
};

// 持久化的整体配置。`master_enabled` 默认 false:全新安装不应该播报操作手
// 从未确认过的内容,静默是唯一安全的默认值。
//
// `voice` 空 = 让语音后端自己挑一个实测可合成的声音。不写死具体名字作为默认值:
// 声音是按需下载的系统资源,换台机器就可能不存在,写死会让新机器直接不可用。
struct ReminderConfig {
    int version = 1;
    bool master_enabled = false;
    std::string voice;
    std::int32_t rate_wpm = 225;
    std::vector<ReminderItem> reminders;
};

// 持久化约束。放在共享头里而不是各写一遍:界面的输入校验和落盘时的拒绝规则
// 必须是同一组数字,否则界面允许保存的内容会在写盘时被拒。
constexpr int kReminderConfigVersion = 1;
constexpr std::size_t kMaxReminders = 100;
constexpr std::size_t kMaxReminderTextChars = 200;
constexpr std::int32_t kMaxReminderRemainingSec = 5999;

// 语速上下限。say -r 接受的范围远比这宽,但过慢的播报会占满整个提醒窗口、过快则
// 听不清,两者在赛场上都等于没播。默认 225 实测六字战术约 1.04 秒读完 —— 一条提醒
// 连两遍加间隔约 3.1 秒,留在操作手还能据此行动的时间内。
// 注意两遍不会糊在一起靠的是**第一遍播放结束后**才起算 kRepeatGapMs,与单遍时长无关。
constexpr std::int32_t kMinReminderRateWpm = 150;
constexpr std::int32_t kMaxReminderRateWpm = 300;
constexpr std::int32_t kDefaultReminderRateWpm = 225;

// 一局比赛的总时长(秒)。正计时输入靠它换算成剩余秒数:操作手说「开赛满 3 分钟」,
// 存下来的必须是协议语义的剩余秒数。裁判系统标准赛制 7 分钟。
constexpr std::int32_t kMatchDurationSec = 420;

// 正计时(已进行秒数)与协议语义的剩余秒数互转。存盘和调度**只认剩余秒数**,
// 正计时纯粹是输入/显示的另一种表达 —— 两者同源可避免出现互相矛盾的两份真相。
// 超出本局时长的输入换算后为负,由调用方判非法,这里不做钳制:悄悄夹到 0 会把
// 「开赛满 8 分钟」变成一条在最后一秒触发的提醒。
constexpr std::int32_t elapsedToRemaining(std::int32_t elapsed_sec) {
    return kMatchDurationSec - elapsed_sec;
}

constexpr std::int32_t remainingToElapsed(std::int32_t remaining_sec) {
    return kMatchDurationSec - remaining_sec;
}

// 离线语音的准备状态。`Unavailable` 是平台边界:非 macOS 上没有已核实的本机
// 中文语音后端,这时要明确报告没有语音,而不是假装 Ready 然后到点静默。
enum class AudioReadiness { Idle, Preparing, Ready, Failed, Unavailable };

// `generation` 绑定到产生这批音频的配置版本。配置一改就换代次,于是准备中的
// 旧回调回来时能被认出是过期结果,不会把上一版文本当成当前配置播出去。
struct AudioStatus {
    std::uint32_t generation = 0;
    AudioReadiness state = AudioReadiness::Idle;
    std::string message;
};

}
