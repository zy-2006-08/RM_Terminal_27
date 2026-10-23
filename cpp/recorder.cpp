#include "recorder.h"

#include <QDateTime>
#include <QDir>
#include <QJsonDocument>

#include <cstring>

namespace rm_terminal {
namespace {

constexpr char kMagic[8] = {'R', 'M', 'T', 'R', 'E', 'C', '1', '\n'};
constexpr quint32 kVersion = 1;
constexpr quint32 kMaxMetadataBytes = 1u << 20;
constexpr quint32 kMaxPayloadBytes = 16u << 20;
constexpr quint16 kMaxTopicBytes = 1024;

void append_u16(QByteArray& out, quint16 value) {
    out.append(static_cast<char>(value & 0xff));
    out.append(static_cast<char>((value >> 8) & 0xff));
}

void append_u32(QByteArray& out, quint32 value) {
    for (int shift = 0; shift < 32; shift += 8)
        out.append(static_cast<char>((value >> shift) & 0xff));
}

void append_u64(QByteArray& out, quint64 value) {
    for (int shift = 0; shift < 64; shift += 8)
        out.append(static_cast<char>((value >> shift) & 0xff));
}

quint16 read_u16(const char* data) {
    return static_cast<quint16>(static_cast<quint8>(data[0])) |
           static_cast<quint16>(static_cast<quint8>(data[1]) << 8);
}

quint32 read_u32(const char* data) {
    quint32 value = 0;
    for (int index = 0; index < 4; ++index)
        value |= static_cast<quint32>(static_cast<quint8>(data[index])) << (index * 8);
    return value;
}

quint64 read_u64(const char* data) {
    quint64 value = 0;
    for (int index = 0; index < 8; ++index)
        value |= static_cast<quint64>(static_cast<quint8>(data[index])) << (index * 8);
    return value;
}

bool read_exact(QFile& file, char* out, qint64 size) {
    qint64 done = 0;
    while (done < size) {
        const qint64 got = file.read(out + done, size - done);
        if (got <= 0) return false;
        done += got;
    }
    return true;
}

}  // namespace

Recorder::Recorder() = default;
Recorder::~Recorder() { stop(); }

bool Recorder::start(const QString& directory, const RecordingMetadata& metadata,
                     QString* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_) {
        if (error) *error = QStringLiteral("already recording");
        return false;
    }

    error_.clear();
    failed_ = false;
    records_ = 0;
    bytes_ = 0;
    path_.clear();

    QDir dir(directory);
    if (!dir.exists() && !QDir().mkpath(directory)) {
        error_ = QStringLiteral("cannot create recording directory: %1").arg(directory);
        failed_ = true;
        if (error) *error = error_;
        return false;
    }

    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    const QString base = QStringLiteral("%1_self%2")
                             .arg(stamp)
                             .arg(metadata.self_robot_id);
    QString path = dir.filePath(base + QStringLiteral(".rmrec"));
    for (int suffix = 1; QFile::exists(path); ++suffix)
        path = dir.filePath(QStringLiteral("%1_%2.rmrec").arg(base).arg(suffix));

    path_ = path;
    file_ = std::make_unique<QFile>(path);
    if (!file_->open(QIODevice::WriteOnly)) {
        error_ = QStringLiteral("cannot open recording: %1").arg(file_->errorString());
        file_.reset();
        failed_ = true;
        if (error) *error = error_;
        return false;
    }

    QJsonObject meta;
    meta[QStringLiteral("format")] = QStringLiteral("rmrec");
    meta[QStringLiteral("version")] = static_cast<int>(kVersion);
    meta[QStringLiteral("self_robot_id")] = metadata.self_robot_id;
    meta[QStringLiteral("started_wall")] =
        QDateTime::currentDateTime().toString(Qt::ISODate);
    meta[QStringLiteral("mqtt_host")] = metadata.mqtt_host;
    meta[QStringLiteral("mqtt_port")] = metadata.mqtt_port;
    meta[QStringLiteral("udp_port")] = metadata.udp_port;
    const QByteArray json = QJsonDocument(meta).toJson(QJsonDocument::Compact);

