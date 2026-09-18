#pragma once

#include "domain.h"
#include "reminder_playback.h"
#include "reminder_repository.h"
#include "reminder_scheduler.h"
#include "speech_backend.h"

#include <QObject>
#include <QString>
#include <deque>
#include <functional>
#include <memory>
#include <optional>

namespace rm_terminal {

// 顶部入口需要显示的一切。聚成一个结构体传出,避免入口按钮为了拼一行状态文字
// 去分别查询调度器、仓库和音频后端三处。
struct ReminderUiState {
    bool master_enabled = false;
    int enabled_count = 0;
    bool settings_locked = false;
    ReminderStatus status = ReminderStatus::Idle;
    AudioReadiness audio = AudioReadiness::Idle;
    QString message;
};

// 快照 → 调度输入。`connected` 由链路层单独给出:快照里的字段只能说明「数据旧了」,
// 说不清是断线还是比赛真的暂停,而这两种情况对播报的处置完全相反。
ReminderInputs reminder_inputs(const Snapshot& snapshot, bool connected);

// 把持久化、语音准备、调度和播放接到一起。这一层拥有全部副作用:文件读写、
// 子进程、以及「现在几点」。调度和排队本身仍是纯逻辑,所以时序规则的证明
// 留在各自的单元测试里,这里只验证接线。
class ReminderController : public QObject {
    Q_OBJECT
public:
    // 时钟可注入。第二遍的 1 秒间隔从**上一遍真正结束的时刻**算起,而那个时刻
    // 只有播放回调自己知道 —— 用最近一次 observe 的时间会把间隔算短。注入后
    // 测试能确定性地推进时间,不必真的睡 1 秒。
    using Clock = std::function<MonotonicMs()>;

    // 后端可注入:测试用假后端就能在毫秒级验证接线,不必真的发声。
    ReminderController(std::unique_ptr<ReminderRepository> repository, SpeechBackend* backend,
                      int freshness_stale_ms, Clock clock = {}, QObject* parent = nullptr);

    // 启动时加载磁盘配置并开始离线准备音频。损坏的配置不会被静默覆盖:
    // 返回的错误交给界面显示,磁盘上的文件原样保留。
    QString start(const QString& cache_directory);

    // 每次收到新快照时调用。`now` 用单调时钟,与项目其余部分一致。
    void observe(const ReminderInputs& inputs, MonotonicMs now);

    // 第二遍的到点检查。播放结束和 observe 都不一定发生在间隔期满的那一刻,
    // 所以由界面的 UI tick 定期调用这个;它只推进播放队列,不碰调度状态。
    void tick(MonotonicMs now);

    // 赛前保存。赛中被拒绝(返回 false),对应「赛中只允许查看和关闭」。
    bool save(const ReminderConfig& config, QString* error);

    // 赛中允许的写操作之一:关掉总开关。立即静音,并尽力持久化 ——
    // 持久化失败也必须保持本次进程静音,操作手的意图优先于落盘结果。
    void disableMaster();

    // disableMaster 的反向操作,赛中同样允许。只重新布防剩下的时间:关闭期间
    // 过掉的阈值不补播,关闭前已播的也不重播。
    void setMasterEnabled(bool enabled);

    // 赛前试听。传入的是**草稿**:操作手还没保存就该能听,所以未命中缓存时
    // 现场合成到独立的试听文件,不污染正式缓存。
    //
    // `voice`/`rate_wpm` 同样取草稿值(voice 为空表示「自动」),否则改完声音再试听
    // 听到的还是上次保存的那个声音 —— 试听就失去了意义。
    //
    // 试听会真的改掉后端的当前声音,且**不**在结束时恢复:恢复要覆盖失败、比赛开始
    // 打断等每条路径,漏一条就会留下错声音。改由 prepareAudio() 在合成正式音频前
    // 重新应用已保存设置来兜底,那是唯一会写正式缓存的地方。
    bool preview(const ReminderItem& item, const QString& voice, int rate_wpm, QString* error);

    ReminderUiState uiState() const;
    const ReminderConfig& config() const { return scheduler_.config(); }

    // 设置界面用:本机系统里列出的中文声音。转发而不是让界面直连后端,
    // 保持界面只依赖控制器这一个入口。
    QStringList availableVoices() const { return backend_->availableVoices(); }

