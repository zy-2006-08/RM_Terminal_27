#include "purchase.h"
#include "purchase_popups.h"
#include "self_command_view.h"

#include "rm_terminal.pb.h"

#include <QApplication>
#include <QDir>
#include <QMouseEvent>
#include <QPushButton>
#include <QWidget>

#include <cmath>
#include <iostream>
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

    QString artifact_dir()
    {
        const QString dir = QStringLiteral(RM_TERMINAL_ARTIFACT_DIR) + QStringLiteral("/purchase");
        QDir().mkpath(dir);
        return dir;
    }

    // 比赛中、脱战、站在补给区、发射机构在线、800 金、满后勤的步兵。
    SelfCommandView infantry()
    {
        SelfCommandView view;
        view.infantry = true;
        view.stage = 4;
        view.remaining = 300;
        view.economy = 800;
        view.ammo = 0;
        view.fired = 0;
        view.hp = 50;
        view.max_hp = 200;
        view.out_of_combat = true;
        view.can_remote_heal = true;
        view.can_remote_ammo = true;
        view.at_exchange_point = true;
        view.shooter_online = true;
        view.logistics = 1.0;
        view.dead = false;
        return view;
    }

    SelfCommandView hero()
    {
        SelfCommandView view = infantry();
        view.infantry = false;
        return view;
    }

    template <class T>
    Field<T> valid(T value, MonotonicMs at, Freshness freshness)
    {
        Field<T> field;
        field.value = value;
        field.last_valid = at;
        field.quality = Quality::Valid;
        field.freshness = freshness;
        return field;
    }

    QPushButton* button(QWidget& parent, const QString& name)
    {
        auto* found = parent.findChild<QPushButton*>(name);
        if (!found)
            throw std::runtime_error("missing button " + name.toStdString());
        return found;
    }

    struct Sent
    {
        std::vector<PurchaseOffer> offers;
        PurchaseFlow::Sender sender()
        {
            return [this](const PurchaseOffer& offer)
            {
                offers.push_back(offer);
                return true;
            };
        }
    };

    void click_at(QWidget& widget, const QPoint& point)
    {
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(point), widget.mapToGlobal(QPointF(point)),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&widget, &release);
    }

    void save(QWidget& host, const QString& name)
    {
        QApplication::processEvents();
        const QString path = artifact_dir() + QLatin1Char('/') + name + QStringLiteral(".png");
        check(host.grab().save(path), "review PNG saved");
        std::cout << "  wrote " << path.toStdString() << '\n';
    }

    void supply_follows_the_rules()
    {
        const SelfCommandView view = infantry();
        PurchaseOffer offer = supply_offer(view, true, Caliber::Small, 50);
        check(offer.available && offer.cost == 50 && offer.cmd_type == 1 && offer.param == 50,
              "17mm: 50 rounds cost 50 coins, CommonCommand 1 with the round count");
        check(offer.question == QStringLiteral("此次兑换将消耗 50 金币，请确认是否兑换"),
              "the confirmation uses the official client's words");
        check(supply_offer(view, true, Caliber::Large, 5).reason == QStringLiteral("本车不用 42mm 弹丸"),
              "an infantry robot cannot buy 42mm");
        SelfCommandView away = view;
        away.at_exchange_point = false;
        check(supply_offer(away, true, Caliber::Small, 50).reason == QStringLiteral("未在兑换点"),
              "off the exchange point: refused, like 「未在补给区」 in the official client");
        away.at_exchange_point = std::nullopt;
        check(!supply_offer(away, true, Caliber::Small, 50).available,
              "never told about an exchange point this run: not on one");
        SelfCommandView offline = view;
        offline.shooter_online = false;
        check(supply_offer(offline, true, Caliber::Small, 50).reason == QStringLiteral("17mm发射机构未连接"),
              "a disconnected launcher blocks the exchange");
        SelfCommandView prep = view;
        prep.stage = 1;
        check(supply_offer(prep, true, Caliber::Small, 50).reason == QStringLiteral("比赛开始后才能兑换"),
              "before the match: refused");
        SelfCommandView dead = view;
        dead.dead = true;
        check(supply_offer(dead, true, Caliber::Small, 50).reason == QStringLiteral("阵亡，复活后才能兑换"),
              "dead: refused");
        check(supply_offer(view, false, Caliber::Small, 50).reason == QStringLiteral("断线，指令不会发送"),
              "link down: refused");
        SelfCommandView poor = view;
        poor.economy = 5;
        check(supply_offer(poor, true, Caliber::Small, 10).reason == QStringLiteral("金币不足，无法兑换"),
              "not even 10 coins: 「金币不足，无法兑换」");
        poor.economy = 40;
        check(!supply_offer(poor, true, Caliber::Small, 50).available, "40 coins do not buy 50 rounds");
        check(supply_offer(view, true, Caliber::Small, 0).reason == QStringLiteral("先选兑换数量"), "0 rounds: pick first");
        check(supply_offer(view, true, Caliber::Small, 15).reason == QStringLiteral("17mm 要按 10 发的整数倍兑换"),
              "17mm only in tens (protocol 2.2.25)");

        SelfCommandView rich = view;
        rich.economy = 755;
        check(supply_max_rounds(rich, Caliber::Small) == 750, "755 coins buy at most 750 rounds of 17mm");
        check(supply_max_rounds(rich, Caliber::Large) == 75, "755 coins buy at most 75 rounds of 42mm");
        const PurchaseOffer heavy = supply_offer(hero(), true, Caliber::Large, 5);
        check(heavy.available && heavy.cost == 50 && heavy.cmd_type == 2 && heavy.param == 5,
              "42mm: 10 coins a round, CommonCommand 2");
    }

    void remote_exchange_follows_the_rules()
    {
        const SelfCommandView view = infantry();
        const PurchaseOffer ammo = remote_ammo_offer(view, true, Caliber::Small);
        check(ammo.available && ammo.cost == 150 && ammo.cmd_type == 5 && ammo.param == 100,
              "remote 17mm: 150 coins for 100 rounds, CommonCommand 5");
        check(ammo.question == QStringLiteral("是否消耗 150 金币，兑换 100 发 17mm 弹丸？"), "remote ammo question");
        SelfCommandView away = view;
        away.at_exchange_point = false;
        check(remote_ammo_offer(away, true, Caliber::Small).available, "remote exchange works anywhere");
        SelfCommandView fighting = view;
        fighting.out_of_combat = false;
        fighting.can_remote_ammo = false;
        fighting.can_remote_heal = false;
        check(remote_ammo_offer(fighting, true, Caliber::Small).reason == QStringLiteral("交战中，脱战后才能远程兑换"),
              "in combat: the server says no and the terminal says why");
        SelfCommandView other = view;
        other.can_remote_ammo = false;
        check(remote_ammo_offer(other, true, Caliber::Small).reason == QStringLiteral("不满足远程兑换条件"),
              "the server says no for another reason: the official wording");
        SelfCommandView bare = hero();
        bare.can_remote_ammo = false;
        bare.shooter_online = false;
        check(remote_ammo_offer(bare, true, Caliber::Large).reason == QStringLiteral("42mm发射机构未连接"),
              "a hero without its 42mm launcher: says the launcher is missing");
        check(remote_ammo_offer(hero(), true, Caliber::Large).param == 10, "remote 42mm is one 10-round unit");
        SelfCommandView poor = view;
        poor.economy = 100;
        check(remote_ammo_offer(poor, true, Caliber::Small).reason == QStringLiteral("金币不足，无法兑换"),
              "100 coins do not pay 150");

        const PurchaseOffer heal = remote_heal_offer(view, true);
        check(heal.available && heal.cost == 90 && heal.cmd_type == 6 && heal.param == 0,
              "remote heal at 5:00 left costs 50 + ROUNDUP(120 / 60 × 20) = 90 (table 5-7)");
        check(heal.heal == 120 && heal.question == QStringLiteral("是否消耗 90 金币，兑换回血（回 120 血）？"),
              "60% of 200 HP with full logistics: 120 HP, written in the question");
        SelfCommandView worn = view;
        worn.logistics = 0.5;
        check(remote_heal_amount(worn) == std::optional<std::uint32_t>(60), "base worn down: λ 0.5 halves the heal");
        SelfCommandView near_full = view;
        near_full.hp = 170;
        check(remote_heal_amount(near_full) == std::optional<std::uint32_t>(30), "the heal stops at max HP");
        SelfCommandView full = view;
        full.hp = 200;
        check(remote_heal_offer(full, true).reason == QStringLiteral("满血，不用回血"), "full HP: refused");
        check(remote_heal_offer(fighting, true).reason == QStringLiteral("交战中，脱战后才能远程兑换"),
              "in combat: no remote heal");
        SelfCommandView no_clock = view;
        no_clock.remaining = std::nullopt;
        check(!remote_heal_offer(no_clock, true).available, "no match clock: no price, no heal");
    }

    void view_reads_the_new_messages()
    {
        Snapshot snapshot;
        snapshot.self = RobotId{3};
        snapshot.game.red_base_hp = valid<std::uint32_t>(8000, 1000, Freshness::Fresh);
        snapshot.game.red_base_max_hp = valid<std::uint32_t>(10000, 1000, Freshness::Fresh);
        RobotState& self = snapshot.robots[RobotId{3}];
        self.dynamic.total_fired = valid<std::uint32_t>(42, 1000, Freshness::Fresh);
        self.modules.small_shooter = valid<std::uint32_t>(1, 1000, Freshness::Fresh);
        SelfCommandView view = self_command_view(snapshot);
        check(!view.at_exchange_point, "no Buff 6 this run: unknown");
        check(view.shooter_online == std::optional<bool>(true) && view.fired == std::optional<std::uint32_t>(42),
              "17mm speed module online, total fired read");
        check(view.logistics && std::abs(*view.logistics - 0.9) < 1e-9, "base lost 2000 HP: λ = 0.9 (rule 5.6.1)");
        self.buffs.can_exchange_ammo = valid(true, 1000, Freshness::Fresh);
        check(self_command_view(snapshot).at_exchange_point == std::optional<bool>(true), "fresh Buff 6: on the point");
        self.buffs.can_exchange_ammo.freshness = Freshness::Stale;
        check(self_command_view(snapshot).at_exchange_point == std::optional<bool>(false),
              "Buff 6 stopped coming: left the point");
        self.modules.small_shooter = valid<std::uint32_t>(2, 1000, Freshness::Fresh);
        check(self_command_view(snapshot).shooter_online == std::optional<bool>(false),
              "module 2 (installed wrongly) counts as offline (protocol 2.2.12)");
    }

    void watch_tells_arrived_from_missing()
    {
        SelfCommandView view = infantry();
        PurchaseWatch watch;
        check(watch.text(0).isEmpty() && !watch.kind(), "nothing bought: nothing to say");
        watch.sent(supply_offer(view, true, Caliber::Small, 50), view, 1000);
        watch.update(view, 1500);
        check(watch.text(1500) == QStringLiteral("兑换弹丸 · 已兑换 50 发，等待到账"), "supply: waiting");
        view.ammo = 50;
        watch.update(view, 1600);
        check(watch.state() == PurchaseWatch::State::Arrived && watch.text(1600) == QStringLiteral("兑换弹丸 · 到账 +50 发"),
              "the allowance went up by 50: arrived");
        check(watch.text(6600).isEmpty(), "the result goes away after 5 seconds");

        view.ammo = 50;
        watch.sent(supply_offer(view, true, Caliber::Small, 50), view, 10000);
        watch.update(view, 12900);
        check(watch.state() == PurchaseWatch::State::Waiting, "supply: still waiting before 3 s");
        watch.update(view, 13000);
        check(watch.state() == PurchaseWatch::State::Missing && watch.problem() &&
                  watch.text(13000) == QStringLiteral("兑换弹丸 · 没到账"),
              "nothing after 3 s: missing");

        SelfCommandView shooting = infantry();
        shooting.ammo = 30;
        shooting.fired = 10;
        watch.sent(remote_ammo_offer(shooting, true, Caliber::Small), shooting, 20000);
        check(watch.text(20000) == QStringLiteral("远程补弹 · 已兑换，6 秒后生效"), "remote: takes effect in 6 s");
        shooting.ammo = 20;
        shooting.fired = 20;
        watch.update(shooting, 23000);
        check(watch.state() == PurchaseWatch::State::Waiting, "shooting while waiting is not a loss");
        shooting.ammo = 120;
        watch.update(shooting, 26200);
        check(watch.state() == PurchaseWatch::State::Arrived && watch.text(26200) == QStringLiteral("远程补弹 · 到账 +100 发"),
              "100 rounds arrived even though 10 were fired meanwhile");
        shooting.ammo = 120;
        watch.sent(remote_ammo_offer(shooting, true, Caliber::Small), shooting, 30000);
        watch.update(shooting, 38900);
        check(watch.state() == PurchaseWatch::State::Waiting, "remote: still waiting before 9 s");
        watch.update(shooting, 39000);
        check(watch.state() == PurchaseWatch::State::Missing, "remote: nothing after 6 s plus margin: missing");

        SelfCommandView hurt = infantry();
        watch.sent(remote_heal_offer(hurt, true), hurt, 40000);
        hurt.hp = 40;
        watch.update(hurt, 42000);
        hurt.hp = 160;
        watch.update(hurt, 46100);
        check(watch.state() == PurchaseWatch::State::Arrived && watch.text(46100) == QStringLiteral("远程回血 · 到账 +120 血"),
              "heal counted from the lowest HP seen while waiting");
        hurt = infantry();
        watch.sent(remote_heal_offer(hurt, true), hurt, 50000);
        hurt.dead = true;
        hurt.hp = 0;
        watch.update(hurt, 53000);
        check(watch.state() == PurchaseWatch::State::Voided &&
                  watch.text(53000) == QStringLiteral("远程回血 · 6 秒内阵亡，这次回血作废，金币不退"),
              "died within 6 s: the heal is void and the coins are gone (rule 5.3.1)");
    }

    void confirm_box_matches_the_official_client()
    {
        QWidget host;
        host.resize(800, 500);
        host.show();
        PurchaseConfirm confirm(&host);
        confirm.setOffer(supply_offer(infantry(), true, Caliber::Small, 50));
        check(button(confirm, QStringLiteral("purchaseYes"))->text() == QStringLiteral("是 [Y]") &&
                  button(confirm, QStringLiteral("purchaseNo"))->text() == QStringLiteral("否 [N]"),
              "supply confirmation: 是 / 否");
        confirm.setOffer(remote_ammo_offer(infantry(), true, Caliber::Small));
        check(button(confirm, QStringLiteral("purchaseYes"))->text() == QStringLiteral("确定 [Y]") &&
                  button(confirm, QStringLiteral("purchaseNo"))->text() == QStringLiteral("取消 [N]"),
              "remote confirmation: 确定 / 取消");
        SelfCommandView fighting = infantry();
        fighting.can_remote_ammo = false;
        fighting.out_of_combat = false;
        confirm.setOffer(remote_ammo_offer(fighting, true, Caliber::Small));
        check(!button(confirm, QStringLiteral("purchaseYes"))->isEnabled() &&
                  confirm.questionText() == QStringLiteral("现在买不了：交战中，脱战后才能远程兑换"),
              "conditions gone while asking: 确定 greys out and the box says why");
    }

    void remote_popup_works_like_the_h_key()
    {
        QWidget host;
        host.resize(1440, 900);
        host.show();
        PurchaseFlow flow(&host);
        Sent sent;
        flow.setSender(sent.sender());
        SelfCommandView view = infantry();
        flow.update(view, true, 1000);
        check(!flow.remotePopup()->isVisible(), "closed until H");
        check(!flow.chooseRemote(1), "Ctrl+2 does nothing while the box is closed");
        flow.toggleRemote();
        RemoteExchangePopup& popup = *flow.remotePopup();
        check(flow.remoteOpen() && popup.isVisible(), "H opens the remote exchange box");
        const QRect box = popup.geometry();
        check(host.rect().contains(box) && std::abs(box.center().x() - 720) <= 1 && box.bottom() > 700,
              "the box sits at the bottom centre, like the official client");
        check(popup.tileText(0) == QStringLiteral("+120 血 · 90 金"), "the heal tile says how much HP it really gives");
        check(popup.tileText(1) == QStringLiteral("100 发 · 150 金"), "17mm tile: 100 rounds for 150 coins");
        check(popup.tileText(2) == QStringLiteral("本车不用 42mm 弹丸"), "42mm tile greyed out on an infantry robot");
        check(popup.barText() == QStringLiteral("按 H 关闭远程兑换面板"), "the bar says how to close it");
        save(host, QStringLiteral("remote-open"));
        check(!flow.chooseRemote(2), "Ctrl+3 on a grey tile does nothing");
        check(flow.chooseRemote(1) && flow.confirmOpen(), "Ctrl+2 asks for a confirmation");
        check(flow.confirmBox()->questionText() == QStringLiteral("是否消耗 150 金币，兑换 100 发 17mm 弹丸？"),
              "the confirmation names coins, rounds and calibre");
        save(host, QStringLiteral("remote-confirm"));
        check(flow.no() && !flow.confirmOpen() && sent.offers.empty(), "N cancels without buying");
        check(flow.chooseRemote(1) && flow.yes(), "Ctrl+2 then Y");
        check(sent.offers.size() == 1 && sent.offers[0].cmd_type == 5 && sent.offers[0].param == 100 && !flow.confirmOpen(),
              "Y sends remote ammo once and closes the confirmation");
        check(flow.statusText() == QStringLiteral("远程补弹 · 已兑换，6 秒后生效") &&
                  popup.barText() == flow.statusText(),
              "the bar says it takes effect in 6 s");
        check(!flow.yes(), "with no confirmation open, Y is left to the revive");
        view.ammo = 100;
        view.economy = 650;
        flow.update(view, true, 7200);
        check(flow.statusText() == QStringLiteral("远程补弹 · 到账 +100 发"), "6 s later the rounds arrived");
        flow.toggleRemote();
        check(!flow.remoteOpen() && popup.isVisible() && popup.barText() == QStringLiteral("远程补弹 · 到账 +100 发"),
              "H closes the box; only the result bar stays for a moment");
        save(host, QStringLiteral("remote-result-bar"));
        flow.update(view, true, 12300);
        check(!popup.isVisible(), "then the bar goes away too");

        flow.toggleRemote();
        QApplication::processEvents();
        click_at(popup, popup.tileRect(0).center());
        check(flow.confirmOpen() && flow.confirmBox()->offer().kind == PurchaseKind::RemoteHeal,
              "clicking the heal tile asks too, like Ctrl+1");
        check(flow.confirmBox()->questionText() == QStringLiteral("是否消耗 90 金币，兑换回血（回 120 血）？"),
              "the heal confirmation shows today's price and the real HP");
        flow.no();

        SelfCommandView fighting = view;
        fighting.out_of_combat = false;
        fighting.can_remote_ammo = false;
        fighting.can_remote_heal = false;
        flow.update(fighting, true, 13000);
        check(popup.barText() == QStringLiteral("交战中，脱战后才能远程兑换") && !flow.chooseRemote(0),
              "in combat the box still opens and says why nothing can be bought");
        save(host, QStringLiteral("remote-in-combat"));
    }

    void supply_panel_works_like_the_o_key()
    {
        QWidget host;
        host.resize(1440, 900);
        host.show();
        PurchaseFlow flow(&host);
        Sent sent;
        flow.setSender(sent.sender());
        std::vector<bool> opened;
        QObject::connect(&flow, &PurchaseFlow::supplyOpenChanged, [&opened](bool open)
                         { opened.push_back(open); });
        SelfCommandView view = infantry();
        flow.update(view, true, 1000);
        flow.toggleSupply(Caliber::Small);
        SupplyPanel& panel = *flow.supplyPanel();
        check(flow.supplyOpen() && opened == std::vector<bool>{true}, "O opens the 17mm supply panel and says so");
        const QRect box = panel.geometry();
        check(std::abs(box.center().x() - 720) <= 1 && std::abs(box.center().y() - 450) <= 1, "the panel sits centred");
        check(panel.infoText() == QStringLiteral("0/800"), "have / can buy with today's coins: 0/800");
        check(button(panel, QStringLiteral("supplyStep0"))->text() == QStringLiteral("−100") &&
                  button(panel, QStringLiteral("supplyStep7"))->text() == QStringLiteral("+100"),
              "−100 … +100 like the official panel");
        check(!button(panel, QStringLiteral("supplyStep3"))->isEnabled() &&
                  !button(panel, QStringLiteral("supplyConfirm"))->isEnabled(),
              "at 0 there is nothing to take away and nothing to confirm");
        button(panel, QStringLiteral("supplyStep5"))->click();
        button(panel, QStringLiteral("supplyStep5"))->click();
        check(panel.quantity() == 40, "two clicks on +20 make 40 (the manual's example)");
        button(panel, QStringLiteral("supplyStep7"))->click();
        button(panel, QStringLiteral("supplyStep1"))->click();
        check(panel.quantity() == 90, "+100 then −50: 90");
        SelfCommandView poor = view;
        poor.economy = 55;
        flow.update(poor, true, 1250);
        check(panel.quantity() == 50 && panel.infoText() == QStringLiteral("0/50") &&
                  !button(panel, QStringLiteral("supplyStep4"))->isEnabled(),
              "the count never goes past what the coins can buy");
        save(host, QStringLiteral("supply-open"));
        button(panel, QStringLiteral("supplyConfirm"))->click();
        check(flow.confirmOpen() &&
                  flow.confirmBox()->questionText() == QStringLiteral("此次兑换将消耗 50 金币，请确认是否兑换"),
              "确定 asks again with the official words");
        save(host, QStringLiteral("supply-confirm"));
        check(flow.yes() && sent.offers.size() == 1 && sent.offers[0].cmd_type == 1 && sent.offers[0].param == 50,
              "是 / Y buys 50 rounds of 17mm");
        check(flow.supplyOpen() && flow.statusText() == QStringLiteral("兑换弹丸 · 已兑换 50 发，等待到账"),
              "the panel stays open and shows the result");

        SelfCommandView away = view;
        away.at_exchange_point = false;
        flow.update(away, true, 1500);
        check(panel.infoText() == QStringLiteral("未在兑换点") && panel.quantity() == 0 &&
                  !button(panel, QStringLiteral("supplyStep7"))->isEnabled(),
              "off the point the whole row greys out and the panel says why");
        save(host, QStringLiteral("supply-away"));
        SelfCommandView offline = view;
        offline.shooter_online = false;
        flow.update(offline, true, 1600);
        check(panel.infoText() == QStringLiteral("17mm发射机构未连接"), "launcher not connected: the official wording");
        SelfCommandView broke = view;
        broke.economy = 0;
        flow.update(broke, true, 1700);
        check(panel.infoText() == QStringLiteral("金币不足，无法兑换"), "no coins: the official wording");

        flow.update(view, true, 1800);
        flow.toggleSupply(Caliber::Large);
        check(flow.supplyOpen() && panel.caliber() == Caliber::Large &&
                  panel.infoText() == QStringLiteral("本车不用 42mm 弹丸") &&
                  button(panel, QStringLiteral("supplyStep7"))->text() == QStringLiteral("+10"),
              "I on an infantry robot shows the 42mm panel and why it cannot buy");
        flow.toggleRemote();
        check(!flow.supplyOpen() && flow.remoteOpen() && opened.back() == false,
              "H closes the supply panel: one box at a time");
        flow.toggleSupply(Caliber::Small);
        check(flow.supplyOpen() && !flow.remoteOpen(), "O closes the remote box");
        flow.toggleSupply(Caliber::Small);
        check(!flow.supplyOpen() && opened.back() == false, "O again closes the panel");

        flow.toggleSupply(Caliber::Small);
        button(panel, QStringLiteral("supplyStep4"))->click();
        button(panel, QStringLiteral("supplyConfirm"))->click();
        SelfCommandView dead = view;
        dead.dead = true;
        flow.update(dead, true, 2000);
        check(!flow.supplyOpen() && !flow.confirmOpen(), "dying closes the panel and the confirmation: the revive box needs the centre");
        flow.toggleSupply(Caliber::Small);
        check(!flow.supplyOpen() && flow.remotePopup()->barText() == QStringLiteral("阵亡，复活后才能兑换"),
              "O while dead says why instead of opening");
        check(sent.offers.size() == 1, "nothing else was bought");

        SupplyPanel heavy(&host);
        heavy.setCaliber(Caliber::Large);
        heavy.setView(hero(), true);
        check(heavy.infoText() == QStringLiteral("0/80") && button(heavy, QStringLiteral("supplyStep7"))->text() ==
                                                                QStringLiteral("+10"),
              "42mm panel: 800 coins buy 80 rounds, steps 1/2/5/10");
    }

    void pane_buttons_use_the_same_confirmation()
    {
        QWidget host;
        host.resize(1440, 900);
        host.show();
        PurchaseFlow flow(&host);
        Sent sent;
        flow.setSender(sent.sender());
        flow.update(infantry(), true, 1000);
        flow.ask(PurchaseKind::Supply, 50);
        const QRect box = flow.confirmBox()->geometry();
        check(flow.confirmOpen() && std::abs(box.center().x() - 720) <= 1 && std::abs(box.center().y() - 450) <= 1,
              "本车「兑换」asks in the middle of the screen");
        check(flow.yes() && sent.offers.size() == 1 && sent.offers[0].param == 50, "and Y buys");
        SelfCommandView away = infantry();
        away.at_exchange_point = false;
        flow.update(away, true, 1200);
        flow.ask(PurchaseKind::Supply, 50);
        check(!flow.confirmOpen(), "a purchase that cannot happen opens nothing");
    }

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    try
    {
        supply_follows_the_rules();
        remote_exchange_follows_the_rules();
        view_reads_the_new_messages();
        watch_tells_arrived_from_missing();
        confirm_box_matches_the_official_client();
        remote_popup_works_like_the_h_key();
        supply_panel_works_like_the_o_key();
        pane_buttons_use_the_same_confirmation();
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    std::cout << "purchase_test OK\n";
    return 0;
}