    QByteArray header(kMagic, sizeof(kMagic));
    append_u32(header, kVersion);
    append_u32(header, static_cast<quint32>(json.size()));
    if (file_->write(header) != header.size() || file_->write(json) != json.size()) {
        const QString reason = file_->errorString();
        failLocked(reason.isEmpty() ? QStringLiteral("write failed") : reason);
        if (error) *error = error_;
        return false;
    }

    started_ = std::chrono::steady_clock::now();
    last_flush_ms_ = 0;
    active_ = true;
    return true;
}

void Recorder::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_) {
        if (active_) file_->flush();
        file_->close();
        file_.reset();
    }
    active_ = false;
}

bool Recorder::active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_;
}

bool Recorder::failed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return failed_;
}

QString Recorder::path() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return path_;
}

QString Recorder::error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

QString Recorder::statusText() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (failed_) {
        return error_.isEmpty() ? QStringLiteral("录制失败")
                                : QStringLiteral("录制失败：%1").arg(error_);
    }
    if (!active_) return QString();

    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::steady_clock::now() - started_)
                             .count();
    const double megabytes = static_cast<double>(bytes_) / (1024.0 * 1024.0);
    return QStringLiteral("● REC %1:%2 · %3 MB")
        .arg(elapsed / 60, 2, 10, QLatin1Char('0'))
        .arg(elapsed % 60, 2, 10, QLatin1Char('0'))
        .arg(megabytes, 0, 'f', 1);
}

std::uint64_t Recorder::records() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return records_;
}

std::uint64_t Recorder::bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bytes_;
}

void Recorder::recordMqtt(const QString& topic, const QByteArray& payload) {
    std::lock_guard<std::mutex> lock(mutex_);
    writeLocked(static_cast<std::uint8_t>(RecordingRecord::Kind::Mqtt), topic, payload);
}

void Recorder::recordVideo(const QByteArray& datagram) {
    std::lock_guard<std::mutex> lock(mutex_);
    writeLocked(static_cast<std::uint8_t>(RecordingRecord::Kind::Video), QString(), datagram);
}

void Recorder::writeLocked(std::uint8_t kind, const QString& topic,
                           const QByteArray& payload) {
    if (!active_ || !file_) return;

    const QByteArray topic_bytes = topic.toUtf8();
    if (topic_bytes.size() > kMaxTopicBytes ||
        static_cast<quint64>(payload.size()) > kMaxPayloadBytes) {
        failLocked(QStringLiteral("record too large"));
        return;
    }

    const auto time_us = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - started_)
                             .count();
    QByteArray header;
    header.reserve(15 + topic_bytes.size());
    append_u64(header, static_cast<quint64>(time_us));
    header.append(static_cast<char>(kind));
    append_u16(header, static_cast<quint16>(topic_bytes.size()));
    append_u32(header, static_cast<quint32>(payload.size()));

    if (file_->write(header) != header.size() ||
        (!topic_bytes.isEmpty() && file_->write(topic_bytes) != topic_bytes.size()) ||
        (!payload.isEmpty() && file_->write(payload) != payload.size())) {
        const QString reason = file_->errorString();
        failLocked(reason.isEmpty() ? QStringLiteral("write failed") : reason);
        return;
    }

    ++records_;
    bytes_ += static_cast<std::uint64_t>(header.size() + topic_bytes.size() + payload.size());

    // ponytail: 每秒 flush 到操作系统,不做 fsync。进程崩溃最多丢约 1s,整机断电
    // 可能丢得更多;需要抗断电时再改成每条记录 fsync。
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - started_)
                            .count();
    if (now_ms - last_flush_ms_ >= 1000) {
        last_flush_ms_ = now_ms;
        if (!file_->flush()) {
            const QString reason = file_->errorString();
            failLocked(reason.isEmpty() ? QStringLiteral("flush failed") : reason);
        }
    }
}

void Recorder::failLocked(const QString& reason) {
    failed_ = true;
    active_ = false;
    if (error_.isEmpty()) error_ = reason;
    if (file_) {
        file_->close();
        file_.reset();
    }
}

