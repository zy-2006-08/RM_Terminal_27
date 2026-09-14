#pragma once
#include <QString>
#include <QStringList>
#include <functional>
namespace rm_terminal {

enum class LogLevel { debug, info, warning, error };

// Emits one greppable record per event:
//   ts=<ISO8601 with ms> level=<level> event=<token> key=value ...
// The only sink is a local file. Operator-facing banners go to stderr through
// qInfo/qCritical instead. A network sink would give the read-only terminal an
// egress path and breach the module-1 control boundary, so there is none.
class StructuredLog {
public:
    static bool open(const QString& destination, LogLevel minimum, QString* error = nullptr);
    static void close();
    static bool isOpen();
    static void write(LogLevel level, const QString& event, const QStringList& fields = {});
    static QString levelName(LogLevel level);
    static QString format(const QString& timestamp, LogLevel level, const QString& event,
                          const QStringList& fields);
};

// Indirection so terminal_domain and terminal_video can report events without
// linking the logger: main.cpp installs the sink at the composition root.
using LogSink = std::function<void(LogLevel, const QString&, const QStringList&)>;
void set_log_sink(LogSink sink);
void emit_log(LogLevel level, const QString& event, const QStringList& fields = {});

}
