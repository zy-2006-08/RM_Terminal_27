#include "tactical_reminder_dialog.h"

#include "theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <vector>

namespace rm_terminal {
namespace {

constexpr int kColEnabled = 0;
constexpr int kColTime = 1;
constexpr int kColElapsed = 2;
constexpr int kColText = 3;

QString status_text(ReminderStatus status) {
    switch (status) {
        case ReminderStatus::Disabled: return QStringLiteral("已关闭");
        case ReminderStatus::Idle: return QStringLiteral("待命（非比赛阶段）");
        case ReminderStatus::WaitingForCountdown: return QStringLiteral("等待本局倒计时");
        case ReminderStatus::WaitingForPause: return QStringLiteral("等待暂停状态");
        case ReminderStatus::Suspended: return QStringLiteral("已暂停（链路或数据失效）");
        case ReminderStatus::ContinuityUncertain: return QStringLiteral("无法确认是否换局");
        case ReminderStatus::Paused: return QStringLiteral("比赛暂停中");
        case ReminderStatus::Active: return QStringLiteral("比赛中，已布防");
    }
    return {};
}

QString audio_text(AudioReadiness audio) {
    switch (audio) {
        case AudioReadiness::Idle: return QStringLiteral("语音空闲");
        case AudioReadiness::Preparing: return QStringLiteral("语音准备中");
        case AudioReadiness::Ready: return QStringLiteral("语音就绪");
        case AudioReadiness::Failed: return QStringLiteral("语音准备失败");
        case AudioReadiness::Unavailable: return QStringLiteral("本平台无离线语音");
    }
    return {};
}

// 新行的 id 必须唯一,否则落盘时会被仓库以「duplicate reminder id」拒绝。文本和
// 时间都可能重复,所以另起一个不与现有 id 冲突的编号,而不是从内容派生。
std::string unique_id(const QSet<QString>& taken) {
    for (int n = 1;; ++n) {
        const auto candidate = QStringLiteral("r%1").arg(n);
        if (!taken.contains(candidate)) return candidate.toStdString();
    }
}

}  // namespace

QString reminder_time_text(std::int32_t remaining_sec) {
    const auto total = std::max<std::int32_t>(0, remaining_sec);
    return QStringLiteral("%1:%2")
        .arg(total / 60, 2, 10, QLatin1Char('0'))
        .arg(total % 60, 2, 10, QLatin1Char('0'));
}

std::optional<std::int32_t> parse_reminder_time(const QString& text) {
    static const QRegularExpression pattern(QStringLiteral("^\\s*(\\d{1,2}):([0-5]\\d)\\s*$"));
    const auto match = pattern.match(text);
    if (!match.hasMatch()) return std::nullopt;
    const auto seconds = match.captured(1).toInt() * 60 + match.captured(2).toInt();
    // 上界与仓库共用同一个常量。界面允许输入而落盘被拒会让操作手无从判断哪里错了。
    if (seconds > kMaxReminderRemainingSec) return std::nullopt;
    return seconds;
}

QString reminder_crowding_warning(const ReminderConfig& config) {
    std::vector<std::int32_t> marks;
    for (const auto& item : config.reminders)
        if (item.enabled) marks.push_back(item.remaining_sec);
    std::sort(marks.begin(), marks.end(), std::greater<>());

    for (std::size_t i = 1; i < marks.size(); ++i) {
        const auto gap = marks[i - 1] - marks[i];
        if (gap >= kReminderCrowdedSec) continue;
        if (gap == 0)
            return QStringLiteral("有两条提醒设在同一时间点（%1），它们会依次播报而不是同时播报。")
                .arg(reminder_time_text(marks[i]));
        return QStringLiteral("%1 与 %2 相隔仅 %3 秒，后一条会排在前一条两遍播报之后，实际播报会晚几秒。")
            .arg(reminder_time_text(marks[i - 1]), reminder_time_text(marks[i]))
            .arg(gap);
    }
    return {};
}

ReminderDialog::ReminderDialog(ReminderController* controller, QWidget* parent)
    : QDialog(parent), controller_(controller) {
    setObjectName(QStringLiteral("reminderDialog"));
    setWindowTitle(QStringLiteral("战术提醒"));
    setStyleSheet(theme::styleSheet());
    setMinimumSize(560, 420);

    auto* root = new QVBoxLayout(this);

    master_ = new QCheckBox(QStringLiteral("启用战术提醒总开关"), this);
    master_->setObjectName(QStringLiteral("reminderMaster"));
    root->addWidget(master_);

    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("reminderStatus"));
    status_->setStyleSheet(QStringLiteral("color:#94a3b8;"));
    root->addWidget(status_);

