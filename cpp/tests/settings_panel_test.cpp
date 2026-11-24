#include "config.h"
#include "dashboard.h"
#include "input_method_guard.h"
#include "settings_panel.h"
#include "terminal_settings.h"

#if defined(RM_TERMINAL_PIPER_SPEECH)
#include "piper_speech_backend.h"
#endif

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QInputMethodQueryEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QVBoxLayout>

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

QPushButton* button(QWidget& parent, const char* name) {
    auto* found = parent.findChild<QPushButton*>(QString::fromLatin1(name));
    if (!found) throw std::runtime_error(std::string("missing button ") + name);
    return found;
}

QSlider* slider(QWidget& parent, const char* name) {
    auto* found = parent.findChild<QSlider*>(QString::fromLatin1(name));
    if (!found) throw std::runtime_error(std::string("missing slider ") + name);
    return found;
}

bool accepts_input_method() {
    QObject* focus = QGuiApplication::focusObject();
    if (!focus) return false;
    QInputMethodQueryEvent query(Qt::ImEnabled);
    QCoreApplication::sendEvent(focus, &query);
    return query.value(Qt::ImEnabled).toBool();
}

bool focus_accepts(QWidget* widget) {
    widget->setFocus(Qt::OtherFocusReason);
    QCoreApplication::processEvents();
    return accepts_input_method();
}

void settings_are_saved_per_machine() {
    QTemporaryDir directory;
    const QString path = QDir(directory.path()).filePath(QStringLiteral("terminal_settings.json"));
    QString error;
    check(load_terminal_settings(path, &error) == TerminalSettings{} && error.isEmpty(),
          "no file yet: the defaults, without an error");
    TerminalSettings changed;
    changed.sensitivity = 80;
    changed.volume = 35;
    changed.video_reversed = true;
    check(save_terminal_settings(path, changed, &error), "settings are saved");
    check(load_terminal_settings(path, &error) == changed, "and read back unchanged");
    QFile broken(path);
    check(broken.open(QIODevice::WriteOnly | QIODevice::Truncate) && broken.write("{not json") > 0, "file broken");
    broken.close();
    check(load_terminal_settings(path, &error) == TerminalSettings{} && !error.isEmpty(),
          "a broken file falls back to the defaults and says so");
    TerminalSettings wild;
    wild.sensitivity = 400;
    wild.volume = -5;
    check(normalized(wild).sensitivity == 100 && normalized(wild).volume == 0, "out-of-range values are pulled back");
    check(sensitivity_scale(50) == 1.0 && sensitivity_scale(100) == 2.0 && sensitivity_scale(25) == 0.5,
          "sensitivity 50 sends mouse moves as they are, 100 doubles them");
#if defined(RM_TERMINAL_PIPER_SPEECH)
    check(player_arguments(QStringLiteral("/usr/bin/pw-play"), QStringLiteral("a.wav"), 0.35) ==
              QStringList({QStringLiteral("--volume=0.35"), QStringLiteral("a.wav")}),
          "pw-play gets the volume as 0-1");
    check(player_arguments(QStringLiteral("/usr/bin/paplay"), QStringLiteral("a.wav"), 0.5) ==
              QStringList({QStringLiteral("--volume=32768"), QStringLiteral("a.wav")}),
          "paplay gets the volume as 0-65536");
    check(player_arguments(QStringLiteral("/usr/bin/aplay"), QStringLiteral("a.wav"), 0.5) ==
              QStringList({QStringLiteral("a.wav")}),
          "aplay has no volume option");
#endif
}

