#include "config.h"
#include "logging.h"

#include <QCoreApplication>
#include <QFile>
#include <QIODevice>
#include <QStringList>
#include <QTemporaryDir>
#include <QTextStream>

#include <cstdio>

namespace {

int failures = 0;

void check(bool condition, const char* label) {
    if (condition) return;
    fprintf(stderr, "FAIL %s\n", label);
    ++failures;
}

bool write_file(const QString& path, const QString& contents) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return false;
    QTextStream out(&file);
    out << contents;
    out.flush();
    file.close();
    return true;
}

QStringList read_lines(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    const QString text = QString::fromUtf8(file.readAll());
    file.close();
    QStringList lines;
    for (const QString& line : text.split(QLatin1Char('\n'))) {
        if (!line.trimmed().isEmpty()) lines.append(line);
    }
    return lines;
}

void missing_file_uses_documented_defaults(const QTemporaryDir& dir) {
    // Given: a config path that does not exist.
    const QString path = dir.filePath(QStringLiteral("absent.conf"));
    // When: the config is loaded.
    rm_terminal::Config cfg;
    QString error;
    const bool ok = rm_terminal::load_config(path, &cfg, &error);
    // Then: loading succeeds with the documented simulation defaults.
    check(ok, "missing file returns true");
    check(error.isEmpty(), "missing file sets no error");
    check(cfg.mqtt_host == QLatin1String("127.0.0.1"), "default mqtt_host");
    check(cfg.mqtt_port == 3333, "default mqtt_port");
    check(cfg.udp_port == 3334, "default udp_port");
    // 2200 rather than 500: RobotPosition publishes at 1Hz, so any window at or
    // below one publish interval marks every position stale on arrival.
    check(cfg.stale_window_ms == 2200, "default stale_window_ms");
    check(cfg.log_level == rm_terminal::LogLevel::info, "default log_level");
    check(cfg.mode_exit_hysteresis_ms == 3000, "default mode_exit_hysteresis_ms");
    check(cfg.event_history_capacity == 50, "default event_history_capacity");
    check(cfg.map_enabled, "default map_enabled");
    check(cfg.blind_stale_fallback_ms == 15000, "default blind_stale_fallback_ms");
}

void every_key_is_honoured(const QTemporaryDir& dir) {
    // Given: a config file overriding every supported key.
    const QString path = dir.filePath(QStringLiteral("full.conf"));
    check(write_file(path, QStringLiteral("# comment line\n"
                                         "\n"
                                         "mqtt_host = 10.0.0.5\n"
                                         "mqtt_port=4444\n"
                                         "udp_port=5555\n"
                                         "stale_window_ms=700\n"
                                         "log_level=debug\n"
                                         "log_destination=custom.log\n"
                                         "mode_exit_hysteresis_ms=4500\n"
                                         "event_history_capacity=120\n"
                                         "map_enabled=false\n"
                                         "blind_stale_fallback_ms=9000\n")),
          "write full config");
    // When: the config is loaded.
    rm_terminal::Config cfg;
    QString error;
    const bool ok = rm_terminal::load_config(path, &cfg, &error);
    // Then: each value replaces its default and comments are ignored.
    check(ok, "full config loads");
    check(cfg.mqtt_host == QLatin1String("10.0.0.5"), "override mqtt_host");
    check(cfg.mqtt_port == 4444, "override mqtt_port");
    check(cfg.udp_port == 5555, "override udp_port");
    check(cfg.stale_window_ms == 700, "override stale_window_ms");
    check(cfg.log_level == rm_terminal::LogLevel::debug, "override log_level");
    check(cfg.log_destination == QLatin1String("custom.log"), "override log_destination");
    check(cfg.mode_exit_hysteresis_ms == 4500, "override mode_exit_hysteresis_ms");
    check(cfg.event_history_capacity == 120, "override event_history_capacity");
    check(!cfg.map_enabled, "override map_enabled");
    check(cfg.blind_stale_fallback_ms == 9000, "override blind_stale_fallback_ms");
}

