#include "sentry_pane.h"

#include "roster_pane.h"
#include "theme.h"

#include <QGridLayout>
#include <QPainter>
#include <QPaintEvent>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

namespace rm_terminal {

namespace {

// 指挥手右列那一格约 380x360。卡片标题和四行按钮吃掉大部分,实测自绘区只有
// 约 137 像素,要装下三格 + 血条 + 发弹量 + 上一条指令 + 花费说明 + 状态行。
// 下面这组数值是按这个预算配平的:任一项再加 1 像素就会把「上一条指令」挤掉。
// 真的要加行时,先量 postures_[0]->geometry().top(),别凭感觉改。
constexpr int kRowHeight = 15;
constexpr int kTileHeight = 36;
constexpr int kBarHeight = 7;
constexpr int kGap = 4;
// 规则手册 5.7.2。
constexpr MonotonicMs kPostureCooldownMs = 5000;
constexpr std::uint32_t kCmdRemoteAmmo = 3;
constexpr std::uint32_t kCmdRemoteHeal = 4;
constexpr std::uint32_t kCmdRevive = 5;
constexpr std::uint32_t kCmdGoldRevive = 6;
constexpr std::uint32_t kPostureFirst = 7;
constexpr std::uint32_t kPostureLast = 12;
// 哨兵在 robot_health 里的席位号:红 7,蓝 107。
constexpr std::uint32_t kSentryRed = 7;
constexpr std::uint32_t kSentryBlue = 107;

const QString kDash = QStringLiteral("--");

template <class T>
std::optional<T> present(const Field<T>& field) {
    if (field.quality != Quality::Valid || !field.value) return std::nullopt;
    return field.value;
}

QPushButton* make_button(QWidget* parent, const QString& text, const QString& name) {
    auto* button = new QPushButton(text, parent);
    button->setObjectName(name);
    button->setStyleSheet(command_button_style());
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::NoFocus);
    return button;
}

}  // namespace

QString sentry_posture_text(std::uint32_t posture) {
    switch (posture) {
        case 1: return QStringLiteral("进攻");
        case 2: return QStringLiteral("防御");
        case 3: return QStringLiteral("移动");
        default: return kDash;
    }
}

QString sentry_command_text(std::uint32_t command_id) {
    switch (command_id) {
        case 1: return QStringLiteral("补血点补弹");
        case 3: return QStringLiteral("远程补弹");
        case 4: return QStringLiteral("远程回血");
        case 5: return QStringLiteral("确认复活");
        case 6: return QStringLiteral("花金币复活");
        case 7: return QStringLiteral("进攻");
        case 8: return QStringLiteral("防御");
        case 9: return QStringLiteral("移动");
        case 10: return QStringLiteral("强化进攻");
        case 11: return QStringLiteral("强化防御");
        case 12: return QStringLiteral("强化移动");
        default: return kDash;
    }
}

SentryView sentry_view(const Snapshot& snapshot) {
    SentryView view;
    view.posture = present(snapshot.sentry.posture);
    view.weakened = present(snapshot.sentry.weakened);
    view.powered = present(snapshot.sentry.powered);
    view.economy = present(snapshot.game.economy);
    view.result = snapshot.sentry_result;

    if (const std::optional<std::uint32_t> faction = self_faction(snapshot)) {
        const std::uint32_t seat = *faction == kRed ? kSentryRed : kSentryBlue;
        for (const RobotHealth& health : snapshot.robot_health) {
            if (health.id.value != seat) continue;
            view.hp = present(health.current_hp);
            view.max_hp = present(health.max_hp);
            view.bullets = present(health.bullets);
            break;
        }
    }
    return view;
}

