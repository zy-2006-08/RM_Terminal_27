#pragma once

#include "recorder.h"

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>

#include <memory>
#include <optional>

namespace rm_terminal {

class Decoder;
class Store;
class VideoReceiver;

// 回放把录制里的原始字节直接喂给解码器和图传重组器,不连 MQTT、不发网络、
// 不再录制。播放位置是一条可暂停、可拖动、可变速的时间轴;时间戳仍用回放当前
// 的本机单调时钟,因此 Store 的新鲜度和界面模式判定与实况走同一套逻辑。
class ReplayPlayer final : public QObject {
    Q_OBJECT
public:
    ReplayPlayer(Store& store, VideoReceiver& video, QObject* parent = nullptr);
    ~ReplayPlayer() override;

    bool start(const QString& path, QString* error = nullptr);
    void stop();

    bool active() const { return active_; }
    bool finished() const { return finished_; }
    bool paused() const { return paused_; }
    double speed() const { return speed_; }
    qint64 positionMs() const;
    qint64 durationMs() const;
    QString statusText() const;

    void togglePause();
    void seek(qint64 position_ms);
    void setSpeed(double speed);

Q_SIGNALS:
    void progress(qint64 position_ms, qint64 duration_ms);
    void pausedChanged(bool paused);

private:
    void pump();
    bool readNext();
    void deliver(const RecordingRecord& record);
    void emitProgress();

    Store& store_;
    VideoReceiver& video_;
    RecordingReader reader_;
    std::unique_ptr<Decoder> decoder_;
    std::optional<RecordingRecord> pending_;
    QTimer timer_;
    QElapsedTimer clock_;
    QString path_;
    QString error_;
    double position_us_ = 0.0;
    qint64 duration_us_ = 0;
    double speed_ = 1.0;
    quint64 records_ = 0;
    int self_id_ = 0;
    bool active_ = false;
    bool finished_ = false;
    bool paused_ = false;
};

}  // namespace rm_terminal
