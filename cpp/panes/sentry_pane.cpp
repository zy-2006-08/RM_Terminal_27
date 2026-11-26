#include "sentry_pane.h"

#include "roster_pane.h"
#include "glass.h"
#include "theme.h"

#include <QGridLayout>
#include <QPainter>
#include <QPaintEvent>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

namespace rm_terminal
{

    namespace
    {

        constexpr int kGap = 4;
        // 规则手册 5.7.2。
        constexpr MonotonicMs kPostureCooldownMs = 5000;
        constexpr std::uint32_t kCmdRemoteAmmo = 3;
        constexpr std::uint32_t kCmdRemoteHeal = 4;
        constexpr std::uint32_t kCmdRevive = 5;
        constexpr std::uint32_t kCmdGoldRevive = 6;
        constexpr std::uint32_t kPostureFirst = 7;
        constexpr std::uint32_t kPostureLast = 12;
        constexpr std::uint32_t kStageMatch = 4;
        // 哨兵在 robot_health 里的席位号:红 7,蓝 107。
        constexpr std::uint32_t kSentryRed = 7;
        constexpr std::uint32_t kSentryBlue = 107;

        const QString kDash = QStringLiteral("--");
        // 血条的斜角和战场数据分析、复活框的条一致(-24),同一套界面只有一个角度。
        constexpr qreal kBarSkewDeg = -24;

        template <class T>
        std::optional<T> present(const Field<T>& field)
        {
            if (field.quality != Quality::Valid || !field.value)
                return std::nullopt;
            return field.value;
        }

        // 小灰标签在上、大亮数值在下,照 BattleAnalysisPane 的格子。note 画在格子右上角。
        // 数值字号是格子高度的 45%,标签 20%:格子长高,字跟着长大。
        void paint_tile(QPainter& painter, const QRect& box, const QString& label, const QString& value,
                        const QColor& ink, bool numeric, const QString& note = QString(),
                        const QColor& note_ink = theme::kTextMuted)
        {
            theme::paintGlass(painter, box, 6, false, theme::kTile, theme::kRowBorder);
            const int label_px = std::max(theme::kMinLabelPx,
                                          static_cast<int>(std::lround(box.height() * 0.20)));
            const int pad = std::max(4, static_cast<int>(std::lround(box.height() * 0.12)));
            const QRect head(box.left() + pad, box.top() + pad / 2, box.width() - pad * 2, label_px + 3);
            painter.setFont(theme::labelPx(label_px));
            painter.setPen(theme::kTextMuted);
            painter.drawText(head, Qt::AlignLeft | Qt::AlignVCenter, label);
            if (!note.isEmpty())
            {
                painter.setPen(note_ink);
                painter.drawText(head, Qt::AlignRight | Qt::AlignVCenter, note);
            }

            const QRect body(box.left() + pad, head.bottom(), box.width() - pad * 2,
                             box.bottom() - head.bottom() - pad / 2);
            const int floor_px = numeric ? theme::kMinNumberPx : theme::kMinLabelPx;
            const int wanted = std::max(floor_px, static_cast<int>(std::lround(box.height() * 0.45)));
            const auto make = [numeric](int px)
            {
                return numeric ? theme::numericPx(px, QFont::Bold) : theme::labelPx(px, QFont::Bold);
            };
            const int px = theme::fitFontPx(value, wanted, floor_px, body.width(), make);
            painter.setFont(make(px));
            painter.setPen(ink);
            painter.drawText(body, Qt::AlignLeft | Qt::AlignVCenter, value);
        }

        QPushButton* make_button(QWidget* parent, const QString& text, const QString& name)
        {
            auto* button = new GlassButton(text, parent);
            button->setObjectName(name);
            button->setCursor(Qt::PointingHandCursor);
            button->setFocusPolicy(Qt::NoFocus);
            // 水平方向允许压到按钮自己的最小宽度(48)以下再省略文字,而不是让「花金币复活 ≈470金」
            // 这种最长的文字去决定整块面板的最小宽度 —— 那会把 320 宽的卡片顶宽,挤到地图那一列。
            // 竖直方向跟着格子长高(字号按高度算,见 dress),最矮 28。
            button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
            button->setMinimumHeight(28);
            return button;
        }

    } // namespace

