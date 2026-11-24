#pragma once

#include "domain.h"

namespace rm_terminal {

// The single time source for everything that produces a `MonotonicMs`.
//
// It must be monotonic, not wall-clock: `Store::apply` keeps `latest_` and
// `Store::snapshot` throws when handed an earlier instant, so one NTP
// correction or manual clock change backward would terminate the process. A
// forward jump is just as harmful - it lets the mode exit window "complete"
// over a sleep the operator never observed as clear samples.
//
// It must also be ONE epoch process-wide. Intake stamps ingested updates and
// the UI tick reads snapshots; if those two used different origins, every
// snapshot would either throw or read as instantly stale.
//
// steady_clock rather than QElapsedTimer: the mosquitto loop thread calls this
// concurrently with the Qt main thread, and steady_clock::now() is guaranteed
// thread-safe, while QElapsedTimer makes no such guarantee.
MonotonicMs monotonic_now();

}  // namespace rm_terminal
