#include "popup_state.h"

namespace rm_terminal {

int popup_priority(Popup popup) {
    switch (popup) {
        case Popup::None: return 0;
        case Popup::PreMatch: return 1;
        case Popup::Paused: return 2;
        case Popup::Eliminated: return 3;
        case Popup::Settlement: return 4;
        case Popup::WaitingForData: return 5;
        case Popup::LinkLost: return 6;
    }
    return 0;
}

PopupStateMachine::PopupStateMachine(MonotonicMs min_visible_ms)
    : min_visible_ms_(min_visible_ms < 0 ? 0 : min_visible_ms) {}

Popup PopupStateMachine::resolve(const PopupInputs& inputs) {
    // Link state is judged before any match value is read. A stale `paused` is not
    // a pause and a stale HP of 0 is not a death; acting on either would state a
    // last-known guess as present fact.
    if (inputs.match_freshness == Freshness::NeverReceived) return Popup::WaitingForData;
    if (inputs.match_freshness == Freshness::Stale) return Popup::LinkLost;

    // 阶段编码来自 proto/rm_terminal.proto: 0未开始 1准备 2自检 3倒计时 4比赛中 5结算
    constexpr std::uint32_t kStageInMatch = 4;
    constexpr std::uint32_t kStageSettlement = 5;

    if (inputs.stage.has_value() && *inputs.stage == kStageSettlement)
        return Popup::Settlement;

    // Death outranks pause: a paused overlay would imply the operator still has a
    // robot to drive back.
    if (inputs.self_hp.has_value() && *inputs.self_hp == 0) return Popup::Eliminated;

    if (inputs.paused.value_or(false)) return Popup::Paused;

    // Only stages strictly before the match count as pre-match. An unknown stage
    // deliberately yields None rather than PreMatch, because covering a live match
    // with a "准备中" overlay is the worse failure.
    if (inputs.stage.has_value() && *inputs.stage < kStageInMatch) return Popup::PreMatch;

    return Popup::None;
}

PopupDecision PopupStateMachine::step(const PopupInputs& inputs, MonotonicMs now) {
    // Same clock defence as UiModeMachine, and for the same reason: a sleep/wake or
    // NTP correction must not let the dwell window "complete" across a gap in which
    // the popup was never actually on screen.
    bool timers_trustworthy = true;
    if (last_now_.has_value()) {
        const MonotonicMs delta = now - *last_now_;
        if (delta < 0) {
            timers_trustworthy = false;
        } else if (min_visible_ms_ > 0 && delta >= min_visible_ms_ * 10) {
            if (shown_since_.has_value()) shown_since_ = now;
        }
    }
    last_now_ = now;

    const Popup target = resolve(inputs);

    if (target == current_) {
        // Countdown keeps tracking while the popup is held, so a PreMatch overlay
        // counts down instead of freezing at the value it was raised with.
        countdown_sec_ = inputs.countdown_sec;
        return PopupDecision{current_, countdown_sec_};
    }

    const bool escalating = popup_priority(target) > popup_priority(current_);
    if (escalating) {
        current_ = target;
        shown_since_ = now;
        countdown_sec_ = inputs.countdown_sec;
        return PopupDecision{current_, countdown_sec_};
    }

    // De-escalation is rate-limited, not blocked: hold the current popup until it
    // has been readable for min_visible_ms_, then take the lower-priority target.
    const bool dwell_satisfied =
        !shown_since_.has_value() ||
        (timers_trustworthy && now - *shown_since_ >= min_visible_ms_);
    if (!dwell_satisfied) return PopupDecision{current_, countdown_sec_};

    current_ = target;
    shown_since_ = target == Popup::None ? std::optional<MonotonicMs>{} : now;
    countdown_sec_ = inputs.countdown_sec;
    return PopupDecision{current_, countdown_sec_};
}

}  // namespace rm_terminal
