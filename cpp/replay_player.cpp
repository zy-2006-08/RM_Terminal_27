#include "replay_player.h"

#include "clock.h"
#include "decoder.h"
#include "store.h"
#include "video_receiver.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace rm_terminal {
namespace {

// 跳转后 ffmpeg 需要从关键帧重新起解。拖到任意时刻时补喂目标前 2 秒的图传,
// 目标画面通常就能在首次显示时出现,而不是等下一个关键帧。
constexpr double kVideoPrerollUs = 2'000'000.0;

QString clock_text(qint64 milliseconds) {
    const qint64 seconds = std::max<qint64>(0, milliseconds) / 1000;
    return QStringLiteral("%1:%2")
        .arg(seconds / 60, 2, 10, QLatin1Char('0'))
        .arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

}  // namespace

ReplayPlayer::ReplayPlayer(Store& store, VideoReceiver& video, QObject* parent)
    : QObject(parent), store_(store), video_(video) {
    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, &ReplayPlayer::pump);
}

ReplayPlayer::~ReplayPlayer() { stop(); }

bool ReplayPlayer::start(const QString& path, QString* error) {
    stop();
    path_ = path;
    records_ = 0;
    position_us_ = 0.0;
    duration_us_ = 0;
    speed_ = 1.0;
    paused_ = false;
    finished_ = false;
    error_.clear();
    clock_.invalidate();

    if (!reader_.open(path_, &error_)) {
        if (error) *error = error_;
        return false;
    }
    self_id_ = reader_.metadata().value(QStringLiteral("self_robot_id")).toInt();

    // 时长只在开播时扫一遍记录头,不解码,11MB 录像约几十毫秒。播放本身仍按
    // 流式读取,不把整份录像载入内存。
    RecordingReader scan;
    if (!scan.open(path_, &error_)) {
        if (error) *error = error_;
        return false;
    }
    RecordingRecord record;
    while (scan.next(&record)) duration_us_ = static_cast<qint64>(record.time_us);
    // 终端被 kill 时最后一条只写了一半;播到最后一条完整记录即可,不能整份拒绝。
    truncated_tail_ = scan.truncatedTail();
    if (!scan.error().isEmpty() && !truncated_tail_) {
        if (error) *error = scan.error();
        return false;
    }
    if (!reader_.open(path_, &error_)) {
        if (error) *error = error_;
        return false;
    }

    decoder_ = std::make_unique<Decoder>(
        store_, RobotId{static_cast<std::uint32_t>(self_id_)});
    active_ = true;
    emitProgress();
    Q_EMIT pausedChanged(false);
    pump();
    return true;
}

void ReplayPlayer::stop() {
    timer_.stop();
    active_ = false;
    paused_ = false;
    pending_.reset();
    decoder_.reset();
    clock_.invalidate();
}

bool ReplayPlayer::readNext() {
    RecordingRecord record;
    if (reader_.next(&record, &error_)) {
        pending_ = std::move(record);
        return true;
    }

    pending_.reset();
    if (reader_.truncatedTail()) error_.clear();
    if (error_.isEmpty() && reader_.atEnd()) {
        finished_ = true;
        active_ = false;
        paused_ = false;
        position_us_ = static_cast<double>(duration_us_);
    } else if (error_.isEmpty()) {
        error_ = QStringLiteral("回放读取失败");
    }
    if (!error_.isEmpty()) active_ = false;
    return false;
}

void ReplayPlayer::pump() {
    if (!active_ || paused_) return;

    if (clock_.isValid()) {
        // 毫秒精度在这里会丢时间:记录密集时一次 tick 常不足 1ms,整数毫秒截断
        // 会把每次的零头都扔掉,播放随之越来越慢。
        const qint64 elapsed_ns = clock_.nsecsElapsed();
        clock_.restart();
        position_us_ += static_cast<double>(elapsed_ns) / 1000.0 * speed_;
    } else {
        clock_.start();
    }

    if (!pending_ && !readNext()) {
        emitProgress();
        Q_EMIT pausedChanged(true);
        return;
    }

    while (pending_ && static_cast<double>(pending_->time_us) <= position_us_) {
        deliver(*pending_);
        pending_.reset();
        if (!readNext()) break;
    }
    if (!active_) {
        emitProgress();
        Q_EMIT pausedChanged(true);
        return;
    }
    if (!pending_) return;

    const double wait_ms =
        (static_cast<double>(pending_->time_us) - position_us_) / (1000.0 * speed_);
    // 下限 1ms:目标就在眼前时不能让 0ms 定时器自旋,否则主线程空转。
    timer_.start(static_cast<int>(std::clamp(std::ceil(wait_ms), 1.0, 50.0)));
    emitProgress();
}

