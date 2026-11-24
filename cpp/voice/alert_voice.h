#pragma once

#include "speech_backend.h"

#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

#include <deque>

namespace rm_terminal {

// 事件提醒的语音准备:赛前把所有固定句子合成好放进缓存目录,到点只放文件。
//
// 用一个只合成、不播放的独立后端实例:战术提醒那边的合成队列一次只走一条,和它挤在
// 同一个进程槽位里会互相杀掉对方正在合成的句子。播放仍交给 ReminderController,
// 保证同一时刻只有一个声音。
class AlertVoice : public QObject {
    Q_OBJECT
public:
    explicit AlertVoice(SpeechBackend* synth_backend, QObject* parent = nullptr);

    // 缓存命中的句子直接算就绪,其余排队串行合成。
    void prepare(const QString& cache_dir, const QStringList& phrases, const QString& voice,
                 int rate_wpm);

    // 已合成好的音频文件;还没准备好(或这台电脑没有语音)时返回空,调用方只出横幅。
    QString pathFor(const QString& phrase) const;
    int readyCount() const { return static_cast<int>(ready_.size()); }

private:
    void startNext();
    QString cacheFile(const QString& phrase) const;

    SpeechBackend* backend_;
    QString cache_dir_;
    QString voice_key_;
    std::deque<QString> pending_;
    QString in_flight_;
    quint64 token_ = 0;
    quint64 next_token_ = 1;
    QSet<QString> ready_;
};

}  // namespace rm_terminal
