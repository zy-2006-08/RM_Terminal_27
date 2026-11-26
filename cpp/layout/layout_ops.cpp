#include "layout_ops.h"

#include <algorithm>
#include <cmath>

namespace rm_terminal
{

    namespace
    {

        void set_units(PaneSlot* slot, const QRect& units)
        {
            slot->column = units.left();
            slot->row = units.top();
            slot->column_span = units.width();
            slot->row_span = units.height();
        }

        std::vector<PaneSlot>::iterator find_pane(std::vector<PaneSlot>& cells, PaneId pane)
        {
            return std::find_if(cells.begin(), cells.end(), [pane](const PaneSlot& slot)
                                { return slot.pane == pane; });
        }

        bool fits(PaneId pane, const QRect& units)
        {
            const QSize min = pane_min_units(pane);
            return units.width() >= min.width() && units.height() >= min.height();
        }

        // 贴着缝的面板(缝在它的右边/下边是 before,左边/上边是 after),只算和 [from, to) 有重叠的。
        struct Sides
        {
            std::vector<std::size_t> before;
            std::vector<std::size_t> after;
            bool crossed = false;
        };

        Sides sides_of(const std::vector<PaneSlot>& cells, const Seam& seam)
        {
            Sides out;
            for (std::size_t i = 0; i < cells.size(); ++i)
            {
                const PaneSlot& s = cells[i];
                const int start = seam.vertical ? s.column : s.row;
                const int end = start + (seam.vertical ? s.column_span : s.row_span);
                const int across = seam.vertical ? s.row : s.column;
                const int across_end = across + (seam.vertical ? s.row_span : s.column_span);
                if (across_end <= seam.from || across >= seam.to)
                    continue;
                if (end == seam.line)
                    out.before.push_back(i);
                else if (start == seam.line)
                    out.after.push_back(i);
                else if (start < seam.line && end > seam.line)
                    out.crossed = true;
            }
            return out;
        }

        // 从按下的那一格出发,把贴着缝的面板的上下(左右)边都包进来,直到不再变大:
        // 这样挪缝时每块都整块地变,不会被切开。
        std::optional<Seam> close_seam(const std::vector<PaneSlot>& cells, Seam seam)
        {
            for (int guard = 0; guard < 32; ++guard)
            {
                const Sides sides = sides_of(cells, seam);
                if (sides.crossed || sides.before.empty() || sides.after.empty())
                    return std::nullopt;
                Seam grown = seam;
                for (const std::size_t i : sides.before)
                {
                    const PaneSlot& s = cells[i];
                    grown.from = std::min(grown.from, seam.vertical ? s.row : s.column);
                    grown.to = std::max(grown.to, seam.vertical ? s.row + s.row_span : s.column + s.column_span);
                }
                for (const std::size_t i : sides.after)
                {
                    const PaneSlot& s = cells[i];
                    grown.from = std::min(grown.from, seam.vertical ? s.row : s.column);
                    grown.to = std::max(grown.to, seam.vertical ? s.row + s.row_span : s.column + s.column_span);
                }
                if (grown == seam)
                    return seam;
                seam = grown;
            }
            return std::nullopt;
        }

    } // namespace

    bool close_hole(std::vector<PaneSlot>* cells, const QRect& hole, int level)
    {
        enum Side
        {
            Left,
            Right,
            Top,
            Bottom
        };
        for (const Side side : {Left, Right, Top, Bottom})
        {
            const bool across = side == Left || side == Right;
            std::vector<PaneSlot*> touching;
            int covered = 0;
            bool exact = true;
            for (PaneSlot& s : *cells)
            {
                const QRect u = s.units();
                const bool adjacent = side == Left    ? u.left() + u.width() == hole.left()
                                      : side == Right ? u.left() == hole.left() + hole.width()
                                      : side == Top   ? u.top() + u.height() == hole.top()
                                                      : u.top() == hole.top() + hole.height();
                if (!adjacent)
                    continue;
                const int a0 = across ? u.top() : u.left();
                const int a1 = a0 + (across ? u.height() : u.width());
                const int h0 = across ? hole.top() : hole.left();
                const int h1 = h0 + (across ? hole.height() : hole.width());
                if (a1 <= h0 || a0 >= h1)
                    continue;
                if (a0 < h0 || a1 > h1)
                    exact = false;
                touching.push_back(&s);
                covered += a1 - a0;
            }
            if (!exact || touching.empty() || covered != (across ? hole.height() : hole.width()))
                continue;
            if (std::any_of(touching.begin(), touching.end(), [across, level](const PaneSlot* s)
                            {
                const bool roster_sideways = across && (s->pane == PaneId::AllyRoster || s->pane == PaneId::EnemyRoster);
                return (level < 1 && s->pane == PaneId::Map) || (level < 2 && roster_sideways); }))
                continue;
            for (PaneSlot* s : touching)
            {
                if (side == Left)
                    s->column_span += hole.width();
                if (side == Right)
                {
                    s->column -= hole.width();
                    s->column_span += hole.width();
                }
                if (side == Top)
                    s->row_span += hole.height();
                if (side == Bottom)
                {
                    s->row -= hole.height();
                    s->row_span += hole.height();
                }
            }
            return true;
        }
        return false;
    }