// blind_stale_fallback_ms is the ONE key that accepts 0. Zero means "never fall
// back to Video on stale blind data", which is a legitimate operator choice, so
// the validator must use >= 0 here while every other integer key demands > 0.
void blind_stale_fallback_accepts_zero_as_disabled(const QTemporaryDir& dir) {
    // Given: a config disabling the stale fallback with an explicit zero.
    const QString path = dir.filePath(QStringLiteral("fallback_zero.conf"));
    check(write_file(path, QStringLiteral("blind_stale_fallback_ms=0\n")), "write zero fallback");
    // When: the config is loaded.
    rm_terminal::Config cfg;
    QString error;
    const bool ok = rm_terminal::load_config(path, &cfg, &error);
    // Then: zero is accepted verbatim and means the fallback is disabled.
    check(ok, "blind_stale_fallback_ms=0 is legal");
    check(error.isEmpty(), "blind_stale_fallback_ms=0 sets no error");
    check(cfg.blind_stale_fallback_ms == 0, "blind_stale_fallback_ms=0 is stored as 0");
}

void map_enabled_accepts_both_booleans(const QTemporaryDir& dir) {
    struct Case {
        const char* name;
        const char* body;
        bool expected;
    };
    // Given: the two accepted spellings of the boolean key.
    const Case cases[] = {
        {"map_true", "map_enabled=true\n", true},
        {"map_false", "map_enabled=false\n", false},
    };
    for (const Case& item : cases) {
        const QString path = dir.filePath(QString::fromLatin1(item.name) + QStringLiteral(".conf"));
        check(write_file(path, QString::fromLatin1(item.body)), item.name);
        // When: the config is loaded.
        rm_terminal::Config cfg;
        QString error;
        const bool ok = rm_terminal::load_config(path, &cfg, &error);
        // Then: the flag reflects the file and no error is raised.
        check(ok, item.name);
        check(cfg.map_enabled == item.expected, item.name);
    }
}

void malformed_config_fails_with_named_key(const QTemporaryDir& dir) {
    struct Case {
        const char* name;
        const char* body;
        const char* must_contain;
    };
    // Given: config files that are present but invalid.
    const Case cases[] = {
        {"unknown_key", "bogus=1\n", "bogus"},
        {"missing_separator", "mqtt_port\n", "key=value"},
        {"non_numeric_port", "mqtt_port=abc\n", "mqtt_port"},
        {"zero_port", "mqtt_port=0\n", "mqtt_port"},
        {"negative_window", "stale_window_ms=-5\n", "stale_window_ms"},
        {"bad_level", "log_level=verbose\n", "log_level"},
        {"empty_value", "mqtt_host=\n", "mqtt_host"},
        {"non_numeric_hysteresis", "mode_exit_hysteresis_ms=abc\n", "mode_exit_hysteresis_ms"},
        {"zero_hysteresis", "mode_exit_hysteresis_ms=0\n", "mode_exit_hysteresis_ms"},
        {"negative_capacity", "event_history_capacity=-1\n", "event_history_capacity"},
        {"zero_capacity", "event_history_capacity=0\n", "event_history_capacity"},
        {"negative_fallback", "blind_stale_fallback_ms=-1\n", "blind_stale_fallback_ms"},
        {"non_numeric_fallback", "blind_stale_fallback_ms=soon\n", "blind_stale_fallback_ms"},
        {"non_boolean_map", "map_enabled=yes\n", "map_enabled"},
    };
    for (const Case& item : cases) {
        const QString path = dir.filePath(QString::fromLatin1(item.name) + QStringLiteral(".conf"));
        check(write_file(path, QString::fromLatin1(item.body)), item.name);
        // When: the invalid config is loaded.
        rm_terminal::Config cfg;
        QString error;
        const bool ok = rm_terminal::load_config(path, &cfg, &error);
        // Then: it fails loudly and names the offending key.
        check(!ok, item.name);
        check(!error.isEmpty(), item.name);
        check(error.contains(QString::fromLatin1(item.must_contain)), item.name);
    }
}

