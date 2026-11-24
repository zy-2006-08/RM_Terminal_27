#include "alert_voice.h"

#include "logging.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>

namespace rm_terminal {

AlertVoice::AlertVoice(SpeechBackend* synth_backend, QObject* parent)
    : QObject(parent), backend_(synth_backend) {
    QObject::connect(backend_, &SpeechBackend::synthesizeFinished, this,
                     [this](quint64 token, bool ok, const QString& error) {
                         if (token == 0 || token != token_) return;
                         token_ = 0;
                         if (ok) {
                             ready_.insert(in_flight_);
                         } else {
                             StructuredLog::write(LogLevel::warning, QStringLiteral("alert_voice"),
                                                  {QStringLiteral("state=synth_failed"),
                                                   QStringLiteral("phrase=%1").arg(in_flight_),
                                                   QStringLiteral("error=%1").arg(error)});
                         }
                         startNext();
                     });
}

QString AlertVoice::cacheFile(const QString& phrase) const {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(voice_key_.toUtf8());
    hash.addData(QByteArrayLiteral("\x1f"));
    hash.addData(phrase.toUtf8());
    // 扩展名与战术提醒的缓存一致。Linux 的 Piper 写的是 WAV,播放程序按文件头识别格式。
    return QDir(cache_dir_).filePath(QStringLiteral("alert-%1.aiff")
                                         .arg(QString::fromLatin1(hash.result().toHex())));
}

void AlertVoice::prepare(const QString& cache_dir, const QStringList& phrases, const QString& voice,
                         int rate_wpm) {
    cache_dir_ = cache_dir;
    pending_.clear();
    ready_.clear();
    token_ = 0;
    if (!backend_->available()) return;
    if (!backend_->setVoiceAndRate(voice, rate_wpm)) {
        StructuredLog::write(LogLevel::warning, QStringLiteral("alert_voice"),
                             {QStringLiteral("state=voice_rejected"),
                              QStringLiteral("voice=%1").arg(voice)});
        return;
    }
    voice_key_ = QStringLiteral("%1@%2").arg(backend_->voice()).arg(rate_wpm);
    QDir().mkpath(cache_dir_);
    for (const QString& phrase : phrases) {
        if (QFileInfo(cacheFile(phrase)).size() > 0)
            ready_.insert(phrase);
        else
            pending_.push_back(phrase);
    }
    StructuredLog::write(LogLevel::info, QStringLiteral("alert_voice"),
                         {QStringLiteral("state=preparing"),
                          QStringLiteral("cached=%1").arg(ready_.size()),
                          QStringLiteral("to_synthesize=%1").arg(pending_.size())});
    if (pending_.empty()) {
        StructuredLog::write(LogLevel::info, QStringLiteral("alert_voice"),
                             {QStringLiteral("state=ready"),
                              QStringLiteral("phrases=%1").arg(ready_.size())});
        return;
    }
    startNext();
}

void AlertVoice::startNext() {
    if (pending_.empty()) {
        if (!in_flight_.isEmpty())
            StructuredLog::write(LogLevel::info, QStringLiteral("alert_voice"),
                                 {QStringLiteral("state=ready"),
                                  QStringLiteral("phrases=%1").arg(ready_.size())});
        in_flight_.clear();
        return;
    }
    in_flight_ = pending_.front();
    pending_.pop_front();
    token_ = next_token_++;
    backend_->synthesize(in_flight_, cacheFile(in_flight_), token_);
}

QString AlertVoice::pathFor(const QString& phrase) const {
    return ready_.contains(phrase) ? cacheFile(phrase) : QString();
}

}  // namespace rm_terminal
