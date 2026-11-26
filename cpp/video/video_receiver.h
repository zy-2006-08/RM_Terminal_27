#pragma once
#include "video.h"
#include "video_recovery.h"
#include <QByteArray>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QJsonObject>
#include <QMutex>
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QUdpSocket>
#include <functional>

namespace rm_terminal
{
    struct VideoEndpoint
    {
        QHostAddress address = QHostAddress::LocalHost;
        quint16 port = 0;
        QString ffmpeg = "ffmpeg";
        // "vaapi" = 核显硬件解码,其余值 = 软件解码。见 Config::video_hwaccel。
        QString hwaccel = "none";
    };
    // 收包、重组、喂解码器、读解码结果,全在这个对象所在的线程里做。实况的本机图传把它整个挪到
    // 一条工作线程(main.cpp):界面线程等屏幕刷新、窗口最小化后每秒只刷一次,都不再耽误收包,
    // 包也就不会在系统缓冲里越积越多。测试、指挥手图传墙、回放仍留在主线程。
    //
    // 线程约定:读接口(latestFrame、online、state、snapshot……)只读 publish() 发布的那一份,
    // 带锁,任何线程都能调。start/startInjected/stop/setDatagramObserver/injectDatagram 从别的
    // 线程调时,转到对象所在的线程执行,做完才返回。
    class VideoReceiver final : public QObject
    {
    public:
        explicit VideoReceiver(QObject* parent = nullptr);
        ~VideoReceiver() override;
        bool start(const VideoEndpoint& endpoint);
        // 回放用:不绑定 UDP 端口,只启动解码器,由 injectDatagram() 喂原始包。
        // 这样回放不会和本机模拟器/另一台终端抢端口,也不会碰网络。
        bool startInjected();
        void stop();
        // 解码前观察原始 datagram。录制必须挂这里:重组会丢掉非法包和部分帧,
        // 而回放要的是链路上实际到达的字节。回调在接收器所在的线程执行:挪到工作线程后,
        // 回调里只能碰线程安全的东西,别的要自己排回主线程。
        void setDatagramObserver(std::function<void(const QByteArray&)> observer);
        void injectDatagram(const QByteArray& datagram);
        QJsonObject snapshot() const;
        quint16 port() const;
        // BGR24 pixels, empty until the first frame completes. 和 frameWidth/frameHeight 出自同一次发布。
        QByteArray latestFrame() const;
        int frameWidth() const;
        int frameHeight() const;
        bool online() const;
        QString state() const;
        qint64 decodedFrames() const;
        quint64 packetsReceived() const;
        // 距离上一帧解出来过了多久;还没解出过任何一帧时为 -1。
        qint64 msSinceLastFrame() const;

    private:
        // 读接口看到的那一份。
        struct Published
        {
            QString state = QStringLiteral("stopped"), failure;
            bool online = false;
            quint16 port = 0;
            qint64 decoder_pid = 0, decoded_frames = 0, failures = 0, invalid_frames = 0, stderr_bytes = 0;
            qint64 reconnects = 0, decoder_restarts = 0, last_frame = -1;
            qint64 cached_frames = 0, raw_bytes = 0, queued_bytes = 0;
            VideoStats stats;
            QByteArray latest;
            int width = 640, height = 360;
        };
        class PublishOnExit;
        void publish();
        Published published() const;
        bool startHere(const VideoEndpoint& endpoint);
        bool startInjectedHere();
        void stopHere();
        void injectHere(const QByteArray& bytes);
        void ready();
        void handleDatagram(const QByteArray& bytes);
        void decoderOutput();
        void tick();
        bool launchDecoder();
        void resolveDecoder();
        bool fallBackToSoftware(const QString& reason);
        void stopDecoder();
        void fail(const QString& reason);
        QByteArray primeAccessUnit(const QByteArray& frame);
        QByteArray parameterSets() const;
        // 父对象是 this:moveToThread 时跟着一起挪。
        QUdpSocket socket_;
        QProcess decoder_;
        QTimer timer_;
        QElapsedTimer clock_;
        VideoReassembler reassembler_;
        VideoEndpoint endpoint_;
        // 实际用于 ffmpeg 的解码方式:auto 在启动时解析成 vaapi/none;硬解失败后落到 none。
        QString active_hwaccel_ = QStringLiteral("none");
        bool hwaccel_fallback_done_ = false;
        std::function<void(const QByteArray&)> datagram_observer_;

        QByteArray raw_, latest_;
        // Latest VPS/SPS/PPS only. Appending would grow without bound and feed the
        // respawned decoder dozens of duplicate parameter sets, which stalls decoding.
        QByteArray vps_, sps_, pps_;
        bool decoder_primed_ = false;
        int width_ = 640;
        int height_ = 360;
        qsizetype frame_bytes_ = qsizetype(width_) * height_ * 3;
        qint64 decoded_frames_ = 0, failures_ = 0, invalid_frames_ = 0, stderr_bytes_ = 0;
        qint64 last_packet_ = -1, last_datagram_ = -1, last_frame_ = -1, last_launch_ = -1000, submitted_ = -1;
        // Owns the dual-liveness thresholds, the restart cooldown, and the stderr
        // edge-detection that tick() used to inline. Recreated by start(), so a restart
        // does not inherit a cooldown earned by the previous session.
        VideoRecoveryPolicy recovery_;
        bool stopping_ = false, running_ = false, online_ = false, received_ = false;
        QString failure_, state_ = "stopped";
        // Loss logging is sampled, not per-packet: a lossy 60fps stream would emit
        // thousands of records per second and bury every other event.
        void reportLoss();
        qint64 last_loss_log_ = -1;
        uint64_t logged_missing_ = 0, logged_incomplete_ = 0, logged_expired_ = 0;
        mutable QMutex published_mutex_;
        Published published_;
    };
} // namespace rm_terminal