void record_format_is_greppable(const QTemporaryDir& dir) {
    // Given: an open structured log at debug level.
    const QString path = dir.filePath(QStringLiteral("format.log"));
    QString error;
    check(rm_terminal::StructuredLog::open(path, rm_terminal::LogLevel::debug, &error),
          "log opens");
    // When: a record with fields is written.
    rm_terminal::StructuredLog::write(rm_terminal::LogLevel::warning, QStringLiteral("packet_loss"),
                                      {QStringLiteral("dropped=3"), QStringLiteral("frame=7")});
    rm_terminal::StructuredLog::close();
    // Then: the line carries ts, level, event, and every field as key=value.
    const QStringList lines = read_lines(path);
    check(lines.size() == 1, "one record written");
    if (lines.isEmpty()) return;
    const QString line = lines.first();
    check(line.startsWith(QLatin1String("ts=")), "record starts with ts");
    check(line.contains(QLatin1String("level=warning")), "record carries level");
    check(line.contains(QLatin1String("event=packet_loss")), "record carries event");
    check(line.contains(QLatin1String("dropped=3")), "record carries first field");
    check(line.contains(QLatin1String("frame=7")), "record carries second field");
    check(line.contains(QLatin1Char('T')) && line.contains(QLatin1Char('.')),
          "timestamp is ISO8601 with milliseconds");
}

void level_filtering_drops_quiet_records(const QTemporaryDir& dir) {
    // Given: a log opened at warning level.
    const QString path = dir.filePath(QStringLiteral("filter.log"));
    check(rm_terminal::StructuredLog::open(path, rm_terminal::LogLevel::warning), "filter opens");
    // When: records below and at the threshold are written.
    rm_terminal::StructuredLog::write(rm_terminal::LogLevel::debug, QStringLiteral("stale_data"));
    rm_terminal::StructuredLog::write(rm_terminal::LogLevel::info, QStringLiteral("startup"));
    rm_terminal::StructuredLog::write(rm_terminal::LogLevel::warning,
                                      QStringLiteral("decode_failure"));
    rm_terminal::StructuredLog::write(rm_terminal::LogLevel::error,
                                      QStringLiteral("readonly_block"));
    rm_terminal::StructuredLog::close();
    // Then: only warning and error survive.
    const QStringList lines = read_lines(path);
    check(lines.size() == 2, "two records survive filtering");
    const QString joined = lines.join(QLatin1Char('|'));
    check(!joined.contains(QLatin1String("event=stale_data")), "debug filtered out");
    check(!joined.contains(QLatin1String("event=startup")), "info filtered out");
    check(joined.contains(QLatin1String("event=decode_failure")), "warning retained");
    check(joined.contains(QLatin1String("event=readonly_block")), "error retained");
}

void reopen_appends_and_is_leak_safe(const QTemporaryDir& dir) {
    // Given: a log that is opened and written once.
    const QString path = dir.filePath(QStringLiteral("restart.log"));
    check(rm_terminal::StructuredLog::open(path, rm_terminal::LogLevel::info), "first open");
    rm_terminal::StructuredLog::write(rm_terminal::LogLevel::info, QStringLiteral("startup"),
                                      {QStringLiteral("run=1")});
    // When: it is reopened without an intervening close, then closed twice.
    check(rm_terminal::StructuredLog::open(path, rm_terminal::LogLevel::info), "second open");
    check(rm_terminal::StructuredLog::open(path, rm_terminal::LogLevel::info), "third open");
    rm_terminal::StructuredLog::write(rm_terminal::LogLevel::info, QStringLiteral("startup"),
                                      {QStringLiteral("run=2")});
    check(rm_terminal::StructuredLog::isOpen(), "log reports open");
    rm_terminal::StructuredLog::close();
    rm_terminal::StructuredLog::close();
    check(!rm_terminal::StructuredLog::isOpen(), "log reports closed");
    // Then: both runs are present because reopening appends.
    const QStringList lines = read_lines(path);
    check(lines.size() == 2, "restart appends rather than truncating");
    const QString joined = lines.join(QLatin1Char('|'));
    check(joined.contains(QLatin1String("run=1")), "first run retained");
    check(joined.contains(QLatin1String("run=2")), "second run retained");
}

