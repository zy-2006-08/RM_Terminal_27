#pragma once

#include "domain.h"
#include "tactical_reminder.h"

#include <cstdint>
#include <deque>
#include <optional>
#include <string>

namespace rm_terminal {

// 第一遍**成功播完**之后至少这么久才播第二遍。用户确认的行为:两遍之间留 1 秒,
// 让操作手在嘈杂的赛场上有机会听清第二遍。计时从上一遍结束算起,不是从开始算起,
// 所以一句长战术不会把两遍挤在一起。
constexpr MonotonicMs kRepeatGapMs = 1000;

// 每条提醒总共播两遍。
constexpr int kPlaysPerReminder = 2;

struct PlayCommand {
    std::string id;
    std::string path;
    std::uint64_t token = 0;
    int pass = 1;
};

// 缓存文件名。由文本、声音和配置版本共同决定:改文字或换声音都会得到新文件,
// 于是不会播出与当前配置不符的旧音频。
std::string reminder_audio_key(const std::string& text, const std::string& voice, int version);

// 串行播放队列。纯逻辑:不碰音频设备也不碰定时器,时间由 tick() 显式传入,
// 因此两遍之间的 1 秒间隔可以被确定性地断言而不需要真的等待。
//
// 单队列 FIFO:同一条的两遍绝不与别的提醒交错,同刻的多条按入队顺序排队。
class ReminderPlayback {
public:
    void enqueue(const ReminderItem& item, const std::string& path);

    // 返回这一刻应该开始的播放,没有就是 nullopt。`allow` 是暂停/就绪门控:
    // 关闭时队列保留但不开始新的播放。
    std::optional<PlayCommand> tick(MonotonicMs now, bool allow);

    // 播放结束的回调。`ok` 为假时不排第二遍:第一遍没成功,重复它没有意义,
    // 而且会掩盖真实故障。
    void finished(std::uint64_t token, bool ok, MonotonicMs now);

    // 只清待播和重复计时,当前一句让它说完。对应「暂停」与字段失效。
    void cancelPending();

    // 连当前播放一起作废。对应关总开关和比赛结束。
    void cancelAll();

    bool busy() const { return active_.has_value(); }
    std::size_t pendingCount() const { return queue_.size(); }

private:
    struct Utterance {
        std::string id;
        std::string path;
        int pass = 1;
    };
    struct Active {
        Utterance utterance;
        std::uint64_t token = 0;
        // cancelPending 允许当前一句说完,但那一句结束后不能再排第二遍 ——
        // 否则「暂停」会在暂停期间又响一次。
        bool repeat_allowed = true;
    };

    std::deque<Utterance> queue_;
    std::optional<Active> active_;
    // 第二遍的最早开始时刻。第一遍成功结束时设置。
    std::optional<MonotonicMs> repeat_at_;
    std::optional<Utterance> repeat_;
    std::uint64_t next_token_ = 1;
};

}