    bool close_hole_any(std::vector<PaneSlot>* cells, const QRect& hole)
    {
        for (int level = 0; level <= 2; ++level)
            if (close_hole(cells, hole, level))
                return true;
        return false;
    }

    void fill_empty_cells(std::vector<PaneSlot>* cells)
    {
        std::vector<char> used(static_cast<std::size_t>(kGridColumns * kGridRows), 0);
        const auto refresh = [&]
        {
            std::fill(used.begin(), used.end(), 0);
            for (const PaneSlot& s : *cells)
                for (int r = s.row; r < s.row + s.row_span; ++r)
                    for (int c = s.column; c < s.column + s.column_span; ++c)
                        if (r >= 0 && r < kGridRows && c >= 0 && c < kGridColumns)
                            used[static_cast<std::size_t>(r * kGridColumns + c)] = 1;
        };
        const auto free_at = [&](int c, int r)
        { return !used[static_cast<std::size_t>(r * kGridColumns + c)]; };
        // 先按整块补:从左上角第一格空的开始,往右、再往下扩成最大的空矩形,交给旁边的面板。
        for (int level = 0; level <= 2; ++level)
            for (bool progress = true; progress;)
            {
                progress = false;
                refresh();
                for (int r = 0; r < kGridRows && !progress; ++r)
                    for (int c = 0; c < kGridColumns && !progress; ++c)
                    {
                        if (!free_at(c, r))
                            continue;
                        int w = 1;
                        while (c + w < kGridColumns && free_at(c + w, r))
                            ++w;
                        int h = 1;
                        for (bool grow = true; grow && r + h < kGridRows;)
                        {
                            for (int x = c; x < c + w; ++x)
                                grow = grow && free_at(x, r + h);
                            if (grow)
                                ++h;
                        }
                        if (close_hole(cells, QRect(c, r, w, h), level))
                            progress = true;
                    }
            }
        // 剩下的:一格一格长过去。
        refresh();
        const auto mark = [&](int column, int row, int width, int height)
        {
            for (int r = row; r < row + height; ++r)
                for (int c = column; c < column + width; ++c)
                    used[static_cast<std::size_t>(r * kGridColumns + c)] = 1;
        };
        const auto bare = [&](int column, int row, int width, int height)
        {
            if (column < 0 || row < 0 || column + width > kGridColumns || row + height > kGridRows)
                return false;
            for (int r = row; r < row + height; ++r)
                for (int c = column; c < column + width; ++c)
                    if (used[static_cast<std::size_t>(r * kGridColumns + c)])
                        return false;
            return true;
        };
        for (bool grew = true; grew;)
        {
            grew = false;
            for (PaneSlot& s : *cells)
            {
                if (bare(s.column + s.column_span, s.row, 1, s.row_span))
                {
                    mark(s.column + s.column_span, s.row, 1, s.row_span);
                    ++s.column_span;
                }
                else if (bare(s.column - 1, s.row, 1, s.row_span))
                {
                    mark(s.column - 1, s.row, 1, s.row_span);
                    --s.column;
                    ++s.column_span;
                }
                else if (bare(s.column, s.row + s.row_span, s.column_span, 1))
                {
                    mark(s.column, s.row + s.row_span, s.column_span, 1);
                    ++s.row_span;
                }
                else if (bare(s.column, s.row - 1, s.column_span, 1))
                {
                    mark(s.column, s.row - 1, s.column_span, 1);
                    --s.row;
                    ++s.row_span;
                }
                else
                {
                    continue;
                }
                grew = true;
            }
        }
    }

    std::vector<PaneSlot> layout_remove(std::vector<PaneSlot> cells, PaneId pane)
    {
        const auto it = find_pane(cells, pane);
        if (it == cells.end())
            return cells;
        const QRect hole = it->units();
        cells.erase(it);
        if (!close_hole_any(&cells, hole))
            fill_empty_cells(&cells);
        return cells;
    }

