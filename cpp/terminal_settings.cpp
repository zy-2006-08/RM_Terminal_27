#include "terminal_settings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>

namespace rm_terminal {

namespace {
constexpr int kVersion = 1;
}

bool operator==(const TerminalSettings& left, const TerminalSettings& right) {
    return left.sensitivity == right.sensitivity && left.volume == right.volume &&
           left.video_reversed == right.video_reversed;
}

bool operator!=(const TerminalSettings& left, const TerminalSettings& right) { return !(left == right); }

double sensitivity_scale(int sensitivity) { return std::clamp(sensitivity, 1, 100) / 50.0; }

TerminalSettings normalized(TerminalSettings settings) {
    settings.sensitivity = std::clamp(settings.sensitivity, 1, 100);
    settings.volume = std::clamp(settings.volume, 0, 100);
    return settings;
}

QString terminal_settings_path() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation))
        .filePath(QStringLiteral("terminal_settings.json"));
}

TerminalSettings load_terminal_settings(const QString& path, QString* error) {
    TerminalSettings settings;
    QFile file(path);
    if (!file.exists()) return settings;
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("读不了 %1").arg(path);
        return settings;
    }
    QJsonParseError parse;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) *error = QStringLiteral("%1 读不懂，用默认设置").arg(path);
        return settings;
    }
    const QJsonObject root = document.object();
    settings.sensitivity = root.value(QStringLiteral("sensitivity")).toInt(settings.sensitivity);
    settings.volume = root.value(QStringLiteral("volume")).toInt(settings.volume);
    settings.video_reversed = root.value(QStringLiteral("video_reversed")).toBool(settings.video_reversed);
    return normalized(settings);
}

bool save_terminal_settings(const QString& path, const TerminalSettings& settings, QString* error) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    const TerminalSettings clean = normalized(settings);
    QJsonObject root;
    root.insert(QStringLiteral("version"), kVersion);
    root.insert(QStringLiteral("sensitivity"), clean.sensitivity);
    root.insert(QStringLiteral("volume"), clean.volume);
    root.insert(QStringLiteral("video_reversed"), clean.video_reversed);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(QJsonDocument(root).toJson(QJsonDocument::Indented)) < 0 || !file.commit()) {
        if (error) *error = QStringLiteral("存不了 %1").arg(path);
        return false;
    }
    return true;
}

}  // namespace rm_terminal
