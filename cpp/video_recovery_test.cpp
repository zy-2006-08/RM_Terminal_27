#include "video_recovery.h"
#include <iostream>
#include <stdexcept>

using namespace rm_terminal;

namespace {

void check(bool condition, const char* name) {
    if (!condition) throw std::runtime_error(name);
    std::cout << "PASS " << name << '\n';
}

constexpr MonotonicMs kUdpSilence = 1000;
constexpr MonotonicMs kDecoderStall = 5000;
constexpr MonotonicMs kCooldown = 2000;

VideoRecoveryPolicy make_policy(MonotonicMs cooldown = kCooldown) {
    return VideoRecoveryPolicy(kUdpSilence, kDecoderStall, cooldown);
}

// An established stream with datagrams arriving and nothing outstanding.
VideoLiveness healthy(MonotonicMs now) {
    VideoLiveness live;
    live.stream_established = true;
    live.last_datagram = now;
    return live;
}

// (1) A healthy link is left alone. A policy that restarts anything here would
// churn the decoder through an entire match for no reason.
void healthy_link_needs_no_action() {
    VideoRecoveryPolicy p = make_policy();
    check(p.step(healthy(1000), 1000) == RecoveryAction::None, "healthy link needs no action");
    check(p.reconnects() == 0 && p.decoderRestarts() == 0, "no actions counted");
}

// (2) Before the first frame the UDP watchdog stays disarmed. A terminal started
// before the sender must wait, not reconnect in a loop.
void unestablished_stream_does_not_reconnect() {
    VideoRecoveryPolicy p = make_policy();
    VideoLiveness live;
    live.stream_established = false;
    live.last_datagram = 0;
    check(p.step(live, 60000) == RecoveryAction::None,
          "never-established stream does not trigger a reconnect");
}

// (3)+(4) The silence boundary is exact: 999ms quiet, 1000ms reconnects.
// `>=` not `>`. An off-by-one here is a reconnect storm on a marginal link.
void udp_silence_boundary_is_exact() {
    VideoRecoveryPolicy early = make_policy();
    VideoLiveness live = healthy(0);
    check(early.step(live, 999) == RecoveryAction::None,
          "still quiet just before the silence deadline");

    VideoRecoveryPolicy late = make_policy();
    check(late.step(live, 1000) == RecoveryAction::ReconnectUdp,
          "reconnects exactly at the silence deadline");
    check(late.reconnects() == 1, "the reconnect is counted");
}

// (5) A mute decoder with datagrams still flowing is the one case a restart fixes.
void mute_decoder_restarts() {
    VideoRecoveryPolicy p = make_policy();
    VideoLiveness live = healthy(6000);
    live.awaiting_decode_since = 1000;
    // Seed the stderr level first, so the second tick can observe "no new chatter".
    p.step(live, 1000);
    check(p.step(live, 6000) == RecoveryAction::RestartDecoder,
          "a mute decoder is restarted after the stall window");
    check(p.decoderRestarts() == 1, "the restart is counted");
}

// (6) A decoder that is emitting stderr is buffering, not dead. Killing it here
// destroys a healthy stream and the replacement fares no better.
void chatty_decoder_is_left_alone() {
    VideoRecoveryPolicy p = make_policy();
    VideoLiveness live = healthy(1000);
    live.awaiting_decode_since = 1000;
    live.decoder_stderr_bytes = 100;
    p.step(live, 1000);

    live.last_datagram = 6000;
    live.decoder_stderr_bytes = 250;  // new chatter since the previous tick
    check(p.step(live, 6000) == RecoveryAction::None,
          "a decoder still emitting stderr is not restarted");
}

// (7) Nothing outstanding means nothing to wait for, so no stall regardless of
// how long the policy has been running.
void no_outstanding_work_means_no_stall() {
    VideoRecoveryPolicy p = make_policy();
    VideoLiveness live = healthy(60000);
    p.step(healthy(0), 0);
    check(p.step(live, 60000) == RecoveryAction::None,
          "an idle decoder with no pending access unit is not restarted");
}

// (8) UDP silence outranks a decoder stall: when the pipe is dry the decoder is
// mute BECAUSE it is fed nothing, so a respawn burns a launch for nothing.
void udp_silence_outranks_decoder_stall() {
    VideoRecoveryPolicy p = make_policy();
    VideoLiveness live;
    live.stream_established = true;
    live.last_datagram = 0;
    live.awaiting_decode_since = 0;
    p.step(live, 0);
    // 5000ms 同时越过两个阈值,又不触发跳变保护(那需要 10 倍静默窗口)。
    check(p.step(live, 5000) == RecoveryAction::ReconnectUdp,
          "both conditions true yields ReconnectUdp");
    check(p.decoderRestarts() == 0, "no decoder restart when the pipe is dry");
}

// (9) The cooldown is the whole point of extracting this class: a decoder that
// dies on startup must not be respawned every single tick.
void cooldown_throttles_repeat_actions() {
    VideoRecoveryPolicy p = make_policy();
    VideoLiveness live = healthy(0);
    check(p.step(live, 1000) == RecoveryAction::ReconnectUdp, "first reconnect fires");
    check(p.step(live, 1500) == RecoveryAction::None, "second attempt is withheld");
    check(p.step(live, 2999) == RecoveryAction::None, "still withheld just before cooldown ends");
    check(p.step(live, 3000) == RecoveryAction::ReconnectUdp, "fires again once cooldown elapsed");
    check(p.reconnects() == 2, "only the two issued actions are counted");
}

// (10) Zero cooldown is documented as "no rate limit"; prove it really fires every
// tick rather than falling back to a hidden default.
void zero_cooldown_allows_every_tick() {
    VideoRecoveryPolicy p = make_policy(0);
    VideoLiveness live = healthy(0);
    check(p.step(live, 1000) == RecoveryAction::ReconnectUdp, "fires at the deadline");
    check(p.step(live, 1001) == RecoveryAction::ReconnectUdp, "fires again on the next tick");
    check(!p.cooldownUntil().has_value(), "no cooldown window is reported");
}

// (11) cooldownUntil is the evidence the throttle is armed, so the caller can
// report "recovery pending" instead of looking idle.
void cooldown_window_is_reported() {
    VideoRecoveryPolicy p = make_policy();
    check(!p.cooldownUntil().has_value(), "no cooldown before any action");
    p.step(healthy(0), 1000);
    check(p.cooldownUntil().has_value() && *p.cooldownUntil() == 1000 + kCooldown,
          "cooldown window ends one interval after the action");
}

// (12) A backward clock must not satisfy a silence window. Same hazard the other
// two machines guard: the silence was never actually observed.
void backward_clock_takes_no_action() {
    VideoRecoveryPolicy p = make_policy();
    VideoLiveness live = healthy(10000);
    p.step(live, 10000);
    live.last_datagram = 10000;
    check(p.step(live, 5000) == RecoveryAction::None, "a backward clock triggers nothing");
}

// (13) A sleep/wake leap must not read as a dead link. Every terminal is a laptop
// that gets closed, and the first tick after waking would otherwise tear down a
// link that is actually fine.
void long_pause_takes_no_action_on_the_first_tick() {
    VideoRecoveryPolicy p = make_policy();
    VideoLiveness live = healthy(0);
    p.step(live, 0);
    check(p.step(live, 600000) == RecoveryAction::None,
          "the first tick after a long pause takes no action");
    // The link really is dead, so the NEXT tick, measured over observed time, acts.
    check(p.step(live, 601000) == RecoveryAction::ReconnectUdp,
          "a genuinely dead link is still caught on the following tick");
}

// (14) Recovery path: the link comes back and the policy goes quiet again rather
// than staying latched on the last fault.
void restored_link_stops_acting() {
    VideoRecoveryPolicy p = make_policy();
    check(p.step(healthy(0), 1000) == RecoveryAction::ReconnectUdp, "reconnect on silence");
    check(p.step(healthy(4000), 4000) == RecoveryAction::None,
          "a restored link needs no further action");
    check(p.reconnects() == 1, "no extra action counted after recovery");
}

}  // namespace

int main() {
    try {
        healthy_link_needs_no_action();
        unestablished_stream_does_not_reconnect();
        udp_silence_boundary_is_exact();
        mute_decoder_restarts();
        chatty_decoder_is_left_alone();
        no_outstanding_work_means_no_stall();
        udp_silence_outranks_decoder_stall();
        cooldown_throttles_repeat_actions();
        zero_cooldown_allows_every_tick();
        cooldown_window_is_reported();
        backward_clock_takes_no_action();
        long_pause_takes_no_action_on_the_first_tick();
        restored_link_stops_acting();
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    std::cout << "video_recovery: all checks passed\n";
    return 0;
}
