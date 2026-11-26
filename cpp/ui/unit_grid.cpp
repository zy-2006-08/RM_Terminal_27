#include "unit_grid.h"

#include <QWidget>
#include <algorithm>
#include <cmath>

namespace rm_terminal
{

    namespace
    {

        // 第 k 条格线(第 k 列的左边)离区域左边多远。前一半从左往右取整,后一半从右往左取整,
        // 这样左右对称的两块一样宽。
        int boundary(int k, int count, int length, int spacing)
        {
            const double pitch = (length - (count - 1) * spacing) / static_cast<double>(count) + spacing;
            if (2 * k <= count)
                return static_cast<int>(std::floor(k * pitch + 0.5));
            return length + spacing - static_cast<int>(std::floor((count - k) * pitch + 0.5));
        }

    } // namespace

    QRect unit_rect(const QRect& area, int columns, int rows, int spacing, const QRect& units)
    {
        const int left = area.left() + boundary(units.left(), columns, area.width(), spacing);
        const int right = area.left() + boundary(units.left() + units.width(), columns, area.width(), spacing) - spacing;
        const int top = area.top() + boundary(units.top(), rows, area.height(), spacing);
        const int bottom = area.top() + boundary(units.top() + units.height(), rows, area.height(), spacing) - spacing;
        return QRect(left, top, std::max(0, right - left), std::max(0, bottom - top));
    }

    QPoint unit_at(const QRect& area, int columns, int rows, int spacing, const QPoint& pixel)
    {
        const double pitch_x = (area.width() + spacing) / static_cast<double>(columns);
        const double pitch_y = (area.height() + spacing) / static_cast<double>(rows);
        const int column = static_cast<int>(std::floor((pixel.x() - area.left()) / pitch_x));
        const int row = static_cast<int>(std::floor((pixel.y() - area.top()) / pitch_y));
        return QPoint(std::clamp(column, 0, columns - 1), std::clamp(row, 0, rows - 1));
    }

    UnitGridLayout::UnitGridLayout(QWidget* parent, int columns, int rows, int spacing)
        : QLayout(parent), columns_(columns), rows_(rows)
    {
        setSpacing(spacing);
        setContentsMargins(0, 0, 0, 0);
    }

    UnitGridLayout::~UnitGridLayout()
    {
        for (Entry& entry : entries_)
            delete entry.item;
    }

    void UnitGridLayout::addWidget(QWidget* widget, const QRect& units)
    {
        addChildWidget(widget);
        entries_.push_back({new QWidgetItem(widget), units});
        invalidate();
    }

    void UnitGridLayout::clearCells()
    {
        for (Entry& entry : entries_)
            delete entry.item;
        entries_.clear();
        invalidate();
    }

    void UnitGridLayout::addItem(QLayoutItem* item) { entries_.push_back({item, QRect(0, 0, 1, 1)}); }

    QLayoutItem* UnitGridLayout::itemAt(int index) const
    {
        return index >= 0 && index < static_cast<int>(entries_.size()) ? entries_[static_cast<std::size_t>(index)].item
                                                                       : nullptr;
    }

    QLayoutItem* UnitGridLayout::takeAt(int index)
    {
        if (index < 0 || index >= static_cast<int>(entries_.size()))
            return nullptr;
        QLayoutItem* item = entries_[static_cast<std::size_t>(index)].item;
        entries_.erase(entries_.begin() + index);
        return item;
    }

    int UnitGridLayout::count() const { return static_cast<int>(entries_.size()); }

    QSize UnitGridLayout::sizeHint() const { return QSize(1424, 742); }

    // 每格至少 24 像素:比这更小的窗口没有意义,也不该让面板的最小尺寸反过来把窗口撑大。
    QSize UnitGridLayout::minimumSize() const
    {
        return QSize(columns_ * 24 + (columns_ - 1) * spacing(), rows_ * 24 + (rows_ - 1) * spacing());
    }

    void UnitGridLayout::setGeometry(const QRect& rect)
    {
        QLayout::setGeometry(rect);
        for (const Entry& entry : entries_)
        {
            const QRect box = unit_rect(rect, columns_, rows_, spacing(), entry.units);
            // 直接给控件定几何:经过 QWidgetItem 会按面板的大小策略截成它想要的大小。
            if (QWidget* widget = entry.item->widget())
                widget->setGeometry(box);
            else
                entry.item->setGeometry(box);
        }
    }

} // namespace rm_terminal
