#include "self_command_pane.h"

#include "glass.h"
#include "theme.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QPainter>
#include <QPaintEvent>
#include <QPushButton>
#include <QFontMetricsF>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace rm_terminal {

namespace {

// 顶上两块信息小牌子(能量机关 / 回血价)高 28,下面按钮之间、按钮和小牌子之间都隔 6(效果图)。
constexpr int kChipHeight = 28;
constexpr int kGap = 6;
constexpr std::uint32_t kStageMatch = 4;

// CommonCommand.cmd_type(2.2.25)。
constexpr std::uint32_t kBuy17 = 1;
constexpr std::uint32_t kBuy42 = 2;
constexpr std::uint32_t kConfirmRevive = 3;
constexpr std::uint32_t kPayRevive = 4;
constexpr std::uint32_t kRemoteAmmo = 5;
constexpr std::uint32_t kRemoteHeal = 6;

// 规则手册表 5-7、5-8、5-9:非远程兑换 17mm 10 金/10 发,42mm 10 金/1 发;
// 远程兑换 150 金一份,17mm 一份 100 发,42mm 一份 10 发。
constexpr std::uint32_t kRemoteUnit17 = 100;
constexpr std::uint32_t kRemoteUnit42 = 10;
constexpr std::uint32_t kRemotePrice = 150;
constexpr std::array<std::uint32_t, 6> kLadder17{10, 20, 50, 100, 200, 500};
constexpr std::array<std::uint32_t, 6> kLadder42{1, 2, 5, 10, 20, 50};

const QString kDash = QStringLiteral("--");

template <class T>
std::optional<T> present(const Field<T>& field) {
    if (field.quality != Quality::Valid || !field.value) return std::nullopt;
    return field.value;
}

GlassButton* make_button(QWidget* parent, const QString& text, const char* name,
                         GlassButton::Kind kind = GlassButton::Kind::Command) {
    auto* button = new GlassButton(text, parent, kind);
    button->setObjectName(QString::fromLatin1(name));
    return button;
}

// 一块信息小牌子:左边说明,右边数值。数值里的数字用数字字体。
void paint_chip(QPainter& painter, const QRectF& box, const QString& label, const QString& value,
                const QColor& ink) {
    theme::paintGlass(painter, box, 4, false, theme::kChip, theme::kRowBorder);
    const QRectF inner = box.adjusted(10, 0, -10, 0);
    const QFont label_font = theme::labelPx(13);
    painter.setFont(label_font);
    painter.setPen(theme::kTextSecondary);
    const qreal label_w = QFontMetricsF(label_font).horizontalAdvance(label);
    painter.drawText(inner, Qt::AlignLeft | Qt::AlignVCenter, label);
    if (value.isEmpty()) return;
    const QFont value_font = theme::labelPx(13, QFont::Bold);
    const QFont number_font = theme::numericPx(15, QFont::Bold);
    const QRectF value_box(inner.left() + label_w + 8, inner.top(), inner.width() - label_w - 8, inner.height());
    theme::paintMixedText(painter, value_box, Qt::AlignRight | Qt::AlignVCenter, value, value_font, number_font, ink);
}

QString rune_text(std::optional<std::uint32_t> status, std::optional<std::uint32_t> arms) {
    if (!status) return kDash;
    switch (*status) {
        case 1: return QStringLiteral("未激活");
        case 2: return QStringLiteral("激活中 %1 臂").arg(arms.value_or(0));
        case 3: return QStringLiteral("已激活");
        default: return QStringLiteral("未知（%1）").arg(*status);
    }
}

}  // namespace

