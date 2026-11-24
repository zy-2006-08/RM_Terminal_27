#include "tactical_reminder_dialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>

using namespace rm_terminal;

namespace {
void check(bool condition, const char* name) {
    if (!condition) throw std::runtime_error(name);
    std::cout << "PASS " << name << '\n';
}

class FakeBackend : public SpeechBackend {
public:
    bool available() const override { return true; }
    QString unavailableReason() const override { return {}; }
    QString voice() const override { return voice_; }

    QStringList availableVoices() const override {
        return {QStringLiteral("FakeVoice"), QStringLiteral("OtherVoice")};
    }

    // 模拟真机行为:只有列表里的声音能合成,其余一律拒绝且保留原设置。
    bool setVoiceAndRate(const QString& voice, int rate) override {
        if (!availableVoices().contains(voice)) return false;
        voice_ = voice;
        rate_ = rate;
        return true;
    }
    int rate() const { return rate_; }

    void synthesize(const QString&, const QString& path, quint64 token) override {
        ++synthesized;
        QFile file(path);
        if (file.open(QIODevice::WriteOnly)) file.write(QByteArrayLiteral("FORM....AIFC"));
        Q_EMIT synthesizeFinished(token, true, {});
    }
    void play(const QString& path, quint64 token) override {
        played.push_back(path);
        Q_UNUSED(token);
    }
    void stop() override {}

