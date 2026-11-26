#include "self_status_pane.h"

#include "presentation.h"
#include "theme.h"

#include <QPainter>
#include <QPaintEvent>
#include <algorithm>
#include <cmath>
#include <vector>

namespace rm_terminal
{

    namespace
    {

        // 按 330 × 190 设计的尺寸。面板拖大就整体放大、拖小就缩小(theme::kMaxPaneScale 倍为止)。
        constexpr int kHeaderHeight = 22;
        constexpr int kTileHeight = 52;
        constexpr int kGaugeHeight = 30;
        constexpr int kPipHeight = 22;
        constexpr double kDesignWidth = 330;
        // 四段从上到下:标题 / 三个数值格 / 三个仪表 / 四个状态灯。格子矮时从下往上少画几段,不压到别的面板。
        constexpr int kContentHeight = kHeaderHeight + 6 + kTileHeight + 8 + 3 * kGaugeHeight + 2 * 8 + 8 + kPipHeight;

        int px(double base, double scale) { return static_cast<int>(std::lround(base * scale)); }

        // 一段最高长到设计高度的多少:超过这个高度,这一段的内容已经撑不满了。
        int scaledPxMax(double design_height)
        {
            return static_cast<int>(std::lround(design_height * theme::kMaxPaneScale));
        }

        // 格子、仪表、状态灯都按「自己那个框的高度 ÷ 设计高度」缩放,所以框变高内容就变大,
        // 框变矮内容就变小,框里不会剩下一条空带。
        void paint_tile(QPainter& painter, const QRect& box, const QString& label, const QString& value, const QColor& ink,
                        double scale)
        {
            theme::paintGlass(painter, QRectF(box), 6, false, theme::kTile, Qt::transparent);
            const int pad = px(10, scale);
            const int text_w = box.width() - pad - px(4, scale);
            // 标签放不下先缩到 11 像素,还放不下才省略:格子窄时「允许发弹量」比字号重要。
            const int label_px = theme::fitFontPx(label, px(12, scale), theme::kMinLabelPx, text_w,
                                                  [](int size)
                                                  { return theme::labelPx(size); });
            const QFont label_font = theme::labelPx(label_px);
            painter.setFont(label_font);
            painter.setPen(theme::kTextSecondary);
            const int label_h = label_px + px(4, scale);
            const QRect label_box(box.left() + pad, box.top() + px(6, scale), text_w, label_h);
            painter.drawText(label_box, Qt::AlignLeft | Qt::AlignVCenter,
                             QFontMetrics(label_font).elidedText(label, Qt::ElideRight, label_box.width()));
            const int value_px = theme::fitFontPx(value, px(22, scale), theme::kMinNumberPx, text_w,
                                                  [](int size)
                                                  { return theme::numericPx(size, QFont::DemiBold); });
            painter.setFont(theme::numericPx(value_px, QFont::DemiBold));
            painter.setPen(ink);
            // 数字占掉标签以下的全部高度,格子变高时它在剩余空间里居中,底下不留空带。
            const int value_top = label_box.bottom() + px(2, scale);
            painter.drawText(QRect(box.left() + pad, value_top, text_w, std::max(1, box.bottom() - value_top - px(4, scale))),
                             Qt::AlignLeft | Qt::AlignVCenter, value);
        }

        void paint_gauge(QPainter& painter, const QRect& box, const QString& label, const QString& value, double ratio,
                         const QColor& fill, double scale)
        {
            const QFont value_font = theme::numericPx(std::max(theme::kMinNumberPx, px(13, scale)), QFont::DemiBold);
            const int value_w = QFontMetrics(value_font).horizontalAdvance(value);
            const int line = std::max(px(14, scale), px(12, scale) + 2);
            const int label_w = box.width() - value_w - px(6, scale);
            const int label_px = theme::fitFontPx(label, px(12, scale), theme::kMinLabelPx, label_w,
                                                  [](int size)
                                                  { return theme::labelPx(size); });
            const QFont label_font = theme::labelPx(label_px);
            painter.setFont(label_font);
            painter.setPen(theme::kTextSecondary);
            painter.drawText(QRect(box.left(), box.top(), label_w, line), Qt::AlignLeft | Qt::AlignVCenter,
                             QFontMetrics(label_font).elidedText(label, Qt::ElideRight, label_w));
            painter.setFont(value_font);
            painter.setPen(theme::kTextBright);
            painter.drawText(QRect(box.left(), box.top(), box.width(), line), Qt::AlignRight | Qt::AlignVCenter, value);
            // 条贴在字下面、吃掉这一格剩下的高度:格子变高时条变厚,不会在条下面空一截。
            const int track_top = box.top() + line + px(3, scale);
            const int track_h = std::clamp(box.bottom() - track_top - px(2, scale), 4, px(10, scale));
            const QRectF track(box.left() + 3, track_top, box.width() - 6, track_h);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(255, 255, 255, 18));
            painter.drawPath(theme::skewBar(track, -24));
            QColor glow = fill;
            glow.setAlphaF(0.88);
            painter.setBrush(glow);
            painter.drawPath(theme::skewBar(QRectF(track.left(), track.top(), track.width() * std::clamp(ratio, 0.0, 1.0),
                                                   track.height()),
                                            -24));
        }