    table_ = new QTableWidget(0, 4, this);
    table_->setObjectName(QStringLiteral("reminderTable"));
    table_->setHorizontalHeaderLabels({QStringLiteral("启用"), QStringLiteral("剩余时间 MM:SS"),
                                       QStringLiteral("开赛后 MM:SS"), QStringLiteral("播报内容")});
    auto* header = table_->horizontalHeader();
    // ResizeToContents 在这三列上算出的宽度比实际绘制的表头文字还窄,中文表头会被
    // 裁成「剩余时间 MM:S」。改为按表头字体自己量,再留出边框和内边距的余量。
    const QFontMetrics header_metrics(header->font());
    const auto fixed_width = [&](int column) {
        const auto label = table_->horizontalHeaderItem(column)->text();
        header->setSectionResizeMode(column, QHeaderView::Fixed);
        table_->setColumnWidth(column, header_metrics.horizontalAdvance(label) + 24);
    };
    fixed_width(kColEnabled);
    fixed_width(kColTime);
    fixed_width(kColElapsed);
    header->setSectionResizeMode(kColText, QHeaderView::Stretch);
    table_->verticalHeader()->setVisible(false);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    root->addWidget(table_, 1);

    auto* voice_row = new QHBoxLayout();
    // 显式给标签上色。主题的 QLabel 默认色是为深色面板选的,放在这一行的浅色
    // 控件旁边几乎看不见 —— 实测截图里「声音」「语速」两个字辨认不出。
    auto* voice_label = new QLabel(QStringLiteral("声音"), this);
    voice_label->setStyleSheet(QStringLiteral("color:#e2e8f0;"));
    voice_row->addWidget(voice_label);
    voice_ = new QComboBox(this);
    voice_->setObjectName(QStringLiteral("reminderVoice"));
    // 列系统里的中文声音。逐个做合成探测要 9.8 秒,不能放在打开窗口这条路径上;
    // 真正切换时后端会探测那一个,不出声就会拒绝保存。
    for (const auto& name : controller_->availableVoices()) voice_->addItem(name);
    if (voice_->count() == 0) {
        voice_->addItem(QStringLiteral("（系统无可用中文语音）"));
        voice_->setEnabled(false);
        voice_no_usable_ = true;
    }
    voice_row->addWidget(voice_, 1);

    auto* rate_label = new QLabel(QStringLiteral("语速"), this);
    rate_label->setStyleSheet(QStringLiteral("color:#e2e8f0;"));
    voice_row->addWidget(rate_label);
    rate_ = new QSpinBox(this);
    rate_->setObjectName(QStringLiteral("reminderRate"));
    rate_->setRange(kMinReminderRateWpm, kMaxReminderRateWpm);
    rate_->setSingleStep(25);
    rate_->setSuffix(QStringLiteral(" 词/分"));
    voice_row->addWidget(rate_);
    root->addLayout(voice_row);

    message_ = new QLabel(this);
    message_->setObjectName(QStringLiteral("reminderMessage"));
    message_->setWordWrap(true);
    root->addWidget(message_);

    auto* buttons = new QHBoxLayout();
    add_ = new QPushButton(QStringLiteral("新增"), this);
    add_->setObjectName(QStringLiteral("reminderAdd"));
    remove_ = new QPushButton(QStringLiteral("删除"), this);
    remove_->setObjectName(QStringLiteral("reminderRemove"));
    preview_ = new QPushButton(QStringLiteral("试听"), this);
    preview_->setObjectName(QStringLiteral("reminderPreview"));
    save_ = new QPushButton(QStringLiteral("保存"), this);
    save_->setObjectName(QStringLiteral("reminderSave"));
    close_ = new QPushButton(QStringLiteral("关闭"), this);
    close_->setObjectName(QStringLiteral("reminderClose"));
    buttons->addWidget(add_);
    buttons->addWidget(remove_);
    buttons->addWidget(preview_);
    buttons->addStretch(1);
    buttons->addWidget(save_);
    buttons->addWidget(close_);
    root->addLayout(buttons);