void writes_after_close_are_dropped(const QTemporaryDir& dir) {
    // Given: a closed log.
    const QString path = dir.filePath(QStringLiteral("closed.log"));
    check(rm_terminal::StructuredLog::open(path, rm_terminal::LogLevel::info), "open before close");
    rm_terminal::StructuredLog::close();
    // When: a record is written after close.
    rm_terminal::StructuredLog::write(rm_terminal::LogLevel::error,
                                      QStringLiteral("decode_failure"));
    // Then: nothing is appended and no crash occurs.
    check(read_lines(path).isEmpty(), "post-close write is dropped");
}

void sink_reaches_the_log_without_deadlock(const QTemporaryDir& dir) {
    // Given: the composition-root sink forwarding into the structured log.
    const QString path = dir.filePath(QStringLiteral("sink.log"));
    check(rm_terminal::StructuredLog::open(path, rm_terminal::LogLevel::info), "sink log opens");
    rm_terminal::set_log_sink([](rm_terminal::LogLevel level, const QString& event,
                                 const QStringList& fields) {
        rm_terminal::StructuredLog::write(level, event, fields);
    });
    // When: a domain-side component emits through the indirection.
    rm_terminal::emit_log(rm_terminal::LogLevel::warning, QStringLiteral("stale_data"),
                          {QStringLiteral("field=game.current_stage")});
    rm_terminal::set_log_sink(nullptr);
    rm_terminal::emit_log(rm_terminal::LogLevel::error, QStringLiteral("decode_failure"));
    rm_terminal::StructuredLog::close();
    // Then: the sunk record lands in the file and emitting with no sink is inert.
    const QStringList lines = read_lines(path);
    check(lines.size() == 1, "sink forwards exactly one record");
    if (lines.isEmpty()) return;
    check(lines.first().contains(QLatin1String("event=stale_data")), "sink preserves event");
    check(lines.first().contains(QLatin1String("field=game.current_stage")),
          "sink preserves fields");
}

void log_level_names_round_trip() {
    // Given: every supported level.
    const rm_terminal::LogLevel levels[] = {rm_terminal::LogLevel::debug,
                                            rm_terminal::LogLevel::info,
                                            rm_terminal::LogLevel::warning,
                                            rm_terminal::LogLevel::error};
    for (const rm_terminal::LogLevel level : levels) {
        // When: a name is rendered and parsed back.
        rm_terminal::LogLevel parsed = rm_terminal::LogLevel::error;
        const QString name = rm_terminal::StructuredLog::levelName(level);
        const bool ok = rm_terminal::parse_log_level(name, &parsed);
        // Then: the round trip is lossless.
        check(ok, "level name parses");
        check(parsed == level, "level round trips");
    }
    rm_terminal::LogLevel ignored = rm_terminal::LogLevel::info;
    check(!rm_terminal::parse_log_level(QStringLiteral("verbose"), &ignored),
          "unknown level rejected");
}

}

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    if (!dir.isValid()) {
        fprintf(stderr, "FAIL temporary directory unavailable\n");
        return 1;
    }

    missing_file_uses_documented_defaults(dir);
    every_key_is_honoured(dir);
    blind_stale_fallback_accepts_zero_as_disabled(dir);
    map_enabled_accepts_both_booleans(dir);
    malformed_config_fails_with_named_key(dir);
    record_format_is_greppable(dir);
    level_filtering_drops_quiet_records(dir);
    reopen_appends_and_is_leak_safe(dir);
    writes_after_close_are_dropped(dir);
    sink_reaches_the_log_without_deadlock(dir);
    log_level_names_round_trip();

    if (failures != 0) {
        fprintf(stderr, "config_test: %d check(s) failed\n", failures);
        return 1;
    }
    printf("config_test: all checks passed\n");
    return 0;
}
