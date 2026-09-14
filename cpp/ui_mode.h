#pragma once

#include "domain.h"
#include <optional>

namespace rm_terminal {

// Which of the two operator layouts is on screen.
enum class UiMode { Info, Video };

// Why that layout is on screen. The operator must be able to tell "I am really
// being blinded" apart from "the blind topic died and we kept the picture" and
// from "someone passed --force-mode" - those three demand different reactions,
// so they are never collapsed into a single Video reason.
enum class ModeReason {
    Startup,
    BlindAsserted,            // live blind=true
    BlindClearedHysteresis,   // blind stayed false long enough to trust it
    ForcedByCli,              // --force-mode, evidence capture only
    BlindDataStale,           // blind data untrustworthy; picture held on purpose
    BlindSignalLost,          // stale for too long; gave up and returned to Info
};

struct ModeDecision {
    UiMode mode;
    ModeReason reason;
};

// Pure-logic mode decider. Holds no Qt types, reads no global clock: `now` is
// injected by the caller so every rule below is unit-testable at exact instants.
//
// Asymmetric on purpose:
//   ENTER video immediately on a fresh assertion - any delay withholds the feed
//     at the exact moment the operator needs it.
//   LEAVE video only after `exit_hysteresis_ms` of continuously observed clear
//     samples - a flapping signal must not strobe the layout.
class UiModeMachine {
public:
    // `stale_fallback_ms` == 0 disables the stuck-forever guard, meaning a dead
    // blind topic pins the terminal to video with no automatic way back.
    explicit UiModeMachine(MonotonicMs exit_hysteresis_ms,
                           MonotonicMs stale_fallback_ms = 15000);

    ModeDecision step(bool blind_asserted, Freshness blind_freshness, MonotonicMs now);

    UiMode mode() const { return forced_.value_or(mode_); }
    ModeReason reason() const {
        return forced_.has_value() ? ModeReason::ForcedByCli : reason_;
    }
    // Set while video is waiting out the exit window; empty otherwise.
    std::optional<MonotonicMs> pendingExitSince() const { return pending_exit_since_; }

    // Overrides the reported mode while set. The automatic state keeps tracking
    // underneath, so clearing the override resumes at the correct conclusion
    // instead of a stale one.
    void forceMode(std::optional<UiMode> forced) { forced_ = forced; }

private:
    void advanceAutomatic(bool blind_asserted, Freshness blind_freshness, MonotonicMs now,
                          bool timers_trustworthy);

    MonotonicMs exit_hysteresis_ms_;
    MonotonicMs stale_fallback_ms_;
    UiMode mode_ = UiMode::Info;
    ModeReason reason_ = ModeReason::Startup;
    std::optional<MonotonicMs> pending_exit_since_;
    std::optional<MonotonicMs> stale_since_;
    std::optional<MonotonicMs> last_now_;
    std::optional<UiMode> forced_;
};

}  // namespace rm_terminal
