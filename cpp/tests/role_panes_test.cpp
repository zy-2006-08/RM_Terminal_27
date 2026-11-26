#include "assembly_flow.h"
#include "battle_analysis_pane.h"
#include "dart_pane.h"
#include "mark_toolbar.h"
#include "next_step_pane.h"
#include "revive_popup.h"
#include "self_command_pane.h"
#include "self_status_pane.h"
#include "sentry_pane.h"
#include "theme.h"

#include "rm_terminal.pb.h"

#include <QApplication>
#include <QDir>
#include <QLabel>
#include <QImage>
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

namespace
{

    void check(bool condition, const char* name)
    {
        if (!condition)
            throw std::runtime_error(name);
        std::cout << "PASS " << name << '\n';
    }

    struct Capture
    {
        std::vector<std::pair<std::string, std::string>> sent;
        bool accept = true;

        std::function<bool(const char*, const std::string&)> sender()
        {
            return [this](const char* topic, const std::string& payload)
            {
                sent.emplace_back(topic, payload);
                return accept;
            };
        }
    };

    template <class T>
    Field<T> present(T value)
    {
        Field<T> field;
        field.value = value;
        field.quality = Quality::Valid;
        field.freshness = Freshness::Fresh;
        return field;
    }

    DartView dart_at(std::uint32_t gate, std::int32_t match_sec, std::uint32_t target = 4)
    {
        DartView view;
        view.gate = gate;
        view.target_id = target;
        view.match_sec = match_sec;
        return view;
    }

