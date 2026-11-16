#include "assembly_flow.h"

#include "theme.h"

#include <QEvent>
#include <QPainter>
#include <QStringList>

#include <algorithm>
#include <tuple>

namespace rm_terminal {

namespace {

// TechCoreMotionStateSync 1 秒一条。按了 Enter 以后等三条多:还不动,就是服务器没让开始。
constexpr MonotonicMs kStartWaitMs = 3500;
constexpr MonotonicMs kResultMs = 5000;
constexpr MonotonicMs kHintMs = 3000;
constexpr MonotonicMs kNotStartedMs = 6000;
constexpr MonotonicMs kNoteMs = 3000;
// 科技核心回到初始位置以后多久之内来的事件 15 还算刚才那次:同步 1 秒一条,事件可能比它晚到。
constexpr MonotonicMs kLateResultMs = 3000;

constexpr int kProgressWidth = 440;
constexpr int kPad = 14;
constexpr int kTimerHeight = 20;
constexpr int kTitleHeight = 28;
constexpr int kChipHeight = 26;
constexpr int kChipWidth = 66;
constexpr int kArrowWidth = 18;
constexpr int kDetailHeight = 20;
constexpr int kKeysHeight = 26;
constexpr int kFooterHeight = 24;
constexpr int kGap = 6;
// 和远程兑换框一样离底边 24。
constexpr int kBottomMargin = 24;

template <class T>
std::optional<T> present(const Field<T>& field) {
    if (field.quality != Quality::Valid || !field.value) return std::nullopt;
    return field.value;
}

const std::array<QString, 3> kStepNames{QStringLiteral("放入"), QStringLiteral("平移"), QStringLiteral("旋转")};

// 键帽和步骤小格:绿色是做完了、现在能按,灰色是还没有。
void paint_cap(QPainter& painter, const QRectF& box, const QString& text, bool lit, int px = 13) {
    theme::paintGlass(painter, box, 3, false, lit ? QColor(28, 64, 50, 200) : QColor(255, 255, 255, 8),
                      lit ? theme::kOk : theme::kRowBorder);
    painter.setFont(theme::labelPx(px, QFont::DemiBold));
    painter.setPen(lit ? theme::kOkText : theme::kTextMuted);
    painter.drawText(box, Qt::AlignCenter, text);
}

void raise_to_top(QWidget* widget) {
    const QObjectList& siblings = widget->parentWidget()->children();
    for (auto it = siblings.crbegin(); it != siblings.crend(); ++it) {
        auto* other = qobject_cast<QWidget*>(*it);
        if (!other || other->isWindow()) continue;
        if (other != widget) widget->raise();
        return;
    }
}

}  // namespace

bool operator==(const AssemblyRow& a, const AssemblyRow& b) {
    return std::tie(a.shown, a.selectable, a.selected, a.blocked, a.note) ==
           std::tie(b.shown, b.selectable, b.selected, b.blocked, b.note);
}

bool operator==(const AssemblyProgressView& a, const AssemblyProgressView& b) {
    return std::tie(a.visible, a.timer, a.title, a.detail, a.steps, a.keys, a.confirm_key, a.confirm_lit, a.footer,
                    a.tone) == std::tie(b.visible, b.timer, b.title, b.detail, b.steps, b.keys, b.confirm_key,
                                        b.confirm_lit, b.footer, b.tone);
}

AssemblyView assembly_view(const Snapshot& snapshot) {
    AssemblyView view;
    const TechCoreFields<Field>& core = snapshot.tech_core;
    view.maximum_difficulty = present(core.maximum_difficulty);
    view.basic_state = present(core.basic_state);
    view.remain_time_step = present(core.remain_time_step);
    view.remain_time_all = present(core.remain_time_all);
    view.enemy_core_status = present(core.enemy_core_status);
    const auto step = [](const Field<std::uint32_t>& field) -> std::optional<bool> {
        if (const auto raw = present(field)) return *raw != 0;
        return std::nullopt;
    };
    view.putin = step(core.putin_state);
    view.move = step(core.move_state);
    view.rotate = step(core.rotate_state);
    view.tech_level = present(snapshot.game.tech_level);

    if (snapshot.self) {
        const auto robot = snapshot.robots.find(*snapshot.self);
        if (robot != snapshot.robots.end()) {
            if (const auto raw = present(robot->second.modules.modular_42mm)) view.modular_42mm = *raw != 0;
        }
    }

    // recent() 新的在前。Event 15 是装配结果(文字在 match_event.cpp 已翻成中文,成功是 level 1),
    // Event 14 是重装战亡后临时激活。各取最新一条。
    for (const EventRecord& record : snapshot.events.recent(snapshot.events.capacity())) {
        if (record.event_id == 15 && !view.last_result_at) {
            view.last_result = QString::fromStdString(record.text);
            view.last_result_at = record.received_at;
            view.last_result_level = record.level;
        } else if (record.event_id == 14 && !view.buffer_at) {
            view.buffer_at = record.received_at;
        }
        if (view.last_result_at && view.buffer_at) break;
    }
    return view;
}

int assembly_step_count(std::uint32_t level) {
    switch (level) {
    case 1: return 1;
    case 2: return 2;
    default: return 3;
    }
}

QString assembly_level_name(std::uint32_t level) {
    switch (level) {
    case 1: return QStringLiteral("一级");
    case 2: return QStringLiteral("二级");
    case 3: return QStringLiteral("三级");
    case 4: return QStringLiteral("四级");
    default: return QStringLiteral("%1 级").arg(level);
    }
}

QString assembly_reward(std::uint32_t level, bool first) {
    if (first) {
        switch (level) {
        case 1: return QStringLiteral("升 5 级，此后每 10 秒 +40 金");
        case 2: return QStringLiteral("升 7 级，等级上限提到 7，每 10 秒再 +20 金");
        case 3: return QStringLiteral("升 10 级，全队 25% 防御，解锁 42mm，每 10 秒再 +20 金");
        case 4: return QStringLiteral("全队 50% 防御，每 10 秒再 +40 金");
        default: return QString();
        }
    }
    switch (level) {
    case 1: return QStringLiteral("每 10 秒再 +4 金");
    case 2: return QStringLiteral("每 10 秒再 +8 金");
    case 3: return QStringLiteral("每 10 秒再 +12 金");
    case 4: return QStringLiteral("四级只能装成一次");
    default: return QString();
    }
}

void AssemblyState::update(const Inputs& inputs, MonotonicMs now) {
    in_ = inputs;
    now_ = now;
    const AssemblyView& view = in_.view;
    // 终端打开以前的结果不算:只认这之后新来的 Event 15。
    if (!primed_) {
        primed_ = true;
        seen_result_at_ = view.last_result_at.value_or(-1);
    }
    const bool away = view.basic_state && (*view.basic_state == 2 || *view.basic_state == 3);
    if (!away) {
        if (started_at_) {
            ended_level_ = level_;
            ended_until_ = now + kLateResultMs;
        }
        started_at_.reset();
        finished_ = false;
    } else if (!started_at_ && pending_) {
        // 只认终端自己开始的那一次:对方装四级时,己方科技核心也会被移过去(规则手册 5.4.3)。
        started_at_ = now;
        level_ = pending_->level;
        pending_.reset();
    }
    if (view.last_result_at && *view.last_result_at > seen_result_at_) {
        seen_result_at_ = *view.last_result_at;
        std::optional<std::uint32_t> level;
        if (pending_) level = pending_->level;
        else if (started_at_) level = level_;
        else if (now < ended_until_) level = ended_level_;
        QString text = view.last_result;
        if (view.last_result_level == 1 && level)
            text = QStringLiteral("%1科技核心装配成功").arg(assembly_level_name(*level));
        result_ = Result{text, view.last_result_level, level, now + kResultMs};
        // 结果来了以后科技核心要等下一条同步(1 秒)才回初始位置,这段时间已经不算在装。
        finished_ = started_at_.has_value();
        pending_.reset();
        hint_until_ = 0;
    }
    // 失败了科技核心还没回去:结果一直留着,等它回初始位置 5 秒后再收起。
    if (result_ && result_->level_code != 1 && started_at_) result_->until = std::max(result_->until, now + kResultMs);
    if (pending_ && now - pending_->at >= kStartWaitMs) {
        pending_.reset();
        not_started_until_ = now + kNotStartedMs;
    }
    if (selected_ && !selectable(*selected_)) selected_.reset();
}

bool AssemblyState::active() const { return started_at_.has_value() && !finished_; }

QString AssemblyState::blockReason() const {
    if (!in_.link_up) return QStringLiteral("断线，指令发不出去");
    if (!in_.in_match) return QStringLiteral("比赛开始后才能装配");
    if (in_.dead) return QStringLiteral("阵亡，复活后才能装配");
    const auto top = in_.view.maximum_difficulty;
    if (!top || *top == 0) return QStringLiteral("还没收到可选的装配难度");
    // 规则手册 5.4.3:一方在装四级时,另一方不能进入装配流程。
    if (*top >= 4 && in_.view.enemy_core_status == std::optional<std::uint32_t>(2))
        return QStringLiteral("对方正在装配四级难度，现在不能装配");
    return QString();
}

bool AssemblyState::selectable(std::uint32_t level) const {
    if (level < 1 || level > 4 || !blockReason().isEmpty()) return false;
    if (level > *in_.view.maximum_difficulty) return false;
    // 规则手册 5.4.3:必须先装成上一级才能选下一级。科技等级没收到时不拦,交给服务器。
    if (in_.view.tech_level && level > *in_.view.tech_level + 1) return false;
    if (level == 4) {
        if (in_.view.tech_level.value_or(0) >= 4) return false;                       // 表 5-12:仅限完成一次
        if (in_.view.enemy_core_status == std::optional<std::uint32_t>(1)) return false;  // 对方在装,不能选四级
    }
    return true;
}

std::optional<std::uint32_t> AssemblyState::recommended() const {
    const std::uint32_t next = std::min<std::uint32_t>(in_.view.tech_level.value_or(0) + 1, 4);
    for (std::uint32_t level = next; level >= 1; --level)
        if (selectable(level)) return level;
    return std::nullopt;
}

AssemblyRow AssemblyState::row() const {
    AssemblyRow row;
    row.shown = true;
    if (const QString reason = blockReason(); !reason.isEmpty()) {
        row.blocked = true;
        row.note = reason;
        return row;
    }
    for (std::uint32_t level = 1; level <= 4; ++level) row.selectable[level - 1] = selectable(level);
    if (selected_ && selectable(*selected_)) row.selected = selected_;
    if (now_ < note_until_) {
        row.note = note_;
    } else if (row.selected) {
        const bool first = in_.view.tech_level.value_or(0) < *row.selected;
        row.note = QStringLiteral("%1 · %2：%3")
                       .arg(assembly_level_name(*row.selected), first ? QStringLiteral("首次") : QStringLiteral("再装"),
                            assembly_reward(*row.selected, first));
    } else {
        row.note = QStringLiteral("按 1–4 选难度，Enter 开始");
    }
    return row;
}

int AssemblyState::stepCount() const { return assembly_step_count(level_.value_or(0)); }

std::vector<std::pair<QString, bool>> AssemblyState::steps() const {
    const std::array<std::optional<bool>, 3> done{in_.view.putin, in_.view.move, in_.view.rotate};
    std::vector<std::pair<QString, bool>> out;
    for (int index = 0; index < stepCount(); ++index)
        out.emplace_back(kStepNames[static_cast<std::size_t>(index)], done[static_cast<std::size_t>(index)].value_or(false));
    return out;
}

bool AssemblyState::stepsDone() const {
    const auto list = steps();
    return std::all_of(list.begin(), list.end(), [](const auto& step) { return step.second; });
}

AssemblyProgressView AssemblyState::progress() const {
    AssemblyProgressView view;
    if (result_ && now_ < result_->until) {
        view.visible = true;
        view.title = result_->text;
        view.tone = result_->level_code == 1   ? AssemblyProgressView::Tone::Good
                    : result_->level_code == 0 ? AssemblyProgressView::Tone::Normal
                                               : AssemblyProgressView::Tone::Bad;
        if (result_->level) view.footer = assembly_level_name(*result_->level) + QStringLiteral("难度");
        // 2026 手册第 14 页:失败时面板上有「L 确定」。科技核心还在外面时 L 有用。
        if (result_->level_code != 1 && started_at_) {
            view.keys = true;
            view.confirm_key = false;
        }
        return view;
    }
    const AssemblyView& core = in_.view;
    if (active()) {
        view.visible = true;
        view.keys = true;
        if (level_) view.footer = assembly_level_name(*level_) + QStringLiteral("难度");
        const bool buffer = core.buffer_at && started_at_ && *core.buffer_at >= *started_at_;
        if (buffer) {
            // 2026 手册第 15 页「临时激活」:战亡的车被临时激活,操作手要把它开离科技核心。
            view.title = QStringLiteral("已临时激活");
            view.detail = QStringLiteral("请让车离开科技核心，再按 L 退出装配");
            if (core.remain_time_step && *core.remain_time_step > 0)
                view.timer = QStringLiteral("缓冲还剩 %1s").arg(*core.remain_time_step);
            view.confirm_key = false;
            view.tone = AssemblyProgressView::Tone::Bad;
        } else if (core.basic_state == std::optional<std::uint32_t>(2)) {
            view.title = QStringLiteral("运动中…");
            view.detail = QStringLiteral("前往装配点");
            view.steps = steps();
        } else {
            view.steps = steps();
            const bool done = stepsDone();
            view.title = done ? QStringLiteral("确认装配前请移出装配机构") : QStringLiteral("请装配");
            view.confirm_lit = done;
            if (level_ == std::optional<std::uint32_t>(4)) {
                QStringList timers;
                if (core.remain_time_all) timers << QStringLiteral("装配倒计时 %1s").arg(*core.remain_time_all);
                if (core.remain_time_step && *core.remain_time_step > 0)
                    timers << QStringLiteral("本步还剩 %1s").arg(*core.remain_time_step);
                view.timer = timers.join(QStringLiteral(" · "));
            }
        }
        if (now_ < hint_until_) view.detail = hint_;
        return view;
    }
    if (pending_) {
        view.visible = true;
        view.keys = true;
        view.title = QStringLiteral("开始装配…");
        view.detail = QStringLiteral("等科技核心动起来");
        view.footer = assembly_level_name(pending_->level) + QStringLiteral("难度");
        return view;
    }
    if (now_ < not_started_until_) {
        view.visible = true;
        view.title = QStringLiteral("没开始装配");
        view.detail = QStringLiteral("要站在装配区上；阵亡、罚下、虚弱时也开不了");
        view.tone = AssemblyProgressView::Tone::Bad;
    }
    return view;
}

void AssemblyState::boxOpened() {
    selected_ = recommended();
    note_until_ = 0;
}

void AssemblyState::choose(std::uint32_t level) {
    if (selectable(level)) {
        selected_ = level;
        note_until_ = 0;
        return;
    }
    // 整排灰着时那一行已经写了原因。
    if (!blockReason().isEmpty() || level < 1 || level > 4) return;
    const std::uint32_t top = *in_.view.maximum_difficulty;
    const std::optional<std::uint32_t> done = in_.view.tech_level;
    if (level == 4 && done && *done >= 4)
        note(QStringLiteral("四级只能装成一次"));
    else if (done && level > *done + 1)
        note(QStringLiteral("先装成%1才能选%2").arg(assembly_level_name(level - 1), assembly_level_name(level)));
    // 表 5-11:开赛后每满 1 分钟多开一级。
    else if (level > top)
        note(QStringLiteral("%1还没开放：开赛后每满 1 分钟多开一级").arg(assembly_level_name(level)));
    else
        note(QStringLiteral("对方正在装配，等它装完才能选四级"));
}

std::optional<AssemblySend> AssemblyState::start() {
    if (!blockReason().isEmpty()) return std::nullopt;
    if (!selected_ || !selectable(*selected_)) {
        note(QStringLiteral("先按 1–4 选难度"));
        return std::nullopt;
    }
    return AssemblySend{0, *selected_};
}

AssemblyState::Key AssemblyState::confirm() {
    if (!active() || in_.dead) return {};
    Key key;
    key.consumed = true;
    const bool buffer = in_.view.buffer_at && started_at_ && *in_.view.buffer_at >= *started_at_;
    if (buffer) {
        hint(QStringLiteral("临时激活中只能按 L 退出装配"), kHintMs);
        return key;
    }
    if (in_.view.basic_state != std::optional<std::uint32_t>(3)) {
        hint(QStringLiteral("科技核心还没到位"), kHintMs);
        return key;
    }
    if (!stepsDone()) {
        QStringList missing;
        for (const auto& [name, done] : steps())
            if (!done) missing << name;
        hint(QStringLiteral("还差：%1").arg(missing.join(QStringLiteral("、"))), kHintMs);
        return key;
    }
    key.send = AssemblySend{1, level_.value_or(0)};
    return key;
}

AssemblyState::Key AssemblyState::exit() {
    if (!canExit()) return {};
    const std::uint32_t level = pending_ ? pending_->level : level_.value_or(0);
    return Key{true, AssemblySend{2, level}};
}

void AssemblyState::busy() { hint(QStringLiteral("装配中不能开兑换框，按 L 退出装配"), kHintMs); }

void AssemblyState::sent(const AssemblySend& send) {
    if (send.operation != 0) return;
    pending_ = Pending{send.difficulty, now_};
    level_ = send.difficulty;
    result_.reset();
    hint_until_ = 0;
    not_started_until_ = 0;
}

void AssemblyState::hint(const QString& text, MonotonicMs ms) {
    hint_ = text;
    hint_until_ = now_ + ms;
}

void AssemblyState::note(const QString& text) {
    note_ = text;
    note_until_ = now_ + kNoteMs;
}

AssemblyProgress::AssemblyProgress(QWidget* host) : QWidget(host) {
    setObjectName(QStringLiteral("assemblyProgress"));
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setVisible(false);
}

void AssemblyProgress::setView(const AssemblyProgressView& view) {
    if (view == view_) return;
    view_ = view;
    updateGeometry();
    update();
}

QSize AssemblyProgress::sizeHint() const {
    int height = kPad * 2 + kTitleHeight;
    if (!view_.timer.isEmpty()) height += kTimerHeight + kGap;
    if (!view_.steps.empty()) height += kChipHeight + kGap;
    if (!view_.detail.isEmpty()) height += kDetailHeight;
    if (view_.keys) height += kKeysHeight + kGap;
    if (!view_.footer.isEmpty()) height += kFooterHeight;
    return QSize(kProgressWidth, height);
}

void AssemblyProgress::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    theme::paintGlass(painter, QRectF(rect()).adjusted(1, 1, -1, -1), 10, false, theme::kOverlay,
                      theme::kBorderBright, true);
    const qreal inner = width() - kPad * 2;
    qreal y = kPad;
    if (!view_.timer.isEmpty()) {
        painter.setFont(theme::labelPx(13, QFont::DemiBold));
        painter.setPen(view_.tone == AssemblyProgressView::Tone::Bad ? theme::kDangerText : theme::kWarn);
        painter.drawText(QRectF(kPad, y, inner, kTimerHeight), Qt::AlignCenter, view_.timer);
        y += kTimerHeight + kGap;
    }
    const QColor title_ink = view_.tone == AssemblyProgressView::Tone::Good  ? theme::kOkText
                             : view_.tone == AssemblyProgressView::Tone::Bad ? theme::kDangerText
                                                                             : theme::kTextPrimary;
    painter.setFont(theme::labelPx(18, QFont::Bold));
    painter.setPen(title_ink);
    const QRectF title(kPad, y, inner, kTitleHeight);
    painter.drawText(title, Qt::AlignCenter,
                     painter.fontMetrics().elidedText(view_.title, Qt::ElideRight, static_cast<int>(inner)));
    y += kTitleHeight;
    if (!view_.steps.empty()) {
        y += kGap;
        const int count = static_cast<int>(view_.steps.size());
        const qreal total = count * kChipWidth + (count - 1) * kArrowWidth;
        qreal x = (width() - total) / 2;
        for (int index = 0; index < count; ++index) {
            const auto& [name, done] = view_.steps[static_cast<std::size_t>(index)];
            paint_cap(painter, QRectF(x, y, kChipWidth, kChipHeight), name, done);
            x += kChipWidth;
            if (index + 1 < count) {
                painter.setFont(theme::labelPx(13));
                painter.setPen(theme::kTextMuted);
                painter.drawText(QRectF(x, y, kArrowWidth, kChipHeight), Qt::AlignCenter, QStringLiteral("›"));
                x += kArrowWidth;
            }
        }
        y += kChipHeight;
    }
    if (!view_.detail.isEmpty()) {
        painter.setFont(theme::labelPx(13));
        painter.setPen(view_.tone == AssemblyProgressView::Tone::Bad ? theme::kDangerText : theme::kTextSecondary);
        const QRectF detail(kPad, y, inner, kDetailHeight);
        painter.drawText(detail, Qt::AlignCenter,
                         painter.fontMetrics().elidedText(view_.detail, Qt::ElideRight, static_cast<int>(inner)));
        y += kDetailHeight;
    }
    if (view_.keys) {
        y += kGap;
        // 键帽 + 字:「[Y] 确认   [L] 退出装配」,整组居中。
        painter.setFont(theme::labelPx(13, QFont::DemiBold));
        const QFontMetrics metrics = painter.fontMetrics();
        const QString confirm_text = QStringLiteral("确认");
        const QString exit_text = QStringLiteral("退出装配");
        const qreal cap = 26;
        const qreal spacing = 24;
        const qreal confirm_width = view_.confirm_key ? cap + 6 + metrics.horizontalAdvance(confirm_text) : 0;
        const qreal exit_width = cap + 6 + metrics.horizontalAdvance(exit_text);
        qreal x = (width() - confirm_width - exit_width - (view_.confirm_key ? spacing : 0)) / 2;
        if (view_.confirm_key) {
            paint_cap(painter, QRectF(x, y, cap, kKeysHeight), QStringLiteral("Y"), view_.confirm_lit);
            painter.setFont(theme::labelPx(13, QFont::DemiBold));
            painter.setPen(view_.confirm_lit ? theme::kOkText : theme::kTextMuted);
            painter.drawText(QRectF(x + cap + 6, y, confirm_width - cap - 6, kKeysHeight),
                             Qt::AlignLeft | Qt::AlignVCenter, confirm_text);
            x += confirm_width + spacing;
        }
        theme::paintGlass(painter, QRectF(x, y, cap, kKeysHeight), 3, false, theme::kChip, theme::kBorder);
        painter.setFont(theme::labelPx(13, QFont::DemiBold));
        painter.setPen(theme::kTextPrimary);
        painter.drawText(QRectF(x, y, cap, kKeysHeight), Qt::AlignCenter, QStringLiteral("L"));
        painter.drawText(QRectF(x + cap + 6, y, exit_width - cap - 6, kKeysHeight), Qt::AlignLeft | Qt::AlignVCenter,
                         exit_text);
        y += kKeysHeight;
    }
    if (!view_.footer.isEmpty()) {
        y += 4;
        painter.setPen(QPen(theme::kRowBorder, 1));
        painter.drawLine(QPointF(kPad, y + 0.5), QPointF(width() - kPad, y + 0.5));
        painter.setFont(theme::labelPx(12));
        painter.setPen(theme::kTextMuted);
        painter.drawText(QRectF(kPad, y, inner, kFooterHeight - 4), Qt::AlignCenter, view_.footer);
    }
}