        // 状态灯画成「圆点 + 字」,不画成按钮:画成按钮的样子,操作手会去点「可远程补血」。补血补弹按 H、O、I。
        void paint_pip(QPainter& painter, const QRect& box, const QString& text, std::optional<bool> on,
                       const QColor& on_colour, double scale)
        {
            const bool lit = on.value_or(false);
            const qreal d = std::max(6.0, 8.0 * scale);
            const QRectF dot(box.left() + 3, box.center().y() + 0.5 - d / 2, d, d);
            if (lit)
            {
                QColor glow = on_colour;
                glow.setAlpha(70);
                painter.setPen(Qt::NoPen);
                painter.setBrush(glow);
                painter.drawEllipse(dot.adjusted(-2.5, -2.5, 2.5, 2.5));
                painter.setBrush(on_colour);
                painter.drawEllipse(dot);
            }
            else
            {
                painter.setPen(QPen(on.has_value() ? theme::kTextSecondary : theme::kTextMuted, 1.2));
                painter.setBrush(Qt::NoBrush);
                painter.drawEllipse(dot.adjusted(0.6, 0.6, -0.6, -0.6));
            }
            const int text_left = static_cast<int>(std::ceil(dot.right())) + 6;
            const QRect text_box(text_left, box.top(), box.right() - text_left, box.height());
            const int weight = lit ? QFont::DemiBold : QFont::Normal;
            const int text_px = theme::fitFontPx(text, px(12, scale), theme::kMinLabelPx, text_box.width(),
                                                 [weight](int size)
                                                 { return theme::labelPx(size, weight); });
            const QFont font = theme::labelPx(text_px, weight);
            painter.setFont(font);
            painter.setPen(!on.has_value() ? theme::kTextMuted : (lit ? theme::kTextPrimary : theme::kTextSecondary));
            painter.drawText(text_box, Qt::AlignLeft | Qt::AlignVCenter,
                             QFontMetrics(font).elidedText(text, Qt::ElideRight, text_box.width()));
        }

        QString name_text(const RosterEntry& e)
        {
            return e.level ? QStringLiteral("本机 %1 号%2 · %3 级")
                                 .arg(display_robot_number(e.id.value))
                                 .arg(robot_class_name(e.id.value))
                                 .arg(*e.level)
                           : QStringLiteral("本机 %1 号%2")
                                 .arg(display_robot_number(e.id.value))
                                 .arg(robot_class_name(e.id.value));
        }

        QString uint_text(std::optional<std::uint32_t> v) { return v ? QString::number(*v) : QStringLiteral("--"); }

