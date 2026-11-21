#include "video_receiver.h"
#include "logging.h"
#include <QCryptographicHash>
#include <QMetaObject>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QThread>
#include <utility>

namespace rm_terminal {
namespace {
// 从别的线程调时转到对象所在的线程执行,做完才返回。对象所在的线程已经停了时直接调:
// 那时不会再有并发。
template <class F>
void run_in(QObject* owner, F&& work) {
    QThread* home=owner->thread();
    if(home==QThread::currentThread() || !home || !home->isRunning()) { work(); return; }
    QMetaObject::invokeMethod(owner,std::forward<F>(work),Qt::BlockingQueuedConnection);
}
}

// 每个入口(事件回调、公开的启停接口)结束时发布一次,读接口看到的总是最近一次处理完的状态。
class VideoReceiver::PublishOnExit {
public:
    explicit PublishOnExit(VideoReceiver* receiver):receiver_(receiver) {}
    ~PublishOnExit(){receiver_->publish();}
    PublishOnExit(const PublishOnExit&)=delete;
    PublishOnExit& operator=(const PublishOnExit&)=delete;
private:
    VideoReceiver* receiver_;
};

VideoReceiver::VideoReceiver(QObject* parent):QObject(parent),socket_(this),decoder_(this),timer_(this) {
    clock_.start();
    connect(&socket_,&QUdpSocket::readyRead,this,&VideoReceiver::ready);
    connect(&decoder_,&QProcess::readyReadStandardOutput,this,&VideoReceiver::decoderOutput);
    connect(&decoder_,&QProcess::readyReadStandardError,this,[this] {
        PublishOnExit at_exit(this);
        const auto bytes=decoder_.readAllStandardError();
        stderr_bytes_+=bytes.size();
    });
    connect(&decoder_,&QProcess::errorOccurred,this,[this](QProcess::ProcessError) {
        PublishOnExit at_exit(this);
        if(!stopping_)fail(decoder_.errorString());
    });
    connect(&decoder_,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,
        [this](int code,QProcess::ExitStatus) {
            PublishOnExit at_exit(this);
            decoderOutput();
            if(!raw_.isEmpty()){++invalid_frames_;raw_.clear();}
            if(!stopping_)fail(QString("decoder_exit=%1").arg(code));
        });
    connect(&timer_,&QTimer::timeout,this,&VideoReceiver::tick);
    timer_.setInterval(50);
}
VideoReceiver::~VideoReceiver(){stop();}
void VideoReceiver::fail(const QString& reason) {
    failure_=reason;state_="decoder_failed";++failures_;online_=false;
    emit_log(LogLevel::error,QStringLiteral("decode_failure"),
        {QStringLiteral("reason=%1").arg(reason),
         QStringLiteral("failures=%1").arg(failures_),
         QStringLiteral("decoded_frames=%1").arg(decoded_frames_),
         QStringLiteral("port=%1").arg(socket_.localPort())});
}
bool VideoReceiver::launchDecoder() {
    last_launch_=clock_.elapsed();
    const auto executable=QStandardPaths::findExecutable(endpoint_.ffmpeg);
    if(executable.isEmpty()){fail("ffmpeg_not_found");return false;}
    decoder_.setProgram(executable);
    // Software HEVC decode at 640x360: the raw pipe carries frameWidth x frameHeight
    // BGR24, and two decoder threads plus low_delay keep the picture close to the
    // official client without flooding the pipe. 1080p BGR24 is 373 MB/s at 60 fps.
    // analyzeduration 0 starves the prober on a lossy pipe, so give probing a
    // real budget. Do not add +discardcorrupt: it silently drops ~8% of frames.
    // hwaccel=vaapi swaps only the decoder: frames come back to system memory and go
    // through the same scale and BGR24 conversion, so the pipe carries the same bytes.
    // fps_mode passthrough: a decoder started before any VPS/SPS/PPS (a cold start
    // mid-stream; they ride about one frame per second) finishes probing without a
    // frame rate, assumes 25 fps and drops every frame above that for its whole life.
    width_ = 640;
    height_ = 360;
    frame_bytes_ = qsizetype(width_) * height_ * 3;
    const QStringList hwaccel=endpoint_.hwaccel==QLatin1String("vaapi")
        ? QStringList{"-hwaccel","vaapi","-hwaccel_device","/dev/dri/renderD128"}
        : QStringList{"-hwaccel","none"};
    decoder_.setArguments(QStringList{"-hide_banner","-loglevel","error"}+hwaccel+QStringList{
        "-fflags","nobuffer","-flags","low_delay",
        "-threads","2","-probesize","262144","-analyzeduration","2000000",
        "-f","hevc",
        "-c:v","hevc","-i","pipe:0","-vf","scale=640:360","-threads","2",
        "-fps_mode","passthrough",
        "-f","rawvideo","-pix_fmt","bgr24","pipe:1"});
    decoder_.start();
    if(!decoder_.waitForStarted(1000)){fail("ffmpeg_start_failed");return false;}
    raw_.clear();submitted_=-1;decoder_primed_=false;state_="waiting";return true;
}
bool VideoReceiver::start(const VideoEndpoint& endpoint) {
    bool ok=false;
    run_in(this,[&] { ok=startHere(endpoint); });
    return ok;
}
bool VideoReceiver::startHere(const VideoEndpoint& endpoint) {
    PublishOnExit at_exit(this);
    stopHere(); endpoint_=endpoint;failure_.clear();last_packet_=-1;last_frame_=-1;last_datagram_=-1;
    vps_.clear();sps_.clear();pps_.clear();
    received_=false;reassembler_.resetSession();
    recovery_=VideoRecoveryPolicy{};
    if(!socket_.bind(endpoint.address,endpoint.port,QAbstractSocket::DontShareAddress)) {
        failure_="udp_bind_failed: "+socket_.errorString();state_="bind_failed";return false;
    }
    socket_.setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption,4*1024*1024);
    if(!launchDecoder()){socket_.close();return false;}
    running_=true;timer_.start();return true;
}
void VideoReceiver::stopDecoder() {
    stopping_=true;
    if(decoder_.state()!=QProcess::NotRunning) {
        decoder_.closeWriteChannel();
        if(!decoder_.waitForFinished(1000)) {
            decoder_.terminate();
            if(!decoder_.waitForFinished(500)){decoder_.kill();decoder_.waitForFinished(1000);}
        }
    }
    decoderOutput();raw_.clear();stopping_=false;
}
void VideoReceiver::stop() {
    run_in(this,[this] { stopHere(); });
}
void VideoReceiver::stopHere() {
    PublishOnExit at_exit(this);
    timer_.stop();socket_.close();running_=false;stopDecoder();online_=false;state_="stopped";
}
void VideoReceiver::setDatagramObserver(std::function<void(const QByteArray&)> observer) {
    run_in(this,[&] { datagram_observer_=std::move(observer); });
}
bool VideoReceiver::startInjected() {
    bool ok=false;
    run_in(this,[&] { ok=startInjectedHere(); });
    return ok;
}
bool VideoReceiver::startInjectedHere() {
    PublishOnExit at_exit(this);
    stopHere();
    endpoint_=VideoEndpoint{};
    failure_.clear();last_packet_=-1;last_frame_=-1;last_datagram_=-1;
    vps_.clear();sps_.clear();pps_.clear();
    received_=false;reassembler_.resetSession();recovery_=VideoRecoveryPolicy{};
    socket_.close();
    if(!launchDecoder())return false;
    running_=true;timer_.start();return true;
}
void VideoReceiver::injectDatagram(const QByteArray& bytes) {
    run_in(this,[&] { injectHere(bytes); });
}
void VideoReceiver::injectHere(const QByteArray& bytes) {
    PublishOnExit at_exit(this);
    if(!running_)return;
    last_datagram_=clock_.elapsed();
    handleDatagram(bytes);
}
void VideoReceiver::ready() {
    PublishOnExit at_exit(this);
    // Bound each event-loop turn as well as each allocation, including oversized datagrams.
    for(int budget=0;budget<256 && socket_.hasPendingDatagrams();++budget) {
        QByteArray bytes(1401,Qt::Uninitialized);
        const auto n=socket_.readDatagram(bytes.data(),bytes.size());
        if(n<0){failure_="udp_read_failed";return;}
        bytes.resize(n);
        last_datagram_=clock_.elapsed();
        // 观察者排在重组之前:录制要拿到链路上实际到达的字节,而不是被重组器
        // 清洗/丢弃后的结果。
        if(datagram_observer_)datagram_observer_(bytes);
        handleDatagram(bytes);
    }
    if(socket_.hasPendingDatagrams())QTimer::singleShot(0,this,&VideoReceiver::ready);
}
void VideoReceiver::handleDatagram(const QByteArray& bytes) {
    const auto frame=reassembler_.push(reinterpret_cast<const uint8_t*>(bytes.constData()),
        size_t(bytes.size()),clock_.elapsed()/1000.0);
    if(!frame)return;
    last_packet_=clock_.elapsed();received_=true;
    if(frame->size()<6 || (*frame)[0]!=0 || (*frame)[1]!=0 ||
        ((*frame)[2]!=1 && ((*frame)[2]!=0 || (*frame)[3]!=1))) {
        ++invalid_frames_;failure_="invalid_annex_b";return;
    }
    if(decoder_.state()==QProcess::NotRunning && clock_.elapsed()-last_launch_>=1000)launchDecoder();
    if(decoder_.state()!=QProcess::Running)return;
    const QByteArray access_unit=primeAccessUnit(QByteArray(reinterpret_cast<const char*>(frame->data()),qint64(frame->size())));
    if(decoder_.bytesToWrite()+access_unit.size()>8*1024*1024) {
        fail("decoder_backpressure");stopDecoder();return;
    }
    if(decoder_.write(access_unit)<0) {
        fail("decoder_write_failed");return;
    }
    if(submitted_<0)submitted_=clock_.elapsed();
}
QByteArray VideoReceiver::primeAccessUnit(const QByteArray& frame) {
    const QByteArray start("\0\0\0\1",4);
    int pos=0;
    while((pos=frame.indexOf(start,pos))>=0 && pos+5<frame.size()) {
        const int next=frame.indexOf(start,pos+4);
        const int end=next<0?frame.size():next;
        const int type=(quint8(frame.at(pos+4))>>1)&0x3f;
        const QByteArray nal=frame.mid(pos,end-pos);
        if(type==32)vps_=nal;
        else if(type==33)sps_=nal;
        else if(type==34)pps_=nal;
        pos=end;
    }
    // Parameter sets ride only 12 of 372 access units, so a respawned decoder must
    // be re-primed from cache. Waiting for the next in-band set stalls decoding for
    // seconds; dropping frames until then loses the stream entirely.
    if(!decoder_primed_) {
        const QByteArray sets=parameterSets();
        if(sets.isEmpty())return frame;
        decoder_primed_=true;
        return sets+frame;
    }
    return frame;
}
QByteArray VideoReceiver::parameterSets() const {
    return vps_+sps_+pps_;
}
void VideoReceiver::decoderOutput() {
    PublishOnExit at_exit(this);
    while(decoder_.bytesAvailable()>0) {
        raw_+=decoder_.read(frame_bytes_-raw_.size());
        // Any decoder output is progress. Clearing the watchdog only on a whole
        // frame kills a healthy decoder whenever loss makes frames incomplete.
        submitted_=-1;
        if(raw_.size()!=frame_bytes_)break;
        latest_=raw_;raw_.clear();++decoded_frames_;last_frame_=clock_.elapsed();
        if(!stopping_){online_=true;state_="online";failure_.clear();}
        submitted_=-1;
    }
}
void VideoReceiver::reportLoss() {
    const auto& s=reassembler_.stats();
    if(s.packets_missing==logged_missing_ && s.frames_dropped_incomplete==logged_incomplete_ &&
        s.frames_dropped_stale==logged_expired_) return;
    const auto now=clock_.elapsed();
    if(last_loss_log_>=0 && now-last_loss_log_<1000) return;
    last_loss_log_=now;
    emit_log(LogLevel::warning,QStringLiteral("packet_loss"),
        {QStringLiteral("missing=%1").arg(s.packets_missing-logged_missing_),
         QStringLiteral("incomplete=%1").arg(s.frames_dropped_incomplete-logged_incomplete_),
         QStringLiteral("expired=%1").arg(s.frames_dropped_stale-logged_expired_),
         QStringLiteral("missing_total=%1").arg(s.packets_missing),
         QStringLiteral("packets=%1").arg(s.packets_received),
         QStringLiteral("completed=%1").arg(s.frames_completed)});
    logged_missing_=s.packets_missing;
    logged_incomplete_=s.frames_dropped_incomplete;
    logged_expired_=s.frames_dropped_stale;
}
void VideoReceiver::tick() {
    PublishOnExit at_exit(this);
    const auto now=clock_.elapsed();reassembler_.expire(now/1000.0);
    reportLoss();
    if(last_frame_>=0 && now-last_frame_>=1000)online_=false;

    // 判活规则、双条件优先级和恢复冷却全部在 VideoRecoveryPolicy 里,可单测;
    // 这里只负责把观测量喂进去,再执行它给出的动作。
    VideoLiveness liveness;
    liveness.stream_established=received_;
    if(last_datagram_>=0)liveness.last_datagram=last_datagram_;
    if(submitted_>=0)liveness.awaiting_decode_since=submitted_;
    liveness.decoder_stderr_bytes=stderr_bytes_;

    switch(recovery_.step(liveness,now)) {
    case RecoveryAction::ReconnectUdp:
        received_=false;stopDecoder();reassembler_.resetSession();online_=false;state_="disconnected";
        emit_log(LogLevel::warning,QStringLiteral("reconnect_udp"),
            {QStringLiteral("state=disconnected"),
             QStringLiteral("silent_ms=%1").arg(last_datagram_>=0?now-last_datagram_:-1),
             QStringLiteral("port=%1").arg(socket_.localPort())});
        break;
    case RecoveryAction::RestartDecoder: {
        // 先取值:下面把 submitted_ 清成 -1,清完再读就只能记出 -1。
        const auto stalled_ms=submitted_>=0?now-submitted_:-1;
        stopDecoder();submitted_=-1;failure_.clear();state_="waiting";
        emit_log(LogLevel::warning,QStringLiteral("restart_decoder"),
            {QStringLiteral("stalled_ms=%1").arg(stalled_ms),
             QStringLiteral("restarts=%1").arg(recovery_.decoderRestarts()),
             QStringLiteral("port=%1").arg(socket_.localPort())});
        break;
    }
    case RecoveryAction::None:
        break;
    }
}
void VideoReceiver::publish() {
    Published next;
    next.state=state_;next.failure=failure_;next.online=online_;
    next.port=socket_.localPort();next.decoder_pid=decoder_.processId();
    next.decoded_frames=decoded_frames_;next.failures=failures_;
    next.invalid_frames=invalid_frames_;next.stderr_bytes=stderr_bytes_;
    next.reconnects=qint64(recovery_.reconnects());next.decoder_restarts=qint64(recovery_.decoderRestarts());
    next.last_frame=last_frame_;
    next.cached_frames=qint64(reassembler_.cachedFrames());
    next.raw_bytes=raw_.size();next.queued_bytes=decoder_.bytesToWrite();
    next.stats=reassembler_.stats();
    next.latest=latest_;next.width=width_;next.height=height_;
    QMutexLocker lock(&published_mutex_);
    published_=std::move(next);
}
VideoReceiver::Published VideoReceiver::published() const {
    QMutexLocker lock(&published_mutex_);
    return published_;
}
quint16 VideoReceiver::port() const { return published().port; }
QByteArray VideoReceiver::latestFrame() const {
    QMutexLocker lock(&published_mutex_);
    return published_.latest;
}
int VideoReceiver::frameWidth() const {
    QMutexLocker lock(&published_mutex_);
    return published_.width;
}
int VideoReceiver::frameHeight() const {
    QMutexLocker lock(&published_mutex_);
    return published_.height;
}
bool VideoReceiver::online() const {
    QMutexLocker lock(&published_mutex_);
    return published_.online;
}
QString VideoReceiver::state() const {
    QMutexLocker lock(&published_mutex_);
    return published_.state;
}
qint64 VideoReceiver::decodedFrames() const {
    QMutexLocker lock(&published_mutex_);
    return published_.decoded_frames;
}
quint64 VideoReceiver::packetsReceived() const {
    QMutexLocker lock(&published_mutex_);
    return published_.stats.packets_received;
}
qint64 VideoReceiver::msSinceLastFrame() const {
    QMutexLocker lock(&published_mutex_);
    return published_.last_frame<0 ? -1 : clock_.elapsed()-published_.last_frame;
}
QJsonObject VideoReceiver::snapshot() const {
    const Published p=published();
    const auto& s=p.stats;
    return {{"state",p.state},{"failure",p.failure},{"online",p.online},
        {"port",p.port},{"decoder_pid",p.decoder_pid},
        {"decoded_frames",p.decoded_frames},{"decoder_failures",p.failures},
        {"recoveries",p.reconnects+p.decoder_restarts},
        {"reconnects",p.reconnects},
        {"decoder_restarts",p.decoder_restarts},
        {"invalid_frames",p.invalid_frames},{"decoder_stderr_bytes",p.stderr_bytes},
        {"packets",qint64(s.packets_received)},{"completed",qint64(s.frames_completed)},
        {"duplicates",qint64(s.packets_duplicated)},{"out_of_order",qint64(s.packets_out_of_order)},
        {"malformed",qint64(s.malformed_packets)},{"stale_packets",qint64(s.stale_packets)},
        {"missing_packets",qint64(s.packets_missing)},{"incomplete",qint64(s.frames_dropped_incomplete)},
        {"expired",qint64(s.frames_dropped_stale)},{"cached_frames",p.cached_frames},
        {"raw_bytes",p.raw_bytes},{"queued_bytes",p.queued_bytes},
        {"frame_bytes",p.latest.size()},
        {"frame_sha256",QString::fromLatin1(QCryptographicHash::hash(p.latest,QCryptographicHash::Sha256).toHex())}};
}
}
