#include "battle_analysis_pane.h"

#include "theme.h"

#include <QHash>
#include <QLinearGradient>
#include <QPainter>
#include <QPaintEvent>
#include <QPixmap>
#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>

namespace rm_terminal
{

    namespace
    {

        // 按高度分档,每一档都整块画得下才用,不裁半截(四周各留 kPadX / kPadY)。
        // 完整:控制度 60、四个阵地 52、堡垒一行 34、统计 70,间隔至少 8。
        constexpr int kFullArea = 60 + 52 + 34 + 70 + 3 * 8;
        // 中等(3 格高,1280 宽的小窗口也够):控制度一行 32、五个色块(四阵地 + 堡垒)40、统计一行 42,间隔至少 8。
        constexpr int kMediumArea = 32 + 40 + 42 + 2 * 8;
        // 紧凑(2 格高):控制度一行 32、五个色块 40,上下留出空;统计挪到卡片标题栏的小字里。
        constexpr int kCompactArea = 32 + 40 + 10;
        // 迷你(1280 × 760 小窗口里的 2 格高):控制度一行 24、五个色块 34。
        constexpr int kMiniArea = 24 + 34 + 4;
        constexpr qreal kPadX = 6;
        constexpr qreal kPadY = 4;
        constexpr int kFullHeight = kFullArea + 2 * 4;

        QColor with_alpha(QColor colour, qreal alpha)
        {
            colour.setAlphaF(alpha);
            return colour;
        }

        QColor side_text(bool blue) { return blue ? theme::kBlueText : theme::kRedText; }
        QColor side_fill(bool blue) { return blue ? theme::kFillBlue : theme::kFillRed; }

