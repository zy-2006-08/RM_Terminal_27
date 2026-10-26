#pragma once

#include "speech_backend.h"

#include <QString>
#include <QStringList>
#include <QTimer>

class QProcess;

namespace rm_terminal {

// Linux 的离线中文语音:Piper「超文」+ pypinyin,由 scripts/setup_voice.sh 安装。
// 合成走 scripts/tts_say.py,播放用 pw-play / paplay / aplay 里先找到的那个。
// 进程参数全部按数组传,不拼 shell 命令行:提醒文本是操作手输入的。
class PiperSpeechBackend : public SpeechBackend {
    Q_OBJECT
public:
    // `install_dir` 为空时用 ~/.local/share/rm_terminal/tts;`script` 是 tts_say.py 的路径。
    PiperSpeechBackend(const QString& install_dir, const QString& script,
                       QObject* parent = nullptr);
    ~PiperSpeechBackend() override;

    bool available() const override { return unavailable_.isEmpty(); }
    QString unavailableReason() const override { return unavailable_; }
    QString voice() const override { return QStringLiteral("超文"); }
    QStringList availableVoices() const override { return {voice()}; }
    bool setVoiceAndRate(const QString& voice, int rate_wpm) override;
    void synthesize(const QString& text, const QString& path, quint64 token) override;
    void play(const QString& path, quint64 token) override;
    void stop() override;

private:
    void discard(QProcess*& slot);
    void finishSynthesis(bool ok, const QString& error);
    void finishPlayback(bool ok, const QString& error);
    QString lengthScale() const;

    QString python_;
    QString script_;
    QString model_;
    QString player_;
    QString unavailable_;
    int rate_wpm_;
    QProcess* synth_ = nullptr;
    QProcess* play_ = nullptr;
    QTimer synth_watchdog_;
    QTimer play_watchdog_;
    quint64 synth_token_ = 0;
    quint64 play_token_ = 0;
};

}  // namespace rm_terminal
