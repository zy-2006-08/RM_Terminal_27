#include "reminder_repository.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>

using namespace rm_terminal;

namespace {
void check(bool condition, const char* name) {
    if (!condition) throw std::runtime_error(name);
    std::cout << "PASS " << name << '\n';
}

ReminderConfig sample() {
    ReminderConfig config;
    config.master_enabled = true;
    config.reminders = {{"a", 90, "剩余一分半，收缩防线", true},
                        {"b", 30, "准备最后一波推进", false}};
    return config;
}

void write_raw(const QString& path, const QByteArray& payload) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) throw std::runtime_error("cannot seed file");
    file.write(payload);
}

void roundtrip(const QString& dir) {
    ReminderRepository repository(dir);
    check(repository.load().result == ReminderLoadResult::Missing,
          "missing file loads empty and disabled");
    check(!repository.load().config.master_enabled, "absent config defaults to disabled");

    QString error;
    check(repository.save(sample(), &error), "valid config saves");
    const auto loaded = repository.load();
    check(loaded.result == ReminderLoadResult::Loaded, "saved config reloads");
    check(loaded.config.reminders.size() == 2, "all items survive the roundtrip");
    check(loaded.config.reminders[0].text == "剩余一分半，收缩防线",
          "chinese text survives byte-for-byte");
    check(loaded.config.reminders[0].remaining_sec == 90 && loaded.config.reminders[0].enabled,
          "seconds and enabled flag survive");
    check(!loaded.config.reminders[1].enabled, "per-item disabled state persists");
    check(loaded.config.master_enabled, "master switch state persists");

    auto off = sample();
    off.master_enabled = false;
    check(repository.save(off, &error), "disabled master saves");
    check(repository.load().result == ReminderLoadResult::Loaded &&
          !repository.load().config.master_enabled, "closing the master switch is durable");
}

void rejects(const QString& dir) {
    ReminderRepository repository(dir);
    QString error;

    auto duplicate = sample();
    duplicate.reminders[1].id = "a";
    check(!repository.save(duplicate, &error) && error.contains(QStringLiteral("duplicate")),
          "duplicate ids rejected");

    auto empty_id = sample();
    empty_id.reminders[0].id.clear();
    check(!repository.save(empty_id, &error), "empty id rejected");

    auto negative = sample();
    negative.reminders[0].remaining_sec = -1;
    check(!repository.save(negative, &error), "negative seconds rejected");

    auto too_late = sample();
    too_late.reminders[0].remaining_sec = kMaxReminderRemainingSec + 1;
    check(!repository.save(too_late, &error), "seconds above range rejected");

    auto empty_text = sample();
    empty_text.reminders[0].text.clear();
    check(!repository.save(empty_text, &error), "empty text rejected");

    auto long_text = sample();
    long_text.reminders[0].text = std::string(kMaxReminderTextChars + 1, 'x');
    check(!repository.save(long_text, &error), "text above limit rejected");

    auto wide_text = sample();
    wide_text.reminders[0].text =
        QString(static_cast<int>(kMaxReminderTextChars), QChar(0x6218)).toStdString();
    check(repository.save(wide_text, &error),
          "200 chinese characters accepted, so the limit counts code points not bytes");

    auto bad_version = sample();
    bad_version.version = 2;
    check(!repository.save(bad_version, &error) && error.contains(QStringLiteral("version")),
          "unknown version rejected");

    ReminderConfig crowd;
    crowd.master_enabled = true;
    for (std::size_t i = 0; i <= kMaxReminders; ++i)
        crowd.reminders.push_back({std::to_string(i), 10, "x", true});
    check(!repository.save(crowd, &error), "more than 100 reminders rejected");
}

void corruption(const QString& dir) {
    ReminderRepository repository(dir);
    QString error;
    check(repository.save(sample(), &error), "seed a good config");
    const auto good = QFile(repository.filePath()).size();

    write_raw(repository.filePath(), QByteArray("{not json"));
    auto loaded = repository.load();
    check(loaded.result == ReminderLoadResult::Invalid && !loaded.error.isEmpty(),
          "malformed JSON reports invalid, never silently empty");
    check(QFile(repository.filePath()).size() > 0, "corrupt file is preserved for inspection");

    write_raw(repository.filePath(), QByteArray(R"({"version":1,"reminders":{}})"));
    check(repository.load().result == ReminderLoadResult::Invalid, "non-array reminders rejected");

    write_raw(repository.filePath(),
              QByteArray(R"({"version":1,"reminders":[{"id":"a","remaining_sec":"90","text":"x"}]})"));
    check(repository.load().result == ReminderLoadResult::Invalid, "wrong field types rejected");

    write_raw(repository.filePath(),
              QByteArray(R"({"version":9,"reminders":[]})"));
    check(repository.load().result == ReminderLoadResult::Invalid, "unknown version on load rejected");

    check(repository.save(sample(), &error), "recovery overwrite is explicit");
    check(QFile(repository.filePath()).size() == good, "recovered file matches the good size");

    auto invalid = sample();
    invalid.reminders[0].remaining_sec = -5;
    check(!repository.save(invalid, &error), "rejected save does not touch disk");
    check(repository.load().result == ReminderLoadResult::Loaded &&
          repository.load().config.reminders.size() == 2,
          "old file stays intact after a failed save");
}

void unwritable(const QString& dir) {
    // 目录不可写时保存必须失败并报错,而不是假装成功:界面据此保留草稿。
    const auto locked = QDir(dir).filePath(QStringLiteral("locked"));
    if (!QDir().mkpath(locked)) throw std::runtime_error("cannot create locked dir");
    ReminderRepository repository(locked);
    QString error;
    check(repository.save(sample(), &error), "baseline save into the directory works");
    QFile::setPermissions(locked, QFileDevice::ReadOwner);
    const auto blocked = !repository.save(sample(), &error) && !error.isEmpty();
    QFile::setPermissions(locked, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                     QFileDevice::ExeOwner);
    check(blocked, "unwritable directory surfaces a save failure");
    check(repository.load().result == ReminderLoadResult::Loaded,
          "previous config survives the failed save");
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir dir;
        if (!dir.isValid()) throw std::runtime_error("cannot create temp dir");
        roundtrip(QDir(dir.path()).filePath(QStringLiteral("roundtrip")));
        rejects(QDir(dir.path()).filePath(QStringLiteral("rejects")));
        corruption(QDir(dir.path()).filePath(QStringLiteral("corruption")));
        unwritable(QDir(dir.path()).filePath(QStringLiteral("perm")));
        check(!ReminderRepository::defaultDirectory().isEmpty(),
              "default directory resolves to a stable app location");
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    std::cout << "reminder_repository: all checks passed\n";
    return 0;
}
