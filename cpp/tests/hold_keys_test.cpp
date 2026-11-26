#include "hold_panels.h"
#include "rune_key.h"
#include "self_command_view.h"

#include <QApplication>
#include <QDir>
#include <QKeyEvent>
#include <QLineEdit>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
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

    template <class T>
    Field<T> valid(T value)
    {
        Field<T> field;
        field.value = value;
        field.last_valid = 1000;
        field.quality = Quality::Valid;
        field.freshness = Freshness::Fresh;
        return field;
    }

    SelfCommandView infantry_at(int elapsed, std::uint32_t rune_status)
    {
        SelfCommandView view;
        view.infantry = true;
        view.stage = 4;
        view.remaining = 420 - elapsed;
        view.rune_status = rune_status;
        return view;
    }

    void rune_key_follows_the_rules()
    {
        RuneKey key;
        key.update(infantry_at(10, 1), 1000);
        check(key.chances() == 1 && key.canOpen(), "the match starts with one small-rune chance (rule 5.6.3)");
        RuneKey::Display shown = key.display(1000);
        check(shown.visible && shown.lit && shown.text == QStringLiteral("F · 长按开启小能量机关 (1)"),
              "the official wording: 长按开启小能量机关 (1)");
        key.press(1000);
        check(key.holding() && !key.due(1900), "not yet at 0.9 s");
        check(key.display(1500).progress > 0.49 && key.display(1500).progress < 0.51, "half way shows half a bar");
        check(key.due(2000), "a full second: time to send");
        key.sent(true, 2000);
        check(!key.due(2100), "sent once per hold, never twice");
        check(key.display(2100).text == QStringLiteral("F · 开启指令已发出"), "says the command went out");
        key.release();
        key.update(infantry_at(11, 2), 2500);
        check(key.chances() == 0, "opening used the chance");
        shown = key.display(2500);
        check(!shown.lit && shown.text == QStringLiteral("F · 已开启，20s后结束"), "activating: counts down from 20 s");
        check(key.display(12500).text == QStringLiteral("F · 已开启，10s后结束"), "10 s later: 10 s left");
        key.update(infantry_at(31, 1), 22000);
        shown = key.display(22000);
        check(!shown.lit && shown.text == QStringLiteral("F · 长按开启小能量机关 (0)"),
              "not hit in time: back to idle with no chance left");
        key.press(22000);
        check(!key.holding(), "with no chance a press does nothing");

        key.update(infantry_at(95, 1), 30000);
        check(key.chances() == 1, "1:30 gives the next small chance");
        key.update(infantry_at(185, 1), 40000);
        check(key.chances() == 1 && key.display(40000).text == QStringLiteral("F · 长按开启大能量机关 (1)  已激活(0)"),
              "after 3:00 the big rune: unused small chances are gone, one big chance");
        key.update(infantry_at(186, 2), 41000);
        key.update(infantry_at(187, 2), 42000);
        SelfCommandView rings = infantry_at(188, 2);
        rings.rune_rings = 8.0;
        rings.rune_arms = 2;
        key.update(rings, 43000);
        check(key.display(43000).text == QStringLiteral("F · 已开启，18s后结束  平均环数/总臂数 8.0 / 2  已激活(0)"),
              "big rune activating shows average rings and lit arms");
        key.update(infantry_at(190, 3), 45000);
        check(key.display(45000).text == QStringLiteral("F · 大能量机关已激活，增益生效中  已激活(1)"),
              "activated: the count goes up");
        key.update(infantry_at(340, 1), 60000);
        check(key.chances() == 2, "4:15 and 5:30 each add a big chance, they pile up");

        RuneKey hero;
        SelfCommandView heavy = infantry_at(10, 1);
        heavy.infantry = false;
        hero.update(heavy, 1000);
        check(!hero.display(1000).visible && !hero.canOpen(), "the hero cannot open the rune: nothing shown");
        RuneKey before;
        SelfCommandView prep = infantry_at(10, 1);
        prep.stage = 1;
        before.update(prep, 1000);
        check(!before.display(1000).visible, "nothing before the match");
        RuneKey dropped;
        dropped.update(infantry_at(10, 1), 1000);
        dropped.press(1000);
        dropped.update(prep, 1500);
        check(!dropped.holding(), "the match ending cancels a hold");
    }

    Snapshot infantry_snapshot()
    {
        Snapshot snapshot;
        snapshot.self = RobotId{3};
        snapshot.game.red_total_damage = valid<std::uint32_t>(1200);
        snapshot.game.blue_total_damage = valid<std::uint32_t>(800);
        snapshot.dart_hits.red = 2;
        snapshot.dart_hits.blue = 5;
        for (const std::uint32_t id : {1u, 3u, 4u, 7u, 101u, 103u, 104u, 107u})
        {
            RobotHealth health;
            health.id = RobotId{id};
            health.faction = faction_of(id);
            health.current_hp = valid<std::uint32_t>(id % 100 == 3 ? 150 : 100);
            health.max_hp = valid<std::uint32_t>(200);
            snapshot.robot_health.push_back(health);
        }
        RobotState& self = snapshot.robots[RobotId{3}];
        self.statics.max_power = valid<std::uint32_t>(45);
        self.statics.max_heat = valid<std::uint32_t>(40);
        self.statics.heat_cooldown_rate = valid<double>(12.0);
        self.injury.total = valid<std::uint32_t>(200);
        self.injury.small_projectile = valid<std::uint32_t>(150);
        self.injury.collision = valid<std::uint32_t>(50);
        self.injury.large_projectile = valid<std::uint32_t>(0);
        self.modules.armor = valid<std::uint32_t>(1);
        self.modules.small_shooter = valid<std::uint32_t>(0);
        self.modules.video_transmission = valid<std::uint32_t>(2);
        return snapshot;
    }

    void panels_show_what_the_protocol_gives()
    {
        const Snapshot snapshot = infantry_snapshot();
        const TeamView team = team_view(snapshot);
        check(team.red_damage == std::optional<std::uint32_t>(1200) && team.blue_damage == std::optional<std::uint32_t>(800),
              "Tab: total damage of both sides");
        check(team.own_hp_total == std::optional<std::uint32_t>(450) && team.own_dart_hits == 2,
              "Tab: own robots' total HP and own dart hits");
        check(team.red.size() == 4 && team.blue.size() == 4, "Tab: four field robots a side");
        const auto self = std::find_if(team.red.begin(), team.red.end(), [](const TeamView::Row& row)
                                       { return row.self; });
        check(self != team.red.end() && self->id == 3 && self->power == std::optional<std::uint32_t>(45) &&
                  self->max_heat == std::optional<std::uint32_t>(40) && self->cooling == std::optional<double>(12.0),
              "Tab: own robot shows chassis power, heat limit and cooling");
        check(!team.red[0].power && team.blue[1].hp == std::optional<std::uint32_t>(150),
              "Tab: other robots only have HP (the protocol sends nothing else)");

        const InjuryView injury = injury_view(snapshot);
        check(injury.total == std::optional<std::uint32_t>(200) && injury.rows[0].name == QStringLiteral("17mm 弹丸") &&
                  injury.rows[0].value == std::optional<std::uint32_t>(150),
              "~: damage by cause for this life");
        check(injury.modules[0].name == QStringLiteral("装甲") && injury.modules[0].state == std::optional<std::uint32_t>(1),
              "~: armor online");
        check(injury.modules[1].name == QStringLiteral("17mm 测速") && injury.modules[1].state == std::optional<std::uint32_t>(0),
              "~: an infantry robot shows its 17mm speed module");
        check(injury.modules[2].state == std::optional<std::uint32_t>(2), "~: installed wrongly is kept apart");
        Snapshot hero = snapshot;
        hero.self = RobotId{1};
        check(injury_view(hero).modules[1].name == QStringLiteral("42mm 测速"), "~: the hero shows its 42mm module");

        QWidget host;
        host.resize(1440, 900);
        host.show();
        TeamPanel team_panel(&host);
        InjuryPanel injury_panel(&host);
        team_panel.setView(team);
        injury_panel.setView(injury);
        check(!team_panel.isVisible() && !injury_panel.isVisible(), "both hidden until held");
        team_panel.setHeld(true);
        injury_panel.setHeld(true);
        QApplication::processEvents();
        check(team_panel.isVisible() && std::abs(team_panel.geometry().center().x() - 720) <= 1,
              "Tab panel in the middle of the screen");
        check(injury_panel.isVisible() && injury_panel.geometry().left() == 24 &&
                  host.rect().contains(injury_panel.geometry()),
              "~ panel on the left, inside the window");
        QDir().mkpath(QStringLiteral(RM_TERMINAL_ARTIFACT_DIR "/hold"));
        check(host.grab().save(QStringLiteral(RM_TERMINAL_ARTIFACT_DIR "/hold/tab-and-tilde.png")), "review PNG saved");
        team_panel.setHeld(false);
        injury_panel.setHeld(false);
        check(!team_panel.isVisible() && !injury_panel.isVisible(), "released: both gone");
    }

    void key(QWidget& target, QEvent::Type type, int code, bool repeat = false)
    {
        QKeyEvent event(type, code, Qt::NoModifier, QString(), repeat);
        QApplication::sendEvent(&target, &event);
    }

    void hold_keys_are_tracked()
    {
        QWidget window;
        auto* layout = new QVBoxLayout(&window);
        auto* typing = new QLineEdit;
        auto* other = new QWidget;
        other->setFocusPolicy(Qt::StrongFocus);
        layout->addWidget(typing);
        layout->addWidget(other);
        window.show();
        window.activateWindow();
        other->setFocus();
        QApplication::processEvents();
        HoldKeyFilter filter;
        std::vector<std::pair<char, bool>> seen;
        QObject::connect(&filter, &HoldKeyFilter::tabChanged, [&seen](bool down)
                         { seen.emplace_back('T', down); });
        QObject::connect(&filter, &HoldKeyFilter::tildeChanged, [&seen](bool down)
                         { seen.emplace_back('~', down); });
        QObject::connect(&filter, &HoldKeyFilter::runeKeyChanged, [&seen](bool down)
                         { seen.emplace_back('F', down); });
        qApp->installEventFilter(&filter);
        key(*other, QEvent::KeyPress, Qt::Key_Tab);
        key(*other, QEvent::KeyRelease, Qt::Key_Tab, true);
        key(*other, QEvent::KeyPress, Qt::Key_Tab, true);
        check(seen.size() == 1 && seen[0] == std::make_pair('T', true), "Tab down; auto-repeat is ignored");
        key(*other, QEvent::KeyRelease, Qt::Key_Tab);
        check(seen.size() == 2 && seen[1] == std::make_pair('T', false), "Tab up");
        key(*other, QEvent::KeyPress, Qt::Key_QuoteLeft);
        key(*other, QEvent::KeyRelease, Qt::Key_QuoteLeft);
        key(*other, QEvent::KeyPress, Qt::Key_F);
        check(seen.size() == 5 && seen[2].first == '~' && seen[4] == std::make_pair('F', true), "~ and F are seen too");
        QEvent deactivate(QEvent::WindowDeactivate);
        QApplication::sendEvent(&window, &deactivate);
        check(seen.size() == 6 && seen[5] == std::make_pair('F', false),
              "the window going to the back counts as letting go: the rune is never opened unattended");
        typing->setFocus();
        QApplication::processEvents();
        key(*typing, QEvent::KeyPress, Qt::Key_F);
        key(*typing, QEvent::KeyPress, Qt::Key_Tab);
        check(seen.size() == 6, "in a text box F and Tab are just typing");
        qApp->removeEventFilter(&filter);
    }

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    try
    {
        rune_key_follows_the_rules();
        panels_show_what_the_protocol_gives();
        hold_keys_are_tracked();
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    std::cout << "hold_keys_test OK\n";
    return 0;
}
