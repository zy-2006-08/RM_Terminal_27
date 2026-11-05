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

namespace rm_terminal {

namespace {

// 按高度分档,每一档都整块画得下才用,不裁半截(四周各留 kPadX / kPadY)。
// 完整:控制度 60、四个阵地 52、堡垒一行 34、统计 70,间隔至少 8。
constexpr int kFullArea = 60 + 52 + 34 + 70 + 3 * 8;
// 中等(3 格高,1280 宽的小窗口也够):控制度一行 32、五个色块(四阵地 + 堡垒)40、统计一行 42,间隔至少 8。
constexpr int kMediumArea = 32 + 40 + 42 + 2 * 8;
// 紧凑(2 格高):控制度一行 32、五个色块 40,上下留出空;统计挪到卡片标题栏的小字里。
constexpr int kCompactArea = 32 + 40 + 10;
// 迷你(1280 × 760 小窗口里的 2 格高):控制度一行 24、五个色块 34。
constexpr int kMiniArea = 24 + 34 + 4;
constexpr int kMaxGap = 24;
constexpr qreal kPadX = 6;
constexpr qreal kPadY = 4;
constexpr int kFullHeight = kFullArea + 2 * 4;

QColor with_alpha(QColor colour, qreal alpha) {
    colour.setAlphaF(alpha);
    return colour;
}

QColor side_text(bool blue) { return blue ? theme::kBlueText : theme::kRedText; }
QColor side_fill(bool blue) { return blue ? theme::kFillBlue : theme::kFillRed; }

// 素材是白色前景 + 透明背景,用 SourceIn 整体染成标签色,视觉重量才和文字一致。
// 按「路径 + 颜色 + 尺寸 + DPR」缓存:paintEvent 每帧都会走到这里。
QPixmap tinted_icon(const QString& path, const QColor& colour, qreal dpr, int size) {
    static QHash<QString, QPixmap> cache;
    const QString key = QStringLiteral("%1|%2|%3|%4").arg(path, colour.name(QColor::HexArgb)).arg(size).arg(dpr);
    const auto hit = cache.constFind(key);
    if (hit != cache.constEnd()) return *hit;
    QPixmap source(path);
    if (source.isNull()) return {};
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

QString span(const QString& text, const QColor& colour, bool number) {
    if (!number) return QStringLiteral("<span style='color:%1'>%2</span>").arg(colour.name(), text.toHtmlEscaped());
    return QStringLiteral("<span style='font-family:\"%1\"; font-weight:600; font-size:14px; color:%2'>%3</span>")
        .arg(theme::numericFamily(), colour.name(), text.toHtmlEscaped());
}

QString value_text(std::optional<std::int64_t> value) { return value ? QString::number(*value) : QStringLiteral("--"); }

// 阵地色块的样子:完全控制是整块阵营色渐变,临时控制是斜纹,左边一道竖条(临时是虚线)。
struct ZoneLook {
    QColor bar;
    QColor text;
    QColor strong;
    QColor weak;
    bool stripes = false;
    bool tinted = false;
};

ZoneLook zone_look(ZoneControl control) {
    ZoneLook look{theme::kTextMuted, theme::kTextBright, theme::kTile, theme::kTile};
    const auto faction = [&look](bool blue, bool full) {
        look.bar = blue ? theme::kBlue : theme::kRed;
        look.text = side_text(blue);
        look.tinted = true;
        look.stripes = !full;
        const QColor base = blue ? theme::kBlue : theme::kRed;
        look.strong = with_alpha(base, full ? (blue ? 0.28 : 0.26) : 0.17);
        look.weak = with_alpha(base, full ? 0.08 : 0.05);
    };
    switch (control) {
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
QString short_zone_text(QString text) {
    text.remove(QStringLiteral("区"));
    text.replace(QStringLiteral("红方"), QStringLiteral("红"));
    text.replace(QStringLiteral("蓝方"), QStringLiteral("蓝"));
    text.replace(QStringLiteral("我方在激活"), QStringLiteral("激活中"));
    text.replace(QStringLiteral("无人激活"), QStringLiteral("无人"));
    text.remove(QLatin1Char(' '));
    return text;
}

QString short_stat_label(QString text) {
    text.remove(QStringLiteral("我方"));
    text.remove(QStringLiteral("总"));
    text.replace(QStringLiteral("能量机关"), QStringLiteral("机关"));
    return text;
}

void paint_tile(QPainter& painter, const QRectF& tile, const ZoneLook& look, const QString& name,
                const QString& status, const QString& note) {
    painter.save();
    painter.setClipPath(theme::chamferPath(tile, 6));
    if (!look.tinted) {
        painter.fillRect(tile, look.strong);
    } else if (look.stripes) {
        // repeating-linear-gradient(-45deg, strong 0 5px, weak 5px 10px)
        QLinearGradient stripes(tile.topLeft(), tile.topLeft() + QPointF(-7.071, -7.071));
        stripes.setSpread(QGradient::RepeatSpread);
        stripes.setColorAt(0, look.strong);
        stripes.setColorAt(0.4999, look.strong);
        stripes.setColorAt(0.5, look.weak);
        stripes.setColorAt(1, look.weak);
        painter.fillRect(tile, stripes);
    } else {
        QLinearGradient fade(tile.topLeft(), tile.topRight());
        fade.setColorAt(0, look.strong);
        fade.setColorAt(1, look.weak);
        painter.fillRect(tile, fade);
    }
    if (look.stripes) {
        for (qreal y = tile.top(); y < tile.bottom(); y += 7)
            painter.fillRect(QRectF(tile.left(), y, 3, std::min<qreal>(4, tile.bottom() - y)), look.bar);
    } else {
        painter.fillRect(QRectF(tile.left(), tile.top(), 3, tile.height()), look.bar);
    }
    // 字跟着色块的高度变,上下居中;色块窄(面板只有 4 格宽)时名字和状态用短的说法。
    const qreal f = std::clamp(tile.height() / 52.0, 0.7, 1.0);
    const int name_px = std::max(11, static_cast<int>(std::lround(12.5 * f)));
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
    int fit_name_px = name_px;
    while (fit_name_px > 10 && QFontMetricsF(theme::labelPx(fit_name_px)).horizontalAdvance(shown_name) > text_w) --fit_name_px;
    painter.setFont(theme::labelPx(fit_name_px));
    painter.setPen(theme::kTextSecondary);
    const QRectF name_box(tile.left() + pad, top, text_w, name_h);
    painter.drawText(name_box, Qt::AlignLeft | Qt::AlignVCenter,
                     painter.fontMetrics().elidedText(shown_name, Qt::ElideRight, static_cast<int>(name_box.width())));
    if (!note.isEmpty() && !narrow) {
        painter.setFont(theme::numericPx(12, QFont::DemiBold));
        painter.setPen(theme::kTextBright);
        painter.drawText(name_box.adjusted(0, 0, -4, 0), Qt::AlignRight | Qt::AlignVCenter, note);
    }
    const auto status_font_of = [narrow](int size) {
        QFont font = theme::labelPx(size, QFont::Bold);
        font.setLetterSpacing(QFont::AbsoluteSpacing, narrow ? 0 : 1);
        return font;
    };
    int fit_status_px = status_px;
    while (fit_status_px > 11 && QFontMetricsF(status_font_of(fit_status_px)).horizontalAdvance(shown_status) > text_w)
        --fit_status_px;
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
               std::optional<std::uint32_t> blue) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(255, 255, 255, 18));
    painter.drawPath(theme::skewBar(bar, -24));
    const double total = red && blue ? static_cast<double>(*red) + *blue : 0.0;
    if (total > 0) {
        const auto side = [&](bool blue_side, double share) {
            const qreal width = std::max<qreal>(0, bar.width() * share - 2);
            if (width <= 0) return;
            const QRectF part(blue_side ? bar.right() - width : bar.left(), bar.top(), width, bar.height());
            const QColor base = side_fill(blue_side);
            for (const auto& [grow, a] : {std::pair{4.0, 0.10}, std::pair{2.0, 0.18}}) {
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

void paint_coin(QPainter& painter, const QRectF& box, const QColor& colour) {
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

}  // namespace

BattleAnalysisPane::BattleAnalysisPane(QWidget* parent) : QWidget(parent) {}

void BattleAnalysisPane::setMetrics(const std::vector<AnalysisMetric>& metrics) {
    metrics_ = metrics;
    update();
}

void BattleAnalysisPane::setFortress(const FortressStatus& status) {
    fortress_ = status;
    update();
}

void BattleAnalysisPane::setZoneBoard(const ZoneBoard& board) {
    board_ = board;
    update();
}

void BattleAnalysisPane::setOwnFaction(std::uint32_t faction) {
    own_faction_ = faction;
    update();
}

// 统计不在面板里画的那几档,统计写到卡片标题栏右边的小字里。
bool BattleAnalysisPane::compact() const {
    const Level l = level();
    return !control_only_ && l != Level::Full && l != Level::Medium;
}

void BattleAnalysisPane::setControlOnly(bool control_only) {
    control_only_ = control_only;
    update();
}

void BattleAnalysisPane::paintBigControl(QPainter& painter, const QRectF& area) const {
    const std::optional<std::uint32_t> red = board_.red_control_points;
    const std::optional<std::uint32_t> blue = board_.blue_control_points;
    const bool own_blue = own_faction_ == 2;
    QString lead = QStringLiteral("等待数据");
    QColor lead_colour = theme::kTextBright;
    if (red && blue) {
        const long ours = static_cast<long>(own_blue ? *blue : *red);
        const long theirs = static_cast<long>(own_blue ? *red : *blue);
        if (ours == theirs) lead = QStringLiteral("双方持平");
        else {
            lead = QStringLiteral("%1 %2 分").arg(ours > theirs ? QStringLiteral("我方领先") : QStringLiteral("敌方领先"))
                       .arg(std::labs(ours - theirs));
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
    const auto value = [&](const QRectF& box, std::optional<std::uint32_t> v, bool blue_side) {
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

QString BattleAnalysisPane::headerText() const {
    if (control_only_) return {};
    const auto ratio = [](const std::optional<double>& v) { return v ? QString::number(*v, 'f', 2) : QStringLiteral("--"); };
    const QString dot = QStringLiteral("&nbsp;<span style='color:%1'>·</span>&nbsp;").arg(theme::kTextMuted.name());
    const auto pair = [&](const QString& red, const QString& blue) {
        return span(red, theme::kRedText, true) + span(QStringLiteral(":"), theme::kTextSecondary, false) +
               span(blue, theme::kBlueText, true);
    };
    const QString logistics =
        QStringLiteral("后勤&nbsp;") + (compact() ? pair(ratio(board_.red_logistics), ratio(board_.blue_logistics))
                                                  : span(ratio(board_.red_logistics), theme::kRedText, true) + dot +
                                                        span(ratio(board_.blue_logistics), theme::kBlueText, true));
    if (!compact()) return logistics;
    const bool blue = own_faction_ == 2;
    const auto metric_pair = [&](std::size_t index) {
        if (index >= metrics_.size()) return pair(QStringLiteral("--"), QStringLiteral("--"));
        const AnalysisMetric& m = metrics_[index];
        return blue ? pair(value_text(m.enemy_value), value_text(m.self_value))
                    : pair(value_text(m.self_value), value_text(m.enemy_value));
    };
    const QString economy = metrics_.empty() ? QStringLiteral("--") : value_text(metrics_[0].self_value);
    return QStringLiteral("经济&nbsp;") + span(economy, theme::kTextBright, true) + dot + QStringLiteral("伤害&nbsp;") +
           metric_pair(1) + dot + QStringLiteral("击杀&nbsp;") + metric_pair(2) + dot + logistics;
}

void BattleAnalysisPane::paintControl(QPainter& painter, const QRectF& row, bool compact) const {
    const std::optional<std::uint32_t> red = board_.red_control_points;
    const std::optional<std::uint32_t> blue = board_.blue_control_points;
    const bool own_blue = own_faction_ == 2;
    QString lead_label = QStringLiteral("等待数据");
    QString lead_number;
    QColor lead_colour = theme::kTextBright;
    if (red && blue) {
        const long ours = static_cast<long>(own_blue ? *blue : *red);
        const long theirs = static_cast<long>(own_blue ? *red : *blue);
        if (ours == theirs) {
            lead_label = QStringLiteral("双方持平");
        } else {
            lead_label = ours > theirs ? QStringLiteral("我方领先") : QStringLiteral("敌方领先");
            lead_number = QString::number(std::labs(ours - theirs));
            lead_colour = side_text(ours > theirs ? own_blue : !own_blue);
        }
    }
    const bool mini = compact && row.height() < 30;
    const int label_px = mini ? 12 : 13;
    const auto paint_lead = [&](const QRectF& box, Qt::Alignment align, int number_px) {
        const QFont label = theme::labelPx(label_px);
        const QFont number = theme::numericPx(number_px, QFont::Bold);
        const qreal width = theme::mixedTextWidth(lead_label, label, label) +
                            (lead_number.isEmpty() ? 0 : QFontMetricsF(number).horizontalAdvance(lead_number) + 6 +
                                                             QFontMetricsF(label).horizontalAdvance(QStringLiteral("分")));
        qreal x = align & Qt::AlignRight ? box.right() - width : box.left();
        x += theme::paintMixedText(painter, QRectF(x, box.top(), width, box.height()), Qt::AlignLeft | Qt::AlignVCenter,
                                   lead_label, label, label, theme::kTextBright);
        if (lead_number.isEmpty()) return width;
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
    const auto value = [&](const QRectF& box, std::optional<std::uint32_t> v, bool blue_side, int px) {
        painter.setFont(theme::numericPx(px, QFont::Bold));
        painter.setPen(v ? side_text(blue_side) : theme::kTextMuted);
        painter.drawText(box, (blue_side ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter,
                         v ? QString::number(*v) : QStringLiteral("--"));
    };

    if (!compact) {
        const QRectF top(row.left(), row.top(), row.width(), 16);
        painter.setFont(theme::labelPx(13));
        painter.setPen(theme::kTextSecondary);
        painter.drawText(top, Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("控制度"));
        paint_lead(top.adjusted(0, -3, 0, 3), Qt::AlignRight, 18);
        const QRectF main(row.left(), row.top() + 22, row.width(), 38);
        value(QRectF(main.left(), main.top(), 64, main.height()), red, false, 38);
        value(QRectF(main.right() - 64, main.top(), 64, main.height()), blue, true, 38);
        paint_tug(painter, QRectF(main.left() + 70, main.center().y() - 6, main.width() - 140, 12), red, blue);
        return;
    }
    painter.setFont(theme::labelPx(label_px));
    painter.setPen(theme::kTextSecondary);
    const qreal label_w = QFontMetricsF(painter.font()).horizontalAdvance(QStringLiteral("控制度"));
    painter.drawText(QRectF(row.left(), row.top(), label_w, row.height()), Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("控制度"));
    const qreal lead_w = theme::mixedTextWidth(lead_label, theme::labelPx(label_px), theme::labelPx(label_px)) +
                         (lead_number.isEmpty() ? 0 : 40);
    const qreal lead_left = row.right() - lead_w;
    paint_lead(QRectF(lead_left, row.top(), lead_w, row.height()), Qt::AlignRight, mini ? 15 : 17);
    const qreal value_w = mini ? 38 : 46;
    const int number_px = mini ? 22 : 28;
    const qreal red_left = row.left() + label_w + (mini ? 8 : 12);
    value(QRectF(red_left, row.top(), value_w, row.height()), red, false, number_px);
    const qreal blue_right = lead_left - (mini ? 8 : 12);
    value(QRectF(blue_right - value_w, row.top(), value_w, row.height()), blue, true, number_px);
    const qreal bar_left = red_left + value_w + 10;
    const qreal bar_right = blue_right - value_w - 10;
    if (bar_right - bar_left > 24)
        paint_tug(painter, QRectF(bar_left, row.center().y() - (mini ? 5 : 6), bar_right - bar_left, mini ? 10 : 12), red,
                  blue);
}

void BattleAnalysisPane::paintZones(QPainter& painter, const QRectF& area, bool with_fortress) const {
    const int count = with_fortress ? 5 : 4;
    const qreal gap = 8;
    const qreal width = (area.width() - gap * (count - 1)) / count;
    for (int i = 0; i < 4; ++i) {
        const ZoneStatus& zone = board_.zones[static_cast<std::size_t>(i)];
        QString note;
        if (zone.progress && *zone.progress != 0)
            note = QStringLiteral("%1 %2s").arg(*zone.progress > 0 ? QStringLiteral("红推") : QStringLiteral("蓝推"))
                       .arg(std::abs(*zone.progress));
        paint_tile(painter, QRectF(area.left() + i * (width + gap), area.top(), width, area.height()),
                   zone_look(zone.control), zone.name.isEmpty() ? QStringLiteral("--") : zone.name,
                   zone_control_text(zone.control), note);
    }
    if (!with_fortress) return;
    ZoneLook look = zone_look(ZoneControl::Neutral);
    QString status = QStringLiteral("--");
    if (fortress_.ours_by_enemy || fortress_.theirs_by_us) status = QStringLiteral("无人激活");
    if (fortress_.theirs_by_us && *fortress_.theirs_by_us > 0) {
        status = QStringLiteral("我方在激活 %1s").arg(*fortress_.theirs_by_us);
        look.text = theme::kOkText;
    }
    if (fortress_.ours_by_enemy && *fortress_.ours_by_enemy > 0) {
        status = QStringLiteral("被偷 %1s/10s").arg(*fortress_.ours_by_enemy);
        look.text = theme::kDangerText;
        look.bar = theme::kDanger;
    }
    paint_tile(painter, QRectF(area.left() + 4 * (width + gap), area.top(), width, area.height()), look,
               QStringLiteral("堡垒"), status, QString());
}

void BattleAnalysisPane::paintFortress(QPainter& painter, const QRectF& row) const {
    theme::paintGlass(painter, row, 6, false, theme::kTile, Qt::transparent);
    const QRectF chip(row.left() + 12, row.center().y() - 10, 38, 20);
    theme::paintGlass(painter, chip, 4, false, theme::kAccentBg, Qt::transparent);
    QFont chip_font = theme::labelPx(12, QFont::Bold);
    chip_font.setLetterSpacing(QFont::AbsoluteSpacing, 1);
    painter.setFont(chip_font);
    painter.setPen(theme::kAccentText);
    painter.drawText(chip, Qt::AlignCenter, QStringLiteral("堡垒"));
    QColor ink = theme::kTextPrimary;
    switch (fortress_tone(fortress_)) {
    case FortressTone::Danger: ink = theme::kDangerText; break;
    case FortressTone::Good: ink = theme::kOkText; break;
    case FortressTone::Unknown: ink = theme::kTextMuted; break;
    case FortressTone::Quiet: break;
    }
    const QRectF text(chip.right() + 10, row.top(), row.right() - chip.right() - 22, row.height());
    painter.setFont(theme::labelPx(14));
    painter.setPen(ink);
    painter.drawText(text, Qt::AlignLeft | Qt::AlignVCenter,
                     painter.fontMetrics().elidedText(fortress_status_text(fortress_), Qt::ElideRight,
                                                      static_cast<int>(text.width())));
}

void BattleAnalysisPane::paintStats(QPainter& painter, const QRectF& row, bool compact) const {
    static const QString icons[4] = {QString(), QStringLiteral(":/images/message/validate_icon_totalhurt.png"),
                                     QStringLiteral(":/images/message/validate_icon_killcount.png"),
                                     QStringLiteral(":/images/message/validate_icon_rune.png")};
    const qreal width = row.width() / 4;
    const bool own_blue = own_faction_ == 2;
    for (int i = 0; i < 4; ++i) {
        const QRectF cell(row.left() + i * width, row.top(), width, row.height());
        if (i > 0) painter.fillRect(QRectF(cell.left(), cell.top(), 1, cell.height()), theme::kRowBorder);
        const bool tight = cell.width() < 120;
        const qreal left = cell.left() + (i == 0 ? 2 : tight ? 8 : 14);
        const qreal right = cell.right() - (tight ? 6 : 14);
        if (static_cast<std::size_t>(i) >= metrics_.size()) continue;
        const AnalysisMetric& metric = metrics_[static_cast<std::size_t>(i)];

        const QRectF icon_box(left, cell.top() + 2, 15, 15);
        if (i == 0) {
            paint_coin(painter, icon_box, theme::kTextSecondary);
        } else {
            const QPixmap icon = tinted_icon(icons[i], theme::kTextSecondary, devicePixelRatioF(), 15);
            if (!icon.isNull()) painter.drawPixmap(icon_box.topLeft(), icon);
        }
        painter.setFont(theme::labelPx(compact ? 12 : 13));
        painter.setPen(theme::kTextSecondary);
        const QRectF label_box(left + 21, cell.top() + 2, right - left - 21, 15);
        // 格子窄时用短的说法(「我方经济」→「经济」),还放不下再省略。
        QString label_text = metric.label;
        if (painter.fontMetrics().horizontalAdvance(label_text) > label_box.width())
            label_text = short_stat_label(label_text);
        painter.drawText(label_box, Qt::AlignLeft | Qt::AlignVCenter,
                         painter.fontMetrics().elidedText(label_text, Qt::ElideRight, static_cast<int>(label_box.width())));

        const QRectF value_box = compact ? QRectF(left, cell.top() + 18, right - left, 24)
                                         : QRectF(left, cell.top() + 26, right - left, 32);
        const bool text_value = !metric.text_value.isEmpty();
        const bool has_value = text_value || metric.self_value.has_value();
        const QString shown = text_value ? metric.text_value : value_text(metric.self_value);
        // 字放不下这一格就缩小,不切掉半个字。
        int value_px = text_value ? (compact ? 16 : 21) : (compact ? 22 : 30);
        const auto make_font = [&](int px) {
            QFont font = text_value ? theme::labelPx(px, QFont::Bold) : theme::numericPx(px, QFont::DemiBold);
            if (text_value) font.setLetterSpacing(QFont::AbsoluteSpacing, 1);
            return font;
        };
        QFont value_font = make_font(value_px);
        while (value_px > 12 && QFontMetricsF(value_font).horizontalAdvance(shown) > value_box.width())
            value_font = make_font(--value_px);
        painter.setFont(value_font);
        painter.setPen(has_value ? theme::kTextPrimary : theme::kTextMuted);
        painter.drawText(value_box, Qt::AlignLeft | Qt::AlignVCenter, shown);
        const qreal value_width = QFontMetricsF(value_font).horizontalAdvance(shown);

        QString aside_label;
        QString aside_number;
        if (metric.has_enemy) {
            aside_label = QStringLiteral("敌");
            aside_number = value_text(metric.enemy_value);
        } else if (!metric.note.isEmpty()) {
            const int space = metric.note.indexOf(QLatin1Char(' '));
            aside_label = space > 0 ? metric.note.left(space) : metric.note;
            aside_number = space > 0 ? metric.note.mid(space + 1) : QString();
        }
        if (!aside_label.isEmpty()) {
            const QFont label = theme::labelPx(compact ? 12 : 13);
            const QFont number = theme::numericPx(compact ? 13 : 15, QFont::DemiBold);
            const qreal aside_width = QFontMetricsF(label).horizontalAdvance(aside_label) + 4 +
                                      QFontMetricsF(number).horizontalAdvance(aside_number);
            if (value_width + 8 + aside_width <= right - left) {
                const QRectF aside(right - aside_width, value_box.top() + (compact ? 4 : 6), aside_width,
                                   value_box.height() - (compact ? 4 : 6));
                painter.setFont(label);
                painter.setPen(theme::kTextSecondary);
                painter.drawText(aside, Qt::AlignLeft | Qt::AlignVCenter, aside_label);
                painter.setFont(number);
                painter.setPen(theme::kTextBright);
                painter.drawText(aside, Qt::AlignRight | Qt::AlignVCenter, aside_number);
            }
        }

        if (metric.has_enemy && metric.self_value && metric.enemy_value) {
            const double total = static_cast<double>(*metric.self_value) + static_cast<double>(*metric.enemy_value);
            const QRectF bar(left, cell.bottom() - (compact ? 3 : 6), right - left, 3);
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

BattleAnalysisPane::Level BattleAnalysisPane::level() const {
    const qreal h = height() - 2 * kPadY;
    if (h >= kFullArea) return Level::Full;
    if (h >= kMediumArea) return Level::Medium;
    if (h >= kCompactArea) return Level::Compact;
    if (h >= kMiniArea) return Level::Mini;
    return Level::Tiny;
}

void BattleAnalysisPane::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF area = QRectF(rect()).adjusted(kPadX, kPadY, -kPadX, -kPadY);
    if (control_only_) {
        paintBigControl(painter, area);
        return;
    }
    // 按高度挑一档,整块都画得下的那档里最全的;多出来的高度匀到各段之间(每道最多 kMaxGap),再多就上下居中。
    struct Block {
        qreal height;
        std::function<void(const QRectF&)> draw;
    };
    std::vector<Block> blocks;
    qreal min_gap = 10;
    const auto control = [&](bool compact) { return [this, &painter, compact](const QRectF& r) { paintControl(painter, r, compact); }; };
    const auto zones = [&](bool with_fortress) {
        return [this, &painter, with_fortress](const QRectF& r) { paintZones(painter, r, with_fortress); };
    };
    const auto stats = [&](bool compact) { return [this, &painter, compact](const QRectF& r) { paintStats(painter, r, compact); }; };
    switch (level()) {
    case Level::Full:
        min_gap = 8;
        blocks = {{60, control(false)}, {52, zones(false)}, {34, [&](const QRectF& r) { paintFortress(painter, r); }},
                  {70, stats(false)}};
        break;
    case Level::Medium:
        min_gap = 8;
        blocks = {{32, control(true)}, {40, zones(true)}, {42, stats(true)}};
        break;
    case Level::Compact: blocks = {{32, control(true)}, {40, zones(true)}}; break;
    case Level::Mini:
        min_gap = 4;
        blocks = {{24, control(true)}, {34, zones(true)}};
        break;
    case Level::Tiny: blocks = {{std::clamp<qreal>(area.height(), 24, 40), control(true)}}; break;
    }
    qreal used = 0;
    for (const Block& block : blocks) used += block.height;
    const int gaps = static_cast<int>(blocks.size()) - 1;
    const qreal spare = std::max<qreal>(0, area.height() - used);
    const qreal gap = gaps > 0 ? std::clamp<qreal>(spare / (gaps + 2), min_gap, kMaxGap) : 0;
    qreal y = area.top() + std::max<qreal>(0, (spare - gap * gaps) / 2);
    for (const Block& block : blocks) {
        block.draw(QRectF(area.left(), y, area.width(), block.height));
        y += block.height + gap;
    }
}

}  // namespace rm_terminal
