#include "reminder_repository.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>

namespace rm_terminal {
namespace {

constexpr auto kFileName = "tactical_reminders.json";

QString validate(const ReminderConfig& config) {
    if (config.version != kReminderConfigVersion)
        return QStringLiteral("unsupported version %1").arg(config.version);
    if (config.reminders.size() > kMaxReminders)
        return QStringLiteral("too many reminders: %1 > %2")
            .arg(config.reminders.size()).arg(kMaxReminders);
    if (config.rate_wpm < kMinReminderRateWpm || config.rate_wpm > kMaxReminderRateWpm)
        return QStringLiteral("rate_wpm %1 out of range [%2, %3]")
            .arg(config.rate_wpm).arg(kMinReminderRateWpm).arg(kMaxReminderRateWpm);
    if (QString::fromStdString(config.voice).size() > 100)
        return QStringLiteral("voice name too long");
    QSet<QString> ids;
    for (const auto& item : config.reminders) {
        const auto id = QString::fromStdString(item.id);
        if (id.isEmpty()) return QStringLiteral("reminder id must not be empty");
        if (ids.contains(id)) return QStringLiteral("duplicate reminder id %1").arg(id);
        ids.insert(id);
        if (item.remaining_sec < 0 || item.remaining_sec > kMaxReminderRemainingSec)
            return QStringLiteral("reminder %1 remaining_sec %2 out of range")
                .arg(id).arg(item.remaining_sec);
        // 按 Unicode 码点数而不是字节数限制:中文播报按字节算会被过早截断。
        const auto text = QString::fromStdString(item.text);
        const auto length = static_cast<std::size_t>(text.size());
        if (length == 0 || length > kMaxReminderTextChars)
            return QStringLiteral("reminder %1 text length %2 out of range").arg(id).arg(length);
    }
    return {};
}

}

ReminderRepository::ReminderRepository(QString directory) : directory_(std::move(directory)) {}

QString ReminderRepository::defaultDirectory() {
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
}

QString ReminderRepository::filePath() const {
    return QDir(directory_).filePath(QString::fromLatin1(kFileName));
}

ReminderLoad ReminderRepository::load() const {
    ReminderLoad out;
    QFile file(filePath());
    if (!file.exists()) {
        out.result = ReminderLoadResult::Missing;
        return out;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        out.result = ReminderLoadResult::Invalid;
        out.error = QStringLiteral("cannot open %1: %2").arg(filePath(), file.errorString());
        return out;
    }
    QJsonParseError parse{};
    const auto document = QJsonDocument::fromJson(file.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) {
        out.result = ReminderLoadResult::Invalid;
        out.error = QStringLiteral("malformed JSON in %1: %2").arg(filePath(), parse.errorString());
        return out;
    }
    const auto root = document.object();
    ReminderConfig config;
    config.version = root.value(QStringLiteral("version")).toInt(-1);
    config.master_enabled = root.value(QStringLiteral("master_enabled")).toBool(false);
    config.voice = root.value(QStringLiteral("voice")).toString().toStdString();
    // 旧配置没有这两个键。缺失时回落到默认语速而不是 0:0 会被 validate 拒掉,
    // 让一份本来合法的旧配置在升级后突然加载失败。
    config.rate_wpm = root.value(QStringLiteral("rate_wpm")).toInt(kDefaultReminderRateWpm);
    const auto array = root.value(QStringLiteral("reminders"));
    if (!array.isArray()) {
        out.result = ReminderLoadResult::Invalid;
        out.error = QStringLiteral("reminders must be an array in %1").arg(filePath());
        return out;
    }
    for (const auto value : array.toArray()) {
        if (!value.isObject()) {
            out.result = ReminderLoadResult::Invalid;
            out.error = QStringLiteral("reminder entry must be an object in %1").arg(filePath());
            return out;
        }
        const auto object = value.toObject();
        const auto seconds = object.value(QStringLiteral("remaining_sec"));
        const auto text = object.value(QStringLiteral("text"));
        const auto id = object.value(QStringLiteral("id"));
        if (!id.isString() || !text.isString() || !seconds.isDouble()) {
            out.result = ReminderLoadResult::Invalid;
            out.error = QStringLiteral("reminder entry has wrong field types in %1").arg(filePath());
            return out;
        }
        ReminderItem item;
        item.id = id.toString().toStdString();
        item.text = text.toString().toStdString();
        item.remaining_sec = seconds.toInt();
        item.enabled = object.value(QStringLiteral("enabled")).toBool(true);
        config.reminders.push_back(std::move(item));
    }
    const auto error = validate(config);
    if (!error.isEmpty()) {
        out.result = ReminderLoadResult::Invalid;
        out.error = QStringLiteral("%1 in %2").arg(error, filePath());
        return out;
    }
    out.result = ReminderLoadResult::Loaded;
    out.config = std::move(config);
    return out;
}

bool ReminderRepository::save(const ReminderConfig& config, QString* error) const {
    const auto invalid = validate(config);
    if (!invalid.isEmpty()) {
        if (error) *error = invalid;
        return false;
    }
    if (!QDir().mkpath(directory_)) {
        if (error) *error = QStringLiteral("cannot create %1").arg(directory_);
        return false;
    }
    QJsonArray array;
    for (const auto& item : config.reminders) {
        QJsonObject object;
        object.insert(QStringLiteral("id"), QString::fromStdString(item.id));
        object.insert(QStringLiteral("remaining_sec"), item.remaining_sec);
        object.insert(QStringLiteral("text"), QString::fromStdString(item.text));
        object.insert(QStringLiteral("enabled"), item.enabled);
        array.append(object);
    }
    QJsonObject root;
    root.insert(QStringLiteral("version"), config.version);
    root.insert(QStringLiteral("master_enabled"), config.master_enabled);
    root.insert(QStringLiteral("voice"), QString::fromStdString(config.voice));
    root.insert(QStringLiteral("rate_wpm"), config.rate_wpm);
    root.insert(QStringLiteral("reminders"), array);

    QSaveFile file(filePath());
    // 关掉非原子回退:回退路径会就地截断旧文件再写,写一半失败时操作手的整套
    // 战术就没了。宁可保存失败并报错,也不要留下一个残缺的配置。
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("cannot open %1: %2").arg(filePath(), file.errorString());
        return false;
    }
    const auto payload = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (file.write(payload) != payload.size()) {
        file.cancelWriting();
        if (error) *error = QStringLiteral("short write to %1").arg(filePath());
        return false;
    }
    if (!file.commit()) {
        if (error) *error = QStringLiteral("cannot commit %1: %2").arg(filePath(), file.errorString());
        return false;
    }
    return true;
}

}