    std::vector<QString> played;
    int synthesized = 0;

private:
    QString voice_ = QStringLiteral("FakeVoice");
    int rate_ = kDefaultReminderRateWpm;
};

template<class T> Field<T> field(T value, MonotonicMs time) {
    return {value, time, Quality::Valid, Freshness::Fresh};
}

ReminderInputs match_at(MonotonicMs time, int seconds, unsigned stage = 4) {
    ReminderInputs in;
    in.connected = true;
    in.stage = field(stage, time);
    in.countdown = field(seconds, time);
    in.paused = field(false, time);
    return in;
}

ReminderConfig two_reminders() {
    ReminderConfig out;
    out.master_enabled = true;
    out.reminders = {{"a", 90, "收缩防线", true}, {"b", 30, "推进高地", true}};
    return out;
}

struct Rig {
    explicit Rig(const QString& root)
        : backend(new FakeBackend),
          controller(std::make_unique<ReminderRepository>(QDir(root).filePath(QStringLiteral("cfg"))),
                     backend, 2200) {
        controller.start(QDir(root).filePath(QStringLiteral("cache")));
    }
    ~Rig() { delete backend; }
    FakeBackend* backend;
    ReminderController controller;
};

void time_text_roundtrips() {
    check(reminder_time_text(90) == QStringLiteral("01:30"),
          "seconds render as the MM:SS the operator typed");
    check(reminder_time_text(0) == QStringLiteral("00:00"), "zero renders as 00:00");
    check(reminder_time_text(kMaxReminderRemainingSec) == QStringLiteral("99:59"),
          "the maximum renders without overflowing the field");

    for (std::int32_t seconds : {0, 1, 59, 60, 90, 599, 3599, kMaxReminderRemainingSec})
        check(parse_reminder_time(reminder_time_text(seconds)) == seconds,
              "format and parse agree on every representative value");
}

// 打错的时间必须被**拒绝**而不是静默变成 0。静默归零会把一条本该在 1:30 播报的
// 提醒变成「剩余 0 秒时播报」,操作手看不出任何异常,直到比赛里它没响。
void bad_time_is_rejected_not_zeroed() {
    for (const char* bad : {"", "90", "1:60", "1:5", "abc", "1:30:00", "-1:30", "100:00", "1.30"})
        check(!parse_reminder_time(QString::fromLatin1(bad)).has_value(),
              "an unparseable time is refused outright");
    check(!parse_reminder_time(QStringLiteral("99:60")).has_value(),
          "sixty seconds is refused rather than carried into the minutes");
    check(parse_reminder_time(QStringLiteral("  01:30  ")) == 90,
          "surrounding whitespace is tolerated, since it is invisible to the operator");
}

void crowding_is_reported_before_the_match() {
    check(reminder_crowding_warning(two_reminders()).isEmpty(),
          "reminders a minute apart raise no warning");

    ReminderConfig same;
    same.reminders = {{"a", 60, "甲", true}, {"b", 60, "乙", true}};
    const auto identical = reminder_crowding_warning(same);
    check(!identical.isEmpty() && identical.contains(QStringLiteral("01:00")),
          "two reminders on the same mark warn, naming the mark");

    ReminderConfig close;
    close.reminders = {{"a", 60, "甲", true}, {"b", 57, "乙", true}};
    check(!reminder_crowding_warning(close).isEmpty(),
          "reminders inside the two-pass window warn that playback will run late");

    // 关掉的那条不参与排队,所以不该报警 —— 否则操作手会为一条不会播的提醒
    // 去改一个没有问题的时间点。
    ReminderConfig disabled;
    disabled.reminders = {{"a", 60, "甲", true}, {"b", 60, "乙", false}};
    check(reminder_crowding_warning(disabled).isEmpty(),
          "a disabled reminder never counts as crowding");
}

void draft_maps_both_directions(const QString& root) {
    Rig rig(root);
    ReminderDialog dialog(&rig.controller);
    dialog.loadDraft(two_reminders());

    const auto draft = dialog.draft();
    check(draft.master_enabled, "the master switch loads from the configuration");
    check(draft.reminders.size() == 2, "every reminder reaches the table");
    check(draft.reminders[0].remaining_sec == 90 && draft.reminders[0].text == "收缩防线",
          "time and text survive the round trip through the table");
    check(draft.reminders[0].id == "a" && draft.reminders[1].id == "b",
          "ids are carried on the row, so editing text cannot re-arm a reminder mid-match");
}

// 草稿绝不能在按下保存之前进入控制器。改到一半关掉窗口是操作手常做的事,
// 那时磁盘上和内存里都必须还是上一次保存的内容。
void cancelling_a_draft_changes_nothing(const QString& root) {
    Rig rig(root);
    QString error;
    check(rig.controller.save(two_reminders(), &error), "a baseline configuration is saved");

    ReminderDialog dialog(&rig.controller);
    auto edited = two_reminders();
    edited.reminders[0].text = "改写但不保存";
    dialog.loadDraft(edited);
    check(dialog.draft().reminders[0].text == "改写但不保存", "the table holds the edit");
    check(rig.controller.config().reminders[0].text == "收缩防线",
          "the controller still holds the saved text, never the unsaved draft");

    dialog.reject();
    check(rig.controller.config().reminders[0].text == "收缩防线",
          "closing without saving leaves the saved configuration untouched");
}

void match_time_locks_editing_but_never_the_off_switch(const QString& root) {
    Rig rig(root);
    QString error;
    rig.controller.save(two_reminders(), &error);
    ReminderDialog dialog(&rig.controller);

    auto* save = dialog.findChild<QPushButton*>(QStringLiteral("reminderSave"));
    auto* add = dialog.findChild<QPushButton*>(QStringLiteral("reminderAdd"));
    auto* preview = dialog.findChild<QPushButton*>(QStringLiteral("reminderPreview"));
    auto* master = dialog.findChild<QCheckBox*>(QStringLiteral("reminderMaster"));
    auto* table = dialog.findChild<QTableWidget*>(QStringLiteral("reminderTable"));
    check(save && add && preview && master && table, "the dialog exposes its controls by name");
    check(save->isEnabled(), "saving is available before the match");

    rig.controller.observe(match_at(0, 120), 0);
    check(!save->isEnabled() && !add->isEnabled() && !preview->isEnabled(),
          "the match locks editing, so a mid-match edit cannot change what is armed");
    check(table->editTriggers() == QAbstractItemView::NoEditTriggers,
          "the table itself refuses edits, not merely the buttons");
    check(master->isEnabled(),
          "the master switch stays live: stopping playback is the one thing always permitted");

    check(rig.controller.uiState().master_enabled, "the switch reads as on while armed");
    master->setChecked(false);
    check(!rig.controller.uiState().master_enabled,
          "unchecking it mid-match silences this match immediately");
}

// 总开关赛中必须双向可用。只能关不能开等于把一次误触变成本局永久哑火 ——
// 操作手关掉去处理别的事,再想开回来时没有任何路径,而提醒本身早已合成好了。
// 「赛中只读」约束的是**编辑**(改时间/文案/声音),不是这个开关。
void match_time_allows_toggling_the_master_switch_both_ways(const QString& root) {
    Rig rig(root);
    QString error;
    rig.controller.save(two_reminders(), &error);
    ReminderDialog dialog(&rig.controller);
    rig.controller.observe(match_at(0, 120), 0);
    check(rig.controller.uiState().settings_locked, "the match really is locked for editing");

    auto* master = dialog.findChild<QCheckBox*>(QStringLiteral("reminderMaster"));
    master->setChecked(false);
    check(!rig.controller.uiState().master_enabled,
          "unchecking it mid-match silences this match immediately");

    master->setChecked(true);
    check(master->isChecked(), "the switch stays where the operator put it");
    check(rig.controller.uiState().master_enabled,
          "re-enabling mid-match is honoured, not sprung back");

    // 关键:恢复的不只是显示,而是真的重新布防 —— 否则开关是个装饰。
    check(rig.controller.uiState().status == ReminderStatus::Active,
          "the scheduler re-arms for the rest of the match instead of staying given up");

    // 编辑仍然锁着:放开开关不等于放开改动。
    auto* save = dialog.findChild<QPushButton*>(QStringLiteral("reminderSave"));
    auto* table = dialog.findChild<QTableWidget*>(QStringLiteral("reminderTable"));
    check(!save->isEnabled(), "editing stays locked even with the master switch back on");
    check(table->editTriggers() == QAbstractItemView::NoEditTriggers,
          "the table stays read-only mid-match");
}

// Regression(真机 QA 发现):上面那个用例的存盘配置是「开」的,所以赛中 setConfig
// 被拒绝也看不出问题。存盘是「关」时才暴露 —— 赛中打开改不进配置,而界面读的就是
// 配置,于是开关一按就弹回。覆盖状态必须能表达「赛中被打开」,不只是「被关掉」。
void a_match_can_enable_reminders_that_were_saved_off(const QString& root) {
    Rig rig(root);
    QString error;
    ReminderConfig off = two_reminders();
    off.master_enabled = false;
    rig.controller.save(off, &error);

    ReminderDialog dialog(&rig.controller);
    rig.controller.observe(match_at(0, 300, 1), 0);
    rig.controller.observe(match_at(1000, 280, 4), 1000);
    check(rig.controller.uiState().settings_locked, "the match is locked for editing");
    check(!rig.controller.uiState().master_enabled, "reminders start off, as saved");

    auto* master = dialog.findChild<QCheckBox*>(QStringLiteral("reminderMaster"));
    master->setChecked(true);
    check(master->isChecked(), "the switch holds instead of springing back");
    check(rig.controller.uiState().master_enabled,
          "enabling mid-match is honoured even though the saved config says off");

    rig.controller.observe(match_at(2000, 260, 4), 2000);
    check(master->isChecked(), "a fresh snapshot does not undo the operator's click");
    check(rig.controller.uiState().master_enabled,
          "the enable survives later snapshots instead of being overwritten by the saved value");

    auto* save = dialog.findChild<QPushButton*>(QStringLiteral("reminderSave"));
    check(!save->isEnabled(), "editing stays locked");
}

// Regression(真机 QA 发现,单元测试当时没覆盖):开关关着进入比赛时,调度器的
// master_off_ 分支早于设置 locked_ 就返回了,于是整局 settings_locked 一直是 false ——
// 赛中还能改时间和文案。锁定必须跟着「比赛开始」,与开关无关。
void a_match_entered_with_reminders_off_still_locks_editing(const QString& root) {
    Rig rig(root);
    QString error;
    ReminderConfig off = two_reminders();
    off.master_enabled = false;
    rig.controller.save(off, &error);

    ReminderDialog dialog(&rig.controller);
    rig.controller.observe(match_at(0, 300, 1), 0);
    check(!rig.controller.uiState().settings_locked, "stage 1 with reminders off is unlocked");

    rig.controller.observe(match_at(1000, 280, 4), 1000);
    check(rig.controller.uiState().settings_locked,
          "entering the match locks editing even with reminders switched off");

    auto* save = dialog.findChild<QPushButton*>(QStringLiteral("reminderSave"));
    auto* table = dialog.findChild<QTableWidget*>(QStringLiteral("reminderTable"));
    check(!save->isEnabled(), "saving is refused mid-match regardless of the master switch");
    check(table->editTriggers() == QAbstractItemView::NoEditTriggers,
          "the table is read-only mid-match regardless of the master switch");
}

// Regression: 准备阶段勾选总开关后,勾会在零点几秒内自己弹回。onMasterToggled 在
// 未锁定时直接 return,勾选只停在草稿里;而模拟器每帧 observe() 都发 stateChanged,
// refreshFromController() 便用控制器里那份**已保存**的旧值把勾抹掉。表现为按不动。
void pre_match_master_toggle_survives_a_controller_refresh(const QString& root) {
    Rig rig(root);
    QString error;
    ReminderConfig off = two_reminders();
    off.master_enabled = false;
    rig.controller.save(off, &error);

    ReminderDialog dialog(&rig.controller);
    rig.controller.observe(match_at(0, 300, 1), 0);
    check(!rig.controller.uiState().settings_locked, "stage 1 is unlocked");

    auto* master = dialog.findChild<QCheckBox*>(QStringLiteral("reminderMaster"));
    check(!master->isChecked(), "the switch starts off, matching the saved configuration");

    master->setChecked(true);
    check(master->isChecked(), "the operator's click registers");

    // 这一拍就是自动弹回的那一拍:准备阶段每帧都会走到。
    rig.controller.observe(match_at(250, 300, 1), 250);
    check(master->isChecked(),
          "the switch stays checked across a controller refresh instead of springing back");
}

void an_invalid_time_blocks_saving(const QString& root) {
    Rig rig(root);
    ReminderDialog dialog(&rig.controller);
    dialog.loadDraft(two_reminders());

    auto* table = dialog.findChild<QTableWidget*>(QStringLiteral("reminderTable"));
    table->item(0, 1)->setText(QStringLiteral("九十秒"));
    check(dialog.draft().reminders[0].remaining_sec < 0,
          "an unparseable cell surfaces as invalid rather than as zero seconds");

    auto* save = dialog.findChild<QPushButton*>(QStringLiteral("reminderSave"));
    save->click();
    check(rig.controller.config().reminders.empty(),
          "the invalid draft never reaches the controller");

    // 保存失败必须保留草稿。清掉等于让操作手把整套战术重打一遍。
    check(dialog.draft().reminders.size() == 2, "the rejected draft is preserved for correction");
}

void preview_plays_the_draft(const QString& root) {
    Rig rig(root);
    ReminderDialog dialog(&rig.controller);
    dialog.loadDraft(two_reminders());

    auto* table = dialog.findChild<QTableWidget*>(QStringLiteral("reminderTable"));
    table->selectRow(0);
    // 第 3 列才是播报内容。写到第 2 列会去改「开赛后」时间,那样断言的就不是
    // 「试听未保存的文本」而是「试听时间列被打坏后的那一行」。
    table->item(0, 3)->setText(QStringLiteral("尚未保存的战术"));
    dialog.findChild<QPushButton*>(QStringLiteral("reminderPreview"))->click();
    check(rig.backend->synthesized == 1,
          "preview synthesises the unsaved text instead of replaying a stale cached file");
}

// 超出本局时长的提醒永远不会触发:倒计时从 07:00 开始,剩余 08:00 这个点不存在。
// 保存它等于让操作手带着一条不会响的战术上场。
void a_time_beyond_the_match_is_refused(const QString& root) {
    Rig rig(root);
    ReminderDialog dialog(&rig.controller);
    dialog.loadDraft(two_reminders());

    auto* table = dialog.findChild<QTableWidget*>(QStringLiteral("reminderTable"));
    table->item(0, 1)->setText(QStringLiteral("08:00"));
    dialog.findChild<QPushButton*>(QStringLiteral("reminderSave"))->click();
    check(rig.controller.config().reminders.empty(),
          "a reminder past the start of the match never reaches the controller");

    // 反向:正计时列填超过本局时长,换算为负,同样必须被拒。
    table->item(0, 1)->setText(QStringLiteral("01:30"));
    table->item(0, 2)->setText(QStringLiteral("08:00"));
    check(dialog.draft().reminders[0].remaining_sec < 0,
          "an elapsed time past the final whistle surfaces as invalid, not as a clamped zero");
    dialog.findChild<QPushButton*>(QStringLiteral("reminderSave"))->click();
    check(rig.controller.config().reminders.empty(), "and it is refused as well");

    // 边界本身必须可保存:07:00 是开赛那一刻,是合法的触发点。
    table->item(0, 1)->setText(QStringLiteral("07:00"));
    dialog.findChild<QPushButton*>(QStringLiteral("reminderSave"))->click();
    check(rig.controller.config().reminders.size() == 2,
          "the kickoff instant itself is still accepted");
}

// 赛中刷新草稿会重建表格行,而新行带的是默认的可编辑标志。不重新上锁会让比赛期间
// 一次刷新就把只读表格变回可编辑。
void reloading_a_draft_mid_match_keeps_the_lock(const QString& root) {
    Rig rig(root);
    QString error;
    rig.controller.save(two_reminders(), &error);
    ReminderDialog dialog(&rig.controller);
    rig.controller.observe(match_at(0, 120), 0);

    auto* table = dialog.findChild<QTableWidget*>(QStringLiteral("reminderTable"));
    dialog.loadDraft(two_reminders());
    check(table->editTriggers() == QAbstractItemView::NoEditTriggers,
          "the table stays read-only after a mid-match refresh");
    check(!(table->item(0, 0)->flags() & Qt::ItemIsUserCheckable),
          "the rebuilt rows are not user-checkable either");
}

// 声音和语速同属「赛中只读」。漏掉它们会让操作手在比赛里改完设置,却发现保存被拒 ——
// 改动看着生效了,实际一条都没落下去。
void the_match_locks_the_voice_controls(const QString& root) {
    Rig rig(root);
    QString error;
    rig.controller.save(two_reminders(), &error);
    ReminderDialog dialog(&rig.controller);

    auto* rate = dialog.findChild<QSpinBox*>(QStringLiteral("reminderRate"));
    auto* voice = dialog.findChild<QComboBox*>(QStringLiteral("reminderVoice"));
    check(rate->isEnabled() && voice->isEnabled(), "both are editable before the match");

    rig.controller.observe(match_at(0, 120), 0);
    check(!rate->isEnabled() && !voice->isEnabled(),
          "the match locks the voice and rate, matching what save() will accept");

    rig.controller.observe(match_at(200, 0), 200);
    rig.controller.observe(match_at(300, 0, 5), 300);
    check(rate->isEnabled() && voice->isEnabled(), "settlement makes them editable again");
}

// 配置里 voice 留空表示「自动」。下拉必须显示实际在用的那个声音,显示列表第一项
// 会让操作手以为在用另一个声音。
void an_automatic_voice_shows_what_is_actually_in_use(const QString& root) {
    Rig rig(root);
    ReminderDialog dialog(&rig.controller);
    auto automatic = two_reminders();
    automatic.voice.clear();
    dialog.loadDraft(automatic);

    auto* voice = dialog.findChild<QComboBox*>(QStringLiteral("reminderVoice"));
    check(voice->currentText() == rig.controller.currentVoice(),
          "the dropdown names the voice the controller resolved automatic to");
    check(dialog.draft().voice == rig.controller.currentVoice().toStdString(),
          "saving from that state pins the resolved voice rather than silently changing it");
}

void adding_a_row_keeps_ids_unique(const QString& root) {
    Rig rig(root);
    ReminderDialog dialog(&rig.controller);
    dialog.loadDraft(two_reminders());
    dialog.findChild<QPushButton*>(QStringLiteral("reminderAdd"))->click();

    const auto draft = dialog.draft();
    check(draft.reminders.size() == 3, "the new row appears in the draft");
    std::set<std::string> ids;
    for (const auto& item : draft.reminders) ids.insert(item.id);
    check(ids.size() == draft.reminders.size(),
          "every id is distinct, which the repository requires to accept the save");
}
}