    QObject::connect(add_, &QPushButton::clicked, this, &ReminderDialog::onAdd);
    QObject::connect(remove_, &QPushButton::clicked, this, &ReminderDialog::onRemove);
    QObject::connect(preview_, &QPushButton::clicked, this, &ReminderDialog::onPreview);
    QObject::connect(save_, &QPushButton::clicked, this, &ReminderDialog::onSave);
    QObject::connect(close_, &QPushButton::clicked, this, &QDialog::reject);
    QObject::connect(master_, &QCheckBox::toggled, this, &ReminderDialog::onMasterToggled);
    QObject::connect(controller_, &ReminderController::stateChanged, this,
                     &ReminderDialog::refreshFromController);
    QObject::connect(table_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* changed) {
        if (syncing_) return;
        // 两列是同一个触发点的两种写法,改一列必须立刻同步另一列。不同步会让界面上
        // 出现两个互相矛盾的读数,而只有其中一个会被保存。
        if (changed && (changed->column() == kColTime || changed->column() == kColElapsed))
            mirrorTimeColumns(changed);
        const auto warning = reminder_crowding_warning(draft());
        showMessage(warning, false);
    });

    // 打开时以控制器里那份**已保存**的配置为草稿起点。取消后再打开必须回到已保存的
    // 状态,所以草稿不跨窗口生命周期保留。
    loadDraft(controller_->config());
    refreshFromController();
}

void ReminderDialog::appendRow(const ReminderItem& item) {
    const auto row = table_->rowCount();
    table_->insertRow(row);

    auto* enabled = new QTableWidgetItem();
    enabled->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    enabled->setCheckState(item.enabled ? Qt::Checked : Qt::Unchecked);
    table_->setItem(row, kColEnabled, enabled);

    auto* time = new QTableWidgetItem(reminder_time_text(item.remaining_sec));
    // id 挂在行上而不是从文本派生:改文字不该让这条提醒被当成新的一条,
    // 那会让它在本局重新播报一次。
    time->setData(Qt::UserRole, QString::fromStdString(item.id));
    table_->setItem(row, kColTime, time);

    table_->setItem(row, kColElapsed,
                    new QTableWidgetItem(reminder_time_text(remainingToElapsed(item.remaining_sec))));

    table_->setItem(row, kColText, new QTableWidgetItem(QString::fromStdString(item.text)));
}

void ReminderDialog::mirrorTimeColumns(QTableWidgetItem* source) {
    const auto row = source->row();
    const bool from_remaining = source->column() == kColTime;
    auto* target = table_->item(row, from_remaining ? kColElapsed : kColTime);
    if (!target) return;

    const auto parsed = parse_reminder_time(source->text());
    // 解析不出来时把对面清空,而不是留着上一次的值:留着会让一个打错的输入旁边
    // 显示一个看起来正常的读数,操作手会以为这行是好的。
    QString mirrored;
    if (parsed) {
        const auto converted =
            from_remaining ? remainingToElapsed(*parsed) : elapsedToRemaining(*parsed);
        // 换算为负 = 输入超出本局时长。这里也必须留空,不能写 reminder_time_text 的
        // 结果:它把负数钳成 00:00,会把「开赛满 8 分钟」显示成「剩余 00:00」——
        // 一个看着合法的读数,最后被保存成一条在最后一秒触发的提醒。
        if (converted >= 0) mirrored = reminder_time_text(converted);
    }
    syncing_ = true;
    target->setText(mirrored);
    syncing_ = false;
}

