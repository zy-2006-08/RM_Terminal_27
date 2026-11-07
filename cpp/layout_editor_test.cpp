#include "layout_editor.h"
#include "ui_profile.h"
#include "ui_profile_store.h"

#include <QApplication>
#include <QMouseEvent>
#include <QPushButton>
#include <QTemporaryDir>
#include <QWheelEvent>
#include <iostream>
#include <stdexcept>

using namespace rm_terminal;

namespace {

void check(bool condition, const char* name) {
    if (!condition) throw std::runtime_error(name);
    std::cout << "PASS " << name << '\n';
}

void send(QWidget* widget, QEvent::Type type, const QPoint& pos, Qt::MouseButton button, Qt::MouseButtons buttons) {
    QMouseEvent event(type, QPointF(pos), widget->mapToGlobal(QPointF(pos)), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(widget, &event);
}

void click(QWidget* widget, const QPoint& pos) {
    send(widget, QEvent::MouseButtonPress, pos, Qt::LeftButton, Qt::LeftButton);
    send(widget, QEvent::MouseButtonRelease, pos, Qt::LeftButton, Qt::NoButton);
}

// 和真鼠标一样:按下、分几步挪过去、松开。
void drag(QWidget* widget, const QPoint& from, const QPoint& to) {
    send(widget, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
    for (int i = 1; i <= 8; ++i) send(widget, QEvent::MouseMove, from + (to - from) * i / 8, Qt::NoButton, Qt::LeftButton);
    send(widget, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
}

void wheel(QWidget* widget, const QPoint& pos, int dy) {
    QWheelEvent event(QPointF(pos), widget->mapToGlobal(QPointF(pos)), QPoint(), QPoint(0, dy), Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(widget, &event);
}

void settle() {
    for (int i = 0; i < 3; ++i) QApplication::processEvents();
}

QRect saved(const UiProfileController& profiles, PaneId pane) {
    for (const PaneSlot& slot : scheme_layout(profiles.role(), profiles.activeScheme()).cells)
        if (slot.pane == pane) return slot.units();
    return {};
}

bool tiles_the_grid(const std::vector<PaneSlot>& cells) {
    std::vector<int> owner(static_cast<std::size_t>(kGridColumns * kGridRows), 0);
    for (const PaneSlot& slot : cells)
        for (int r = slot.row; r < slot.row + slot.row_span; ++r)
            for (int c = slot.column; c < slot.column + slot.column_span; ++c)
                if (++owner[static_cast<std::size_t>(r * kGridColumns + c)] > 1) return false;
    return std::all_of(owner.begin(), owner.end(), [](int n) { return n == 1; });
}

// 一页的界面设置(效果图 10~13)。每一下都走和鼠标一样的判断,改完马上存,能一步步撤销。
void one_page_settings() {
    QTemporaryDir directory;
    UiProfileController profiles{UiProfileRepository(directory.path()), Role::Infantry};
    LayoutEditor editor(&profiles);
    editor.resize(1440, 900);
    editor.show();
    settle();
    editor.begin();
    // 主界面截图:整个窗口 1440 × 900,格子区和真主界面一样在 (8, 128) 起。
    QPixmap window(1440, 900);
    window.fill(Qt::black);
    editor.setPreview(window, QRect(8, 128, 1424, 764));
    settle();
    const auto centre = [&](PaneId pane) { return editor.tileRect(pane).center(); };
    const auto box = [&](PaneId pane) {
        for (const PaneSlot& slot : editor.cells())
            if (slot.pane == pane) return editor.unitToCanvas(slot.units());
        return QRect();
    };
    check(tiles_the_grid(editor.cells()), "the page starts from the screen as it is");
    check(!editor.tileRect(PaneId::NextStep).isNull() && !editor.tileRect(PaneId::Timeline).isNull() &&
              editor.tileRect(PaneId::Map).isNull(),
          "the library lists only what is not on the screen");

    // 删掉:旁边的补上,提示条能撤销。
    click(&editor, editor.closeRect(PaneId::Events).center());
    settle();
    check(saved(profiles, PaneId::Events).isNull() && tiles_the_grid(editor.cells()),
          "the cross removes a pane and the neighbours fill its place");
    check(editor.toastText().contains(QStringLiteral("已删掉「战场事件」")), "the bar says what was removed");
    check(!editor.tileRect(PaneId::Events).isNull(), "the removed pane is back in the library");
    editor.undo();
    settle();
    check(saved(profiles, PaneId::Events) == QRect(12, 4, 4, 5), "undo puts it back where it was");
    check(!editor.canUndo(), "and nothing is left to undo");

    // 地图、花名册也能删。
    click(&editor, editor.closeRect(PaneId::Map).center());
    settle();
    check(saved(profiles, PaneId::Map).isNull() && tiles_the_grid(editor.cells()), "even the map can be removed");
    editor.undo();
    settle();

    // 拖缝:两边一起变,拖不过最小尺寸。
    const QRect ally = box(PaneId::AllyRoster);
    const QRect analysis = box(PaneId::Analysis);
    const QPoint seam((ally.right() + analysis.left()) / 2, analysis.center().y());
    drag(&editor, seam, QPoint(editor.unitToCanvas(QRect(5, 0, 1, 1)).left() - 2, seam.y()));
    settle();
    check(saved(profiles, PaneId::AllyRoster) == QRect(0, 0, 5, 7) && saved(profiles, PaneId::Analysis) == QRect(5, 0, 7, 4),
          "dragging the seam makes one side wider and the other narrower");
    const QRect wide = box(PaneId::AllyRoster);
    const QRect narrow = box(PaneId::Analysis);
    drag(&editor, QPoint((wide.right() + narrow.left()) / 2, narrow.center().y()),
         QPoint(editor.unitToCanvas(QRect(10, 0, 1, 1)).left(), narrow.center().y()));
    settle();
    check(saved(profiles, PaneId::Analysis).width() == pane_min_units(PaneId::Analysis).width(),
          "a seam stops where the other side would get too small");
    editor.undo();
    settle();
    check(saved(profiles, PaneId::Analysis) == QRect(5, 0, 7, 4), "each drag is one undo step");
    editor.undo();
    settle();

    // 拖到别的面板边上:插进去,那一块让出地方,原来的位置旁边补上。
    const QRect map = box(PaneId::Map);
    drag(&editor, box(PaneId::Events).center(), QPoint(map.left() + 12, map.center().y()));
    settle();
    check(saved(profiles, PaneId::Events) == QRect(4, 4, 4, 5) && saved(profiles, PaneId::Map) == QRect(8, 4, 4, 5) &&
              saved(profiles, PaneId::EnemyRoster) == QRect(12, 0, 4, 9),
          "dropping on the left edge of the map inserts the pane there");
    editor.undo();
    settle();

    // 放正中间:两块换位置,大小也跟着换。
    drag(&editor, box(PaneId::Events).center(), box(PaneId::Analysis).center());
    settle();
    check(saved(profiles, PaneId::Events) == QRect(4, 0, 8, 4) && saved(profiles, PaneId::Analysis) == QRect(12, 4, 4, 5),
          "dropping in the middle swaps two panes");
    editor.undo();
    settle();

    // 边上插不下(花名册只有 4 列,插地图要 4 + 3 列):两块换位置。
    const QRect roster = box(PaneId::AllyRoster);
    drag(&editor, box(PaneId::Map).center(), QPoint(roster.left() + 8, roster.center().y()));
    settle();
    check(saved(profiles, PaneId::Map) == QRect(0, 0, 4, 7) && saved(profiles, PaneId::AllyRoster) == QRect(4, 4, 8, 5),
          "an edge drop that cannot fit swaps the two panes instead");
    editor.undo();
    settle();

    // 换也换不了(花名册拖窄到 3 列,地图最小 4 列):原样不动,说清楚要多大。
    const QPoint ally_seam((box(PaneId::AllyRoster).right() + box(PaneId::Analysis).left()) / 2,
                           box(PaneId::Analysis).center().y());
    drag(&editor, ally_seam, QPoint(editor.unitToCanvas(QRect(3, 0, 1, 1)).left() - 2, ally_seam.y()));
    settle();
    check(saved(profiles, PaneId::AllyRoster) == QRect(0, 0, 3, 7), "the roster is now three columns wide");
    const QRect map_before = saved(profiles, PaneId::Map);
    drag(&editor, box(PaneId::Map).center(), box(PaneId::AllyRoster).center());
    settle();
    check(saved(profiles, PaneId::Map) == map_before && editor.toastText().contains(QStringLiteral("放不下")) &&
              editor.toastText().contains(QStringLiteral("4 × 3")),
          "a drop that does not fit changes nothing and says how big the pane has to be");
    editor.undo();
    settle();

    // 画面里的拖回组件库:拿掉,和点叉一样能撤销。
    drag(&editor, box(PaneId::Events).center(), centre(PaneId::Timeline));
    settle();
    check(saved(profiles, PaneId::Events).isNull() && tiles_the_grid(editor.cells()) &&
              editor.toastText().contains(QStringLiteral("已删掉「战场事件」")) && !editor.tileRect(PaneId::Events).isNull(),
          "dragging a pane back to the library removes it");
    // 组件库一列放不下就用滚轮翻(再拿掉图传,组件库里就有五个)。
    click(&editor, editor.closeRect(PaneId::Video).center());
    settle();
    check(saved(profiles, PaneId::Video).isNull() && !editor.tileRect(PaneId::Video).isNull(),
          "the video is a pane of its own and can be removed like any other");
    {
        UiProfileController again{UiProfileRepository(directory.path()), Role::Infantry};
        const std::vector<PaneSlot> cells = scheme_layout(Role::Infantry, again.activeScheme()).cells;
        check(std::none_of(cells.begin(), cells.end(), [](const PaneSlot& slot) { return slot.pane == PaneId::Video; }),
              "a removed video stays removed after a restart");
    }
    const QRect timeline_tile = editor.tileRect(PaneId::Timeline);
    const QPoint in_library = editor.tileRect(PaneId::NextStep).center();
    wheel(&editor, in_library, -240);
    check(editor.tileRect(PaneId::Timeline).top() < timeline_tile.top(), "the library scrolls when it is longer than the page");
    wheel(&editor, in_library, 2400);
    check(editor.tileRect(PaneId::Timeline) == timeline_tile, "and scrolls back to the top");
    editor.undo();
    settle();
    editor.undo();
    settle();
    check(saved(profiles, PaneId::Events) == QRect(12, 4, 4, 5) && saved(profiles, PaneId::Video) == QRect(0, 7, 4, 2),
          "undo brings the panes back from the library");

    // 组件库来的放不下就替换,换下来的回到组件库。图传只有 4 × 2,再分不出一块来。
    drag(&editor, centre(PaneId::SelfStatus), box(PaneId::Video).center());
    settle();
    check(saved(profiles, PaneId::SelfStatus) == QRect(0, 7, 4, 2) && saved(profiles, PaneId::Video).isNull() &&
              !editor.tileRect(PaneId::Video).isNull() && editor.toastText().contains(QStringLiteral("换下来了")),
          "a library pane that cannot squeeze in replaces the pane under it");
    editor.undo();
    settle();
    check(saved(profiles, PaneId::Video) == QRect(0, 7, 4, 2), "undo puts the replaced pane back");

    // 「本车」删掉了:画面里没有,组件库里也没有。
    check(saved(profiles, PaneId::SelfCommands).isNull() && editor.tileRect(PaneId::SelfCommands).isNull(),
          "本车 is neither on the page nor in the library");

    // 从组件库拖进来。
    const QRect events = box(PaneId::Events);
    drag(&editor, centre(PaneId::Timeline), QPoint(events.center().x(), events.bottom() - 6));
    settle();
    check(saved(profiles, PaneId::Timeline) == QRect(12, 6, 4, 3) && saved(profiles, PaneId::Events) == QRect(12, 4, 4, 2),
          "a library pane dropped on the bottom edge of a pane takes its bottom part");
    check(editor.tileRect(PaneId::Timeline).isNull(), "and leaves the library");
    editor.undo();
    settle();

    // 齿轮:细项在旁边弹出来,改了马上生效,撤销能改回去。
    check(editor.gearRect(PaneId::Events).isNull() && !editor.gearRect(PaneId::Analysis).isNull(),
          "only panes with details have a gear");
    click(&editor, editor.gearRect(PaneId::Analysis).center());
    settle();
    check(editor.detailsVisible(), "the gear opens the details");
    auto* control_only = editor.findChild<QPushButton*>(QStringLiteral("analysisMode_1"));
    check(control_only && control_only->isVisible(), "the analysis details offer 「只看控制度」");
    control_only->click();
    settle();
    check(profiles.activeScheme().options.analysis_control_only, "choosing it is saved at once");
    click(&editor, box(PaneId::Map).center());
    settle();
    check(!editor.detailsVisible(), "clicking elsewhere closes the details");
    editor.undo();
    settle();
    check(!profiles.activeScheme().options.analysis_control_only, "undo turns it back off");

    // 点方案:整个画面马上换,撤销回到原来的方案。
    QPushButton* novice = editor.schemeButton(QStringLiteral("novice"));
    check(novice != nullptr, "the schemes are listed along the top");
    novice->click();
    settle();
    check(profiles.activeScheme().id == QStringLiteral("novice") && !saved(profiles, PaneId::NextStep).isNull(),
          "clicking a scheme switches to it");
    check(editor.tileRect(PaneId::NextStep).isNull(), "the library follows the scheme");
    editor.undo();
    settle();
    check(profiles.activeScheme().id == QStringLiteral("standard"), "undo goes back to the previous scheme");

    // 至少留一块。
    const std::vector<PaneSlot> before = editor.cells();
    for (const PaneSlot& slot : before)
        if (slot.pane != PaneId::Map) {
            click(&editor, editor.closeRect(slot.pane).center());
            settle();
        }
    check(editor.cells().size() == 1 && saved(profiles, PaneId::Map) == QRect(0, 0, 16, 9),
          "removing everything else leaves the map on the whole screen");
    click(&editor, editor.closeRect(PaneId::Map).center());
    settle();
    check(editor.cells().size() == 1 && editor.toastText().contains(QStringLiteral("至少")),
          "the last pane stays");

    // 比赛中锁定:什么都改不了。
    profiles.setLocked(true);
    const std::vector<PaneSlot> locked = editor.cells();
    drag(&editor, centre(PaneId::Events), box(PaneId::Map).center());
    settle();
    check(editor.cells() == locked && saved(profiles, PaneId::Events).isNull(), "a locked profile refuses changes");
}

}  // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    try {
        one_page_settings();
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
