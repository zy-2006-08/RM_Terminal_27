#pragma once

#include "domain.h"
#include <cstdint>
#include <optional>

namespace rm_terminal {

// Which blocking overlay the operator should see. Exactly one at a time: two
// stacked overlays hide each other's text, so the machine resolves a winner
// instead of letting the UI draw every condition that happens to be true.
enum class Popup {
    None,
    LinkLost,        // match data was flowing and stopped
    WaitingForData,  // never received anything yet
    Settlement,      // stage 5
    Eliminated,      // self HP reached 0
    Paused,          // referee paused a live match
    PreMatch,        // stages 0-3: 未开始 / 准备 / 自检 / 倒计时
};

// Higher wins. LinkLost and WaitingForData sit above every match-derived popup
// because without trustworthy data "paused" and "eliminated" are last-known
// guesses, and an overlay that states a stale guess as fact is worse than one
// that admits the link is gone. Settlement outranks Eliminated because once the
// match is scored, telling the operator they died is no longer actionable.
int popup_priority(Popup popup);

// Caller extracts these from a Snapshot. Every optional means "no trustworthy
// value", so a missing HP can never be mistaken for a real 0.
struct PopupInputs {
    Freshness match_freshness = Freshness::NeverReceived;
    std::optional<std::uint32_t> stage;
    std::optional<bool> paused;
    std::optional<std::uint32_t> self_hp;
    std::optional<std::int32_t> countdown_sec;
};

struct PopupDecision {
    Popup popup = Popup::None;
    // Mirrored from the input so the UI renders the same instant the decision was
    // made on, rather than re-reading a snapshot that may have advanced.
    std::optional<std::int32_t> countdown_sec;
};

// Pure-logic popup decider. No Qt types, no global clock: `now` is injected so
// the dwell rule below is testable at exact instants.
//
// Asymmetric like UiModeMachine, for the same reason and in the same direction:
//   RAISE a higher-priority popup immediately - preemption is the whole point.
//   CLEAR (or downgrade to something less urgent) only after `min_visible_ms`,
//     so a single-tick condition cannot flash an unreadable overlay.
class PopupStateMachine {
public:
    // `min_visible_ms` == 0 disables the dwell guarantee, letting a popup appear
    // and vanish inside one 250ms tick.
    explicit PopupStateMachine(MonotonicMs min_visible_ms = 1000);

    PopupDecision step(const PopupInputs& inputs, MonotonicMs now);

    Popup popup() const { return current_; }
    // Set while a popup is being held past its trigger by the dwell rule.
    std::optional<MonotonicMs> shownSince() const { return shown_since_; }

private:
    static Popup resolve(const PopupInputs& inputs);

    MonotonicMs min_visible_ms_;
    Popup current_ = Popup::None;
    std::optional<MonotonicMs> shown_since_;
    std::optional<MonotonicMs> last_now_;
    std::optional<std::int32_t> countdown_sec_;
};

}  // namespace rm_terminal
