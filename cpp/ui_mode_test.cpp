#include "ui_mode.h"
#include <iostream>
#include <stdexcept>

using namespace rm_terminal;

namespace {

void check(bool condition, const char* name) {
    if (!condition) throw std::runtime_error(name);
    std::cout << "PASS " << name << '\n';
}

constexpr MonotonicMs kHysteresis = 3000;
constexpr MonotonicMs kStaleFallback = 15000;

// Shorthands: the three blind-signal shapes the machine must distinguish.
// A fresh assertion is the only thing allowed to ENTER video mode.
ModeDecision feed_fresh_blind(UiModeMachine& m, MonotonicMs now) {
    return m.step(true, Freshness::Fresh, now);
}
ModeDecision feed_fresh_clear(UiModeMachine& m, MonotonicMs now) {
    return m.step(false, Freshness::Fresh, now);
}
ModeDecision feed_stale(UiModeMachine& m, MonotonicMs now) {
    // Value is deliberately `true` to prove staleness, not the value, drives the
    // decision: a stale `true` must not be treated as a live assertion.
    return m.step(true, Freshness::Stale, now);
}

UiModeMachine make_machine(MonotonicMs hysteresis = kHysteresis,
                           MonotonicMs stale_fallback = kStaleFallback) {
    return UiModeMachine(hysteresis, stale_fallback);
}

// (1) A terminal that boots into video mode would hide the map for no reason.
void starts_in_info_mode() {
    UiModeMachine m = make_machine();
    check(m.mode() == UiMode::Info, "initial mode is Info");
    check(m.reason() == ModeReason::Startup, "initial reason is Startup");
    check(!m.pendingExitSince().has_value(), "initial state has no pending exit");
}

// (2) Blinding is the moment the operator needs the video feed. No hysteresis on
// the way IN - any delay here is a delay in the one thing that matters.
void enters_video_immediately_on_fresh_blind() {
    UiModeMachine m = make_machine();
    const ModeDecision d = feed_fresh_blind(m, 1000);
    check(d.mode == UiMode::Video, "fresh blind enters Video");
    check(d.reason == ModeReason::BlindAsserted, "entry reason is BlindAsserted");
}

// (3)+(4) The exit boundary is exact: 2999ms still video, 3000ms switches back.
// `>=` not `>`. An off-by-one here is a mode flap in the field.
void exit_boundary_is_exact() {
    UiModeMachine m = make_machine();
    feed_fresh_blind(m, 0);
    feed_fresh_clear(m, 1000);  // clear observed at t=1000, timer starts here

    check(feed_fresh_clear(m, 1000 + 2999).mode == UiMode::Video,
          "still Video at 2999ms after clear");
    const ModeDecision at_limit = feed_fresh_clear(m, 1000 + 3000);
    check(at_limit.mode == UiMode::Info, "returns to Info at exactly 3000ms");
    check(at_limit.reason == ModeReason::BlindClearedHysteresis,
          "exit reason is BlindClearedHysteresis");
}

// (5) The anti-flap core. A single re-assert must RESET the window, not extend
// it. Total elapsed 5998ms without exiting proves the timer restarted rather
// than accumulating toward the threshold.
void reassert_resets_the_exit_timer() {
    UiModeMachine m = make_machine();
    feed_fresh_blind(m, 0);
    feed_fresh_clear(m, 100);
    check(feed_fresh_clear(m, 100 + 2999).mode == UiMode::Video, "not yet exited");

    feed_fresh_blind(m, 100 + 2999);  // re-assert one tick before exit
    check(!m.pendingExitSince().has_value(), "re-assert clears pending exit");

    const MonotonicMs resumed = 100 + 2999;
    feed_fresh_clear(m, resumed + 1);
    check(feed_fresh_clear(m, resumed + 2999).mode == UiMode::Video,
          "still Video 5998ms after first clear (timer reset, not accumulated)");
    check(feed_fresh_clear(m, resumed + 1 + 3000).mode == UiMode::Info,
          "exits a full hysteresis after the re-assert");
}

// (6) Repeated clear samples must not emit repeated transitions. The GUI logs on
// transition, so a duplicate here becomes log spam and a false flap signal.
void steady_state_produces_one_transition_each_way() {
    UiModeMachine m = make_machine();
    int to_video = 0;
    int to_info = 0;
    UiMode previous = m.mode();

    for (int round = 0; round < 3; ++round) {
        const MonotonicMs base = static_cast<MonotonicMs>(round) * 100000;
        for (int i = 0; i < 20; ++i) {  // 20 blind samples
            const ModeDecision d = feed_fresh_blind(m, base + i * 10);
            if (d.mode != previous) {
                if (d.mode == UiMode::Video) ++to_video; else ++to_info;
                previous = d.mode;
            }
        }
        for (int i = 0; i <= 400; ++i) {  // 4000ms of clear, past the 3000ms exit
            const ModeDecision d = feed_fresh_clear(m, base + 200 + i * 10);
            if (d.mode != previous) {
                if (d.mode == UiMode::Video) ++to_video; else ++to_info;
                previous = d.mode;
            }
        }
    }
    check(to_video == 3, "exactly 3 transitions into Video across 3 rounds");
    check(to_info == 3, "exactly 3 transitions into Info across 3 rounds");
}

// (7) A stale `true` is untrustworthy, but dropping video the instant the feed
// stutters would rip the picture away exactly when it is needed. Hold, and say why.
void stale_blind_holds_video_with_distinct_reason() {
    UiModeMachine m = make_machine();
    feed_fresh_blind(m, 0);
    const ModeDecision d = feed_stale(m, 500);
    check(d.mode == UiMode::Video, "stale blind keeps Video");
    check(d.reason == ModeReason::BlindDataStale, "stale reason is BlindDataStale");
}

// (8) Never having received the field is not evidence of blinding.
void never_received_stays_in_info() {
    UiModeMachine m = make_machine();
    const ModeDecision d = m.step(false, Freshness::NeverReceived, 5000);
    check(d.mode == UiMode::Info, "NeverReceived stays in Info");
}

// (9) The CLI override must be honest about being an override, so evidence
// screenshots can never be mistaken for real blinding.
void forced_mode_overrides_and_releases() {
    UiModeMachine m = make_machine();
    m.forceMode(UiMode::Video);
    const ModeDecision forced = m.step(false, Freshness::Fresh, 100);
    check(forced.mode == UiMode::Video, "force overrides automatic Info");
    check(forced.reason == ModeReason::ForcedByCli, "forced reason is ForcedByCli");

    m.forceMode(std::nullopt);
    check(m.step(false, Freshness::Fresh, 200).mode == UiMode::Info,
          "releasing force returns to the automatic conclusion");
}

// (10) NTP steps and manual clock changes are routine on a competition laptop.
// Going backwards must not advance hysteresis and must not crash.
void backward_clock_does_not_advance_hysteresis() {
    UiModeMachine m = make_machine();
    feed_fresh_blind(m, 10000);
    feed_fresh_clear(m, 11000);
    check(feed_fresh_clear(m, 5000).mode == UiMode::Video,
          "backward time does not trigger exit");
    check(feed_fresh_clear(m, 11000 + 2999).mode == UiMode::Video,
          "backward time did not corrupt the window");
    check(feed_fresh_clear(m, 11000 + 3000).mode == UiMode::Info,
          "window still completes correctly after a backward step");
}

// (11) The hysteresis is configurable, so the boundary must hold at any value.
void boundary_holds_for_non_default_hysteresis() {
    UiModeMachine m = make_machine(500);
    feed_fresh_blind(m, 0);
    feed_fresh_clear(m, 1000);
    check(feed_fresh_clear(m, 1000 + 499).mode == UiMode::Video, "Video at 499ms (hys=500)");
    check(feed_fresh_clear(m, 1000 + 500).mode == UiMode::Info, "Info at 500ms (hys=500)");
}

// (12) A stale gap must VOID the window. Otherwise time counted while the data
// was untrustworthy gets credited toward exiting, and the mode leaves early.
void stale_voids_the_pending_window() {
    UiModeMachine m = make_machine();
    feed_fresh_blind(m, 0);
    feed_fresh_clear(m, 1000);
    feed_fresh_clear(m, 3000);  // 2000ms accumulated
    feed_stale(m, 3100);        // must void it
    check(!m.pendingExitSince().has_value(), "stale clears the pending exit timer");

    feed_fresh_clear(m, 3200);  // fresh window starts here
    check(feed_fresh_clear(m, 3200 + 2999).mode == UiMode::Video,
          "prior 2000ms is not credited after a stale gap");
    check(feed_fresh_clear(m, 3200 + 3000).mode == UiMode::Info,
          "requires a full window after the stale gap");
}

// (13) Recovery from stale into a live assertion is a real re-assertion.
void stale_to_fresh_blind_restores_asserted_reason() {
    UiModeMachine m = make_machine();
    feed_fresh_blind(m, 0);
    feed_stale(m, 500);
    const ModeDecision d = feed_fresh_blind(m, 1000);
    check(d.mode == UiMode::Video, "still Video");
    check(d.reason == ModeReason::BlindAsserted, "recovered reason is BlindAsserted");
}

// (14) Recovery from stale into a clear starts a brand-new window.
void stale_to_fresh_clear_starts_new_window() {
    UiModeMachine m = make_machine();
    feed_fresh_blind(m, 0);
    feed_stale(m, 500);
    feed_fresh_clear(m, 1000);
    check(m.pendingExitSince().has_value(), "a new window opened");
    check(feed_fresh_clear(m, 1000 + 2999).mode == UiMode::Video, "window not yet complete");
    check(feed_fresh_clear(m, 1000 + 3000).mode == UiMode::Info, "window completes");
}

// (15) Sleep/wake jumps the clock forward by a lot. Treating that as "the window
// elapsed" would switch modes on evidence nobody observed - the samples during
// the gap never happened.
void long_pause_resets_rather_than_completing() {
    UiModeMachine m = make_machine();
    feed_fresh_blind(m, 0);
    feed_fresh_clear(m, 1000);

    const MonotonicMs after_wake = 1000 + kHysteresis * 10;
    check(feed_fresh_clear(m, after_wake).mode == UiMode::Video,
          "a long pause does not itself complete the window");
    check(feed_fresh_clear(m, after_wake + 2999).mode == UiMode::Video,
          "window restarted at wake time");
    check(feed_fresh_clear(m, after_wake + 3000).mode == UiMode::Info,
          "full window required after wake");
}

// (16) The stuck-forever guard. Without it, a dead blind topic pins the terminal
// to fullscreen video indefinitely with no way back to the map.
void stale_fallback_recovers_from_a_dead_signal() {
    UiModeMachine m = make_machine();
    feed_fresh_blind(m, 0);
    feed_stale(m, 1000);
    check(feed_stale(m, 1000 + 14999).mode == UiMode::Video,
          "still Video just before the fallback deadline");
    const ModeDecision d = feed_stale(m, 1000 + 15000);
    check(d.mode == UiMode::Info, "falls back to Info after 15000ms of stale");
    check(d.reason == ModeReason::BlindSignalLost, "fallback reason is BlindSignalLost");
}

// (16b) Zero disables the guard. Documented as "never auto-recover", so prove it
// really never fires rather than defaulting to some hidden timeout.
void zero_fallback_disables_the_guard() {
    UiModeMachine m = make_machine(kHysteresis, 0);
    feed_fresh_blind(m, 0);
    feed_stale(m, 100);
    check(feed_stale(m, 60000).mode == UiMode::Video,
          "fallback disabled: still Video after 60000ms of stale");
}

}  // namespace

int main() {
    try {
        starts_in_info_mode();
        enters_video_immediately_on_fresh_blind();
        exit_boundary_is_exact();
        reassert_resets_the_exit_timer();
        steady_state_produces_one_transition_each_way();
        stale_blind_holds_video_with_distinct_reason();
        never_received_stays_in_info();
        forced_mode_overrides_and_releases();
        backward_clock_does_not_advance_hysteresis();
        boundary_holds_for_non_default_hysteresis();
        stale_voids_the_pending_window();
        stale_to_fresh_blind_restores_asserted_reason();
        stale_to_fresh_clear_starts_new_window();
        long_pause_resets_rather_than_completing();
        stale_fallback_recovers_from_a_dead_signal();
        zero_fallback_disables_the_guard();
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    std::cout << "ui_mode: all checks passed\n";
    return 0;
}