// 正计时和剩余时间是同一触发点的两种写法,换算必须严格互逆。差一秒就会让
// 操作手按「开赛满 3 分」设的提醒错开一秒触发。
void elapsed_and_remaining_are_exact_inverses() {
    check(elapsedToRemaining(0) == kMatchDurationSec,
          "at kickoff the whole match still remains");
    check(remainingToElapsed(kMatchDurationSec) == 0,
          "a full remaining clock means nothing has elapsed yet");
    check(elapsedToRemaining(180) == 240, "3 minutes in leaves 4 minutes of a 7-minute match");
    check(remainingToElapsed(240) == 180, "4 minutes left means 3 minutes have run");
    for (std::int32_t seconds = 0; seconds <= kMatchDurationSec; seconds += 7)
        check(elapsedToRemaining(remainingToElapsed(seconds)) == seconds,
              "the conversion round-trips without drifting a single second");
    // 越界不钳制:悄悄夹到 0 会把「开赛满 8 分钟」变成最后一秒触发的提醒。
    check(elapsedToRemaining(kMatchDurationSec + 60) == -60,
          "an elapsed time past the final whistle converts to a negative, not a clamped zero");
}

// 表格两列必须联动。不联动会让界面上出现两个矛盾读数,而只有一个会被保存。
void editing_either_time_column_updates_the_other(const QString& root) {
    Rig rig(root);
    ReminderDialog dialog(&rig.controller);
    dialog.loadDraft(two_reminders());

    auto* table = dialog.findChild<QTableWidget*>(QStringLiteral("reminderTable"));
    if (!table) throw std::runtime_error("reminderTable not found");

    check(table->item(0, 1)->text() == QStringLiteral("01:30"),
          "the remaining column shows the saved 90 seconds");
    check(table->item(0, 2)->text() == QStringLiteral("05:30"),
          "the elapsed column shows the same instant counted forwards");

    // 改正计时列 -> 剩余列跟随,且存下来的是协议语义的剩余秒数。
    table->item(0, 2)->setText(QStringLiteral("03:00"));
    check(table->item(0, 1)->text() == QStringLiteral("04:00"),
          "typing an elapsed time mirrors into the remaining column");
    check(dialog.draft().reminders[0].remaining_sec == 240,
          "what gets saved is the remaining seconds the protocol uses");

    // 反向:改剩余列 -> 正计时列跟随。
    table->item(0, 1)->setText(QStringLiteral("01:00"));
    check(table->item(0, 2)->text() == QStringLiteral("06:00"),
          "typing a remaining time mirrors into the elapsed column");

    // 打错时对面清空,而不是留一个看起来正常的旧读数。
    table->item(0, 1)->setText(QStringLiteral("啊"));
    check(table->item(0, 2)->text().isEmpty(),
          "an unparsable entry blanks its mirror instead of showing a stale plausible value");
}

