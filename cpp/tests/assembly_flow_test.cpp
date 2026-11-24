#include "assembly_flow.h"
#include "purchase_popups.h"

#include "rm_terminal.pb.h"

#include <QApplication>
#include <QDir>
#include <QMouseEvent>
#include <QPixmap>

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace rm_terminal;

namespace {

void check(bool condition, const char* name) {
    if (!condition) throw std::runtime_error(name);
    std::cout << "PASS " << name << '\n';
}

struct Capture {
    std::vector<rm::AssemblyCommand> sent;
    bool accept = true;

    AssemblyFlow::Sender sender() {
        return [this](const char* topic, const std::string& payload) {
            if (std::string(topic) != "AssemblyCommand") throw std::runtime_error("assembly uses the message-name topic");
            rm::AssemblyCommand command;
            if (!command.ParseFromString(payload)) throw std::runtime_error("payload parses as AssemblyCommand");
            sent.push_back(command);
            return accept;
        };
    }
};

template <class T>
Field<T> present(T value) {
    Field<T> field;
    field.value = value;
    field.quality = Quality::Valid;
    field.freshness = Freshness::Fresh;
    return field;
}

// 比赛中、链路通、活着,开赛两分钟:能选一到三级,已经装成一级。
AssemblyState::Inputs ready(std::uint32_t top = 3, std::uint32_t done = 1) {
    AssemblyState::Inputs in;
    in.link_up = true;
    in.in_match = true;
    in.view.maximum_difficulty = top;
    in.view.tech_level = done;
    in.view.basic_state = 1;
    in.view.putin = false;
    in.view.move = false;
    in.view.rotate = false;
    return in;
}

QString artifact_dir() {
    const QString dir = QStringLiteral(RM_TERMINAL_ARTIFACT_DIR) + QStringLiteral("/roles");
    if (!QDir().mkpath(dir)) throw std::runtime_error("cannot create artifacts/roles");
    return dir;
}

void save(QWidget& widget, const QString& name) {
    QApplication::processEvents();
    const QString path = QStringLiteral("%1/%2.png").arg(artifact_dir(), name);
    check(widget.grab().save(path), "PNG saved");
    std::cout << "  wrote " << path.toStdString() << '\n';
}

void the_row_says_why_it_is_grey() {
    AssemblyState state;
    AssemblyState::Inputs in = ready();
    in.link_up = false;
    state.update(in, 1000);
    check(state.row().blocked && state.row().note == QStringLiteral("断线，指令发不出去"), "link down greys the row");
    in = ready();
    in.in_match = false;
    state.update(in, 1100);
    check(state.row().note == QStringLiteral("比赛开始后才能装配"), "no assembly before the match");
    in = ready();
    in.dead = true;
    state.update(in, 1200);
    check(state.row().note == QStringLiteral("阵亡，复活后才能装配"), "no assembly while dead");
    in = ready();
    in.view.maximum_difficulty.reset();
    state.update(in, 1300);
    check(state.row().note == QStringLiteral("还没收到可选的装配难度"), "missing maximum is not read as 0");
    in = ready(4, 3);
    in.view.enemy_core_status = 2;
    state.update(in, 1400);
    check(state.row().blocked && state.row().note.contains(QStringLiteral("对方正在装配四级难度")),
          "the other side assembling level 4 blocks everyone (rule 5.4.3)");
    check(!state.start(), "a blocked row starts nothing");
}

void opening_the_box_picks_the_next_level() {
    AssemblyState state;
    state.update(ready(3, 1), 1000);
    state.boxOpened();
    AssemblyRow row = state.row();
    check(row.shown && !row.blocked, "the row is live in the match");
    check(row.selectable == (std::array<bool, 4>{true, true, false, false}),
          "levels done and the next one are selectable, not beyond");
    check(row.selected == std::optional<std::uint32_t>(2), "the next level not done yet is picked");
    check(row.note == QStringLiteral("二级 · 首次：升 7 级，等级上限提到 7，每 10 秒再 +20 金"),
          "the note shows the first-time reward from table 5-12");
    state.choose(1);
    check(state.row().note == QStringLiteral("一级 · 再装：每 10 秒再 +4 金"), "a level done before shows the repeat reward");
    state.choose(4);
    check(state.row().selected == std::optional<std::uint32_t>(1) &&
              state.row().note == QStringLiteral("先装成三级才能选四级"),
          "an out-of-reach level keeps the old pick and says why");

    AssemblyState early;
    early.update(ready(1, 1), 1000);
    early.choose(2);
    check(early.row().note == QStringLiteral("二级还没开放：开赛后每满 1 分钟多开一级"), "time-locked levels say so");

    AssemblyState skipped;
    skipped.update(ready(4, 1), 1000);
    check(skipped.row().selectable == (std::array<bool, 4>{true, true, false, false}),
          "levels above the next one stay grey even when time allows them (rule 5.4.3)");
    AssemblyState::Inputs unknown = ready(4, 1);
    unknown.view.tech_level.reset();
    skipped.update(unknown, 1100);
    check(skipped.row().selectable == (std::array<bool, 4>{true, true, true, true}),
          "without the tech level the terminal leaves it to the server");

    AssemblyState done;
    done.update(ready(4, 4), 1000);
    done.choose(4);
    check(!done.row().selectable[3] && done.row().note == QStringLiteral("四级只能装成一次"),
          "level 4 can only be completed once");

    AssemblyState busy_enemy;
    AssemblyState::Inputs in = ready(4, 3);
    in.view.enemy_core_status = 1;
    busy_enemy.update(in, 1000);
    busy_enemy.choose(4);
    check(!busy_enemy.row().selectable[3] && busy_enemy.row().selectable[2] &&
              busy_enemy.row().note == QStringLiteral("对方正在装配，等它装完才能选四级"),
          "the other side assembling blocks only level 4");
}

void enter_then_nothing_moves_means_not_started() {
    AssemblyState state;
    state.update(ready(3, 1), 1000);
    state.boxOpened();
    const auto command = state.start();
    check(command && command->operation == 0 && command->difficulty == 2, "Enter starts the picked level");
    state.sent(*command);
    check(state.waiting() && !state.active(), "after Enter it waits for the tech core");
    check(state.progress().title == QStringLiteral("开始装配…") && state.progress().footer == QStringLiteral("二级难度"),
          "the strip says it is starting");
    state.update(ready(3, 1), 3000);
    check(state.waiting(), "still waiting two seconds later");
    state.update(ready(3, 1), 4600);
    check(!state.waiting(), "3.5 seconds without motion ends the wait");
    const AssemblyProgressView view = state.progress();
    check(view.visible && view.title == QStringLiteral("没开始装配") &&
              view.detail == QStringLiteral("要站在装配区上；阵亡、罚下、虚弱时也开不了") &&
              view.tone == AssemblyProgressView::Tone::Bad,
          "the strip says why the terminal thinks it did not start");
    state.update(ready(3, 1), 11000);
    check(!state.progress().visible, "the hint goes away");

    AssemblyState none;
    none.update(ready(3, 1), 1000);
    check(!none.start() && none.row().note == QStringLiteral("先按 1–4 选难度"), "Enter with nothing picked says what to do");
}

void the_tech_core_drives_the_strip() {
    AssemblyState state;
    state.update(ready(3, 1), 1000);
    state.boxOpened();
    state.sent(*state.start());
    AssemblyState::Inputs in = ready(3, 1);
    in.view.basic_state = 2;
    state.update(in, 2000);
    check(state.active() && !state.waiting(), "motion means the assembly is on");
    AssemblyProgressView view = state.progress();
    check(view.title == QStringLiteral("运动中…") && view.detail == QStringLiteral("前往装配点"), "moving says so");
    check(view.steps.size() == 2 && !view.steps[0].second && view.steps[1].first == QStringLiteral("平移"),
          "level 2 needs 放入 and 平移 (table 5-10)");
    check(!state.confirm().send, "Y while moving sends nothing");
    check(state.progress().detail == QStringLiteral("科技核心还没到位"), "and says the core is not there yet");

    in.view.basic_state = 3;
    in.view.putin = true;
    state.update(in, 3000);
    view = state.progress();
    check(view.title == QStringLiteral("请装配") && view.steps[0].second && !view.steps[1].second && !view.confirm_lit,
          "a step done lights up, Y stays grey");
    const AssemblyState::Key early = state.confirm();
    check(early.consumed && !early.send, "Y belongs to the assembly but sends nothing yet");
    check(state.progress().detail == QStringLiteral("还差：平移"), "the strip names the missing step");

    in.view.move = true;
    state.update(in, 6500);
    view = state.progress();
    check(view.title == QStringLiteral("确认装配前请移出装配机构") && view.confirm_lit, "all steps done lights Y");
    const AssemblyState::Key yes = state.confirm();
    check(yes.consumed && yes.send && yes.send->operation == 1 && yes.send->difficulty == 2, "Y confirms");
    const AssemblyState::Key leave = state.exit();
    check(leave.consumed && leave.send && leave.send->operation == 2, "L exits");
    state.busy();
    check(state.progress().detail == QStringLiteral("装配中不能开兑换框，按 L 退出装配"), "H while assembling explains");

    AssemblyState idle;
    idle.update(ready(), 1000);
    check(!idle.confirm().consumed && !idle.exit().consumed, "Y and L fall through when nothing is going on");
}

void a_result_ends_the_assembly() {
    AssemblyState state;
    AssemblyState::Inputs in = ready(3, 1);
    in.view.last_result = QStringLiteral("装配失败：超时");
    in.view.last_result_at = 500;
    state.update(in, 1000);
    check(!state.progress().visible, "a result from before the terminal opened is not shown");

    state.boxOpened();
    state.sent(*state.start());
    in.view.basic_state = 3;
    in.view.putin = true;
    in.view.move = true;
    state.update(in, 2000);
    in.view.last_result = QStringLiteral("装配成功");
    in.view.last_result_at = 2400;
    in.view.last_result_level = 1;
    state.update(in, 2500);
    check(!state.active(), "the core still at its pose after Event 15 no longer counts as assembling");
    AssemblyProgressView view = state.progress();
    check(view.title == QStringLiteral("二级科技核心装配成功") && view.tone == AssemblyProgressView::Tone::Good,
          "success names the level, in green");
    in.view.basic_state = 1;
    state.update(in, 8000);
    check(!state.progress().visible, "the result fades after five seconds");

    // 科技核心先回了初始位置,事件 15 晚一点才到:还是记在刚才那次头上。
    AssemblyState late;
    late.update(ready(3, 1), 1000);
    late.boxOpened();
    late.sent(*late.start());
    AssemblyState::Inputs late_in = ready(3, 1);
    late_in.view.basic_state = 3;
    late.update(late_in, 2000);
    late_in.view.basic_state = 1;
    late.update(late_in, 3000);
    late_in.view.last_result = QStringLiteral("装配成功");
    late_in.view.last_result_at = 3800;
    late_in.view.last_result_level = 1;
    late.update(late_in, 3800);
    check(late.progress().title == QStringLiteral("二级科技核心装配成功") &&
              late.progress().footer == QStringLiteral("二级难度"),
          "a result arriving just after the core went home still names the level");

    in.view.last_result = QStringLiteral("装配失败：离开装配区过久");
    in.view.last_result_at = 9000;
    in.view.last_result_level = 2;
    state.update(in, 9000);
    view = state.progress();
    check(view.title == QStringLiteral("装配失败：离开装配区过久") && view.tone == AssemblyProgressView::Tone::Bad,
          "a failure shows the Event 15 reason in red");
    in.view.last_result = QStringLiteral("装配已主动退出");
    in.view.last_result_at = 10000;
    in.view.last_result_level = 0;
    state.update(in, 10000);
    check(state.progress().tone == AssemblyProgressView::Tone::Normal, "exiting with L is not shown as a failure");
}

void level_four_shows_its_timers_and_the_buffer() {
    AssemblyState state;
    state.update(ready(4, 3), 1000);
    state.boxOpened();
    check(state.row().selected == std::optional<std::uint32_t>(4), "after level 3 the box picks level 4");
    state.sent(*state.start());
    AssemblyState::Inputs in = ready(4, 3);
    in.view.basic_state = 3;
    in.view.putin = true;
    in.view.remain_time_all = 43;
    in.view.remain_time_step = 4;
    state.update(in, 2000);
    AssemblyProgressView view = state.progress();
    check(view.timer == QStringLiteral("装配倒计时 43s · 本步还剩 4s"), "level 4 shows the 45 s clock and the step window");
    check(view.steps.size() == 3, "level 4 needs all three steps");

    in.view.buffer_at = 2500;
    in.view.remain_time_step = 12;
    in.dead = true;
    state.update(in, 3000);
    view = state.progress();
    check(view.title == QStringLiteral("已临时激活") && !view.confirm_key && view.timer == QStringLiteral("缓冲还剩 12s"),
          "Event 14 turns the strip into the buffer countdown");
    check(!state.confirm().consumed, "while dead Y is left to the revive popup");
    check(state.exit().send.has_value(), "L still exits");
}

void only_the_terminals_own_start_counts() {
    AssemblyState state;
    AssemblyState::Inputs in = ready(3, 1);
    in.view.basic_state = 2;
    state.update(in, 1000);
    check(!state.active() && !state.canExit() && !state.progress().visible,
          "the core moving without a start from this terminal is not taken as our assembly");
    check(!state.confirm().consumed && !state.exit().consumed, "and Y / L are left alone");
}

void a_failure_waits_for_l_while_the_core_is_out() {
    AssemblyState state;
    state.update(ready(3, 1), 1000);
    state.boxOpened();
    state.sent(*state.start());
    AssemblyState::Inputs in = ready(3, 1);
    in.view.basic_state = 3;
    in.view.putin = true;
    state.update(in, 2000);
    in.view.last_result = QStringLiteral("装配失败：重装战亡");
    in.view.last_result_at = 2500;
    in.view.last_result_level = 2;
    in.dead = true;
    state.update(in, 2500);
    AssemblyProgressView view = state.progress();
    check(view.title == QStringLiteral("装配失败：重装战亡") && view.keys && !view.confirm_key,
          "a failure with the core still out shows only L (rule 5.4.3: cancel by hand)");
    check(state.canExit() && state.exit().send.has_value(), "L can still cancel");
    state.update(in, 20000);
    check(state.progress().visible, "the failure stays up while the core is out");
    in.view.basic_state = 1;
    state.update(in, 21000);
    check(state.progress().visible && !state.progress().keys && !state.canExit(), "the core back home drops L");
    state.update(in, 26100);
    check(!state.progress().visible, "and the failure fades five seconds later");
}

void the_view_reads_events_14_and_15() {
    Snapshot snapshot;
    snapshot.self = RobotId{1};
    snapshot.tech_core.maximum_difficulty = present<std::uint32_t>(2);
    snapshot.events.push(EventRecord{15, 2, "装配失败：超时", 4000, kRed, std::nullopt});
    snapshot.events.push(EventRecord{14, 2, "我方重装战亡，装配进入强制退出缓冲期", 5000, kRed, std::nullopt});
    snapshot.events.push(EventRecord{15, 1, "装配成功", 6000, kRed, std::nullopt});
    const AssemblyView view = assembly_view(snapshot);
    check(view.maximum_difficulty == std::optional<std::uint32_t>(2), "tech core fields land");
    check(view.last_result == QStringLiteral("装配成功") && view.last_result_at == 6000 && view.last_result_level == 1,
          "the newest Event 15 wins and keeps its level");
    check(view.buffer_at == std::optional<MonotonicMs>(5000), "Event 14 marks the buffer");
}

void the_flow_sends_once_and_never_while_down() {
    QWidget host;
    host.resize(1440, 900);
    AssemblyFlow flow(&host);
    Capture capture;
    flow.setSender(capture.sender());
    std::vector<bool> exits;
    QObject::connect(&flow, &AssemblyFlow::canExitChanged, &host, [&exits](bool on) { exits.push_back(on); });
    flow.update(ready(3, 1), 1000);
    flow.boxOpened();
    check(flow.start(), "Enter sends");
    check(capture.sent.size() == 1 && capture.sent[0].operation() == 0 && capture.sent[0].difficulty() == 2,
          "AssemblyCommand operation 0 with the picked difficulty");
    check(exits == std::vector<bool>{true}, "L becomes live while waiting");
    check(flow.exit(), "L is taken");
    check(capture.sent.size() == 1, "a second command inside 100 ms is dropped, not queued");
    flow.update(ready(3, 1), 1200);
    check(flow.exit() && capture.sent.size() == 2 && capture.sent[1].operation() == 2, "the next window lets L through");

    AssemblyState::Inputs down = ready(3, 1);
    down.link_up = false;
    flow.update(down, 6000);
    flow.boxOpened();
    check(!flow.start() && capture.sent.size() == 2, "nothing goes out while the link is down");
    flow.update(ready(3, 1), 7000);
    check(capture.sent.size() == 2, "nothing is replayed when the link returns");
}

SelfCommandView heavy_view() {
    SelfCommandView view;
    view.infantry = false;
    view.stage = 4;
    view.remaining = 300;
    view.economy = 800;
    view.ammo = 3;
    view.fired = 0;
    view.hp = 200;
    view.max_hp = 400;
    view.out_of_combat = true;
    view.can_remote_heal = true;
    view.can_remote_ammo = true;
    view.shooter_online = true;
    view.logistics = 1.0;
    view.dead = false;
    return view;
}

void the_heavy_box_has_the_row_and_two_tiles() {
    QWidget host;
    host.setStyleSheet(QStringLiteral("background:#07090d;"));
    host.resize(1000, 560);
    host.show();
    PurchaseFlow flow(&host);
    std::vector<bool> opened;
    QObject::connect(&flow, &PurchaseFlow::remoteOpenChanged, &host, [&opened](bool open) { opened.push_back(open); });
    std::vector<std::uint32_t> picked;
    int started = 0;
    flow.setAssemblyActions([&picked](std::uint32_t level) { picked.push_back(level); }, [&started] { ++started; });
    AssemblyState state;
    state.update(ready(3, 1), 1000);
    state.boxOpened();
    flow.update(heavy_view(), true, 1000);
    flow.setAssemblyRow(state.row());
    flow.toggleRemote();
    check(opened == std::vector<bool>{true}, "opening the box is announced");
    RemoteExchangePopup& popup = *flow.remotePopup();
    check(popup.isVisible() && popup.assembly().shown, "the heavy box carries the assembly row");
    check(!popup.hasItem(1) && popup.tileRect(1).isNull(), "no 17mm in the heavy box (table 3-3)");
    check(!flow.chooseRemote(1), "Ctrl+2 does nothing for the heavy");
    const QRect heal = popup.tileRect(0);
    const QRect large = popup.tileRect(2);
    check(!heal.isNull() && !large.isNull() && heal.top() == large.top() && heal.right() < large.left(),
          "回血 and 42mm sit side by side");
    check(popup.levelRect(4).bottom() < heal.top(), "the assembly row is above the tiles");
    check(popup.barText() == QStringLiteral("按 H 关闭"), "the bar says how to close it");
    save(host, QStringLiteral("assembly-h-box"));

    const auto click = [&popup](const QRect& box) {
        const QPointF at = box.center();
        QMouseEvent release(QEvent::MouseButtonRelease, at, popup.mapToGlobal(at), Qt::LeftButton, Qt::NoButton,
                            Qt::NoModifier);
        QApplication::sendEvent(&popup, &release);
    };
    click(popup.levelRect(3));
    click(popup.startRect());
    check(picked == std::vector<std::uint32_t>{3} && started == 1, "the mouse picks a level and starts");
    check(flow.chooseRemote(0), "Ctrl+1 still asks to buy health");
    flow.no();
    flow.closeAll();
    check(opened == (std::vector<bool>{true, false}), "closing is announced");

    PurchaseFlow infantry(&host);
    SelfCommandView small = heavy_view();
    small.infantry = true;
    infantry.update(small, true, 1000);
    infantry.toggleRemote();
    check(infantry.remotePopup()->hasItem(1) && !infantry.remotePopup()->assembly().shown,
          "the infantry box keeps its three tiles and no assembly row");
}

void the_strip_paints_each_state() {
    QWidget host;
    host.setStyleSheet(QStringLiteral("background:#07090d;"));
    host.resize(520, 220);
    host.show();
    AssemblyProgress strip(&host);
    strip.show();
    const auto shoot = [&](const AssemblyProgressView& view, const QString& name) {
        strip.setView(view);
        strip.resize(strip.sizeHint());
        strip.move((host.width() - strip.width()) / 2, 10);
        check(strip.height() > 60, "the strip has room for its rows");
        save(host, QStringLiteral("assembly-progress-") + name);
    };
    AssemblyState state;
    state.update(ready(4, 3), 1000);
    state.boxOpened();
    state.sent(*state.start());
    AssemblyState::Inputs in = ready(4, 3);
    in.view.basic_state = 2;
    state.update(in, 2000);
    shoot(state.progress(), QStringLiteral("moving"));
    in.view.basic_state = 3;
    in.view.putin = true;
    in.view.remain_time_all = 43;
    in.view.remain_time_step = 4;
    state.update(in, 3000);
    shoot(state.progress(), QStringLiteral("level4"));
    in.view.move = true;
    in.view.rotate = true;
    state.update(in, 4000);
    shoot(state.progress(), QStringLiteral("ready"));
    in.view.last_result = QStringLiteral("装配成功");
    in.view.last_result_at = 4500;
    in.view.last_result_level = 1;
    state.update(in, 4600);
    shoot(state.progress(), QStringLiteral("success"));

    AssemblyState lost;
    lost.update(ready(3, 1), 1000);
    lost.boxOpened();
    lost.sent(*lost.start());
    lost.update(ready(3, 1), 5000);
    shoot(lost.progress(), QStringLiteral("not-started"));
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        the_row_says_why_it_is_grey();
        opening_the_box_picks_the_next_level();
        enter_then_nothing_moves_means_not_started();
        the_tech_core_drives_the_strip();
        a_result_ends_the_assembly();
        level_four_shows_its_timers_and_the_buffer();
        only_the_terminals_own_start_counts();
        a_failure_waits_for_l_while_the_core_is_out();
        the_view_reads_events_14_and_15();
        the_flow_sends_once_and_never_while_down();
        the_heavy_box_has_the_row_and_two_tiles();
        the_strip_paints_each_state();
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    std::cout << "assembly_flow_test: all passed\n";
    return 0;
}
