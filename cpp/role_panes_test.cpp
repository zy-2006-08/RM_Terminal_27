#include "assembly_pane.h"
#include "dart_pane.h"
#include "revive_popup.h"
#include "self_command_pane.h"
#include "sentry_pane.h"

#include "rm_terminal.pb.h"

#include <QApplication>
#include <QDir>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QVBoxLayout>

#include <cmath>
#include <iostream>
#include <memory>
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
    std::vector<std::pair<std::string, std::string>> sent;
    bool accept = true;

    std::function<bool(const char*, const std::string&)> sender() {
        return [this](const char* topic, const std::string& payload) {
            sent.emplace_back(topic, payload);
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

AssemblyView full_assembly() {
    AssemblyView view;
    view.maximum_difficulty = 4;
    view.tech_level = 3;
    view.modular_42mm = true;
    view.basic_state = 2;
    view.putin = true;
    view.move = true;
    view.rotate = false;
    view.remain_time_step = 12;
    view.remain_time_all = 48;
    view.enemy_core_status = 2;
    view.last_result = QStringLiteral("装配成功");
    view.last_result_at = 5000;
    return view;
}

DartView full_dart() {
    DartView view;
    view.gate = 2;
    view.target_id = 3;
    view.event_lines = {QStringLiteral("我方飞镖命中敌方基地"),
                        QStringLiteral("敌方飞镖闸门开启")};
    return view;
}

SentryView full_sentry() {
    SentryView view;
    view.posture = 1;
    view.weakened = false;
    view.powered = true;
    view.hp = 480;
    view.max_hp = 600;
    view.bullets = 320;
    view.economy = 1250;
    view.result = SentryCtrlOutcome{10, 0, 4000};
    return view;
}

// 面板在布局里都装在卡片下方,按钮位置要靠真实布局算出来。grab 之前必须让
// 布局跑完一轮,否则 paintEvent 读到的按钮几何全是 0,画布高度算成 0。
template <class Pane>
void settle(Pane& pane, int w, int h) {
    pane.resize(w, h);
    pane.layout()->activate();
    pane.show();
    QApplication::processEvents();
}

QString artifact_dir() {
    const QString dir = QStringLiteral(RM_TERMINAL_ARTIFACT_DIR) + QStringLiteral("/roles");
    if (!QDir().mkpath(dir)) throw std::runtime_error("cannot create artifacts/roles");
    return dir;
}

void shoot(QWidget& widget, const QString& name, int w, int h) {
    widget.resize(w, h);
    widget.layout()->activate();
    QApplication::processEvents();
    const QPixmap shot = widget.grab();
    const QString path =
        QStringLiteral("%1/%2-%3x%4.png").arg(artifact_dir(), name).arg(w).arg(h);
    check(!shot.isNull() && shot.save(path), "pane PNG saved");
    std::cout << "  wrote " << path.toStdString() << '\n';
}

// 三个面板都装在 dashboard 的「card + heading」卡片里。截图也照这个包一层,
// 否则图上看不出标题和面板的实际间距,而那正是最容易挤到的地方。
QWidget* carded(QWidget* pane, const QString& heading) {
    auto* box = new QWidget;
    box->setStyleSheet(QStringLiteral("background:#0e121a;"));
    auto* column = new QVBoxLayout(box);
    column->setContentsMargins(9, 4, 9, 4);
    column->setSpacing(2);
    auto* title = new QLabel(heading, box);
    title->setStyleSheet(
        QStringLiteral("color:#94a3b8; font-family:'PingFang SC'; font-weight:600;"
                       "letter-spacing:2px; padding:2px 0px 5px 0px; border-bottom:1px solid #263042;"));
    column->addWidget(title);
    pane->setParent(box);
    column->addWidget(pane, 1);
    return box;
}

void assembly_shows_every_field_and_sends_once() {
    AssemblyPane pane;
    Capture capture;
    pane.setView(full_assembly());
    pane.setSender(capture.sender());
    pane.setLinkUp(true);
    pane.setArm(29.5, std::array<float, 6>{10.0F, -20.5F, 30.0F, 0.0F, 45.5F, -90.0F});
    pane.tick(10000);
    settle(pane, 420, 300);

    check(pane.buttonsEnabled(), "assembly buttons are live when the link is up");
    check(pane.statusText() == QStringLiteral("就绪"), "assembly reports ready");

    auto* start = pane.findChild<QPushButton*>(QStringLiteral("assemblyStart"));
    auto* confirm = pane.findChild<QPushButton*>(QStringLiteral("assemblyConfirm"));
    auto* cancel = pane.findChild<QPushButton*>(QStringLiteral("assemblyCancel"));
    auto* plus = pane.findChild<QPushButton*>(QStringLiteral("assemblyDifficultyUp"));
    check(start && confirm && cancel && plus, "assembly buttons exist by name");
    check(start->text().contains(QStringLiteral("难度 1")), "difficulty starts at 1");

    plus->click();
    plus->click();
    check(pane.selectedDifficulty() == 3, "the selector walks up");
    check(start->text().contains(QStringLiteral("难度 3")), "the button shows the selection");

    start->click();
    check(capture.sent.size() == 1, "one click sends exactly one command");
    check(capture.sent[0].first == "AssemblyCommand", "assembly uses the message-name topic");
    check(pane.statusText() == QStringLiteral("等待回显"), "sending waits for the echo");

    // 限频:同一毫秒里的第二次点击必须被丢掉,不是排队。
    confirm->click();
    check(capture.sent.size() == 1, "a second click inside 100ms is dropped");
    pane.tick(10100);
    confirm->click();
    check(capture.sent.size() == 2, "the next window lets a command through");

    // 难度上限来自协议。+ 不能越过 maximum_difficulty,否则会发出被服务器拒绝的难度。
    for (int i = 0; i < 6; ++i) plus->click();
    check(pane.selectedDifficulty() == 4, "the selector stops at the protocol maximum");
}

void assembly_cancel_needs_a_full_second() {
    AssemblyPane pane;
    Capture capture;
    pane.setView(full_assembly());
    pane.setSender(capture.sender());
    pane.setLinkUp(true);
    settle(pane, 420, 300);
    auto* cancel = pane.findChild<QPushButton*>(QStringLiteral("assemblyCancel"));

    pane.tick(1000);
    cancel->pressed();
    pane.tick(1500);
    cancel->released();
    check(capture.sent.empty(), "releasing 取消装配 early sends nothing");

    pane.tick(2000);
    cancel->pressed();
    pane.tick(3000);
    cancel->released();
    check(capture.sent.size() == 1, "holding 取消装配 a full second sends it");
    rm::AssemblyCommand parsed;
    check(parsed.ParseFromString(capture.sent[0].second) && parsed.operation() == 2,
          "the held button sends operation 2");
}

void assembly_waiting_lines_name_their_source() {
    AssemblyPane pane;
    pane.setView(AssemblyView{});
    pane.setLinkUp(false);
    pane.tick(1000);
    settle(pane, 420, 300);
    check(!pane.buttonsEnabled(), "an empty view with no link keeps the buttons dead");
    check(pane.statusText() == QStringLiteral("断线，指令不会发送"),
          "link down says so in red");
}

void assembly_drops_clicks_while_the_link_is_down() {
    AssemblyPane pane;
    Capture capture;
    pane.setView(full_assembly());
    pane.setSender(capture.sender());
    pane.setLinkUp(false);
    pane.tick(1000);
    settle(pane, 420, 300);

    auto* start = pane.findChild<QPushButton*>(QStringLiteral("assemblyStart"));
    check(!start->isEnabled(), "link down disables 开始兑换");
    // 即使按钮被程序外的途径触发,发送路径本身也要拒绝,并且不排队补发。
    start->click();
    check(capture.sent.empty(), "clicks while the link is down are dropped");
    pane.setLinkUp(true);
    pane.tick(2000);
    check(capture.sent.empty(), "nothing is replayed when the link returns");
}

void dart_gates_the_launch_button() {
    DartPane pane;
    Capture capture;
    DartView view = full_dart();
    view.gate = 0;
    pane.setView(view);
    pane.setSender(capture.sender());
    pane.setLinkUp(true);
    pane.tick(1000);
    settle(pane, 380, 300);

    auto* launch = pane.findChild<QPushButton*>(QStringLiteral("dartLaunch"));
    auto* open = pane.findChild<QPushButton*>(QStringLiteral("dartOpenGate"));
    auto* target2 = pane.findChild<QPushButton*>(QStringLiteral("dartTarget2"));
    check(launch && open && target2, "dart buttons exist by name");
    check(!launch->isEnabled(), "确认发射 is dead while the gate is shut");
    check(open->isEnabled() && target2->isEnabled(), "the other dart buttons are live");

    target2->click();
    check(capture.sent.size() == 1 && capture.sent[0].first == "DartCommand",
          "a target click sends DartCommand");
    rm::DartCommand parsed;
    check(parsed.ParseFromString(capture.sent[0].second) && parsed.target_id() == 2 &&
              !parsed.launch_confirm(),
          "选目标 carries the target without confirming a launch");

    view.gate = 2;
    pane.setView(view);
    pane.tick(2000);
    check(launch->isEnabled(), "确认发射 opens once the gate reports 已开启");

    launch->pressed();
    pane.tick(2500);
    launch->released();
    check(capture.sent.size() == 1, "an early release never launches");

    pane.tick(3000);
    launch->pressed();
    pane.tick(4000);
    launch->released();
    check(capture.sent.size() == 2, "a full second launches");
    rm::DartCommand launched;
    check(launched.ParseFromString(capture.sent[1].second) && launched.launch_confirm() &&
              launched.open(),
          "the launch sets launch_confirm");
}

void dart_echo_goes_unconfirmed_without_a_change() {
    DartPane pane;
    Capture capture;
    pane.setView(full_dart());
    pane.setSender(capture.sender());
    pane.setLinkUp(true);
    pane.tick(1000);
    settle(pane, 380, 300);

    pane.findChild<QPushButton*>(QStringLiteral("dartTarget1"))->click();
    check(pane.statusText() == QStringLiteral("等待回显"), "a sent dart command waits");
    pane.tick(2000);
    check(pane.statusText() == QStringLiteral("等待回显"), "still waiting before 2s");
    pane.tick(3000);
    check(pane.statusText() == QStringLiteral("未确认"),
          "no DartSelectTargetStatusSync change within 2s reads as 未确认");

    DartView changed = full_dart();
    changed.target_id = 1;
    pane.setView(changed);
    pane.tick(3100);
    check(pane.statusText() == QStringLiteral("未确认"),
          "a change after the timeout does not rewrite the verdict");

    DartPane fresh;
    Capture capture2;
    fresh.setView(full_dart());
    fresh.setSender(capture2.sender());
    fresh.setLinkUp(true);
    fresh.tick(1000);
    settle(fresh, 380, 300);
    fresh.findChild<QPushButton*>(QStringLiteral("dartTarget1"))->click();
    DartView moved = full_dart();
    moved.target_id = 1;
    fresh.setView(moved);
    fresh.tick(1500);
    check(fresh.statusText() == QStringLiteral("已确认"), "a target change confirms the echo");
}

void dart_link_down_disables_everything() {
    DartPane pane;
    Capture capture;
    pane.setView(full_dart());
    pane.setSender(capture.sender());
    pane.setLinkUp(false);
    pane.tick(1000);
    settle(pane, 380, 300);
    check(!pane.buttonsEnabled(), "dart buttons die with the link");
    check(!pane.findChild<QPushButton*>(QStringLiteral("dartLaunch"))->isEnabled(),
          "确认发射 dies with the link even with the gate open");
    check(pane.statusText() == QStringLiteral("断线，指令不会发送"), "dart says the link is down");
    pane.findChild<QPushButton*>(QStringLiteral("dartTarget5"))->click();
    check(capture.sent.empty(), "dart clicks while down are dropped");
}

void sentry_cools_down_posture_buttons_after_a_success() {
    SentryPane pane;
    Capture capture;
    SentryView view = full_sentry();
    view.result.reset();
    pane.setView(view);
    pane.setSender(capture.sender());
    pane.setLinkUp(true);
    pane.tick(1000);
    settle(pane, 380, 360);

    auto* attack = pane.findChild<QPushButton*>(QStringLiteral("sentryPosture7"));
    auto* defend = pane.findChild<QPushButton*>(QStringLiteral("sentryPosture8"));
    check(attack && defend, "posture buttons exist by name");
    check(attack->isEnabled(), "posture buttons start live");

    defend->click();
    check(capture.sent.size() == 1 && capture.sent[0].first == "SentryCtrlCommand",
          "a posture click sends SentryCtrlCommand");
    rm::SentryCtrlCommand parsed;
    check(parsed.ParseFromString(capture.sent[0].second) && parsed.command_id() == 8,
          "防御 sends command_id 8");

    view.result = SentryCtrlOutcome{8, 0, 1200};
    pane.setView(view);
    pane.tick(1200);
    check(pane.postureCoolingDown(), "a successful posture result starts the 5s cooldown");
    check(!attack->isEnabled() && !defend->isEnabled(), "posture buttons grey out while cooling");
    check(pane.findChild<QPushButton*>(QStringLiteral("sentryAmmo"))->isEnabled(),
          "the cooldown only touches the posture buttons");

    pane.tick(6199);
    check(pane.postureCoolingDown() && !attack->isEnabled(),
          "the cooldown runs a full 5 seconds from the result, not from the click");
    pane.tick(6200);
    check(!pane.postureCoolingDown() && attack->isEnabled(),
          "posture buttons return exactly 5 seconds after the result");

    // 失败的切换不该锁住按钮:操作手得立刻能再试一次。
    SentryPane other;
    Capture capture2;
    SentryView failed = full_sentry();
    failed.result = SentryCtrlOutcome{9, 3, 2000};
    other.setView(failed);
    other.setSender(capture2.sender());
    other.setLinkUp(true);
    other.tick(2000);
    settle(other, 380, 360);
    check(!other.postureCoolingDown(), "a failed posture result does not start a cooldown");
    check(other.lastResultText() == QStringLiteral("移动 失败（码 3）"),
          "a failure code is shown verbatim with its command name");
    check(other.findChild<QPushButton*>(QStringLiteral("sentryPosture9"))->isEnabled(),
          "a failed posture switch can be retried at once");

    SentryPane quiet;
    check(quiet.lastResultText() == QStringLiteral("-- 等待 SentryCtrlResult"),
          "no result yet names the message it waits for");
}

void sentry_gold_revive_needs_a_hold_and_reports_unconfirmed() {
    SentryPane pane;
    Capture capture;
    SentryView view = full_sentry();
    view.result.reset();
    pane.setView(view);
    pane.setSender(capture.sender());
    pane.setLinkUp(true);
    pane.tick(1000);
    settle(pane, 380, 360);

    auto* gold = pane.findChild<QPushButton*>(QStringLiteral("sentryGoldRevive"));
    gold->pressed();
    pane.tick(1500);
    gold->released();
    check(capture.sent.empty(), "an early release never spends gold");

    pane.tick(2000);
    gold->pressed();
    pane.tick(3000);
    gold->released();
    check(capture.sent.size() == 1, "holding a full second spends gold");
    rm::SentryCtrlCommand parsed;
    check(parsed.ParseFromString(capture.sent[0].second) && parsed.command_id() == 6,
          "花金币复活 sends command_id 6");
    check(pane.statusText().contains(QStringLiteral("等待回显")), "the gold revive waits for a result");

    pane.tick(5100);
    check(pane.statusText().contains(QStringLiteral("未确认")),
          "no SentryCtrlResult within 2s reads as 未确认");
}

void sentry_reads_the_right_seat_for_each_faction() {
    Snapshot snapshot;
    snapshot.self = RobotId{1};
    snapshot.sentry.posture = present<std::uint32_t>(2);
    snapshot.sentry.weakened = present(true);
    snapshot.sentry.powered = present(false);
    snapshot.game.economy = present<std::uint32_t>(700);
    RobotHealth red_sentry;
    red_sentry.id = RobotId{7};
    red_sentry.faction = kRed;
    red_sentry.current_hp = present<std::uint32_t>(300);
    red_sentry.max_hp = present<std::uint32_t>(600);
    red_sentry.bullets = present<std::int32_t>(150);
    RobotHealth blue_sentry;
    blue_sentry.id = RobotId{107};
    blue_sentry.faction = kBlue;
    blue_sentry.current_hp = present<std::uint32_t>(90);
    blue_sentry.max_hp = present<std::uint32_t>(600);
    blue_sentry.bullets = present<std::int32_t>(20);
    snapshot.robot_health = {red_sentry, blue_sentry};

    const SentryView red = sentry_view(snapshot);
    check(red.hp == 300 && red.bullets == 150, "a red seat reads sentry 7");
    check(red.posture == 2 && red.weakened == true && red.economy == 700,
          "the view carries posture, weakening and economy");

    snapshot.self = RobotId{106};
    const SentryView blue = sentry_view(snapshot);
    check(blue.hp == 90 && blue.bullets == 20, "a blue seat reads sentry 107");

    // 没选席位时不能默认按红方读:那会把敌方哨兵的血画成我方的。
    snapshot.self.reset();
    const SentryView unknown = sentry_view(snapshot);
    check(!unknown.hp && !unknown.bullets, "no seat means no sentry health");
}

void builders_keep_missing_data_missing() {
    Snapshot empty;
    const AssemblyView assembly = assembly_view(empty);
    check(!assembly.maximum_difficulty && !assembly.basic_state && !assembly.putin,
          "an empty snapshot leaves every assembly field missing");
    check(assembly.last_result.isEmpty(), "no Event 15 means no result line");

    const DartView dart = dart_view(empty);
    check(!dart.gate && !dart.target_id && dart.event_lines.empty(),
          "an empty snapshot leaves the dart view missing");

    Snapshot filled;
    filled.self = RobotId{1};
    filled.tech_core.maximum_difficulty = present<std::uint32_t>(4);
    filled.tech_core.basic_state = present<std::uint32_t>(3);
    filled.tech_core.putin_state = present<std::uint32_t>(1);
    filled.tech_core.move_state = present<std::uint32_t>(0);
    filled.tech_core.rotate_state = present<std::uint32_t>(0);
    filled.tech_core.remain_time_step = present<std::uint32_t>(7);
    filled.tech_core.enemy_core_status = present<std::uint32_t>(2);
    filled.game.tech_level = present<std::uint32_t>(2);
    filled.robots[RobotId{1}].modules.modular_42mm = present<std::uint32_t>(1);
    filled.events.push(EventRecord{15, 1, "装配成功", 4000, kRed, std::nullopt});
    filled.events.push(EventRecord{15, 2, "装配失败：超时", 5000, kRed, std::nullopt});
    filled.dart.gate = present<std::uint32_t>(1);
    filled.dart.target_id = present<std::uint32_t>(4);
    filled.events.push(EventRecord{9, 1, "我方飞镖命中敌方基地", 6000, kRed, std::nullopt});

    const AssemblyView built = assembly_view(filled);
    check(built.maximum_difficulty == 4 && built.basic_state == 3, "tech core fields land");
    // 0 和缺失必须分开:move_state 的真实 0 要是 false,不是 nullopt。
    check(built.putin == true && built.move == false && built.rotate == false,
          "a protocol zero reads as false, not as missing");
    check(!built.remain_time_all, "a field the server never sent stays missing");
    check(built.modular_42mm == true, "the 42mm permission comes from the self robot");
    // 只取最新一条 Event 15,不是第一条。
    check(built.last_result == QStringLiteral("装配失败：超时") && built.last_result_at == 5000,
          "the newest Event 15 wins");

    const DartView built_dart = dart_view(filled);
    check(built_dart.gate == 1 && built_dart.target_id == 4, "dart fields land");
    check(built_dart.event_lines.size() == 1 &&
              built_dart.event_lines[0] == QStringLiteral("我方飞镖命中敌方基地"),
          "only Event 9/10 reach the dart panel");
}

void panes_degrade_at_small_sizes() {
    AssemblyPane assembly;
    assembly.setView(full_assembly());
    assembly.setLinkUp(true);
    assembly.tick(1000);
    settle(assembly, 320, 240);
    check(assembly.height() == 240, "assembly accepts a 320x240 box");
    check(!assembly.grab().isNull(), "assembly still paints at 320x240");

    DartPane dart;
    dart.setView(full_dart());
    dart.setLinkUp(true);
    dart.tick(1000);
    settle(dart, 320, 240);
    check(!dart.grab().isNull(), "dart still paints at 320x240");

    SentryPane sentry;
    sentry.setView(full_sentry());
    sentry.setLinkUp(true);
    sentry.tick(1000);
    settle(sentry, 320, 240);
    check(!sentry.grab().isNull(), "sentry still paints at 320x240");
}

SelfCommandView infantry_in_match() {
    SelfCommandView view;
    view.infantry = true;
    view.stage = 4;
    view.remaining = 300;
    view.economy = 800;
    view.ammo = 0;
    view.hp = 120;
    view.max_hp = 200;
    view.level = 1;
    view.out_of_combat = true;
    view.can_remote_heal = true;
    view.can_remote_ammo = true;
    view.dead = false;
    view.rune_status = 1;
    view.shooter = 1;
    view.chassis = 1;
    return view;
}

QPushButton* button(QWidget& pane, const char* name) {
    auto* found = pane.findChild<QPushButton*>(QString::fromLatin1(name));
    if (!found) throw std::runtime_error(std::string("missing button ") + name);
    return found;
}

rm::CommonCommand common(const std::pair<std::string, std::string>& sent) {
    if (sent.first != "CommonCommand") throw std::runtime_error("expected CommonCommand, got " + sent.first);
    rm::CommonCommand parsed;
    if (!parsed.ParseFromString(sent.second)) throw std::runtime_error("CommonCommand does not parse");
    return parsed;
}

void self_pane_sends_exchanges_with_the_right_caliber() {
    SelfCommandPane pane;
    Capture capture;
    pane.setView(infantry_in_match());
    pane.setSender(capture.sender());
    pane.setLinkUp(true);
    settle(pane, 232, 200);
    pane.tick(1000);
    check(!pane.preMatch(), "in the 7-minute stage the infantry sees the match buttons");
    check(button(pane, "selfExchange")->text() == QStringLiteral("兑换 50发 · 50金"),
          "17mm exchange defaults to 50 rounds for 50 coins");
    button(pane, "selfExchange")->click();
    check(capture.sent.size() == 1, "one click sends one command");
    auto parsed = common(capture.sent[0]);
    check(parsed.cmd_type() == 1 && parsed.param() == 50, "infantry buys 17mm: cmd 1, param = rounds");
    button(pane, "selfExchange")->click();
    check(capture.sent.size() == 1, "a second click inside 100ms is dropped (10Hz)");
    button(pane, "selfExchangeMore")->click();
    button(pane, "selfExchangeMore")->click();
    check(pane.exchangeRounds() == 200, "+ walks the ladder 50 -> 100 -> 200");

    pane.tick(1200);
    button(pane, "selfRemoteAmmo")->click();
    parsed = common(capture.sent.back());
    check(parsed.cmd_type() == 5 && parsed.param() == 100, "remote ammo sends one 100-round unit");
    pane.tick(1400);
    button(pane, "selfRemoteHeal")->click();
    parsed = common(capture.sent.back());
    check(parsed.cmd_type() == 6 && parsed.param() == 0, "remote heal is cmd 6");
    check(pane.statusText() == QStringLiteral("远程回血 · 等待回显"), "the status line names the command");
    SelfCommandView healed = infantry_in_match();
    healed.economy = 700;
    pane.setView(healed);
    pane.tick(1500);
    check(pane.statusText() == QStringLiteral("远程回血 · 已确认"), "spent coins confirm the command");
    SelfCommandView full = healed;
    full.hp = 200;
    pane.setView(full);
    pane.tick(1600);
    check(!button(pane, "selfRemoteHeal")->isEnabled(), "full HP: remote heal would only waste coins");

    SelfCommandView hero = infantry_in_match();
    hero.infantry = false;
    SelfCommandPane hero_pane;
    Capture hero_capture;
    hero_pane.setView(hero);
    hero_pane.setSender(hero_capture.sender());
    hero_pane.setLinkUp(true);
    settle(hero_pane, 232, 200);
    hero_pane.tick(1000);
    check(hero_pane.exchangeRounds() == 5 && hero_pane.exchangeCost() == 50, "42mm: 5 rounds for 50 coins");
    check(!button(hero_pane, "selfRune")->isVisible(), "the hero has no energy-rune button");
    button(hero_pane, "selfExchange")->click();
    parsed = common(hero_capture.sent.back());
    check(parsed.cmd_type() == 2 && parsed.param() == 5, "hero buys 42mm: cmd 2");
    hero_pane.tick(1200);
    button(hero_pane, "selfRemoteAmmo")->click();
    parsed = common(hero_capture.sent.back());
    check(parsed.cmd_type() == 5 && parsed.param() == 10, "hero remote ammo is one 10-round unit");
}

void self_pane_revive_follows_the_official_client() {
    SelfCommandPane pane;
    Capture capture;
    pane.setView(infantry_in_match());
    pane.setSender(capture.sender());
    pane.setLinkUp(true);
    settle(pane, 232, 200);
    pane.tick(1000);
    check(pane.findChild<QPushButton*>(QStringLiteral("selfRevive")) == nullptr,
          "the panel has no revive button: like the official client, revive lives in a popup");
    check(!pane.confirmRevive() && !pane.payRevive() && capture.sent.empty(), "alive: Y and the popup send nothing");

    SelfCommandView dead = infantry_in_match();
    dead.dead = true;
    dead.revive_total = 20;
    dead.revive_current = 5;
    dead.revive_cost = 100;
    dead.can_free_revive = false;
    dead.can_pay_revive = true;
    pane.setView(dead);
    pane.tick(1100);
    check(!button(pane, "selfExchange")->isEnabled() && !button(pane, "selfRemoteAmmo")->isEnabled(),
          "a dead robot cannot exchange");
    check(pane.contextText().startsWith(QStringLiteral("阵亡 读条 5/20")), "the context line shows the bar");
    check(pane.statusText() == pane.contextText(),
          "dead with nothing pending: the one status line shows the bar and the revive price");
    check(!pane.confirmRevive() && capture.sent.empty(), "Y before the bar fills sends nothing");
    check(pane.payRevive() && capture.sent.size() == 1 && common(capture.sent[0]).cmd_type() == 4,
          "one click pays for the revive (cmd 4), no hold, like the official client");
    check(!pane.payRevive() && capture.sent.size() == 1, "a second click inside 100ms is dropped (10Hz)");
    check(pane.reviveEchoText() == QStringLiteral("金币复活 · 等待回显"), "the popup shows the echo");

    dead.can_free_revive = true;
    pane.setView(dead);
    pane.tick(1300);
    check(pane.confirmRevive() && capture.sent.size() == 2 && common(capture.sent[1]).cmd_type() == 3,
          "bar complete: Y confirms the free revive (cmd 3)");

    dead.can_free_revive = false;
    dead.can_pay_revive = false;
    pane.setView(dead);
    pane.tick(1500);
    check(!pane.payRevive() && capture.sent.size() == 2, "not enough coins: the paid revive sends nothing");

    // 复活框:活着时藏起来,阵亡时弹出;两颗按钮按状态可点。
    QWidget host;
    host.resize(1440, 900);
    host.show();
    RevivePopup popup(&host);
    int confirms = 0;
    int pays = 0;
    popup.setActions([&confirms] { ++confirms; }, [&pays] { ++pays; });
    popup.setLinkUp(true);
    popup.setView(infantry_in_match());
    QApplication::processEvents();
    check(!popup.isVisible(), "alive: no revive popup");
    dead.can_pay_revive = true;
    pane.setView(dead);
    popup.setView(dead);
    QApplication::processEvents();
    check(popup.isVisible() && popup.titleText() == QStringLiteral("复活中 15s"), "dead: the popup counts down");
    auto* pay = popup.findChild<QPushButton*>(QStringLiteral("revivePay"));
    auto* free = popup.findChild<QPushButton*>(QStringLiteral("reviveFree"));
    check(pay->isEnabled() && pay->text() == QStringLiteral("金币复活 · 100金"), "paid revive shows its price");
    check(!free->isEnabled() && free->text() == QStringLiteral("免费复活 [Y]"), "free revive waits for the bar");
    pay->click();
    check(pays == 1, "clicking 金币复活 calls the pay action once");
    dead.can_pay_revive = false;
    dead.can_free_revive = true;
    popup.setView(dead);
    check(!pay->isEnabled() && pay->text() == QStringLiteral("100 金币不足"), "too poor: says so, greyed out");
    check(free->isEnabled() && popup.titleText() == QStringLiteral("可以复活"), "bar complete: free revive lights up");
    free->click();
    check(confirms == 1, "clicking 免费复活 calls the confirm action");
    popup.setLinkUp(false);
    check(!free->isEnabled() && !pay->isEnabled(), "link down: the popup buttons are greyed out");
    const QRect box = popup.geometry();
    check(host.rect().contains(box) && std::abs(box.center().x() - host.width() / 2) <= 1,
          "the popup sits centred inside the window");
}

void self_pane_rune_needs_a_hold_and_an_idle_rune() {
    SelfCommandPane pane;
    Capture capture;
    pane.setView(infantry_in_match());
    pane.setSender(capture.sender());
    pane.setLinkUp(true);
    settle(pane, 232, 200);
    pane.tick(1000);
    auto* rune = button(pane, "selfRune");
    rune->pressed();
    pane.tick(1500);
    rune->released();
    check(capture.sent.empty(), "a short press does not open the rune");
    rune->pressed();
    pane.tick(2600);
    rune->released();
    check(capture.sent.size() == 1 && capture.sent[0].first == "RuneActivateCommand",
          "a full second opens the rune");
    rm::RuneActivateCommand parsed;
    check(parsed.ParseFromString(capture.sent[0].second) && parsed.activate() == 1, "activate = 1");
    SelfCommandView active = infantry_in_match();
    active.rune_status = 2;
    active.rune_arms = 2;
    pane.setView(active);
    pane.tick(2700);
    check(!rune->isEnabled(), "a rune already activating cannot be opened again");
    check(pane.statusText() == QStringLiteral("开能量机关 · 已确认"), "rune status change confirms");
}

void self_pane_performance_is_infantry_prematch_only() {
    SelfCommandPane pane;
    Capture capture;
    SelfCommandView prep = infantry_in_match();
    prep.stage = 1;
    prep.chassis = std::nullopt;
    prep.shooter = std::nullopt;
    pane.setView(prep);
    pane.setSender(capture.sender());
    pane.setLinkUp(true);
    settle(pane, 232, 200);
    pane.tick(1000);
    check(pane.preMatch(), "the infantry sees the performance page before the match");
    check(!button(pane, "selfExchange")->isVisible(), "exchange buttons are hidden before the match");
    button(pane, "selfChassisPower")->click();
    check(capture.sent.size() == 1 && capture.sent[0].first == "RobotPerformanceSelectionCommand",
          "a chassis click sends RobotPerformanceSelectionCommand");
    rm::RobotPerformanceSelectionCommand parsed;
    check(parsed.ParseFromString(capture.sent[0].second) && parsed.chassis() == 2 && parsed.shooter() == 1,
          "power-first chassis; the unchosen shooter goes out as the rule default (cooling-first)");
    prep.chassis = 2;
    prep.shooter = 1;
    pane.setView(prep);
    pane.tick(1200);
    check(button(pane, "selfChassisPower")->isChecked() && !button(pane, "selfChassisHp")->isChecked(),
          "the synced choice is checked");
    button(pane, "selfShooterBurst")->click();
    check(parsed.ParseFromString(capture.sent.back().second) && parsed.shooter() == 2 && parsed.chassis() == 2,
          "changing the shooter keeps the chosen chassis");

    // 连点两项、同步(1Hz)还没回来:第二条必须带上第一条的选择,不能拿旧同步值把它改回去。
    SelfCommandPane quick;
    Capture quick_capture;
    SelfCommandView fresh = infantry_in_match();
    fresh.stage = 1;
    quick.setView(fresh);
    quick.setSender(quick_capture.sender());
    quick.setLinkUp(true);
    settle(quick, 232, 200);
    quick.tick(1000);
    button(quick, "selfChassisPower")->click();
    quick.tick(1150);
    button(quick, "selfShooterBurst")->click();
    check(quick_capture.sent.size() == 2 && parsed.ParseFromString(quick_capture.sent[1].second) &&
              parsed.chassis() == 2 && parsed.shooter() == 2,
          "a quick second pick keeps the first one before the sync arrives");
    check(button(quick, "selfChassisPower")->isChecked() && button(quick, "selfShooterBurst")->isChecked(),
          "both picks show as chosen while waiting for the sync");
    quick.tick(4000);
    check(button(quick, "selfChassisHp")->isChecked(),
          "no sync within the echo timeout: the buttons fall back to what the server reports");

    prep.stage = 0;
    pane.setView(prep);
    pane.tick(1400);
    check(!button(pane, "selfChassisHp")->isEnabled(), "before the prep stage the choice is locked");
    prep.stage = 4;
    pane.setView(prep);
    pane.tick(1500);
    check(!pane.preMatch() && button(pane, "selfExchange")->isVisible(),
          "once the match starts the performance page is gone");

    SelfCommandPane hero;
    SelfCommandView hero_view = infantry_in_match();
    hero_view.infantry = false;
    hero_view.stage = 1;
    hero.setView(hero_view);
    hero.setSender(capture.sender());
    hero.setLinkUp(true);
    settle(hero, 232, 200);
    hero.tick(1000);
    check(!hero.preMatch() && !button(hero, "selfExchange")->isEnabled(),
          "the hero has no performance system: match buttons, greyed out until the match");
}

void self_pane_drops_clicks_while_the_link_is_down() {
    SelfCommandPane pane;
    Capture capture;
    pane.setView(infantry_in_match());
    pane.setSender(capture.sender());
    pane.setLinkUp(true);
    settle(pane, 232, 200);
    pane.tick(1000);
    button(pane, "selfRune")->pressed();
    pane.setLinkUp(false);
    pane.tick(2500);
    button(pane, "selfRune")->released();
    check(capture.sent.empty(), "a hold that spans a disconnect never fires");
    check(!button(pane, "selfExchange")->isEnabled() && !button(pane, "selfRemoteHeal")->isEnabled(),
          "link down: every command button is greyed out");
    button(pane, "selfExchange")->click();
    check(capture.sent.empty(), "link down: clicks are dropped, not queued");
    check(pane.statusText() == QStringLiteral("断线，指令不会发送"), "link down is said in words");
    pane.setKeyboardMouseState(true, QStringLiteral("W A"));
    pane.setLinkUp(true);
    check(pane.statusText() == QStringLiteral("键鼠控制中，Esc 退出 · W A"),
          "keyboard/mouse control shows in the status line");
}

void write_review_pngs() {
    const auto build = [](QWidget* pane, const QString& heading) {
        return carded(pane, heading);
    };

    // 本车指令在花名册那一列:列宽 250,卡片内约 232;高度按 1440x900 和 1920x1080 两档。
    Capture self_capture;
    const auto self_card = [&](const SelfCommandView& view, const QString& name) {
        auto* pane = new SelfCommandPane;
        pane->setView(view);
        pane->setSender(self_capture.sender());
        pane->setLinkUp(true);
        pane->tick(10000);
        std::unique_ptr<QWidget> card(build(pane, QStringLiteral("本车指令")));
        card->show();
        shoot(*card, name, 250, 192);
        shoot(*card, name, 250, 221);
    };
    self_card(infantry_in_match(), QStringLiteral("self-infantry"));
    SelfCommandView prep = infantry_in_match();
    prep.stage = 1;
    self_card(prep, QStringLiteral("self-infantry-prep"));
    SelfCommandView dead = infantry_in_match();
    dead.dead = true;
    dead.revive_total = 36;
    dead.revive_current = 12;
    dead.revive_cost = 100;
    dead.can_free_revive = false;
    dead.can_pay_revive = true;
    self_card(dead, QStringLiteral("self-infantry-dead"));
    SelfCommandView hero = infantry_in_match();
    hero.infantry = false;
    self_card(hero, QStringLiteral("self-hero"));

    QWidget popup_host;
    popup_host.setStyleSheet(QStringLiteral("background:#07090d;"));
    popup_host.resize(640, 360);
    popup_host.show();
    RevivePopup popup(&popup_host);
    popup.setLinkUp(true);
    popup.setView(dead);
    QApplication::processEvents();
    const QString popup_path = artifact_dir() + QStringLiteral("/revive-popup.png");
    check(popup_host.grab().save(popup_path), "revive popup PNG saved");
    std::cout << "  wrote " << popup_path.toStdString() << '\n';

    auto* assembly = new AssemblyPane;
    Capture capture;
    assembly->setView(full_assembly());
    assembly->setSender(capture.sender());
    assembly->setLinkUp(true);
    assembly->setArm(30.0, std::array<float, 6>{12.5F, -34.0F, 78.5F, 0.0F, -15.0F, 92.5F});
    assembly->tick(10000);
    std::unique_ptr<QWidget> assembly_card(build(assembly, QStringLiteral("装配")));
    assembly_card->show();
    shoot(*assembly_card, QStringLiteral("assembly"), 420, 300);
    shoot(*assembly_card, QStringLiteral("assembly"), 320, 240);

    auto* dart = new DartPane;
    dart->setView(full_dart());
    dart->setSender(capture.sender());
    dart->setLinkUp(true);
    dart->tick(10000);
    std::unique_ptr<QWidget> dart_card(build(dart, QStringLiteral("飞镖")));
    dart_card->show();
    shoot(*dart_card, QStringLiteral("dart"), 380, 300);
    shoot(*dart_card, QStringLiteral("dart"), 320, 240);

    auto* sentry = new SentryPane;
    sentry->setView(full_sentry());
    sentry->setSender(capture.sender());
    sentry->setLinkUp(true);
    sentry->tick(10000);
    std::unique_ptr<QWidget> sentry_card(build(sentry, QStringLiteral("哨兵")));
    sentry_card->show();
    shoot(*sentry_card, QStringLiteral("sentry"), 380, 360);
    shoot(*sentry_card, QStringLiteral("sentry"), 320, 240);

    // 断线和空数据各出一张:这两种状态在赛前调试时最常见,也最容易画错。
    auto* down = new AssemblyPane;
    down->setView(AssemblyView{});
    down->setLinkUp(false);
    down->tick(1000);
    std::unique_ptr<QWidget> down_card(build(down, QStringLiteral("装配")));
    down_card->show();
    shoot(*down_card, QStringLiteral("assembly-linkdown"), 420, 300);

    auto* waiting = new SentryPane;
    waiting->setView(SentryView{});
    waiting->setLinkUp(true);
    waiting->setSender(capture.sender());
    waiting->tick(1000);
    std::unique_ptr<QWidget> waiting_card(build(waiting, QStringLiteral("哨兵")));
    waiting_card->show();
    shoot(*waiting_card, QStringLiteral("sentry-waiting"), 380, 360);
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        assembly_shows_every_field_and_sends_once();
        assembly_cancel_needs_a_full_second();
        assembly_waiting_lines_name_their_source();
        assembly_drops_clicks_while_the_link_is_down();
        dart_gates_the_launch_button();
        dart_echo_goes_unconfirmed_without_a_change();
        dart_link_down_disables_everything();
        sentry_cools_down_posture_buttons_after_a_success();
        sentry_gold_revive_needs_a_hold_and_reports_unconfirmed();
        sentry_reads_the_right_seat_for_each_faction();
        builders_keep_missing_data_missing();
        panes_degrade_at_small_sizes();
        self_pane_sends_exchanges_with_the_right_caliber();
        self_pane_revive_follows_the_official_client();
        self_pane_rune_needs_a_hold_and_an_idle_rune();
        self_pane_performance_is_infantry_prematch_only();
        self_pane_drops_clicks_while_the_link_is_down();
        write_review_pngs();
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    std::cout << "role_panes_test OK\n";
    return 0;
}
