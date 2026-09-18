#include "reminder_scheduler.h"
#include <iostream>
#include <stdexcept>

using namespace rm_terminal;

namespace {
void check(bool condition, const char* name) {
    if (!condition) throw std::runtime_error(name);
    std::cout << "PASS " << name << '\n';
}

ReminderConfig config() {
    ReminderConfig out;
    out.master_enabled = true;
    out.reminders = {{"thirty", 30, "Thirty", true},
                     {"sixty", 60, "Sixty", true},
                     {"sixty-second", 60, "Same time", true},
                     {"disabled", 45, "Disabled", false}};
    return out;
}

template<class T> Field<T> field(T value, MonotonicMs time) {
    return {value, time, Quality::Valid, Freshness::Fresh};
}

ReminderInputs sample(MonotonicMs time, int seconds, unsigned stage = 4) {
    ReminderInputs in;
    in.connected = true;
    in.stage = field(stage, time);
    in.countdown = field(seconds, time);
    in.paused = field(false, time);
    in.audio = {1, AudioReadiness::Ready, {}};
    return in;
}

void crossings() {
    ReminderScheduler m(config(), 2200);
    check(m.step(sample(0, 61), 0).enqueue.empty(), "startup establishes baseline only");
    auto d = m.step(sample(100, 59), 100);
    check(d.enqueue.size() == 2 && d.enqueue[0].id == "sixty" &&
          d.enqueue[1].id == "sixty-second", "61->59 consumes 60 in stable saved order");
    check(m.step(sample(100, 59), 200).enqueue.empty(), "repeated snapshot cannot redispatch after failure");
    check(m.step(sample(300, 59), 300).enqueue.empty(), "repeated value with new timestamp does not redispatch");
    ReminderScheduler skipped(config(), 2200);
    skipped.step(sample(0, 70), 0);
    d = skipped.step(sample(100, 20), 100);
    check(d.enqueue.size() == 3 && d.enqueue[0].id == "sixty" &&
          d.enqueue[2].id == "thirty", "multiple thresholds sorted descending, disabled item excluded");
    ReminderScheduler late(config(), 2200);
    check(late.step(sample(0, 59), 0).enqueue.empty(), "startup at 59 skips past 60");
    late.step(sample(100, 31), 100);
    check(late.step(sample(200, 29), 200).enqueue.size() == 1, "startup future-only 31->29 triggers 30");
    ReminderScheduler exact(config(), 2200);
    exact.step(sample(0, 60), 0);
    check(exact.step(sample(100, 59), 100).enqueue.empty(), "threshold equal to baseline is not replayed");
}

void starts() {
    for (unsigned stage : {3u, 5u}) {
        ReminderScheduler m(config(), 2200);
        m.step(sample(0, 61), 0);
        m.step(sample(100, 59), 100);
        auto end = m.step(sample(200, 70, stage), 200);
        check(end.cancel == ReminderCancellation::All && !end.settings_locked,
              "fresh non-4 ends playback and unlocks settings");
        auto cached = sample(300, 59);
        cached.countdown = field(59, 200);
        auto d = m.step(cached, 300);
        check(d.enqueue.empty() && d.settings_locked && d.cancel_preview &&
              d.status == ReminderStatus::WaitingForCountdown,
              "3/5->4 locks, cancels preview and rejects cached countdown");
        check(m.step(sample(400, 61), 400).enqueue.empty(), "new-stage countdown establishes baseline");
        check(m.step(sample(500, 59), 500).enqueue.size() == 2, "3/5->4 rearms consumed reminders");
    }
}

void continuity() {
    ReminderScheduler m(config(), 2200);
    m.step(sample(0, 70), 0);
    auto in = sample(100, 70);
    in.connected = false;
    auto d = m.step(in, 100);
    check(d.cancel == ReminderCancellation::Pending && d.settings_locked,
          "disconnect clears queue/repeat timer, preserves match lock");
    d = m.step(sample(200, 50), 200);
    check(d.enqueue.empty() && d.status == ReminderStatus::Suspended,
          "70 disconnect 50 reconnect never catches up or rearms");
    check(m.step(sample(300, 29), 300).enqueue.empty(), "reconnect cannot use startup exception for future thresholds");
    auto stale = sample(400, 70, 5);
    stale.stage.freshness = Freshness::Stale;
    m.step(stale, 400);
    check(m.step(sample(500, 61), 500).status == ReminderStatus::Suspended,
          "stale stage transition cannot rearm");
    m.step(sample(600, 70, 5), 600);
    m.step(sample(700, 61), 700);
    check(m.step(sample(800, 59), 800).enqueue.size() == 2, "confirmed next start recovers suspended scheduler");
    ReminderScheduler gap(config(), 500);
    gap.step(sample(0, 70), 0);
    check(gap.step(sample(500, 59), 500).status == ReminderStatus::Suspended,
          "event-loop gap at configured boundary suspends even with fresh fields");
    ReminderScheduler upward(config(), 2200);
    upward.step(sample(0, 61), 0);
    upward.step(sample(100, 59), 100);
    check(upward.step(sample(200, 70), 200).status == ReminderStatus::ContinuityUncertain,
          "upward countdown reports uncertainty rather than a new match");
    check(upward.step(sample(300, 59), 300).enqueue.empty(), "upward correction cannot replay consumed reminders");
}

void freshness() {
    for (int which = 0; which < 3; ++which) {
        ReminderScheduler m(config(), 500);
        m.step(sample(0, 70), 0);
        auto in = sample(499, 61);
        if (which == 0) in.stage.last_valid = 0;
        if (which == 1) in.countdown.last_valid = 0;
        if (which == 2) in.paused.last_valid = 0;
        check(m.step(in, 499).status == ReminderStatus::Active, "each field remains fresh at boundary minus one");
        check(m.step(in, 500).status == ReminderStatus::Suspended, "each independent field is stale exactly at boundary");
    }
    ReminderScheduler m(config(), 2200);
    auto in = sample(0, 61);
    in.paused = {};
    check(m.step(in, 0).status == ReminderStatus::WaitingForPause, "missing pause waits, never guesses unpaused");
    m.step(sample(100, 61), 100);
    in = sample(200, 59);
    in.paused.quality = Quality::Invalid;
    check(m.step(in, 200).status == ReminderStatus::Suspended, "invalid pause suspends established match");
    ReminderScheduler bad(config(), 2200);
    in = sample(0, 61, 6);
    check(!bad.step(in, 0).allow_playback, "illegal stage cannot start playback");
    in = sample(100, -1);
    check(!bad.step(in, 100).allow_playback, "negative countdown cannot start playback");
}

void gates() {
    ReminderScheduler m(config(), 2200);
    m.step(sample(0, 61), 0);
    auto in = sample(100, 61);
    in.paused = field(true, 100);
    auto d = m.step(in, 100);
    check(d.status == ReminderStatus::Paused && !d.allow_playback &&
          d.cancel == ReminderCancellation::None, "pause lets current utterance finish, holds repeat and next item");
    d = m.step(sample(200, 59), 200);
    check(d.allow_playback && d.enqueue.size() == 2, "resume opens queue gate");
    in = sample(300, 31);
    in.master_enabled = false;
    d = m.step(in, 300);
    check(d.cancel == ReminderCancellation::All && !d.allow_playback,
          "disable immediately stops audio and clears queue");
    // 赛中重新打开总开关必须真的重新布防:只能关不能开会把一次误触变成本局永久哑火。
    // 但关闭期间过掉的阈值不补播 —— 那几条已经过时,补播出来是误导。
    d = m.step(sample(400, 29), 400);
    check(d.status == ReminderStatus::Active, "master can be reenabled mid-match and re-arms");
    check(d.enqueue.empty(), "re-arming establishes a fresh baseline instead of back-firing");

    // 重新布防之后的阈值照常播报,否则「重新启用」只是个显示。
    d = m.step(sample(500, 19), 500);
    check(d.status == ReminderStatus::Active, "the re-armed match keeps running");
    check(!m.setConfig(config()), "settings replacement rejected while locked");
    m.step(sample(500, 70, 5), 500);
    check(m.setConfig(config()), "settings replacement allowed before next match");
    ReminderScheduler preparing(config(), 2200);
    in = sample(0, 61);
    in.audio.state = AudioReadiness::Preparing;
    preparing.step(in, 0);
    in = sample(100, 59);
    in.audio.state = AudioReadiness::Failed;
    d = preparing.step(in, 100);
    check(d.enqueue.empty() && !d.allow_playback && d.audio.state == AudioReadiness::Failed,
          "preparation failure is visible and crossed items consumed without dispatch");
    check(preparing.step(sample(200, 59), 200).enqueue.empty(), "readiness recovery never catches up missed thresholds");
    in = sample(300, 29);
    in.audio.generation = 99;
    check(preparing.step(in, 300).enqueue.empty(), "obsolete preparation generation cannot dispatch");
}

void adversarial() {
    ReminderScheduler m(config(), 500);
    m.step(sample(0, 61), 0);
    auto dispatched = m.step(sample(100, 59), 100);
    auto in = sample(200, 59);
    in.paused = field(true, 200);
    check(m.step(in, 200).cancel == ReminderCancellation::None, "pause retains dispatched FIFO work");
    in = sample(300, 59);
    in.countdown.freshness = Freshness::Stale;
    auto cancelled = m.step(in, 300);
    check(cancelled.cancel == ReminderCancellation::Pending && !cancelled.allow_playback &&
          cancelled.playback_generation != dispatched.playback_generation,
          "stale countdown cancels queued repeats and invalidates old completion tokens");
    check(!m.step(sample(400, 29), 400).allow_playback, "cancel then resume cannot revive cancelled queue");

    ReminderScheduler broken_start(config(), 500);
    broken_start.step(sample(0, 10, 3), 0);
    in = sample(100, 10, 3);
    in.connected = false;
    broken_start.step(in, 100);
    check(broken_start.step(sample(200, 61), 200).status == ReminderStatus::Suspended,
          "disconnect between non-4 and 4 is not a continuous new start");

    ReminderScheduler backwards(config(), 500);
    backwards.step(sample(100, 61), 100);
    check(!backwards.step(sample(99, 59), 99).allow_playback, "backwards explicit time suspends instead of crossing");
    ReminderScheduler future(config(), 500);
    check(!future.step(sample(100, 61), 99).allow_playback, "future field timestamps are not fresh evidence");

    for (int which = 0; which < 3; ++which) {
        ReminderScheduler invalid(config(), 500);
        invalid.step(sample(0, 61), 0);
        in = sample(100, 59);
        if (which == 0) in.stage.quality = Quality::Invalid;
        if (which == 1) in.countdown.quality = Quality::Invalid;
        if (which == 2) in.paused.quality = Quality::Invalid;
        const auto d = invalid.step(in, 100);
        check(d.cancel == ReminderCancellation::Pending && d.enqueue.empty(),
              "retained valid value cannot mask independent invalid quality");
    }
    ReminderScheduler paused(config(), 500);
    paused.step(sample(0, 61), 0);
    in = sample(100, 59);
    in.paused = field(true, 100);
    auto d = paused.step(in, 100);
    check(d.enqueue.size() == 2 && !d.allow_playback, "crossing during fresh pause queues but cannot play");
    check(paused.step(sample(200, 59), 200).allow_playback, "fresh resume releases gate without redispatch");

    ReminderScheduler defaults(ReminderConfig{}, 2200);
    check(defaults.step(sample(0, 61), 0).status == ReminderStatus::Disabled,
          "default configuration is disabled, not misleadingly ready");
    bool rejected = false;
    try { ReminderScheduler bad(config(), 0); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "nonpositive freshness configuration rejected explicitly");
}
}

int main() {
    try {
        crossings();
        starts();
        continuity();
        freshness();
        gates();
        adversarial();
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    std::cout << "reminder_scheduler: all checks passed\n";
    return 0;
}
