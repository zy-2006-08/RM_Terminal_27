#include "assembly_pane.h"

#include "theme.h"

#include <QHBoxLayout>
#include <QPainter>
#include <QPaintEvent>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

namespace rm_terminal {

namespace {

// 重装那一格在 1920x1080 下约 420x300,卡片标题吃掉约 35,按钮两行吃掉 76,
// 留给自绘内容的只有约 169 像素。行高和格高按这个预算定:放大任一项都会把
// 机械臂那行挤出面板,而那是操作手判断机械臂是否还在回传的唯一依据。
constexpr int kRowHeight = 16;
constexpr int kTileHeight = 38;
constexpr int kChipHeight = 22;
constexpr int kGap = 6;
constexpr std::uint32_t kAssemblyStart = 0;
constexpr std::uint32_t kAssemblyConfirm = 1;
constexpr std::uint32_t kAssemblyCancel = 2;

const QString kDash = QStringLiteral("--");

template <class T>
std::optional<T> present(const Field<T>& field) {
    if (field.quality != Quality::Valid || !field.value) return std::nullopt;
    return field.value;
}

QString basic_state_text(std::optional<std::uint32_t> state) {
    if (!state) return kDash;
    switch (*state) {
        case 1: return QStringLiteral("初始位置");
        case 2: return QStringLiteral("运动中");
        case 3: return QStringLiteral("已到达");
        default: return QStringLiteral("未知（%1）").arg(*state);
    }
}

QString enemy_core_text(std::optional<std::uint32_t> status) {
    if (!status) return kDash;
    switch (*status) {
        case 0: return QStringLiteral("无");
        case 1: return QStringLiteral("非四级");
        case 2: return QStringLiteral("四级");
        default: return QStringLiteral("未知（%1）").arg(*status);
    }
}

QPushButton* make_button(QWidget* parent, const QString& text, const char* name) {
    auto* button = new QPushButton(text, parent);
    button->setObjectName(QString::fromLatin1(name));
    button->setStyleSheet(command_button_style());
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::NoFocus);
    return button;
}

}  // namespace

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
            if (const auto raw = present(robot->second.modules.modular_42mm))
                view.modular_42mm = *raw != 0;
        }
    }

    // Event 15 是装配结果,文字在 match_event.cpp 已翻成中文。只取最新一条:
    // 面板显示「最近一次结果」,把历史留给战场事件面板。
    for (const EventRecord& record : snapshot.events.recent(snapshot.events.capacity())) {
        if (record.event_id != 15) continue;
        view.last_result = QString::fromStdString(record.text);
        view.last_result_at = record.received_at;
        break;
    }
    return view;
}

AssemblyPane::AssemblyPane(QWidget* parent) : QWidget(parent) {
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(kGap);
    column->addStretch(1);

    auto* top = new QHBoxLayout;
    top->setSpacing(4);
    minus_ = make_button(this, QStringLiteral("−"), "assemblyDifficultyDown");
    plus_ = make_button(this, QStringLiteral("＋"), "assemblyDifficultyUp");
    minus_->setFixedWidth(34);
    plus_->setFixedWidth(34);
    start_ = make_button(this, QStringLiteral("开始兑换"), "assemblyStart");
    top->addWidget(minus_);
    top->addWidget(plus_);
    top->addWidget(start_, 1);
    column->addLayout(top);

    auto* bottom = new QHBoxLayout;
    bottom->setSpacing(4);
    confirm_ = make_button(this, QStringLiteral("确认装配"), "assemblyConfirm");
    cancel_ = make_button(this, QStringLiteral("取消装配 按住"), "assemblyCancel");
    bottom->addWidget(confirm_, 1);
    bottom->addWidget(cancel_, 1);
    column->addLayout(bottom);

    QObject::connect(minus_, &QPushButton::clicked, this, [this] {
        difficulty_ = std::max<std::uint32_t>(difficulty_ - 1, 1);
        refreshButtons();
        update();
    });
    QObject::connect(plus_, &QPushButton::clicked, this, [this] {
        const std::uint32_t top_level = view_.maximum_difficulty.value_or(1);
        difficulty_ = std::min(difficulty_ + 1, std::max<std::uint32_t>(top_level, 1));
        refreshButtons();
        update();
    });
    QObject::connect(start_, &QPushButton::clicked, this,
                     [this] { send(kAssemblyStart, difficulty_); });
    QObject::connect(confirm_, &QPushButton::clicked, this,
                     [this] { send(kAssemblyConfirm, difficulty_); });
    // 取消装配撤不回(role-layouts D4),按住一秒才发。pressed/released 而不是
    // clicked:clicked 在松手时才来,拿不到按下的时刻。
    QObject::connect(cancel_, &QPushButton::pressed, this, [this] {
        cancel_hold_.press(now_);
        refreshButtons();
    });
    QObject::connect(cancel_, &QPushButton::released, this, [this] {
        const bool fired = cancel_hold_.release(now_);
        refreshButtons();
        if (fired) send(kAssemblyCancel, difficulty_);
    });

    setMinimumHeight(190);
    refreshButtons();
}

