#include "popup_state.h"
#include <iostream>
#include <stdexcept>

using namespace rm_terminal;

namespace {

void check(bool condition, const char* name) {
    if (!condition) throw std::runtime_error(name);
    std::cout << "PASS " << name << '\n';
}

constexpr MonotonicMs kDwell = 1000;

// A live match with nothing wrong. Individual cases mutate one field, so each
// assertion isolates the single condition it claims to test.
PopupInputs healthy() {
    PopupInputs in;
    in.match_freshness = Freshness::Fresh;
    in.stage = 4;
    in.paused = false;
    in.self_hp = 300;
    in.countdown_sec = 200;
    return in;
}

PopupStateMachine make_machine(MonotonicMs dwell = kDwell) {
    return PopupStateMachine(dwell);
}

// (1) Before any data arrives the operator must be told the terminal is waiting,
// not shown an empty dashboard that looks like a real 0-0 match.
void starts_waiting_for_data() {
    PopupStateMachine m = make_machine();
    check(m.popup() == Popup::None, "initial popup is None before any step");
    PopupInputs in;
    in.match_freshness = Freshness::NeverReceived;
    check(m.step(in, 0).popup == Popup::WaitingForData, "never-received yields WaitingForData");
}

// (2) The whole point of the machine: a healthy live match shows nothing. An
// overlay in steady state trains the operator to ignore overlays.
void healthy_match_shows_nothing() {
    PopupStateMachine m = make_machine();
    check(m.step(healthy(), 1000).popup == Popup::None, "healthy live match shows no popup");
}

// (3) Stale match data outranks every match-derived popup. A stale `paused=true`
// is not a pause, so LinkLost must win over it.
void stale_link_outranks_match_popups() {
    PopupStateMachine m = make_machine();
    PopupInputs in = healthy();
    in.paused = true;
    in.self_hp = 0;
    in.stage = 5;
    in.match_freshness = Freshness::Stale;
    check(m.step(in, 1000).popup == Popup::LinkLost,
          "stale data yields LinkLost over paused/eliminated/settlement");
}

// (4) Priority order among match popups, each checked against the one it must beat.
void match_popup_priority_order() {
    check(popup_priority(Popup::LinkLost) > popup_priority(Popup::WaitingForData),
          "LinkLost outranks WaitingForData");
    check(popup_priority(Popup::WaitingForData) > popup_priority(Popup::Settlement),
          "WaitingForData outranks Settlement");
    check(popup_priority(Popup::Settlement) > popup_priority(Popup::Eliminated),
          "Settlement outranks Eliminated");
    check(popup_priority(Popup::Eliminated) > popup_priority(Popup::Paused),
          "Eliminated outranks Paused");
    check(popup_priority(Popup::Paused) > popup_priority(Popup::PreMatch),
          "Paused outranks PreMatch");
    check(popup_priority(Popup::PreMatch) > popup_priority(Popup::None),
          "PreMatch outranks None");
}

// (5) Death while paused reads as Eliminated: a paused overlay would imply the
// operator still has a robot to drive.
void eliminated_beats_paused() {
    PopupStateMachine m = make_machine();
    PopupInputs in = healthy();
    in.paused = true;
    in.self_hp = 0;
    check(m.step(in, 1000).popup == Popup::Eliminated, "hp=0 while paused yields Eliminated");
}

// (6) Settlement outranks death: once scored, "you died" is no longer actionable.
void settlement_beats_eliminated() {
    PopupStateMachine m = make_machine();
    PopupInputs in = healthy();
    in.stage = 5;
    in.self_hp = 0;
    check(m.step(in, 1000).popup == Popup::Settlement, "stage 5 with hp=0 yields Settlement");
}

// (7) Missing HP is not death. This is the field-vocabulary contract: absent must
// never be rendered as a real 0.
void missing_hp_is_not_death() {
    PopupStateMachine m = make_machine();
    PopupInputs in = healthy();
    in.self_hp.reset();
    check(m.step(in, 1000).popup == Popup::None, "absent hp does not yield Eliminated");
}

// (8) Unknown stage must not cover a live match with a pre-match overlay.
void unknown_stage_does_not_claim_prematch() {
    PopupStateMachine m = make_machine();
    PopupInputs in = healthy();
    in.stage.reset();
    check(m.step(in, 1000).popup == Popup::None, "absent stage does not yield PreMatch");
}

// (9) Each pre-match stage code from the proto maps to PreMatch.
void prematch_covers_stages_zero_through_three() {
    for (std::uint32_t stage = 0; stage <= 3; ++stage) {
        PopupStateMachine m = make_machine();
        PopupInputs in = healthy();
        in.stage = stage;
        check(m.step(in, 1000).popup == Popup::PreMatch, "pre-match stage yields PreMatch");
    }
}

// (10) Escalation is immediate. Withholding a more urgent overlay to finish
// showing a less urgent one is exactly backwards.
void escalation_is_immediate() {
    PopupStateMachine m = make_machine();
    PopupInputs pre = healthy();
    pre.stage = 1;
    check(m.step(pre, 0).popup == Popup::PreMatch, "raises PreMatch");

    PopupInputs dead = healthy();
    dead.self_hp = 0;
    // Well inside the dwell window: escalation must ignore it.
    check(m.step(dead, 100).popup == Popup::Eliminated,
          "escalates to Eliminated inside the dwell window");
}

// (11)+(12) The dwell boundary is exact: 999ms still held, 1000ms releases.
// `>=` not `>`. An off-by-one here is an overlay that flashes unreadably.
void dwell_boundary_is_exact() {
    PopupStateMachine m = make_machine();
    PopupInputs dead = healthy();
    dead.self_hp = 0;
    m.step(dead, 0);

    check(m.step(healthy(), 999).popup == Popup::Eliminated,
          "still Eliminated just before the dwell deadline");
    check(m.step(healthy(), 1000).popup == Popup::None,
          "clears exactly at the dwell deadline");
}

// (13) Zero dwell is documented as "no minimum"; prove it really clears at once
// rather than falling back to a hidden default.
void zero_dwell_clears_immediately() {
    PopupStateMachine m = make_machine(0);
    PopupInputs dead = healthy();
    dead.self_hp = 0;
    m.step(dead, 0);
    check(m.step(healthy(), 1).popup == Popup::None, "zero dwell clears on the next tick");
}

// (14) A backward clock must not satisfy the dwell window. Same hazard as the
// mode machine: the popup was never actually on screen for that span.
void backward_clock_does_not_satisfy_dwell() {
    PopupStateMachine m = make_machine();
    PopupInputs dead = healthy();
    dead.self_hp = 0;
    m.step(dead, 10000);
    check(m.step(healthy(), 5000).popup == Popup::Eliminated,
          "backward clock keeps the popup held");
}

// (15) A sleep/wake forward leap re-anchors instead of crediting the gap, so the
// popup still gets its readable window after the machine wakes up.
void long_pause_reanchors_the_dwell_window() {
    PopupStateMachine m = make_machine();
    PopupInputs dead = healthy();
    dead.self_hp = 0;
    m.step(dead, 0);
    check(m.step(healthy(), 600000).popup == Popup::Eliminated,
          "forward leap re-anchors rather than completing the dwell");
    check(m.step(healthy(), 600000 + 1000).popup == Popup::None,
          "clears one full window after the re-anchor");
}

// (16) Countdown tracks while a popup is held, so a PreMatch overlay counts down
// instead of freezing at the value it was raised with.
void countdown_tracks_while_held() {
    PopupStateMachine m = make_machine();
    PopupInputs pre = healthy();
    pre.stage = 1;
    pre.countdown_sec = 10;
    check(m.step(pre, 0).countdown_sec == 10, "countdown reported on raise");
    pre.countdown_sec = 7;
    const PopupDecision d = m.step(pre, 250);
    check(d.popup == Popup::PreMatch, "still PreMatch while counting down");
    check(d.countdown_sec == 7, "countdown advances while the popup is held");
}

// (17) shownSince is the evidence the dwell rule is armed; empty once nothing is up.
void shown_since_reflects_held_state() {
    PopupStateMachine m = make_machine();
    check(!m.shownSince().has_value(), "no shownSince before any popup");
    PopupInputs dead = healthy();
    dead.self_hp = 0;
    m.step(dead, 4000);
    check(m.shownSince().has_value() && *m.shownSince() == 4000,
          "shownSince anchors at the raising instant");
    m.step(healthy(), 4000 + kDwell);
    check(!m.shownSince().has_value(), "shownSince clears once no popup is shown");
}

// (18) Recovery path: link dies mid-match, then returns. The terminal must not
// stay stuck on LinkLost once data flows again.
void link_recovery_returns_to_none() {
    PopupStateMachine m = make_machine();
    m.step(healthy(), 0);
    PopupInputs stale = healthy();
    stale.match_freshness = Freshness::Stale;
    check(m.step(stale, 1000).popup == Popup::LinkLost, "link loss raises LinkLost");
    check(m.step(healthy(), 1000 + kDwell).popup == Popup::None,
          "restored link clears LinkLost after the dwell window");
}

}  // namespace

int main() {
    try {
        starts_waiting_for_data();
        healthy_match_shows_nothing();
        stale_link_outranks_match_popups();
        match_popup_priority_order();
        eliminated_beats_paused();
        settlement_beats_eliminated();
        missing_hp_is_not_death();
        unknown_stage_does_not_claim_prematch();
        prematch_covers_stages_zero_through_three();
        escalation_is_immediate();
        dwell_boundary_is_exact();
        zero_dwell_clears_immediately();
        backward_clock_does_not_satisfy_dwell();
        long_pause_reanchors_the_dwell_window();
        countdown_tracks_while_held();
        shown_since_reflects_held_state();
        link_recovery_returns_to_none();
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    std::cout << "popup_state: all checks passed\n";
    return 0;
}