SelfCommandView self_command_view(const Snapshot& snapshot) {
    SelfCommandView view;
    view.stage = present(snapshot.game.current_stage);
    if (view.stage && *view.stage == kStageMatch) view.remaining = present(snapshot.game.stage_countdown_sec);
    view.economy = present(snapshot.game.economy);
    view.rune_status = present(snapshot.game.rune_status);
    view.rune_arms = present(snapshot.game.rune_activated_arms);
    view.shooter = present(snapshot.game.selected_shooter);
    view.chassis = present(snapshot.game.selected_chassis);
    if (!snapshot.self) return view;
    view.infantry = snapshot.self->value % 100 != 1;
    const auto robot = snapshot.robots.find(*snapshot.self);
    if (robot == snapshot.robots.end()) return view;
    const RobotState& self = robot->second;
    view.ammo = present(self.dynamic.remaining_ammo);
    view.hp = present(self.dynamic.current_hp);
    view.out_of_combat = present(self.dynamic.out_of_combat);
    view.can_remote_heal = present(self.dynamic.can_remote_heal);
    view.can_remote_ammo = present(self.dynamic.can_remote_ammo);
    view.max_hp = present(self.statics.max_hp);
    view.level = present(self.statics.level);
    if (!view.shooter) view.shooter = present(self.statics.shooter_system);
    if (!view.chassis) view.chassis = present(self.statics.chassis_system);
    view.dead = present(self.respawn.pending);
    view.revive_total = present(self.respawn.total_progress);
    view.revive_current = present(self.respawn.current_progress);
    view.revive_cost = present(self.respawn.gold_cost);
    view.can_free_revive = present(self.respawn.can_free);
    view.can_pay_revive = present(self.respawn.can_pay);
    return view;
}

SelfCommandPane::SelfCommandPane(QWidget* parent) : QWidget(parent) {
    auto* column = new QVBoxLayout(this);
    // 顶上留出两块信息小牌子的高度,由 paintEvent 画。
    column->setContentsMargins(0, kChipHeight + kGap, 0, 0);
    column->setSpacing(kGap);

    pre_page_ = new QWidget(this);
    auto* pre = new QGridLayout(pre_page_);
    pre->setContentsMargins(0, 0, 0, 0);
    pre->setSpacing(kGap);
    chassis_hp_ = make_button(pre_page_, QStringLiteral("血量优先"), "selfChassisHp");
    chassis_power_ = make_button(pre_page_, QStringLiteral("功率优先"), "selfChassisPower");
    shooter_cooling_ = make_button(pre_page_, QStringLiteral("冷却优先"), "selfShooterCooling");
    shooter_burst_ = make_button(pre_page_, QStringLiteral("爆发优先"), "selfShooterBurst");
    for (QPushButton* button : {chassis_hp_, chassis_power_, shooter_cooling_, shooter_burst_})
        button->setCheckable(true);
    pre->addWidget(chassis_hp_, 0, 0);
    pre->addWidget(chassis_power_, 0, 1);
    pre->addWidget(shooter_cooling_, 1, 0);
    pre->addWidget(shooter_burst_, 1, 1);
    column->addWidget(pre_page_);

    // 比赛中最多三行按钮:兑换一行、远程兑换一行、能量机关一行(只有步兵)。花名册那一格
    // 在 1440x900 下只剩约 200 像素高,复活不占这里,阵亡时另外弹框。
    match_page_ = new QWidget(this);
    auto* match = new QGridLayout(match_page_);
    match->setContentsMargins(0, 0, 0, 0);
    match->setSpacing(kGap);
    auto* amount = new QHBoxLayout;
    amount->setSpacing(kGap);
    minus_ = make_button(match_page_, QStringLiteral("−"), "selfExchangeLess");
    plus_ = make_button(match_page_, QStringLiteral("+"), "selfExchangeMore");
    static_cast<GlassButton*>(minus_)->setSquare(true);
    static_cast<GlassButton*>(plus_)->setSquare(true);
    minus_->setFixedWidth(42);
    plus_->setFixedWidth(42);
    exchange_ = make_button(match_page_, QString(), "selfExchange");
    amount->addWidget(minus_);
    amount->addWidget(exchange_, 1);
    amount->addWidget(plus_);
    match->addLayout(amount, 0, 0, 1, 2);
    remote_ammo_ = make_button(match_page_, QStringLiteral("远程补弹"), "selfRemoteAmmo");
    remote_heal_ = make_button(match_page_, QStringLiteral("远程回血"), "selfRemoteHeal");
    rune_ = make_button(match_page_, QStringLiteral("能量机关 按住"), "selfRune", GlassButton::Kind::Accent);
    match->addWidget(remote_ammo_, 1, 0);
    match->addWidget(remote_heal_, 1, 1);
    match->addWidget(rune_, 2, 0, 1, 2);
    column->addWidget(match_page_);

    QObject::connect(chassis_hp_, &QPushButton::clicked, this,
                     [this] { sendPerformance(shooterChoice(), 1); });
    QObject::connect(chassis_power_, &QPushButton::clicked, this,
                     [this] { sendPerformance(shooterChoice(), 2); });
    QObject::connect(shooter_cooling_, &QPushButton::clicked, this,
                     [this] { sendPerformance(1, chassisChoice()); });
    QObject::connect(shooter_burst_, &QPushButton::clicked, this,
                     [this] { sendPerformance(2, chassisChoice()); });
    QObject::connect(minus_, &QPushButton::clicked, this, [this] {
        if (rounds_index_ > 0) --rounds_index_;
        refreshButtons();
        update();
    });
    QObject::connect(plus_, &QPushButton::clicked, this, [this] {
        if (rounds_index_ + 1 < ladder().size()) ++rounds_index_;
        refreshButtons();
        update();
    });
    QObject::connect(exchange_, &QPushButton::clicked, this, [this] {
        sendCommon(Kind::Exchange, view_.infantry ? kBuy17 : kBuy42, exchangeRounds());
    });
    QObject::connect(remote_ammo_, &QPushButton::clicked, this, [this] {
        sendCommon(Kind::RemoteAmmo, kRemoteAmmo, view_.infantry ? kRemoteUnit17 : kRemoteUnit42);
    });
    QObject::connect(remote_heal_, &QPushButton::clicked, this,
                     [this] { sendCommon(Kind::RemoteHeal, kRemoteHeal, 0); });
    // 开能量机关撤不回(role-layouts D4,官方选手端开能量机关也是长按),按住一秒才发。
    // pressed/released 而不是 clicked:clicked 拿不到按下的时刻。
    QObject::connect(rune_, &QPushButton::pressed, this, [this] {
        rune_hold_.press(now_);
        refreshButtons();
    });
    QObject::connect(rune_, &QPushButton::released, this, [this] {
        const bool fired = rune_hold_.release(now_);
        refreshButtons();
        if (fired) sendRune();
    });

    setMinimumHeight(match_page_->minimumSizeHint().height() + kGap + kChipHeight);
    refreshButtons();
}