        // 素材是白色前景 + 透明背景,用 SourceIn 整体染成标签色,视觉重量才和文字一致。
        // 按「路径 + 颜色 + 尺寸 + DPR」缓存:paintEvent 每帧都会走到这里。
        QPixmap tinted_icon(const QString& path, const QColor& colour, qreal dpr, int size)
        {
            static QHash<QString, QPixmap> cache;
            const QString key = QStringLiteral("%1|%2|%3|%4").arg(path, colour.name(QColor::HexArgb)).arg(size).arg(dpr);
            const auto hit = cache.constFind(key);
            if (hit != cache.constEnd())
                return *hit;
            QPixmap source(path);
            if (source.isNull())
                return {};
            const int px = static_cast<int>(std::lround(size * dpr));
            QPixmap fitted = source.scaled(px, px, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            QPixmap out(fitted.size());
            out.setDevicePixelRatio(dpr);
            out.fill(Qt::transparent);
            QPainter p(&out);
            p.drawPixmap(0, 0, fitted);
            p.setCompositionMode(QPainter::CompositionMode_SourceIn);
            p.fillRect(out.rect(), colour);
            p.end();
            cache.insert(key, out);
            return out;
        }

        QString span(const QString& text, const QColor& colour, bool number)
        {
            if (!number)
                return QStringLiteral("<span style='color:%1'>%2</span>").arg(colour.name(), text.toHtmlEscaped());
            return QStringLiteral("<span style='font-family:\"%1\"; font-weight:600; font-size:14px; color:%2'>%3</span>")
                .arg(theme::numericFamily(), colour.name(), text.toHtmlEscaped());
        }

        QString value_text(std::optional<std::int64_t> value) { return value ? QString::number(*value) : QStringLiteral("--"); }

        // 阵地色块的样子:完全控制是整块阵营色渐变,临时控制是斜纹,左边一道竖条(临时是虚线)。
        struct ZoneLook
        {
            QColor bar;
            QColor text;
            QColor strong;
            QColor weak;
            bool stripes = false;
            bool tinted = false;
        };

        ZoneLook zone_look(ZoneControl control)
        {
            ZoneLook look{theme::kTextMuted, theme::kTextBright, theme::kTile, theme::kTile};
            const auto faction = [&look](bool blue, bool full)
            {
                look.bar = blue ? theme::kBlue : theme::kRed;
                look.text = side_text(blue);
                look.tinted = true;
                look.stripes = !full;
                const QColor base = blue ? theme::kBlue : theme::kRed;
                look.strong = with_alpha(base, full ? (blue ? 0.28 : 0.26) : 0.17);
                look.weak = with_alpha(base, full ? 0.08 : 0.05);
            };
            switch (control)
            {
            case ZoneControl::RedFull: faction(false, true); break;
            case ZoneControl::RedTemporary: faction(false, false); break;
            case ZoneControl::BlueFull: faction(true, true); break;
            case ZoneControl::BlueTemporary: faction(true, false); break;
            case ZoneControl::Neutral:
            case ZoneControl::Unknown: break;
            }
            return look;
        }

        // 窄色块里的短说法:「红方前哨区」→「红前哨」,「蓝方完全」→「蓝完全」,「无人激活」→「无人」。
        QString short_zone_text(QString text)
        {
            text.remove(QStringLiteral("区"));
            text.replace(QStringLiteral("红方"), QStringLiteral("红"));
            text.replace(QStringLiteral("蓝方"), QStringLiteral("蓝"));
            text.replace(QStringLiteral("我方在激活"), QStringLiteral("激活中"));
            text.replace(QStringLiteral("无人激活"), QStringLiteral("无人"));
            text.remove(QLatin1Char(' '));
            return text;
        }

        QString short_stat_label(QString text)
        {
            text.remove(QStringLiteral("我方"));
            text.remove(QStringLiteral("总"));
            text.replace(QStringLiteral("能量机关"), QStringLiteral("机关"));
            return text;
        }

        void paint_tile(QPainter& painter, const QRectF& tile, const ZoneLook& look, const QString& name,
                        const QString& status, const QString& note)
        {
            painter.save();
            painter.setClipPath(theme::chamferPath(tile, 6));
            if (!look.tinted)
            {
                painter.fillRect(tile, look.strong);
            }
            else if (look.stripes)
            {
                // repeating-linear-gradient(-45deg, strong 0 5px, weak 5px 10px)
                QLinearGradient stripes(tile.topLeft(), tile.topLeft() + QPointF(-7.071, -7.071));
                stripes.setSpread(QGradient::RepeatSpread);
                stripes.setColorAt(0, look.strong);
                stripes.setColorAt(0.4999, look.strong);
                stripes.setColorAt(0.5, look.weak);
                stripes.setColorAt(1, look.weak);
                painter.fillRect(tile, stripes);
            }
            else
            {
                QLinearGradient fade(tile.topLeft(), tile.topRight());
                fade.setColorAt(0, look.strong);
                fade.setColorAt(1, look.weak);
                painter.fillRect(tile, fade);
            }
            if (look.stripes)
            {
                for (qreal y = tile.top(); y < tile.bottom(); y += 7)
                    painter.fillRect(QRectF(tile.left(), y, 3, std::min<qreal>(4, tile.bottom() - y)), look.bar);
            }
            else
            {
                painter.fillRect(QRectF(tile.left(), tile.top(), 3, tile.height()), look.bar);
            }
            // 字跟着色块的高度变,上下居中;色块窄(面板只有 4 格宽)时名字和状态用短的说法。
            // 上限放到 kMaxPaneScale:色块变高字就跟着变大,不再卡在 1 倍让色块里空一大半。
            const qreal f = std::clamp<qreal>(tile.height() / 52.0, 0.7, theme::kMaxPaneScale);
            const int name_px = std::max(theme::kMinLabelPx, static_cast<int>(std::lround(12.5 * f)));
            const int status_px = std::max(13, static_cast<int>(std::lround(16 * f)));
            const bool narrow = tile.width() < 96;
            const qreal pad = narrow ? 9 : 15;
            const qreal name_h = name_px + 3;
            const qreal status_h = status_px + 3;
            const qreal gap = 1 + 3 * f;
            const qreal top = tile.top() + std::max<qreal>(1, (tile.height() - name_h - gap - status_h) / 2);
            const QString shown_name = narrow ? short_zone_text(name) : name;
            const QString shown_status = narrow ? short_zone_text(status) : status;
            const qreal text_w = tile.width() - pad - 5;
            // 放不下先缩小字(最小 10 / 11 像素),还放不下再省略。
            const int fit_name_px = theme::fitFontPx(shown_name, name_px, 10, text_w,
                                                     [](int size)
                                                     { return theme::labelPx(size); });
            painter.setFont(theme::labelPx(fit_name_px));
            painter.setPen(theme::kTextSecondary);
            const QRectF name_box(tile.left() + pad, top, text_w, name_h);
            painter.drawText(name_box, Qt::AlignLeft | Qt::AlignVCenter,
                             painter.fontMetrics().elidedText(shown_name, Qt::ElideRight, static_cast<int>(name_box.width())));
            // 「红推 4s」靠右写在名字那一行。名字已经占到右边时就不写:两段字叠在一起比少一个
            // 推进秒数难读得多,秒数在阵地色块的颜色和状态里也能看出来。
            if (!note.isEmpty() && !narrow)
            {
                const QFont note_font = theme::numericPx(std::max(theme::kMinNumberPx, static_cast<int>(std::lround(12 * f))),
                                                         QFont::DemiBold);
                const qreal note_w = QFontMetricsF(note_font).horizontalAdvance(note);
                const qreal name_w = QFontMetricsF(theme::labelPx(fit_name_px)).horizontalAdvance(shown_name);
                if (name_w + 8 + note_w <= name_box.width())
                {
                    painter.setFont(note_font);
                    painter.setPen(theme::kTextBright);
                    painter.drawText(name_box.adjusted(0, 0, -4, 0), Qt::AlignRight | Qt::AlignVCenter, note);
                }
            }
            const auto status_font_of = [narrow](int size)
            {
                QFont font = theme::labelPx(size, QFont::Bold);
                font.setLetterSpacing(QFont::AbsoluteSpacing, narrow ? 0 : 1);
                return font;
            };
            const int fit_status_px = theme::fitFontPx(shown_status, status_px, theme::kMinLabelPx, text_w, status_font_of);
            const QFont status_font = status_font_of(fit_status_px);
            painter.setFont(status_font);
            painter.setPen(look.text);
            const QRectF status_box(tile.left() + pad, top + name_h + gap, tile.width() - pad - 5, status_h);
            painter.drawText(status_box, Qt::AlignLeft | Qt::AlignVCenter,
                             painter.fontMetrics().elidedText(shown_status, Qt::ElideRight, static_cast<int>(status_box.width())));
            painter.restore();
        }

        // 控制度对拉条:红从左、蓝从右,按各自占比伸出,中间留 4 像素缝;正中一道白线是一半的位置。
        // 整条按 CSS skewX(-24deg) 斜切。两边都是 0 或缺数据时只画底槽和中线。
        void paint_tug(QPainter& painter, const QRectF& bar, std::optional<std::uint32_t> red,
                       std::optional<std::uint32_t> blue)
        {
            painter.save();
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(255, 255, 255, 18));
            painter.drawPath(theme::skewBar(bar, -24));
            const double total = red && blue ? static_cast<double>(*red) + *blue : 0.0;
            if (total > 0)
            {
                const auto side = [&](bool blue_side, double share)
                {
                    const qreal width = std::max<qreal>(0, bar.width() * share - 2);
                    if (width <= 0)
                        return;
                    const QRectF part(blue_side ? bar.right() - width : bar.left(), bar.top(), width, bar.height());
                    const QColor base = side_fill(blue_side);
                    for (const auto& [grow, a] : {std::pair{4.0, 0.10}, std::pair{2.0, 0.18}})
                    {
                        painter.setBrush(with_alpha(base, a));
                        painter.drawPath(theme::skewBar(part.adjusted(-grow, -grow / 2, grow, grow / 2), -24));
                    }
                    QLinearGradient fill(blue_side ? part.topRight() : part.topLeft(), blue_side ? part.topLeft() : part.topRight());
                    fill.setColorAt(0, with_alpha(base, 0.55));
                    fill.setColorAt(1, with_alpha(base, 0.95));
                    painter.setBrush(fill);
                    painter.drawPath(theme::skewBar(part, -24));
                };
                side(false, *red / total);
                side(true, *blue / total);
            }
            painter.setBrush(QColor(255, 255, 255, 178));
            painter.drawPath(theme::skewBar(QRectF(bar.center().x() - 1, bar.top() - 5, 2, bar.height() + 10), -24));
            painter.restore();
        }