void ReplayPlayer::deliver(const RecordingRecord& record) {
    ++records_;
    if (record.kind == RecordingRecord::Kind::Video) {
        video_.injectDatagram(record.payload);
        return;
    }
    decoder_->accept(record.topic.toStdString(),
                     std::string(record.payload.constData(),
                                 static_cast<std::size_t>(record.payload.size())),
                     monotonic_now());
}

void ReplayPlayer::emitProgress() {
    Q_EMIT progress(positionMs(), durationMs());
}

void ReplayPlayer::togglePause() {
    if (finished_) {
        seek(0);
        return;
    }
    if (!active_) return;

    paused_ = !paused_;
    if (paused_) {
        timer_.stop();
    } else {
        clock_.start();
        pump();
    }
    Q_EMIT pausedChanged(paused_);
}

void ReplayPlayer::seek(qint64 position_ms) {
    if (path_.isEmpty() || (!active_ && !finished_)) return;

    timer_.stop();
    const double target_us =
        static_cast<double>(std::clamp<qint64>(position_ms, 0, durationMs())) * 1000.0;

    // 重置到零再喂一遍。只重喂目标之前的记录会让目标之后才更新的字段继续留在
    // Store 里,画面就会提前显示未来的值。
    store_.reset();
    decoder_ = std::make_unique<Decoder>(
        store_, RobotId{static_cast<std::uint32_t>(self_id_)});
    video_.stop();
    video_.startInjected();

    if (!reader_.open(path_, &error_)) {
        active_ = false;
        emitProgress();
        return;
    }
    pending_.reset();
    RecordingRecord record;
    while (reader_.next(&record)) {
        if (static_cast<double>(record.time_us) > target_us) {
            pending_ = std::move(record);
            break;
        }
        // 图传只补喂临近目标的一小段,整段解码会拖慢拖动响应。
        if (record.kind == RecordingRecord::Kind::Mqtt ||
            static_cast<double>(record.time_us) + kVideoPrerollUs >= target_us) {
            if (record.kind == RecordingRecord::Kind::Video) {
                video_.injectDatagram(record.payload);
            } else {
                decoder_->accept(
                    record.topic.toStdString(),
                    std::string(record.payload.constData(),
                                static_cast<std::size_t>(record.payload.size())),
                    monotonic_now());
            }
        }
    }

    position_us_ = target_us;
    finished_ = false;
    active_ = true;
    emitProgress();
    Q_EMIT pausedChanged(paused_);
    if (!paused_) {
        clock_.start();
        pump();
    }
}

void ReplayPlayer::setSpeed(double speed) {
    if (speed <= 0.0) return;
    speed_ = speed;
}

qint64 ReplayPlayer::positionMs() const {
    return static_cast<qint64>(std::llround(position_us_ / 1000.0));
}

qint64 ReplayPlayer::durationMs() const { return (duration_us_ + 999) / 1000; }

QString ReplayPlayer::statusText() const {
    if (!error_.isEmpty()) return QStringLiteral("回放失败：%1").arg(error_);
    if (!active_ && !finished_) return QString();
    if (finished_) {
        return QStringLiteral("▶ 回放结束 %1 · %2 条%3")
            .arg(clock_text(durationMs()))
            .arg(records_)
            .arg(truncated_tail_ ? QStringLiteral(" · 录像结尾不完整") : QString());
    }
    return QStringLiteral("%1 回放 %2 / %3 · %4x")
        .arg(paused_ ? QStringLiteral("⏸") : QStringLiteral("▶"),
             clock_text(positionMs()), clock_text(durationMs()))
        .arg(speed_);
}

}  // namespace rm_terminal
