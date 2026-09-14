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
}

bool parse_log_level(const QString& name, LogLevel* out) {
    if (name == QLatin1String("debug")) *out = LogLevel::debug;
    else if (name == QLatin1String("info")) *out = LogLevel::info;
    else if (name == QLatin1String("warning")) *out = LogLevel::warning;
    else if (name == QLatin1String("error")) *out = LogLevel::error;
    else return false;
    return true;
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
        } else if (key == QLatin1String("stale_window_ms")) {
            if (!parse_integer(value, key, line, &config->stale_window_ms, error)) return false;
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