AssemblyFlow::AssemblyFlow(QWidget* host) : QObject(host), host_(host), progress_(new AssemblyProgress(host)) {
    host->installEventFilter(this);
    refresh();
}

void AssemblyFlow::setSender(Sender sender) { sender_ = std::move(sender); }

void AssemblyFlow::setNeighbor(QWidget* neighbor) {
    neighbor_ = neighbor;
    place();
}

void AssemblyFlow::update(const AssemblyState::Inputs& inputs, MonotonicMs now) {
    link_up_ = inputs.link_up;
    now_ = now;
    state_.update(inputs, now);
    refresh();
}

void AssemblyFlow::boxOpened() {
    state_.boxOpened();
    refresh();
}

void AssemblyFlow::choose(std::uint32_t level) {
    state_.choose(level);
    refresh();
}

bool AssemblyFlow::start() {
    const std::optional<AssemblySend> command = state_.start();
    const bool sent = command && send(*command);
    refresh();
    return sent;
}

bool AssemblyFlow::confirm() {
    const AssemblyState::Key key = state_.confirm();
    if (key.send) send(*key.send);
    refresh();
    return key.consumed;
}

bool AssemblyFlow::exit() {
    const AssemblyState::Key key = state_.exit();
    if (key.send) send(*key.send);
    refresh();
    return key.consumed;
}