void AssemblyPane::setView(const AssemblyView& view) {
    view_ = view;
    const std::uint32_t top_level = std::max<std::uint32_t>(view_.maximum_difficulty.value_or(1), 1);
    difficulty_ = std::clamp<std::uint32_t>(difficulty_, 1, top_level);
    refreshButtons();
    update();
}

void AssemblyPane::setLinkUp(bool up) {
    if (link_up_ == up) return;
    link_up_ = up;
    // 断线时丢掉按住中的状态:链路恢复后那次按下已经没有意义,继续计时会在
    // 操作手没按的情况下把「取消装配」发出去。
    if (!up) cancel_hold_.cancel();
    refreshButtons();
    update();
}

void AssemblyPane::setSender(
    std::function<bool(const char* topic, const std::string& payload)> sender) {
    sender_ = std::move(sender);
}

void AssemblyPane::setArm(std::optional<double> fps, std::optional<std::array<float, 6>> joints) {
    arm_fps_ = fps;
    arm_joints_ = joints;
    update();
}

void AssemblyPane::tick(MonotonicMs now) {
    now_ = now;
    echo_result_ = echo_.update(now, echoFingerprint());
    refreshButtons();
    update();
}

std::uint64_t AssemblyPane::echoFingerprint() const {
    // 装配没有显式回执:TechCoreMotionStateSync 的步骤位、位姿状态,加上最近一条
    // Event 15 的时刻,任一变化都算服务器响应了。
    std::uint64_t key = 0;
    const auto fold = [&key](std::uint64_t part) { key = key * 131u + part + 1u; };
    fold(view_.basic_state.value_or(0));
    fold(view_.putin.value_or(false) ? 1 : 0);
    fold(view_.move.value_or(false) ? 1 : 0);
    fold(view_.rotate.value_or(false) ? 1 : 0);
    fold(static_cast<std::uint64_t>(view_.last_result_at.value_or(-1)));
    return key;
}

void AssemblyPane::send(std::uint32_t operation, std::uint32_t difficulty) {
    // 断线期间的点击直接丢弃,不排队补发:补发会在链路恢复的瞬间把一串旧指令
    // 倒给服务器,那时的战场已经不是按下按钮时的战场了。
    if (!link_up_ || !sender_) return;
    if (!gate_.allow(now_)) return;
    if (!sender_("AssemblyCommand", encode_assembly(operation, difficulty))) return;
    echo_.sent(now_, operation, echoFingerprint());
    echo_result_ = echo_.result();
    refreshButtons();
    update();
}

