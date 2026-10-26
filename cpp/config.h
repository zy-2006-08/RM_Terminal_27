#pragma once

#include "logging.h"
#include <QString>
#include <QStringList>

namespace rm_terminal {

// Process exit-code contract, stable across tasks 1-6:
//   0 success
//   2 malformed command-line arguments (asserted by cpp/assert_bad_args.cmake)
//   3 transport start failure (MQTT intake)
//   4 configuration or log-destination failure
//   5 evidence capture failure (--screenshot could not write the PNG, or
//     --dump-layout could not write the JSON)
constexpr int kExitSuccess = 0;
constexpr int kExitBadArguments = 2;
constexpr int kExitStartFailure = 3;
constexpr int kExitConfigError = 4;
constexpr int kExitEvidenceFailure = 5;

// Default configuration file, consulted when neither --config nor
// RM_TERMINAL_CONFIG is supplied.
constexpr const char* kDefaultConfigFileName = "rm_terminal.conf";

// Immutable runtime configuration. Defaults mirror the 2026 local-simulation
// baseline in core/constants.py so the native terminal and the Python
// simulator agree with no config file present.
struct Config {
    QString mqtt_host = QStringLiteral("127.0.0.1");
    QString log_destination = QStringLiteral("rm_terminal.log");
    int mqtt_port = 3333;
    int udp_port = 3334;
    // 2027 阵容编号：红 1/3/4/6/7，蓝方 +100。决定解码器把哪台车当成自机。
    int self_robot_id = 3;
    // 必须容得下最慢字段的两个发布周期。RobotPosition/RobotModuleStatus 是 1Hz,
    // 500ms 的窗口比发布间隔本身还短 —— 位置刚到就被判过期,地图上每台车都常驻
    // 「过期」,于是这个标记再也无法指示真正的断流。
    int stale_window_ms = 2200;
    int mode_exit_hysteresis_ms = 3000;
    int event_history_capacity = 50;
    // 0 disables the stale-blind fallback entirely; see load_config in config.cpp.
    int blind_stale_fallback_ms = 15000;
    bool map_enabled = true;
    LogLevel log_level = LogLevel::info;
    // 自定义控制器(小机械臂)的串口。空表示本席位没接控制器,状态灯显示「未启用」。
    // 真机用 /dev/serial/by-id/... 这种不会变的名字:USB 重插后 ttyUSB0 可能变成 ttyUSB1。
    QString controller_serial;
    int controller_baud = 115200;
    // 网线被拔时对端不会正常挂断,只能靠保活超时发现,大约要两个保活周期。原先写死
    // 30 秒,意味着断线后近一分钟才开始重连。
    int mqtt_keepalive_s = 5;
    // 关掉的事件提醒种类,取值见 alert_kind_names()。默认全开。
    QStringList alerts_disabled;
    // 控制度落后多少分提醒一次。
    int alert_control_gap = 30;
    // Linux 离线语音的安装目录(scripts/setup_voice.sh 装到哪里)。空 = 默认位置
    // ~/.local/share/rm_terminal/tts。
    QString speech_dir;
};

// 事件提醒种类在配置里的名字。与 match_alerts.cpp 的 alert_kind_name 一一对应,
// match_alerts_test 断言两边一致。
const QStringList& alert_kind_names();

// Loads configuration from `path`.
//
// A MISSING file is not an error: `out` receives the documented defaults and
// the call returns true, so a fresh checkout runs without a config file. A
// file that EXISTS but cannot be parsed is always a loud failure: the call
// returns false and `error` receives a message naming the offending key, its
// value, and the line number.
bool load_config(const QString& path, Config* out, QString* error);

// 2027 阵容编号：红 1/3/4/6/7，蓝方 +100。
bool valid_self_robot_id(int id);

bool parse_log_level(const QString& name, LogLevel* out);

// 9600 到 921600 之间的标准波特率。控制器固件用 115200。
bool valid_controller_baud(int baud);

}
