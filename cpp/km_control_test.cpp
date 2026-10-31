#include "km_control.h"

#include "rm_terminal.pb.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QWidget>

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace rm_terminal;

namespace {

void check(bool condition, const char* name) {
    if (!condition) throw std::runtime_error(name);
    std::cout << "PASS " << name << '\n';
}

void keys_follow_the_protocol_bit_order() {
    const int order[] = {Qt::Key_W, Qt::Key_S, Qt::Key_A, Qt::Key_D, Qt::Key_Shift,
#if defined(Q_OS_MACOS)
                         Qt::Key_Meta,
#else
                         Qt::Key_Control,
#endif
                         Qt::Key_Q, Qt::Key_E, Qt::Key_R, Qt::Key_F, Qt::Key_G, Qt::Key_Z,
                         Qt::Key_X, Qt::Key_C, Qt::Key_V, Qt::Key_B};
    for (int bit = 0; bit < 16; ++bit)
        check(km_key_bit(order[bit]) == (1u << bit), "each key lands on its protocol bit");
    check(km_key_bit(Qt::Key_Escape) == 0 && km_key_bit(Qt::Key_F9) == 0 && km_key_bit(Qt::Key_1) == 0,
          "keys outside the table are never sent to the robot");
    check(km_keys_text(0b10101u) == QStringLiteral("W A Shift"), "pressed keys read in bit order");
}

void frame_round_trips() {
    KmFrame frame;
    frame.mouse_x = -12;
    frame.mouse_y = 7;
    frame.mouse_z = -1;
    frame.left = true;
    frame.middle = true;
    frame.keys = 0x8001;
    rm::KeyboardMouseControl parsed;
    check(parsed.ParseFromString(encode_keyboard_mouse(frame)), "KeyboardMouseControl parses");
    check(parsed.mouse_x() == -12 && parsed.mouse_y() == 7 && parsed.mouse_z() == -1, "keeps motion");
    check(parsed.left_button_down() && !parsed.right_button_down() && parsed.mid_button_down(),
          "keeps buttons");
    check(parsed.keyboard_value() == 0x8001, "keeps the key mask");
    rm::KeyboardMouseControl idle;
    check(idle.ParseFromString(encode_keyboard_mouse(KmFrame{})), "idle frame parses");
    check(idle.has_keyboard_value() && idle.has_left_button_down() && idle.has_mouse_x(),
          "an idle frame still writes every field: absent and released differ on the server");
}

void state_accumulates_motion_between_frames() {
    KmState state;
    state.key(Qt::Key_W, true);
    state.key(Qt::Key_A, true);
    state.key(Qt::Key_W, false);
    state.button(KmButton::Right, true);
    state.move(3.6, -2.2);
    state.move(2.0, 0.5);
    state.wheel(180);
    KmFrame frame = state.take();
    check(frame.keys == km_key_bit(Qt::Key_A), "keys are levels: W released, A held");
    check(frame.right && !frame.left, "buttons are levels");
    check(frame.mouse_x == 5 && frame.mouse_y == 1, "motion sums, truncates, and flips y (down is negative)");
    check(frame.mouse_z == 1, "one full wheel notch forward");
    frame = state.take();
    check(frame.keys == km_key_bit(Qt::Key_A) && frame.right, "levels survive take()");
    check(frame.mouse_x == 0 && frame.mouse_y == 0 && frame.mouse_z == 0, "motion is per frame");
    state.move(0.5, 0.0);
    state.wheel(60);
    frame = state.take();
    check(frame.mouse_x == 1 && frame.mouse_z == 1, "fractions carry over instead of being lost");
    state.releaseAll();
    frame = state.take();
    check(frame.keys == 0 && !frame.right && frame.mouse_x == 0, "releaseAll lets go of everything");
}

void clock_averages_seventy_five_hertz_without_bunching() {
    KmClock clock;
    clock.reset(0);
    check(clock.dueIn(0) == 0, "the first frame goes out at once");
    std::int64_t now = 0;
    int frames = 0;
    while (now < 1'000'000'000) {
        now += clock.dueIn(now);
        if (now >= 1'000'000'000) break;
        clock.sent(now);
        ++frames;
    }
    check(frames == 75, "exactly 75 frames in one second when sent on time");
    clock.reset(0);
    clock.sent(0);
    clock.sent(14'000'000);
    check(clock.dueIn(14'000'000) == 2 * KmClock::kPeriodNs - 14'000'000,
          "a frame a little late keeps the grid, so the average stays 75Hz");
    clock.sent(35'000'000);
    check(clock.dueIn(35'000'000) == KmClock::kMinGapNs,
          "but the next frame is never closer than 10ms: frames do not bunch up");
    clock.sent(500'000'000);
    check(clock.dueIn(500'000'000) == KmClock::kMinGapNs,
          "after a long stall there is no burst of make-up frames");
}

struct Frames {
    std::vector<rm::KeyboardMouseControl> sent;
    bool accept = true;
    KmCapture::Sender sender() {
        return [this](const char* topic, const std::string& payload) {
            if (std::string(topic) != "KeyboardMouseControl") throw std::runtime_error("wrong topic");
            rm::KeyboardMouseControl frame;
            if (!frame.ParseFromString(payload)) throw std::runtime_error("bad payload");
            sent.push_back(frame);
            return accept;
        };
    }
};

void pump(int ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QApplication::processEvents(QEventLoop::AllEvents, 5);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void key(QWidget& window, QEvent::Type type, int code, bool repeat = false) {
    QKeyEvent event(type, code, Qt::NoModifier, QString(), repeat);
    QApplication::sendEvent(&window, &event);
}

void move_to(QWidget& window, QPointF global) {
    QMouseEvent event(QEvent::MouseMove, window.mapFromGlobal(global), global, Qt::NoButton,
                      Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&window, &event);
}

void click(QWidget& window, QEvent::Type type, Qt::MouseButton button) {
    QMouseEvent event(type, QPointF(10, 10), window.mapToGlobal(QPointF(10, 10)), button,
                      type == QEvent::MouseButtonPress ? button : Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&window, &event);
}

void capture_takes_over_the_window() {
    QWidget window;
    window.resize(800, 600);
    window.show();
    window.activateWindow();
    QApplication::processEvents();

    Frames frames;
    KmCapture capture(&window);
    std::vector<QString> stops;
    QObject::connect(&capture, &KmCapture::activeChanged, [&stops](bool active, const QString& reason) {
        if (!active) stops.push_back(reason);
    });
    capture.setSender(frames.sender());
    check(!capture.start(), "no capture while the link is down");
    capture.setLinkUp(true);
    check(capture.start() && capture.active(), "capture starts once the link is up");
    check(frames.sent.size() == 1, "the first frame goes out immediately");

    key(window, QEvent::KeyPress, Qt::Key_W);
    key(window, QEvent::KeyPress, Qt::Key_D);
    key(window, QEvent::KeyRelease, Qt::Key_D, true);
    pump(40);
    check(frames.sent.back().keyboard_value() == (km_key_bit(Qt::Key_W) | km_key_bit(Qt::Key_D)),
          "held keys are sent; an auto-repeat release does not drop D");

    QKeyEvent override_event(QEvent::ShortcutOverride, Qt::Key_R, Qt::NoModifier);
    QApplication::sendEvent(&window, &override_event);
    check(override_event.isAccepted(), "shortcuts are swallowed while capturing");

    const QPointF origin = window.mapToGlobal(QPointF(400, 300));
    move_to(window, origin);
    pump(30);
    const std::size_t before = frames.sent.size();
    move_to(window, origin + QPointF(6, -4));
    move_to(window, origin + QPointF(10, -4));
    click(window, QEvent::MouseButtonPress, Qt::LeftButton);
    QWheelEvent wheel(QPointF(10, 10), window.mapToGlobal(QPointF(10, 10)), QPoint(), QPoint(0, -120),
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&window, &wheel);
    pump(40);
    std::int32_t dx = 0;
    std::int32_t dy = 0;
    std::int32_t dz = 0;
    for (std::size_t i = before; i < frames.sent.size(); ++i) {
        dx += frames.sent[i].mouse_x();
        dy += frames.sent[i].mouse_y();
        dz += frames.sent[i].mouse_z();
    }
    check(dx == 10 && dy == 4, "mouse motion arrives as right +10, up +4 (protocol: down is negative)");
    check(dz == -1, "wheel backwards is negative");
    check(frames.sent.back().left_button_down(), "left button is held");
    click(window, QEvent::MouseButtonRelease, Qt::LeftButton);
    pump(30);
    check(!frames.sent.back().left_button_down(), "left button released");

    const std::size_t at = frames.sent.size();
    pump(400);
    const double rate = static_cast<double>(frames.sent.size() - at) / 0.4;
    check(rate > 60.0 && rate < 80.0, "frames go out at about 75Hz");

    key(window, QEvent::KeyPress, Qt::Key_Escape);
    check(!capture.active(), "Esc leaves keyboard/mouse control");
    const auto& last = frames.sent.back();
    check(last.keyboard_value() == 0 && !last.left_button_down() && last.mouse_x() == 0,
          "the last frame releases everything so the robot stops");
    check(!stops.empty() && stops.back() == QStringLiteral("按 Esc 退出"), "the stop reason is reported");
    const std::size_t after_stop = frames.sent.size();
    pump(60);
    check(frames.sent.size() == after_stop, "nothing is sent after leaving");
    key(window, QEvent::KeyPress, Qt::Key_W);
    pump(20);
    check(frames.sent.size() == after_stop, "keys go back to the window once released");

    check(capture.start(), "capture can start again");
    QEvent deactivate(QEvent::WindowDeactivate);
    QApplication::sendEvent(&window, &deactivate);
    check(!capture.active() && stops.back() == QStringLiteral("窗口失去焦点"),
          "losing focus leaves keyboard/mouse control");

    check(capture.start(), "capture starts for the link test");
    const std::size_t before_drop = frames.sent.size();
    capture.setLinkUp(false);
    check(!capture.active() && stops.back() == QStringLiteral("断线"), "a dropped link leaves at once");
    check(frames.sent.size() == before_drop, "no frame is attempted on a dead link");

    capture.setLinkUp(true);
    check(capture.start(), "capture starts for the death test");
    std::vector<bool> pauses;
    QObject::connect(&capture, &KmCapture::suspendedChanged, [&pauses](bool paused) { pauses.push_back(paused); });
    key(window, QEvent::KeyPress, Qt::Key_W);
    capture.setSuspended(true);
    check(capture.active() && capture.suspended() && pauses == std::vector<bool>{true},
          "dying pauses keyboard/mouse control instead of leaving it");
    key(window, QEvent::KeyPress, Qt::Key_A);
    check(capture.keysDown() == 0, "paused: W was let go and A is not captured (keys go to the terminal, Y revives)");
    move_to(window, window.mapToGlobal(QPointF(100, 100)));
    move_to(window, window.mapToGlobal(QPointF(300, 100)));
    const std::size_t paused_at = frames.sent.size();
    pump(100);
    bool all_released = frames.sent.size() > paused_at;
    for (std::size_t i = paused_at; i < frames.sent.size(); ++i)
        all_released = all_released && frames.sent[i].keyboard_value() == 0 && frames.sent[i].mouse_x() == 0 &&
                       !frames.sent[i].left_button_down();
    check(all_released, "paused: the stream keeps going at 75Hz with everything released");
    window.activateWindow();
    QApplication::processEvents();
    if (window.isActiveWindow()) {
        capture.setSuspended(false);
        check(capture.active() && !capture.suspended() && pauses.back() == false,
              "revived: keyboard/mouse control resumes by itself");
        key(window, QEvent::KeyPress, Qt::Key_D);
        pump(40);
        check(frames.sent.back().keyboard_value() == km_key_bit(Qt::Key_D),
              "after resuming only keys pressed since then count");
        capture.setSuspended(true);
    }
    QEvent deactivate_again(QEvent::WindowDeactivate);
    QApplication::sendEvent(&window, &deactivate_again);
    check(capture.suspended(), "paused: losing focus does not matter while dead");
    QWidget other;
    other.resize(200, 200);
    other.show();
    other.activateWindow();
    QApplication::processEvents();
    check(!window.isActiveWindow(), "another window is in front");
    capture.setSuspended(false);
    check(!capture.active() && stops.back() == QStringLiteral("复活时窗口不在前台"),
          "revived while another window is in front: leave instead of grabbing its mouse");
    other.hide();
    window.activateWindow();
    QApplication::processEvents();

    check(capture.start(), "capture starts for the send-failure test");
    frames.accept = false;
    pump(40);
    check(!capture.active() && stops.back() == QStringLiteral("发送失败"),
          "a failed publish leaves instead of driving blind");
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        keys_follow_the_protocol_bit_order();
        frame_round_trips();
        state_accumulates_motion_between_frames();
        clock_averages_seventy_five_hertz_without_bunching();
        capture_takes_over_the_window();
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    std::cout << "km_control_test OK\n";
    return 0;
}