void voice_and_rate_persist(const QString& root) {
    Rig rig(root);
    auto config = two_reminders();
    config.voice = "OtherVoice";
    config.rate_wpm = 250;

    QString error;
    check(rig.controller.save(config, &error), "a voice and rate reach the repository");
    check(rig.backend->voice() == QStringLiteral("OtherVoice"),
          "saving applies the voice to the speech backend");
    check(rig.backend->rate() == 250, "saving applies the rate to the speech backend");

    ReminderDialog dialog(&rig.controller);
    auto* rate = dialog.findChild<QSpinBox*>(QStringLiteral("reminderRate"));
    auto* voice = dialog.findChild<QComboBox*>(QStringLiteral("reminderVoice"));
    if (!rate || !voice) throw std::runtime_error("voice controls not found");
    check(rate->value() == 250, "the dialog shows the saved rate");
    check(voice->currentText() == QStringLiteral("OtherVoice"), "the dialog shows the saved voice");
    check(dialog.draft().rate_wpm == 250, "the draft carries the rate back out");
}

// 合成不出声的声音必须整体拒绝保存。否则磁盘上会留下一份指向哑声音的配置,
// 下次启动直接静默 —— 而操作手以为自己设好了。
void an_unusable_voice_is_refused(const QString& root) {
    Rig rig(root);
    auto config = two_reminders();
    config.voice = "GhostVoice";

    QString error;
    check(!rig.controller.save(config, &error), "an unusable voice is refused");
    check(!error.isEmpty(), "the refusal explains itself to the operator");
    check(rig.backend->voice() == QStringLiteral("FakeVoice"),
          "the previous voice survives a refused save");
    check(rig.controller.config().reminders.empty(),
          "a refused save leaves the stored configuration untouched");
}