void SelfCommandPane::setView(const SelfCommandView& view) {
    const bool infantry_changed = view.infantry != view_.infantry;
    view_ = view;
    if (infantry_changed) {
        rounds_index_ = 2;
        placeRune();
    }
    if (wanted_shooter_ && view_.shooter == wanted_shooter_ && view_.chassis == wanted_chassis_) {
        wanted_shooter_.reset();
        wanted_chassis_.reset();
    }
    refreshButtons();
    update();
}

std::uint32_t SelfCommandPane::shooterChoice() const {
    return wanted_shooter_.value_or(view_.shooter.value_or(1));
}

std::uint32_t SelfCommandPane::chassisChoice() const {
    return wanted_chassis_.value_or(view_.chassis.value_or(1));
}

void SelfCommandPane::placeRune() {
    // 重装没有能量机关:整行拿掉,而不是留一行隐藏的按钮占着高度。
    auto* grid = static_cast<QGridLayout*>(match_page_->layout());
    grid->removeWidget(rune_);
    if (view_.infantry) grid->addWidget(rune_, 2, 0, 1, 2);
}

bool SelfCommandPane::confirmRevive() {
    // 官方选手端:读条结束后按 Y 免费复活。读条没满时服务器也不会执行,先不发。
    if (!view_.dead.value_or(false) || !view_.can_free_revive.value_or(false)) return false;
    return sendCommon(Kind::ConfirmRevive, kConfirmRevive, 0);
}

bool SelfCommandPane::payRevive() {
    // 官方选手端:复活框里点一下就花金币复活,不用按住。
    if (!view_.dead.value_or(false) || !view_.can_pay_revive.value_or(true)) return false;
    return sendCommon(Kind::PayRevive, kPayRevive, 0);
}

void SelfCommandPane::setLinkUp(bool up) {
    if (link_up_ == up) return;
    link_up_ = up;
    // 断线时丢掉按住中的状态:链路恢复后那次按下已经没有意义,继续计时会在操作手
    // 没按的情况下把指令发出去。
    if (!up) rune_hold_.cancel();
    refreshButtons();
    update();
}

void SelfCommandPane::setSender(
    std::function<bool(const char* topic, const std::string& payload)> sender) {
    sender_ = std::move(sender);
    refreshButtons();
}

void SelfCommandPane::setKeyboardMouseState(bool active, const QString& detail) {
    km_active_ = active;
    km_detail_ = detail;
    refreshButtons();
    update();
}

