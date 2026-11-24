#include "ui_mode.h"

namespace rm_terminal {

UiModeMachine::UiModeMachine(MonotonicMs exit_hysteresis_ms, MonotonicMs stale_fallback_ms)
    : exit_hysteresis_ms_(exit_hysteresis_ms < 0 ? 0 : exit_hysteresis_ms),
      stale_fallback_ms_(stale_fallback_ms < 0 ? 0 : stale_fallback_ms) {}

ModeDecision UiModeMachine::step(bool blind_asserted, Freshness blind_freshness, MonotonicMs now) {
    // A competition laptop sleeps, wakes, and gets NTP-corrected. Both a
    // backward step and an implausibly large forward jump mean the samples that
    // would have filled the window were never actually observed, so any window in
    // flight is restarted from this instant rather than judged complete. Without
    // this, one sleep/wake would switch modes on evidence nobody saw.
    bool timers_trustworthy = true;
    if (last_now_.has_value()) {
        const MonotonicMs delta = now - *last_now_;
        if (delta < 0) {
            // Backward and forward clock jumps get deliberately OPPOSITE handling.
            // Backward: skip this sample, keep the anchor. Re-anchoring would compare
            // instants from two different epochs; the window still completes once the
            // clock moves forward again, delayed at most by the jump size.
            timers_trustworthy = false;
        } else if (exit_hysteresis_ms_ > 0 && delta >= exit_hysteresis_ms_ * 10) {
            // Forward leap of many windows is a sleep/wake: the clear samples that
            // would have filled the window were never observed, so re-anchor instead
            // of crediting the gap as elapsed observation time.
            if (pending_exit_since_.has_value()) pending_exit_since_ = now;
            if (stale_since_.has_value()) stale_since_ = now;
        }
    }
    last_now_ = now;

    advanceAutomatic(blind_asserted, blind_freshness, now, timers_trustworthy);

    if (forced_.has_value()) return ModeDecision{*forced_, ModeReason::ForcedByCli};
    return ModeDecision{mode_, reason_};
}

void UiModeMachine::advanceAutomatic(bool blind_asserted, Freshness blind_freshness,
                                     MonotonicMs now, bool timers_trustworthy) {
    // Only a live reading can be acted on. A stale `true` is not an assertion and
    // a stale `false` is not a clear - staleness is handled as its own case below.
    const bool trustworthy = blind_freshness == Freshness::Fresh;

    if (!trustworthy) {
        if (mode_ != UiMode::Video) {
            // Never blinded and no trustworthy data: nothing to react to. Sitting
            // in Info is correct, so do not invent a reason to leave it.
            stale_since_.reset();
            return;
        }
        // Holding video on untrustworthy data is deliberate: dropping the feed the
        // moment the topic stutters would take the picture away during blinding.
        // The window in flight is voided, because time measured while the data was
        // untrustworthy must never be credited toward leaving.
        pending_exit_since_.reset();
        if (!stale_since_.has_value()) stale_since_ = now;
        reason_ = ModeReason::BlindDataStale;

        // Terminating condition. Without it a dead publisher keeps the terminal
        // fullscreen forever, long after the blinding actually ended.
        if (timers_trustworthy && stale_fallback_ms_ > 0 &&
            now - *stale_since_ >= stale_fallback_ms_) {
            mode_ = UiMode::Info;
            reason_ = ModeReason::BlindSignalLost;
            stale_since_.reset();
        }
        return;
    }

    stale_since_.reset();

    if (blind_asserted) {
        // Enter (or stay) immediately, and cancel any exit in flight so a single
        // re-assertion restarts the full window instead of extending a partial one.
        mode_ = UiMode::Video;
        reason_ = ModeReason::BlindAsserted;
        pending_exit_since_.reset();
        return;
    }

    if (mode_ != UiMode::Video) return;

    if (!pending_exit_since_.has_value()) {
        pending_exit_since_ = now;
        return;
    }
    if (timers_trustworthy && now - *pending_exit_since_ >= exit_hysteresis_ms_) {
        mode_ = UiMode::Info;
        reason_ = ModeReason::BlindClearedHysteresis;
        pending_exit_since_.reset();
    }
}

}  // namespace rm_terminal
