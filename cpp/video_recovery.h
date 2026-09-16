#pragma once

#include "domain.h"
#include <cstdint>
#include <optional>

namespace rm_terminal {

// What the receiver should do about the link this tick.
enum class RecoveryAction {
    None,
    // Datagrams stopped after the stream had been established. Rebind/reset the
    // session, not just the decoder: the sender may have restarted with a new
    // frame numbering that the reassembler must not try to continue.
    ReconnectUdp,
    // Datagrams are still arriving but the decoder has gone mute. Respawn it and
    // leave the socket alone.
    RestartDecoder,
};

// What the receiver observed this tick. Optionals mean "never happened", which
// must not be confused with "happened at time 0".
struct VideoLiveness {
    // False until the first complete frame arrives. The UDP watchdog stays disarmed
    // until then, so a terminal started before the sender does not "reconnect" in a
    // loop while legitimately waiting for the first packet.
    bool stream_established = false;
    std::optional<MonotonicMs> last_datagram;
    // When the oldest un-decoded access unit was handed to the decoder, cleared by
    // the caller on ANY decoder output. Empty means nothing is outstanding.
    std::optional<MonotonicMs> awaiting_decode_since;
    // Cumulative decoder stderr bytes. The policy keeps its own previous value, so
    // "did the decoder say anything since last tick" is decided in one place.
    std::int64_t decoder_stderr_bytes = 0;
};

// Pure-logic recovery decider for the video link. No Qt types, no QProcess, no
// ffmpeg, no clock of its own: `now` is injected, so every threshold below is
// testable at exact instants.
//
// Two independent liveness signals, deliberately NOT merged into one timer:
//   DATAGRAM arrival proves the link. Frame completion does not - under loss many
//     access units never complete, so a frame-based watchdog would destroy a
//     healthy decoder exactly when the pipe got lossy.
//   DECODER OUTPUT (stdout progress or stderr chatter) proves the decoder. A mute
//     decoder with datagrams still flowing is the only case a restart can fix.
//
// Every action is rate-limited by `cooldown_ms`. Without it a decoder that dies
// on startup is restarted every tick, and each respawn costs an ffmpeg process
// launch plus a re-prime, which makes a bad link worse rather than better.
class VideoRecoveryPolicy {
public:
    // `cooldown_ms` == 0 disables rate limiting, allowing a restart on every tick
    // that meets a trigger condition.
    explicit VideoRecoveryPolicy(MonotonicMs udp_silence_ms = 1000,
                                 MonotonicMs decoder_stall_ms = 5000,
                                 MonotonicMs cooldown_ms = 2000);

    RecoveryAction step(const VideoLiveness& liveness, MonotonicMs now);

    // Counts actions this policy actually issued, for the diagnostic snapshot.
    std::int64_t reconnects() const { return reconnects_; }
    std::int64_t decoderRestarts() const { return decoder_restarts_; }

    // Set while an action is being withheld by the cooldown; empty otherwise. Lets
    // the caller report "recovery pending" instead of looking idle.
    std::optional<MonotonicMs> cooldownUntil() const;

private:
    MonotonicMs udp_silence_ms_;
    MonotonicMs decoder_stall_ms_;
    MonotonicMs cooldown_ms_;
    std::optional<MonotonicMs> last_action_at_;
    std::optional<MonotonicMs> last_now_;
    std::int64_t last_stderr_bytes_ = 0;
    bool stderr_seeded_ = false;
    std::int64_t reconnects_ = 0;
    std::int64_t decoder_restarts_ = 0;
};

}  // namespace rm_terminal
