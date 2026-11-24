#include "logging.h"
#include <QDateTime>
#include <QFile>
#include <QMutex>
#include <QTextStream>
namespace rm_terminal {
namespace {

QFile* g_file = nullptr;
LogLevel g_threshold = LogLevel::info;
QMutex g_mutex;

// Guards the sink separately from the file. emit_log must not hold the file
// mutex while invoking the sink, because the installed sink calls back into
// StructuredLog::write and QMutex is not recursive.
QMutex g_sink_mutex;
LogSink g_sink;

}

QString StructuredLog::levelName(LogLevel level) {
    switch (level) {
        case LogLevel::debug: return QStringLiteral("debug");
        case LogLevel::info: return QStringLiteral("info");
        case LogLevel::warning: return QStringLiteral("warning");
        case LogLevel::error: return QStringLiteral("error");
    }
    return QStringLiteral("info");
}

QString StructuredLog::format(const QString& timestamp, LogLevel level, const QString& event,
                              const QStringList& fields) {
    QString record = QStringLiteral("ts=%1 level=%2 event=%3")
                         .arg(timestamp, levelName(level), event);
    for (const QString& field : fields) {
        record += QLatin1Char(' ');
        record += field;
    }
    return record;
}

bool StructuredLog::open(const QString& destination, LogLevel minimum, QString* error) {
    QMutexLocker lock(&g_mutex);
    if (g_file) {
        g_file->flush();
        g_file->close();
        delete g_file;
        g_file = nullptr;
    }
    g_threshold = minimum;
    auto candidate = new QFile(destination);
    if (!candidate->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        if (error) *error = candidate->errorString();
        delete candidate;
        return false;
    }
    g_file = candidate;
    return true;
}

bool StructuredLog::isOpen() {
    QMutexLocker lock(&g_mutex);
    return g_file != nullptr;
}

void StructuredLog::write(LogLevel level, const QString& event, const QStringList& fields) {
    QMutexLocker lock(&g_mutex);
    if (!g_file || static_cast<int>(level) < static_cast<int>(g_threshold)) return;
    const QString record = format(
        QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs), level, event, fields);
    QTextStream out(g_file);
    out << record << '\n';
    out.flush();
    g_file->flush();
}

void StructuredLog::close() {
    QMutexLocker lock(&g_mutex);
    if (!g_file) return;
    g_file->flush();
    g_file->close();
    delete g_file;
    g_file = nullptr;
}

void set_log_sink(LogSink sink) {
    QMutexLocker lock(&g_sink_mutex);
    g_sink = std::move(sink);
}

void emit_log(LogLevel level, const QString& event, const QStringList& fields) {
    LogSink sink;
    {
        QMutexLocker lock(&g_sink_mutex);
        sink = g_sink;
    }
    if (sink) sink(level, event, fields);
}

}
