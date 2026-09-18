#include "tactical_reminder_controller.h"

#include "clock.h"

#include <QDir>
#include <QFileInfo>
#include <algorithm>

namespace rm_terminal {

ReminderInputs reminder_inputs(const Snapshot& snapshot, bool connected) {
    ReminderInputs out;
    out.connected = connected;
    // 运行时开关**不**在这里决定。快照说不出操作手有没有在赛中按下关闭,那个意图
    // 由控制器的 master_override_ 保管并在 observe 里压过这个值;这里传 true 只表示
    // 「链路层没有额外的静音要求」,不是「提醒已启用」。
    out.master_enabled = true;
    // 三个字段整体照搬,连 quality/freshness 一起。只取 value 会把「没收到」和
    // 「收到过但已过期」压成同一种情况,而调度器正是靠这个区别决定该等待还是该暂停。
    out.stage = snapshot.game.current_stage;
    out.countdown = snapshot.game.stage_countdown_sec;
    out.paused = snapshot.game.is_paused;
    // audio 留空:它是控制器自己的准备状态,observe 会覆写。调用方无从得知。
    return out;
}

ReminderController::ReminderController(std::unique_ptr<ReminderRepository> repository,
                                      SpeechBackend* backend, int freshness_stale_ms, Clock clock,
                                      QObject* parent)
    : QObject(parent), repository_(std::move(repository)), backend_(backend),
      clock_(clock ? std::move(clock) : Clock(&monotonic_now)),
      scheduler_(ReminderConfig{}, freshness_stale_ms) {
    QObject::connect(backend_, &SpeechBackend::synthesizeFinished, this,
                     &ReminderController::onSynthesized);
    QObject::connect(backend_, &SpeechBackend::playbackFinished, this,
                     &ReminderController::onPlayed);
}

MonotonicMs ReminderController::now() const { return clock_(); }

QString ReminderController::start(const QString& cache_directory) {
    cache_dir_ = cache_directory;

    // 先记下后端自己挑中的声音:配置里 voice 留空表示「自动」,而「自动」就是这一个。
    // 必须在任何 applyVoiceSettings 之前取,那之后取到的可能是刚被试听改过的草稿声音。
    voice_ = backend_->voice();

    const auto loaded = repository_->load();
    QString error;
    switch (loaded.result) {
        case ReminderLoadResult::Loaded:
            scheduler_.setConfig(loaded.config);
            error = applyVoiceSettings(loaded.config);
            if (!error.isEmpty()) message_ = error;
            break;
        case ReminderLoadResult::Missing:
            break;
        case ReminderLoadResult::Invalid:
            // 损坏的配置既不加载也不覆盖。以空配置运行并把错误报给操作手,
            // 磁盘上的原文件留着,还有人工挽救的机会。
            error = loaded.error;
            message_ = loaded.error;
            break;
    }
    prepareAudio();
    Q_EMIT stateChanged();
    return error;
}

QString ReminderController::resolveVoice(const QString& voice) const {
    // 空 = 「自动」,解析成**已保存的**那个声音,而不是后端此刻的当前声音:试听会把
    // 后端临时切到草稿声音,拿当前值解析会让草稿的选择渗进正式设置。
    return voice.isEmpty() ? voice_ : voice;
}

QString ReminderController::applyVoiceToBackend(const QString& voice, std::int32_t rate_wpm) {
    if (!backend_->available()) return {};

    const auto target = resolveVoice(voice);
    if (!backend_->availableVoices().contains(target))
        return QStringLiteral("声音「%1」在本机无法合成中文语音").arg(target);
    // 失败一律上报,连「只改语速」也不例外:语速没落到后端就意味着播报语速不是
    // 操作手设的那个,而缓存键会按他设的值去记 —— 于是错语速的音频被当成正确的复用。
    // 给后端的语速必须和进缓存键的那个值同源(都过 effectiveRate),否则越界语速被
    // 后端钳一次、键里记另一个值,同一个文件会被两种语速共用。
    if (!backend_->setVoiceAndRate(target, effectiveRate(rate_wpm)))
        return QStringLiteral("声音「%1」在本机无法合成中文语音").arg(target);
    return {};
}

QString ReminderController::applyVoiceSettings(const ReminderConfig& config) {
    const auto voice = QString::fromStdString(config.voice);
    const auto error = applyVoiceToBackend(voice, config.rate_wpm);
    if (!error.isEmpty()) return error;
    if (backend_->available()) {
        // 记成「已保存身份」的必须是解析后的具体声音和后端**真正采用**的语速:
        // 后端会把越界语速钳到自己的范围内,记原值会让缓存键指向一个从未合成过的组合。
        voice_ = resolveVoice(voice);
        voice_rate_ = effectiveRate(config.rate_wpm);
    }
    return {};
}

std::int32_t ReminderController::effectiveRate(std::int32_t rate_wpm) {
    return std::clamp(rate_wpm, kMinReminderRateWpm, kMaxReminderRateWpm);
}

QString ReminderController::voiceKeyFor(const QString& voice, std::int32_t rate_wpm) {
    // 语速必须进缓存键。只用声音名的话,把语速从 175 调到 250 会命中同一个文件,
    // 操作手改完设置试听,听到的还是旧语速。
    return QStringLiteral("%1@%2").arg(voice).arg(rate_wpm);
}

QString ReminderController::voiceKey() const { return voiceKeyFor(voice_, voice_rate_); }

QString ReminderController::cachePath(const ReminderItem& item) const {
    // 只用内容(文本+声音+配置格式版本)做键,不用运行时的配置代次:代次是进程内
    // 计数器,拿它命名会让改一条提醒就废掉全部音频,而且重启后能否命中缓存
    // 取决于两次运行各保存过几次 —— 那不是缓存,那是巧合。
    const auto name =
        reminder_audio_key(item.text, voiceKey().toStdString(), kReminderConfigVersion);
    return QDir(cache_dir_).filePath(QString::fromStdString(name));
}

QString ReminderController::previewPath(const ReminderItem& item, const QString& voice_key) const {
    return QDir(cache_dir_).filePath(QStringLiteral("preview-%1.aiff")
                                         .arg(QString::fromStdString(reminder_audio_key(
                                             item.text, voice_key.toStdString(), 0))));
}

void ReminderController::prepareAudio() {
    playback_.cancelAll();
    backend_->stop();
    synth_queue_.clear();
    synth_token_ = 0;
    preview_synth_token_ = 0;
    preview_play_token_ = 0;
    audio_.generation = scheduler_.configGeneration();
    synth_generation_ = scheduler_.configGeneration();

    if (!backend_->available()) {
        audio_.state = AudioReadiness::Unavailable;
        audio_.message = backend_->unavailableReason().toStdString();
        message_ = backend_->unavailableReason();
        return;
    }
    const auto& config = scheduler_.config();
    // 试听可能刚把后端切到草稿里的声音。正式音频必须用**已保存**的设置合成,所以
    // 在这里重新应用一次 —— 这是唯一会写正式缓存的地方,把兜底放在这一点上就不必
    // 在试听的每条结束路径上做恢复。
    if (const auto voice_error = applyVoiceSettings(config); !voice_error.isEmpty()) {
        // 应用不上就**不合成**。继续下去会用后端此刻停留的那个声音写正式缓存文件,
        // 而文件名按已保存身份命名 —— 缓存从此指向一份错声音的音频,且再也不会
        // 被重新合成,因为下次启动看到的是「已命中缓存」。
        audio_.state = AudioReadiness::Failed;
        audio_.message = voice_error.toStdString();
        message_ = voice_error;
        return;
    }

    if (!config.master_enabled || config.reminders.empty()) {
        audio_.state = AudioReadiness::Idle;
        audio_.message.clear();
        return;
    }
    if (!QDir().mkpath(cache_dir_)) {
        audio_.state = AudioReadiness::Failed;
        message_ = QStringLiteral("无法创建语音缓存目录 %1").arg(cache_dir_);
        audio_.message = message_.toStdString();
        return;
    }

    for (const auto& item : config.reminders) {
        if (!item.enabled) continue;
        const auto path = cachePath(item);
        // 缓存命中直接跳过合成。键由内容决定,所以命中意味着文件内容确实对应
        // 这条文本和这个声音,不是「碰巧同名」。
        if (QFileInfo(path).size() > 0) continue;
        synth_queue_.push_back({QString::fromStdString(item.text), path});
    }

    message_.clear();
    if (synth_queue_.empty()) {
        audio_.state = AudioReadiness::Ready;
        audio_.message.clear();
        return;
    }
    audio_.state = AudioReadiness::Preparing;
    audio_.message.clear();
    startNextSynth();
}

void ReminderController::startNextSynth() {
    if (synth_queue_.empty()) {
        synth_token_ = 0;
        audio_.state = AudioReadiness::Ready;
        return;
    }
    const auto job = synth_queue_.front();
    synth_queue_.pop_front();
    synth_token_ = next_token_++;
    // 一次只发一条:真实后端只有一个 say 进程槽位,并发下发会让后一条杀掉
    // 前一条,于是先合成的那几条永远不会落盘却被当成已就绪。
    backend_->synthesize(job.text, job.path, synth_token_);
}

void ReminderController::onSynthesized(quint64 token, bool ok, const QString& error) {
    if (preview_synth_token_ != 0 && token == preview_synth_token_) {
        preview_synth_token_ = 0;
        if (!ok) {
            message_ = error;
        } else {
            preview_play_token_ = next_token_++;
            backend_->play(preview_path_, preview_play_token_);
        }
        Q_EMIT stateChanged();
        return;
    }
    // 令牌不是当前在途的那条 = 这是被取消或被换代作废的回调。丢弃它,否则旧文本
    // 会被登记成当前配置的「已就绪」。
    if (token == 0 || token != synth_token_) return;
    if (synth_generation_ != scheduler_.configGeneration()) return;
    synth_token_ = 0;

    if (!ok) {
        // 一条失败即整批失败:少一条战术没播和全都没播一样是故障,含糊地报
        // 「部分就绪」会让操作手以为剩下的还能响。
        synth_queue_.clear();
        audio_.state = AudioReadiness::Failed;
        audio_.message = error.toStdString();
        message_ = error;
        Q_EMIT stateChanged();
        return;
    }
    const bool was_last = synth_queue_.empty();
    startNextSynth();
    if (was_last) Q_EMIT stateChanged();
}

void ReminderController::onPlayed(quint64 token, bool ok, const QString& error) {
    if (preview_play_token_ != 0 && token == preview_play_token_) {
        preview_play_token_ = 0;
        if (!ok) {
            message_ = error;
            Q_EMIT stateChanged();
        }
        return;
    }
    // 用真正的完成时刻,不是最近一次 observe 的时刻:observe 可能发生在几百毫秒
    // 之前,拿它当起点会把两遍之间的间隔算短。
    const auto completed_at = now();
    playback_.finished(token, ok, completed_at);
    if (!ok) {
        message_ = error;
        Q_EMIT stateChanged();
    }
    pump(completed_at);
}

void ReminderController::observe(const ReminderInputs& inputs, MonotonicMs now) {
    last_inputs_ = inputs;
    last_observed_ = now;

    auto scoped = inputs;
    scoped.audio = audio_;
    // 赛中按下的开关要压过调用方传来的值,否则每个新快照都会把它冲掉。
    if (master_override_) scoped.master_enabled = *master_override_;

    const auto decision = scheduler_.step(scoped, now);

    if (decision.cancel_preview && (preview_play_token_ != 0 || preview_synth_token_ != 0)) {
        // 比赛开始必须掐掉试听:试听声会盖住真正的战术播报。
        backend_->stop();
        preview_play_token_ = 0;
        preview_synth_token_ = 0;
    }
    switch (decision.cancel) {
        case ReminderCancellation::None:
            break;
        case ReminderCancellation::Pending:
            playback_.cancelPending();
            break;
        case ReminderCancellation::All:
            playback_.cancelAll();
            backend_->stop();
            break;
    }
    for (const auto& item : decision.enqueue)
        playback_.enqueue(item, cachePath(item).toStdString());
    allow_playback_ = decision.allow_playback;
    status_ = decision.status;

    // 赛中按下的关闭在解锁后才能写进调度器。趁这一刻落地,否则下一局会用内存里
    // 那份仍然「开着」的配置重新布防。
    if (master_override_ && !scheduler_.settingsLocked()) {
        const bool enabled = *master_override_;
        auto config = scheduler_.config();
        config.master_enabled = enabled;
        scheduler_.setConfig(config);
        master_override_.reset();
        if (!enabled) {
            status_ = ReminderStatus::Disabled;
            allow_playback_ = false;
        }
    }

    pump(now);
    Q_EMIT stateChanged();
}

void ReminderController::tick(MonotonicMs now) { pump(now); }

void ReminderController::pump(MonotonicMs now) {
    if (preview_play_token_ != 0 || preview_synth_token_ != 0) return;
    const auto command = playback_.tick(now, allow_playback_);
    if (!command) return;
    backend_->play(QString::fromStdString(command->path), command->token);
}

bool ReminderController::save(const ReminderConfig& config, QString* error) {
    if (scheduler_.settingsLocked()) {
        if (error) *error = QStringLiteral("比赛进行中，设置为只读");
        return false;
    }
    // 先真的把设置应用到后端,再落盘。只做只读校验是不够的:校验只看声音在不在
    // 列表里,而真正的失败发生在后端切换那一步(实测合成不出声)。校验通过、落盘成功、
    // 切换失败,会留下一份磁盘上指向哑声音的配置。
    const auto previous_voice = voice_;
    const auto previous_rate = voice_rate_;
    if (const auto voice_error = applyVoiceSettings(config); !voice_error.isEmpty()) {
        if (error) *error = voice_error + QStringLiteral("，已保留原声音设置");
        return false;
    }
    if (!repository_->save(config, error)) {
        // 落盘失败必须把后端退回原设置:否则界面报「保存失败」,而这一局的播报
        // 已经换成了那个没能存下去的声音。
        //
        // 退回本身也可能失败。这时后端仍停在新声音上,已保存身份就必须跟着记成新声音:
        // 成员与后端不一致会让缓存键指向一个声音、say 实际用另一个,到点播出的语速/音色
        // 与缓存内容不符。宁可如实记录并把这件事告诉操作手。
        if (applyVoiceToBackend(previous_voice, previous_rate).isEmpty()) {
            voice_ = previous_voice;
            voice_rate_ = previous_rate;
        } else if (error) {
            *error += QStringLiteral("，且无法退回原声音设置，当前生效声音为「%1」").arg(voice_);
        }
        return false;
    }
    scheduler_.setConfig(config);
    prepareAudio();
    Q_EMIT stateChanged();
    return true;
}

void ReminderController::setMasterEnabled(bool enabled) {
    if (!enabled) {
        disableMaster();
        return;
    }

    auto config = scheduler_.config();
    const bool already_on = master_override_ ? *master_override_ : config.master_enabled;
    if (already_on) return;
    config.master_enabled = true;

    // 赛中调度器被锁,setConfig 会被拒绝,所以本局的重新布防靠覆盖值记住「开」;
    // 未锁定时直接落成配置并清掉覆盖。两条路径都要落盘,否则重启后开关又变回关着。
    if (scheduler_.settingsLocked()) {
        master_override_ = true;
    } else {
        scheduler_.setConfig(config);
        master_override_.reset();
    }

    QString error;
    if (!repository_->save(config, &error))
        message_ = QStringLiteral("已重新启用，但保存失败：%1").arg(error);
    else
        message_.clear();

    // 关闭时清空过合成队列,音频缓存要重新准备好才能到点播出。
    prepareAudio();

    // 立刻用最近一次输入复算一次调度,把状态从 Disabled 推回真实值。少了这一步,
    // 界面要等下一帧快照才反映,操作手会以为开关没生效。复算走的是 observe 的
    // 同一条路径,状态判定不在这里复制一份。
    if (last_inputs_) observe(*last_inputs_, last_observed_);
    Q_EMIT stateChanged();
}

void ReminderController::disableMaster() {
    auto config = scheduler_.config();
    const bool already_off = master_override_ ? !*master_override_ : !config.master_enabled;
    if (already_off) return;
    config.master_enabled = false;

    // 先静音再落盘。反过来会让持久化失败时静音也失败 —— 操作手按了关闭就必须
    // 立刻安静,落盘结果只是一条提示。
    playback_.cancelAll();
    backend_->stop();
    synth_queue_.clear();
    synth_token_ = 0;
    preview_play_token_ = 0;
    preview_synth_token_ = 0;
    allow_playback_ = false;
    master_override_ = false;
    status_ = ReminderStatus::Disabled;

    QString error;
    if (!repository_->save(config, &error))
        message_ = QStringLiteral("已停止播报，但保存失败：%1").arg(error);

    // 赛中调度器被锁,配置改不进去,本局的静音全靠 master_off_。解锁状态下才
    // 直接落成配置。这里不调 prepareAudio():它会清掉上面那条保存失败提示。
    if (!scheduler_.settingsLocked()) {
        scheduler_.setConfig(config);
        master_override_.reset();
    }
    audio_.state = AudioReadiness::Idle;
    Q_EMIT stateChanged();
}

bool ReminderController::preview(const ReminderItem& item, const QString& voice, int rate_wpm,
                                 QString* error) {
    if (scheduler_.settingsLocked()) {
        if (error) *error = QStringLiteral("比赛进行中，无法试听");
        return false;
    }
    if (!backend_->available()) {
        if (error) *error = backend_->unavailableReason();
        return false;
    }
    if (item.text.empty()) {
        if (error) *error = QStringLiteral("提醒内容为空，无法试听");
        return false;
    }
    // 正式音频还在合成时不受理试听。后端只有一个 say 槽位,试听会把在途的那条
    // 合成杀掉,而它的回调令牌已经作废 —— 准备状态就永远停在「语音准备中」,
    // 界面看着在忙,实际上再也不会有音频落盘。
    if (synth_token_ != 0 || !synth_queue_.empty()) {
        if (error) *error = QStringLiteral("语音正在准备中，请稍候再试听");
        return false;
    }

    if (const auto voice_error = applyVoiceToBackend(voice, rate_wpm); !voice_error.isEmpty()) {
        if (error) *error = voice_error;
        return false;
    }
    // 草稿的标识只留在局部。写进 voice_ 会让 observe() 之后按草稿路径去排播放,
    // 而那些文件从未被合成 —— 到点就是静默。
    const auto draft_key = voiceKeyFor(resolveVoice(voice), effectiveRate(rate_wpm));

    // 只有草稿设置和已保存设置一致时才能直接放正式缓存:声音或语速改过而去放旧文件,
    // 操作手听到的正是他刚刚改掉的那个声音。
    if (draft_key == voiceKey()) {
        const auto cached = cachePath(item);
        if (QFileInfo(cached).size() > 0) {
            preview_play_token_ = next_token_++;
            backend_->play(cached, preview_play_token_);
            return true;
        }
    }
    // 草稿还没保存过,缓存里当然没有。现场合成到独立的试听文件:操作手应该
    // 在保存前就能听见自己写的内容,而正式缓存不该被未确认的草稿污染。
    if (!QDir().mkpath(cache_dir_)) {
        if (error) *error = QStringLiteral("无法创建语音缓存目录 %1").arg(cache_dir_);
        return false;
    }
    preview_path_ = previewPath(item, draft_key);
    preview_synth_token_ = next_token_++;
    backend_->synthesize(QString::fromStdString(item.text), preview_path_, preview_synth_token_);
    return true;
}

ReminderUiState ReminderController::uiState() const {
    ReminderUiState out;
    const auto& config = scheduler_.config();
    // 赛中按下的开关还没能写进配置,但界面必须显示刚按下的那个状态 —— 显示成旧值
    // 会让操作手以为没按上,然后反复去按一个已经生效的开关。
    out.master_enabled = master_override_ ? *master_override_ : config.master_enabled;
    for (const auto& item : config.reminders)
        if (item.enabled) ++out.enabled_count;
    out.settings_locked = scheduler_.settingsLocked();
    out.status = status_;
    out.audio = audio_.state;
    out.message = message_;
    return out;
}

}
