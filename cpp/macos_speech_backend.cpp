#include "macos_speech_backend.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStringList>
#include <QTemporaryFile>
#include <algorithm>

namespace rm_terminal {
namespace {

constexpr auto kSay = "/usr/bin/say";
constexpr auto kPlay = "/usr/bin/afplay";
// 合成一句 200 字的中文远低于这个上限。看门狗到点说明 say 卡住了,必须报失败,
// 而不是让准备状态永远停在 Preparing。
constexpr int kSynthesizeTimeoutMs = 20000;
// 一句 200 字的中文读完远不到这个时长。afplay 卡住时必须放行队列,
// 否则后面所有战术提醒都会被一个僵死的进程堵掉。
constexpr int kPlaybackTimeoutMs = 60000;
// 探测只在启动和赛前改设置时跑,单次合成两段短文本。给足余量但不能无限等,
// 否则一个卡住的 say 会让启动永远停住。
constexpr int kVoiceProbeTimeoutMs = 8000;

}

MacosSpeechBackend::MacosSpeechBackend(QObject* parent, QString voice, int rate_wpm)
    : SpeechBackend(parent), voice_(std::move(voice)), rate_wpm_(clampRate(rate_wpm)) {
    const auto installed =
        QFileInfo::exists(QString::fromLatin1(kSay)) && QFileInfo::exists(QString::fromLatin1(kPlay));
    // 只枚举,不逐个做合成探测:探测整张列表实测要 9.8 秒,启动阶段卡这么久不可接受。
    // 枚举保留完整名字,而完整名字的 zh_CN 声音实测都能正常合成;真正可能不出声的
    // 是操作手手动切换到的声音,那一个在 setVoiceAndRate 里单独探测。
    const auto available_voices = installed ? chineseVoices() : QStringList();
    voices_ = available_voices;
    if (voice_.isEmpty()) voice_ = preferredVoice(available_voices);

    if (!installed) {
        unavailable_ = QStringLiteral("系统缺少 say 或 afplay，无法生成离线语音");
    } else if (available_voices.isEmpty()) {
        unavailable_ = QStringLiteral(
            "系统没有可用的中文语音。请在「系统设置 → 辅助功能 → 朗读内容 → 系统声音」"
            "中添加一个中文（简体）声音后重启终端。");
    } else if (!available_voices.contains(voice_)) {
        // say 对不在列表里的声音**不报错**,而是静默回退到系统默认(英文)声音并以 0
        // 退出。退出码会把这种情况判为成功,所以必须在这里自己挡掉 —— 否则到点播出的
        // 是英文音节。
        //
        // 裸名(`Eddy`)正属于这一类:它匹配的是同名英文声音,不是中文的
        // `Eddy (中文（中国大陆）)`。列表里保留完整名字就是为了让这个比较能挡住它。
        unavailable_ = QStringLiteral("声音「%1」无法离线合成中文，可用的有：%2")
                           .arg(voice_, available_voices.join(QStringLiteral("、")));
    }

    synth_watchdog_.setSingleShot(true);
    play_watchdog_.setSingleShot(true);
    QObject::connect(&synth_watchdog_, &QTimer::timeout, this, [this] {
        discard(synth_);
        finishSynthesis(false, QStringLiteral("say 超过 %1 秒未完成，已放弃")
                                   .arg(kSynthesizeTimeoutMs / 1000));
    });
    QObject::connect(&play_watchdog_, &QTimer::timeout, this, [this] {
        discard(play_);
        finishPlayback(false, QStringLiteral("afplay 超过 %1 秒未完成，已放弃")
                                  .arg(kPlaybackTimeoutMs / 1000));
    });
}

QStringList MacosSpeechBackend::chineseVoices() {
    // 只在构造时枚举一次(实测 0.44 秒),结果决定 available()。这里的阻塞等待是
    // 刻意的:available() 必须同步可答,而这一次发生在启动阶段,不在比赛中。
    QProcess probe;
    probe.start(QString::fromLatin1(kSay), {QStringLiteral("-v"), QStringLiteral("?")});
    if (!probe.waitForFinished(5000)) {
        probe.kill();
        probe.waitForFinished(1000);
        return {};
    }
    QStringList voices;
    const auto listing = QString::fromUtf8(probe.readAllStandardOutput());
    for (const auto& line : listing.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        // 每行形如 `Tingting            zh_CN    # 你好…`。只认 zh_CN:zh_TW 是
        // 繁体台湾音,读简体战术词会有明显的用词和口音差异。
        if (!line.contains(QStringLiteral("zh_CN"))) continue;
        const auto name = line.section(QStringLiteral("zh_CN"), 0, 0).trimmed();
        if (!name.isEmpty() && !voices.contains(name)) voices.append(name);
    }
    return voices;
}

int MacosSpeechBackend::clampRate(int rate_wpm) {
    return std::clamp(rate_wpm, kMinReminderRateWpm, kMaxReminderRateWpm);
}

QString MacosSpeechBackend::preferredVoice(const QStringList& voices) {
    // (Enhanced) 是同一个发音人的高音质版本,系统里两者并存时列表顺序不保证哪个在前。
    // 优先挑它:战术播报要在场地噪声里被听清,而普通版实测明显更闷。
    for (const auto& name : voices)
        if (name.contains(QStringLiteral("(Enhanced)"))) return name;
    return voices.value(0);
}

bool MacosSpeechBackend::voiceCanSynthesize(const QString& voice) {
    // 判据是「时长随文本增长」而不是「文件非空」:空音频也有 4800 字节的文件头,
    // 单看大小或退出码都会把它当成功。两段文本长度差约 10 倍,可用声音的时长差
    // 必然远超测量噪声。
    const auto measure = [&voice](const QString& text) -> qint64 {
        QTemporaryFile out(QDir::tempPath() + QStringLiteral("/rm_voice_probe_XXXXXX.aiff"));
        if (!out.open()) return -1;
        const auto path = out.fileName();
        out.close();
        QProcess say;
        say.start(QString::fromLatin1(kSay),
                  {QStringLiteral("-v"), voice, QStringLiteral("-o"), path, text});
        if (!say.waitForFinished(kVoiceProbeTimeoutMs)) {
            say.kill();
            say.waitForFinished(1000);
            return -1;
        }
        if (say.exitStatus() != QProcess::NormalExit || say.exitCode() != 0) return -1;
        return QFileInfo(path).size();
    };

    const auto brief = measure(QStringLiteral("推进"));
    if (brief <= 0) return false;
    const auto lengthy = measure(QStringLiteral("全体推进高地注意左翼敌方工程车正在修复"));
    if (lengthy <= 0) return false;
    return lengthy > brief * 2;
}

bool MacosSpeechBackend::setVoiceAndRate(const QString& voice, int rate_wpm) {
    const auto next_rate = clampRate(rate_wpm);
    if (voice == voice_) {
        // 声音没变,但后端可能本来就不可用(构造时一个中文声音都没挑到,voice_ 是空的)。
        // 这条路径原本无条件返回 true,于是「只改语速」会被报成功,到点却是静默。
        if (!available()) return false;
        rate_wpm_ = next_rate;
        return true;
    }
    // 只接受枚举里的 zh_CN 完整名。say 对列表外的名字**不报错**,而是静默回退到系统
    // 默认(英文)声音并以 0 退出 —— 那样探测也会「通过」,到点播出的却是英文音节。
    if (!voices_.contains(voice)) return false;
    if (!voiceCanSynthesize(voice)) return false;
    voice_ = voice;
    rate_wpm_ = next_rate;
    unavailable_.clear();
    return true;
}

bool MacosSpeechBackend::available() const { return unavailable_.isEmpty(); }

QString MacosSpeechBackend::unavailableReason() const { return unavailable_; }

void MacosSpeechBackend::discard(QProcess*& slot) {
    if (!slot) return;
    auto* process = slot;
    slot = nullptr;
    // 先断开:kill() 是异步的,不断开的话那个迟到的退出信号会被记到下一次操作的
    // 令牌上,让刚开始的一句立刻被当成已经结束。
    process->disconnect(this);
    if (process->state() == QProcess::NotRunning) {
        process->deleteLater();
        return;
    }
    QObject::connect(process, &QProcess::finished, process, &QProcess::deleteLater);
    process->kill();
}

void MacosSpeechBackend::finishSynthesis(bool ok, const QString& error) {
    // 令牌归零 = 这次操作已经完成过。finished 和 errorOccurred 可能都会来,
    // 发两次会让上层把同一条提醒排两遍第二遍。
    if (synth_token_ == 0) return;
    const auto token = synth_token_;
    synth_token_ = 0;
    synth_watchdog_.stop();
    Q_EMIT synthesizeFinished(token, ok, error);
}

void MacosSpeechBackend::finishPlayback(bool ok, const QString& error) {
    if (play_token_ == 0) return;
    const auto token = play_token_;
    play_token_ = 0;
    play_watchdog_.stop();
    Q_EMIT playbackFinished(token, ok, error);
}

void MacosSpeechBackend::synthesize(const QString& text, const QString& path, quint64 token) {
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
                         finishSynthesis(ok, ok ? QString()
                                                : QStringLiteral("say 退出码 %1").arg(code));
                     });
    QObject::connect(synth_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        finishSynthesis(false, QStringLiteral("say 执行失败: %1").arg(synth_->errorString()));
    });
    synth_watchdog_.start(kSynthesizeTimeoutMs);
    // 参数数组:文本作为独立 argv 元素传入,shell 元字符不会被解释。
    synth_->start(QString::fromLatin1(kSay),
                  {QStringLiteral("-v"), voice_, QStringLiteral("-r"),
                   QString::number(rate_wpm_), QStringLiteral("-o"), path, text});
}

void MacosSpeechBackend::play(const QString& path, quint64 token) {
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
                                               : QStringLiteral("afplay 退出码 %1").arg(code));
                     });
    QObject::connect(play_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        finishPlayback(false, QStringLiteral("afplay 执行失败: %1").arg(play_->errorString()));
    });
    play_watchdog_.start(kPlaybackTimeoutMs);
    play_->start(QString::fromLatin1(kPlay), {path});
}

void MacosSpeechBackend::stop() {
    discard(play_);
    // 被主动停止不是一次「播完」。清掉令牌,于是不会有完成回调去触发第二遍 ——
    // 关总开关之后再响一声正是操作手最不能接受的。
    play_token_ = 0;
    play_watchdog_.stop();
}

}
