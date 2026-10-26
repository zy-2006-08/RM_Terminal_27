#include "config.h"

#include <QFile>

namespace rm_terminal {
namespace {
bool parse_integer(const QString& value, const QString& key, int line, int* result, QString* error) {
    bool ok = false;
    const int parsed = value.toInt(&ok);
    if (!ok || parsed <= 0) {
        if (error) {
            *error = QStringLiteral("line %1: invalid %2=%3; expected positive integer")
                         .arg(line).arg(key, value);
        }
        return false;
    }
    *result = parsed;
    return true;
}

// Deliberate exception to parse_integer's `> 0` rule: blind_stale_fallback_ms
// accepts 0, which DISABLES the stale-blind fallback so the UI never leaves
// info mode merely because blind telemetry went stale. An operator who does not
// trust the fallback needs a way to switch it off; rejecting 0 would force them
// to fake a huge timeout instead. Negative values remain invalid.
bool parse_non_negative_integer(const QString& value, const QString& key, int line, int* result,
                                QString* error) {
    bool ok = false;
    const int parsed = value.toInt(&ok);
    if (!ok || parsed < 0) {
        if (error) {
            *error = QStringLiteral("line %1: invalid %2=%3; expected non-negative integer "
                                    "(0 disables the fallback)")
                         .arg(line).arg(key, value);
        }
        return false;
    }
    *result = parsed;
    return true;
}

bool parse_boolean(const QString& value, const QString& key, int line, bool* result,
                   QString* error) {
    if (value == QLatin1String("true")) {
        *result = true;
        return true;
    }
    if (value == QLatin1String("false")) {
        *result = false;
        return true;
    }
    if (error) {
        *error = QStringLiteral("line %1: invalid %2=%3; expected true or false")
                     .arg(line).arg(key, value);
    }
    return false;
}
}

bool parse_log_level(const QString& name, LogLevel* out) {
    if (name == QLatin1String("debug")) *out = LogLevel::debug;
    else if (name == QLatin1String("info")) *out = LogLevel::info;
    else if (name == QLatin1String("warning")) *out = LogLevel::warning;
    else if (name == QLatin1String("error")) *out = LogLevel::error;
    else return false;
    return true;
}

bool valid_self_robot_id(int id) {
    return id == 1 || id == 3 || id == 4 || id == 6 || id == 7
           || id == 101 || id == 103 || id == 104 || id == 106 || id == 107;
}

bool valid_controller_baud(int baud) {
    for (const int known : {9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600})
        if (baud == known) return true;
    return false;
}

bool load_config(const QString& path, Config* config, QString* error) {
    *config = Config{};
    QFile file(path);
    if (!file.exists()) return true;
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) *error = file.errorString();
        return false;
    }
    int line = 0;
    while (!file.atEnd()) {
        ++line;
        const QString text = QString::fromUtf8(file.readLine()).trimmed();
        if (text.isEmpty() || text.startsWith('#')) continue;
        const int separator = text.indexOf('=');
        if (separator <= 0) {
            if (error) *error = QString("line %1: expected key=value").arg(line);
            return false;
        }
        const QString key = text.left(separator).trimmed();
        const QString value = text.mid(separator + 1).trimmed();
        if (value.isEmpty()) {
            if (error) {
                *error = QStringLiteral("line %1: %2 has an empty value").arg(line).arg(key);
            }
            return false;
        }
        if (key == QLatin1String("mqtt_host")) {
            config->mqtt_host = value;
        } else if (key == QLatin1String("log_destination")) {
            config->log_destination = value;
        } else if (key == QLatin1String("mqtt_port")) {
            if (!parse_integer(value, key, line, &config->mqtt_port, error)) return false;
        } else if (key == QLatin1String("udp_port")) {
            if (!parse_integer(value, key, line, &config->udp_port, error)) return false;
        } else if (key == QLatin1String("self_robot_id")) {
            if (!parse_integer(value, key, line, &config->self_robot_id, error)) return false;
            if (!valid_self_robot_id(config->self_robot_id)) {
                if (error) {
                    *error = QStringLiteral("line %1: invalid self_robot_id=%2; expected "
                                            "1, 3, 4, 6, 7, 101, 103, 104, 106, or 107")
                                 .arg(line).arg(config->self_robot_id);
                }
                return false;
            }
        } else if (key == QLatin1String("stale_window_ms")) {
            if (!parse_integer(value, key, line, &config->stale_window_ms, error)) return false;
        } else if (key == QLatin1String("mode_exit_hysteresis_ms")) {
            if (!parse_integer(value, key, line, &config->mode_exit_hysteresis_ms, error)) {
                return false;
            }
        } else if (key == QLatin1String("event_history_capacity")) {
            if (!parse_integer(value, key, line, &config->event_history_capacity, error)) {
                return false;
            }
        } else if (key == QLatin1String("blind_stale_fallback_ms")) {
            if (!parse_non_negative_integer(value, key, line, &config->blind_stale_fallback_ms,
                                            error)) {
                return false;
            }
        } else if (key == QLatin1String("map_enabled")) {
            if (!parse_boolean(value, key, line, &config->map_enabled, error)) return false;
        } else if (key == QLatin1String("controller_serial")) {
            config->controller_serial = value;
        } else if (key == QLatin1String("controller_baud")) {
            if (!parse_integer(value, key, line, &config->controller_baud, error)) return false;
            if (!valid_controller_baud(config->controller_baud)) {
                if (error) {
                    *error = QStringLiteral("line %1: invalid controller_baud=%2; expected 9600, "
                                            "19200, 38400, 57600, 115200, 230400, 460800, or "
                                            "921600").arg(line).arg(value);
                }
                return false;
            }
        } else if (key == QLatin1String("mqtt_keepalive_s")) {
            if (!parse_integer(value, key, line, &config->mqtt_keepalive_s, error)) return false;
        } else if (key == QLatin1String("log_level")) {
            if (!parse_log_level(value, &config->log_level)) {
                if (error) {
                    *error = QStringLiteral("line %1: invalid log_level=%2; expected debug, info, "
                                            "warning, or error").arg(line).arg(value);
                }
                return false;
            }
        } else {
            if (error) *error = QStringLiteral("line %1: unknown key %2").arg(line).arg(key);
            return false;
        }
    }
    return true;
}
}