bool RecordingReader::open(const QString& path, QString* error) {
    file_.close();
    metadata_ = QJsonObject();
    error_.clear();
    eof_ = false;
    file_.setFileName(path);
    if (!file_.open(QIODevice::ReadOnly)) {
        error_ = QStringLiteral("cannot open recording: %1").arg(file_.errorString());
        if (error) *error = error_;
        return false;
    }

    char magic[sizeof(kMagic)];
    if (!read_exact(file_, magic, sizeof(magic)) ||
        std::memcmp(magic, kMagic, sizeof(kMagic)) != 0) {
        error_ = QStringLiteral("not an rmrec recording");
        if (error) *error = error_;
        return false;
    }

    char buffer[4];
    if (!read_exact(file_, buffer, 4) || read_u32(buffer) != kVersion) {
        error_ = QStringLiteral("unsupported recording version");
        if (error) *error = error_;
        return false;
    }
    if (!read_exact(file_, buffer, 4)) {
        error_ = QStringLiteral("truncated metadata length");
        if (error) *error = error_;
        return false;
    }
    const quint32 metadata_size = read_u32(buffer);
    if (metadata_size == 0 || metadata_size > kMaxMetadataBytes) {
        error_ = QStringLiteral("invalid metadata length");
        if (error) *error = error_;
        return false;
    }

    QByteArray json(static_cast<qsizetype>(metadata_size), Qt::Uninitialized);
    if (!read_exact(file_, json.data(), metadata_size)) {
        error_ = QStringLiteral("truncated metadata");
        if (error) *error = error_;
        return false;
    }
    const QJsonDocument document = QJsonDocument::fromJson(json);
    if (!document.isObject()) {
        error_ = QStringLiteral("invalid metadata JSON");
        if (error) *error = error_;
        return false;
    }
    metadata_ = document.object();
    return true;
}

bool RecordingReader::next(RecordingRecord* out, QString* error) {
    if (!file_.isOpen()) {
        error_ = QStringLiteral("recording is not open");
        if (error) *error = error_;
        return false;
    }

    char header[15];
    const qint64 got = file_.read(header, sizeof(header));
    if (got == 0) {
        eof_ = true;
        if (error) error->clear();
        return false;
    }
    if (got != static_cast<qint64>(sizeof(header))) {
        eof_ = true;
        error_ = QStringLiteral("truncated record header");
        if (error) *error = error_;
        return false;
    }

    const quint64 time_us = read_u64(header);
    const quint8 kind = static_cast<quint8>(header[8]);
    const quint16 topic_size = read_u16(header + 9);
    const quint32 payload_size = read_u32(header + 11);
    if (topic_size > kMaxTopicBytes || payload_size > kMaxPayloadBytes) {
        eof_ = true;
        error_ = QStringLiteral("invalid record length");
        if (error) *error = error_;
        return false;
    }

    QByteArray topic(static_cast<qsizetype>(topic_size), Qt::Uninitialized);
    if (topic_size > 0 && !read_exact(file_, topic.data(), topic_size)) {
        eof_ = true;
        error_ = QStringLiteral("truncated topic");
        if (error) *error = error_;
        return false;
    }
    QByteArray payload(static_cast<qsizetype>(payload_size), Qt::Uninitialized);
    if (payload_size > 0 && !read_exact(file_, payload.data(), payload_size)) {
        eof_ = true;
        error_ = QStringLiteral("truncated payload");
        if (error) *error = error_;
        return false;
    }

    RecordingRecord record;
    record.time_us = time_us;
    record.topic = QString::fromUtf8(topic);
    record.payload = std::move(payload);
    if (kind == static_cast<quint8>(RecordingRecord::Kind::Mqtt)) {
        record.kind = RecordingRecord::Kind::Mqtt;
    } else if (kind == static_cast<quint8>(RecordingRecord::Kind::Video)) {
        record.kind = RecordingRecord::Kind::Video;
    } else {
        eof_ = true;
        error_ = QStringLiteral("unknown record kind");
        if (error) *error = error_;
        return false;
    }

    if (out) *out = std::move(record);
    return true;
}

}  // namespace rm_terminal