SentryPane::SentryPane(QWidget* parent) : QWidget(parent) {
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(kGap);
    column->addStretch(1);

    auto* grid = new QGridLayout;
    grid->setSpacing(4);
    for (std::size_t i = 0; i < postures_.size(); ++i) {
        const std::uint32_t id = kPostureFirst + static_cast<std::uint32_t>(i);
        postures_[i] = make_button(this, sentry_command_text(id),
                                   QStringLiteral("sentryPosture%1").arg(id));
        postures_[i]->setCheckable(true);
        grid->addWidget(postures_[i], static_cast<int>(i / 3), static_cast<int>(i % 3));
        QObject::connect(postures_[i], &QPushButton::clicked, this, [this, id] { send(id); });
    }
    column->addLayout(grid);

    auto* actions = new QGridLayout;
    actions->setSpacing(4);
    ammo_ = make_button(this, QStringLiteral("补弹 50金"), QStringLiteral("sentryAmmo"));
    heal_ = make_button(this, QStringLiteral("回血 50金"), QStringLiteral("sentryHeal"));
    revive_ = make_button(this, QStringLiteral("确认复活"), QStringLiteral("sentryRevive"));
    gold_revive_ =
        make_button(this, QStringLiteral("花金币复活 按住"), QStringLiteral("sentryGoldRevive"));
    actions->addWidget(ammo_, 0, 0);
    actions->addWidget(heal_, 0, 1);
    actions->addWidget(revive_, 1, 0);
    actions->addWidget(gold_revive_, 1, 1);
    column->addLayout(actions);

    QObject::connect(ammo_, &QPushButton::clicked, this, [this] { send(kCmdRemoteAmmo); });
    QObject::connect(heal_, &QPushButton::clicked, this, [this] { send(kCmdRemoteHeal); });
    QObject::connect(revive_, &QPushButton::clicked, this, [this] { send(kCmdRevive); });
    QObject::connect(gold_revive_, &QPushButton::pressed, this, [this] {
        gold_hold_.press(now_);
        refreshButtons();
    });
    QObject::connect(gold_revive_, &QPushButton::released, this, [this] {
        const bool fired = gold_hold_.release(now_);
        refreshButtons();
        if (fired) send(kCmdGoldRevive);
    });

    setMinimumHeight(230);
    refreshButtons();
}

void SentryPane::setView(const SentryView& view) {
    view_ = view;
    refreshButtons();
    update();
}

void SentryPane::setLinkUp(bool up) {
    if (link_up_ == up) return;
    link_up_ = up;
    if (!up) gold_hold_.cancel();
    refreshButtons();
    update();
}

void SentryPane::setSender(
    std::function<bool(const char* topic, const std::string& payload)> sender) {
    sender_ = std::move(sender);
}

std::uint32_t SentryPane::lastPostureCommand() const {
    if (!view_.result) return 0;
    const std::uint32_t id = view_.result->command_id;
    return id >= kPostureFirst && id <= kPostureLast ? id : 0;
}

void SentryPane::tick(MonotonicMs now) {
    now_ = now;
    // SentryCtrlResult 是显式回执,按 command_id 核对。同一条结果只处理一次:
    // received_at 不变就是同一条,重复处理会把 5 秒冷却一直往后推。
    if (view_.result &&
        (view_.result->command_id != last_seen_result_id_ ||
         last_seen_result_at_ != std::optional<MonotonicMs>(view_.result->received_at))) {
        last_seen_result_id_ = view_.result->command_id;
        last_seen_result_at_ = view_.result->received_at;
        echo_result_ = echo_.applyResult(now, view_.result->command_id, view_.result->result_code);
        if (view_.result->result_code == 0 && lastPostureCommand() != 0)
            posture_cooldown_from_ = view_.result->received_at;
    }
    echo_result_ = echo_.update(now, 0);
    refreshButtons();
    update();
}

void SentryPane::send(std::uint32_t command_id) {
    if (!link_up_ || !sender_) return;
    if (command_id >= kPostureFirst && command_id <= kPostureLast && postureCoolingDown()) return;
    if (!gate_.allow(now_)) return;
    if (!sender_("SentryCtrlCommand", encode_sentry(command_id))) return;
    // baseline 固定为 0,与 update() 里传的 0 一致:哨兵靠 SentryCtrlResult 显式
    // 回执,没有可比的状态指纹,超时就是「未确认」。
    echo_.sent(now_, command_id, 0);
    echo_result_ = echo_.result();
    refreshButtons();
    update();
}

bool SentryPane::postureCoolingDown() const {
    if (!posture_cooldown_from_) return false;
    return now_ - *posture_cooldown_from_ < kPostureCooldownMs;
}