    // 「自动」当前指向哪个声音。界面用它在配置没写声音时显示实际会用的那一个,
    // 而不是让下拉停在列表第一项 —— 那会显示成一个并非实际在用的声音。
    QString currentVoice() const { return voice_; }

Q_SIGNALS:
    void stateChanged();

private:
    // 一批待合成任务。真实后端只有一个 say 进程槽位,并发下发会让后一个任务
    // 杀掉前一个 —— 所以整批串行:一条回来了才发下一条。
    struct SynthJob {
        QString text;
        QString path;
    };

    // 只改后端,不碰任何成员。试听必须走这个:成员里的语速参与正式缓存键,
    // 被草稿值改写后 observe() 会按不存在的路径排播放。
    QString applyVoiceToBackend(const QString& voice, std::int32_t rate_wpm);

    // 「自动」(空声音)指向哪个具体声音。解析成已保存的那个而不是后端当前那个:
    // 试听期间后端停在草稿声音上,拿当前值会把草稿的选择当成已保存的。
    QString resolveVoice(const QString& voice) const;

    // 后端真正会采用的语速。后端把越界值钳进自己的范围,而缓存键必须记这个钳后的值,
    // 否则键指向一个从未被合成过的组合,每次启动都重新合成同一批音频。
    static std::int32_t effectiveRate(std::int32_t rate_wpm);

    // 把配置里的声音和语速交给后端,并记为「已保存设置」。失败时不改动后端,
    // 由调用方决定是拒绝保存还是带着提示继续启动。
    QString applyVoiceSettings(const ReminderConfig& config);

    void prepareAudio();
    void startNextSynth();
    void onSynthesized(quint64 token, bool ok, const QString& error);
    void onPlayed(quint64 token, bool ok, const QString& error);
    // 声音+语速的组合标识,参与音频缓存键:两者任一改变都必须重新合成。
    static QString voiceKeyFor(const QString& voice, std::int32_t rate_wpm);
    // 已保存设置对应的标识。正式缓存只认这一个 —— 试听用的草稿标识绝不能写进
    // voice_/voice_rate_,否则 observe() 排进播放队列的路径会指向不存在的文件。
    QString voiceKey() const;
    QString cachePath(const ReminderItem& item) const;
    QString previewPath(const ReminderItem& item, const QString& voice_key) const;
    void pump(MonotonicMs now);
    MonotonicMs now() const;

    std::unique_ptr<ReminderRepository> repository_;
    SpeechBackend* backend_;
    Clock clock_;
    ReminderScheduler scheduler_;
    ReminderPlayback playback_;
    QString cache_dir_;
    QString voice_;
    std::int32_t voice_rate_ = kDefaultReminderRateWpm;
    AudioStatus audio_;
    QString message_;
    ReminderStatus status_ = ReminderStatus::Idle;
    bool allow_playback_ = false;
    // 赛中按下的总开关。调度器此时被锁,配置改不进去,所以本局的开关状态靠这个
    // 覆盖值:它同时压住送进调度器的开关输入和界面显示,等解锁后再落成配置。
    //
    // 必须是三态而不是 bool。bool 只能表达「赛中被关掉」,于是赛中重新打开时无处
    // 记录 —— 配置里那个 false 改不动,界面就一直显示关着,开关按上去弹回去。
    std::optional<bool> master_override_;

    // 最近一次 observe 的输入与时刻。总开关是界面事件,不跟着快照来,所以重新启用
    // 时要用它复算一次调度 —— 否则状态要等到下一帧快照才更新,开关看着像没生效。
    std::optional<ReminderInputs> last_inputs_;
    MonotonicMs last_observed_ = 0;

    // 串行合成的剩余任务与当前在途任务。`synth_token_` 为 0 表示没有在途任务。
    std::deque<SynthJob> synth_queue_;
    quint64 synth_token_ = 0;
    // 在途合成属于哪一版配置。回调回来时比对这个而不是比对「当前代次」:
    // 令牌本身必须能自证归属,否则连改两次配置后旧回调仍会被当成新的。
    std::uint32_t synth_generation_ = 0;

    // 试听:令牌与在途合成分开记,回调据此区分归属。
    quint64 preview_play_token_ = 0;
    quint64 preview_synth_token_ = 0;
    QString preview_path_;

    quint64 next_token_ = 1;
};

}