    QString sentry_posture_text(std::uint32_t posture)
    {
        switch (posture)
        {
        case 1: return QStringLiteral("进攻");
        case 2: return QStringLiteral("防御");
        case 3: return QStringLiteral("移动");
        default: return kDash;
        }
    }

    QString sentry_command_text(std::uint32_t command_id)
    {
        switch (command_id)
        {
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

    SentryView sentry_view(const Snapshot& snapshot, const BuffBoard* buffs)
    {
        SentryView view;
        view.posture = present(snapshot.sentry.posture);
        view.weakened = present(snapshot.sentry.weakened);
        view.powered = present(snapshot.sentry.powered);
        view.economy = present(snapshot.game.economy);
        view.result = snapshot.sentry_result;
        if (present(snapshot.game.current_stage) == kStageMatch)
            view.remaining_sec = present(snapshot.game.stage_countdown_sec);
        view.automatic = present(snapshot.game.sentry_control) != 1u;
        if (buffs && buffs->sentry)
        {
            view.posture_effect = buffs->sentry->effect;
            view.posture_held_sec = buffs->sentry->held_sec;
            view.powered_left_sec = buffs->sentry->powered_left_sec;
        }

        if (const std::optional<std::uint32_t> faction = self_faction(snapshot))
        {
            const std::uint32_t seat = *faction == kRed ? kSentryRed : kSentryBlue;
            for (const RobotHealth& health : snapshot.robot_health)
            {
                if (health.id.value != seat)
                    continue;
                view.hp = present(health.current_hp);
                view.max_hp = present(health.max_hp);
                view.bullets = present(health.bullets);
                break;
            }
        }
        return view;
    }

    SentryPane::SentryPane(QWidget* parent) : QWidget(parent)
    {
        auto* column = new QVBoxLayout(this);
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(kGap);
        // 上面那一块(三格 + 血量)自绘,按 36% 的高度留白;十颗按钮分走剩下的 64%,
        // 四排一样高。以前按钮高度钉死 34,格子一大所有多出来的高度都堆在自绘区,
        // 2560 下三个格子各有 280 高、里面一个 26 像素的数字。
        canvas_ = new QWidget(this);
        canvas_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        canvas_->setAttribute(Qt::WA_TransparentForMouseEvents);
        column->addWidget(canvas_, 36);

        auto* grid = new QGridLayout;
        grid->setSpacing(4);
        for (std::size_t i = 0; i < postures_.size(); ++i)
        {
            const std::uint32_t id = kPostureFirst + static_cast<std::uint32_t>(i);
            postures_[i] = make_button(this, sentry_command_text(id),
                                       QStringLiteral("sentryPosture%1").arg(id));
            postures_[i]->setCheckable(true);
            grid->addWidget(postures_[i], static_cast<int>(i / 3), static_cast<int>(i % 3));
            QObject::connect(postures_[i], &QPushButton::clicked, this, [this, id]
                             { arm(id); });
        }
        column->addLayout(grid, 32);

        auto* actions = new QGridLayout;
        actions->setSpacing(4);
        ammo_ = make_button(this, QString(), QStringLiteral("sentryAmmo"));
        heal_ = make_button(this, QString(), QStringLiteral("sentryHeal"));
        revive_ = make_button(this, QString(), QStringLiteral("sentryRevive"));
        gold_revive_ = make_button(this, QString(), QStringLiteral("sentryGoldRevive"));
        actions->addWidget(ammo_, 0, 0);
        actions->addWidget(heal_, 0, 1);
        actions->addWidget(revive_, 1, 0);
        actions->addWidget(gold_revive_, 1, 1);
        column->addLayout(actions, 32);

        for (const std::uint32_t id : {kCmdRemoteAmmo, kCmdRemoteHeal, kCmdRevive, kCmdGoldRevive})
            QObject::connect(buttonFor(id), &QPushButton::clicked, this, [this, id]
                             { arm(id); });

        // 不钉最小高度:按钮在下面排好,上面画的信息放不下就少画几行。钉死的话格子矮时
        // 面板会比卡片高,最下面一排按钮被裁掉(1280x760 的指挥手、重装实测)。
        refreshButtons();
    }

    void SentryPane::resizeEvent(QResizeEvent* event)
    {
        QWidget::resizeEvent(event);
        refreshButtons();
    }

    void SentryPane::setView(const SentryView& view)
    {
        view_ = view;
        refreshButtons();
        update();
    }

    void SentryPane::setLinkUp(bool up)
    {
        if (link_up_ == up)
            return;
        link_up_ = up;
        if (!up)
            armed_.reset();
        refreshButtons();
        update();
    }

    void SentryPane::setSender(
        std::function<bool(const char* topic, const std::string& payload)> sender)
    {
        sender_ = std::move(sender);
    }

    std::uint32_t SentryPane::lastPostureCommand() const
    {
        if (!view_.result)
            return 0;
        const std::uint32_t id = view_.result->command_id;
        return id >= kPostureFirst && id <= kPostureLast ? id : 0;
    }

    void SentryPane::tick(MonotonicMs now)
    {
        now_ = now;
        // SentryCtrlResult 是显式回执,按 command_id 核对。同一条结果只处理一次:
        // received_at 不变就是同一条,重复处理会把 5 秒冷却一直往后推。
        if (view_.result &&
            (view_.result->command_id != last_seen_result_id_ ||
             last_seen_result_at_ != std::optional<MonotonicMs>(view_.result->received_at)))
        {
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

    QPushButton* SentryPane::buttonFor(std::uint32_t command_id) const
    {
        switch (command_id)
        {
        case kCmdRemoteAmmo: return ammo_;
        case kCmdRemoteHeal: return heal_;
        case kCmdRevive: return revive_;
        case kCmdGoldRevive: return gold_revive_;
        default: break;
        }
        if (command_id < kPostureFirst || command_id > kPostureLast)
            return nullptr;
        return postures_[command_id - kPostureFirst];
    }

    std::optional<std::uint32_t> SentryPane::priceOf(std::uint32_t command_id) const
    {
        return sentry_cost(command_id, view_.remaining_sec, view_.automatic);
    }

    void SentryPane::arm(std::uint32_t command_id)
    {
        const QPushButton* button = buttonFor(command_id);
        if (!button || !button->isEnabled())
            return;
        if (armed_ == command_id && now_ - armed_at_ < kArmedMs)
        {
            disarm();
            send(command_id);
            return;
        }
        armed_ = command_id;
        armed_at_ = now_;
        refreshButtons();
        update();
    }

    void SentryPane::disarm()
    {
        if (!armed_)
            return;
        armed_.reset();
        refreshButtons();
        update();
    }

    void SentryPane::send(std::uint32_t command_id)
    {
        if (!link_up_ || !sender_)
            return;
        if (command_id >= kPostureFirst && command_id <= kPostureLast && postureCoolingDown())
            return;
        if (!gate_.allow(now_))
            return;
        if (!sender_("SentryCtrlCommand", encode_sentry(command_id)))
            return;
        // baseline 固定为 0,与 update() 里传的 0 一致:哨兵靠 SentryCtrlResult 显式
        // 回执,没有可比的状态指纹,超时就是「未确认」。
        echo_.sent(now_, command_id, 0);
        echo_result_ = echo_.result();
        refreshButtons();
        update();
    }

    bool SentryPane::postureCoolingDown() const
    {
        if (!posture_cooldown_from_)
            return false;
        return now_ - *posture_cooldown_from_ < kPostureCooldownMs;
    }

    void SentryPane::refreshButtons()
    {
        const bool live = link_up_ && static_cast<bool>(sender_);
        const bool cooling = postureCoolingDown();
        // 武装过期、或者那颗按钮已经不能点了,都要自己解除:操作手不会再点它,
        // 留着高亮会让人以为还能确认。
        if (armed_ && (now_ - armed_at_ >= kArmedMs || !live ||
                       (*armed_ >= kPostureFirst && *armed_ <= kPostureLast && cooling)))
            armed_.reset();

        const std::uint32_t shown =
            view_.posture ? *view_.posture + (view_.powered.value_or(false) ? 9u : 6u) : 0u;
        for (std::size_t i = 0; i < postures_.size(); ++i)
        {
            const std::uint32_t id = kPostureFirst + static_cast<std::uint32_t>(i);
            postures_[i]->setEnabled(live && !cooling);
            // 协议只发 posture 1/2/3 和 powered,不发「按的是哪个按钮」。
            // 强化三档对应 10/11/12,普通三档对应 7/8/9。
            const bool chosen = shown == id;
            if (postures_[i]->isChecked() != chosen)
                postures_[i]->setChecked(chosen);
            dress(postures_[i], id, sentry_command_text(id));
        }
        for (const auto& [id, name] : {std::pair{kCmdRemoteAmmo, QStringLiteral("远程补弹")},
                                       std::pair{kCmdRemoteHeal, QStringLiteral("远程回血")},
                                       std::pair{kCmdRevive, QStringLiteral("确认复活")},
                                       std::pair{kCmdGoldRevive, QStringLiteral("花金币复活")}})
        {
            buttonFor(id)->setEnabled(live);
            dress(buttonFor(id), id, name);
        }
    }

    void SentryPane::dress(QPushButton* button, std::uint32_t command_id, const QString& name)
    {
        auto* glass = static_cast<GlassButton*>(button);
        const std::optional<std::uint32_t> price = priceOf(command_id);
        const bool armed = armed_ == command_id;
        // 回血和金币复活按比赛时间涨价,只能算出估价,用 ≈ 说明,不另占一行字。
        const bool estimated = command_id == kCmdRemoteHeal || command_id == kCmdGoldRevive;
        const QString money = price ? (estimated ? QStringLiteral("≈%1金").arg(*price)
                                                 : QStringLiteral("%1金").arg(*price))
                                    : QString();
        // 六颗姿态按钮的价钱都一样,写六遍是白占地方(也把三列挤宽),单价改到卡片标题右侧
        // 说一次(headerText)。花钱不一样的那四颗才各自标价。
        const bool posture = command_id >= kPostureFirst && command_id <= kPostureLast;
        const QString tail = posture ? QString() : (money.isEmpty() ? QString() : QStringLiteral(" ") + money);
        button->setText(armed ? (money.isEmpty() ? QStringLiteral("再点确认")
                                                 : QStringLiteral("再点确认 ") + money)
                              : name + tail);
        glass->setKind(armed ? GlassButton::Kind::Accent : GlassButton::Kind::Command);
        glass->setGlow(armed);
        glass->setDimTail(!armed && !tail.isEmpty());
        // 字号是这颗按钮高度的 38%:按钮跟着格子长高,字跟着按钮长大。
        glass->setTextPx(std::max(theme::kMinLabelPx,
                                  static_cast<int>(std::lround(button->height() * 0.38))));
        button->setToolTip(armed ? QStringLiteral("再点一次发送，%1 秒后自动取消")
                                       .arg((kArmedMs - (now_ - armed_at_) + 999) / 1000)
                                 : (estimated && price ? QStringLiteral("按当前比赛时间估算的总价")
                                                       : QString()));
    }

    bool SentryPane::buttonsEnabled() const { return ammo_->isEnabled() && revive_->isEnabled(); }

    QString SentryPane::statusText() const
    {
        if (!link_up_)
            return QStringLiteral("断线，指令不会发送");
        if (!sender_)
            return QStringLiteral("未接入发送链路");
        if (echo_result_.status != EchoStatus::Idle && echo_.tag() != 0)
            return sentry_command_text(echo_.tag()) + QStringLiteral(" · ") +
                   echo_status_text(echo_result_);
        if (!view_.posture)
            return QStringLiteral("等待哨兵数据");
        // 一切正常时不留字:状态槽只在真有事(断线、未确认、失败、冷却)时才占地方。
        return QString();
    }

    QString SentryPane::headerText() const
    {
        return view_.automatic ? QStringLiteral("干预 50 金/次") : QStringLiteral("半自动 · 不花金币");
    }

    QString SentryPane::lastResultText() const
    {
        if (!view_.result)
            return QString();
        return sentry_command_text(view_.result->command_id) +
               (view_.result->result_code == 0
                    ? QStringLiteral(" 成功")
                    : QStringLiteral(" 失败（码 %1）").arg(view_.result->result_code));
    }

    QSize SentryPane::sizeHint() const { return QSize(380, 310); }

    // 唯一的状态槽。按紧急程度排:断线 > 失败/未确认 > 等回显 > 刚成功 > 姿态冷却。
    // 一切正常且没有新结果时返回空,槽整个不占地方。
    SentryPane::Banner SentryPane::bannerNow() const
    {
        if (!link_up_)
            return {QStringLiteral("断线，指令不会发送"), theme::kDangerText};
        if (!sender_)
            return {QStringLiteral("未接入发送链路"), theme::kDangerText};
        if (echo_result_.status == EchoStatus::Failed || echo_result_.status == EchoStatus::Unconfirmed)
            return {statusText(), echo_result_.status == EchoStatus::Failed ? theme::kDangerText
                                                                            : theme::kWarn};
        if (echo_result_.status == EchoStatus::Pending)
            return {statusText(), theme::kAccentText};
        // 成功只亮一下(kResultFlashMs)就收起来:常驻一行「成功」没人会再看。
        if (view_.result && view_.result->result_code == 0 &&
            now_ >= view_.result->received_at && now_ - view_.result->received_at < kResultFlashMs)
            return {lastResultText(), theme::kOkText};
        if (view_.result && view_.result->result_code != 0)
            return {lastResultText(), theme::kDangerText};
        if (postureCoolingDown())
            return {QStringLiteral("姿态冷却 %1 秒")
                        .arg((kPostureCooldownMs - (now_ - *posture_cooldown_from_) + 999) / 1000),
                    theme::kWarn};
        if (!view_.posture)
            return {QStringLiteral("等待 SentryStatusSync"), theme::kTextMuted};
        // 没别的事时,这一行写当前姿态的实际效果(表 5-28)。
        if (!view_.posture_effect.isEmpty())
            return {view_.posture_effect, theme::kTextSecondary};
        return {};
    }

    void SentryPane::paintEvent(QPaintEvent*)
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const int w = width();
        // 自绘区就是 canvas_ 那块地(布局分的 36%)。和第一排按钮之间留一口气:
        // 血条贴着按钮边框时两条线会糊成一条。
        const int canvas_h = canvas_ ? std::max(canvas_->height() - kGap, 0) : 0;
        if (canvas_h <= 0 || w <= 0)
            return;

        const Banner banner = bannerNow();
        const int gap = std::max(canvas_h / 24, 3);
        // 状态槽按自绘区高度的 18% 给,字是槽高的 52%。
        const int status_h = banner.text.isEmpty()
                                 ? 0
                                 : std::clamp(static_cast<int>(std::lround(canvas_h * 0.18)), 16, 46);

        int bottom = canvas_h;
        if (!banner.text.isEmpty())
        {
            const QRect slot(0, bottom - status_h, w, status_h);
            theme::paintGlass(painter, slot, 4, false, theme::kChip, theme::kRowBorder);
            const int px = std::max(theme::kMinLabelPx,
                                    static_cast<int>(std::lround(status_h * 0.52)));
            painter.setFont(theme::labelPx(px, QFont::DemiBold));
            painter.setPen(banner.ink);
            const QRect text_box = slot.adjusted(px / 2, 0, -px / 2, 0);
            painter.drawText(text_box, Qt::AlignLeft | Qt::AlignVCenter,
                             painter.fontMetrics().elidedText(banner.text, Qt::ElideRight,
                                                              text_box.width()));
            bottom -= status_h + gap;
        }

        const double ratio = view_.hp && view_.max_hp && *view_.max_hp > 0
                                 ? static_cast<double>(*view_.hp) / *view_.max_hp
                                 : 0.0;
        const QColor hp_ink = view_.hp ? theme::healthColor(ratio) : theme::kTextMuted;
        // 整块面板的高度里三格占 20%、血量占 16%(按钮那 64% 由布局管)。这里按剩下的
        // 自绘高度把这两段按 20:16 分,格子拖大两段一起长,字号再按各自段高算。
        const int tiles_h = std::max(0, (bottom - gap) * 20 / 36);
        const int hp_h = std::max(0, bottom - gap - tiles_h);
        const int bar_h = std::clamp(static_cast<int>(std::lround(hp_h * 0.22)), 5, 20);

        // 矮到放不下时血量优先,三格先退 —— 姿态在按钮上还看得出来,血量没有别的地方可看。
        int y = 0;
        const bool room_for_tiles = tiles_h >= 34 && hp_h >= 24;
        if (room_for_tiles)
        {
            const int unit = (w - gap * 2) / 3;
            // 姿态格右上角:弱化了写弱化;强化中写强化还剩几秒;平时写这个姿态累计多久,快到 3 分钟变黄。
            QString note;
            QColor note_ink = theme::kTextMuted;
            if (view_.weakened.value_or(false))
            {
                note = QStringLiteral("已弱化");
                note_ink = theme::kDangerText;
            }
            else if (view_.powered.value_or(false))
            {
                note = view_.powered_left_sec ? QStringLiteral("强化 %1秒").arg(*view_.powered_left_sec)
                                              : QStringLiteral("强化");
                note_ink = theme::kOkText;
            }
            else if (view_.posture_held_sec)
            {
                const std::int32_t held = *view_.posture_held_sec;
                note = QStringLiteral("%1:%2/3:00").arg(held / 60).arg(held % 60, 2, 10, QLatin1Char('0'));
                if (held >= 150)
                    note_ink = theme::kWarn;
            }
            paint_tile(painter, QRect(0, y, unit, tiles_h), QStringLiteral("姿态"),
                       view_.posture ? sentry_posture_text(*view_.posture) : kDash,
                       view_.posture ? theme::kAccentText : theme::kTextMuted, false, note, note_ink);
            paint_tile(painter, QRect(unit + gap, y, unit, tiles_h), QStringLiteral("发弹"),
                       view_.bullets ? QString::number(*view_.bullets) : kDash,
                       view_.bullets ? theme::kTextPrimary : theme::kTextMuted, true);
            paint_tile(painter, QRect((unit + gap) * 2, y, w - (unit + gap) * 2, tiles_h),
                       QStringLiteral("经济"),
                       view_.economy ? QString::number(*view_.economy) : kDash,
                       view_.economy ? theme::kTextPrimary : theme::kTextMuted, true);
            y += tiles_h + gap;
        }

        const int band = room_for_tiles ? hp_h : bottom;
        if (band <= 0)
            return;
        // 血量数字是这一条带的 60%,标签 26%。
        const int hp_value_px = std::max(theme::kMinNumberPx,
                                         static_cast<int>(std::lround(band * 0.60)));
        const int hp_label_px = std::max(theme::kMinLabelPx,
                                         static_cast<int>(std::lround(band * 0.26)));
        const QRect head(0, y, w, band - bar_h - std::max(1, band / 20));
        painter.setFont(theme::labelPx(hp_label_px));
        painter.setPen(theme::kTextSecondary);
        painter.drawText(head, Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("血量"));
        if (view_.hp)
        {
            const QFont big = theme::numericPx(hp_value_px, QFont::Bold);
            const QFont small = theme::numericPx(hp_label_px);
            const QString total = view_.max_hp ? QStringLiteral(" / %1").arg(*view_.max_hp) : QString();
            const qreal tail = QFontMetricsF(small).horizontalAdvance(total);
            painter.setFont(small);
            painter.setPen(theme::kTextMuted);
            painter.drawText(head, Qt::AlignRight | Qt::AlignVCenter, total);
            painter.setFont(big);
            painter.setPen(hp_ink);
            painter.drawText(head.adjusted(0, 0, -static_cast<int>(tail), 0),
                             Qt::AlignRight | Qt::AlignVCenter, QString::number(*view_.hp));
        }
        else
        {
            painter.setFont(theme::labelPx(hp_label_px));
            painter.setPen(theme::kTextMuted);
            painter.drawText(head, Qt::AlignRight | Qt::AlignVCenter, kDash);
        }

        const QRectF track(0, y + band - bar_h, w, bar_h);
        painter.setPen(Qt::NoPen);
        painter.setBrush(theme::kSurfaceRaised);
        painter.drawPath(theme::skewBar(track, kBarSkewDeg));
        if (view_.hp && view_.max_hp && *view_.max_hp > 0)
        {
            painter.setBrush(hp_ink);
            painter.drawPath(theme::skewBar(
                QRectF(track.left(), track.top(), track.width() * std::clamp(ratio, 0.0, 1.0),
                       track.height()),
                kBarSkewDeg));
        }
    }

} // namespace rm_terminal