void SentryPane::refreshButtons() {
    const bool live = link_up_ && static_cast<bool>(sender_);
    const bool cooling = postureCoolingDown();
    for (std::size_t i = 0; i < postures_.size(); ++i) {
        const std::uint32_t id = kPostureFirst + static_cast<std::uint32_t>(i);
        postures_[i]->setEnabled(live && !cooling);
        // 协议只发 posture 1/2/3 和 powered,不发「按的是哪个按钮」。
        // 强化三档对应 10/11/12,普通三档对应 7/8/9。
        const std::uint32_t shown =
            view_.posture ? *view_.posture + (view_.powered.value_or(false) ? 9u : 6u) : 0u;
        const bool chosen = shown == id;
        if (postures_[i]->isChecked() != chosen) postures_[i]->setChecked(chosen);
    }
    ammo_->setEnabled(live);
    heal_->setEnabled(live);
    revive_->setEnabled(live);
    gold_revive_->setEnabled(live);

    const double held = gold_hold_.progress(now_);
    gold_revive_->setStyleSheet(held > 0.0 ? hold_button_style(held) : command_button_style());
    gold_revive_->setText(gold_hold_.ready(now_) ? QStringLiteral("花金币复活 松手")
                                                 : QStringLiteral("花金币复活 按住"));
}

bool SentryPane::buttonsEnabled() const { return ammo_->isEnabled() && revive_->isEnabled(); }

QString SentryPane::statusText() const {
    if (!link_up_) return QStringLiteral("断线，指令不会发送");
    if (!sender_) return QStringLiteral("未接入发送链路");
    if (echo_result_.status != EchoStatus::Idle && echo_.tag() != 0)
        return sentry_command_text(echo_.tag()) + QStringLiteral(" · ") +
               echo_status_text(echo_result_);
    if (!view_.posture) return QStringLiteral("等待 SentryStatusSync");
    return QStringLiteral("就绪");
}

QString SentryPane::lastResultText() const {
    if (!view_.result) return QStringLiteral("-- 等待 SentryCtrlResult");
    return sentry_command_text(view_.result->command_id) +
           (view_.result->result_code == 0
                ? QStringLiteral(" 成功")
                : QStringLiteral(" 失败（码 %1）").arg(view_.result->result_code));
}

QSize SentryPane::sizeHint() const { return QSize(380, 310); }