void only_text_boxes_take_the_input_method() {
    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    auto* line = new QLineEdit;
    auto* spin = new QSpinBox;
    auto* editable = new QComboBox;
    editable->setEditable(true);
    auto* list = new QListWidget;
    list->addItem(QStringLiteral("a"));
    // 自称能接字、其实不是打字框的东西:输入法一看到它就会把字母拿走。
    auto* claims = new QWidget;
    claims->setFocusPolicy(Qt::StrongFocus);
    claims->setAttribute(Qt::WA_InputMethodEnabled);
    auto* plain = new QPushButton(QStringLiteral("b"));
    plain->setFocusPolicy(Qt::StrongFocus);
    for (QWidget* widget : std::vector<QWidget*>{line, spin, editable, list, claims, plain}) layout->addWidget(widget);
    window.show();
    window.activateWindow();
    QCoreApplication::processEvents();

    check(focus_accepts(claims), "without the guard a widget that claims text input takes the input method");
    InputMethodGuard guard;
    QStringList blocked;
    QObject::connect(&guard, &InputMethodGuard::blocked, [&blocked](const QString& what) { blocked << what; });
    qApp->installEventFilter(&guard);
    check(!focus_accepts(claims), "with the guard it does not: Y, H, O, I, P and WASD reach the terminal");
    check(blocked.size() == 1 && blocked[0].startsWith(QStringLiteral("QWidget in QWidget")),
          "the blocked widget is reported once for the log");
    focus_accepts(claims);
    check(blocked.size() == 1, "and only once");
    check(!focus_accepts(list) && !focus_accepts(plain), "lists and buttons never take the input method");
    check(focus_accepts(line) && focus_accepts(spin) && focus_accepts(editable),
          "real text boxes still take Chinese input");
    line->setFocus();
    QCoreApplication::processEvents();
    check(InputMethodGuard::takesText(line), "a visible text box takes text");
    line->hide();
    check(!InputMethodGuard::takesText(line), "a hidden text box never takes the letters");
    line->show();

    Config config;
    config.self_robot_id = 3;
    Dashboard dashboard(config);
    dashboard.resize(1440, 900);
    dashboard.show();
    dashboard.activateWindow();
    QCoreApplication::processEvents();
    QList<QWidget*> widgets = dashboard.findChildren<QWidget*>();
    widgets.prepend(&dashboard);
    int focusable = 0;
    int taking = 0;
    for (QWidget* widget : widgets) {
        if (widget != &dashboard && (widget->focusPolicy() == Qt::NoFocus || !widget->isVisible())) continue;
        ++focusable;
        if (focus_accepts(widget) && !InputMethodGuard::takesText(QGuiApplication::focusObject())) ++taking;
    }
    check(focusable > 3 && taking == 0, "nothing on the main screen hands letters to the input method");
    qApp->removeEventFilter(&guard);
}

void video_can_be_turned_around() {
    VideoPane pane;
    pane.resize(200, 100);
    QImage frame(200, 100, QImage::Format_RGB32);
    frame.fill(Qt::blue);
    for (int y = 0; y < 100; ++y)
        for (int x = 0; x < 100; ++x) frame.setPixelColor(x, y, Qt::red);
    pane.setFrame(frame);
    pane.show();
    QCoreApplication::processEvents();
    check(pane.grab().toImage().pixelColor(20, 50) == QColor(Qt::red), "forward: the left half stays on the left");
    pane.setRotated(true);
    QCoreApplication::processEvents();
    const QImage turned = pane.grab().toImage();
    check(turned.pixelColor(20, 50) == QColor(Qt::blue) && turned.pixelColor(180, 50) == QColor(Qt::red),
          "reverse: the picture is turned 180° for an upside-down camera");
}

SettingsView infantry_prep() {
    SettingsView view;
    view.seat = QStringLiteral("红方 3 号 步兵");
    view.login = QStringLiteral("已登录 · 服务器核对过编号");
    view.stage = 1;
    view.has_performance = true;
    view.performance_open = true;
    view.chassis = 1;
    view.shooter = 1;
    view.performance_note = QStringLiteral("还是默认，开赛就锁定成血量优先 + 冷却优先");
    view.performance_warn = true;
    view.video = QStringLiteral("4 帧/秒");
    return view;
}

