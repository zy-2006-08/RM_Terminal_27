#include "next_step_pane.h"

#include "theme.h"

#include <QFontMetricsF>
#include <QLinearGradient>
#include <QPainter>
#include <QPaintEvent>
#include <algorithm>
#include <cmath>

namespace rm_terminal
{

    namespace
    {

        // 一张卡片的设计尺寸(效果图 7):并排那种 200 × 92。
        constexpr qreal kCardDesignWidth = 200;
        constexpr qreal kCardDesignHeight = 92;
        // 并排的最低要求:每张卡至少这么宽,窄过这个数标题就只剩两三个字。
        constexpr qreal kMinSideBySideWidth = 200;
        // 并排还要求面板本身够扁:宽不到高的 1.6 倍就改成竖着排。
        constexpr qreal kWideAspect = 1.6;

        struct Card
        {
            QRectF box;
            qreal scale;
        };

    } // namespace

    NextStepPane::NextStepPane(QWidget* parent) : QWidget(parent) {}

    void NextStepPane::setCards(const std::vector<NextCard>& cards)
    {
        const bool same = cards.size() == cards_.size() &&
                          std::equal(cards.begin(), cards.end(), cards_.begin(), [](const NextCard& a, const NextCard& b)
                                     { return a.rule == b.rule && a.title == b.title && a.reason == b.reason; });
        if (same)
            return;
        cards_ = cards;
        update();
    }

    void NextStepPane::setCardSize(int size)
    {
        if (size_ == size)
            return;
        size_ = size;
        update();
    }

    void NextStepPane::setCapacity(int capacity)
    {
        if (capacity_ == capacity)
            return;
        capacity_ = std::clamp(capacity, 1, 3);
        update();
    }

    QSize NextStepPane::sizeHint() const { return QSize(600, 92); }

    QSize NextStepPane::minimumSizeHint() const { return QSize(240, 48); }

    // 并排还是竖排(卡片里的排法也用这个结论):两个条件都满足才并排 —— 每张卡至少
    // kMinSideBySideWidth 宽,且面板宽到高的 kWideAspect 倍以上。否则竖着排,每张卡是一整行
    // (排名在左、标题和原因在右),横着的空间用来写字,不用来留白。
    // 只有一张卡时它占满面板,卡里按同一个形状判断横排还是竖排。
    bool NextStepPane::sideBySide(int count) const
    {
        const int columns = std::max(1, count);
        const qreal each = (width() - 8.0 * (columns - 1)) / columns;
        return each >= kMinSideBySideWidth && width() >= height() * kWideAspect;
    }

