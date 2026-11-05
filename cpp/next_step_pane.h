#pragma once

#include "next_step.h"

#include <QWidget>
#include <vector>

namespace rm_terminal {

// 「下一步」三张卡(效果图 7):排名数字、分类小标签、一句动词开头的话、带数字的原因。
// 第 1 张用点缀色高亮。没有要紧的事时写一行说明,不留空白卡。
class NextStepPane : public QWidget {
public:
    explicit NextStepPane(QWidget* parent = nullptr);

    void setCards(const std::vector<NextCard>& cards);
    const std::vector<NextCard>& cards() const { return cards_; }
    // 0 紧凑 1 标准 2 大。
    void setCardSize(int size);
    // 卡片的格数:显示几件就分几格,少了的那几格留空。
    void setCapacity(int capacity);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    std::vector<NextCard> cards_;
    int size_ = 1;
    int capacity_ = 3;
};

}  // namespace rm_terminal
