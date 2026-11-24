#pragma once

#include "layout_profile.h"

#include <optional>
#include <vector>

namespace rm_terminal {

// 界面设置页里改布局的几种动作。都只改格子(16 × 9),改完画面仍然铺满、互不重叠;
// 做不到(会小于某块的最小尺寸)就返回空,原样不动。

// 把一块空出来的矩形交给旁边的面板:哪一边贴着它的面板正好把这条边铺满,这几块就一起长过去。
// level 0 先不动地图、也不让花名册横着长(花名册拉宽很难看);1 可以动地图;2 谁都可以。补上了返回 true。
bool close_hole(std::vector<PaneSlot>* cells, const QRect& hole, int level);
// 依次放宽 level 0~2,直到补上。
bool close_hole_any(std::vector<PaneSlot>* cells, const QRect& hole);
// 所有没有面板的格子都交给旁边的面板:先整块整块地补(close_hole),补不上的再一格一格长过去。
// 边对不齐、谁都长不过去的格子留空。
void fill_empty_cells(std::vector<PaneSlot>* cells);

// 拿掉一块,它的地方由旁边的面板补上。
std::vector<PaneSlot> layout_remove(std::vector<PaneSlot> cells, PaneId pane);

// 松手落在哪:正中间是换位置,四条边是插到那一边(那一块让出地方)。
enum class DropSide { Center, Left, Right, Top, Bottom };

// 把 pane(画面里的或者组件库里新拖来的)放到 target 上。
// - 画面里的放到 Center:两块换位置,大小也跟着换。
// - 新来的放到 Center:插到 target 长的那一边的后半截。
// - 放到边上:target 让出那一边,pane 占那条;pane 原来的地方由旁边补上。
std::optional<std::vector<PaneSlot>> layout_drop(std::vector<PaneSlot> cells, PaneId pane, PaneId target, DropSide side);

// 用 pane 换掉 target:pane 占 target 那一整块,target 从画面里拿掉(回到组件库)。pane 本来就在画面里的话,
// 它原来的地方由旁边补上。pane 放不进那一块(小于最小尺寸)就返回空。
std::optional<std::vector<PaneSlot>> layout_replace(std::vector<PaneSlot> cells, PaneId pane, PaneId target);

// 两块面板中间的缝。vertical:竖缝,line 是第几条列线(1~15),[from, to) 是它管的行;
// 横缝时 line 是行线、[from, to) 是列。拖这条缝时,贴着它两边的面板一起变。
struct Seam {
    bool vertical = true;
    int line = 0;
    int from = 0;
    int to = 0;
    bool operator==(const Seam& other) const {
        return vertical == other.vertical && line == other.line && from == other.from && to == other.to;
    }
    bool operator!=(const Seam& other) const { return !(*this == other); }
};

// 画面上所有能拖的缝(画把手用)。
std::vector<Seam> all_seams(const std::vector<PaneSlot>& cells);
// (x, y) 是以格为单位的位置;离某条缝不到 tolerance 格就算按在缝上。
std::optional<Seam> seam_at(const std::vector<PaneSlot>& cells, double x, double y, double tolerance);
// 把缝挪到 line。会让某块小于最小尺寸就返回空。
std::optional<std::vector<PaneSlot>> layout_move_seam(std::vector<PaneSlot> cells, const Seam& seam, int line);

}  // namespace rm_terminal