void AssemblyPane::refreshButtons() {
    const std::uint32_t top_level = view_.maximum_difficulty.value_or(0);
    const bool has_sender = static_cast<bool>(sender_);
    const bool live = link_up_ && has_sender;
    start_->setEnabled(live && top_level >= 1);
    confirm_->setEnabled(live);
    cancel_->setEnabled(live);
    minus_->setEnabled(live && difficulty_ > 1);
    plus_->setEnabled(live && difficulty_ < std::max<std::uint32_t>(top_level, 1));

    const double held = cancel_hold_.progress(now_);
    cancel_->setStyleSheet(held > 0.0 ? hold_button_style(held) : command_button_style());
    cancel_->setText(cancel_hold_.ready(now_) ? QStringLiteral("取消装配 松手发送")
                                              : QStringLiteral("取消装配 按住"));
    start_->setText(QStringLiteral("开始兑换 难度 %1")
                        .arg(top_level >= 1 ? QString::number(difficulty_) : kDash));
}

bool AssemblyPane::buttonsEnabled() const {
    return confirm_->isEnabled() && cancel_->isEnabled();
}

QString AssemblyPane::statusText() const {
    if (!link_up_) return QStringLiteral("断线，指令不会发送");
    if (!sender_) return QStringLiteral("未接入发送链路");
    const QString echo = echo_status_text(echo_result_);
    if (!echo.isEmpty()) return echo;
    return QStringLiteral("就绪");
}

QSize AssemblyPane::sizeHint() const { return QSize(420, 270); }