    SentryView full_sentry()
    {
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
    void settle(Pane& pane, int w, int h)
    {
        pane.resize(w, h);
        if (pane.layout())
            pane.layout()->activate();
        pane.show();
        QApplication::processEvents();
    }

    QPushButton* button(QWidget& pane, const char* name)
    {
        auto* found = pane.findChild<QPushButton*>(QString::fromLatin1(name));
        if (!found)
            throw std::runtime_error(std::string("missing button ") + name);
        return found;
    }

    QString artifact_dir()
    {
        const QString dir = QStringLiteral(RM_TERMINAL_ARTIFACT_DIR) + QStringLiteral("/roles");
        if (!QDir().mkpath(dir))
            throw std::runtime_error("cannot create artifacts/roles");
        return dir;
    }

    void shoot(QWidget& widget, const QString& name, int w, int h, bool exact_size = false)
    {
        widget.resize(w, h);
        widget.layout()->activate();
        QApplication::processEvents();
        const QPixmap shot = widget.grab();
        const QString path =
            QStringLiteral("%1/%2-%3x%4.png").arg(artifact_dir(), name).arg(w).arg(h);
        check(!shot.isNull() && shot.save(path), "pane PNG saved");
        // 飞镖和哨兵的图必须正好是要的尺寸。按钮文字一长,面板的最小宽度就能把卡片顶宽,
        // 图比要求宽几十像素,而评审看的就是这张图 —— 让它当场失败,别让界面悄悄变宽。
        if (exact_size)
            check(shot.width() == w && shot.height() == h,
                  (name.toStdString() + " PNG is exactly the requested size").c_str());
        std::cout << "  wrote " << path.toStdString() << '\n';
    }

    // 三个面板都装在 dashboard 的「card + heading」卡片里。截图也照这个包一层,
    // 否则图上看不出标题和面板的实际间距,而那正是最容易挤到的地方。
    QWidget* carded(QWidget* pane, const QString& heading)
    {
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

    void dart_f_asks_and_y_opens_the_gate()
    {
        DartPane pane;
        Capture capture;
        pane.setSender(capture.sender());
        pane.setLinkUp(true);
        pane.tick(1000);
        pane.setView(dart_at(0, 90, 1));
        check(pane.prompt().step == DartStep::Ready, "a chance left and the gate shut: F is ready");

        pane.confirmOpen();
        check(capture.sent.empty(), "Y without F sends nothing");
        pane.requestOpen();
        check(capture.sent.empty() && pane.prompt().step == DartStep::Asking, "F alone only asks");
        pane.cancelOpen();
        check(capture.sent.empty() && pane.prompt().step == DartStep::Ready, "N drops the question");

        pane.requestOpen();
        pane.confirmOpen();
        rm::DartCommand opened;
        check(capture.sent.size() == 1 && capture.sent[0].first == "DartCommand" &&
                  opened.ParseFromString(capture.sent[0].second) && opened.target_id() == 1 &&
                  opened.open() && !opened.launch_confirm(),
              "F then Y opens the gate for the selected target");

        pane.setView(dart_at(1, 91, 1));
        check(pane.prompt().step == DartStep::Opening, "gate 1 reads as 闸门运行中");
        pane.requestOpen();
        check(pane.prompt().step == DartStep::Opening, "F does nothing while the gate moves");

        DartPane other;
        Capture other_capture;
        other.setSender(other_capture.sender());
        other.setLinkUp(true);
        other.setView(dart_at(0, 90, 1));
        other.requestOpen();
        other.setLinkUp(false);
        other.setLinkUp(true);
        other.confirmOpen();
        check(other_capture.sent.empty(), "a dropped link forgets the question");
    }

    void dart_l_launches_once_the_gate_is_fully_open()
    {
        DartPane pane;
        Capture capture;
        pane.setSender(capture.sender());
        pane.setLinkUp(true);
        pane.tick(1000);
        pane.setView(dart_at(0, 90));
        pane.launch();
        check(capture.sent.empty(), "L does nothing while the gate is shut");
        pane.setView(dart_at(1, 91));
        pane.launch();
        check(capture.sent.empty(), "L does nothing while the gate moves");

        pane.setView(dart_at(2, 98));
        check(pane.prompt().step == DartStep::Open && pane.prompt().seconds == 30,
              "fully open: L ready, 30 s until the gate closes");
        pane.setView(dart_at(2, 109));
        check(pane.prompt().seconds == 19, "the close countdown follows match time");

        pane.launch();
        rm::DartCommand launched;
        check(capture.sent.size() == 1 && launched.ParseFromString(capture.sent[0].second) &&
                  launched.target_id() == 4 && launched.open() && launched.launch_confirm(),
              "one press of L sends launch_confirm, no hold");
    }

    void dart_j_cycles_targets_and_keeps_the_gate()
    {
        DartPane pane;
        Capture capture;
        pane.setSender(capture.sender());
        pane.setLinkUp(true);
        pane.tick(1000);
        pane.setView(dart_at(0, 90, 5));

        pane.cycleTarget();
        rm::DartCommand wrapped;
        check(capture.sent.size() == 1 && wrapped.ParseFromString(capture.sent[0].second) &&
                  wrapped.target_id() == 1 && !wrapped.open() && !wrapped.launch_confirm(),
              "J after 基地末端移动 wraps to 前哨站 with the gate left shut");

        pane.setView(dart_at(2, 98, 1));
        pane.tick(2000);
        pane.cycleTarget();
        rm::DartCommand kept;
        check(capture.sent.size() == 2 && kept.ParseFromString(capture.sent[1].second) &&
                  kept.target_id() == 2 && kept.open() && !kept.launch_confirm(),
              "J while the gate is open asks for the next target without closing the gate");
    }

    void dart_counts_gate_chances_like_the_official_client()
    {
        DartPane pane;
        pane.setView(dart_at(0, 20));
        check(pane.prompt().step == DartStep::NextChance && pane.prompt().seconds == 10,
              "before 0:30: 10 s until the first chance");
        pane.setView(dart_at(0, 30));
        check(pane.prompt().step == DartStep::Ready, "0:30 gives the first chance");
        pane.setView(dart_at(1, 31));
        pane.setView(dart_at(2, 38));
        pane.setView(dart_at(0, 68));
        pane.setView(dart_at(0, 120));
        check(pane.prompt().step == DartStep::NextChance && pane.prompt().seconds == 120,
              "first chance used: 120 s until the 4:00 one");
        pane.setView(dart_at(0, 245));
        check(pane.prompt().step == DartStep::Ready, "4:00 gives the second chance");
        pane.setView(dart_at(1, 246));
        pane.setView(dart_at(2, 253));
        pane.setView(dart_at(0, 283));
        check(pane.prompt().step == DartStep::Exhausted, "both chances used: 已达上限");
        pane.setView(dart_at(0, 5));
        check(pane.prompt().step == DartStep::NextChance && pane.prompt().seconds == 25,
              "match time going back means a new round: the count starts over");

        DartPane saved;
        saved.setView(dart_at(0, 250));
        check(saved.prompt().step == DartStep::Ready, "unused chances add up");
        saved.setView(dart_at(1, 251));
        saved.setView(dart_at(2, 258));
        saved.setView(dart_at(0, 288));
        saved.setView(dart_at(0, 290));
        check(saved.prompt().step == DartStep::Cooldown && saved.prompt().seconds == 13,
              "a chance left after closing: 15 s cooldown first");
        saved.setView(dart_at(0, 303));
        check(saved.prompt().step == DartStep::Ready, "cooldown over: F is ready again");

        DartPane outside;
        DartView prep = dart_at(0, 0);
        prep.match_sec.reset();
        outside.setView(prep);
        check(outside.prompt().step == DartStep::NextChance && !outside.prompt().seconds,
              "outside a match there is no chance to open");
        DartPane waiting;
        check(waiting.prompt().step == DartStep::NoData, "no DartSelectTargetStatusSync yet: nothing to do");
    }

    void dart_stops_after_four_darts_a_round()
    {
        Snapshot snapshot;
        snapshot.dart_hits = DartTally{3, 1};
        snapshot.self = RobotId{6};
        check(dart_view(snapshot).hits == 3, "a red commander counts red dart hits");
        snapshot.self = RobotId{106};
        check(dart_view(snapshot).hits == 1, "a blue commander counts blue dart hits");
        snapshot.self.reset();
        check(dart_view(snapshot).hits == 0, "no seat, no count");

        DartPane pane;
        Capture capture;
        pane.setSender(capture.sender());
        pane.setLinkUp(true);
        pane.tick(1000);
        DartView open = dart_at(2, 100);
        open.hits = 3;
        pane.setView(open);
        check(pane.prompt().step == DartStep::Open, "three darts hit: the fourth can still go");
        pane.launch();
        check(capture.sent.size() == 1, "L sends while a dart is left");

        open.hits = 4;
        pane.setView(open);
        pane.tick(2000);
        check(pane.prompt().step == DartStep::Spent, "four darts hit: used up even with the gate open");
        pane.launch();
        check(capture.sent.size() == 1, "L sends nothing once the four darts are used");

        DartView later = dart_at(0, 250);
        later.hits = 4;
        pane.setView(later);
        check(pane.prompt().step == DartStep::Spent, "the 4:00 chance is no use without darts");
        pane.requestOpen();
        pane.confirmOpen();
        check(capture.sent.size() == 1, "F then Y sends nothing once the four darts are used");

        pane.setView(dart_at(0, 40));
        check(pane.prompt().step == DartStep::Ready, "a new round starts with four darts");

        DartView settling = dart_at(0, 0);
        settling.match_sec.reset();
        settling.hits = 4;
        pane.setView(settling);
        check(pane.prompt().step == DartStep::NextChance, "outside the match the count blocks nothing");
    }

    void dart_link_down_drops_every_key()
    {
        DartPane pane;
        Capture capture;
        pane.setSender(capture.sender());
        pane.setLinkUp(false);
        pane.tick(1000);
        pane.setView(dart_at(2, 98));
        pane.cycleTarget();
        pane.launch();
        pane.setView(dart_at(0, 300));
        pane.requestOpen();
        pane.confirmOpen();
        check(capture.sent.empty(), "dart keys while the link is down are dropped");
    }

    void sentry_cools_down_posture_buttons_after_a_success()
    {
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
        check(capture.sent.empty(), "one click only arms the posture button");
        defend->click();
        check(capture.sent.size() == 1 && capture.sent[0].first == "SentryCtrlCommand",
              "a second posture click sends SentryCtrlCommand");
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
        check(quiet.lastResultText().isEmpty(),
              "no result yet draws nothing: the debug string is gone from the panel");
    }

    // 二次确认:所有花金币的哨兵指令都走同一套。单击只点亮按钮并报价,3 秒内再点同一颗才发。
    void sentry_spends_coins_only_on_a_second_click()
    {
        SentryPane pane;
        Capture capture;
        SentryView view = full_sentry();
        view.result.reset();
        view.remaining_sec = 120; // 打到 5:00,规则 5.7.2 的涨价都按这个算
        view.automatic = true;
        pane.setView(view);
        pane.setSender(capture.sender());
        pane.setLinkUp(true);
        pane.tick(1000);
        settle(pane, 380, 360);

        // 单击一律不发:四颗花钱的按钮都要守住这条。
        for (const char* name : {"sentryAmmo", "sentryHeal", "sentryRevive", "sentryGoldRevive"})
        {
            SentryPane one;
            Capture once;
            one.setView(view);
            one.setSender(once.sender());
            one.setLinkUp(true);
            one.tick(1000);
            settle(one, 380, 360);
            button(one, name)->click();
            check(once.sent.empty(), (std::string("a single click on ") + name + " sends nothing").c_str());
            check(one.armedCommand().has_value(), (std::string(name) + " is armed instead").c_str());
        }

        auto* ammo = button(pane, "sentryAmmo");
        ammo->click();
        check(capture.sent.empty() && pane.armedCommand() == std::optional<std::uint32_t>(3),
              "远程补弹 arms on the first click");
        ammo->click();
        rm::SentryCtrlCommand parsed;
        check(capture.sent.size() == 1 && parsed.ParseFromString(capture.sent[0].second) &&
                  parsed.command_id() == 3,
              "the second click on the same button sends exactly one 远程补弹");
        check(!pane.armedCommand(), "sending clears the armed state");

        // 超时作废:点亮后超过 3 秒,再点只是重新点亮,不发。
        SentryPane stale;
        Capture stale_capture;
        stale.setView(view);
        stale.setSender(stale_capture.sender());
        stale.setLinkUp(true);
        stale.tick(1000);
        settle(stale, 380, 360);
        button(stale, "sentryRevive")->click();
        stale.tick(1000 + SentryPane::kArmedMs + 1);
        check(!stale.armedCommand(), "an armed confirmation expires after the timeout");
        button(stale, "sentryRevive")->click();
        check(stale_capture.sent.empty(), "a click after the timeout only arms again, it does not send");

        // 点别的按钮就作废:操作手改了主意,原来那颗不该还等着确认。
        SentryPane switched;
        Capture switched_capture;
        switched.setView(view);
        switched.setSender(switched_capture.sender());
        switched.setLinkUp(true);
        switched.tick(1000);
        settle(switched, 380, 360);
        button(switched, "sentryGoldRevive")->click();
        button(switched, "sentryAmmo")->click();
        check(switched.armedCommand() == std::optional<std::uint32_t>(3),
              "clicking another button moves the armed state to it");
        button(switched, "sentryGoldRevive")->click();
        check(switched_capture.sent.empty(),
              "the first button lost its arming, so a click on it sends nothing");

        // 断线也作废,且回显照旧:发出去 2 秒没有 SentryCtrlResult 就是未确认。
        SentryPane dropped;
        Capture dropped_capture;
        dropped.setView(view);
        dropped.setSender(dropped_capture.sender());
        dropped.setLinkUp(true);
        dropped.tick(1000);
        settle(dropped, 380, 360);
        button(dropped, "sentryGoldRevive")->click();
        dropped.setLinkUp(false);
        check(!dropped.armedCommand(), "a dropped link cancels the armed confirmation");
        dropped.setLinkUp(true);
        button(dropped, "sentryGoldRevive")->click();
        button(dropped, "sentryGoldRevive")->click();
        check(dropped_capture.sent.size() == 1 && parsed.ParseFromString(dropped_capture.sent[0].second) &&
                  parsed.command_id() == 6,
              "花金币复活 still sends command_id 6, now on two clicks instead of a hold");
        check(dropped.statusText().contains(QStringLiteral("等待回显")),
              "the gold revive waits for a result");
        dropped.tick(1000 + 2100);
        check(dropped.statusText().contains(QStringLiteral("未确认")),
              "no SentryCtrlResult within 2s reads as 未确认");
    }

    // 按钮上的价钱照 sentry_cost:打到 5:00、自动哨兵时姿态 50、确认复活 50、补弹 200、
    // 回血 200、花金币复活 470。赛外没有涨价依据,这两项不标价。
    void sentry_buttons_show_the_real_price()
    {
        SentryPane pane;
        Capture capture;
        SentryView view = full_sentry();
        view.remaining_sec = 120;
        view.automatic = true;
        pane.setView(view);
        pane.setSender(capture.sender());
        pane.setLinkUp(true);
        pane.tick(1000);
        settle(pane, 380, 360);

        check(pane.priceOf(7) == std::optional<std::uint32_t>(50), "a posture switch costs 50");
        check(pane.priceOf(5) == std::optional<std::uint32_t>(50), "确认复活 costs 50");
        check(pane.priceOf(3) == std::optional<std::uint32_t>(200), "远程补弹 costs 200");
        check(pane.priceOf(4) == std::optional<std::uint32_t>(200), "远程回血 costs 200 at 5:00 played");
        check(pane.priceOf(6) == std::optional<std::uint32_t>(470), "花金币复活 costs 470 at 5:00 played");
        check(button(pane, "sentryAmmo")->text().contains(QStringLiteral("200")),
              "远程补弹 wears its 200 coin price");
        check(button(pane, "sentryGoldRevive")->text().contains(QStringLiteral("470")),
              "花金币复活 wears its 470 coin estimate");
        check(button(pane, "sentryGoldRevive")->text().contains(QStringLiteral("≈")),
              "a price that depends on match time is marked as an estimate");

        // 半自动哨兵不收 50 干预费(规则 5.7.2)。
        SentryView semi = view;
        semi.automatic = false;
        pane.setView(semi);
        check(pane.priceOf(7) == std::optional<std::uint32_t>(0), "a semi-automatic posture switch is free");
        check(pane.priceOf(3) == std::optional<std::uint32_t>(150), "semi-automatic 远程补弹 is just the 150");

        // 赛外:回血和金币复活没有涨价依据,不标价。
        SentryView outside = view;
        outside.remaining_sec.reset();
        pane.setView(outside);
        check(!pane.priceOf(4) && !pane.priceOf(6), "outside the match the time-priced commands show no price");
        check(pane.priceOf(7) == std::optional<std::uint32_t>(50), "the flat 50 fee still applies");
    }

    void sentry_reads_the_right_seat_for_each_faction()
    {
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

    void builders_keep_missing_data_missing()
    {
        Snapshot empty;
        const AssemblyView assembly = assembly_view(empty);
        check(!assembly.maximum_difficulty && !assembly.basic_state && !assembly.putin,
              "an empty snapshot leaves every assembly field missing");
        check(assembly.last_result.isEmpty(), "no Event 15 means no result line");

        const DartView dart = dart_view(empty);
        check(!dart.gate && !dart.target_id && !dart.match_sec,
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
        filled.game.current_stage = present<std::uint32_t>(4);
        filled.game.stage_elapsed_sec = present<std::int32_t>(95);

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
        check(built_dart.gate == 1 && built_dart.target_id == 4 && built_dart.match_sec == 95,
              "dart fields land");
        filled.game.current_stage = present<std::uint32_t>(5);
        check(!dart_view(filled).match_sec, "match time only counts during the match");
    }

    // 320 宽的评审卡片里,面板拿到的身子只有 302 像素。面板的最小宽度一超过它,卡片就会
    // 被顶宽,截图比要求的尺寸宽,真界面里右边那一列也会挤到地图。
    void role_panes_fit_a_narrow_card()
    {
        constexpr int kNarrowBody = 302;
        DartPane dart;
        dart.setView(dart_at(2, 98));
        dart.setLinkUp(true);
        dart.tick(1000);
        settle(dart, 320, 240);
        check(dart.minimumSizeHint().width() <= kNarrowBody,
              "the dart pane fits a 302 px wide card body");

        SentryPane sentry;
        SentryView priced = full_sentry();
        // 最贵的一档:金币复活 ≈470 金,按钮文字最长的时候也要装得下。
        priced.remaining_sec = 120;
        priced.automatic = true;
        sentry.setView(priced);
        sentry.setLinkUp(true);
        sentry.tick(1000);
        settle(sentry, 320, 240);
        check(sentry.minimumSizeHint().width() <= kNarrowBody,
              "the sentry pane fits a 302 px wide card body even with prices on the action buttons");

        // 姿态按钮只写名字:单价在卡片标题右边说一次。
        check(!button(sentry, "sentryPosture7")->text().contains(QStringLiteral("金")),
              "a posture button shows its name only, no repeated fee");
        check(sentry.headerText() == QStringLiteral("干预 50 金/次"),
              "an automatic sentry names the intervention fee once, in the card header");
        SentryView semi = priced;
        semi.automatic = false;
        sentry.setView(semi);
        check(sentry.headerText() == QStringLiteral("半自动 · 不花金币"),
              "a semi-automatic sentry says so in the header");

        // 等数据时说人话,不报协议消息名。
        SentryPane waiting;
        waiting.setLinkUp(true);
        waiting.setSender([](const char*, const std::string&)
                          { return true; });
        waiting.tick(1000);
        check(waiting.statusText() == QStringLiteral("等待哨兵数据"),
              "the waiting state is plain Chinese, not a protocol message name");
    }

    void panes_degrade_at_small_sizes()
    {
        DartPane dart;
        dart.setView(dart_at(2, 98));
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

    // 拖到任意尺寸都要铺满:把面板画到一张图上,量最上面一行和最下面一行有内容的像素。
    // 「有内容」= 和左上角那个像素(底色)差得够多的像素占这一行的 1% 以上。
    struct Ink
    {
        int top = -1;
        int bottom = -1;
        int widest = 0;
    };

    Ink ink_rows(const QImage& shot)
    {
        Ink ink;
        const QRgb background = shot.pixel(0, 0);
        for (int y = 0; y < shot.height(); ++y)
        {
            int lit = 0;
            for (int x = 0; x < shot.width(); ++x)
            {
                const QRgb pixel = shot.pixel(x, y);
                const int distance = std::abs(qRed(pixel) - qRed(background)) +
                                     std::abs(qGreen(pixel) - qGreen(background)) +
                                     std::abs(qBlue(pixel) - qBlue(background));
                if (distance > 18)
                    ++lit;
            }
            if (lit <= shot.width() / 100)
                continue;
            if (ink.top < 0)
                ink.top = y;
            ink.bottom = y;
            ink.widest = std::max(ink.widest, lit);
        }
        return ink;
    }

    // 自己铺一张底再让面板画上去:不给父窗口的 QWidget 用 grab() 得到的底色是未初始化的,
    // 量出来的「有内容」会跟着上一次的内存变。底色取面板在界面里真正坐着的那块玻璃色。
    template <class Pane>
    QImage pane_shot(Pane& pane, int w, int h)
    {
        pane.resize(w, h);
        QApplication::processEvents();
        QImage shot(w, h, QImage::Format_ARGB32);
        shot.fill(QColor(22, 38, 58));
        pane.render(&shot, QPoint(), QRegion(), QWidget::DrawChildren);
        return shot;
    }

    RosterEntry self_status_entry()
    {
        RosterEntry entry;
        entry.id = RobotId{3};
        entry.faction = 1;
        entry.is_self = true;
        entry.live = LiveState::Alive;
        entry.hp = 200;
        entry.max_hp = 200;
        entry.level = 1;
        entry.ammo = 0;
        entry.bullet_speed = 29.0;
        entry.economy = 800;
        entry.heat = 0.0;
        entry.heat_limit = 40;
        entry.buffer_energy = 43;
        entry.buffer_limit = 60;
        entry.modules_online = 9;
        entry.modules_total = 10;
        entry.out_of_combat = true;
        entry.can_remote_heal = true;
        entry.can_remote_ammo = false;
        entry.pending_respawn = false;
        return entry;
    }

    std::vector<NextCard> next_cards(std::size_t count)
    {
        std::vector<NextCard> cards;
        NextCard rune;
        rune.rule = NextRule::OpenRune;
        rune.category = QStringLiteral("进攻");
        rune.title = QStringLiteral("开小能量机关");
        rune.reason = QStringLiteral("刚开放，长按 F 1 秒开启");
        cards.push_back(rune);
        NextCard ammo;
        ammo.rule = NextRule::BuyAmmo;
        ammo.category = QStringLiteral("补给");
        ammo.title = QStringLiteral("兑换弹丸");
        ammo.reason = QStringLiteral("只剩 0 发，50 金换 50 发");
        cards.push_back(ammo);
        NextCard base;
        base.rule = NextRule::DefendBase;
        base.category = QStringLiteral("防守");
        base.title = QStringLiteral("回防我方基地");
        base.reason = QStringLiteral("基地正在掉血，还剩 4660");
        cards.push_back(base);
        cards.resize(count);
        return cards;
    }

    ZoneBoard analysis_board()
    {
        ZoneBoard board;
        board.zones[0] = ZoneStatus{QStringLiteral("红方基地区"), ZoneControl::RedTemporary, 4};
        board.zones[1] = ZoneStatus{QStringLiteral("红方前哨区"), ZoneControl::RedFull, std::nullopt};
        board.zones[2] = ZoneStatus{QStringLiteral("蓝方前哨区"), ZoneControl::BlueFull, std::nullopt};
        board.zones[3] = ZoneStatus{QStringLiteral("蓝方基地区"), ZoneControl::Neutral, std::nullopt};
        board.red_control_points = 186;
        board.blue_control_points = 143;
        board.red_logistics = 1.0;
        board.blue_logistics = 0.83;
        return board;
    }

    std::vector<AnalysisMetric> analysis_metrics()
    {
        std::vector<AnalysisMetric> metrics;
        AnalysisMetric economy;
        economy.label = QStringLiteral("我方经济");
        economy.self_value = 800;
        economy.has_enemy = false;
        economy.note = QStringLiteral("后勤 1.00");
        metrics.push_back(economy);
        AnalysisMetric damage;
        damage.label = QStringLiteral("总伤害");
        damage.self_value = 3860;
        damage.enemy_value = 2940;
        metrics.push_back(damage);
        AnalysisMetric kills;
        kills.label = QStringLiteral("击杀");
        kills.self_value = 3;
        kills.enemy_value = 2;
        metrics.push_back(kills);
        AnalysisMetric rune;
        rune.label = QStringLiteral("我方能量机关");
        rune.text_value = QStringLiteral("小机关激活");
        rune.has_enemy = false;
        metrics.push_back(rune);
        return metrics;
    }

    // 共用的缩放助手:一个面板一个系数,拖大放大、拖小缩小,有上下限。
    void pane_scale_grows_and_shrinks_within_bounds()
    {
        const QSizeF design(330, 190);
        check(theme::paneScale(design, design) == 1.0, "the design size scales by exactly 1");
        check(theme::paneScale(QSizeF(660, 380), design) == 2.0, "twice the design size scales by 2");
        check(theme::paneScale(QSizeF(165, 95), design, 0.2) == 0.5, "half the design size scales by 0.5");
        check(theme::paneScale(QSizeF(264, 152), design) == 0.8, "a smaller pane scales below 1");
        // 宽高各算一遍取小的:窄的那一边决定能放多大,放大时才不会溢出。
        check(theme::paneScale(QSizeF(3300, 380), design) == 2.0, "the smaller axis decides the scale");
        check(theme::paneScale(QSizeF(9900, 9900), design) == theme::kMaxPaneScale,
              "growth stops at the shared upper bound");
        check(theme::paneScale(QSizeF(1, 1), design, 0.6) == 0.6, "shrinking stops at the requested floor");
        check(theme::kMaxPaneScale >= 2.5, "panes may grow to at least 2.5x the design size");

        const auto label = [](int px)
        { return theme::labelPx(px); };
        const QString text = QStringLiteral("允许发弹量");
        const int wide = theme::fitFontPx(text, 20, theme::kMinLabelPx, 1000, label);
        check(wide == 20, "a box with room keeps the wanted size");
        const int narrow = theme::fitFontPx(text, 20, theme::kMinLabelPx, 40, label);
        check(narrow < 20 && narrow >= theme::kMinLabelPx, "a narrow box shrinks the font, never below the floor");
        check(theme::fitFontPx(text, 20, theme::kMinLabelPx, 1, label) == theme::kMinLabelPx,
              "an impossible box stops at the floor instead of vanishing");
        check(theme::kMinLabelPx >= 11 && theme::kMinNumberPx >= 12, "labels floor at 11px and numbers at 12px");
    }

    // 本机状态:四段按设计高度的比例分走整块高度。以前整块在面板里居中,上面空一大片
    // (scale 封在 1.5、段间隔封在 36),1080x650 的格子里上下各空两百多像素。
    void self_status_fills_whatever_box_it_gets()
    {
        SelfStatusPane pane;
        pane.setEntry(self_status_entry());
        pane.show();
        int previous_height = 0;
        for (const auto [w, h] : {std::pair{300, 150}, std::pair{340, 220}, std::pair{560, 420},
                                  std::pair{560, 960}, std::pair{1080, 650}, std::pair{1400, 260}})
        {
            const Ink ink = ink_rows(pane_shot(pane, w, h));
            check(ink.top >= 0, "the self status pane paints something at every size");
            // 最上面和最下面的内容都要贴着边:留的白边不超过高度的 5%,也不超过 24 像素。
            const int slack = std::min(24, h / 20);
            check(ink.top <= slack, "no blank band above the self status content");
            check(h - 1 - ink.bottom <= slack, "no blank band below the self status content");
            previous_height = h;
        }
        check(previous_height == 260, "every listed size was exercised");

        // 字跟着长:同样的宽度,面板高一倍,本机名字那一行的字就更大(高度进内容,不进空白)。
        const QImage small = pane_shot(pane, 560, 200);
        const QImage large = pane_shot(pane, 560, 600);
        check(ink_rows(large).widest > ink_rows(small).widest,
              "a taller self status pane draws more ink per row, so its fonts grew");
    }

    // 战场数据分析:几块按权重分走整块高度。以前多出来的高度一半当间隔、一半上下居中,
    // 560x960 的格子里上下各空三百多像素,块里的字也不跟着变大。
    void battle_analysis_fills_whatever_box_it_gets()
    {
        BattleAnalysisPane pane;
        pane.setOwnFaction(1);
        pane.setMetrics(analysis_metrics());
        pane.setZoneBoard(analysis_board());
        pane.setFortress(FortressStatus{0, 6});
        pane.show();
        for (const auto [w, h] : {std::pair{300, 150}, std::pair{340, 220}, std::pair{560, 420},
                                  std::pair{560, 960}, std::pair{1080, 650}, std::pair{1400, 260}})
        {
            const Ink ink = ink_rows(pane_shot(pane, w, h));
            check(ink.top >= 0, "the analysis pane paints something at every size");
            // 留一个固定的余量:内容区本身有 kPadY 的边距,最上面那一块是「一行字」,
            // 字在自己那条带里上下居中,带比字高时上面自然有一点空。
            // 要抓的是改之前那种「上下各空三百多像素」,所以 24 像素的余量仍然差一个数量级。
            const int slack = std::max(24, h / 20);
            check(ink.top <= slack, "no blank band above the analysis content");
            check(h - 1 - ink.bottom <= slack, "no blank band below the analysis content");
        }
        // 最高那一档照旧是「完整」:铺满高度不该让它掉档,统计那一块还要在面板里。
        pane.resize(560, 960);
        check(pane.level() == BattleAnalysisPane::Level::Full, "a tall analysis pane still shows the full tier");
        check(!pane.compact(), "a tall analysis pane keeps its stats inside the pane");
        pane.resize(1400, 120);
        check(pane.level() != BattleAnalysisPane::Level::Full, "a short analysis pane still drops to a smaller tier");

        const QImage small = pane_shot(pane, 560, 200);
        const QImage large = pane_shot(pane, 560, 600);
        check(ink_rows(large).widest > ink_rows(small).widest,
              "a taller analysis pane draws more ink per row, so its fonts grew");
    }

    // 下一步:只画真有的事,几件就分几份,一块空白都不留。以前不够三件时补虚线占位卡
    // (「暂时没有更要紧的事」),窄而高的格子里还把三张卡并排挤成细条,字下面空一大片。
    void next_step_fills_whatever_box_it_gets()
    {
        NextStepPane pane;
        pane.setCapacity(3);
        pane.show();
        for (std::size_t count : {std::size_t{1}, std::size_t{2}, std::size_t{3}})
        {
            pane.setCards(next_cards(count));
            for (const auto [w, h] : {std::pair{300, 150}, std::pair{340, 220}, std::pair{560, 420},
                                      std::pair{560, 960}, std::pair{1080, 650}, std::pair{1400, 260}})
            {
                const Ink ink = ink_rows(pane_shot(pane, w, h));
                check(ink.top >= 0, "the next step pane paints something at every size");
                // 卡片自己就是底色块,所以内容必须从第一行铺到最后一行。
                check(ink.top <= 2, "the next step cards start at the top edge");
                check(h - 1 - ink.bottom <= 2, "the next step cards reach the bottom edge");
            }
        }

        // 形状决定排法:扁的并排,窄或高的竖着一行一张。
        pane.setCards(next_cards(3));
        pane.resize(1400, 260);
        check(pane.sideBySide(3), "a wide pane puts three cards side by side");
        pane.resize(560, 960);
        check(!pane.sideBySide(3), "a tall pane stacks three cards as full-width rows");
        pane.resize(300, 150);
        check(!pane.sideBySide(3), "a narrow pane stacks instead of squeezing three columns");
        pane.resize(1400, 260);
        check(pane.sideBySide(1), "one card in a wide pane fills it as a single card");

        // 容量仍然封顶:给四件也只画三件。
        std::vector<NextCard> four = next_cards(3);
        four.push_back(four.front());
        pane.setCapacity(2);
        pane.setCards(four);
        check(pane.cards().size() == 4, "the pane keeps every card it was given");
        const Ink capped = ink_rows(pane_shot(pane, 1400, 260));
        check(capped.top <= 2, "a capped pane still fills its box");

        // 没有要紧的事:那句话画大、居中,面板越大字越大。以前是一行小字贴在左边。
        pane.setCards({});
        const Ink empty_small = ink_rows(pane_shot(pane, 340, 220));
        const Ink empty_large = ink_rows(pane_shot(pane, 1080, 650));
        check(empty_small.top > 0 && empty_large.top > 0, "the empty line is centred, not pinned to the top");
        check(empty_large.widest > empty_small.widest, "the empty line grows with the pane");
        const int small_centre = (empty_small.top + empty_small.bottom) / 2;
        const int large_centre = (empty_large.top + empty_large.bottom) / 2;
        check(std::abs(small_centre - 220 / 2) <= 220 / 10, "the empty line sits in the middle of a small pane");
        check(std::abs(large_centre - 650 / 2) <= 650 / 10, "the empty line sits in the middle of a large pane");

        // 那句话的字号按面板大小只算一次:拖成别的大小必须重算,画出来和一块新建的面板一样。
        const auto fresh_shot = [](int w, int h)
        {
            NextStepPane fresh;
            fresh.show();
            return pane_shot(fresh, w, h);
        };
        check(pane_shot(pane, 1080, 650) == fresh_shot(1080, 650), "a resized empty pane draws like a fresh one");
        check(pane_shot(pane, 340, 220) == fresh_shot(340, 220), "shrinking the empty pane back re-fits the line");
    }

    SelfCommandView infantry_in_match()
    {
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
        view.at_exchange_point = true;
        view.shooter_online = true;
        view.logistics = 1.0;
        view.dead = false;
        view.rune_status = 1;
        view.shooter = 1;
        view.chassis = 1;
        return view;
    }

    rm::CommonCommand common(const std::pair<std::string, std::string>& sent)
    {
        if (sent.first != "CommonCommand")
            throw std::runtime_error("expected CommonCommand, got " + sent.first);
        rm::CommonCommand parsed;
        if (!parsed.ParseFromString(sent.second))
            throw std::runtime_error("CommonCommand does not parse");
        return parsed;
    }

    void self_pane_sends_exchanges_with_the_right_caliber()
    {
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
        std::vector<std::pair<PurchaseKind, std::uint32_t>> asked;
        pane.setPurchaseRequest([&asked](PurchaseKind kind, std::uint32_t rounds)
                                { asked.emplace_back(kind, rounds); });
        button(pane, "selfExchange")->click();
        check(asked.size() == 1 && asked[0].first == PurchaseKind::Supply && asked[0].second == 50 &&
                  capture.sent.empty(),
              "like the official client a click only asks for a confirmation, nothing is sent yet");
        const SelfCommandView view = infantry_in_match();
        check(pane.sendPurchase(supply_offer(view, true, Caliber::Small, 50)) && capture.sent.size() == 1,
              "the confirmed exchange goes out once");
        auto parsed = common(capture.sent[0]);
        check(parsed.cmd_type() == 1 && parsed.param() == 50, "infantry buys 17mm: cmd 1, param = rounds");
        check(!pane.sendPurchase(supply_offer(view, true, Caliber::Small, 50)) && capture.sent.size() == 1,
              "a second exchange inside 100ms is dropped (10Hz)");
        button(pane, "selfExchangeMore")->click();
        button(pane, "selfExchangeMore")->click();
        check(pane.exchangeRounds() == 200, "+ walks the ladder 50 -> 100 -> 200");

        pane.tick(1200);
        button(pane, "selfRemoteAmmo")->click();
        check(asked.back().first == PurchaseKind::RemoteAmmo && asked.back().second == 100,
              "remote ammo asks for one 100-round unit");
        check(pane.sendPurchase(remote_ammo_offer(view, true, Caliber::Small)), "remote ammo goes out after the Y");
        parsed = common(capture.sent.back());
        check(parsed.cmd_type() == 5 && parsed.param() == 100, "remote ammo sends one 100-round unit");
        pane.tick(1400);
        button(pane, "selfRemoteHeal")->click();
        check(asked.back().first == PurchaseKind::RemoteHeal, "remote heal asks too");
        check(pane.sendPurchase(remote_heal_offer(view, true)), "remote heal goes out after the Y");
        parsed = common(capture.sent.back());
        check(parsed.cmd_type() == 6 && parsed.param() == 0, "remote heal is cmd 6");
        check(pane.statusText() == QStringLiteral("远程回血 · 等待回显"), "the status line names the command");
        SelfCommandView healed = infantry_in_match();
        healed.economy = 700;
        pane.setView(healed);
        pane.tick(1500);
        check(pane.statusText() == QStringLiteral("远程回血 · 已确认"), "spent coins confirm the command");
        pane.setPurchaseText(QStringLiteral("远程回血 · 已兑换，6 秒后生效"), false);
        check(pane.statusText() == QStringLiteral("远程回血 · 已兑换，6 秒后生效"),
              "the purchase result takes the status line");
        pane.setPurchaseText(QString(), false);
        SelfCommandView full = healed;
        full.hp = 200;
        pane.setView(full);
        pane.tick(1600);
        check(!button(pane, "selfRemoteHeal")->isEnabled() &&
                  button(pane, "selfRemoteHeal")->toolTip() == QStringLiteral("满血，不用回血"),
              "full HP: remote heal would only waste coins, the tooltip says so");
        SelfCommandView away = healed;
        away.at_exchange_point = false;
        pane.setView(away);
        pane.tick(1700);
        check(!button(pane, "selfExchange")->isEnabled() &&
                  button(pane, "selfExchange")->toolTip() == QStringLiteral("未在兑换点"),
              "off the exchange point the exchange button is grey and says why");
        check(button(pane, "selfRemoteAmmo")->isEnabled(), "remote ammo does not need the exchange point");
        away.shooter_online = false;
        away.at_exchange_point = true;
        pane.setView(away);
        pane.tick(1800);
        check(button(pane, "selfExchange")->toolTip() == QStringLiteral("17mm发射机构未连接"),
              "a disconnected launcher blocks the exchange like the official client");
        button(pane, "selfExchange")->click();
        check(asked.back().first != PurchaseKind::Supply, "a grey button asks nothing");

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
        check(hero_pane.sendPurchase(supply_offer(hero, true, Caliber::Large, 5)), "hero exchange goes out");
        parsed = common(hero_capture.sent.back());
        check(parsed.cmd_type() == 2 && parsed.param() == 5, "hero buys 42mm: cmd 2");
        hero_pane.tick(1200);
        check(hero_pane.sendPurchase(remote_ammo_offer(hero, true, Caliber::Large)), "hero remote ammo goes out");
        parsed = common(hero_capture.sent.back());
        check(parsed.cmd_type() == 5 && parsed.param() == 10, "hero remote ammo is one 10-round unit");
    }

    void self_pane_revive_follows_the_official_client()
    {
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
        popup.setActions([&confirms]
                         { ++confirms; }, [&pays]
                         { ++pays; });
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
        check(pay->isEnabled() && pay->text() == QStringLiteral("金币复活 · 100金 [F1]"),
              "paid revive shows its price and the official F1 key");
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

    void self_pane_rune_needs_a_hold_and_an_idle_rune()
    {
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

    void self_pane_performance_is_infantry_prematch_only()
    {
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
        check(pane.performanceChosen() && pane.performanceEcho() == QStringLiteral("等待回显") &&
                  pane.shownChassis() == std::optional<std::uint32_t>(2),
              "the P panel sees the pick and its echo before the sync comes back");
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
        const std::size_t sent_before = capture.sent.size();
        check(!pane.choosePerformance(2, 2) && capture.sent.size() == sent_before,
              "after the match starts the P panel cannot change it either");

        // 准备阶段点的就是现在生效的那一个(确认用默认):不发指令,但算选过了,提醒收起来。
        SelfCommandPane keep;
        Capture keep_capture;
        SelfCommandView defaults = infantry_in_match();
        defaults.stage = 1;
        keep.setView(defaults);
        keep.setSender(keep_capture.sender());
        keep.setLinkUp(true);
        keep.tick(1000);
        check(!keep.performanceChosen(), "nothing chosen yet in this preparation stage");
        check(keep.choosePerformance(std::nullopt, 1) && keep_capture.sent.empty() && keep.performanceChosen(),
              "picking the choice already in force sends nothing and counts as chosen");

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

    void self_pane_drops_clicks_while_the_link_is_down()
    {
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

    void write_review_pngs()
    {
        const auto build = [](QWidget* pane, const QString& heading)
        {
            return carded(pane, heading);
        };

        // 本车指令在花名册那一列:列宽 250,卡片内约 232;高度按 1440x900 和 1920x1080 两档。
        Capture self_capture;
        const auto self_card = [&](const SelfCommandView& view, const QString& name)
        {
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

        Capture capture;
        // 飞镖的几步各出一张:赛外、可开闸、闸门开着倒计时、本局用完。
        const auto dart_card_for = [&](const DartView& first, const std::vector<DartView>& feed,
                                       const QString& name)
        {
            auto* pane = new DartPane;
            pane->setSender(capture.sender());
            pane->setLinkUp(true);
            pane->tick(10000);
            pane->setView(first);
            for (const DartView& step : feed)
                pane->setView(step);
            std::unique_ptr<QWidget> card(build(pane, QStringLiteral("飞镖")));
            card->show();
            shoot(*card, name, 380, 300, true);
            return card;
        };
        DartView outside = dart_at(0, 0);
        outside.match_sec.reset();
        dart_card_for(outside, {}, QStringLiteral("dart-outside"));
        dart_card_for(dart_at(0, 40), {}, QStringLiteral("dart-ready"));
        DartView spent = dart_at(2, 100);
        spent.hits = 4;
        dart_card_for(spent, {}, QStringLiteral("dart-spent"));

        auto* dart = new DartPane;
        dart->setSender(capture.sender());
        dart->setLinkUp(true);
        dart->tick(10000);
        for (const auto& [gate, sec] : {std::pair<std::uint32_t, std::int32_t>{1, 91}, {2, 98}, {2, 109}})
            dart->setView(dart_at(gate, sec));
        std::unique_ptr<QWidget> dart_card(build(dart, QStringLiteral("飞镖")));
        dart_card->show();
        shoot(*dart_card, QStringLiteral("dart"), 380, 300, true);
        shoot(*dart_card, QStringLiteral("dart"), 320, 240, true);

        // 哨兵:比赛中带价、武装一颗、冷却中,外加小尺寸那一档。
        SentryView in_match = full_sentry();
        in_match.remaining_sec = 120;
        in_match.automatic = true;
        in_match.result.reset();
        const auto sentry_card_for = [&](const SentryView& view, const QString& name,
                                         const std::function<void(SentryPane&)>& act = {})
        {
            auto* pane = new SentryPane;
            pane->setView(view);
            pane->setSender(capture.sender());
            pane->setLinkUp(true);
            pane->tick(10000);
            std::unique_ptr<QWidget> card(build(pane, QStringLiteral("哨兵")));
            card->show();
            card->resize(380, 360);
            card->layout()->activate();
            QApplication::processEvents();
            if (act)
                act(*pane);
            shoot(*card, name, 380, 360, true);
            return card;
        };
        sentry_card_for(in_match, QStringLiteral("sentry-in-match"));
        sentry_card_for(in_match, QStringLiteral("sentry-armed"), [](SentryPane& pane)
                        { pane.findChild<QPushButton*>(QStringLiteral("sentryGoldRevive"))->click(); });
        SentryView cooling = in_match;
        cooling.result = SentryCtrlOutcome{8, 0, 9000};
        {
            auto* pane = new SentryPane;
            pane->setView(cooling);
            pane->setSender(capture.sender());
            pane->setLinkUp(true);
            // 成功只闪 4 秒,姿态冷却 5 秒:取中间这一段才看得到「姿态冷却 N 秒」。
            pane->tick(13500);
            std::unique_ptr<QWidget> card(build(pane, QStringLiteral("哨兵")));
            card->show();
            shoot(*card, QStringLiteral("sentry-cooling"), 380, 360, true);
        }

        auto* sentry = new SentryPane;
        sentry->setView(full_sentry());
        sentry->setSender(capture.sender());
        sentry->setLinkUp(true);
        sentry->tick(10000);
        std::unique_ptr<QWidget> sentry_card(build(sentry, QStringLiteral("哨兵")));
        sentry_card->show();
        shoot(*sentry_card, QStringLiteral("sentry"), 380, 360, true);
        shoot(*sentry_card, QStringLiteral("sentry"), 320, 240, true);

        // 标记工具条:默认、转发给哨兵开着、出错三张。放在地图卡片那样的深色底上量间距。
        const auto toolbar_shot = [](const QString& name, const std::function<void(MarkToolbar&)>& act)
        {
            auto* host = new QWidget;
            host->setStyleSheet(QStringLiteral("background:#0b1120;"));
            auto* column = new QVBoxLayout(host);
            column->setContentsMargins(8, 8, 8, 8);
            auto* bar = new MarkToolbar(host);
            bar->setLinkUp(true);
            column->addWidget(bar);
            host->show();
            host->resize(640, 70);
            host->layout()->activate();
            QApplication::processEvents();
            if (act)
                act(*bar);
            std::unique_ptr<QWidget> keep(host);
            shoot(*host, name, 640, 70);
        };
        toolbar_shot(QStringLiteral("mark-toolbar"), {});
        toolbar_shot(QStringLiteral("mark-toolbar-sentry-on"), [](MarkToolbar& bar)
                     {
        auto* toggle = bar.findChild<QPushButton*>(QStringLiteral("markSentry"));
        toggle->click();
        toggle->click(); });
        toolbar_shot(QStringLiteral("mark-toolbar-error"), [](MarkToolbar& bar)
                     { bar.setStatus(mark_rejection_text(MarkRejection::TooSoon), true); });

        // 空数据出一张:这种状态在赛前调试时最常见,也最容易画错。
        auto* waiting = new SentryPane;
        waiting->setView(SentryView{});
        waiting->setLinkUp(true);
        waiting->setSender(capture.sender());
        waiting->tick(1000);
        std::unique_ptr<QWidget> waiting_card(build(waiting, QStringLiteral("哨兵")));
        waiting_card->show();
        shoot(*waiting_card, QStringLiteral("sentry-waiting"), 380, 360, true);
    }

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    try
    {
        dart_f_asks_and_y_opens_the_gate();
        dart_l_launches_once_the_gate_is_fully_open();
        dart_j_cycles_targets_and_keeps_the_gate();
        dart_counts_gate_chances_like_the_official_client();
        dart_stops_after_four_darts_a_round();
        dart_link_down_drops_every_key();
        sentry_cools_down_posture_buttons_after_a_success();
        sentry_spends_coins_only_on_a_second_click();
        sentry_buttons_show_the_real_price();
        sentry_reads_the_right_seat_for_each_faction();
        builders_keep_missing_data_missing();
        role_panes_fit_a_narrow_card();
        panes_degrade_at_small_sizes();
        pane_scale_grows_and_shrinks_within_bounds();
        self_status_fills_whatever_box_it_gets();
        battle_analysis_fills_whatever_box_it_gets();
        next_step_fills_whatever_box_it_gets();
        self_pane_sends_exchanges_with_the_right_caliber();
        self_pane_revive_follows_the_official_client();
        self_pane_rune_needs_a_hold_and_an_idle_rune();
        self_pane_performance_is_infantry_prematch_only();
        self_pane_drops_clicks_while_the_link_is_down();
        write_review_pngs();
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    std::cout << "role_panes_test OK\n";
    return 0;
}
