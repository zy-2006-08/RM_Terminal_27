#include "tactical_reminder_controller.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace rm_terminal;

namespace {
void check(bool condition, const char* name) {
    if (!condition) throw std::runtime_error(name);
    std::cout << "PASS " << name << '\n';
}

// 假后端:合成落一个真实文件(缓存命中逻辑要能被真的触发),播放不发声但可控
// 完成时刻。因此整套接线能在毫秒内验证,不必等真实语音。
class FakeBackend : public SpeechBackend {
public:
    bool available() const override { return available_; }
    QString unavailableReason() const override { return reason_; }
    QString voice() const override { return voice_; }

    QStringList availableVoices() const override {
        return {QStringLiteral("FakeVoice"), QStringLiteral("OtherVoice")};
    }

    // 照真机行为建模:列表外的声音一律拒绝并保留原设置。`refuse_setter` 模拟一个
    // 连列表内声音都切不动的后端 —— 那种失败必须能被上层看见。
    bool setVoiceAndRate(const QString& voice, int rate) override {
        if (refuse_setter) return false;
        if (!availableVoices().contains(voice)) return false;
        voice_ = voice;
        rate_ = rate;
        return true;
    }
    int rate() const { return rate_; }

    void synthesize(const QString& text, const QString& path, quint64 token) override {
        ++synth_calls;
        synthesized.push_back(text);
        if (!available_) {
            Q_EMIT synthesizeFinished(token, false, reason_);
            return;
        }
        if (fail_synth) {
            Q_EMIT synthesizeFinished(token, false, QStringLiteral("合成失败"));
            return;
        }
        if (defer_synth) {
            deferred_token = token;
            deferred_path = path;
            return;
        }
        QFile file(path);
        if (file.open(QIODevice::WriteOnly)) file.write(QByteArrayLiteral("FORM....AIFC"));
        Q_EMIT synthesizeFinished(token, true, {});
    }

    // 放掉一条被挂住的合成。用来制造「正式音频正在准备中」这个真实存在的时间窗。
    void releaseSynth() {
        QFile file(deferred_path);
        if (file.open(QIODevice::WriteOnly)) file.write(QByteArrayLiteral("FORM....AIFC"));
        const auto token = deferred_token;
        deferred_token = 0;
        Q_EMIT synthesizeFinished(token, true, {});
    }

    void play(const QString& path, quint64 token) override {
        played.push_back(path);
        playing = token;
    }

    void stop() override {
        ++stops;
        playing = 0;
    }

    // 完成时刻由测试指定并推进注入的时钟。控制器从这一刻起算两遍之间的 1 秒,
    // 所以间隔行为可以被确定性地断言,不必真的等待。
    void complete(bool ok, MonotonicMs when) {
        now = when;
        const auto token = playing;
        playing = 0;
        Q_EMIT playbackFinished(token, ok, ok ? QString() : QStringLiteral("播放失败"));
    }

    MonotonicMs now = 0;

    void makeUnavailable(const QString& reason) {
        available_ = false;
        reason_ = reason;
    }

