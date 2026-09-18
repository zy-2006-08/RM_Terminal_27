#include "reminder_scheduler.h"

#include <algorithm>
#include <stdexcept>

namespace rm_terminal {
namespace {

// 一个字段可用 = 值在、质量合法、标记新鲜、时间戳既不过期也不来自未来。
// 逐字段独立判定沿用既有领域模型的约定:一个字段过期不该让旁边好的字段一起失效。
template<class T>
bool usable(const Field<T>& field, MonotonicMs now, int stale_ms) {
    if (!field.value || field.quality != Quality::Valid) return false;
    if (field.freshness != Freshness::Fresh || !field.last_valid) return false;
    const auto age = now - *field.last_valid;
    return age >= 0 && age < stale_ms;
}

}

ReminderScheduler::ReminderScheduler(ReminderConfig config, int freshness_stale_ms)
    : config_(std::move(config)), freshness_ms_(freshness_stale_ms) {
    if (freshness_stale_ms <= 0)
        throw std::invalid_argument("reminder freshness window must be positive");
}

bool ReminderScheduler::setConfig(ReminderConfig config) {
    if (locked_) return false;
    config_ = std::move(config);
    ++config_generation_;
    consumed_.clear();
    return true;
}

void ReminderScheduler::suspend(ReminderDecision* out) {
    suspended_ = true;
    out->cancel = ReminderCancellation::Pending;
    ++playback_generation_;
}

ReminderDecision ReminderScheduler::step(const ReminderInputs& inputs, MonotonicMs now) {
    const bool first = first_observation_;
    first_observation_ = false;

    // 观察间隔本身超过过期窗口时,即使字段自称新鲜也不能相信:事件循环卡了这么久,
    // 期间的倒计时穿越根本没被看到,继续按「连续观察」处理就会漏播或错播。
    const bool gap = last_now_ && (now - *last_now_ >= freshness_ms_ || now < *last_now_);
    last_now_ = now;

    ReminderDecision out;
    out.audio = inputs.audio;

    const bool master_on = config_.master_enabled && inputs.master_enabled;
    if (!master_on && !master_off_) {
        master_off_ = true;
        baseline_.reset();
        if (in_match_) {
            out.cancel = ReminderCancellation::All;
            ++playback_generation_;
        }
    } else if (master_on && master_off_) {
        // 赛中重新打开总开关。baseline_ 在关闭时已清空,这里不重建 —— 交给下面的
        // 建立基线分支,于是关闭期间过掉的阈值不会被补播。consumed_ 一并保留:
        // 关之前已经播过的那几条不该因为开关一关一开就再响一遍。
        master_off_ = false;
    }

    const bool stage_ok = usable(inputs.stage, now, freshness_ms_);
    const bool stage_legal = stage_ok && *inputs.stage.value <= kMaxStage;
    const bool in_match = stage_legal && *inputs.stage.value == kMatchStage;

    if (!inputs.connected || gap || !stage_legal) {
        // 断线或阶段失效期间不能保留「上一局已结束」的证据:否则一次重连会被
        // 误判成连续观察到的换局,从而重新布防并补播已经过去的阈值。
        saw_non4_at_.reset();
        baseline_.reset();
        if (in_match_) suspend(&out);
        out.settings_locked = locked_;
        out.playback_generation = playback_generation_;
        out.status = master_off_ ? ReminderStatus::Disabled
                    : in_match_ ? ReminderStatus::Suspended
                                : ReminderStatus::Idle;
        return out;
    }

    if (!in_match) {
        // 新鲜合法的非 4 阶段是唯一可信的「上一局已结束」证据。记下它,
        // 下一次进入 4 才允许重新布防。
        saw_non4_at_ = now;
        if (in_match_) {
            out.cancel = ReminderCancellation::All;
            ++playback_generation_;
        }
        in_match_ = false;
        locked_ = false;
        suspended_ = false;
        master_off_ = false;
        awaiting_countdown_ = false;
        baseline_.reset();
        consumed_.clear();
        out.playback_generation = playback_generation_;
        out.status = config_.master_enabled ? ReminderStatus::Idle : ReminderStatus::Disabled;
        return out;
    }

    if (master_off_) {
        // 锁定跟着「比赛开始」,不跟着开关。这里曾经直接返回,于是开关关着进入比赛
        // 时 locked_ 一直是 false —— 赛中还能改时间和文案。开关只决定播不播,
        // 不决定能不能编辑。
        in_match_ = true;
        locked_ = true;
        out.settings_locked = locked_;
        out.playback_generation = playback_generation_;
        out.status = ReminderStatus::Disabled;
        return out;
    }

    if (!in_match_) {
        in_match_ = true;
        locked_ = true;
        awaiting_countdown_ = true;
        match_entered_at_ = now;
        out.cancel_preview = true;
        baseline_.reset();
        if (saw_non4_at_) {
            suspended_ = false;
            consumed_.clear();
        } else if (!first) {
            // 进程内既没看到过本局之前的非 4 阶段,也不是刚启动 —— 无法确认是否
            // 换过局,保守暂停本局提醒,而不是猜一个可能错的基线。
            suspended_ = true;
        }
        saw_non4_at_.reset();
    }

    out.settings_locked = true;

    const bool paused_ok = usable(inputs.paused, now, freshness_ms_);
    if (!paused_ok) {
        // 缺暂停信息时只等待,不猜「未暂停」:猜错会在暂停期间照常播报。
        const bool never_received = inputs.paused.freshness == Freshness::NeverReceived;
        if (!never_received) suspend(&out);
        baseline_.reset();
        out.playback_generation = playback_generation_;
        out.status = never_received ? ReminderStatus::WaitingForPause : ReminderStatus::Suspended;
        return out;
    }

    const bool countdown_ok = usable(inputs.countdown, now, freshness_ms_);
    if (!countdown_ok || *inputs.countdown.value < 0) {
        if (!awaiting_countdown_) suspend(&out);
        baseline_.reset();
        out.playback_generation = playback_generation_;
        out.status = awaiting_countdown_ ? ReminderStatus::WaitingForCountdown
                                         : ReminderStatus::Suspended;
        return out;
    }

    // 进入阶段 4 后的第一个倒计时样本必须是**这一局**产生的。时间戳早于本次进入
    // 的样本一律拒绝,否则会拿换局前缓存的剩余时间布防。
    if (awaiting_countdown_) {
        if (*inputs.countdown.last_valid < match_entered_at_) {
            out.playback_generation = playback_generation_;
            out.status = ReminderStatus::WaitingForCountdown;
            return out;
        }
        awaiting_countdown_ = false;
    }

    const auto current = *inputs.countdown.value;
    const bool paused_now = *inputs.paused.value;
    out.playback_generation = playback_generation_;

    if (suspended_) {
        baseline_ = current;
        out.status = ReminderStatus::Suspended;
        return out;
    }

    if (!baseline_) {
        // 建立基线只记住起点,不播报。首次启动和恢复都走这里,于是断线期间
        // 过掉的阈值自然不会被补播。
        baseline_ = current;
        out.allow_playback = !paused_now;
        out.status = paused_now ? ReminderStatus::Paused : ReminderStatus::Active;
        return out;
    }

    const auto previous = *baseline_;
    baseline_ = current;
    if (current > previous) {
        // 倒计时回升。可能换局也可能数据错乱,无法区分,所以保守报告不确定并保留
        // 已消费项:既不重播,也不假装这是新一局。
        out.allow_playback = !paused_now;
        out.status = ReminderStatus::ContinuityUncertain;
        return out;
    }

    std::vector<ReminderItem> crossed;
    for (const auto& item : config_.reminders) {
        if (!item.enabled || consumed_.count(item.id)) continue;
        if (previous > item.remaining_sec && item.remaining_sec >= current) crossed.push_back(item);
    }
    // 剩余秒数降序:一次跨过多个阈值时先播时间靠前的那条。同一秒的多条保持保存
    // 顺序,靠 stable_sort 而不是 sort 保证。
    std::stable_sort(crossed.begin(), crossed.end(),
                     [](const ReminderItem& left, const ReminderItem& right) {
                         return left.remaining_sec > right.remaining_sec;
                     });

    // 派发即标记已消费,与是否真的播出无关。音频没准备好时这批内容被丢弃而不是
    // 排队:到点很久之后才响的战术提醒是误导,不如不响。
    const bool audio_ready = inputs.audio.state == AudioReadiness::Ready &&
                             inputs.audio.generation == config_generation_;
    for (const auto& item : crossed) consumed_.insert(item.id);
    if (audio_ready) out.enqueue = std::move(crossed);
    out.allow_playback = !paused_now && audio_ready;
    out.status = paused_now ? ReminderStatus::Paused : ReminderStatus::Active;
    return out;
}

}
