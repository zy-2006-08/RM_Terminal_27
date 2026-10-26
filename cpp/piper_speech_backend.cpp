#include "piper_speech_backend.h"

#include "tactical_reminder.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

#include <algorithm>

namespace rm_terminal {
namespace {

// 一句话合成实测约 1.7 秒,大半是加载模型。到点卡住必须报失败,不能让准备状态一直挂着。
constexpr int kSynthesizeTimeoutMs = 30000;
constexpr int kPlaybackTimeoutMs = 60000;
// 默认语速对应的 Piper 语速系数。操作手试听后选的是「快一点」那一档。
constexpr double kDefaultLengthScale = 0.8;

}  // namespace

PiperSpeechBackend::PiperSpeechBackend(const QString& install_dir, const QString& script,
                                       QObject* parent)
    : SpeechBackend(parent), script_(script), rate_wpm_(kDefaultReminderRateWpm) {
    const QString dir = install_dir.isEmpty()
                            ? QDir::home().filePath(QStringLiteral(".local/share/rm_terminal/tts"))
                            : install_dir;
    python_ = QDir(dir).filePath(QStringLiteral("venv/bin/python"));
    model_ = QDir(dir).filePath(QStringLiteral("voices/zh_CN-chaowen-medium.onnx"));
    for (const auto* candidate : {"pw-play", "paplay", "aplay"}) {
        player_ = QStandardPaths::findExecutable(QString::fromLatin1(candidate));
        if (!player_.isEmpty()) break;
    }
    if (!QFileInfo::exists(python_) || !QFileInfo::exists(model_))
        unavailable_ = QStringLiteral("没装离线语音：运行 bash scripts/setup_voice.sh");
    else if (!QFileInfo::exists(script_))
        unavailable_ = QStringLiteral("找不到合成脚本 %1").arg(script_);
    else if (player_.isEmpty())
        unavailable_ = QStringLiteral("找不到播放程序（pw-play / paplay / aplay）");

    synth_watchdog_.setSingleShot(true);
    play_watchdog_.setSingleShot(true);
    QObject::connect(&synth_watchdog_, &QTimer::timeout, this, [this] {
        discard(synth_);
        finishSynthesis(false, QStringLiteral("语音合成超过 %1 秒未完成，已放弃")
                                   .arg(kSynthesizeTimeoutMs / 1000));
    });
    QObject::connect(&play_watchdog_, &QTimer::timeout, this, [this] {
        discard(play_);
        finishPlayback(false, QStringLiteral("播放超过 %1 秒未完成，已放弃")
                                  .arg(kPlaybackTimeoutMs / 1000));
    });
}

PiperSpeechBackend::~PiperSpeechBackend() {
    discard(synth_);
    discard(play_);
}

bool PiperSpeechBackend::setVoiceAndRate(const QString& voice, int rate_wpm) {
    if (!available()) return false;
    if (!voice.isEmpty() && voice != this->voice()) return false;
    rate_wpm_ = std::clamp(rate_wpm, kMinReminderRateWpm, kMaxReminderRateWpm);
    return true;
}

QString PiperSpeechBackend::lengthScale() const {
    // 语速越快,系数越小。225 是默认语速。
    const double scale = kDefaultLengthScale * kDefaultReminderRateWpm / rate_wpm_;
    return QString::number(std::clamp(scale, 0.5, 1.5), 'f', 2);
}

void PiperSpeechBackend::discard(QProcess*& slot) {
    if (!slot) return;
    auto* process = slot;
    slot = nullptr;
    // 先断开:kill() 是异步的,迟到的退出信号会被记到下一次操作的令牌上。
    process->disconnect(this);
    if (process->state() == QProcess::NotRunning) {
        process->deleteLater();
        return;
    }
    QObject::connect(process, &QProcess::finished, process, &QProcess::deleteLater);
    process->kill();
}

void PiperSpeechBackend::finishSynthesis(bool ok, const QString& error) {
    if (synth_token_ == 0) return;
    const auto token = synth_token_;
    synth_token_ = 0;
    synth_watchdog_.stop();
    Q_EMIT synthesizeFinished(token, ok, error);
}

void PiperSpeechBackend::finishPlayback(bool ok, const QString& error) {
    if (play_token_ == 0) return;
    const auto token = play_token_;
    play_token_ = 0;
    play_watchdog_.stop();
    Q_EMIT playbackFinished(token, ok, error);
}

void PiperSpeechBackend::synthesize(const QString& text, const QString& path, quint64 token) {
    discard(synth_);
    synth_token_ = token;
    if (!available()) {
        finishSynthesis(false, unavailable_);
        return;
    }
    synth_ = new QProcess(this);
    QObject::connect(synth_, &QProcess::finished, this,
                     [this](int code, QProcess::ExitStatus status) {
                         const bool ok = status == QProcess::NormalExit && code == 0;
                         const QString detail = ok ? QString()
                                                   : QString::fromUtf8(synth_->readAllStandardError())
                                                         .trimmed()
                                                         .section(QLatin1Char('\n'), -1);
                         finishSynthesis(ok, ok ? QString()
                                                : QStringLiteral("语音合成失败（%1）%2")
                                                      .arg(code)
                                                      .arg(detail));
                     });
    QObject::connect(synth_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        finishSynthesis(false, QStringLiteral("语音合成启动失败：%1").arg(synth_->errorString()));
    });
    synth_watchdog_.start(kSynthesizeTimeoutMs);
    synth_->start(python_, {script_, QStringLiteral("--model"), model_, QStringLiteral("--output"),
                            path, QStringLiteral("--length-scale"), lengthScale(),
                            QStringLiteral("--"), text});
}

void PiperSpeechBackend::play(const QString& path, quint64 token) {
    discard(play_);
    play_token_ = token;
    if (!available()) {
        finishPlayback(false, unavailable_);
        return;
    }
    if (!QFileInfo::exists(path)) {
        finishPlayback(false, QStringLiteral("音频文件缺失: %1").arg(path));
        return;
    }
    play_ = new QProcess(this);
    QObject::connect(play_, &QProcess::finished, this,
                     [this](int code, QProcess::ExitStatus status) {
                         const bool ok = status == QProcess::NormalExit && code == 0;
                         finishPlayback(ok, ok ? QString()
                                               : QStringLiteral("播放退出码 %1").arg(code));
                     });
    QObject::connect(play_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        finishPlayback(false, QStringLiteral("播放启动失败：%1").arg(play_->errorString()));
    });
    play_watchdog_.start(kPlaybackTimeoutMs);
    play_->start(player_, {path});
}

void PiperSpeechBackend::stop() {
    discard(play_);
    // 被主动停止不是一次「播完」:清掉令牌,不发完成回调,否则会触发第二遍。
    play_token_ = 0;
    play_watchdog_.stop();
}

}  // namespace rm_terminal