    std::vector<QString> synthesized;
    std::vector<QString> played;
    int synth_calls = 0;
    int stops = 0;
    quint64 playing = 0;
    bool fail_synth = false;
    bool defer_synth = false;
    bool refuse_setter = false;
    quint64 deferred_token = 0;
    QString deferred_path;

private:
    bool available_ = true;
    QString reason_;
    QString voice_ = QStringLiteral("FakeVoice");
    int rate_ = kDefaultReminderRateWpm;
};

template<class T> Field<T> field(T value, MonotonicMs time) {
    return {value, time, Quality::Valid, Freshness::Fresh};
}

ReminderInputs sample(MonotonicMs time, int seconds, unsigned stage = 4) {
    ReminderInputs in;
    in.connected = true;
    in.stage = field(stage, time);
    in.countdown = field(seconds, time);
    in.paused = field(false, time);
    return in;
}

ReminderConfig config() {
    ReminderConfig out;
    out.master_enabled = true;
    out.reminders = {{"a", 60, "收缩防线", true}, {"b", 30, "推进高地", true}};
    return out;
}

struct Rig {
    explicit Rig(const QString& root, int freshness = 2200)
        : backend(new FakeBackend),
          controller(std::make_unique<ReminderRepository>(QDir(root).filePath(QStringLiteral("cfg"))),
                     backend, freshness, [this] { return backend->now; }) {
        cache = QDir(root).filePath(QStringLiteral("cache"));
    }
    ~Rig() { delete backend; }
    FakeBackend* backend;
    ReminderController controller;
    QString cache;
};

void end_to_end(const QString& root) {
    Rig rig(root);
    check(rig.controller.start(rig.cache).isEmpty(), "clean start reports no error");
    check(rig.controller.uiState().audio == AudioReadiness::Idle,
          "empty configuration does not claim audio is ready");

    QString error;
    check(rig.controller.save(config(), &error), "prematch save succeeds");
    check(rig.backend->synth_calls == 2, "saving prepares offline audio for both reminders");
    check(rig.controller.uiState().audio == AudioReadiness::Ready,
          "audio becomes ready only after every synthesis finishes");
    check(rig.controller.uiState().enabled_count == 2, "entry shows the enabled count");

    rig.controller.observe(sample(0, 61), 0);
    check(rig.controller.uiState().settings_locked, "match start locks the settings");
    check(rig.backend->played.empty(), "baseline observation plays nothing");

    rig.controller.observe(sample(100, 59), 100);
    check(rig.backend->played.size() == 1, "crossing 60 starts the first pass");
    rig.backend->complete(true, 100);
    check(rig.backend->played.size() == 1, "the repeat waits for the one second gap");
    rig.controller.observe(sample(1099, 59), 1099);
    check(rig.backend->played.size() == 1, "repeat is still withheld before the gap elapses");
    rig.controller.observe(sample(1100, 59), 1100);
    check(rig.backend->played.size() == 2 && rig.backend->played[0] == rig.backend->played[1],
          "second pass plays the same audio file exactly once");
    rig.backend->complete(true, 1100);
    rig.controller.observe(sample(3000, 59), 3000);
    check(rig.backend->played.size() == 2, "no third pass ever plays");
}

void next_match(const QString& root) {
    Rig rig(root);
    rig.controller.start(rig.cache);
    QString error;
    rig.controller.save(config(), &error);
    rig.controller.observe(sample(0, 61), 0);
    rig.controller.observe(sample(100, 59), 100);
    check(rig.backend->played.size() == 1, "first match plays");
    rig.backend->complete(true, 100);

    rig.controller.observe(sample(200, 0, 5), 200);
    check(!rig.controller.uiState().settings_locked, "settlement unlocks the settings again");
    check(rig.backend->played.size() == 1,
          "the pending second pass is dropped when the match ends, never played into the next one");

    rig.controller.observe(sample(300, 61), 300);
    rig.controller.observe(sample(400, 59), 400);
    check(rig.backend->played.size() == 2, "confirmed next match rearms the same reminder");
}

void disconnect_no_catchup(const QString& root) {
    Rig rig(root);
    rig.controller.start(rig.cache);
    QString error;
    rig.controller.save(config(), &error);
    rig.controller.observe(sample(0, 70), 0);
    auto lost = sample(100, 70);
    lost.connected = false;
    rig.controller.observe(lost, 100);
    rig.controller.observe(sample(200, 50), 200);
    check(rig.backend->played.empty(),
          "a reminder missed while disconnected is never played late");
    check(rig.controller.uiState().status == ReminderStatus::Suspended,
          "the entry reports the suspended state instead of pretending to be armed");
}

void match_readonly(const QString& root) {
    Rig rig(root);
    rig.controller.start(rig.cache);
    QString error;
    rig.controller.save(config(), &error);
    rig.controller.observe(sample(0, 61), 0);

    check(!rig.controller.save(config(), &error) && !error.isEmpty(),
          "saving during a match is refused with a reason");
    check(!rig.controller.preview(config().reminders[0], {}, kDefaultReminderRateWpm, &error),
          "previewing during a match is refused");

    rig.controller.observe(sample(100, 59), 100);
    check(rig.backend->played.size() == 1, "playback is running before the switch is closed");
    const auto stops = rig.backend->stops;
    rig.controller.disableMaster();
    check(rig.backend->stops > stops, "closing the master switch stops audio immediately");

    check(!rig.controller.uiState().master_enabled,
          "the entry shows the switch as closed immediately, before the config can be rewritten");

    // 快照仍然带着 master_enabled=true:调用方并不知道操作手按过关闭。控制器必须
    // 自己压住它,否则下一个快照就会把提醒重新打开。
    rig.controller.observe(sample(200, 31), 200);
    check(rig.backend->played.size() == 1, "no further reminder plays after the switch is closed");
    check(rig.controller.uiState().status == ReminderStatus::Disabled, "state reports disabled");
}

void persistence(const QString& root) {
    const auto cache = QDir(root).filePath(QStringLiteral("cache"));
    {
        Rig rig(root);
        rig.controller.start(rig.cache);
        QString error;
        check(rig.controller.save(config(), &error), "first process saves the configuration");
    }
    Rig second(root);
    check(second.controller.start(second.cache).isEmpty(), "second process starts clean");
    check(second.controller.config().reminders.size() == 2,
          "configuration survives a process restart");
    check(second.controller.uiState().master_enabled, "master switch state survives a restart");
    check(second.backend->synth_calls == 0,
          "cached audio is reused across restarts, so nothing is resynthesised");
    check(second.controller.uiState().audio == AudioReadiness::Ready,
          "cache hits report ready without any synthesis");
}

void disable_persists(const QString& root) {
    {
        Rig rig(root);
        rig.controller.start(rig.cache);
        QString error;
        rig.controller.save(config(), &error);
        rig.controller.observe(sample(0, 61), 0);
        rig.controller.disableMaster();
    }
    Rig second(root);
    second.controller.start(second.cache);
    check(!second.controller.uiState().master_enabled,
          "the master switch closed during a match stays closed after a restart");
}

void audio_failures(const QString& root) {
    Rig rig(root);
    rig.backend->fail_synth = true;
    rig.controller.start(rig.cache);
    QString error;
    check(rig.controller.save(config(), &error), "save succeeds even when synthesis will fail");
    check(rig.controller.uiState().audio == AudioReadiness::Failed,
          "synthesis failure is visible, never reported as ready");
    check(!rig.controller.uiState().message.isEmpty(), "the failure carries a message");

    rig.controller.observe(sample(0, 61), 0);
    rig.controller.observe(sample(100, 59), 100);
    check(rig.backend->played.empty(), "a failed preparation never plays silence as success");

    Rig missing(root + QStringLiteral("-unavailable"));
    missing.backend->makeUnavailable(QStringLiteral("此平台没有中文离线语音"));
    missing.controller.start(missing.cache);
    check(missing.controller.uiState().audio == AudioReadiness::Unavailable,
          "a platform without a speech backend reports unavailable");
    check(missing.controller.uiState().message.contains(QStringLiteral("语音")),
          "the unavailable reason reaches the entry");
    check(!missing.controller.preview(config().reminders[0], {}, kDefaultReminderRateWpm, &error),
          "preview is refused when no backend exists");
}

void corrupt_config(const QString& root) {
    const auto dir = QDir(root).filePath(QStringLiteral("cfg"));
    QDir().mkpath(dir);
    QFile file(QDir(dir).filePath(QStringLiteral("tactical_reminders.json")));
    if (!file.open(QIODevice::WriteOnly)) throw std::runtime_error("cannot seed");
    file.write(QByteArrayLiteral("{broken"));
    file.close();

    Rig rig(root);
    const auto error = rig.controller.start(rig.cache);
    check(!error.isEmpty(), "a corrupt configuration is reported at startup");
    check(rig.controller.config().reminders.empty(), "a corrupt configuration is not loaded");
    check(QFile(file.fileName()).size() > 0, "the corrupt file is preserved, never overwritten");
    check(!rig.controller.uiState().master_enabled, "a corrupt configuration stays disabled");
}

void preview_cancelled(const QString& root) {
    Rig rig(root);
    rig.controller.start(rig.cache);
    QString error;
    rig.controller.save(config(), &error);
    check(rig.controller.preview(config().reminders[0], {}, kDefaultReminderRateWpm, &error),
          "prematch preview plays");
    const auto stops = rig.backend->stops;
    rig.controller.observe(sample(0, 61), 0);
    check(rig.backend->stops > stops, "match start cancels an in-flight preview");
}

// 快照 → 调度输入的映射。这里断言的重点不是「值搬对了」,而是 quality/freshness
// 一起搬了:只取 value 会把「没收到」和「收到过但已过期」压成同一种情况,而调度器
// 正是靠这个区别决定该等待还是该暂停 —— 丢了元数据,提醒会在断线后照播。
void inputs_from_snapshot() {
    Snapshot snapshot;
    snapshot.game.current_stage = field(4u, 500);
    snapshot.game.stage_countdown_sec = field(59, 500);
    snapshot.game.is_paused = field(false, 500);

    const auto mapped = reminder_inputs(snapshot, true);
    check(mapped.connected, "link state comes from the caller, not from the snapshot");
    check(mapped.stage.value == 4u && mapped.countdown.value == 59
              && mapped.paused.value == false,
          "stage, countdown and pause values are mapped from the game fields");
    check(mapped.stage.quality == Quality::Valid && mapped.stage.freshness == Freshness::Fresh
              && mapped.stage.last_valid == 500,
          "field metadata is carried through, not flattened to the bare value");

    const auto lost = reminder_inputs(snapshot, false);
    check(!lost.connected, "a disconnected link maps to connected=false");

    Snapshot empty;
    const auto missing = reminder_inputs(empty, true);
    check(missing.countdown.quality == Quality::Missing
              && missing.countdown.freshness == Freshness::NeverReceived,
          "a never-received countdown stays distinguishable from a stale one");
    check(missing.audio.state == AudioReadiness::Idle,
          "audio readiness is left for the controller to fill in, never guessed by the mapping");
}

// 已保存的声音和语速必须在重启后真的回到后端。只落盘不重放等于每局开赛都用
// 系统默认声音播报,而界面显示的是操作手选的那个。
void saved_voice_is_reapplied_on_restart(const QString& root) {
    auto chosen = config();
    chosen.voice = "OtherVoice";
    chosen.rate_wpm = 250;
    {
        Rig rig(root);
        rig.controller.start(rig.cache);
        QString error;
        check(rig.controller.save(chosen, &error), "a chosen voice and rate are saved");
    }
    Rig second(root);
    second.controller.start(second.cache);
    check(second.backend->voice() == QStringLiteral("OtherVoice"),
          "the saved voice is reapplied to the backend at startup, not left at the default");
    check(second.backend->rate() == 250, "the saved rate is reapplied at startup too");
    check(second.backend->synth_calls == 0,
          "the cache key matches what the previous run wrote, so nothing is resynthesised");
}

// 落盘失败后,后端必须退回原设置。不退回会让界面报「保存失败」而本局播报已经
// 换成了那个没存下去的声音 —— 操作手无从察觉。
void a_failed_write_rolls_the_backend_back(const QString& root) {
    Rig rig(root);
    rig.controller.start(rig.cache);
    QString error;
    check(rig.controller.save(config(), &error), "a baseline configuration is saved");

    auto rejected = config();
    rejected.voice = "OtherVoice";
    // 仓库会拒绝空文本,于是落盘失败发生在后端已经被切换之后 —— 正是需要回滚的时刻。
    rejected.reminders[0].text = "";
    check(!rig.controller.save(rejected, &error), "a configuration the repository rejects fails");
    check(rig.backend->voice() == QStringLiteral("FakeVoice"),
          "the backend is rolled back to the voice that is actually on disk");
}

// 只改语速也必须真的落到后端。切不动时必须报错而不是静默成功:缓存键会按操作手
// 设的语速去记,于是一份错语速的音频被当成正确的反复复用。
void a_rate_only_change_must_reach_the_backend(const QString& root) {
    Rig rig(root);
    rig.controller.start(rig.cache);
    QString error;
    auto faster = config();
    faster.rate_wpm = 275;
    check(rig.controller.save(faster, &error), "a rate-only change saves");
    check(rig.backend->rate() == 275, "the new rate reaches the backend");

    Rig stubborn(root + QStringLiteral("-stubborn"));
    stubborn.controller.start(stubborn.cache);
    stubborn.backend->refuse_setter = true;
    auto again = config();
    again.rate_wpm = 300;
    check(!stubborn.controller.save(again, &error),
          "a backend that cannot apply the rate makes the save fail instead of reporting success");
    check(!error.isEmpty(), "the refusal explains itself");
}

// 试听用草稿设置,但**不能**改动正式缓存身份。改动了,observe() 就会按草稿路径
// 去排播放,而那些文件从未合成 —— 到点就是静默。
void previewing_a_draft_leaves_the_saved_identity_alone(const QString& root) {
    Rig rig(root);
    rig.controller.start(rig.cache);
    QString error;
    check(rig.controller.save(config(), &error), "a baseline configuration is saved");
    const auto after_save = rig.backend->synth_calls;

    check(rig.controller.preview(config().reminders[0], QStringLiteral("OtherVoice"), 300, &error),
          "a draft voice and rate can be auditioned before saving");
    check(rig.backend->synth_calls == after_save + 1,
          "the draft is synthesised to its own file rather than served from the official cache");
    check(rig.backend->synthesized.back() == QStringLiteral("收缩防线"),
          "what gets auditioned is the selected reminder");
    check(rig.backend->played.size() == 1
              && rig.backend->played.back().contains(QStringLiteral("preview-")),
          "the audition plays its own file, so the official cache is left untouched");
    const auto after_preview = rig.backend->synth_calls;

    // 试听结束后到点播报:必须播已保存身份那份缓存,且不需要重新合成。会重新合成
    // 就说明草稿的声音渗进了正式缓存键。
    rig.controller.observe(sample(0, 61), 0);
    rig.controller.observe(sample(100, 59), 100);
    check(rig.backend->synth_calls == after_preview,
          "the saved cache identity survived the audition, so nothing is resynthesised");
    check(rig.backend->played.size() == 2
              && !rig.backend->played.back().contains(QStringLiteral("preview-")),
          "what plays for the match is the official cached audio, never the draft preview file");
}

// 正式音频还在合成时不受理试听:后端只有一个 say 槽位,试听会杀掉在途的合成,
// 而它的令牌已作废 —— 准备状态从此永远停在「准备中」。
void previewing_is_refused_while_audio_is_still_preparing(const QString& root) {
    Rig rig(root);
    rig.controller.start(rig.cache);
    rig.backend->defer_synth = true;

    QString error;
    check(rig.controller.save(config(), &error), "the save starts preparing audio");
    check(rig.controller.uiState().audio == AudioReadiness::Preparing,
          "preparation is genuinely in flight");
    check(!rig.controller.preview(config().reminders[0], {}, kDefaultReminderRateWpm, &error),
          "an audition is refused while official audio is still being synthesised");
    check(!error.isEmpty(), "the refusal tells the operator to wait");

    rig.backend->defer_synth = false;
    rig.backend->releaseSynth();
    check(rig.controller.uiState().audio != AudioReadiness::Preparing,
          "preparation continues normally once the in-flight job completes");
}

// 磁盘上的声音在这台机器上不存在(换机器、声音被删)时,启动必须报错并且**不**用
// 别的声音去写正式缓存 —— 那份缓存会以已保存身份命名,从此再也不会被重新合成。
void an_unusable_saved_voice_fails_loudly_at_startup(const QString& root) {
    const auto cache = QDir(root).filePath(QStringLiteral("cache"));
    {
        Rig rig(root);
        rig.controller.start(rig.cache);
        QString error;
        auto ok = config();
        ok.voice = "OtherVoice";
        check(rig.controller.save(ok, &error), "a valid voice is saved first");
    }
    // 直接改磁盘:模拟这台机器上没有那个声音的情形。
    const auto path = QDir(QDir(root).filePath(QStringLiteral("cfg")))
                          .filePath(QStringLiteral("tactical_reminders.json"));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("cannot read config");
    auto text = QString::fromUtf8(file.readAll());
    file.close();
    text.replace(QStringLiteral("OtherVoice"), QStringLiteral("GhostVoice"));
    if (!file.open(QIODevice::WriteOnly)) throw std::runtime_error("cannot rewrite config");
    file.write(text.toUtf8());
    file.close();

    Rig second(root);
    const auto error = second.controller.start(second.cache);
    check(!error.isEmpty(), "a saved voice that this machine cannot use is reported at startup");
    check(second.controller.uiState().audio == AudioReadiness::Failed,
          "audio reports failed rather than pretending to be ready");
    check(second.backend->synth_calls == 0,
          "nothing is synthesised, so no wrong-voice file is written under the saved identity");
    check(!second.controller.uiState().message.isEmpty(), "the operator gets a message");
}

void config_change_regenerates(const QString& root) {
    Rig rig(root);
    rig.controller.start(rig.cache);
    QString error;
    rig.controller.save(config(), &error);
    const auto first = rig.backend->synth_calls;

    auto edited = config();
    edited.reminders[0].text = "改写后的战术";
    check(rig.controller.save(edited, &error), "editing text saves");
    check(rig.backend->synth_calls > first, "changed text is resynthesised, not served from cache");
    check(rig.backend->synthesized.back() == QStringLiteral("改写后的战术"),
          "the newly synthesised text is the edited one");
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir dir;
        if (!dir.isValid()) throw std::runtime_error("cannot create temp dir");
        const auto at = [&](const char* name) { return QDir(dir.path()).filePath(QString::fromLatin1(name)); };
        end_to_end(at("e2e"));
        next_match(at("next"));
        disconnect_no_catchup(at("gap"));
        match_readonly(at("readonly"));
        persistence(at("persist"));
        disable_persists(at("disable"));
        audio_failures(at("audio"));
        corrupt_config(at("corrupt"));
        preview_cancelled(at("preview"));
        saved_voice_is_reapplied_on_restart(at("revoice"));
        a_failed_write_rolls_the_backend_back(at("rollback"));
        a_rate_only_change_must_reach_the_backend(at("rate"));
        previewing_a_draft_leaves_the_saved_identity_alone(at("draft"));
        previewing_is_refused_while_audio_is_still_preparing(at("busy"));
        an_unusable_saved_voice_fails_loudly_at_startup(at("ghost"));
        config_change_regenerates(at("regen"));
        inputs_from_snapshot();
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    std::cout << "reminder_controller: all checks passed\n";
    return 0;
}