// 语速进缓存键。不进的话改完语速试听,听到的还是旧语速那份音频。
void changing_the_rate_invalidates_cached_audio(const QString& root) {
    Rig rig(root);
    auto config = two_reminders();
    config.rate_wpm = 175;
    QString error;
    check(rig.controller.save(config, &error), "a baseline rate is saved");
    const auto before = rig.backend->synthesized;

    config.rate_wpm = 275;
    check(rig.controller.save(config, &error), "a new rate is saved");
    check(rig.backend->synthesized > before,
          "a rate change re-synthesizes instead of replaying the old cached audio");
}

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    try {
        QTemporaryDir dir;
        if (!dir.isValid()) throw std::runtime_error("cannot create temp dir");
        const auto at = [&](const char* name) {
            return QDir(dir.path()).filePath(QString::fromLatin1(name));
        };
        time_text_roundtrips();
        bad_time_is_rejected_not_zeroed();
        crowding_is_reported_before_the_match();
        draft_maps_both_directions(at("draft"));
        cancelling_a_draft_changes_nothing(at("cancel"));
        match_time_locks_editing_but_never_the_off_switch(at("lock"));
        match_time_allows_toggling_the_master_switch_both_ways(at("reenable"));
        a_match_can_enable_reminders_that_were_saved_off(at("savedoff"));
        pre_match_master_toggle_survives_a_controller_refresh(at("premaster"));
        a_match_entered_with_reminders_off_still_locks_editing(at("offlock"));
        an_invalid_time_blocks_saving(at("invalid"));
        preview_plays_the_draft(at("preview"));
        a_time_beyond_the_match_is_refused(at("beyond"));
        reloading_a_draft_mid_match_keeps_the_lock(at("relock"));
        the_match_locks_the_voice_controls(at("lockvoice"));
        an_automatic_voice_shows_what_is_actually_in_use(at("auto"));
        adding_a_row_keeps_ids_unique(at("ids"));
        elapsed_and_remaining_are_exact_inverses();
        editing_either_time_column_updates_the_other(at("mirror"));
        voice_and_rate_persist(at("voice"));
        an_unusable_voice_is_refused(at("ghost"));
        changing_the_rate_invalidates_cached_audio(at("rate"));
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    std::cout << "reminder_dialog: all checks passed\n";
    return 0;
}