    void NextStepPane::paintEvent(QPaintEvent*)
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        if (cards_.empty())
        {
            paintEmpty(painter);
            return;
        }
        // 只画真有的事,几张就把面板分成几份:1 张铺满,2 张各一半,3 张各三分之一。
        // 不再画「暂时没有更要紧的事」的虚线占位卡 —— 那是在拿空卡填空,看着就是一块空白。
        const int count = std::min<int>(static_cast<int>(cards_.size()), capacity_);
        const bool columns = sideBySide(count);
        const qreal gap = std::min<qreal>(8, (columns ? width() : height()) / (count * 6.0));
        const qreal span = ((columns ? width() : height()) - gap * (count - 1)) / count;
        for (int i = 0; i < count; ++i)
        {
            const qreal offset = i * (span + gap);
            const QRectF box = columns ? QRectF(offset, 0, span, height()) : QRectF(0, offset, width(), span);
            paintCard(painter, cards_[static_cast<std::size_t>(i)], box, i, columns);
        }
    }

    // 没有要紧的事:一句话画大、居中,按面板大小缩放,放不下一行就折成两行。
    // 这是面板的正常状态之一(局势平稳),所以要像一句话,不是像一行被遗忘的小字。
    void NextStepPane::paintEmpty(QPainter& painter) const
    {
        const QString text = QStringLiteral("现在没有要紧的事：盯住地图和血量，按自己的节奏打");
        const QRectF box = QRectF(rect()).adjusted(12, 8, -12, -8);
        if (box.width() <= 0 || box.height() <= 0)
            return;
        if (empty_plan_.size != size())
        {
            // 一行和两行各算一遍能给到的最大字号,取字更大的那种:宽面板一行就够(字可以很大),
            // 窄面板折两行才能把字写大。字号上限按面板高度给,免得一行字高过整块面板。
            const auto fits = [&](int px, int lines)
            {
                const QFontMetricsF metrics(theme::labelPx(px));
                return metrics.height() * lines <= box.height() && metrics.horizontalAdvance(text) <= box.width() * lines;
            };
            // 先按 kRefPx 量一次、按比例推出字号,再上下微调一两号。从上限一号一号往下试的话,每个字号
            // 都要新加载一份字体,高面板一次要试几百号、画一遍零点几秒;准备和结算阶段它一直空着,
            // 底下一重画整个界面就卡住。
            const QFontMetricsF ref(theme::labelPx(kRefPx));
            const auto largest = [&](int lines)
            {
                const int cap = std::max(theme::kMinLabelPx, static_cast<int>(std::lround(box.height() / lines)));
                const qreal ratio = std::min(box.height() / (ref.height() * lines),
                                             box.width() * lines / std::max<qreal>(1, ref.horizontalAdvance(text)));
                int px = std::clamp(static_cast<int>(kRefPx * ratio), theme::kMinLabelPx, cap);
                while (px > theme::kMinLabelPx && !fits(px, lines))
                    --px;
                while (px < cap && fits(px + 1, lines))
                    ++px;
                return px;
            };
            const int one_line = largest(1);
            const int two_lines = largest(2);
            empty_plan_ = {size(), std::max(one_line, two_lines), two_lines > one_line};
        }
        const bool wrap = empty_plan_.wrap;
        const QFont font = theme::labelPx(empty_plan_.px);
        painter.setFont(font);
        painter.setPen(theme::kTextSecondary);
        painter.drawText(box, Qt::AlignCenter | (wrap ? Qt::TextWordWrap : Qt::TextSingleLine), text);
    }

    void NextStepPane::paintCard(QPainter& painter, const NextCard& card, const QRectF& box, int index,
                                 bool columns) const
    {
        const bool top = index == 0;
        painter.save();
        painter.setClipPath(theme::chamferPath(box, 6));
        if (top)
        {
            QLinearGradient glow(box.topLeft(), box.bottomRight());
            glow.setColorAt(0, QColor(64, 170, 230, 77));
            glow.setColorAt(0.7, QColor(64, 170, 230, 20));
            glow.setColorAt(1, QColor(64, 170, 230, 20));
            painter.fillRect(box, glow);
        }
        else
        {
            painter.fillRect(box, QColor(255, 255, 255, 10));
        }
        painter.restore();

        // 卡片自己的缩放系数:卡片拖大字就变大、拖小就变小,上下都放开(最多 kMaxPaneScale)。
        // 竖排时一张卡是一整行,宽度管够,所以主要跟着高度走。
        // 竖排的一行按 420 × 56 设计。行高是这里真正变的那一维(行数固定,面板越高行越高),
        // 所以让高度主导:取两维里大的那个,再被宽度那维兜住不超过它的 2 倍,
        // 窄而高的格子里字才会跟着变大又不顶到边。放不下的那一步由 planText 缩字号兜底。
        const qreal k = columns
                            ? theme::paneScale(box.size(), QSizeF(kCardDesignWidth, kCardDesignHeight), 0.6)
                            : std::min(box.height() / 56.0, std::max<qreal>(1.0, box.width() / 420.0) * 2.0);
        // 左侧竖条:第 1 张是点缀色并发光。
        const qreal inset = std::min<qreal>(10 * k, box.height() / 4);
        const QRectF bar(box.left(), box.top() + inset, std::max<qreal>(3, 3 * k), box.height() - 2 * inset);
        if (top)
        {
            painter.fillRect(bar.adjusted(-2, -2, 2, 2), QColor(142, 210, 242, 50));
            painter.fillRect(bar, theme::kCyan);
        }
        else
        {
            painter.fillRect(bar, theme::kTextMuted);
        }

        const qreal pad = std::max<qreal>(8, 16 * k);
        const int rank_px = std::max(theme::kMinNumberPx, static_cast<int>(std::lround((size_ == 0 ? 20 : 24) * k)));
        const int tag_px = std::max(theme::kMinLabelPx, static_cast<int>(std::lround(12 * k)));
        QFont tag_font = theme::labelPx(tag_px, QFont::Bold);
        tag_font.setLetterSpacing(QFont::AbsoluteSpacing, 1);
        const qreal tag_w = QFontMetricsF(tag_font).horizontalAdvance(card.category) + 12 * k;
        const qreal tag_h = std::lround(19 * k);
        const qreal rank_w = rank_px * 0.9;

        if (columns)
        {
            // 并排:排名和分类标签在上面一行,标题和原因在下面。两部分作为一块在卡片里居中,
            // 卡片变高时一起往中间走,不会头贴着顶、字贴着底,中间裂开一条空带。
            const qreal left = box.left() + pad;
            const qreal right = box.right() - pad * 0.75;
            const qreal margin = std::max<qreal>(6, 10 * k);
            const qreal head_gap = std::max<qreal>(4, 8 * k);
            const qreal room = box.height() - 2 * margin;
            const qreal used = tag_h + head_gap + textHeight(card, right - left, k);
            const qreal head_top = box.top() + margin + std::max<qreal>(0, (room - used) / 2);
            theme::paintMetalText(painter, QRectF(left, head_top, rank_w, tag_h), QString::number(index + 1), rank_px,
                                  Qt::AlignLeft | Qt::AlignVCenter);
            paintTag(painter, QRectF(left + rank_w + 6 * k, head_top, tag_w, tag_h), card.category, tag_font, top);
            const qreal text_top = head_top + tag_h + head_gap;
            paintText(painter, card, QRectF(left, text_top, right - left, std::max<qreal>(1, box.bottom() - margin - text_top)),
                      k, top);
            return;
        }
        // 竖排:排名数字在左边一列,分类标签、标题和原因在右边,一张卡就是一整行。
        const qreal left = box.left() + pad;
        const qreal number_w = std::max(rank_w, 18 * k);
        theme::paintMetalText(painter, QRectF(left, box.top(), number_w, box.height()), QString::number(index + 1),
                              rank_px, Qt::AlignLeft | Qt::AlignVCenter);
        const qreal text_left = left + number_w + 8 * k;
        const qreal text_right = box.right() - pad * 0.75;
        // 行矮到放不下「标签 + 标题 + 原因」三行时先丢标签:标题是那句要做的事,比分类更要紧。
        // 标签跟着文字块一起在行里居中,不钉在行顶 —— 钉在顶上,行一高标签就孤零零浮在上面。
        const bool tag_stacked = box.height() >= tag_h * 3.2;
        const qreal tag_space = tag_stacked ? tag_h + std::max<qreal>(2, 4 * k) : 0;
        const qreal margin = std::max<qreal>(4, 6 * k);
        const QRectF content(text_left, box.top() + margin, text_right - text_left,
                             std::max<qreal>(1, box.height() - 2 * margin));
        const qreal used = tag_space + textHeight(card, content.width(), k);
        const qreal block_top = content.top() + std::max<qreal>(0, (content.height() - used) / 2);
        if (tag_stacked)
            paintTag(painter, QRectF(text_left, block_top, tag_w, tag_h), card.category, tag_font, top);
        paintText(painter, card,
                  QRectF(content.left(), block_top + tag_space, content.width(),
                         std::max<qreal>(1, content.bottom() - block_top - tag_space)),
                  k, top);
    }

    void NextStepPane::paintTag(QPainter& painter, const QRectF& box, const QString& text, const QFont& font,
                                bool top) const
    {
        theme::paintGlass(painter, box, 4, false, top ? QColor(18, 44, 66, 230) : QColor(255, 255, 255, 20),
                          Qt::transparent);
        painter.setFont(font);
        painter.setPen(top ? theme::kAccentText : theme::kTextBright);
        painter.drawText(box, Qt::AlignCenter, text);
    }

    // 标题和原因的字号与行数。字号跟着卡片长大(以前乘了 min(1.0, k),把放大全扔了);
    // 放不下时先缩小,再让原因折成两行,最后才省略 —— 一句话被切掉一半比小一号的字难读得多。
    // 量和画用同一个函数,免得「量出来放得下、画出来放不下」。
    NextStepPane::TextPlan NextStepPane::planText(const NextCard& card, qreal width, qreal height, qreal k) const
    {
        TextPlan plan;
        const int wanted_title = static_cast<int>(std::lround((size_ == 0 ? 15 : size_ == 2 ? 21
                                                                                            : 18) *
                                                              k));
        plan.title_px = theme::fitFontPx(card.title, wanted_title, theme::kMinLabelPx, width, [](int px)
                                         {
        QFont font = theme::labelPx(px, QFont::Bold);
        font.setLetterSpacing(QFont::AbsoluteSpacing, 0.5);
        return font; });
        QFont title_font = theme::labelPx(plan.title_px, QFont::Bold);
        title_font.setLetterSpacing(QFont::AbsoluteSpacing, 0.5);
        plan.title_h = QFontMetricsF(title_font).height();

        const int wanted_reason = static_cast<int>(std::lround((size_ == 2 ? 14 : 12.5) * k));
        const int capped = std::max(theme::kMinLabelPx, std::min(wanted_reason, static_cast<int>(plan.title_px * 0.8)));
        // 一行放不下时允许用两行的宽度来量,也就是「可以折行」。
        plan.reason_px = theme::fitFontPx(card.reason, capped, theme::kMinLabelPx, width * 2,
                                          [](int px)
                                          { return theme::labelPx(px); });
        const QFontMetricsF reason_metrics(theme::labelPx(plan.reason_px));
        const qreal room = height - plan.title_h;
        plan.show_reason = room >= reason_metrics.height();
        plan.reason_lines = reason_metrics.horizontalAdvance(card.reason) <= width
                                ? 1
                                : (room >= reason_metrics.height() * 2 ? 2 : 1);
        plan.reason_h = reason_metrics.height() * plan.reason_lines;
        plan.gap = plan.show_reason ? std::max<qreal>(2, 5 * k) : 0;
        return plan;
    }

    qreal NextStepPane::textHeight(const NextCard& card, qreal width, qreal k) const
    {
        // 量的时候给足高度:这里要的是「这块字想要多高」,不是「挤进多高」。
        const TextPlan plan = planText(card, width, kCardDesignHeight * theme::kMaxPaneScale, k);
        return plan.title_h + (plan.show_reason ? plan.gap + plan.reason_h : 0);
    }

    void NextStepPane::paintText(QPainter& painter, const NextCard& card, const QRectF& box, qreal k, bool top) const
    {
        if (box.height() <= 0 || box.width() <= 0)
            return;
        const TextPlan plan = planText(card, box.width(), box.height(), k);
        QFont title_font = theme::labelPx(plan.title_px, QFont::Bold);
        title_font.setLetterSpacing(QFont::AbsoluteSpacing, 0.5);
        const QFontMetricsF title_metrics(title_font);

        // 标题和原因作为一块在卡片里居中:卡片变高时两行一起往中间走,不会一行贴顶一行贴底。
        const qreal block_h = plan.title_h + (plan.show_reason ? plan.gap + plan.reason_h : 0);
        const qreal y = box.top() + std::max<qreal>(0, (box.height() - block_h) / 2);

        painter.setFont(title_font);
        painter.setPen(Qt::white);
        painter.drawText(QRectF(box.left(), y, box.width(), plan.title_h), Qt::AlignLeft | Qt::AlignVCenter,
                         title_metrics.elidedText(card.title, Qt::ElideRight, box.width()));
        if (!plan.show_reason)
            return;
        const QFont reason_font = theme::labelPx(plan.reason_px);
        painter.setFont(reason_font);
        painter.setPen(top ? theme::kTextBright : theme::kTextSecondary);
        const QRectF reason_box(box.left(), y + plan.title_h + plan.gap, box.width(), plan.reason_h);
        if (plan.reason_lines > 1)
        {
            painter.drawText(reason_box, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, card.reason);
            return;
        }
        painter.drawText(reason_box, Qt::AlignLeft | Qt::AlignVCenter,
                         QFontMetricsF(reason_font).elidedText(card.reason, Qt::ElideRight, box.width()));
    }

} // namespace rm_terminal