void AssemblyPane::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const int w = width();
    // 画布只到按钮上沿:按钮由布局摆放,写死高度会在卡片变矮时让文字压到按钮上。
    const int canvas_h = std::max(minus_->geometry().top() - kGap, 0);
    if (canvas_h <= 0 || w <= 0) return;

    const QRect status(0, canvas_h - kRowHeight, w, kRowHeight);
    painter.setFont(theme::labelFont(10, !link_up_));
    painter.setPen(link_up_ ? (echo_result_.status == EchoStatus::Unconfirmed ||
                                       echo_result_.status == EchoStatus::Failed
                                   ? theme::kWarn
                                   : theme::kTextSecondary)
                            : theme::kDanger);
    painter.drawText(status, Qt::AlignLeft | Qt::AlignVCenter, statusText());

    int y = 0;
    const int limit = status.top() - 2;
    const auto fits = [&](int h) { return y + h <= limit; };

    if (fits(kTileHeight)) {
        const int tile_w = (w - kGap * 2) / 3;
        role_paint::tile(painter, QRect(0, y, tile_w, kTileHeight), QStringLiteral("最高难度"),
                         view_.maximum_difficulty ? QString::number(*view_.maximum_difficulty) : kDash,
                         view_.maximum_difficulty ? theme::kTextPrimary : theme::kTextMuted);
        role_paint::tile(painter, QRect(tile_w + kGap, y, tile_w, kTileHeight),
                         QStringLiteral("科技等级"),
                         view_.tech_level ? QString::number(*view_.tech_level) : kDash,
                         view_.tech_level ? theme::kTextPrimary : theme::kTextMuted);
        const QString mod = view_.modular_42mm
                                ? (*view_.modular_42mm ? QStringLiteral("已授权")
                                                       : QStringLiteral("未授权"))
                                : kDash;
        role_paint::tile(painter, QRect((tile_w + kGap) * 2, y, w - (tile_w + kGap) * 2, kTileHeight),
                         QStringLiteral("42mm 权限"), mod,
                         !view_.modular_42mm ? theme::kTextMuted
                                             : (*view_.modular_42mm ? theme::kOk : theme::kTextSecondary));
        y += kTileHeight + kGap;
    }

    if (fits(kChipHeight)) {
        const int arrow_w = 16;
        const int chip_w = (w - arrow_w * 2) / 3;
        const std::array<std::pair<QString, std::optional<bool>>, 3> steps{
            {{QStringLiteral("放入"), view_.putin},
             {QStringLiteral("平移"), view_.move},
             {QStringLiteral("旋转"), view_.rotate}}};
        int x = 0;
        for (std::size_t i = 0; i < steps.size(); ++i) {
            const bool done = steps[i].second.value_or(false);
            const QString text =
                steps[i].second ? steps[i].first : steps[i].first + QStringLiteral(" --");
            role_paint::chip(painter, QRect(x, y, chip_w, kChipHeight), text, done, theme::kCyanDim);
            x += chip_w;
            if (i + 1 < steps.size()) {
                painter.setFont(theme::labelFont(9));
                painter.setPen(theme::kTextMuted);
                painter.drawText(QRect(x, y, arrow_w, kChipHeight), Qt::AlignCenter,
                                 QStringLiteral("›"));
                x += arrow_w;
            }
        }
        y += kChipHeight + 4;
    }

    const auto line = [&](const QString& label, const QString& value, const QColor& ink) {
        if (!fits(kRowHeight)) return;
        role_paint::row(painter, QRect(0, y, w, kRowHeight), label, value, ink);
        y += kRowHeight;
    };

    // 两项都缺时只画一个「--」:「-- · --」会被当成某种编码而不是「没数据」。
    const bool pose_known = view_.basic_state || view_.enemy_core_status;
    line(QStringLiteral("位姿 / 对方装配"),
         pose_known ? basic_state_text(view_.basic_state) + QStringLiteral(" · ") +
                          enemy_core_text(view_.enemy_core_status)
                    : kDash,
         view_.enemy_core_status.value_or(0) == 2
             ? theme::kDanger
             : (view_.basic_state ? theme::kTextPrimary : theme::kTextMuted));
    // 剩余时间只在四级装配中有效(协议 2.2.22 字段 7/8),没有时写明条件而不是画 0。
    const bool has_time = view_.remain_time_step || view_.remain_time_all;
    line(QStringLiteral("剩余"),
         has_time ? QStringLiteral("本步 %1s / 共 %2s")
                        .arg(view_.remain_time_step ? QString::number(*view_.remain_time_step) : kDash,
                             view_.remain_time_all ? QString::number(*view_.remain_time_all) : kDash)
                  : QStringLiteral("-- 仅四级装配中有效"),
         has_time ? theme::kTextPrimary : theme::kTextMuted);
    line(QStringLiteral("最近结果"), view_.last_result.isEmpty() ? kDash : view_.last_result,
         view_.last_result.isEmpty() ? theme::kTextMuted : theme::kTextPrimary);

    // 机械臂一行里同时给帧率和六个关节角:角度单独一行在 300 高的格子里放不下,
    // 而只给帧率看不出机械臂是卡在某个姿态还是真的在动。
    QString arm = arm_fps_ ? QStringLiteral("%1 帧/s").arg(*arm_fps_, 0, 'f', 1)
                           : QStringLiteral("-- 等待机械臂回传");
    if (arm_joints_) {
        QStringList angles;
        for (const float angle : *arm_joints_) angles << QString::number(angle, 'f', 0);
        arm += QStringLiteral("  ") + angles.join(QStringLiteral(" "));
    }
    line(QStringLiteral("机械臂"), arm, arm_fps_ ? theme::kOk : theme::kTextMuted);

    // 缺数据时在最后一行点名还在等哪几条消息。消息名很长,420 宽放不下三条,
    // 所以只报第一条并带上还差几条 —— 省略成「...Sync / Global...」谁都看不懂。
    QStringList waiting;
    if (!view_.maximum_difficulty || !view_.basic_state)
        waiting << QStringLiteral("TechCoreMotionStateSync");
    if (!view_.tech_level) waiting << QStringLiteral("GlobalLogisticsStatus");
    if (!view_.modular_42mm) waiting << QStringLiteral("RobotModuleStatus");
    if (!waiting.isEmpty() && fits(kRowHeight)) {
        painter.setFont(theme::labelFont(9));
        painter.setPen(theme::kTextMuted);
        QString text = QStringLiteral("等待 ") + waiting.front();
        if (waiting.size() > 1)
            text += QStringLiteral(" 等 %1 条").arg(waiting.size());
        const QRect box(0, y, w, kRowHeight);
        painter.drawText(box, Qt::AlignLeft | Qt::AlignVCenter,
                         painter.fontMetrics().elidedText(text, Qt::ElideRight, w));
    }
}

}  // namespace rm_terminal
