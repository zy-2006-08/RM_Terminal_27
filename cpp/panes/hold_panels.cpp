#include "hold_panels.h"

#include "input_method_guard.h"
#include "theme.h"

#include <QEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QPainter>

#include <algorithm>
#include <array>
#include <cmath>

namespace rm_terminal
{

    namespace
    {

        constexpr int kPad = 16;
        constexpr int kInjuryWidth = 300;
        constexpr int kRowHeight = 24;
        constexpr int kModuleHeight = 28;
        constexpr int kTeamWidth = 1040;
        constexpr int kTeamRowHeight = 50;

        template <class T>
        std::optional<T> valid(const Field<T>& field)
        {
            if (field.quality != Quality::Valid || !field.value)
                return std::nullopt;
            return field.value;
        }

        QString type_name(std::uint32_t id)
        {
            switch (id % 100)
            {
            case 1: return QStringLiteral("重装");
            case 3:
            case 4: return QStringLiteral("步兵");
            case 7: return QStringLiteral("哨兵");
            default: return QStringLiteral("机器人");
            }
        }

        QString number_or_dash(std::optional<std::uint32_t> value)
        {
            return value ? QString::number(*value) : QStringLiteral("-");
        }

        void place_left(QWidget* panel)
        {
            QWidget* host = panel->parentWidget();
            if (!host)
                return;
            const QSize size = panel->sizeHint();
            panel->setGeometry(24, std::max(0, (host->height() - size.height()) / 2), size.width(),
                               std::min(size.height(), host->height()));
        }

        void place_centre(QWidget* panel)
        {
            QWidget* host = panel->parentWidget();
            if (!host)
                return;
            const QSize size = panel->sizeHint();
            const int width = std::min(size.width(), host->width() - 40);
            panel->setGeometry((host->width() - width) / 2, std::max(0, (host->height() - size.height()) / 2), width,
                               std::min(size.height(), host->height()));
        }

    } // namespace

    InjuryView injury_view(const Snapshot& snapshot)
    {
        InjuryView view;
        const bool hero = snapshot.self && snapshot.self->value % 100 == 1;
        const RobotState* self = nullptr;
        if (snapshot.self)
        {
            const auto found = snapshot.robots.find(*snapshot.self);
            if (found != snapshot.robots.end())
                self = &found->second;
        }
        const auto injury = [self](auto member) -> std::optional<std::uint32_t>
        {
            return self ? valid(self->injury.*member) : std::nullopt;
        };
        using Injury = InjuryFields<Field>;
        view.total = injury(&Injury::total);
        view.rows = {{QStringLiteral("17mm 弹丸"), injury(&Injury::small_projectile)},
                     {QStringLiteral("42mm 弹丸"), injury(&Injury::large_projectile)},
                     {QStringLiteral("撞击"), injury(&Injury::collision)},
                     {QStringLiteral("飞镖溅射"), injury(&Injury::dart_splash)},
                     {QStringLiteral("模块离线"), injury(&Injury::module_offline)},
                     {QStringLiteral("异常离线"), injury(&Injury::offline)},
                     {QStringLiteral("判罚"), injury(&Injury::penalty)},
                     {QStringLiteral("服务器判定战亡"), injury(&Injury::server_kill)}};
        const auto module = [self](auto member) -> std::optional<std::uint32_t>
        {
            return self ? valid(self->modules.*member) : std::nullopt;
        };
        using Modules = ModuleFields<Field>;
        view.modules = {{QStringLiteral("装甲"), module(&Modules::armor)},
                        hero ? InjuryView::Module{QStringLiteral("42mm 测速"), module(&Modules::big_shooter)}
                             : InjuryView::Module{QStringLiteral("17mm 测速"), module(&Modules::small_shooter)},
                        {QStringLiteral("图传"), module(&Modules::video_transmission)},
                        {QStringLiteral("RFID"), module(&Modules::rfid)},
                        {QStringLiteral("灯条"), module(&Modules::light_strip)},
                        {QStringLiteral("电容"), module(&Modules::capacitor)},
                        {QStringLiteral("UWB 定位"), module(&Modules::uwb)},
                        {QStringLiteral("电源管理"), module(&Modules::power_manager)},
                        {QStringLiteral("主控"), module(&Modules::main_controller)}};
        return view;
    }

