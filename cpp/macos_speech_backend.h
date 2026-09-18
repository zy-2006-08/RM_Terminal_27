#pragma once

#include "speech_backend.h"
#include "tactical_reminder.h"

#include <QString>
#include <QStringList>
#include <QTimer>

class QProcess;

namespace rm_terminal {

// macOS 本机离线语音:`say -v <voice> -o <file> <text>` 合成,`afplay <file>` 播放。
// 两者都在系统内完成,不发任何网络请求。
//
// 全部走 QProcess 的**参数数组**形式,绝不拼 shell 命令行:提醒文本由操作手输入,
// 拼字符串会让一句含引号或反引号的战术变成可执行命令。
//
// synthesize/play/stop 全程不阻塞:它们在 GUI 线程上被调用,而合成一句中文要几百
// 毫秒。比赛中卡住界面是不可接受的,所以完成一律通过信号异步回来。
//
// 阻塞等待只存在于两个 static 探测(chineseVoices / voiceCanSynthesize),它们分别
// 发生在构造和赛前主动切换声音时 —— 都不在比赛计时中,且 available() 必须同步可答。
class MacosSpeechBackend : public SpeechBackend {
    Q_OBJECT
public:
    // 未指定声音时自动挑一个 zh_CN 声音。挑不到就是 unavailable ——
    // 用英文声音念中文只会得到无法听懂的音节。
    explicit MacosSpeechBackend(QObject* parent = nullptr, QString voice = {},
                                int rate_wpm = kDefaultReminderRateWpm);

    bool available() const override;
    QString unavailableReason() const override;
    QString voice() const override { return voice_; }

    // 赛前改声音/语速。返回 false 表示这个声音不在系统枚举里、或实测合成不出声,
    // 已拒绝并保留原设置。
    bool setVoiceAndRate(const QString& voice, int rate_wpm) override;
    QStringList availableVoices() const override { return voices_; }
    int rateWpm() const { return rate_wpm_; }
    void synthesize(const QString& text, const QString& path, quint64 token) override;
    void play(const QString& path, quint64 token) override;
    void stop() override;

    // 系统里列出的 zh_CN 声音,`say -v '?'` 的解析结果,保留**完整名字**。
    // 完整名字是必须的:`Eddy` 这样的裸名会匹配到同名的英文声音,合成出一个与
    // 中文文本无关的 0.016 秒空音频且退出码为 0;`Eddy (中文（中国大陆）)` 则正常。
    static QStringList chineseVoices();

    // 单个声音是否真能出声。判据是同一声音念长短两段文本,输出**文件大小**必须
    // 随文本增长:空音频也有几 KB 的文件头,只看大小或退出码都会把静默当成功。
    //
    // 一次调用要跑两遍 say,实测约 0.7~1.1 秒。所以只在赛前切换声音时探测**这一个**
    // 声音,绝不在构造里遍历整张列表 —— 那是 11 个声音 22 次 say,实测 9.8 秒。
    static bool voiceCanSynthesize(const QString& voice);

    // 未指定声音时从枚举里挑默认那一个。优先 (Enhanced):同一发音人的高音质版本,
    // 战术播报要在场地噪声里被听清。系统里两个版本并存时列表顺序不保证哪个在前。
    static QString preferredVoice(const QStringList& voices);

private:
    // 主动放弃一个子进程:**先断开信号再杀**。不断开就无法区分「正常播完」和
    // 「被我们杀掉」,而 kill 是异步的 —— 那个迟到的退出信号会被记到下一次操作的
    // 令牌上,让刚开始的一句立刻被当成已经结束。
    void discard(QProcess*& slot);

    // 一次操作只允许完成一次。finished 和 errorOccurred 可能都会来,
    // 两次都发信号会让上层为同一条提醒排两遍第二遍。
    void finishSynthesis(bool ok, const QString& error);
    void finishPlayback(bool ok, const QString& error);

    // 合成和播放各自独立的进程槽位:第二遍的重复播放不该被上一次合成的
    // 生命周期干扰。
    QProcess* synth_ = nullptr;
    QProcess* play_ = nullptr;
    QTimer synth_watchdog_;
    QTimer play_watchdog_;
    static int clampRate(int rate_wpm);

    QString voice_;
    int rate_wpm_;
    // 构造时枚举一次的 zh_CN 声音列表(单次 say -v '?',实测 0.44 秒)。
    // 不在这里逐个做合成探测:那是 22 次 say、实测 9.8 秒的启动卡顿,而实测表明
    // 完整名字的 zh_CN 声音都能正常合成 —— 真正需要探测的只有操作手主动切换的那一个。
    QStringList voices_;
    QString unavailable_;
    quint64 synth_token_ = 0;
    quint64 play_token_ = 0;
};

}
