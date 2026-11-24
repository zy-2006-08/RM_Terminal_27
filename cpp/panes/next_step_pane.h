#pragma once

#include "next_step.h"

#include <QWidget>
#include <vector>

namespace rm_terminal {

// 「下一步」最多三张卡(效果图 7):排名数字、分类小标签、一句动词开头的话、带数字的原因。
// 第 1 张用点缀色高亮。只画真有的事,几张就把面板分成几份;没有要紧的事时把那句说明
// 画大居中,不留空白卡。面板扁就并排、窄或高就竖排,字号跟着卡片变大变小。
class NextStepPane : public QWidget {
public:
    explicit NextStepPane(QWidget* parent = nullptr);

    void setCards(const std::vector<NextCard>& cards);
    const std::vector<NextCard>& cards() const { return cards_; }
    // 0 紧凑 1 标准 2 大。
    void setCardSize(int size);
    // 最多显示几件事。显示几件就把面板分成几份,没有空格子。
    void setCapacity(int capacity);

    // 当前尺寸下这么多张卡是并排(true)还是竖着一行一张(false)。
    bool sideBySide(int count) const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    // 一张卡里那两行字的排版结果:量和画共用,免得两边算得不一样。
    struct TextPlan {
        int title_px = 0;
        int reason_px = 0;
        qreal title_h = 0;
        qreal reason_h = 0;
        qreal gap = 0;
        int reason_lines = 1;
        bool show_reason = false;
    };

    void paintEmpty(QPainter& painter) const;
    void paintCard(QPainter& painter, const NextCard& card, const QRectF& box, int index, bool columns) const;
    void paintTag(QPainter& painter, const QRectF& box, const QString& text, const QFont& font, bool top) const;
    void paintText(QPainter& painter, const NextCard& card, const QRectF& box, qreal scale, bool top) const;
    TextPlan planText(const NextCard& card, qreal width, qreal height, qreal scale) const;
    qreal textHeight(const NextCard& card, qreal width, qreal scale) const;

    // 「没有要紧的事」那句话的字号只跟面板大小有关:算一次存下来,重画时直接用。
    struct EmptyPlan {
        QSize size;
        int px = 0;
        bool wrap = false;
    };
    static constexpr int kRefPx = 100;

    std::vector<NextCard> cards_;
    int size_ = 1;
    int capacity_ = 3;
    mutable EmptyPlan empty_plan_;
};

}  // namespace rm_terminal
