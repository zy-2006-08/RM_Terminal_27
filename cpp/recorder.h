#pragma once

#include <QByteArray>
#include <QFile>
#include <QJsonObject>
#include <QString>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>

namespace rm_terminal {

// 一次录制的元数据。只在开录时写一次,回放方据此知道这份录像属于哪台车。
struct RecordingMetadata {
    int self_robot_id = 0;
    QString mqtt_host;
    int mqtt_port = 0;
    int udp_port = 0;
};

// 落盘格式是内部格式,不是 MCAP:
//
//   magic "RMTREC1\n" | u32 version | u32 metadata_len | metadata JSON
//   record*: u64 time_us | u8 kind | u16 topic_len | u32 payload_len | topic | payload
//
// 所有整数小端。时间是从开录起算的本机单调微秒 —— 回放只关心相对间隔,
// 不跨机器比较。原始 MQTT 字节和原始 UDP 包都原样保存,坏了也能回放。
class Recorder final {
public:
    Recorder();
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    // 创建目录并打开 recordings/<时间>_self<编号>.rmrec。失败返回 false,error 说明原因。
    bool start(const QString& directory, const RecordingMetadata& metadata, QString* error);
    void stop();

    bool active() const;
    bool failed() const;
    QString path() const;
    QString error() const;
    // "● REC 00:12 · 1.2 MB";失败时是"录制失败：…";未开录为空。
    QString statusText() const;
    std::uint64_t records() const;
    std::uint64_t bytes() const;

    // 线程安全。mosquitto 网络线程调用,必须排在本机解码之前。
    void recordMqtt(const QString& topic, const QByteArray& payload);
    // 线程安全。Qt 主线程调用,必须在 UDP 重组之前。
    void recordVideo(const QByteArray& datagram);

private:
    void writeLocked(std::uint8_t kind, const QString& topic, const QByteArray& payload);
    void failLocked(const QString& reason);

    mutable std::mutex mutex_;
    std::unique_ptr<QFile> file_;
    QString path_;
    QString error_;
    std::chrono::steady_clock::time_point started_{};
    std::int64_t last_flush_ms_ = 0;
    std::uint64_t records_ = 0;
    std::uint64_t bytes_ = 0;
    bool active_ = false;
    bool failed_ = false;
};

struct RecordingRecord {
    enum class Kind : std::uint8_t { Mqtt = 1, Video = 2 };
    Kind kind = Kind::Mqtt;
    std::uint64_t time_us = 0;
    QString topic;
    QByteArray payload;
};

class RecordingReader final {
public:
    bool open(const QString& path, QString* error = nullptr);
    // 读到文件尾返回 false 且 atEnd()==true;读到截断/非法记录返回 false 且 error() 非空。
    bool next(RecordingRecord* out, QString* error = nullptr);
    bool atEnd() const { return eof_; }
    const QJsonObject& metadata() const { return metadata_; }
    const QString& error() const { return error_; }

private:
    QFile file_;
    QJsonObject metadata_;
    QString error_;
    bool eof_ = false;
};

}  // namespace rm_terminal
