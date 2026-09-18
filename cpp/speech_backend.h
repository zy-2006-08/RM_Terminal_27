#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <utility>

namespace rm_terminal {

// 语音后端的平台边界。生成和播放都是**异步**的:合成一句中文要几百毫秒,
// 在 GUI 线程里同步等会让整个终端卡住 —— 比赛中卡顿是不可接受的。
//
// 触发时只播放已经落盘的文件,绝不临时合成:到点才开始合成会让播报晚于阈值。
class SpeechBackend : public QObject {
    Q_OBJECT
public:
    explicit SpeechBackend(QObject* parent = nullptr) : QObject(parent) {}

    // 平台是否提供已核实的本机中文离线语音。false 时必须明确告知没有语音,
    // 不能假装可用然后到点静默。
    virtual bool available() const = 0;

    // 不可用的原因,面向操作手且可操作(例如「系统缺少中文语音,请在系统设置
    // 中添加」),而不是内部错误码。
    virtual QString unavailableReason() const = 0;

    // 实际使用的声音名。上层把它算进缓存文件名,所以换声音会得到新文件而不会
    // 播出上一个声音的旧音频。放在接口上而不是让上层 cast 到具体平台类:
    // 那样非 macOS 构建会因为引用 macOS 后端而编译失败。
    virtual QString voice() const = 0;

    // 界面可选的声音列表。默认空 = 该平台没有可选项。放在接口上的理由同 voice():
    // 让非 macOS 构建不必引用 macOS 后端。
    virtual QStringList availableVoices() const { return {}; }

    // 赛前切换声音/语速。返回 false = 该声音在本机实测无法合成,已拒绝且保留原设置。
    //
    // available() 为 true 的后端**必须**覆写这个方法:上层要靠返回值判断「操作手选的
    // 声音和语速真的生效了吗」,而语速是每次保存都要落到后端的。默认实现只服务于
    // available()==false 的后端 —— 那种后端根本不会被要求改设置,因为没有可用语音时
    // 上层在更早的一步就报了「本机没有中文语音」。
    virtual bool setVoiceAndRate(const QString&, int) { return false; }

    // 把 `text` 合成到 `path`。`token` 原样回传,调用方据此丢弃过期回调。
    virtual void synthesize(const QString& text, const QString& path, quint64 token) = 0;

    // 播放已存在的文件。同一时刻只播一个:队列由上层串行化。
    virtual void play(const QString& path, quint64 token) = 0;

    // 立刻停止当前播放。用于关总开关和比赛结束。
    virtual void stop() = 0;

Q_SIGNALS:
    void synthesizeFinished(quint64 token, bool ok, const QString& error);
    void playbackFinished(quint64 token, bool ok, const QString& error);
};

// 没有已核实的本机中文离线语音的平台用这个。刻意不是 nullptr:上层拿到空指针
// 就得在每个调用点判空,而漏掉一处就是一次崩溃。这里把「没有语音」表达成一个
// 始终 available()==false 的后端,于是平台差异只存在于构造那一行。
class UnavailableSpeechBackend final : public SpeechBackend {
    Q_OBJECT
public:
    explicit UnavailableSpeechBackend(QString reason, QObject* parent = nullptr)
        : SpeechBackend(parent), reason_(std::move(reason)) {}

    bool available() const override { return false; }
    QString unavailableReason() const override { return reason_; }
    QString voice() const override { return {}; }

    // 立即回一个失败而不是静默丢弃:上层等的是一个完成回调,不给它就会让
    // 「准备中」永远挂着,界面显示成正在准备而实际上什么都不会发生。
    void synthesize(const QString&, const QString&, quint64 token) override {
        Q_EMIT synthesizeFinished(token, false, reason_);
    }
    void play(const QString&, quint64 token) override {
        Q_EMIT playbackFinished(token, false, reason_);
    }
    void stop() override {}

private:
    QString reason_;
};

}