    InjuryPanel::InjuryPanel(QWidget* host) : QWidget(host)
    {
        setObjectName(QStringLiteral("injuryPanel"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setVisible(false);
    }

    void InjuryPanel::setView(const InjuryView& view)
    {
        view_ = view;
        if (isVisible())
            update();
    }

    void InjuryPanel::setHeld(bool held)
    {
        if (held)
        {
            place_left(this);
            show();
            raise();
        }
        else
        {
            hide();
        }
    }

    QSize InjuryPanel::sizeHint() const
    {
        const int rows = static_cast<int>(view_.rows.size());
        const int modules = (static_cast<int>(view_.modules.size()) + 1) / 2;
        return QSize(kInjuryWidth, kPad + 30 + (rows + 1) * kRowHeight + 14 + 30 + modules * (kModuleHeight + 6) + kPad);
    }

    void InjuryPanel::paintEvent(QPaintEvent*)
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        theme::paintGlass(painter, QRectF(rect()).adjusted(1, 1, -1, -1), 10, false, theme::kOverlay,
                          theme::kBorderBright, true);
        const int inner = width() - kPad * 2;
        int y = kPad;
        painter.setFont(theme::labelPx(16, QFont::Bold));
        painter.setPen(theme::kTextPrimary);
        painter.drawText(QRect(kPad, y, inner, 26), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("伤害面板"));
        painter.setFont(theme::labelPx(11));
        painter.setPen(theme::kTextMuted);
        painter.drawText(QRect(kPad, y, inner, 26), Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("这条命受到的伤害，复活后清零"));
        y += 30;
        const std::uint32_t total = view_.total.value_or(0);
        const auto line = [&](const QString& name, std::optional<std::uint32_t> value, bool bold)
        {
            painter.setFont(theme::labelPx(13, bold ? QFont::Bold : QFont::Medium));
            painter.setPen(theme::kTextBright);
            painter.drawText(QRect(kPad, y, inner, kRowHeight), Qt::AlignLeft | Qt::AlignVCenter, name);
            QString text = QStringLiteral("--");
            if (value)
            {
                const int percent = total > 0 ? static_cast<int>(std::lround(*value * 100.0 / total)) : 0;
                text = bold ? QString::number(*value) : QStringLiteral("%1  |  %2%").arg(*value).arg(percent);
            }
            painter.setFont(theme::numericPx(14, QFont::DemiBold));
            painter.setPen(value && *value > 0 ? theme::kTextPrimary : theme::kTextMuted);
            painter.drawText(QRect(kPad, y, inner, kRowHeight), Qt::AlignRight | Qt::AlignVCenter, text);
            y += kRowHeight;
        };
        for (const auto& row : view_.rows)
            line(row.name, row.value, false);
        line(QStringLiteral("合计"), view_.total, true);
        y += 6;
        painter.setPen(QPen(theme::kBorder, 1));
        painter.drawLine(QPointF(kPad, y + 0.5), QPointF(width() - kPad, y + 0.5));
        y += 8;
        painter.setFont(theme::labelPx(16, QFont::Bold));
        painter.setPen(theme::kTextPrimary);
        painter.drawText(QRect(kPad, y, inner, 26), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("模块状态"));
        painter.setFont(theme::labelPx(11));
        painter.setPen(theme::kTextMuted);
        painter.drawText(QRect(kPad, y, inner, 26), Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("绿框在线，红框离线"));
        y += 30;
        const int column_width = (inner - 8) / 2;
        for (std::size_t index = 0; index < view_.modules.size(); ++index)
        {
            const auto& module = view_.modules[index];
            const QRectF box(kPad + static_cast<int>(index % 2) * (column_width + 8),
                             y + static_cast<int>(index / 2) * (kModuleHeight + 6), column_width, kModuleHeight);
            const QColor stroke = !module.state ? theme::kOffline : *module.state == 1 ? theme::kOk
                                                                                       : theme::kDanger;
            theme::paintGlass(painter, box, 4, false, theme::kTile, stroke);
            painter.setFont(theme::labelPx(13, QFont::DemiBold));
            painter.setPen(module.state ? theme::kTextPrimary : theme::kTextMuted);
            QString name = module.name;
            if (module.state == std::optional<std::uint32_t>(2))
                name += QStringLiteral("（安装不规范）");
            painter.drawText(box, Qt::AlignCenter, name);
        }
    }

    TeamView team_view(const Snapshot& snapshot)
    {
        TeamView view;
        view.own_faction = snapshot.self ? faction_of(snapshot.self->value) : kRed;
        view.red_damage = valid(snapshot.game.red_total_damage);
        view.blue_damage = valid(snapshot.game.blue_total_damage);
        view.own_dart_hits = view.own_faction == kRed ? snapshot.dart_hits.red : snapshot.dart_hits.blue;
        const RobotState* self = nullptr;
        if (snapshot.self)
        {
            const auto found = snapshot.robots.find(*snapshot.self);
            if (found != snapshot.robots.end())
                self = &found->second;
        }
        std::uint32_t own_total = 0;
        bool own_known = false;
        for (const std::uint32_t number : kFieldRobots)
        {
            for (const std::uint32_t faction : {kRed, kBlue})
            {
                TeamView::Row row;
                row.id = faction == kRed ? number : number + 100;
                row.type = type_name(number);
                const auto health = std::find_if(snapshot.robot_health.begin(), snapshot.robot_health.end(),
                                                 [&](const RobotHealth& robot)
                                                 { return robot.id.value == row.id; });
                if (health != snapshot.robot_health.end())
                {
                    row.hp = valid(health->current_hp);
                    row.max_hp = valid(health->max_hp);
                }
                row.self = snapshot.self && snapshot.self->value == row.id;
                if (row.self && self)
                {
                    row.power = valid(self->statics.max_power);
                    row.max_heat = valid(self->statics.max_heat);
                    row.cooling = valid(self->statics.heat_cooldown_rate);
                    if (!row.hp)
                        row.hp = valid(self->dynamic.current_hp);
                    if (!row.max_hp)
                        row.max_hp = valid(self->statics.max_hp);
                }
                if (faction == view.own_faction && row.hp)
                {
                    own_total += *row.hp;
                    own_known = true;
                }
                (faction == kRed ? view.red : view.blue).push_back(row);
            }
        }
        if (own_known)
            view.own_hp_total = own_total;
        return view;
    }

    TeamPanel::TeamPanel(QWidget* host) : QWidget(host)
    {
        setObjectName(QStringLiteral("teamPanel"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setVisible(false);
    }

    void TeamPanel::setView(const TeamView& view)
    {
        view_ = view;
        if (isVisible())
            update();
    }

    void TeamPanel::setHeld(bool held)
    {
        if (held)
        {
            place_centre(this);
            show();
            raise();
        }
        else
        {
            hide();
        }
    }

    QSize TeamPanel::sizeHint() const
    {
        const int rows = static_cast<int>(std::max(view_.red.size(), view_.blue.size()));
        return QSize(kTeamWidth, kPad + 54 + 30 + rows * (kTeamRowHeight + 6) + 30 + kPad);
    }

    void TeamPanel::paintEvent(QPaintEvent*)
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        theme::paintGlass(painter, QRectF(rect()).adjusted(1, 1, -1, -1), 10, false, theme::kOverlay,
                          theme::kBorderBright, true);
        const int inner = width() - kPad * 2;

        // 总伤害对比:红方 N ✕ 蓝方 N。
        const QRect score(width() / 2 - 160, kPad, 320, 44);
        theme::paintGlass(painter, QRectF(score), 6, false, theme::kTile, theme::kRowBorder);
        painter.setFont(theme::numericPx(26, QFont::Bold));
        painter.setPen(theme::kRedText);
        painter.drawText(QRect(score.left(), score.top(), 130, score.height()), Qt::AlignRight | Qt::AlignVCenter,
                         number_or_dash(view_.red_damage));
        painter.setPen(theme::kBlueText);
        painter.drawText(QRect(score.right() - 130, score.top(), 130, score.height()), Qt::AlignLeft | Qt::AlignVCenter,
                         number_or_dash(view_.blue_damage));
        painter.setFont(theme::labelPx(12));
        painter.setPen(theme::kTextSecondary);
        painter.drawText(score, Qt::AlignCenter, QStringLiteral("总伤害"));
        painter.setFont(theme::labelPx(13));
        painter.setPen(theme::kTextBright);
        painter.drawText(QRect(score.right() + 24, score.top(), width() - score.right() - 24 - kPad, score.height()),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("己方机器人总剩余血量 %1　　飞镖命中 %2")
                             .arg(number_or_dash(view_.own_hp_total))
                             .arg(view_.own_dart_hits));

        const int top = kPad + 54;
        const int half = (inner - 16) / 2;
        const std::array<QString, 6> headers{QStringLiteral("#"), QStringLiteral("机器人"),
                                             QStringLiteral("底盘功率上限"), QStringLiteral("热量上限"),
                                             QStringLiteral("热量冷却"), QStringLiteral("射速上限")};
        const std::array<int, 6> widths{28, 180, 0, 0, 0, 0};
        const auto table = [&](const std::vector<TeamView::Row>& rows, int left, const QColor& tone)
        {
            const int rest = (half - widths[0] - widths[1]) / 4;
            const auto column_x = [&](int column)
            {
                int x = left;
                for (int index = 0; index < column; ++index)
                    x += index < 2 ? widths[static_cast<std::size_t>(index)] : rest;
                return x;
            };
            const auto column_w = [&](int column)
            { return column < 2 ? widths[static_cast<std::size_t>(column)] : rest; };
            painter.setFont(theme::labelPx(12));
            painter.setPen(theme::kTextSecondary);
            for (int column = 0; column < 6; ++column)
                painter.drawText(QRect(column_x(column), top, column_w(column), 24), Qt::AlignCenter,
                                 headers[static_cast<std::size_t>(column)]);
            painter.setPen(QPen(tone, 2));
            painter.drawLine(QPointF(left, top + 27), QPointF(left + half, top + 27));
            int y = top + 32;
            for (const auto& row : rows)
            {
                const QRectF box(left, y, half, kTeamRowHeight);
                theme::paintGlass(painter, box, 6, false, row.self ? QColor(36, 60, 92, 150) : theme::kTile,
                                  row.self ? theme::kCyan : theme::kRowBorder);
                painter.setFont(theme::numericPx(16, QFont::Bold));
                painter.setPen(theme::kTextPrimary);
                painter.drawText(QRect(column_x(0), y, column_w(0), kTeamRowHeight), Qt::AlignCenter,
                                 QString::number(row.id % 100));
                painter.setFont(theme::labelPx(13, QFont::DemiBold));
                painter.drawText(QRect(column_x(1), y + 6, column_w(1), 18), Qt::AlignLeft | Qt::AlignVCenter,
                                 row.self ? row.type + QStringLiteral(" · 本机") : row.type);
                const QRectF bar(column_x(1), y + 28, column_w(1) - 70, 6);
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(255, 255, 255, 20));
                painter.drawRect(bar);
                if (row.hp && row.max_hp && *row.max_hp > 0)
                {
                    const double ratio = std::clamp(static_cast<double>(*row.hp) / *row.max_hp, 0.0, 1.0);
                    painter.setBrush(tone);
                    painter.drawRect(QRectF(bar.left(), bar.top(), bar.width() * ratio, bar.height()));
                }
                painter.setFont(theme::numericPx(12, QFont::DemiBold));
                painter.setPen(theme::kTextBright);
                painter.drawText(QRect(static_cast<int>(bar.right()) + 6, y + 22, 64, 18), Qt::AlignLeft | Qt::AlignVCenter,
                                 QStringLiteral("%1/%2").arg(number_or_dash(row.hp), number_or_dash(row.max_hp)));
                const QString cooling = row.cooling ? QString::number(*row.cooling, 'f', 0) : QStringLiteral("-");
                const std::array<QString, 4> values{number_or_dash(row.power), number_or_dash(row.max_heat), cooling,
                                                    QStringLiteral("-")};
                painter.setFont(theme::numericPx(16, QFont::Bold));
                for (int column = 2; column < 6; ++column)
                {
                    const QString& text = values[static_cast<std::size_t>(column - 2)];
                    painter.setPen(text == QStringLiteral("-") ? theme::kTextMuted : theme::kTextPrimary);
                    painter.drawText(QRect(column_x(column), y, column_w(column), kTeamRowHeight), Qt::AlignCenter, text);
                }
                y += kTeamRowHeight + 6;
            }
        };
        table(view_.red, kPad, theme::kFillRed);
        table(view_.blue, kPad + half + 16, theme::kFillBlue);
        painter.setFont(theme::labelPx(11));
        painter.setPen(theme::kTextMuted);
        painter.drawText(QRect(kPad, height() - kPad - 22, inner, 22), Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("上限值协议只发本机的，别的车只有血量；射速上限协议不发。飞镖命中从终端打开时数起。松开 Tab 收起"));
    }

    HoldKeyFilter::HoldKeyFilter(QObject* parent) : QObject(parent) {}

    void HoldKeyFilter::releaseAll()
    {
        if (tab_)
        {
            tab_ = false;
            Q_EMIT tabChanged(false);
        }
        if (tilde_)
        {
            tilde_ = false;
            Q_EMIT tildeChanged(false);
        }
        if (rune_)
        {
            rune_ = false;
            Q_EMIT runeKeyChanged(false);
        }
    }

    bool HoldKeyFilter::eventFilter(QObject* watched, QEvent* event)
    {
        Q_UNUSED(watched);
        const QEvent::Type type = event->type();
        if (type == QEvent::ApplicationStateChange && QGuiApplication::applicationState() != Qt::ApplicationActive)
        {
            releaseAll();
            return false;
        }
        if (type == QEvent::WindowDeactivate)
        {
            releaseAll();
            return false;
        }
        if (type != QEvent::KeyPress && type != QEvent::KeyRelease)
            return false;
        auto* key = static_cast<QKeyEvent*>(event);
        if (InputMethodGuard::takesText(QGuiApplication::focusObject()))
            return false;
        const bool down = type == QEvent::KeyPress;
        const auto flip = [&](bool& state, void (HoldKeyFilter::*signal)(bool))
        {
            if (!key->isAutoRepeat() && state != down)
            {
                state = down;
                Q_EMIT(this->*signal)(down);
            }
        };
        switch (key->key())
        {
        case Qt::Key_Tab:
        case Qt::Key_Backtab:
            flip(tab_, &HoldKeyFilter::tabChanged);
            return true;
        case Qt::Key_QuoteLeft:
        case Qt::Key_AsciiTilde:
            flip(tilde_, &HoldKeyFilter::tildeChanged);
            return true;
        case Qt::Key_F:
            flip(rune_, &HoldKeyFilter::runeKeyChanged);
            return false;
        default:
            return false;
        }
    }

} // namespace rm_terminal
