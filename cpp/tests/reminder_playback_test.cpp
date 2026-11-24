#include "reminder_playback.h"

#include <iostream>
#include <stdexcept>

using namespace rm_terminal;

namespace {
void check(bool condition, const char* name) {
    if (!condition) throw std::runtime_error(name);
    std::cout << "PASS " << name << '\n';
}

ReminderItem item(const char* id) { return {id, 60, "text", true}; }

void two_passes() {
    ReminderPlayback playback;
    playback.enqueue(item("a"), "a.aiff");
    auto first = playback.tick(0, true);
    check(first && first->pass == 1, "first pass starts immediately");
    check(!playback.tick(0, true), "single queue never overlaps while busy");

    playback.finished(first->token, true, 4000);
    check(!playback.tick(4999, true), "repeat is withheld until the 1000ms gap elapses");
    auto second = playback.tick(5000, true);
    check(second && second->pass == 2 && second->id == "a",
          "second pass starts exactly one second after the first finished");
    playback.finished(second->token, true, 6000);
    check(!playback.tick(9000, true), "exactly two plays per reminder, never a third");
    check(playback.pendingCount() == 0 && !playback.busy(), "queue drains after two passes");
}

void serialization() {
    ReminderPlayback playback;
    playback.enqueue(item("a"), "a.aiff");
    playback.enqueue(item("b"), "b.aiff");
    auto a1 = playback.tick(0, true);
    playback.finished(a1->token, true, 100);
    check(!playback.tick(500, true), "next reminder cannot jump ahead of a pending repeat");
    auto a2 = playback.tick(1100, true);
    check(a2->id == "a" && a2->pass == 2, "both passes of one reminder stay adjacent");
    playback.finished(a2->token, true, 1200);
    auto b1 = playback.tick(1200, true);
    check(b1->id == "b" && b1->pass == 1, "queued reminder starts only after the previous finishes");
    check(playback.pendingCount() == 0, "FIFO order preserved");
}

void gating() {
    ReminderPlayback playback;
    playback.enqueue(item("a"), "a.aiff");
    check(!playback.tick(0, false), "pause gate holds the first pass");
    auto first = playback.tick(100, true);
    check(first != std::nullopt, "resume releases the queue");
    playback.finished(first->token, true, 200);
    check(!playback.tick(5000, false), "pause gate holds the repeat too");
    check(playback.tick(5000, true) != std::nullopt, "repeat survives a pause and plays on resume");
}

void failures() {
    ReminderPlayback playback;
    playback.enqueue(item("a"), "a.aiff");
    auto first = playback.tick(0, true);
    playback.finished(first->token, false, 100);
    check(!playback.tick(5000, true), "a failed first pass is not repeated");
    check(playback.pendingCount() == 0, "failed reminder is not retried automatically");

    ReminderPlayback stale;
    stale.enqueue(item("a"), "a.aiff");
    auto active = stale.tick(0, true);
    stale.cancelAll();
    stale.finished(active->token, true, 100);
    check(!stale.tick(5000, true), "completion callback from a cancelled play cannot schedule a repeat");

    ReminderPlayback pending;
    pending.enqueue(item("a"), "a.aiff");
    pending.enqueue(item("b"), "b.aiff");
    auto running = pending.tick(0, true);
    pending.cancelPending();
    check(pending.busy(), "cancelPending lets the current utterance finish");
    pending.finished(running->token, true, 100);
    check(!pending.tick(5000, true), "cancelPending drops both the repeat and the queue");

    ReminderPlayback unknown;
    unknown.enqueue(item("a"), "a.aiff");
    auto only = unknown.tick(0, true);
    unknown.finished(only->token + 99, true, 100);
    check(unknown.busy(), "an unknown token is ignored, not treated as completion");
}

void cache_keys() {
    const auto base = reminder_audio_key("撤退", "Tingting", 1);
    check(base != reminder_audio_key("推进", "Tingting", 1), "different text yields a different file");
    check(base != reminder_audio_key("撤退", "Sinji", 1), "different voice yields a different file");
    check(base != reminder_audio_key("撤退", "Tingting", 2), "different version yields a different file");
    check(base == reminder_audio_key("撤退", "Tingting", 1), "same inputs are stable across calls");
    check(reminder_audio_key("ab", "c", 1) != reminder_audio_key("a", "bc", 1),
          "field boundaries are separated, so concatenations cannot collide");
    check(base.size() > 4 && base.substr(base.size() - 5) == ".aiff", "key names a playable file");
}
}

int main() {
    try {
        two_passes();
        serialization();
        gating();
        failures();
        cache_keys();
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    std::cout << "reminder_playback: all checks passed\n";
    return 0;
}