void SelfCommandPane::setRuneBadge(const QString& badge) {
    auto* rune = static_cast<GlassButton*>(rune_);
    rune->setBadge(badge);
    rune->setGlow(!badge.isEmpty());
}

void SelfCommandPane::tick(MonotonicMs now) {
    now_ = now;
    echo_result_ = echo_.update(now, fingerprint(last_kind_));
    // 服务器没接受(超时没变化):按钮回到服务器报的选择,不再假装选上了。
    if (last_kind_ == Kind::Performance && echo_result_.status == EchoStatus::Unconfirmed) {
        wanted_shooter_.reset();
        wanted_chassis_.reset();
    }
    refreshButtons();
    update();
}

// 只有步兵有赛前页(选性能体系);重装赛前看到的是比赛页,按钮按阶段置灰。
bool SelfCommandPane::preMatch() const {
    return view_.infantry && view_.stage && *view_.stage < kStageMatch;
}

const std::array<std::uint32_t, 6>& SelfCommandPane::ladder() const {
    return view_.infantry ? kLadder17 : kLadder42;
}

std::uint32_t SelfCommandPane::exchangeRounds() const { return ladder()[rounds_index_]; }

std::uint32_t SelfCommandPane::exchangeCost() const {
    return view_.infantry ? exchangeRounds() : exchangeRounds() * 10;
}

std::uint64_t SelfCommandPane::fingerprint(Kind kind) const {
    // CommonCommand 等三条指令没有显式回执:按各自会改动的状态判「服务器响应了」。
    std::uint64_t key = 0;
    const auto fold = [&key](std::uint64_t part) { key = key * 131u + part + 1u; };
    switch (kind) {
        case Kind::None: break;
        case Kind::Exchange:
        case Kind::RemoteAmmo:
            fold(view_.economy.value_or(0));
            fold(view_.ammo.value_or(0));
            break;
        case Kind::RemoteHeal:
            fold(view_.economy.value_or(0));
            fold(view_.hp.value_or(0));
            break;
        case Kind::ConfirmRevive:
        case Kind::PayRevive:
            fold(view_.dead.value_or(false) ? 1 : 0);
            fold(view_.economy.value_or(0));
            break;
        case Kind::Rune: fold(view_.rune_status.value_or(0)); break;
        case Kind::Performance:
            fold(view_.shooter.value_or(0));
            fold(view_.chassis.value_or(0));
            break;
    }
    return key;
}

void SelfCommandPane::afterSend(Kind kind) {
    last_kind_ = kind;
    echo_.sent(now_, static_cast<std::uint32_t>(kind), fingerprint(kind));
    echo_result_ = echo_.result();
    refreshButtons();
    update();
}

bool SelfCommandPane::sendCommon(Kind kind, std::uint32_t cmd_type, std::uint32_t param) {
    // 断线期间的点击直接丢弃,不排队补发。
    if (!link_up_ || !sender_) return false;
    if (!common_gate_.allow(now_)) return false;
    if (!sender_("CommonCommand", encode_common(cmd_type, param))) return false;
    afterSend(kind);
    return true;
}

void SelfCommandPane::sendPerformance(std::uint32_t shooter, std::uint32_t chassis) {
    if (!link_up_ || !sender_) return;
    if (!performance_gate_.allow(now_)) return;
    if (!sender_("RobotPerformanceSelectionCommand", encode_performance(shooter, chassis))) return;
    wanted_shooter_ = shooter;
    wanted_chassis_ = chassis;
    afterSend(Kind::Performance);
}

void SelfCommandPane::sendRune() {
    if (!link_up_ || !sender_) return;
    if (!rune_gate_.allow(now_)) return;
    if (!sender_("RuneActivateCommand", encode_rune_activate())) return;
    afterSend(Kind::Rune);
}