void ReminderDialog::loadDraft(const ReminderConfig& config) {
    syncing_ = true;
    table_->setRowCount(0);
    for (const auto& item : config.reminders) appendRow(item);
    master_->setChecked(config.master_enabled);
    rate_->setValue(config.rate_wpm ? config.rate_wpm : kDefaultReminderRateWpm);
    // 配置里的声音可能在这台机器上不存在(换机器或声音被删)。找不到就保持下拉当前
    // 选中的第一个可用声音,而不是插入一个选不中的条目。
    //
    // 留空表示「自动」,这时要显示控制器实际在用的那个声音:显示列表第一项会让
    // 操作手以为在用普通版,而自动挑中的是 (Enhanced)。
    const auto saved_voice = config.voice.empty() ? controller_->currentVoice()
                                                  : QString::fromStdString(config.voice);
    if (const auto index = voice_->findText(saved_voice); index >= 0) voice_->setCurrentIndex(index);
    syncing_ = false;

    // 重新建过表格行,行上的可勾选标志是新的默认值。赛中锁定必须再套一遍,
    // 否则比赛期间刷新一次草稿就会把只读表格变回可编辑。
    applyLock(controller_->uiState().settings_locked);
}

ReminderConfig ReminderDialog::draft() const {
    ReminderConfig out;
    out.version = kReminderConfigVersion;
    out.master_enabled = master_->isChecked();
    out.rate_wpm = rate_->value();
    // 判据是「有没有可用声音」,不是控件当前是否 enabled:赛中锁定同样会禁用这个
    // 下拉,拿 isEnabled() 当条件会把已保存的声音丢成空(=自动)。
    if (!voice_no_usable_) out.voice = voice_->currentText().toStdString();
    for (int row = 0; row < table_->rowCount(); ++row) {
        const auto* enabled = table_->item(row, kColEnabled);
        const auto* time = table_->item(row, kColTime);
        const auto* text = table_->item(row, kColText);
        if (!enabled || !time || !text) continue;

        ReminderItem item;
        item.id = time->data(Qt::UserRole).toString().toStdString();
        item.enabled = enabled->checkState() == Qt::Checked;
        item.text = text->text().trimmed().toStdString();
        // 解析失败时留 -1,让 onSave 的校验去报告具体行号。这里静默填 0 会把
        // 一个打错的时间保存成「剩余 0 秒」。
        const auto seconds = parse_reminder_time(time->text());
        item.remaining_sec = seconds ? *seconds : -1;
        out.reminders.push_back(item);
    }
    return out;
}

void ReminderDialog::onAdd() {
    QSet<QString> taken;
    for (int row = 0; row < table_->rowCount(); ++row)
        if (const auto* time = table_->item(row, kColTime))
            taken.insert(time->data(Qt::UserRole).toString());

    syncing_ = true;
    appendRow({unique_id(taken), 60, "", true});
    syncing_ = false;
    table_->selectRow(table_->rowCount() - 1);
    table_->editItem(table_->item(table_->rowCount() - 1, kColText));
}

void ReminderDialog::onRemove() {
    const auto row = table_->currentRow();
    if (row < 0) {
        showMessage(QStringLiteral("请先选中要删除的提醒。"), true);
        return;
    }
    table_->removeRow(row);
}

void ReminderDialog::onPreview() {
    const auto row = table_->currentRow();
    if (row < 0) {
        showMessage(QStringLiteral("请先选中要试听的提醒。"), true);
        return;
    }
    const auto config = draft();
    if (row >= static_cast<int>(config.reminders.size())) return;

    QString error;
    // 试听走草稿而不是已保存的配置:操作手应该在保存前就能听见自己写的内容,
    // 声音和语速同样取草稿值,否则改完设置试听听到的还是上次保存的那一套。
    if (!controller_->preview(config.reminders[static_cast<std::size_t>(row)],
                              QString::fromStdString(config.voice), config.rate_wpm, &error))
        showMessage(error, true);
    else
        showMessage(QStringLiteral("正在试听…"), false);
}

