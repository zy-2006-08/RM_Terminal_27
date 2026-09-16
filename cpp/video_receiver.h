#pragma once
#include "video.h"
#include "video_recovery.h"
#include <QByteArray>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QUdpSocket>

namespace rm_terminal {
struct VideoEndpoint {
    QHostAddress address=QHostAddress::LocalHost;
    quint16 port=0;
    QString ffmpeg="ffmpeg";
};
class VideoReceiver final : public QObject {
public:
    explicit VideoReceiver(QObject* parent=nullptr);
    ~VideoReceiver() override;
    bool start(const VideoEndpoint& endpoint);
    void stop();
    QJsonObject snapshot() const;
    quint16 port() const { return socket_.localPort(); }
    // BGR24 pixels, empty until the first frame completes.
    //
    // THREAD AFFINITY: every access to this class's state, including `latest_`,
    // must happen on the thread that owns the instance. `latest_` has NO mutex.
    // This is safe today only because there is no threading in cpp/ at all (no
    // QThread / std::thread / moveToThread), so decoderOutput() and the
    // dashboard's refresh tick both run on the Qt main thread. That is an absence
    // of concurrency, NOT thread safety -- do not describe it as the latter.
    // CONSTRAINT: if decoding ever moves to a worker thread, `latest_` must gain a
    // mutex or be replaced by publishing an owned snapshot. A caller on another
    // thread would otherwise race a QByteArray refcount and read a torn frame.
    QByteArray latestFrame() const { return latest_; }
    static constexpr int frameWidth() { return 320; }
    static constexpr int frameHeight() { return 180; }
    bool online() const { return online_; }
    QString state() const { return state_; }
    qint64 decodedFrames() const { return decoded_frames_; }
private:
    void ready();
    void decoderOutput();
    void tick();
    bool launchDecoder();
    void stopDecoder();
    void fail(const QString& reason);
    QByteArray primeAccessUnit(const QByteArray& frame);
    QByteArray parameterSets() const;
    QUdpSocket socket_;
    QProcess decoder_;
    QTimer timer_;
    QElapsedTimer clock_;
    VideoReassembler reassembler_;
    VideoEndpoint endpoint_;
    QByteArray raw_, latest_;
    // Latest VPS/SPS/PPS only. Appending would grow without bound and feed the
    // respawned decoder dozens of duplicate parameter sets, which stalls decoding.
    QByteArray vps_, sps_, pps_;
    bool decoder_primed_=false;
    static constexpr qsizetype frame_bytes_=320*180*3;
    qint64 decoded_frames_=0, failures_=0, invalid_frames_=0, stderr_bytes_=0;
    qint64 last_packet_=-1, last_datagram_=-1, last_frame_=-1, last_launch_=-1000, submitted_=-1;
    // Owns the dual-liveness thresholds, the restart cooldown, and the stderr
    // edge-detection that tick() used to inline. Recreated by start(), so a restart
    // does not inherit a cooldown earned by the previous session.
    VideoRecoveryPolicy recovery_;
    bool stopping_=false, running_=false, online_=false, received_=false;
    QString failure_, state_="stopped";
    // Loss logging is sampled, not per-packet: a lossy 60fps stream would emit
    // thousands of records per second and bury every other event.
    void reportLoss();
    qint64 last_loss_log_=-1;
    uint64_t logged_missing_=0, logged_incomplete_=0, logged_expired_=0;
};
}