    std::optional<std::vector<PaneSlot>> layout_drop(std::vector<PaneSlot> cells, PaneId pane, PaneId target,
                                                     DropSide side)
    {
        if (pane == target)
            return std::nullopt;
        std::optional<QRect> old;
        if (const auto it = find_pane(cells, pane); it != cells.end())
        {
            old = it->units();
            cells.erase(it);
        }
        if (find_pane(cells, target) == cells.end())
            return std::nullopt;
        if (side == DropSide::Center)
        {
            const auto t = find_pane(cells, target);
            const QRect tr = t->units();
            if (old)
            {
                if (!fits(pane, tr) || !fits(target, *old))
                    return std::nullopt;
                set_units(&*t, *old);
                cells.push_back({pane, tr.top(), tr.left(), tr.height(), tr.width()});
                return cells;
            }
            side = tr.width() >= tr.height() ? DropSide::Right : DropSide::Bottom;
        }
        // 原来的地方先交给旁边的面板,再插:挨着的两块(比如图传就在本车下面),本车先长过来才分得出地方。
        if (old && !close_hole_any(&cells, *old))
            fill_empty_cells(&cells);
        const auto t = find_pane(cells, target);
        const QRect tr = t->units();
        const bool across = side == DropSide::Left || side == DropSide::Right;
        const int span = across ? tr.width() : tr.height();
        const int keep = across ? pane_min_units(target).width() : pane_min_units(target).height();
        const int need = across ? pane_min_units(pane).width() : pane_min_units(pane).height();
        const int other = across ? tr.height() : tr.width();
        const int other_need = across ? pane_min_units(pane).height() : pane_min_units(pane).width();
        if (span - keep < need || other < other_need)
            return std::nullopt;
        const QSize wanted = old ? old->size() : pane_default_units(pane);
        const int k = std::clamp(across ? wanted.width() : wanted.height(), need, span - keep);
        QRect placed;
        QRect rest;
        switch (side)
        {
        case DropSide::Left:
            placed = QRect(tr.left(), tr.top(), k, tr.height());
            rest = QRect(tr.left() + k, tr.top(), tr.width() - k, tr.height());
            break;
        case DropSide::Right:
            placed = QRect(tr.left() + tr.width() - k, tr.top(), k, tr.height());
            rest = QRect(tr.left(), tr.top(), tr.width() - k, tr.height());
            break;
        case DropSide::Top:
            placed = QRect(tr.left(), tr.top(), tr.width(), k);
            rest = QRect(tr.left(), tr.top() + k, tr.width(), tr.height() - k);
            break;
        case DropSide::Bottom:
        case DropSide::Center:
            placed = QRect(tr.left(), tr.top() + tr.height() - k, tr.width(), k);
            rest = QRect(tr.left(), tr.top(), tr.width(), tr.height() - k);
            break;
        }
        set_units(&*t, rest);
        cells.push_back({pane, placed.top(), placed.left(), placed.height(), placed.width()});
        return cells;
    }

    std::optional<std::vector<PaneSlot>> layout_replace(std::vector<PaneSlot> cells, PaneId pane, PaneId target)
    {
        if (pane == target)
            return std::nullopt;
        std::optional<QRect> old;
        if (const auto it = find_pane(cells, pane); it != cells.end())
        {
            old = it->units();
            cells.erase(it);
        }
        const auto t = find_pane(cells, target);
        if (t == cells.end() || !fits(pane, t->units()))
            return std::nullopt;
        t->pane = pane;
        if (old && !close_hole_any(&cells, *old))
            fill_empty_cells(&cells);
        return cells;
    }

    std::vector<Seam> all_seams(const std::vector<PaneSlot>& cells)
    {
        std::vector<Seam> out;
        for (const bool vertical : {true, false})
        {
            const int lines = vertical ? kGridColumns : kGridRows;
            const int cross = vertical ? kGridRows : kGridColumns;
            for (int line = 1; line < lines; ++line)
                for (int at = 0; at < cross; ++at)
                {
                    const auto seam = close_seam(cells, {vertical, line, at, at + 1});
                    if (seam && std::find(out.begin(), out.end(), *seam) == out.end())
                        out.push_back(*seam);
                }
        }
        return out;
    }

    std::optional<Seam> seam_at(const std::vector<PaneSlot>& cells, double x, double y, double tolerance)
    {
        std::optional<Seam> best;
        double best_distance = tolerance;
        for (const bool vertical : {true, false})
        {
            const double along = vertical ? x : y;
            const int line = static_cast<int>(std::lround(along));
            const double distance = std::abs(along - line);
            if (distance > best_distance || line <= 0 || line >= (vertical ? kGridColumns : kGridRows))
                continue;
            const int at = static_cast<int>(std::floor(vertical ? y : x));
            if (at < 0 || at >= (vertical ? kGridRows : kGridColumns))
                continue;
            if (const auto seam = close_seam(cells, {vertical, line, at, at + 1}))
            {
                best = seam;
                best_distance = distance;
            }
        }
        return best;
    }

    std::optional<std::vector<PaneSlot>> layout_move_seam(std::vector<PaneSlot> cells, const Seam& seam, int line)
    {
        const int delta = line - seam.line;
        if (delta == 0)
            return cells;
        const Sides sides = sides_of(cells, seam);
        if (sides.crossed)
            return std::nullopt;
        for (const std::size_t i : sides.before)
        {
            PaneSlot& s = cells[i];
            (seam.vertical ? s.column_span : s.row_span) += delta;
            if (!fits(s.pane, s.units()))
                return std::nullopt;
        }
        for (const std::size_t i : sides.after)
        {
            PaneSlot& s = cells[i];
            (seam.vertical ? s.column : s.row) += delta;
            (seam.vertical ? s.column_span : s.row_span) -= delta;
            if (!fits(s.pane, s.units()))
                return std::nullopt;
        }
        return cells;
    }

} // namespace rm_terminal