void SelfCommandPane::refreshButtons() {
    const bool live = link_up_ && static_cast<bool>(sender_);
    const bool pre = preMatch();
    pre_page_->setVisible(pre);
    match_page_->setVisible(!pre);

    // 规则手册 5.5.2:性能体系只有步兵能选,准备阶段开始后才能选,七分钟比赛开始后锁定。
    const bool choosing = live && view_.infantry && view_.stage && *view_.stage >= 1 &&
                          *view_.stage < kStageMatch;
    for (QPushButton* button : {chassis_hp_, chassis_power_, shooter_cooling_, shooter_burst_}) {
        button->setVisible(view_.infantry);
        button->setEnabled(choosing);
    }
    const auto check = [](QPushButton* button, bool on) {
        if (button->isChecked() != on) button->setChecked(on);
    };
    const std::optional<std::uint32_t> chassis = wanted_chassis_ ? wanted_chassis_ : view_.chassis;
    const std::optional<std::uint32_t> shooter = wanted_shooter_ ? wanted_shooter_ : view_.shooter;
    check(chassis_hp_, chassis == std::optional<std::uint32_t>(1));
    check(chassis_power_, chassis == std::optional<std::uint32_t>(2));
    check(shooter_cooling_, shooter == std::optional<std::uint32_t>(1));
    check(shooter_burst_, shooter == std::optional<std::uint32_t>(2));

    const bool playing = !view_.stage || *view_.stage == kStageMatch;
    const bool dead = view_.dead.value_or(false);
    const bool alive = !dead;
    exchange_->setText(QStringLiteral("兑换 %1发 · %2金").arg(exchangeRounds()).arg(exchangeCost()));
    exchange_->setEnabled(live && playing && alive);
    minus_->setEnabled(rounds_index_ > 0);
    plus_->setEnabled(rounds_index_ + 1 < ladder().size());
    remote_ammo_->setEnabled(live && playing && alive && view_.can_remote_ammo.value_or(true));
    // 满血时远程回血只会白花金币(规则:加血不超过上限),先置灰。
    const bool full = view_.hp && view_.max_hp && *view_.hp >= *view_.max_hp;
    remote_heal_->setEnabled(live && playing && alive && !full && view_.can_remote_heal.value_or(true));
    rune_->setVisible(view_.infantry);
    rune_->setEnabled(live && view_.infantry && playing && view_.rune_status.value_or(1) == 1);
    static_cast<GlassButton*>(rune_)->setProgress(rune_hold_.progress(now_));
    rune_->setText(rune_hold_.ready(now_) ? QStringLiteral("能量机关 松手")
                                          : QStringLiteral("能量机关 按住"));
}

QString SelfCommandPane::statusText() const {
    if (!link_up_) return QStringLiteral("断线，指令不会发送");
    if (!sender_) return QStringLiteral("未接入发送链路");
    if (km_active_)
        return QStringLiteral("键鼠控制中，Esc 退出") +
               (km_detail_.isEmpty() ? QString() : QStringLiteral(" · ") + km_detail_);
    const bool settled = echo_result_.status == EchoStatus::Idle || echo_result_.status == EchoStatus::Confirmed;
    // 阵亡时读条和复活价钱比「上一条指令已确认」要紧;1440x900 下面板只放得下这一行。
    if (view_.dead.value_or(false) && settled) return contextText();
    if (echo_result_.status != EchoStatus::Idle && last_kind_ != Kind::None) {
        QString what;
        switch (last_kind_) {
            case Kind::None: break;
            case Kind::Exchange: what = QStringLiteral("兑换"); break;
            case Kind::RemoteAmmo: what = QStringLiteral("远程补弹"); break;
            case Kind::RemoteHeal: what = QStringLiteral("远程回血"); break;
            case Kind::ConfirmRevive: what = QStringLiteral("免费复活"); break;
            case Kind::PayRevive: what = QStringLiteral("金币复活"); break;
            case Kind::Rune: what = QStringLiteral("开能量机关"); break;
            case Kind::Performance: what = QStringLiteral("选性能体系"); break;
        }
        return what + QStringLiteral(" · ") + echo_status_text(echo_result_);
    }
    return QStringLiteral("就绪");
}

QString SelfCommandPane::reviveEchoText() const {
    if (echo_result_.status == EchoStatus::Idle) return QString();
    if (last_kind_ == Kind::ConfirmRevive)
        return QStringLiteral("免费复活 · ") + echo_status_text(echo_result_);
    if (last_kind_ == Kind::PayRevive)
        return QStringLiteral("金币复活 · ") + echo_status_text(echo_result_);
    return QString();
}