        void paint_coin(QPainter& painter, const QRectF& box, const QColor& colour)
        {
            painter.save();
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setPen(QPen(colour, 1.5));
            painter.setBrush(Qt::NoBrush);
            const QPointF c = box.center();
            painter.drawEllipse(c, box.width() * 0.4, box.height() * 0.4);
            painter.drawLine(QPointF(c.x() - 2.4, c.y() - 1.4), QPointF(c.x() + 2.4, c.y() - 1.4));
            painter.drawLine(QPointF(c.x() - 2.4, c.y() + 1.4), QPointF(c.x() + 2.4, c.y() + 1.4));
            painter.drawLine(QPointF(c.x(), c.y() - 3.4), QPointF(c.x(), c.y() + 3.4));
            painter.restore();
        }

    } // namespace

    BattleAnalysisPane::BattleAnalysisPane(QWidget* parent) : QWidget(parent) {}

    void BattleAnalysisPane::setMetrics(const std::vector<AnalysisMetric>& metrics)
    {
        metrics_ = metrics;
        update();
    }

    void BattleAnalysisPane::setFortress(const FortressStatus& status)
    {
        fortress_ = status;
        update();
    }

    void BattleAnalysisPane::setZoneBoard(const ZoneBoard& board)
    {
        board_ = board;
        update();
    }

    void BattleAnalysisPane::setOwnFaction(std::uint32_t faction)
    {
        own_faction_ = faction;
        update();
    }

    // 统计不在面板里画的那几档,统计写到卡片标题栏右边的小字里。
    bool BattleAnalysisPane::compact() const
    {
        const Level l = level();
        return !control_only_ && l != Level::Full && l != Level::Medium;
    }

    void BattleAnalysisPane::setControlOnly(bool control_only)
    {
        control_only_ = control_only;
        update();
    }