void ReminderDialog::onSave() {
    const auto config = draft();
    for (std::size_t i = 0; i < config.reminders.size(); ++i) {
        const auto& item = config.reminders[i];
        const auto line = static_cast<int>(i) + 1;
        if (item.remaining_sec < 0) {
            showMessage(QStringLiteral("第 %1 行时间无效，应为 MM:SS，且两列都要在本局时长内。")
                            .arg(line),
                        true);
            return;
        }
        // 超过本局时长的提醒永远不会触发:倒计时从 07:00 开始,剩余 08:00 这个点
        // 不存在。保存它等于让操作手带着一条不会响的战术上场。
        if (item.remaining_sec > kMatchDurationSec) {
            showMessage(QStringLiteral("第 %1 行超出本局时长，剩余时间不能大于 %2。")
                            .arg(line)
                            .arg(reminder_time_text(kMatchDurationSec)),
                        true);
            return;
        }
        if (item.text.empty()) {
            showMessage(QStringLiteral("第 %1 行播报内容为空。").arg(line), true);
            return;
        }
        if (QString::fromStdString(item.text).size() > static_cast<int>(kMaxReminderTextChars)) {
            showMessage(QStringLiteral("第 %1 行播报内容超过 %2 字。")
                            .arg(line)
                            .arg(kMaxReminderTextChars),
                        true);
            return;
        }
    }

    QString error;
    // 保存失败时**保留草稿**:清掉等于让操作手把刚填的一整套战术重打一遍。
    if (!controller_->save(config, &error)) {
        showMessage(error, true);
        return;
    }
    const auto warning = reminder_crowding_warning(config);
    showMessage(warning.isEmpty() ? QStringLiteral("已保存。")
                                  : QStringLiteral("已保存。注意：%1").arg(warning),
                !warning.isEmpty());
}

void ReminderDialog::onMasterToggled(bool checked) {
    if (syncing_) return;
    // 总开关立即生效并落盘,赛中赛前都一样:它不是草稿的一部分。
    //
    // 曾经在未锁定时直接 return,把勾选只留在草稿里 —— 而 refreshFromController()
    // 每次 stateChanged 都用已保存值回写这个勾。比赛数据每帧都在发,于是准备阶段
    // 勾上零点几秒就自己弹回,表现为「按不动」。
    controller_->setMasterEnabled(checked);
    showMessage(checked ? QStringLiteral("已启用战术提醒。")
                        : QStringLiteral("已关闭战术提醒，本局不再播报。"),
                false);
}

void ReminderDialog::applyLock(bool locked) {
    table_->setEditTriggers(locked ? QAbstractItemView::NoEditTriggers
                                   : QAbstractItemView::DoubleClicked
                                         | QAbstractItemView::EditKeyPressed);
    for (int row = 0; row < table_->rowCount(); ++row) {
        if (auto* enabled = table_->item(row, kColEnabled)) {
            auto flags = enabled->flags();
            flags.setFlag(Qt::ItemIsUserCheckable, !locked);
            enabled->setFlags(flags);
        }
    }
    add_->setEnabled(!locked);
    remove_->setEnabled(!locked);
    preview_->setEnabled(!locked);
    save_->setEnabled(!locked);
    // 声音和语速同样属于「赛中只读」。漏掉它们会让操作手在比赛里改完设置,
    // 却发现保存被拒 —— 改动看着生效了,实际一条都没落下去。
    rate_->setEnabled(!locked);
    // 系统无可用中文语音时下拉本来就是禁用的,解锁不该把它打开:那会让一个
    // 选不出有效声音的下拉变成可选。
    if (voice_->count() > 0 && !voice_no_usable_) voice_->setEnabled(!locked);
    // master_ 刻意不禁用:赛中关闭提醒是操作手必须随时能做的事。
}

void ReminderDialog::refreshFromController() {
    const auto state = controller_->uiState();

    syncing_ = true;
    master_->setChecked(state.master_enabled);
    syncing_ = false;

    applyLock(state.settings_locked);
    status_->setText(QStringLiteral("%1 · %2 · 已启用 %3 条%4")
                         .arg(status_text(state.status), audio_text(state.audio))
                         .arg(state.enabled_count)
                         .arg(state.settings_locked ? QStringLiteral(" · 比赛中，设置为只读")
                                                    : QString()));
    if (!state.message.isEmpty()) showMessage(state.message, true);
}

void ReminderDialog::showMessage(const QString& text, bool error) {
    message_->setText(text);
    message_->setStyleSheet(error ? QStringLiteral("color:#ff9aa2;")
                                  : QStringLiteral("color:#40d68a;"));
}

}
