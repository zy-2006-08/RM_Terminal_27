#include "video_recovery.h"

namespace rm_terminal {

VideoRecoveryPolicy::VideoRecoveryPolicy(MonotonicMs udp_silence_ms,
                                         MonotonicMs decoder_stall_ms,
                                         MonotonicMs cooldown_ms)
    : udp_silence_ms_(udp_silence_ms < 0 ? 0 : udp_silence_ms),
      decoder_stall_ms_(decoder_stall_ms < 0 ? 0 : decoder_stall_ms),
      cooldown_ms_(cooldown_ms < 0 ? 0 : cooldown_ms) {}

std::optional<MonotonicMs> VideoRecoveryPolicy::cooldownUntil() const {
    if (!last_action_at_.has_value() || cooldown_ms_ == 0) return std::nullopt;
    return *last_action_at_ + cooldown_ms_;
}

RecoveryAction VideoRecoveryPolicy::step(const VideoLiveness& liveness, MonotonicMs now) {
    // Same clock defence as the mode and popup machines: a backward step or a
    // sleep/wake leap means the silence being measured was never actually observed.
    // Crediting it would kill a healthy decoder on the first tick after a wake.
    bool timers_trustworthy = true;
    if (last_now_.has_value()) {
        const MonotonicMs delta = now - *last_now_;
        if (delta < 0) {
            timers_trustworthy = false;
        } else if (udp_silence_ms_ > 0 && delta >= udp_silence_ms_ * 10) {
            timers_trustworthy = false;
            if (last_action_at_.has_value()) last_action_at_ = now;
        }
    }
    last_now_ = now;

    // Stderr is a level, not an edge: seed on the first tick so a decoder that has
    // been chatting since before the policy existed does not read as newly mute.
    const std::int64_t previous_stderr = last_stderr_bytes_;
    const bool stderr_seeded = stderr_seeded_;
    last_stderr_bytes_ = liveness.decoder_stderr_bytes;
    stderr_seeded_ = true;

    if (!timers_trustworthy) return RecoveryAction::None;

    const bool in_cooldown =
        cooldown_ms_ > 0 && last_action_at_.has_value() &&
        now - *last_action_at_ < cooldown_ms_;

    // UDP silence is checked first and outranks a decoder stall. When datagrams have
    // stopped, the decoder is mute BECAUSE it is being fed nothing - restarting it
    // would burn a process launch without addressing the cause.
    const bool udp_silent = liveness.stream_established &&
                            liveness.last_datagram.has_value() &&
                            now - *liveness.last_datagram >= udp_silence_ms_;
    if (udp_silent) {
        if (in_cooldown) return RecoveryAction::None;
        last_action_at_ = now;
        ++reconnects_;
        return RecoveryAction::ReconnectUdp;
    }

    // Only a truly mute decoder is restarted. Under loss FFmpeg emits stderr and
    // buffers for many seconds before its first frame; killing it then destroys a
    // healthy stream and the replacement fares no better.
    const bool decoder_spoke = stderr_seeded && liveness.decoder_stderr_bytes != previous_stderr;
    const bool decoder_stalled = liveness.awaiting_decode_since.has_value() &&
                                 now - *liveness.awaiting_decode_since >= decoder_stall_ms_ &&
                                 !decoder_spoke;
    if (decoder_stalled) {
        if (in_cooldown) return RecoveryAction::None;
        last_action_at_ = now;
        ++decoder_restarts_;
        return RecoveryAction::RestartDecoder;
    }

    return RecoveryAction::None;
}

}  // namespace rm_terminal
