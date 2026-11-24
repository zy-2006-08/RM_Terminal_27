#include "layout_ops.h"
#include "ui_profile.h"
#include "ui_profile_store.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QTemporaryDir>
#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace rm_terminal;

namespace {

void check(bool condition, const char* name) {
    if (!condition) throw std::runtime_error(name);
    std::cout << "PASS " << name << '\n';
}

void write_raw(const QString& path, const QByteArray& payload) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) throw std::runtime_error("cannot seed file");
    file.write(payload);
}

QByteArray read_raw(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

// 格子里每一块都能关,所以枚举默认布局里所有面板的每一种关法。
std::vector<std::vector<PaneId>> hideable_subsets(Role role) {
    std::vector<PaneId> hideable;
    for (const PaneSlot& slot : layout_profile(role).cells)
        if (pane_hideable(slot.pane)) hideable.push_back(slot.pane);
    std::vector<std::vector<PaneId>> out;
    for (unsigned mask = 0; mask < (1u << hideable.size()); ++mask) {
        std::vector<PaneId> subset;
        for (std::size_t i = 0; i < hideable.size(); ++i)
            if (mask & (1u << i)) subset.push_back(hideable[i]);
        out.push_back(subset);
    }
    return out;
}

// 每种席位、每种藏法:剩下的面板一个不少,藏掉的都不在,格子互不重叠,而且每一格
// 都被某块面板盖住 —— 也就是画面上没有空洞。全藏光了就按没藏排。
void hiding_never_leaves_holes() {
    for (const Role role : {Role::Infantry, Role::Hero, Role::Commander}) {
        const LayoutProfile& base = layout_profile(role);
        for (const auto& subset : hideable_subsets(role)) {
            UiScheme scheme;
            scheme.hidden = std::set<PaneId>(subset.begin(), subset.end());
            const bool everything = subset.size() == base.cells.size();
            const std::set<PaneId> hidden = everything ? std::set<PaneId>{} : scheme.hidden;
            const LayoutProfile out = scheme_layout(role, scheme);
            const int rows = kGridRows;
            const int columns = kGridColumns;
            std::vector<int> owner(static_cast<std::size_t>(rows * columns), 0);
            bool overlap = false;
            for (const PaneSlot& slot : out.cells)
                for (int r = slot.row; r < slot.row + slot.row_span; ++r)
                    for (int c = slot.column; c < slot.column + slot.column_span; ++c)
                        if (++owner[static_cast<std::size_t>(r * columns + c)] > 1) overlap = true;
            check(!overlap, "hidden panes are filled without overlap");
            bool holes = false;
            for (int r = 0; r < rows; ++r)
                for (int c = 0; c < columns; ++c)
                    if (owner[static_cast<std::size_t>(r * columns + c)] == 0) holes = true;
            check(!holes, "hiding leaves no empty cell");
            bool all_kept = true;
            for (const PaneSlot& slot : base.cells) {
                const bool present = std::any_of(out.cells.begin(), out.cells.end(),
                                                 [&](const PaneSlot& s) { return s.pane == slot.pane; });
                if (present == static_cast<bool>(hidden.count(slot.pane))) all_kept = false;
            }
            check(all_kept, "exactly the hidden panes are gone");
        }
    }
    UiScheme no_map;
    no_map.hidden = {PaneId::Map, PaneId::AllyRoster};
    const LayoutProfile without = scheme_layout(Role::Infantry, no_map);
    check(without.cells.size() + 2 == layout_profile(Role::Infantry).cells.size() &&
              unfilled_units(Role::Infantry, no_map) == 0,
          "the map and a roster can be turned off too, and the rest fills the screen");
}

void builtins_are_sane() {
    for (const Role role : {Role::Infantry, Role::Hero, Role::Commander}) {
        const RoleSetup setup = default_role_setup(role);
        check(setup.active == QStringLiteral("standard"), "a new operator starts on the standard scheme");
        // 「新手 · 带下一步」只给有「下一步」的席位(步兵、重装)。
        // 「图传为主」也只给步兵、重装:指挥手的图传本来就单独占一格。
        check(setup.schemes.size() == (role == Role::Commander ? 2u : 4u), "built-in schemes per seat");
        for (const UiScheme& scheme : setup.schemes) {
            for (const PaneId pane : scheme.hidden) check(pane_hideable(pane), "built-in schemes only hide hideable panes");
            check(scheme.cells.empty() || cells_valid(role, scheme.cells), "built-in layouts are valid");
            check(unfilled_units(role, scheme) == 0, "built-in schemes leave no empty cell");
        }
    }
}

// 界面设置存下来的格子:读写来回不变;读坏了(重叠、出界、一块都没有)就整份回到默认布局,
// 不去猜;缩小、挪走以后空出来的格子主界面上由旁边的面板补满。
void dragged_layouts() {
    const Role role = Role::Infantry;
    UiScheme scheme = default_role_setup(role).schemes.front();
    std::vector<PaneSlot> cells = layout_profile(role).cells;
    for (PaneSlot& slot : cells)
        if (slot.pane == PaneId::Map) slot.column_span = 5;
    cells.push_back({PaneId::Timeline, 6, 9, 2, 3});
    check(cells_valid(role, cells), "a shrunk map plus the timeline is a valid layout");
    scheme.cells = cells;
    const LayoutProfile shown = scheme_layout(role, scheme);
    check(unfilled_units(role, scheme) == 0, "the main screen fills the cells a shrunk pane left behind");
    std::vector<int> owner(static_cast<std::size_t>(kGridColumns * kGridRows), 0);
    bool overlap = false;
    for (const PaneSlot& slot : shown.cells)
        for (int r = slot.row; r < slot.row + slot.row_span; ++r)
            for (int c = slot.column; c < slot.column + slot.column_span; ++c)
                if (++owner[static_cast<std::size_t>(r * kGridColumns + c)] > 1) overlap = true;
    check(!overlap, "filling empty cells never overlaps panes");

    UiProfileBook book;
    book.operators = {default_operator(QStringLiteral("A"))};
    RoleSetup setup = default_role_setup(role);
    setup.schemes.front().cells = cells;
    book.operators[0].roles[role] = setup;
    book.last_operator = QStringLiteral("A");
    QString error;
    const auto back = book_from_json(book_to_json(book), &error);
    check(back.has_value() && back->operators[0].roles.at(role).schemes.front().cells == cells,
          "dragged cells survive a save and load");

    const auto broken = [&](const std::vector<PaneSlot>& bad, const char* name) {
        UiProfileBook copy = book;
        copy.operators[0].roles[role].schemes.front().cells = bad;
        const auto loaded = book_from_json(book_to_json(copy), &error);
        check(loaded.has_value() && loaded->operators[0].roles.at(role).schemes.front().cells.empty(), name);
    };
    std::vector<PaneSlot> overlapping = cells;
    overlapping.push_back({PaneId::SelfStatus, 0, 0, 2, 3});
    broken(overlapping, "overlapping cells fall back to the default layout");
    std::vector<PaneSlot> outside = cells;
    outside.back().column = 15;
    broken(outside, "cells outside the grid fall back to the default layout");
    broken({}, "an empty layout falls back to the default layout");
    std::vector<PaneSlot> no_map;
    for (const PaneSlot& slot : cells)
        if (slot.pane != PaneId::Map) no_map.push_back(slot);
    UiProfileBook mapless = book;
    mapless.operators[0].roles[role].schemes.front().cells = no_map;
    const auto kept = book_from_json(book_to_json(mapless), &error);
    check(kept.has_value() && kept->operators[0].roles.at(role).schemes.front().cells == no_map,
          "a layout without the map is kept: every pane can be turned off");

    // 旧版本存的「控制度（大号）」这一版没有了(改成战场数据分析的「只看控制度」):读的时候拿掉,
    // 它的地方由旁边的面板补上,其余照旧。
    QJsonObject old = book_to_json(book);
    QJsonArray operators = old.value("operators").toArray();
    QJsonObject first = operators[0].toObject();
    QJsonObject roles = first.value("roles").toObject();
    QJsonObject infantry = roles.value("infantry").toObject();
    QJsonArray schemes = infantry.value("schemes").toArray();
    QJsonObject standard = schemes[0].toObject();
    QJsonArray old_cells = standard.value("cells").toArray();
    for (int i = 0; i < old_cells.size(); ++i) {
        QJsonObject cell = old_cells[i].toObject();
        if (cell.value("pane").toString() == QStringLiteral("timeline")) {
            cell.insert("pane", QStringLiteral("big_control"));
            old_cells[i] = cell;
        }
    }
    standard.insert("cells", old_cells);
    schemes[0] = standard;
    infantry.insert("schemes", schemes);
    roles.insert("infantry", infantry);
    first.insert("roles", roles);
    operators[0] = first;
    old.insert("operators", operators);
    const auto upgraded = book_from_json(old, &error);
    check(upgraded.has_value(), "a file with the old big control still reads");
    const std::vector<PaneSlot>& upgraded_cells = upgraded->operators[0].roles.at(role).schemes.front().cells;
    check(upgraded_cells.size() + 1 == cells.size() && cells_valid(role, upgraded_cells) &&
              std::none_of(upgraded_cells.begin(), upgraded_cells.end(),
                           [](const PaneSlot& slot) { return slot.pane == PaneId::BigControl; }),
          "the old big control is dropped and the rest of the layout is kept");
    UiScheme upgraded_scheme;
    upgraded_scheme.cells = upgraded_cells;
    check(unfilled_units(role, upgraded_scheme) == 0, "its place is filled by a neighbour");
}

QRect units_in(const std::vector<PaneSlot>& cells, PaneId pane) {
    for (const PaneSlot& slot : cells)
        if (slot.pane == pane) return slot.units();
    return {};
}

bool tiles_the_grid(const std::vector<PaneSlot>& cells) {
    std::vector<int> owner(static_cast<std::size_t>(kGridColumns * kGridRows), 0);
    for (const PaneSlot& slot : cells)
        for (int r = slot.row; r < slot.row + slot.row_span; ++r)
            for (int c = slot.column; c < slot.column + slot.column_span; ++c) {
                if (r < 0 || c < 0 || r >= kGridRows || c >= kGridColumns) return false;
                if (++owner[static_cast<std::size_t>(r * kGridColumns + c)] > 1) return false;
            }
    return std::all_of(owner.begin(), owner.end(), [](int n) { return n == 1; });
}

// 界面设置页里的几种改法(效果图 10、11):删掉、插到边上、放正中间换位置、拖缝。
// 每一下改完画面都正好铺满、互不重叠,每块都不小于自己的最小尺寸;做不到就原样不动。
void editing_moves() {
    const Role role = Role::Infantry;
    const std::vector<PaneSlot> start = layout_profile(role).cells;
    std::vector<PaneSlot> standard = scheme_layout(role, builtin_scheme(role, QStringLiteral("standard")).value()).cells;
    check(tiles_the_grid(standard), "the standard layout tiles the grid");

    const std::vector<PaneSlot> removed = layout_remove(standard, PaneId::Events);
    check(units_in(removed, PaneId::Events).isNull() && tiles_the_grid(removed), "removing a pane leaves no hole");
    check(units_in(removed, PaneId::EnemyRoster) == QRect(12, 0, 4, 9),
          "the pane above takes the removed pane's place");
    const std::vector<PaneSlot> no_analysis = layout_remove(standard, PaneId::Analysis);
    check(units_in(no_analysis, PaneId::Map) == QRect(4, 0, 8, 9) &&
              units_in(no_analysis, PaneId::AllyRoster) == QRect(0, 0, 4, 7),
          "the map grows up rather than a roster growing sideways");

    const auto inserted = layout_drop(standard, PaneId::Events, PaneId::Map, DropSide::Left);
    check(inserted && tiles_the_grid(*inserted), "dropping on an edge keeps the grid tiled");
    check(units_in(*inserted, PaneId::Events) == QRect(4, 4, 4, 5) && units_in(*inserted, PaneId::Map) == QRect(8, 4, 4, 5),
          "the dropped pane takes the left part of the map and the map gets narrower");
    check(units_in(*inserted, PaneId::EnemyRoster) == QRect(12, 0, 4, 9), "the old place is filled by a neighbour");

    const auto swapped = layout_drop(standard, PaneId::Events, PaneId::Analysis, DropSide::Center);
    check(swapped && units_in(*swapped, PaneId::Events) == units_in(standard, PaneId::Analysis) &&
              units_in(*swapped, PaneId::Analysis) == units_in(standard, PaneId::Events),
          "dropping in the middle swaps two panes of different sizes");

    const auto added = layout_drop(standard, PaneId::Timeline, PaneId::Events, DropSide::Bottom);
    check(added && tiles_the_grid(*added) && units_in(*added, PaneId::Timeline) == QRect(12, 6, 4, 3) &&
              units_in(*added, PaneId::Events) == QRect(12, 4, 4, 2),
          "a library pane dropped on the bottom edge takes the bottom of that pane");
    const auto added_middle = layout_drop(standard, PaneId::SelfStatus, PaneId::Map, DropSide::Center);
    check(added_middle && tiles_the_grid(*added_middle) && units_in(*added_middle, PaneId::SelfStatus) == QRect(8, 4, 4, 5),
          "a library pane dropped in the middle takes the far half of the longer side");

    check(!layout_drop(standard, PaneId::Map, PaneId::AllyRoster, DropSide::Left),
          "a drop that would squeeze a pane below its minimum is refused");
    check(!layout_drop(standard, PaneId::Map, PaneId::Map, DropSide::Center), "a pane cannot be dropped on itself");

    const auto seam = seam_at(standard, 4.05, 1.5, 0.24);
    check(seam && seam->vertical && seam->line == 4 && seam->from == 0 && seam->to == 9,
          "the seam between the left column and the middle is found, full height");
    const std::vector<Seam> handles = all_seams(standard);
    check(std::find(handles.begin(), handles.end(), *seam) != handles.end(), "and it has a handle");
    const auto wider = layout_move_seam(standard, *seam, 5);
    check(wider && units_in(*wider, PaneId::AllyRoster) == QRect(0, 0, 5, 7) &&
              units_in(*wider, PaneId::Analysis) == QRect(5, 0, 7, 4) && tiles_the_grid(*wider),
          "dragging the seam makes one side wider and the other narrower");
    check(units_in(*wider, PaneId::EnemyRoster) == units_in(standard, PaneId::EnemyRoster),
          "panes that do not touch the seam stay as they are");
    check(!layout_move_seam(standard, *seam, 9), "a seam cannot squeeze a pane below its minimum");
    check(!seam_at(standard, 2.0, 1.5, 0.24), "the middle of a pane is not a seam");
    const auto across = seam_at(standard, 2.0, 7.02, 0.24);
    check(across && !across->vertical && across->line == 7 && across->from == 0 && across->to == 4,
          "the seam under the ally roster only spans the panes that touch it");

    const auto replaced = layout_replace(standard, PaneId::SelfStatus, PaneId::AllyRoster);
    check(replaced && tiles_the_grid(*replaced) && units_in(*replaced, PaneId::SelfStatus) == QRect(0, 0, 4, 7) &&
              units_in(*replaced, PaneId::AllyRoster).isNull(),
          "a library pane can take another pane's whole place");
    const auto moved_onto = layout_replace(standard, PaneId::Events, PaneId::AllyRoster);
    check(moved_onto && tiles_the_grid(*moved_onto) && units_in(*moved_onto, PaneId::Events) == QRect(0, 0, 4, 7) &&
              units_in(*moved_onto, PaneId::AllyRoster).isNull() &&
              units_in(*moved_onto, PaneId::EnemyRoster) == QRect(12, 0, 4, 9),
          "a pane put in another's place leaves its old place to a neighbour");
    const auto narrow = layout_move_seam(standard, *seam, 3);
    check(narrow && !layout_replace(*narrow, PaneId::Map, PaneId::AllyRoster),
          "a pane never replaces one that is smaller than its minimum");

    std::vector<PaneSlot> holed = start;
    holed.erase(std::remove_if(holed.begin(), holed.end(), [](const PaneSlot& slot) { return slot.pane == PaneId::Events; }),
                holed.end());
    fill_empty_cells(&holed);
    check(tiles_the_grid(holed), "empty cells are given to the neighbours");
}

void json_round_trip_and_upgrade() {
    UiProfileBook book;
    book.operators = {default_operator(QStringLiteral("小王")), default_operator(QStringLiteral("A"))};
    RoleSetup setup = default_role_setup(Role::Infantry);
    UiScheme custom = setup.schemes.front();
    custom.id = QStringLiteral("custom-1");
    custom.name = QStringLiteral("地图大");
    custom.hidden = {PaneId::Events};
    custom.options.map_background_percent = 45;
    custom.options.alerts_muted = {QStringLiteral("energy")};
    custom.options.analysis_control_only = true;
    setup.schemes.push_back(custom);
    setup.active = custom.id;
    book.operators[0].roles[Role::Infantry] = setup;
    book.last_operator = QStringLiteral("小王");
    QString error;
    const auto back = book_from_json(book_to_json(book), &error);
    check(back.has_value(), "a saved book reads back");
    const UiScheme& active = active_scheme(back->operators[0].roles.at(Role::Infantry));
    check(active.name == QStringLiteral("地图大") && active.hidden == std::set<PaneId>{PaneId::Events},
          "the custom scheme keeps its name and hidden panes");
    check(active.options.map_background_percent == 45 && active.options.alerts_muted == QStringList{"energy"} &&
              active.options.analysis_control_only,
          "the custom scheme keeps its detail settings");
    check(back->last_operator == QStringLiteral("小王"), "the last operator is remembered");

    QJsonObject old = book_to_json(book);
    QJsonArray operators = old.value("operators").toArray();
    QJsonObject first = operators[0].toObject();
    QJsonObject roles = first.value("roles").toObject();
    QJsonObject infantry = roles.value("infantry").toObject();
    QJsonArray schemes = infantry.value("schemes").toArray();
    QJsonObject scheme = schemes.last().toObject();
    scheme.insert("hidden", QJsonArray{"events", "a_pane_from_the_future", "map"});
    scheme.insert("options", QJsonObject{{"alerts_muted", QJsonArray{"energy", "kind_we_removed"}}});
    schemes.replace(schemes.size() - 1, scheme);
    schemes.removeAt(1);
    infantry.insert("schemes", schemes);
    roles.insert("infantry", infantry);
    roles.insert("a_role_from_the_future", QJsonObject{});
    first.insert("roles", roles);
    operators.replace(0, first);
    old.insert("operators", operators);
    const auto upgraded = book_from_json(old, &error);
    check(upgraded.has_value(), "unknown panes, kinds and roles do not make a file unreadable");
    const RoleSetup& fixed = upgraded->operators[0].roles.at(Role::Infantry);
    check(fixed.schemes.size() == default_role_setup(Role::Infantry).schemes.size() + 1,
          "a missing built-in scheme is added back");
    const UiScheme& kept = active_scheme(fixed);
    check(kept.hidden == (std::set<PaneId>{PaneId::Events, PaneId::Map}),
          "unknown panes are dropped; the map can be hidden like every other pane");
    check(kept.options.alerts_muted == QStringList{"energy"}, "unknown alert kinds are dropped");
    check(kept.options.map_show_marks, "missing option keys take their defaults");

    QJsonObject wrong = book_to_json(book);
    wrong.insert("version", 99);
    check(!book_from_json(wrong, &error).has_value(), "a file from another version is not guessed at");
    check(!name_problem(QStringLiteral(" A")).isEmpty() && !name_problem(QString()).isEmpty() &&
              !name_problem(QStringLiteral("一二三四五六七八九十一二三")).isEmpty() && name_problem("小王").isEmpty(),
          "operator names are checked");
}

// 以前的内置方案「安静」和「标准」画面一样,去掉了:没改过的拿掉(正在用的回到标准),改过的留成自建方案。
void old_quiet_scheme_is_retired() {
    const auto load_with_quiet = [](bool edited) {
        UiProfileBook book;
        book.operators = {default_operator(QStringLiteral("zy"))};
        RoleSetup setup = default_role_setup(Role::Infantry);
        UiScheme quiet = setup.schemes.front();
        quiet.id = QStringLiteral("quiet");
        quiet.name = QStringLiteral("安静");
        quiet.cells.clear();
        quiet.hidden = {PaneId::NextStep};
        quiet.options.enemy_low_hp_pulse = false;
        quiet.options.alert_voice = false;
        quiet.options.next_voice = false;
        quiet.options.alerts_muted = {QStringLiteral("energy"), QStringLiteral("dart")};
        if (edited) quiet.options.map_background_percent = 45;
        setup.schemes.push_back(quiet);
        setup.active = quiet.id;
        book.operators[0].roles[Role::Infantry] = setup;
        book.last_operator = QStringLiteral("zy");
        QString error;
        return book_from_json(book_to_json(book), &error).value().operators[0].roles.at(Role::Infantry);
    };
    const auto find = [](const RoleSetup& setup, const QString& name) {
        return std::find_if(setup.schemes.begin(), setup.schemes.end(),
                            [&](const UiScheme& scheme) { return scheme.name == name; });
    };
    const RoleSetup untouched = load_with_quiet(false);
    check(find(untouched, QStringLiteral("安静")) == untouched.schemes.end() &&
              untouched.schemes.size() == default_role_setup(Role::Infantry).schemes.size() &&
              untouched.active == QStringLiteral("standard"),
          "an untouched old quiet scheme is dropped and its user is back on the standard scheme");
    const RoleSetup edited = load_with_quiet(true);
    const auto kept = find(edited, QStringLiteral("安静"));
    check(kept != edited.schemes.end() && kept->id.startsWith(QStringLiteral("custom-")) && edited.active == kept->id &&
              kept->options.map_background_percent == 45 && !kept->options.alert_voice,
          "an edited old quiet scheme stays as the operator's own scheme");
}

// 以前步兵的图传在「本车」里面:旧文件里本车 5 格高,没有图传那一块。打开时拆成两块;删过图传的不加回来。
void old_video_inside_self_commands_is_split() {
    const auto load = [](bool video_removed) {
        UiProfileBook book;
        book.operators = {default_operator(QStringLiteral("zy"))};
        RoleSetup setup = default_role_setup(Role::Infantry);
        UiScheme& standard = setup.schemes.front();
        standard.cells = {PaneSlot{PaneId::AllyRoster, 0, 0, 4, 4},   PaneSlot{PaneId::Analysis, 0, 4, 4, 8},
                          PaneSlot{PaneId::EnemyRoster, 0, 12, 4, 4}, PaneSlot{PaneId::SelfCommands, 4, 0, 5, 4},
                          PaneSlot{PaneId::Map, 4, 4, 5, 8},          PaneSlot{PaneId::Events, 4, 12, 5, 4}};
        standard.hidden = video_removed ? std::set<PaneId>{PaneId::Video} : std::set<PaneId>{};
        book.operators[0].roles[Role::Infantry] = setup;
        book.last_operator = QStringLiteral("zy");
        QString error;
        return book_from_json(book_to_json(book), &error).value().operators[0].roles.at(Role::Infantry).schemes.front().cells;
    };
    // 「本车」也删掉了:拆出来的图传留下,本车那几格由上面的花名册长下来补上。
    const std::vector<PaneSlot> split = load(false);
    check(units_in(split, PaneId::SelfCommands).isNull() && units_in(split, PaneId::Video) == QRect(0, 7, 4, 2) &&
              units_in(split, PaneId::AllyRoster) == QRect(0, 0, 4, 7) && tiles_the_grid(split),
          "an old layout with the video inside 本车 keeps the video; 本车 is gone and its cells are filled");
    const std::vector<PaneSlot> removed = load(true);
    check(units_in(removed, PaneId::SelfCommands).isNull() && units_in(removed, PaneId::Video).isNull() &&
              tiles_the_grid(removed),
          "a layout whose video was removed keeps it removed, and 本车 is gone too");
}

void controller_persists(const QString& dir) {
    {
        UiProfileController controller{UiProfileRepository(dir), Role::Infantry};
        check(controller.loadResult() == UiProfileLoadResult::Missing, "first run has no file");
        check(controller.operatorNames() == QStringList{"默认"}, "first run has one default operator");
        check(controller.addOperator(QStringLiteral("小王")).isEmpty(), "an operator can be added");
        check(controller.currentOperator() == QStringLiteral("小王"), "a new operator becomes current");
        check(controller.selectScheme(QStringLiteral("minimal")), "a scheme can be chosen");
        controller.setPaneHidden(PaneId::Analysis, true);
        UiOptions options = controller.activeScheme().options;
        options.map_show_enemies = false;
        controller.setOptions(options);
        check(controller.createScheme(QStringLiteral("我的")).isEmpty(), "a scheme can be copied");
        check(controller.activeScheme().id == QStringLiteral("custom-1"), "the copy becomes active");
        controller.setLocked(true);
        check(!controller.selectScheme(QStringLiteral("standard")), "a locked controller refuses changes");
        check(!controller.addOperator(QStringLiteral("B")).isEmpty(), "a locked controller refuses new operators");
    }
    {
        UiProfileController again{UiProfileRepository(dir), Role::Infantry};
        check(again.loadResult() == UiProfileLoadResult::Loaded, "the file is there after restart");
        check(again.currentOperator() == QStringLiteral("小王"), "the last operator comes back after restart");
        const UiScheme& active = again.activeScheme();
        check(active.name == QStringLiteral("我的") &&
                  active.hidden ==
                      std::set<PaneId>{PaneId::Events, PaneId::Video, PaneId::Analysis, PaneId::NextStep} &&
                  !active.options.map_show_enemies,
              "the active scheme and its settings come back after restart");
        check(again.resetScheme(QStringLiteral("minimal")).isEmpty(), "a built-in scheme can be reset");
        check(!again.resetScheme(QStringLiteral("custom-1")).isEmpty(), "a custom scheme has no factory reset");
        check(!again.removeScheme(QStringLiteral("standard")).isEmpty(), "a built-in scheme cannot be removed");
        check(again.removeScheme(QStringLiteral("custom-1")).isEmpty() &&
                  again.activeScheme().id == QStringLiteral("standard"),
              "removing the active scheme falls back to standard");
        check(again.switchOperator(QStringLiteral("默认")), "operators can be switched");
    }
    UiProfileController hero{UiProfileRepository(dir), Role::Hero};
    check(hero.currentOperator() == QStringLiteral("默认"), "the switch was saved");
    check(hero.activeScheme().id == QStringLiteral("standard"), "each seat has its own schemes");
}

void export_import(const QString& dir) {
    const QString one = QDir(dir).filePath("one");
    const QString two = QDir(dir).filePath("two");
    QDir().mkpath(one);
    QDir().mkpath(two);
    const QString file = QDir(dir).filePath("小王.json");
    {
        UiProfileController source{UiProfileRepository(one), Role::Infantry};
        source.addOperator(QStringLiteral("小王"));
        source.selectScheme(QStringLiteral("minimal"));
        QString error;
        check(source.exportCurrent(file, &error), "the current operator exports to a file");
    }
    QString error;
    check(UiProfileController::operatorsIn(file, &error) == QStringList{"小王"}, "an export holds one operator");
    UiProfileController target{UiProfileRepository(two), Role::Infantry};
    QStringList replaced;
    check(target.importFrom(file, &replaced).isEmpty() && replaced.isEmpty(), "an export imports on another computer");
    check(target.currentOperator() == QStringLiteral("小王") && target.activeScheme().id == QStringLiteral("minimal"),
          "importing one operator switches to them with their scheme");
    check(target.importFrom(file, &replaced).isEmpty() && replaced == QStringList{"小王"},
          "importing the same operator again replaces them");
    check(target.operatorNames().size() == 2, "replacing does not duplicate");
    write_raw(QDir(dir).filePath("bad.json"), "{nope");
    check(!target.importFrom(QDir(dir).filePath("bad.json"), &replaced).isEmpty(), "an unreadable import is refused");
}

void unreadable_file_is_kept(const QString& dir) {
    const QString path = QDir(dir).filePath("ui_profiles.json");
    const QByteArray garbage = "{\"version\": 1, \"operators\": 7}";
    write_raw(path, garbage);
    UiProfileController controller{UiProfileRepository(dir), Role::Infantry};
    check(controller.loadResult() == UiProfileLoadResult::Invalid, "a broken file is reported");
    check(!controller.problem().isEmpty(), "the operator is told about the broken file");
    check(read_raw(path) == garbage, "opening does not touch the broken file");
    controller.addOperator(QStringLiteral("小王"));
    check(read_raw(QDir(dir).filePath("ui_profiles.bad-1.json")) == garbage,
          "the first save keeps the broken file under another name");
    check(UiProfileRepository(dir).load().result == UiProfileLoadResult::Loaded, "then the new settings save");
    check(controller.problem().isEmpty(), "and the warning clears");
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    try {
        hiding_never_leaves_holes();
        builtins_are_sane();
        dragged_layouts();
        editing_moves();
        json_round_trip_and_upgrade();
        old_quiet_scheme_is_retired();
        old_video_inside_self_commands_is_split();
        QTemporaryDir a, b, c;
        controller_persists(a.path());
        export_import(b.path());
        unreadable_file_is_kept(c.path());
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