        QString double_text(std::optional<double> v, int precision)
        {
            return v ? QStringLiteral("%1").arg(*v, 0, 'f', precision) : QStringLiteral("--");
        }

    } // namespace

    SelfStatusPane::SelfStatusPane(QWidget* parent) : QWidget(parent) {}

    void SelfStatusPane::setEntry(const std::optional<RosterEntry>& entry)
    {
        entry_ = entry;
        update();
    }

    QSize SelfStatusPane::sizeHint() const { return QSize(420, kContentHeight); }

    void SelfStatusPane::paintEvent(QPaintEvent*)
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        if (!entry_)
        {
            painter.setFont(theme::labelPx(14));
            painter.setPen(theme::kTextMuted);
            painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("等待本机数据"));
            return;
        }

        const RosterEntry& e = *entry_;
        const int w = width();
        const int h = height();
        // 一个系数管全部:字号、块高、间距都乘它,面板拖大就一起变大、拖小就一起变小。
        const double scale = theme::paneScale(QSizeF(w, h), QSizeF(kDesignWidth, kContentHeight), 0.6);
        // 高度够时三个仪表竖着排,每个占一整行;不够就并成一行,先少画再谈缩字。
        const auto height_of = [&](bool stacked)
        {
            const int gap = px(8, scale);
            const int gauges = stacked ? 3 * px(kGaugeHeight, scale) + 2 * gap : px(kGaugeHeight, scale);
            return px(kHeaderHeight, scale) + px(6, scale) + px(kTileHeight, scale) + gap + gauges + gap +
                   px(kPipHeight, scale);
        };
        const bool stacked = h >= height_of(true);
        // 剩下的高度按各段的设计高度分下去,全部进内容:块更高、字更大。绝不上下居中留白,
        // 也绝不空在底部 —— 操作手把面板拖多大,这四段就铺满多大。
        const int gap = px(8, scale);
        const int gauge_rows = stacked ? 3 : 1;
        // 一行三个数值格;面板高得连格子都撑不满时改成竖着三行,每行一个占满宽度。
        // 多出来的高度这样变成「更多更大的格子」,而不是一个高得离谱的空格子。
        const auto plan = [&](int tile_rows)
        {
            const int gaps = 2 + gauge_rows + (tile_rows - 1);
            const double weights = kHeaderHeight + tile_rows * kTileHeight + gauge_rows * kGaugeHeight + kPipHeight;
            const int usable = std::max(0, h - gaps * gap);
            return std::pair{usable, weights};
        };
        const auto tile_row_height = [&](int tile_rows)
        {
            const auto [usable, weights] = plan(tile_rows);
            const int header = std::min(static_cast<int>(usable * kHeaderHeight / weights), scaledPxMax(kHeaderHeight));
            const int gauge = std::min(static_cast<int>(usable * kGaugeHeight / weights), scaledPxMax(kGaugeHeight));
            const int pip = std::min(static_cast<int>(usable * kPipHeight / weights), scaledPxMax(kPipHeight));
            return std::max(1, (usable - header - gauge_rows * gauge - pip) / tile_rows);
        };
        const int tile_rows = tile_row_height(1) > scaledPxMax(kTileHeight) ? 3 : 1;
        const int gaps = 2 + gauge_rows + (tile_rows - 1);
        const auto [spare, weight_total] = plan(tile_rows);
        // 标题、仪表、状态灯这三段撑到设计高度的 kMaxPaneScale 倍就用不动了(字封了顶、
        // 条再厚就不像条),超出的高度全部让给数值格:玻璃格子配一个大数字,越高越好看,
        // 也不会变成一条空带。
        const auto capped = [&](double design_height)
        {
            const int share = std::max(1, static_cast<int>(std::floor(spare * design_height / weight_total)));
            return std::min(share, scaledPxMax(design_height));
        };
        const int header_h = capped(kHeaderHeight);
        const int gauge_h = capped(kGaugeHeight);
        const int pip_h = capped(kPipHeight);
        // 数值格吃掉全部零头,最后一段的底边正好落在面板底部。
        const int tile_h = std::max(1, (spare - header_h - gauge_rows * gauge_h - pip_h) / tile_rows);
        // 字号跟着自己那一段的高度走,段高才是真正的可用空间;但封顶在 kMaxPaneScale:
        // 面板被拖成细长条时段高能到设计值的四五倍,字再跟着长就比面板标题还大了。
        // 多出来的高度留给玻璃格子和条本身变厚,那是图形,长大了不难看。
        const auto font_k = [](int actual, double design)
        {
            return std::min<double>(actual / design, theme::kMaxPaneScale);
        };
        const double header_k = font_k(header_h, kHeaderHeight);
        const double tile_k = font_k(tile_h, kTileHeight);
        const double gauge_k = font_k(gauge_h, kGaugeHeight);
        const double pip_k = font_k(pip_h, kPipHeight);
        const int tile_gap = px(6, scale);

        const QString hp_text = e.hp && e.max_hp && *e.max_hp > 0 ? QStringLiteral("%1 / %2").arg(*e.hp).arg(*e.max_hp)
                                                                  : QString();
        // 标题行:左边名字、右边血量。血量先占好宽度,名字用剩下的 —— 两者都放不下时缩名字,
        // 血量是本机最要紧的数,不让它变小。
        const int hp_px = theme::fitFontPx(hp_text, px(21, header_k), theme::kMinNumberPx, w * 0.5,
                                           [](int size)
                                           { return theme::numericPx(size, QFont::Bold); });
        const QFont hp_font = theme::numericPx(hp_px, QFont::Bold);
        const int hp_w = hp_text.isEmpty() ? 0 : QFontMetrics(hp_font).horizontalAdvance(hp_text) + px(8, scale);
        const int name_px = theme::fitFontPx(name_text(e), px(15, header_k), theme::kMinLabelPx, w - hp_w,
                                             [](int size)
                                             { return theme::labelPx(size, QFont::Bold); });
        const QFont title = theme::labelPx(name_px, QFont::Bold);
        int y = 0;
        painter.setFont(title);
        painter.setPen(theme::kTextPrimary);
        const QString name = name_text(e);
        painter.drawText(QRect(0, y, w - hp_w, header_h), Qt::AlignLeft | Qt::AlignVCenter,
                         QFontMetrics(title).elidedText(name, Qt::ElideRight, w - hp_w));
        if (!hp_text.isEmpty())
        {
            const double ratio = static_cast<double>(*e.hp) / static_cast<double>(*e.max_hp);
            painter.setFont(hp_font);
            painter.setPen(ratio > 0.25 ? theme::kTextPrimary : theme::kDangerText);
            painter.drawText(QRect(0, y, w, header_h), Qt::AlignRight | Qt::AlignVCenter, hp_text);
        }

        y += header_h + gap;
        const struct
        {
            QString label;
            QString value;
            QColor ink;
        } tiles[] = {
            {QStringLiteral("允许发弹量"), uint_text(e.ammo), e.ammo && *e.ammo == 0 ? theme::kWarn : theme::kTextPrimary},
            {QStringLiteral("弹速 m/s"), double_text(e.bullet_speed, 1), theme::kTextPrimary},
            {QStringLiteral("我方经济"), uint_text(e.economy), theme::kTextPrimary},
        };
        if (tile_rows == 1)
        {
            const int tile_w = (w - tile_gap * 2) / 3;
            for (int i = 0; i < 3; ++i)
                paint_tile(painter, QRect(i * (tile_w + tile_gap), y, tile_w, tile_h), tiles[i].label, tiles[i].value,
                           tiles[i].ink, tile_k);
            y += tile_h;
        }
        else
        {
            for (int i = 0; i < 3; ++i)
            {
                paint_tile(painter, QRect(0, y, w, tile_h), tiles[i].label, tiles[i].value, tiles[i].ink, tile_k);
                y += tile_h + (i < 2 ? gap : 0);
            }
        }

        struct Gauge
        {
            QString label;
            QString value;
            double ratio;
            QColor fill;
        };
        std::vector<Gauge> gauges;
        // 热量的危险方向和血量相反,所以取 1-ratio 喂给 healthColor:热量越接近上限越该显红。
        if (e.heat && e.heat_limit && *e.heat_limit > 0)
        {
            const double ratio = *e.heat / static_cast<double>(*e.heat_limit);
            gauges.push_back({QStringLiteral("枪口热量"), QStringLiteral("%1 / %2").arg(*e.heat, 0, 'f', 0).arg(*e.heat_limit),
                              ratio, theme::healthColor(1.0 - ratio)});
        }
        else
        {
            gauges.push_back({QStringLiteral("枪口热量"), QStringLiteral("--"), 0.0, theme::kOffline});
        }
        if (e.buffer_energy && e.buffer_limit && *e.buffer_limit > 0)
            gauges.push_back({QStringLiteral("缓冲能量"), QStringLiteral("%1 / %2").arg(*e.buffer_energy).arg(*e.buffer_limit),
                              static_cast<double>(*e.buffer_energy) / *e.buffer_limit, theme::kCyan});
        else
            gauges.push_back({QStringLiteral("缓冲能量"), uint_text(e.buffer_energy), 0.0, theme::kOffline});
        if (e.modules_online && e.modules_total)
            gauges.push_back({QStringLiteral("模块在线"), QStringLiteral("%1 / %2").arg(*e.modules_online).arg(*e.modules_total),
                              static_cast<double>(*e.modules_online) / static_cast<double>(*e.modules_total),
                              *e.modules_online == *e.modules_total ? theme::kOk : theme::kWarn});
        else
            gauges.push_back({QStringLiteral("模块在线"), QStringLiteral("--"), 0.0, theme::kOffline});

        y += gap;
        if (stacked)
        {
            for (const Gauge& g : gauges)
            {
                paint_gauge(painter, QRect(0, y, w, gauge_h), g.label, g.value, g.ratio, g.fill, gauge_k);
                y += gauge_h + gap;
            }
            y -= gap;
        }
        else
        {
            const int gauge_w = (w - tile_gap * 2 * 2) / 3;
            for (std::size_t i = 0; i < gauges.size(); ++i)
                paint_gauge(painter, QRect(static_cast<int>(i) * (gauge_w + tile_gap * 2), y, gauge_w, gauge_h), gauges[i].label,
                            gauges[i].value, gauges[i].ratio, gauges[i].fill, gauge_k);
            y += gauge_h;
        }

        y += gap;
        const int pip_gap = px(6, scale);
        const int pip_w = (w - 3 * pip_gap) / 4;
        const struct
        {
            QString text;
            std::optional<bool> on;
            QColor colour;
        } pips[] = {
            {QStringLiteral("脱战"), e.out_of_combat, theme::kOk},
            {QStringLiteral("可远程补血"), e.can_remote_heal, theme::kCyan},
            {QStringLiteral("可远程补弹"), e.can_remote_ammo, theme::kCyan},
            {QStringLiteral("待复活"), e.pending_respawn, theme::kWarn},
        };
        for (int i = 0; i < 4; ++i)
            paint_pip(painter, QRect(i * (pip_w + pip_gap), y, pip_w, pip_h), pips[i].text, pips[i].on, pips[i].colour, pip_k);
    }

} // namespace rm_terminal
