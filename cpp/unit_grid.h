#pragma once

#include <QLayout>
#include <QRect>
#include <vector>

namespace rm_terminal {

// 格子在像素里的位置。每一列一样宽,格子之间留 spacing;左右两半按镜像取整,两侧对称的
// 面板(我方 / 敌方花名册)永远一样宽。units 的 x/y 是第几列第几行,宽高是占几格。
QRect unit_rect(const QRect& area, int columns, int rows, int spacing, const QRect& units);
// 像素点落在哪一格(夹在格子范围内)。
QPoint unit_at(const QRect& area, int columns, int rows, int spacing, const QPoint& pixel);

// 信息页的布局:每个面板占几格就是几格,不看面板自己想要多大。格子小于面板的最小尺寸时
// 面板里面挤一点、裁一点,但永远不会压到旁边的面板上。
class UnitGridLayout final : public QLayout {
public:
    UnitGridLayout(QWidget* parent, int columns, int rows, int spacing);
    ~UnitGridLayout() override;

    void addWidget(QWidget* widget, const QRect& units);
    // 把面板都摘下来(不删控件)。
    void clearCells();

    void addItem(QLayoutItem* item) override;
    QLayoutItem* itemAt(int index) const override;
    QLayoutItem* takeAt(int index) override;
    int count() const override;
    QSize sizeHint() const override;
    QSize minimumSize() const override;
    void setGeometry(const QRect& rect) override;
    Qt::Orientations expandingDirections() const override { return Qt::Horizontal | Qt::Vertical; }

    int columns() const { return columns_; }
    int rows() const { return rows_; }

private:
    struct Entry {
        QLayoutItem* item;
        QRect units;
    };
    std::vector<Entry> entries_;
    int columns_;
    int rows_;
};

}  // namespace rm_terminal