    void BattleAnalysisPane::paintBigControl(QPainter& painter, const QRectF& area) const
    {
        const std::optional<std::uint32_t> red = board_.red_control_points;
        const std::optional<std::uint32_t> blue = board_.blue_control_points;
        const bool own_blue = own_faction_ == 2;
        QString lead = QStringLiteral("等待数据");
        QColor lead_colour = theme::kTextBright;
        if (red && blue)
        {
            const long ours = static_cast<long>(own_blue ? *blue : *red);
            const long theirs = static_cast<long>(own_blue ? *red : *blue);
            if (ours == theirs)
                lead = QStringLiteral("双方持平");
            else
            {
                lead = QStringLiteral("%1 %2 分").arg(ours > theirs ? QStringLiteral("我方领先") : QStringLiteral("敌方领先")).arg(std::labs(ours - theirs));
                lead_colour = ours > theirs ? side_text(own_blue) : side_text(!own_blue);
            }
        }
        const int number_px = static_cast<int>(std::clamp(area.height() * 0.5, 32.0, 84.0));
        const qreal number_w = QFontMetricsF(theme::numericPx(number_px, QFont::Bold)).horizontalAdvance(QStringLiteral("888"));
        // 上面一行:红方 / 谁领先 / 蓝方(效果图 13)。
        painter.setFont(theme::labelPx(14));
        painter.setPen(theme::kTextSecondary);
        painter.drawText(QRectF(area.left(), area.top(), area.width(), 20), Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("红方"));
        painter.drawText(QRectF(area.left(), area.top(), area.width(), 20), Qt::AlignRight | Qt::AlignVCenter,
                         QStringLiteral("蓝方"));
        painter.setFont(theme::labelPx(15, QFont::Bold));
        painter.setPen(lead_colour);
        painter.drawText(QRectF(area.left(), area.top(), area.width(), 20), Qt::AlignHCenter | Qt::AlignVCenter, lead);
        const QRectF main(area.left(), area.top() + 24, area.width(), area.height() - 24);
        const auto value = [&](const QRectF& box, std::optional<std::uint32_t> v, bool blue_side)
        {
            painter.setFont(theme::numericPx(number_px, QFont::Bold));
            painter.setPen(v ? side_text(blue_side) : theme::kTextMuted);
            painter.drawText(box, (blue_side ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter,
                             v ? QString::number(*v) : QStringLiteral("--"));
        };
        value(QRectF(main.left(), main.top(), number_w, main.height()), red, false);
        value(QRectF(main.right() - number_w, main.top(), number_w, main.height()), blue, true);
        const qreal bar_left = main.left() + number_w + 12;
        const qreal bar_right = main.right() - number_w - 12;
        if (bar_right - bar_left > 24)
            paint_tug(painter, QRectF(bar_left, main.center().y() - 7, bar_right - bar_left, 14), red, blue);
    }

    QSize BattleAnalysisPane::sizeHint() const { return QSize(460, kFullHeight); }

    QSize BattleAnalysisPane::minimumSizeHint() const { return QSize(240, kMiniArea + 2 * 4); }

    QString BattleAnalysisPane::headerText() const
    {
        if (control_only_)
            return {};
        const auto ratio = [](const std::optional<double>& v)
        { return v ? QString::number(*v, 'f', 2) : QStringLiteral("--"); };
        const QString dot = QStringLiteral("&nbsp;<span style='color:%1'>·</span>&nbsp;").arg(theme::kTextMuted.name());
        const auto pair = [&](const QString& red, const QString& blue)
        {
            return span(red, theme::kRedText, true) + span(QStringLiteral(":"), theme::kTextSecondary, false) +
                   span(blue, theme::kBlueText, true);
        };
        const QString logistics =
            QStringLiteral("后勤&nbsp;") + (compact() ? pair(ratio(board_.red_logistics), ratio(board_.blue_logistics))
                                                      : span(ratio(board_.red_logistics), theme::kRedText, true) + dot +
                                                            span(ratio(board_.blue_logistics), theme::kBlueText, true));
        if (!compact())
            return logistics;
        const bool blue = own_faction_ == 2;
        const auto metric_pair = [&](std::size_t index)
        {
            if (index >= metrics_.size())
                return pair(QStringLiteral("--"), QStringLiteral("--"));
            const AnalysisMetric& m = metrics_[index];
            return blue ? pair(value_text(m.enemy_value), value_text(m.self_value))
                        : pair(value_text(m.self_value), value_text(m.enemy_value));
        };
        // 有雷达情报时经济也是红:蓝两个数,和伤害、击杀一样。
        const QString economy = metrics_.empty()          ? span(QStringLiteral("--"), theme::kTextBright, true)
                                : metrics_[0].enemy_value ? metric_pair(0)
                                                          : span(value_text(metrics_[0].self_value), theme::kTextBright, true);
        return QStringLiteral("经济&nbsp;") + economy + dot + QStringLiteral("伤害&nbsp;") +
               metric_pair(1) + dot + QStringLiteral("击杀&nbsp;") + metric_pair(2) + dot + logistics;
    }

    void BattleAnalysisPane::paintControl(QPainter& painter, const QRectF& row, bool compact) const
    {
        const std::optional<std::uint32_t> red = board_.red_control_points;
        const std::optional<std::uint32_t> blue = board_.blue_control_points;
        const bool own_blue = own_faction_ == 2;
        QString lead_label = QStringLiteral("等待数据");
        QString lead_number;
        QColor lead_colour = theme::kTextBright;
        if (red && blue)
        {
            const long ours = static_cast<long>(own_blue ? *blue : *red);
            const long theirs = static_cast<long>(own_blue ? *red : *blue);
            if (ours == theirs)
            {
                lead_label = QStringLiteral("双方持平");
            }
            else
            {
                lead_label = ours > theirs ? QStringLiteral("我方领先") : QStringLiteral("敌方领先");
                lead_number = QString::number(std::labs(ours - theirs));
                lead_colour = side_text(ours > theirs ? own_blue : !own_blue);
            }
        }
        const bool mini = compact && row.height() < 30;
        const int label_px = mini ? 12 : 13;
        const auto paint_lead = [&](const QRectF& box, Qt::Alignment align, int number_px)
        {
            const QFont label = theme::labelPx(label_px);
            const QFont number = theme::numericPx(number_px, QFont::Bold);
            const qreal width = theme::mixedTextWidth(lead_label, label, label) +
                                (lead_number.isEmpty() ? 0 : QFontMetricsF(number).horizontalAdvance(lead_number) + 6 + QFontMetricsF(label).horizontalAdvance(QStringLiteral("分")));
            qreal x = align & Qt::AlignRight ? box.right() - width : box.left();
            x += theme::paintMixedText(painter, QRectF(x, box.top(), width, box.height()), Qt::AlignLeft | Qt::AlignVCenter,
                                       lead_label, label, label, theme::kTextBright);
            if (lead_number.isEmpty())
                return width;
            painter.setFont(number);
            painter.setPen(lead_colour);
            const QFontMetricsF metrics(number);
            painter.drawText(QRectF(x + 3, box.top(), metrics.horizontalAdvance(lead_number), box.height()),
                             Qt::AlignLeft | Qt::AlignVCenter, lead_number);
            x += metrics.horizontalAdvance(lead_number) + 6;
            painter.setFont(label);
            painter.setPen(theme::kTextBright);
            painter.drawText(QRectF(x, box.top(), 20, box.height()), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("分"));
            return width;
        };
        const auto value = [&](const QRectF& box, std::optional<std::uint32_t> v, bool blue_side, int px)
        {
            painter.setFont(theme::numericPx(px, QFont::Bold));
            painter.setPen(v ? side_text(blue_side) : theme::kTextMuted);
            painter.drawText(box, (blue_side ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter,
                             v ? QString::number(*v) : QStringLiteral("--"));
        };

        if (!compact)
        {
            // 完整档:一行小标题 + 一行大数字和对拉条。两行按 16:38 分这一块的高度,
            // 字号和条的粗细都跟着走 —— 块变高就一起变大,不在块里留空带。
            const qreal k = std::min(row.height() / 60.0, theme::kMaxPaneScale);
            const qreal label_h = row.height() * 16.0 / 60.0;
            const qreal main_h = row.height() - label_h - 6 * k;
            const QRectF top(row.left(), row.top(), row.width(), label_h);
            painter.setFont(theme::labelPx(std::max(theme::kMinLabelPx, static_cast<int>(std::lround(13 * k)))));
            painter.setPen(theme::kTextSecondary);
            painter.drawText(top, Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("控制度"));
            paint_lead(top, Qt::AlignRight, std::max(theme::kMinNumberPx, static_cast<int>(std::lround(18 * k))));
            const QRectF main(row.left(), top.bottom() + 6 * k, row.width(), main_h);
            // 大数字按块高来,但两侧数字加中间的条必须排得进这一行的宽度:宽度不够就缩字号,
            // 不让数字压到条上。三位数「888」是最宽的情况,按它量。
            const qreal number_room = row.width() * 0.26;
            const int number_px = theme::fitFontPx(QStringLiteral("888"),
                                                   std::max(theme::kMinNumberPx, static_cast<int>(std::lround(main_h * 0.9))),
                                                   theme::kMinNumberPx, number_room,
                                                   [](int size)
                                                   { return theme::numericPx(size, QFont::Bold); });
            const qreal number_w = QFontMetricsF(theme::numericPx(number_px, QFont::Bold))
                                       .horizontalAdvance(QStringLiteral("888")) +
                                   4;
            value(QRectF(main.left(), main.top(), number_w, main.height()), red, false, number_px);
            value(QRectF(main.right() - number_w, main.top(), number_w, main.height()), blue, true, number_px);
            const qreal bar_h = std::clamp<qreal>(main_h * 0.3, 10, 26);
            const qreal bar_left = main.left() + number_w + 8 * k;
            const qreal bar_right = main.right() - number_w - 8 * k;
            if (bar_right - bar_left > 24)
                paint_tug(painter, QRectF(bar_left, main.center().y() - bar_h / 2, bar_right - bar_left, bar_h), red, blue);
            return;
        }
        // 一行式的控制度:字号跟着这一行的高度走(设计 32、迷你档 24),行拖高字就变大,
        // 不会在行里上下各空一截。
        const qreal rk = std::min(row.height() / (mini ? 24.0 : 32.0), theme::kMaxPaneScale);
        const int row_label_px = std::max(theme::kMinLabelPx, static_cast<int>(std::lround(label_px * rk)));
        painter.setFont(theme::labelPx(row_label_px));
        painter.setPen(theme::kTextSecondary);
        const qreal label_w = QFontMetricsF(painter.font()).horizontalAdvance(QStringLiteral("控制度"));
        painter.drawText(QRectF(row.left(), row.top(), label_w, row.height()), Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("控制度"));
        const QFont lead_font = theme::labelPx(row_label_px);
        const qreal lead_w = theme::mixedTextWidth(lead_label, lead_font, lead_font) +
                             (lead_number.isEmpty() ? 0 : 40 * rk);
        const qreal lead_left = row.right() - lead_w;
        paint_lead(QRectF(lead_left, row.top(), lead_w, row.height()), Qt::AlignRight,
                   std::max(theme::kMinNumberPx, static_cast<int>(std::lround((mini ? 15 : 17) * rk))));
        const qreal red_left = row.left() + label_w + (mini ? 8 : 12);
        const qreal blue_right = lead_left - (mini ? 8 : 12);
        // 两个数字要并排放进「标题」和「领先几分」之间,中间还得留出对拉条的位置。
        // 所以数字的字号同时被行高和这段宽度管着:行拖高字变大,但绝不让红蓝两个数字撞在一起。
        const qreal number_room = std::max<qreal>(1, (blue_right - red_left) / 2 - 12);
        const int number_px = theme::fitFontPx(
            QStringLiteral("888"), std::max(theme::kMinNumberPx, static_cast<int>(std::lround((mini ? 22 : 28) * rk))),
            theme::kMinNumberPx, number_room, [](int size)
            { return theme::numericPx(size, QFont::Bold); });
        const qreal value_w = QFontMetricsF(theme::numericPx(number_px, QFont::Bold)).horizontalAdvance(QStringLiteral("888"));
        value(QRectF(red_left, row.top(), value_w, row.height()), red, false, number_px);
        value(QRectF(blue_right - value_w, row.top(), value_w, row.height()), blue, true, number_px);
        const qreal bar_left = red_left + value_w + 10;
        const qreal bar_right = blue_right - value_w - 10;
        const qreal bar_h = std::clamp<qreal>((mini ? 10 : 12) * rk, 10, 26);
        if (bar_right - bar_left > 24)
            paint_tug(painter, QRectF(bar_left, row.center().y() - bar_h / 2, bar_right - bar_left, bar_h), red, blue);
    }

    void BattleAnalysisPane::paintZones(QPainter& painter, const QRectF& area, bool with_fortress) const
    {
        const int count = with_fortress ? 5 : 4;
        const qreal gap = 8;
        const qreal width = (area.width() - gap * (count - 1)) / count;
        for (int i = 0; i < 4; ++i)
        {
            const ZoneStatus& zone = board_.zones[static_cast<std::size_t>(i)];
            QString note;
            if (zone.progress && *zone.progress != 0)
                note = QStringLiteral("%1 %2s").arg(*zone.progress > 0 ? QStringLiteral("红推") : QStringLiteral("蓝推")).arg(std::abs(*zone.progress));
            paint_tile(painter, QRectF(area.left() + i * (width + gap), area.top(), width, area.height()),
                       zone_look(zone.control), zone.name.isEmpty() ? QStringLiteral("--") : zone.name,
                       zone_control_text(zone.control), note);
        }
        if (!with_fortress)
            return;
        ZoneLook look = zone_look(ZoneControl::Neutral);
        QString status = QStringLiteral("--");
        if (fortress_.ours_by_enemy || fortress_.theirs_by_us)
            status = QStringLiteral("无人激活");
        if (fortress_.theirs_by_us && *fortress_.theirs_by_us > 0)
        {
            status = QStringLiteral("我方在激活 %1s").arg(*fortress_.theirs_by_us);
            look.text = theme::kOkText;
        }
        if (fortress_.ours_by_enemy && *fortress_.ours_by_enemy > 0)
        {
            status = QStringLiteral("被偷 %1s/10s").arg(*fortress_.ours_by_enemy);
            look.text = theme::kDangerText;
            look.bar = theme::kDanger;
        }
        paint_tile(painter, QRectF(area.left() + 4 * (width + gap), area.top(), width, area.height()), look,
                   QStringLiteral("堡垒"), status, QString());
    }

    void BattleAnalysisPane::paintFortress(QPainter& painter, const QRectF& row) const
    {
        theme::paintGlass(painter, row, 6, false, theme::kTile, Qt::transparent);
        // 这一行里的牌子和字都按行高缩放(设计行高 34),行变高字就变大。
        const qreal k = std::min(row.height() / 34.0, theme::kMaxPaneScale);
        const qreal chip_h = std::min(row.height() - 4, 20 * k);
        const QRectF chip(row.left() + 12 * k, row.center().y() - chip_h / 2, 38 * k, chip_h);
        theme::paintGlass(painter, chip, 4, false, theme::kAccentBg, Qt::transparent);
        QFont chip_font = theme::labelPx(std::max(theme::kMinLabelPx, static_cast<int>(std::lround(12 * k))), QFont::Bold);
        chip_font.setLetterSpacing(QFont::AbsoluteSpacing, 1);
        painter.setFont(chip_font);
        painter.setPen(theme::kAccentText);
        painter.drawText(chip, Qt::AlignCenter, QStringLiteral("堡垒"));
        QColor ink = theme::kTextPrimary;
        switch (fortress_tone(fortress_))
        {
        case FortressTone::Danger: ink = theme::kDangerText; break;
        case FortressTone::Good: ink = theme::kOkText; break;
        case FortressTone::Unknown: ink = theme::kTextMuted; break;
        case FortressTone::Quiet: break;
        }
        const QRectF text(chip.right() + 10 * k, row.top(), row.right() - chip.right() - 22 * k, row.height());
        const int text_px = theme::fitFontPx(fortress_status_text(fortress_), static_cast<int>(std::lround(14 * k)),
                                             theme::kMinLabelPx, text.width(),
                                             [](int size)
                                             { return theme::labelPx(size); });
        painter.setFont(theme::labelPx(text_px));
        painter.setPen(ink);
        painter.drawText(text, Qt::AlignLeft | Qt::AlignVCenter,
                         painter.fontMetrics().elidedText(fortress_status_text(fortress_), Qt::ElideRight,
                                                          static_cast<int>(text.width())));
    }

    void BattleAnalysisPane::paintStats(QPainter& painter, const QRectF& row, bool compact) const
    {
        static const QString icons[4] = {QString(), QStringLiteral(":/images/message/validate_icon_totalhurt.png"),
                                         QStringLiteral(":/images/message/validate_icon_killcount.png"),
                                         QStringLiteral(":/images/message/validate_icon_rune.png")};
        const qreal width = row.width() / 4;
        const bool own_blue = own_faction_ == 2;
        // 四格统计按自己这一块的高度缩放(完整档设计高 70、中等档 42),块变高字和图标就变大。
        const qreal k = std::min(row.height() / (compact ? 42.0 : 70.0), theme::kMaxPaneScale);
        // 图标最多长到 24:再大它就比标签还抢眼,还要从标签那里抢走宽度。
        const qreal icon = std::min<qreal>(24, std::lround(15 * k));
        for (int i = 0; i < 4; ++i)
        {
            const QRectF cell(row.left() + i * width, row.top(), width, row.height());
            if (i > 0)
                painter.fillRect(QRectF(cell.left(), cell.top(), 1, cell.height()), theme::kRowBorder);
            const bool tight = cell.width() < 120;
            const qreal left = cell.left() + (i == 0 ? 2 : tight ? 8
                                                                 : 14);
            const qreal right = cell.right() - (tight ? 6 : 14);
            if (static_cast<std::size_t>(i) >= metrics_.size())
                continue;
            const AnalysisMetric& metric = metrics_[static_cast<std::size_t>(i)];

            const QRectF icon_box(left, cell.top() + 2 * k, icon, icon);
            if (i == 0)
            {
                paint_coin(painter, icon_box, theme::kTextSecondary);
            }
            else
            {
                const QPixmap tinted = tinted_icon(icons[i], theme::kTextSecondary, devicePixelRatioF(),
                                                   static_cast<int>(icon));
                if (!tinted.isNull())
                    painter.drawPixmap(icon_box.topLeft(), tinted);
            }
            const int wanted_label_px = std::max(theme::kMinLabelPx,
                                                 static_cast<int>(std::lround((compact ? 12 : 13) * k)));
            const qreal label_h = std::max<qreal>(icon, wanted_label_px + 3);
            const QRectF label_box(left + icon + 6 * k, cell.top() + 2 * k, right - left - icon - 6 * k, label_h);

            const bool text_value = !metric.text_value.isEmpty();
            const bool has_value = text_value || metric.self_value.has_value();
            const QString shown = text_value ? metric.text_value : value_text(metric.self_value);
            // 字放不下这一格就缩小,不切掉半个字。
            int value_px = std::max(theme::kMinNumberPx,
                                    static_cast<int>(std::lround((text_value ? (compact ? 16 : 21) : (compact ? 22 : 30)) * k)));
            // 数值紧跟标签往下排,高度按字号给,不摊开整格 —— 摊开后数字会被居中到很低的地方,
            // 标签和数字之间裂出一条空带。格子更高时是字号变大把空间吃掉,不是把两行拉开。
            const qreal value_top = label_box.bottom() + 2 * k;
            const qreal value_bottom = cell.bottom() - (metric.has_enemy ? 8 * k : 2 * k);
            const qreal value_room = std::max<qreal>(1, value_bottom - value_top);
            const QRectF value_box(left, value_top, right - left, std::min<qreal>(value_room, value_px * 1.4));
            const auto make_font = [&](int px)
            {
                QFont font = text_value ? theme::labelPx(px, QFont::Bold) : theme::numericPx(px, QFont::DemiBold);
                if (text_value)
                    font.setLetterSpacing(QFont::AbsoluteSpacing, 1);
                return font;
            };
            QFont value_font = make_font(value_px);
            while (value_px > theme::kMinNumberPx && QFontMetricsF(value_font).horizontalAdvance(shown) > value_box.width())
                value_font = make_font(--value_px);
            const qreal value_width = QFontMetricsF(value_font).horizontalAdvance(shown);

            QString aside_label;
            QString aside_number;
            if (metric.has_enemy)
            {
                aside_label = QStringLiteral("敌");
                aside_number = value_text(metric.enemy_value);
            }
            else if (!metric.note.isEmpty())
            {
                const int space = metric.note.indexOf(QLatin1Char(' '));
                aside_label = space > 0 ? metric.note.left(space) : metric.note;
                aside_number = space > 0 ? metric.note.mid(space + 1) : QString();
            }
            const QFont aside_label_font = theme::labelPx(
                std::max(theme::kMinLabelPx, static_cast<int>(std::lround((compact ? 12 : 13) * k))));
            const QFont aside_number_font = theme::numericPx(
                std::max(theme::kMinNumberPx, static_cast<int>(std::lround((compact ? 13 : 15) * k))), QFont::DemiBold);
            const qreal aside_width = aside_label.isEmpty()
                                          ? 0
                                          : QFontMetricsF(aside_label_font).horizontalAdvance(aside_label) + 4 +
                                                QFontMetricsF(aside_number_font).horizontalAdvance(aside_number);

            // 格子窄时先用短的说法(「我方经济」→「经济」),再缩字号,最后才省略。
            QString label_text = metric.label;
            const auto label_fits = [&](const QString& text, qreal room)
            {
                return QFontMetricsF(theme::labelPx(wanted_label_px)).horizontalAdvance(text) <= room;
            };
            // 右边那一小段(「敌 0」「累计 800」「敌 约36s」)放数值右边,放不下就不画。note_first 的格子例外:
            // 依次试标签那一行(标签不够就用短说法)、数值下面一行,都不行就只留图标、把标签那一行让给它。
            enum class AsideAt
            {
                None,
                BesideValue,
                LabelRow,
                Below
            };
            AsideAt aside_at = AsideAt::None;
            if (!aside_label.isEmpty())
            {
                const qreal label_row_room = label_box.width() - 8 - aside_width;
                if (value_width + 8 + aside_width <= right - left)
                {
                    aside_at = AsideAt::BesideValue;
                }
                else if (!metric.note_first)
                {
                    aside_at = AsideAt::None;
                }
                else if (label_fits(label_text, label_row_room))
                {
                    aside_at = AsideAt::LabelRow;
                }
                else if (label_fits(short_stat_label(label_text), label_row_room))
                {
                    label_text = short_stat_label(label_text);
                    aside_at = AsideAt::LabelRow;
                }
                else if (aside_width <= right - left &&
                         value_box.bottom() + QFontMetricsF(aside_number_font).height() <= cell.bottom())
                {
                    aside_at = AsideAt::Below;
                }
                else if (aside_width <= label_box.width())
                {
                    label_text.clear();
                    aside_at = AsideAt::LabelRow;
                }
            }
            const qreal label_room = aside_at == AsideAt::LabelRow ? label_box.width() - 8 - aside_width : label_box.width();
            if (!label_text.isEmpty())
            {
                if (!label_fits(label_text, label_room))
                    label_text = short_stat_label(label_text);
                const int label_px = theme::fitFontPx(label_text, wanted_label_px, theme::kMinLabelPx, label_room,
                                                      [](int size)
                                                      { return theme::labelPx(size); });
                painter.setFont(theme::labelPx(label_px));
                painter.setPen(theme::kTextSecondary);
                painter.drawText(QRectF(label_box.left(), label_box.top(), label_room, label_box.height()),
                                 Qt::AlignLeft | Qt::AlignVCenter,
                                 painter.fontMetrics().elidedText(label_text, Qt::ElideRight, static_cast<int>(label_room)));
            }

            painter.setFont(value_font);
            painter.setPen(has_value ? theme::kTextPrimary : theme::kTextMuted);
            painter.drawText(value_box, Qt::AlignLeft | Qt::AlignVCenter, shown);

            if (aside_at != AsideAt::None)
            {
                QRectF aside;
                if (aside_at == AsideAt::BesideValue)
                {
                    const qreal inset = (compact ? 4 : 6) * k;
                    aside = QRectF(right - aside_width, value_box.top() + inset, aside_width,
                                   std::max<qreal>(1, value_box.height() - inset));
                }
                else if (aside_at == AsideAt::LabelRow)
                {
                    aside = QRectF(label_box.right() - aside_width, label_box.top(), aside_width, label_box.height());
                }
                else
                {
                    aside = QRectF(left, value_box.bottom(), aside_width, QFontMetricsF(aside_number_font).height());
                }
                painter.setFont(aside_label_font);
                painter.setPen(theme::kTextSecondary);
                painter.drawText(aside, Qt::AlignLeft | Qt::AlignVCenter, aside_label);
                painter.setFont(aside_number_font);
                painter.setPen(theme::kTextBright);
                painter.drawText(aside, Qt::AlignRight | Qt::AlignVCenter, aside_number);
            }

            if (metric.has_enemy && metric.self_value && metric.enemy_value)
            {
                const double total = static_cast<double>(*metric.self_value) + static_cast<double>(*metric.enemy_value);
                const qreal bar_h = std::clamp<qreal>(std::lround(3 * k), 3, 8);
                const QRectF bar(left, cell.bottom() - bar_h - (compact ? 1 : 3) * k, right - left, bar_h);
                painter.fillRect(bar, with_alpha(side_fill(!own_blue), 0.8));
                if (total > 0)
                    painter.fillRect(QRectF(bar.left(), bar.top(), bar.width() * (*metric.self_value / total), bar.height()),
                                     with_alpha(side_fill(own_blue), 0.9));
                else
                    painter.fillRect(bar, QColor(255, 255, 255, 30));
                painter.fillRect(QRectF(bar.center().x() - 0.5, bar.top() - 2, 1, bar.height() + 4),
                                 QColor(255, 255, 255, 128));
            }
        }
    }

    BattleAnalysisPane::Level BattleAnalysisPane::level() const
    {
        const qreal h = height() - 2 * kPadY;
        if (h >= kFullArea)
            return Level::Full;
        if (h >= kMediumArea)
            return Level::Medium;
        if (h >= kCompactArea)
            return Level::Compact;
        if (h >= kMiniArea)
            return Level::Mini;
        return Level::Tiny;
    }

    void BattleAnalysisPane::paintEvent(QPaintEvent*)
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRectF area = QRectF(rect()).adjusted(kPadX, kPadY, -kPadX, -kPadY);
        if (control_only_)
        {
            paintBigControl(painter, area);
            return;
        }
        // 按高度挑一档,整块都画得下的那档里最全的。height 现在只是分高度用的权重(效果图上
        // 那几块的高度比),不是写死的像素:面板多高,这几块就按这个比例铺满多高。
        struct Block
        {
            qreal height;
            std::function<void(const QRectF&)> draw;
            // 面板高得连放大到上限都用不完时,多出来的高度给谁。阵地色块是整块的颜色加居中的字,
            // 多高都好看;控制度、堡垒、统计都是「一行字」,拉太高只会在块里空一截。
            bool stretch = false;
        };
        std::vector<Block> blocks;
        qreal min_gap = 10;
        const auto control = [&](bool compact)
        { return [this, &painter, compact](const QRectF& r)
          { paintControl(painter, r, compact); }; };
        const auto zones = [&](bool with_fortress)
        {
            return [this, &painter, with_fortress](const QRectF& r)
            { paintZones(painter, r, with_fortress); };
        };
        const auto stats = [&](bool compact)
        { return [this, &painter, compact](const QRectF& r)
          { paintStats(painter, r, compact); }; };
        switch (level())
        {
        case Level::Full:
            min_gap = 8;
            blocks = {{60, control(false), false},
                      {52, zones(false), true},
                      {34, [&](const QRectF& r)
                       { paintFortress(painter, r); }, false},
                      {70, stats(false), false}};
            break;
        case Level::Medium:
            min_gap = 8;
            blocks = {{32, control(true), false}, {40, zones(true), true}, {42, stats(true), false}};
            break;
        case Level::Compact: blocks = {{32, control(true), false}, {40, zones(true), true}}; break;
        case Level::Mini:
            min_gap = 4;
            blocks = {{24, control(true), false}, {34, zones(true), true}};
            break;
        case Level::Tiny: blocks = {{area.height(), control(true), true}}; break;
        }
        // 每块按设计高度的占比分走全部可用高度,块里的字跟着块的高度变大变小。
        // 不再上下居中、也不再把余量当间隔留白:操作手把面板拖多高,这几块就铺满多高。
        qreal weights = 0;
        for (const Block& block : blocks)
            weights += block.height;
        const int gaps = static_cast<int>(blocks.size()) - 1;
        const qreal gap = std::min<qreal>(min_gap, gaps > 0 ? area.height() / (gaps * 4.0) : min_gap);
        const qreal usable = std::max<qreal>(1, area.height() - gap * gaps);
        // 先按权重分,再把「长到上限就用不动了」的那几块多出来的高度全部让给 stretch 的块。
        // 这样总高度一分不剩(最后一块的底边落在内容区底部),又不会有哪块被拉高后空一截。
        std::vector<qreal> heights(blocks.size());
        for (std::size_t i = 0; i < blocks.size(); ++i)
            heights[i] = usable * blocks[i].height / weights;
        qreal freed = 0;
        qreal stretch_weights = 0;
        for (std::size_t i = 0; i < blocks.size(); ++i)
        {
            const qreal cap = blocks[i].height * theme::kMaxPaneScale;
            if (blocks[i].stretch)
            {
                stretch_weights += blocks[i].height;
            }
            else if (heights[i] > cap)
            {
                freed += heights[i] - cap;
                heights[i] = cap;
            }
        }
        if (freed > 0 && stretch_weights > 0)
            for (std::size_t i = 0; i < blocks.size(); ++i)
                if (blocks[i].stretch)
                    heights[i] += freed * blocks[i].height / stretch_weights;
        qreal y = area.top();
        for (std::size_t i = 0; i < blocks.size(); ++i)
        {
            // 最后一块吃掉浮点的零头,底边正好落在内容区的底部。
            const qreal height = i + 1 == blocks.size() ? area.bottom() - y : std::floor(heights[i]);
            blocks[i].draw(QRectF(area.left(), y, area.width(), height));
            y += height + gap;
        }
    }

} // namespace rm_terminal