void SentryPane::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const int w = width();
    const int canvas_h = std::max(postures_[0]->geometry().top() - kGap, 0);
    if (canvas_h <= 0 || w <= 0) return;

    const bool bad_echo = echo_result_.status == EchoStatus::Unconfirmed ||
                          echo_result_.status == EchoStatus::Failed;
    const QRect status(0, canvas_h - kRowHeight, w, kRowHeight);
    painter.setFont(theme::labelFont(10, !link_up_));
    painter.setPen(!link_up_ ? theme::kDanger
                             : (bad_echo ? theme::kWarn : theme::kTextSecondary));
    painter.drawText(status, Qt::AlignLeft | Qt::AlignVCenter,
                     painter.fontMetrics().elidedText(statusText(), Qt::ElideRight, w));

    // 规则手册 5.7.2 的两条约束常被忘掉,固定写在面板上而不是靠按钮文字暗示。
    // 但它排在数据之后:格子被压到 320x240 时,先保证血量和发弹量画得出来,
    // 说明文字可以丢 —— 丢掉说明只是少一句提醒,丢掉血量是看不见哨兵快死了。
    const int body_needed = kTileHeight + kGap + kRowHeight + kBarHeight + kGap + kRowHeight * 2;
    const bool room_for_note = status.top() - kGap - body_needed >= kRowHeight;
    const QRect note(0, status.top() - kRowHeight, w, kRowHeight);
    const int limit = (room_for_note ? note.top() : status.top()) - kGap;
    if (room_for_note) {
        painter.setFont(theme::labelFont(9));
        painter.setPen(theme::kTextMuted);
        painter.drawText(note, Qt::AlignLeft | Qt::AlignVCenter,
                         painter.fontMetrics().elidedText(
                             QStringLiteral("自动哨兵每次干预消耗 50 金币；姿态切换冷却 5 秒"),
                             Qt::ElideRight, w));
    }

    int y = 0;
    const auto fits = [&](int h) { return y + h <= limit; };
    // 三格(姿态/增益/经济)占的高度最大,空间不够时第一个退,把位置让给血量
    // 和发弹量 —— 姿态在按钮上还看得出来,血量没有别的地方可看。
    if (fits(kTileHeight + kGap + kRowHeight)) {
        const int tile_w = (w - kGap * 2) / 3;
        const QString posture =
            view_.posture ? sentry_posture_text(*view_.posture) : kDash;
        role_paint::tile(painter, QRect(0, y, tile_w, kTileHeight), QStringLiteral("姿态"), posture,
                         view_.posture ? theme::kTextPrimary : theme::kTextMuted);

        // 弱化和强化是两个独立的布尔,同时为真时都要说出来,不能二选一显示。
        QStringList marks;
        if (view_.weakened.value_or(false)) marks << QStringLiteral("弱化");
        if (view_.powered.value_or(false)) marks << QStringLiteral("强化");
        const bool known = view_.weakened || view_.powered;
        const QString buff = marks.isEmpty() ? (known ? QStringLiteral("常态") : kDash)
                                             : marks.join(QStringLiteral("+"));
        role_paint::tile(painter, QRect(tile_w + kGap, y, tile_w, kTileHeight),
                         QStringLiteral("增益"), buff,
                         !known ? theme::kTextMuted
                                : (view_.weakened.value_or(false) ? theme::kDanger
                                                                  : (view_.powered.value_or(false)
                                                                         ? theme::kOk
                                                                         : theme::kTextSecondary)));
        role_paint::tile(painter, QRect((tile_w + kGap) * 2, y, w - (tile_w + kGap) * 2, kTileHeight),
                         QStringLiteral("经济"),
                         view_.economy ? QString::number(*view_.economy) : kDash,
                         view_.economy ? theme::kTextPrimary : theme::kTextMuted);
        y += kTileHeight + kGap;
    }

    // 血量永远画:空间不够时退成只有数字的一行(丢血条,不丢数字)。
    if (fits(kRowHeight)) {
        const double ratio = view_.hp && view_.max_hp && *view_.max_hp > 0
                                 ? static_cast<double>(*view_.hp) / *view_.max_hp
                                 : 0.0;
        role_paint::row(painter, QRect(0, y, w, kRowHeight), QStringLiteral("哨兵血量"),
                        view_.hp ? QStringLiteral("%1 / %2").arg(
                                       QString::number(*view_.hp),
                                       view_.max_hp ? QString::number(*view_.max_hp) : kDash)
                                 : QStringLiteral("-- 等待 GlobalUnitStatus"),
                        view_.hp ? theme::healthColor(ratio) : theme::kTextMuted);
        y += kRowHeight;
        if (fits(kBarHeight)) {
            const QRect track(0, y, w, kBarHeight);
            painter.setPen(Qt::NoPen);
            painter.setBrush(theme::kSurfaceRaised);
            painter.drawRect(track);
            if (view_.hp && view_.max_hp && *view_.max_hp > 0) {
                painter.setBrush(theme::healthColor(ratio));
                painter.drawRect(QRect(track.left(), track.top(),
                                       static_cast<int>(track.width() * std::clamp(ratio, 0.0, 1.0)),
                                       track.height()));
            }
            y += kBarHeight + kGap;
        }
    }

    const int line_h = kRowHeight + 3;
    const auto line = [&](const QString& label, const QString& value, const QColor& ink) {
        if (!fits(line_h)) return;
        role_paint::row(painter, QRect(0, y, w, kRowHeight), label, value, ink);
        y += line_h;
    };

    // 冷却中时把剩余秒数并到发弹量这一行的右侧:姿态按钮已经是灰的,操作手要知道
    // 还要等多久才能再切。不冷却时这一行只有发弹量。
    const QString ammo_label = postureCoolingDown()
                                   ? QStringLiteral("剩余发弹量 · 姿态冷却 %1s")
                                         .arg((kPostureCooldownMs - (now_ - *posture_cooldown_from_) +
                                               999) /
                                              1000)
                                   : QStringLiteral("剩余发弹量");
    line(ammo_label,
         view_.bullets ? QString::number(*view_.bullets) : QStringLiteral("-- 等待 GlobalUnitStatus"),
         view_.bullets ? theme::kTextPrimary : theme::kTextMuted);
    line(QStringLiteral("上一条指令"), lastResultText(),
         !view_.result ? theme::kTextMuted
                       : (view_.result->result_code == 0 ? theme::kOk : theme::kDanger));
}

}  // namespace rm_terminal
