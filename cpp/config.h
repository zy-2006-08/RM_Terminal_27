#pragma once

#include "logging.h"
#include <QString>

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

// Read-only runtime configuration. Defaults mirror the 2026 local-simulation
// baseline in core/constants.py so the native terminal and the Python
// simulator agree with no config file present.
struct Config {
    QString mqtt_host = QStringLiteral("127.0.0.1");
    QString log_destination = QStringLiteral("rm_terminal.log");
    int mqtt_port = 3333;
    int udp_port = 3334;
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
};

// Loads configuration from `path`.
//
// A MISSING file is not an error: `out` receives the documented defaults and
// the call returns true, so a fresh checkout runs without a config file. A
// file that EXISTS but cannot be parsed is always a loud failure: the call
// returns false and `error` receives a message naming the offending key, its
// value, and the line number.
bool load_config(const QString& path, Config* out, QString* error);

bool parse_log_level(const QString& name, LogLevel* out);

}
