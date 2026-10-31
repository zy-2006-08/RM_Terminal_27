#include "role_commands.h"

#include "rm_terminal.pb.h"

#include <iostream>
#include <stdexcept>
#include <string>

using namespace rm_terminal;

namespace {

void check(bool condition, const char* name) {
    if (!condition) throw std::runtime_error(name);
    std::cout << "PASS " << name << '\n';
}

// 编码必须逐字段往返。协议字段号写错时报文仍然能序列化,只有解回来逐字段比对
// 才会暴露 —— 联调现场的表现是「服务器收到了但什么都没发生」。
void assembly_round_trips_every_operation() {
    for (std::uint32_t operation = 0; operation <= 2; ++operation) {
        for (std::uint32_t difficulty = 1; difficulty <= 4; ++difficulty) {
            rm::AssemblyCommand parsed;
            check(parsed.ParseFromString(encode_assembly(operation, difficulty)),
                  "AssemblyCommand parses");
            check(parsed.has_operation() && parsed.operation() == operation,
                  "AssemblyCommand keeps operation");
            check(parsed.has_difficulty() && parsed.difficulty() == difficulty,
                  "AssemblyCommand keeps difficulty");
        }
    }
    // operation 0 带 difficulty 0 也要显式带上字段:少写字段和写 0 在服务器侧不同。
    rm::AssemblyCommand zero;
    check(zero.ParseFromString(encode_assembly(0, 0)), "AssemblyCommand parses with zeroes");
    check(zero.has_difficulty() && zero.difficulty() == 0,
          "AssemblyCommand writes difficulty explicitly even when zero");
}

void dart_round_trips_every_target() {
    for (std::uint32_t target = 1; target <= 5; ++target) {
        for (int flags = 0; flags < 4; ++flags) {
            const bool open = (flags & 1) != 0;
            const bool launch = (flags & 2) != 0;
            rm::DartCommand parsed;
            check(parsed.ParseFromString(encode_dart(target, open, launch)),
                  "DartCommand parses");
            check(parsed.has_target_id() && parsed.target_id() == target,
                  "DartCommand keeps target_id");
            check(parsed.has_open() && parsed.open() == open, "DartCommand keeps open");
            check(parsed.has_launch_confirm() && parsed.launch_confirm() == launch,
                  "DartCommand keeps launch_confirm");
        }
    }
}

void sentry_round_trips_every_command_id() {
    for (std::uint32_t id : {1u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u}) {
        rm::SentryCtrlCommand parsed;
        check(parsed.ParseFromString(encode_sentry(id)), "SentryCtrlCommand parses");
        check(parsed.has_command_id() && parsed.command_id() == id,
              "SentryCtrlCommand keeps command_id");
    }
}

void common_round_trips_every_type() {
    for (std::uint32_t type = 1; type <= 6; ++type) {
        for (std::uint32_t param : {0u, 1u, 10u, 100u, 1000u}) {
            rm::CommonCommand parsed;
            check(parsed.ParseFromString(encode_common(type, param)), "CommonCommand parses");
            check(parsed.has_cmd_type() && parsed.cmd_type() == type, "CommonCommand keeps cmd_type");
            check(parsed.has_param() && parsed.param() == param,
                  "CommonCommand writes param explicitly, zero included");
        }
    }
}

void performance_writes_shooter_and_chassis_only() {
    for (std::uint32_t shooter = 1; shooter <= 2; ++shooter) {
        for (std::uint32_t chassis = 1; chassis <= 2; ++chassis) {
            rm::RobotPerformanceSelectionCommand parsed;
            check(parsed.ParseFromString(encode_performance(shooter, chassis)),
                  "RobotPerformanceSelectionCommand parses");
            check(parsed.has_shooter() && parsed.shooter() == shooter, "keeps shooter");
            check(parsed.has_chassis() && parsed.chassis() == chassis, "keeps chassis");
            // 写 0 就是替哨兵选了「自动控制」,步兵终端不能碰这个字段。
            check(!parsed.has_sentry_control(), "leaves sentry_control absent");
        }
    }
}

void rune_activate_is_one() {
    rm::RuneActivateCommand parsed;
    check(parsed.ParseFromString(encode_rune_activate()), "RuneActivateCommand parses");
    check(parsed.has_activate() && parsed.activate() == 1, "RuneActivateCommand activate=1");
}

// 协议 2.1:这些指令最高 10Hz。
void gate_allows_at_most_one_per_hundred_ms() {
    CommandGate gate;
    check(gate.interval() == 100, "default gate is 10Hz");
    check(gate.allow(1000), "first send passes");
    check(!gate.allow(1000), "same instant is blocked");
    check(!gate.allow(1099), "99ms later is still blocked");
    check(gate.allow(1100), "exactly 100ms later passes");
    check(!gate.allow(1199), "the window restarts from the last send");
    check(gate.allow(1200), "next window passes");

    // 被拦下的那次不能改写上次发送的时刻,否则连续点击会把窗口无限往后推,
    // 表现为操作手快速点按时按钮彻底发不出去。
    CommandGate steady;
    check(steady.allow(0), "steady gate first send");
    for (MonotonicMs t = 10; t < 100; t += 10) check(!steady.allow(t), "blocked inside window");
    check(steady.allow(100), "blocked attempts do not extend the window");
}

void hold_fires_only_after_the_full_second() {
    HoldGate hold;
    check(hold.holdMs() == 1000, "default hold is one second");
    check(!hold.pressed(), "starts released");

    hold.press(5000);
    check(hold.pressed(), "press is recorded");
    check(!hold.ready(5999), "not ready one millisecond early");
    check(!hold.release(5999), "early release does not fire");
    check(!hold.pressed(), "early release clears the press");
    // 早松之后的残留状态最危险:如果 pressed_at_ 留着,下一次松手会立刻触发。
    check(!hold.release(9000), "a release without a press never fires");

    hold.press(10000);
    check(hold.ready(11000), "ready at exactly one second");
    check(hold.release(11000), "release at exactly one second fires");
    check(!hold.pressed(), "firing clears the press");

    hold.press(20000);
    hold.cancel();
    check(!hold.release(30000), "cancel stops a held button from firing");
}

void hold_progress_is_monotonic_and_clamped() {
    HoldGate hold;
    check(hold.progress(0) == 0.0, "no progress before a press");
    hold.press(1000);
    check(hold.progress(1000) == 0.0, "progress starts at zero");
    check(hold.progress(1500) == 0.5, "halfway is one half");
    check(hold.progress(1999) < 1.0, "not full before the deadline");
    check(hold.progress(2000) == 1.0, "full at the deadline");
    check(hold.progress(9000) == 1.0, "progress is clamped at one");
}

void echo_watch_transitions() {
    // 隐式回显(飞镖/装配):状态指纹变了算确认。
    EchoWatch implicit;
    check(implicit.result().status == EchoStatus::Idle, "starts idle");
    implicit.sent(1000, 3, 42);
    check(implicit.result().status == EchoStatus::Pending, "sending enters pending");
    check(implicit.update(1500, 42).status == EchoStatus::Pending, "unchanged state stays pending");
    check(implicit.update(1500, 43).status == EchoStatus::Confirmed, "changed state confirms");
    check(implicit.update(9000, 43).status == EchoStatus::Confirmed, "confirmation is sticky");

    EchoWatch timeout;
    timeout.sent(1000, 1, 7);
    check(timeout.update(2999, 7).status == EchoStatus::Pending, "pending just before the timeout");
    check(timeout.update(3000, 7).status == EchoStatus::Unconfirmed,
          "no change within 2000ms reads as unconfirmed");
    check(echo_status_text(timeout.result()) == QStringLiteral("未确认"),
          "unconfirmed renders as the Chinese label");

    // 显式回显(哨兵):SentryCtrlResult 带 command_id 和 result_code。
    EchoWatch explicit_result;
    explicit_result.sent(1000, 7, 0);
    check(explicit_result.applyResult(1200, 8, 0).status == EchoStatus::Pending,
          "a result for another command id is ignored");
    check(explicit_result.applyResult(1200, 7, 0).status == EchoStatus::Confirmed,
          "a matching success confirms");

    EchoWatch failure;
    failure.sent(1000, 6, 0);
    const EchoResult failed = failure.applyResult(1100, 6, 4);
    check(failed.status == EchoStatus::Failed && failed.code == 4, "a non-zero code fails");
    check(echo_status_text(failed) == QStringLiteral("失败（码 4）"),
          "failure renders its code");

    // 超时后迟到的结果照样要收:操作手需要知道那条指令最终成了还是败了。
    EchoWatch late;
    late.sent(1000, 5, 0);
    check(late.update(4000, 0).status == EchoStatus::Unconfirmed, "times out first");
    check(late.applyResult(4500, 5, 0).status == EchoStatus::Confirmed,
          "a late success still confirms");

    late.clear();
    check(late.result().status == EchoStatus::Idle, "clear returns to idle");
    check(echo_status_text(late.result()).isEmpty(), "idle has no label");
}

}  // namespace

int main() {
    try {
        assembly_round_trips_every_operation();
        dart_round_trips_every_target();
        sentry_round_trips_every_command_id();
        common_round_trips_every_type();
        performance_writes_shooter_and_chassis_only();
        rune_activate_is_one();
        gate_allows_at_most_one_per_hundred_ms();
        hold_fires_only_after_the_full_second();
        hold_progress_is_monotonic_and_clamped();
        echo_watch_transitions();
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    std::cout << "role_commands_test OK\n";
    return 0;
}