void panel_works_like_the_official_p_panel() {
    QWidget host;
    host.resize(1440, 900);
    host.show();
    SettingsPanel panel(&host);
    std::vector<TerminalSettings> changes;
    std::vector<std::pair<std::optional<std::uint32_t>, std::optional<std::uint32_t>>> picks;
    std::vector<bool> displays;
    int resets = 0;
    panel.setActions([&changes](const TerminalSettings& settings) { changes.push_back(settings); },
                     [&picks](std::optional<std::uint32_t> shooter, std::optional<std::uint32_t> chassis) {
                         picks.emplace_back(shooter, chassis);
                     },
                     [&displays](bool full) { displays.push_back(full); }, [&resets] { ++resets; });
    int opened = 0;
    QObject::connect(&panel, &SettingsPanel::openChanged, [&opened](bool) { ++opened; });
    check(!panel.isOpen(), "closed until P");
    panel.toggle();
    QCoreApplication::processEvents();
    const QRect box = panel.geometry();
    check(panel.isOpen() && opened == 1 && host.rect().contains(box) &&
              std::abs(box.center().x() - 720) <= 1 && std::abs(box.center().y() - 450) <= 1,
          "P opens the panel in the middle of the screen");

    panel.setView(infantry_prep());
    check(button(panel, "settingsChassisHp")->isChecked() && button(panel, "settingsShooterCooling")->isChecked(),
          "the choice the server reports is lit");
    button(panel, "settingsChassisPower")->click();
    check(picks.size() == 1 && !picks[0].first && picks[0].second == std::optional<std::uint32_t>(2),
          "power-first sends the chassis choice and keeps the shooter");
    button(panel, "settingsShooterBurst")->click();
    check(picks.back().first == std::optional<std::uint32_t>(2) && !picks.back().second, "burst-first: shooter 2");
    check(button(panel, "settingsReset")->isEnabled(), "reset works in the preparation stage");
    button(panel, "settingsReset")->click();
    check(resets == 1, "reset asks the terminal to restore its own settings");
    QDir().mkpath(QStringLiteral(RM_TERMINAL_ARTIFACT_DIR "/settings"));
    const QString shot = QStringLiteral(RM_TERMINAL_ARTIFACT_DIR "/settings/p-panel-prep.png");
    check(host.grab().save(shot), "review PNG saved");
    std::cout << "  wrote " << shot.toStdString() << '\n';

    SettingsView match = infantry_prep();
    match.stage = 4;
    match.performance_open = false;
    match.performance_note = QStringLiteral("比赛中已锁定");
    panel.setView(match);
    check(!button(panel, "settingsChassisHp")->isEnabled() && !button(panel, "settingsReset")->isEnabled(),
          "in the match the performance choice and the reset are locked, like the official client");
    button(panel, "settingsChassisHp")->click();
    check(picks.size() == 2, "a locked choice sends nothing");

    SettingsView hero = infantry_prep();
    hero.has_performance = false;
    hero.performance_note = QStringLiteral("重装没有性能体系");
    panel.setView(hero);
    check(!button(panel, "settingsChassisPower")->isEnabled() && !button(panel, "settingsChassisHp")->isChecked(),
          "the hero has no performance system: greyed out, nothing lit");

    slider(panel, "settingsSensitivity")->setValue(80);
    check(!changes.empty() && changes.back().sensitivity == 80, "the sensitivity slider changes the setting");
    button(panel, "settingsVideoReverse")->click();
    check(changes.back().video_reversed && button(panel, "settingsVideoReverse")->isChecked(),
          "video direction reverse");
    slider(panel, "settingsVolume")->setValue(40);
    check(changes.back().volume == 40 && changes.back().video_reversed && changes.back().sensitivity == 80,
          "each change keeps the others");
    SettingsView mute = hero;
    mute.volume_adjustable = false;
    panel.setView(mute);
    check(!slider(panel, "settingsVolume")->isEnabled(), "a player that cannot change volume greys the slider out");
    button(panel, "settingsFullscreen")->click();
    check(displays == std::vector<bool>{true}, "全屏 asks for fullscreen");
    SettingsView full = mute;
    full.fullscreen = true;
    panel.setView(full);
    check(button(panel, "settingsFullscreen")->isChecked() && !button(panel, "settingsWindowed")->isChecked(),
          "the display buttons follow the real window state");

    TerminalSettings defaults;
    panel.setSettings(defaults);
    check(slider(panel, "settingsSensitivity")->value() == 50 && button(panel, "settingsVideoForward")->isChecked(),
          "settings put back from outside (after a reset) show up on the panel");
    const std::size_t before = changes.size();
    TerminalSettings by_hand;
    by_hand.sensitivity = 70;
    panel.setSettings(by_hand);
    check(changes.size() == before, "showing settings does not echo them back as a change");
    panel.toggle();
    check(!panel.isOpen() && opened == 2, "P again closes it");
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        settings_are_saved_per_machine();
        only_text_boxes_take_the_input_method();
        video_can_be_turned_around();
        panel_works_like_the_official_p_panel();
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    std::cout << "settings_panel_test OK\n";
    return 0;
}
