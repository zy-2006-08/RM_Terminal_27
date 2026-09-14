#pragma once

#include "logging.h"
#include <QString>

namespace rm_terminal {

// Process exit-code contract, stable across tasks 1-6:
//   0 success
//   2 malformed command-line arguments (asserted by cpp/assert_bad_args.cmake)
//   3 transport start failure (MQTT intake)
//   4 configuration or log-destination failure
//   5 evidence capture failure (--screenshot could not write the PNG)
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
    int stale_window_ms = 500;
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