QString SelfCommandPane::contextText() const {
    if (preMatch()) {
        if (!view_.infantry) return QStringLiteral("比赛开始后可兑换和复活");
        if (*view_.stage == 0) return QStringLiteral("准备阶段开始后可选性能体系");
        return QStringLiteral("选底盘和发射机构，开赛后锁定");
    }
    if (view_.dead.value_or(false)) {
        QString text = QStringLiteral("阵亡 读条 %1/%2")
                           .arg(view_.revive_current ? QString::number(*view_.revive_current) : kDash,
                                view_.revive_total ? QString::number(*view_.revive_total) : kDash);
        if (view_.revive_cost) text += QStringLiteral(" · 金币复活 %1金").arg(*view_.revive_cost);
        return text;
    }
    const QString heal = view_.remaining
                             ? QStringLiteral("回血 %1金").arg(remote_heal_cost(*view_.remaining))
                             : QStringLiteral("回血 --金");
    if (view_.infantry)
        return QStringLiteral("能量机关 %1 · %2").arg(rune_text(view_.rune_status, view_.rune_arms), heal);
    return QStringLiteral("远程补弹 %1金/%2发 · %3").arg(kRemotePrice).arg(kRemoteUnit42).arg(heal);
}

QString SelfCommandPane::headerHtml() const {
    const QString status = statusText();
    const bool bad_echo = echo_result_.status == EchoStatus::Unconfirmed || echo_result_.status == EchoStatus::Failed;
    QColor ink = theme::kTextBright;
    if (!link_up_) ink = theme::kDangerText;
    else if (!sender_) ink = theme::kTextMuted;
    else if (km_active_) ink = theme::kAccentText;
    else if (view_.dead.value_or(false) && status == contextText()) ink = theme::kDangerText;
    else if (bad_echo) ink = theme::kWarn;
    else if (status == QStringLiteral("就绪") || echo_result_.status == EchoStatus::Confirmed) ink = theme::kOkText;
    QString html = QStringLiteral("<span style='color:%1; font-size:10px'>●</span>&nbsp;<span style='color:%1'>%2</span>")
                       .arg(ink.name(), status.toHtmlEscaped());
    if (link_up_ && !view_.dead.value_or(false) && view_.out_of_combat)
        html += QStringLiteral("<span style='color:%1'>&nbsp;·&nbsp;%2</span>")
                    .arg(theme::kTextSecondary.name(),
                         *view_.out_of_combat ? QStringLiteral("脱战") : QStringLiteral("交战中"));
    return html;
}

// 宽度给小值:面板放在花名册那一列,列宽由花名册钉死(两侧花名册必须等宽),
// 这里报大了会把整列撑宽。
QSize SelfCommandPane::sizeHint() const {
    const QWidget* page = preMatch() ? pre_page_ : match_page_;
    return QSize(200, kChipHeight + kGap + page->sizeHint().height());
}

void SelfCommandPane::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const qreal w = width();
    if (w <= 0) return;
    // 顶上两块信息小牌子。赛前和阵亡时换成那一刻最要紧的事。
    const QRectF row(0, 0, w, kChipHeight);
    const qreal half = (w - kGap) / 2;
    const QRectF left(row.left(), row.top(), half, row.height());
    const QRectF right(row.left() + half + kGap, row.top(), half, row.height());
    if (preMatch()) {
        paint_chip(painter, row, contextText(), QString(), theme::kTextPrimary);
        return;
    }
    if (view_.dead.value_or(false)) {
        paint_chip(painter, left, QStringLiteral("阵亡"),
                   QStringLiteral("读条 %1/%2")
                       .arg(view_.revive_current ? QString::number(*view_.revive_current) : kDash,
                            view_.revive_total ? QString::number(*view_.revive_total) : kDash),
                   theme::kDangerText);
        paint_chip(painter, right, QStringLiteral("金币复活"),
                   view_.revive_cost ? QStringLiteral("%1 金").arg(*view_.revive_cost) : kDash, theme::kTextPrimary);
        return;
    }
    if (view_.infantry)
        paint_chip(painter, left, QStringLiteral("能量机关"), rune_text(view_.rune_status, view_.rune_arms),
                   theme::kTextPrimary);
    else
        paint_chip(painter, left, QStringLiteral("远程补弹"),
                   QStringLiteral("%1 金/%2 发").arg(kRemotePrice).arg(kRemoteUnit42), theme::kTextPrimary);
    paint_chip(painter, right, QStringLiteral("回血"),
               view_.remaining ? QStringLiteral("%1 金").arg(remote_heal_cost(*view_.remaining)) : kDash,
               theme::kTextPrimary);
}

}  // namespace rm_terminal