void AssemblyFlow::busy() {
    state_.busy();
    refresh();
}

void AssemblyFlow::raiseOpen() {
    if (progress_->isVisible()) raise_to_top(progress_);
}

bool AssemblyFlow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == host_ && event->type() == QEvent::Resize) place();
    return false;
}

bool AssemblyFlow::send(const AssemblySend& command) {
    // 断线和限频时直接丢掉,不排队补发:补发的时候战场早就不是按键那一刻的样子了。
    bool ok = link_up_ && sender_ && gate_.allow(now_);
    if (ok) ok = sender_("AssemblyCommand", encode_assembly(command.operation, command.difficulty));
    if (ok) state_.sent(command);
    Q_EMIT commandSent(command.operation, command.difficulty, ok);
    return ok;
}

void AssemblyFlow::refresh() {
    progress_->setView(state_.progress());
    place();
    if (canExit() != can_exit_) {
        can_exit_ = canExit();
        Q_EMIT canExitChanged(can_exit_);
    }
    if (const AssemblyRow row = state_.row(); row != row_) {
        row_ = row;
        Q_EMIT rowChanged();
    }
}

void AssemblyFlow::place() {
    const bool show = progress_->view().visible;
    if (show) {
        const QRect area = host_->rect();
        const QSize size = progress_->sizeHint();
        int bottom = area.height() - kBottomMargin;
        if (neighbor_ && neighbor_->isVisible()) bottom = std::min(bottom, neighbor_->geometry().top() - 8);
        progress_->setGeometry((area.width() - size.width()) / 2, std::max(0, bottom - size.height()), size.width(),
                               size.height());
    }
    if (show == progress_->isHidden()) progress_->setVisible(show);
    if (show) raise_to_top(progress_);
}

}  // namespace rm_terminal
