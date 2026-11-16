#include "ui_profile.h"

#include "layout_ops.h"

#include "config.h"
#include "next_step.h"

#include <QJsonArray>
#include <QSet>
#include <algorithm>

namespace rm_terminal {
namespace {

constexpr PaneId kAllPanes[] = {PaneId::AllyRoster, PaneId::EnemyRoster, PaneId::Map,
                                PaneId::Events,     PaneId::Analysis,    PaneId::Video,
                                PaneId::Assembly,   PaneId::Dart,        PaneId::Sentry,
                                PaneId::SelfCommands, PaneId::NextStep,  PaneId::BigControl,
                                PaneId::SelfStatus, PaneId::Timeline};
constexpr Role kAllRoles[] = {Role::Infantry, Role::Hero, Role::Commander};

std::optional<Role> role_from_name(const QString& name) {
    for (const Role role : kAllRoles)
        if (role_name(role) == name) return role;
    return std::nullopt;
}

bool in_profile(Role role, PaneId pane) {
    const std::vector<PaneId> panes = available_panes(role);
    return std::find(panes.begin(), panes.end(), pane) != panes.end();
}

UiScheme make_scheme(const QString& id, const QString& name) {
    UiScheme scheme;
    scheme.id = id;
    scheme.name = name;
    return scheme;
}

QJsonObject options_to_json(const UiOptions& options) {
    QJsonObject out;
    out.insert(QStringLiteral("map_background_percent"), options.map_background_percent);
    out.insert(QStringLiteral("map_show_enemies"), options.map_show_enemies);
    out.insert(QStringLiteral("map_show_marks"), options.map_show_marks);
    out.insert(QStringLiteral("enemy_low_hp_pulse"), options.enemy_low_hp_pulse);
    out.insert(QStringLiteral("analysis_control_only"), options.analysis_control_only);
    out.insert(QStringLiteral("alert_voice"), options.alert_voice);
    out.insert(QStringLiteral("alerts_muted"), QJsonArray::fromStringList(options.alerts_muted));
    out.insert(QStringLiteral("next_mode"), options.next_mode);
    out.insert(QStringLiteral("next_count"), options.next_count);
    out.insert(QStringLiteral("next_size"), options.next_size);
    out.insert(QStringLiteral("next_voice"), options.next_voice);
    out.insert(QStringLiteral("next_dwell_sec"), options.next_dwell_sec);
    return out;
}

bool read_bool(const QJsonObject& object, const char* key, bool fallback, bool* ok) {
    const auto value = object.value(QLatin1String(key));
    if (value.isUndefined()) return fallback;
    if (!value.isBool()) *ok = false;
    return value.toBool(fallback);
}

std::optional<UiOptions> options_from_json(const QJsonObject& object) {
    UiOptions options;
    bool ok = true;
    const auto percent = object.value(QStringLiteral("map_background_percent"));
    if (!percent.isUndefined()) {
        if (!percent.isDouble()) return std::nullopt;
        options.map_background_percent = percent.toInt();
    }
    options.map_show_enemies = read_bool(object, "map_show_enemies", true, &ok);
    options.map_show_marks = read_bool(object, "map_show_marks", true, &ok);
    options.enemy_low_hp_pulse = read_bool(object, "enemy_low_hp_pulse", true, &ok);
    options.analysis_control_only = read_bool(object, "analysis_control_only", false, &ok);
    options.alert_voice = read_bool(object, "alert_voice", true, &ok);
    options.next_voice = read_bool(object, "next_voice", true, &ok);
    for (const auto& [key, target] : {std::pair{"next_mode", &options.next_mode},
                                      std::pair{"next_count", &options.next_count},
                                      std::pair{"next_size", &options.next_size},
                                      std::pair{"next_dwell_sec", &options.next_dwell_sec}}) {
        const auto value = object.value(QLatin1String(key));
        if (value.isUndefined()) continue;
        if (!value.isDouble()) return std::nullopt;
        *target = value.toInt();
    }
    const auto muted = object.value(QStringLiteral("alerts_muted"));
    if (!muted.isUndefined()) {
        if (!muted.isArray()) return std::nullopt;
        for (const auto item : muted.toArray()) {
            if (!item.isString()) return std::nullopt;
            options.alerts_muted.append(item.toString());
        }
    }
    if (!ok) return std::nullopt;
    return options;
}

}  // namespace

bool UiOptions::operator==(const UiOptions& other) const {
    return map_background_percent == other.map_background_percent &&
           map_show_enemies == other.map_show_enemies && map_show_marks == other.map_show_marks &&
           enemy_low_hp_pulse == other.enemy_low_hp_pulse && alert_voice == other.alert_voice &&
           analysis_control_only == other.analysis_control_only &&
           alerts_muted == other.alerts_muted && next_mode == other.next_mode && next_count == other.next_count &&
           next_size == other.next_size && next_voice == other.next_voice &&
           next_dwell_sec == other.next_dwell_sec;
}

// 每一块都能关(操作手自己定)。界面设置页里只拦一件事:画面里至少留一块。
bool pane_hideable(PaneId) { return true; }

QString pane_title(PaneId pane) {
    switch (pane) {
    case PaneId::AllyRoster: return QStringLiteral("我方机器人");
    case PaneId::EnemyRoster: return QStringLiteral("敌方机器人");
    case PaneId::Map: return QStringLiteral("战术地图");
    case PaneId::Events: return QStringLiteral("战场事件");
    case PaneId::Analysis: return QStringLiteral("战场数据分析");
    case PaneId::Video: return QStringLiteral("图传");
    case PaneId::Assembly: return QStringLiteral("装配");
    case PaneId::Dart: return QStringLiteral("飞镖");
    case PaneId::Sentry: return QStringLiteral("哨兵");
    case PaneId::SelfCommands: return QStringLiteral("本车");
    case PaneId::NextStep: return QStringLiteral("下一步（三件事）");
    case PaneId::BigControl: return QStringLiteral("控制度（大号）");
    case PaneId::SelfStatus: return QStringLiteral("本机状态（大号）");
    case PaneId::Timeline: return QStringLiteral("接下来的时间点");
    }
    return QString();
}

std::optional<PaneId> pane_from_name(const QString& name) {
    for (const PaneId pane : kAllPanes)
        if (pane_name(pane) == name) return pane;
    return std::nullopt;
}

QString alert_kind_title(const QString& kind) {
    if (kind == QLatin1String("outpost")) return QStringLiteral("前哨区被敌方控制");
    if (kind == QLatin1String("control")) return QStringLiteral("控制度落后");
    if (kind == QLatin1String("fortress")) return QStringLiteral("敌方在激活我方堡垒");
    if (kind == QLatin1String("dart")) return QStringLiteral("飞镖闸门时间点");
    if (kind == QLatin1String("energy")) return QStringLiteral("能量机关时间点");
    if (kind == QLatin1String("low_hp")) return QStringLiteral("敌方残血");
    if (kind == QLatin1String("base")) return QStringLiteral("我方基地被攻击");
    return kind;
}

bool is_builtin_scheme(const QString& id) {
    return id == QLatin1String("standard") || id == QLatin1String("novice") || id == QLatin1String("minimal") ||
           id == QLatin1String("video");
}

std::vector<UiScheme> builtin_schemes(Role role) {
    // 「下一步」只在新手方案里默认打开:嫌它挤的操作手用标准方案就看不到它。
    UiScheme standard = make_scheme(QStringLiteral("standard"), QStringLiteral("标准"));
    standard.hidden = {PaneId::NextStep};
    UiScheme novice = make_scheme(QStringLiteral("novice"), QStringLiteral("新手 · 带下一步"));
    UiScheme minimal = make_scheme(QStringLiteral("minimal"), QStringLiteral("极简"));
    minimal.hidden = {PaneId::Events, PaneId::Video, PaneId::NextStep};
    // 图传为主(效果图 8):官方选手端被遮挡时用。图传占右上 12 × 6 格(1440 × 900 下接近 16:9),
    // 左列 我方 / 敌方 / 地图,下排 战场数据分析。
    UiScheme video = make_scheme(QStringLiteral("video"), QStringLiteral("图传为主"));
    const auto cell = [](PaneId pane, int column, int row, int width, int height) {
        return PaneSlot{pane, row, column, height, width};
    };
    video.cells = {cell(PaneId::AllyRoster, 0, 0, 4, 3), cell(PaneId::EnemyRoster, 0, 3, 4, 3),
                   cell(PaneId::Map, 0, 6, 4, 3), cell(PaneId::Video, 4, 0, 12, 6),
                   cell(PaneId::Analysis, 4, 6, 12, 3)};
    std::vector<UiScheme> out{standard, minimal};
    if (in_profile(role, PaneId::NextStep)) out.insert(out.begin() + 1, novice);
    if (role != Role::Commander) out.push_back(video);
    for (UiScheme& scheme : out)
        for (auto it = scheme.hidden.begin(); it != scheme.hidden.end();)
            it = in_profile(role, *it) ? std::next(it) : scheme.hidden.erase(it);
    return out;
}

std::optional<UiScheme> builtin_scheme(Role role, const QString& id) {
    for (const UiScheme& scheme : builtin_schemes(role))
        if (scheme.id == id) return scheme;
    return std::nullopt;
}

QString scheme_description(const UiScheme& scheme) {
    if (scheme.id == QLatin1String("standard")) return QStringLiteral("所有面板都在，适合大多数人。");
    if (scheme.id == QLatin1String("novice")) return QStringLiteral("画面中间直接告诉你接下来最该做的三件事。");
    if (scheme.id == QLatin1String("minimal")) return QStringLiteral("关掉战场事件和图传小窗，画面更清爽。");
    if (scheme.id == QLatin1String("video")) return QStringLiteral("官方选手端被遮挡时用，图传放到最大。");
    QStringList parts;
    for (const PaneId pane : scheme.hidden) parts << pane_title(pane);
    return parts.isEmpty() ? QStringLiteral("自己建的方案，所有面板都在。")
                           : QStringLiteral("自己建的方案，关掉了%1。").arg(parts.join(QStringLiteral("、")));
}

RoleSetup default_role_setup(Role role) {
    RoleSetup setup;
    setup.active = QStringLiteral("standard");
    setup.schemes = builtin_schemes(role);
    return setup;
}

QString default_operator_name() { return QStringLiteral("默认"); }

OperatorProfile default_operator(const QString& name) {
    OperatorProfile profile;
    profile.name = name;
    return profile;
}

namespace {

// 以前的内置方案「安静」:画面和标准一样,只关了残血闪烁、提醒语音和两种时间点提醒。和「标准」重复,去掉了。
bool is_untouched_old_quiet(const UiScheme& scheme) {
    UiOptions options;
    options.enemy_low_hp_pulse = false;
    options.alert_voice = false;
    options.next_voice = false;
    options.alerts_muted = {QStringLiteral("dart"), QStringLiteral("energy")};
    UiOptions seen = scheme.options;
    seen.alerts_muted.sort();
    return scheme.cells.empty() && scheme.hidden == std::set<PaneId>{PaneId::NextStep} && seen == options;
}

// 以前步兵、重装的图传在「本车」里面(本车 5 格高,下面一截是图传小窗),现在图传单独一块。
// 旧布局里本车够高、图传没删过的:本车留上面,下面 2 格给图传。本车太矮的,图传留在组件库里。
void split_old_video(Role role, UiScheme* scheme) {
    if (role == Role::Commander || scheme->cells.empty() || scheme->hidden.count(PaneId::Video)) return;
    const auto has = [&](PaneId pane) {
        return std::any_of(scheme->cells.begin(), scheme->cells.end(), [pane](const PaneSlot& slot) { return slot.pane == pane; });
    };
    if (has(PaneId::Video)) return;
    const auto host = std::find_if(scheme->cells.begin(), scheme->cells.end(),
                                   [](const PaneSlot& slot) { return slot.pane == PaneId::SelfCommands; });
    const int video_rows = pane_min_units(PaneId::Video).height();
    if (host == scheme->cells.end() || host->row_span < pane_min_units(PaneId::SelfCommands).height() + video_rows) return;
    host->row_span -= video_rows;
    scheme->cells.push_back(PaneSlot{PaneId::Video, host->row + host->row_span, host->column, video_rows, host->column_span});
}


}  // namespace

void normalize(RoleSetup* setup, Role role) {
    const QStringList known_kinds = alert_kind_names();
    // 旧文件里的「安静」:改过的留成自建方案(名字不变);没动过的直接拿掉,正在用它的换回「标准」。
    for (UiScheme& scheme : setup->schemes) {
        if (scheme.id != QLatin1String("quiet") || is_untouched_old_quiet(scheme)) continue;
        int n = 1;
        const auto taken = [&](const QString& id) {
            return std::any_of(setup->schemes.begin(), setup->schemes.end(),
                               [&](const UiScheme& other) { return other.id == id; });
        };
        while (taken(QStringLiteral("custom-%1").arg(n))) ++n;
        const QString id = QStringLiteral("custom-%1").arg(n);
        if (setup->active == scheme.id) setup->active = id;
        scheme.id = id;
    }
    std::vector<UiScheme> kept;
    QSet<QString> ids;
    for (UiScheme scheme : setup->schemes) {
        if (scheme.id.isEmpty() || ids.contains(scheme.id)) continue;
        if (!is_builtin_scheme(scheme.id) && !scheme.id.startsWith(QLatin1String("custom-"))) continue;
        ids.insert(scheme.id);
        for (auto it = scheme.hidden.begin(); it != scheme.hidden.end();)
            it = pane_hideable(*it) && in_profile(role, *it) ? std::next(it) : scheme.hidden.erase(it);
        UiOptions& options = scheme.options;
        options.map_background_percent =
            std::clamp(options.map_background_percent, kMinMapBackgroundPercent, kMaxMapBackgroundPercent);
        QStringList muted;
        for (const QString& kind : options.alerts_muted)
            if (known_kinds.contains(kind) && !muted.contains(kind)) muted << kind;
        options.alerts_muted = muted;
        options.next_mode = std::clamp(options.next_mode, 0, 1);
        options.next_count = std::clamp(options.next_count, 1, 3);
        options.next_size = std::clamp(options.next_size, 0, 2);
        options.next_dwell_sec = std::clamp(options.next_dwell_sec, 1, 10);
        const std::optional<UiScheme> builtin = builtin_scheme(role, scheme.id);
        if (is_builtin_scheme(scheme.id) && !builtin) continue;
        // 这一版没有的面板(比如去掉的「控制度（大号）」)先拿掉,空出来的旁边补上;
        // 剩下的格子还对不上(改了格数、文件坏了)就回到默认布局,不猜。
        if (!scheme.cells.empty()) {
            // 更旧的文件里图传还在本车里面:先拆出来,再删本车(「本车」也删掉了),图传才不会跟着
            // 本车一起没了。本车空出来的格子和别的没了的面板一样,由旁边的面板长过来补上。
            split_old_video(role, &scheme);
            const std::size_t before = scheme.cells.size();
            scheme.cells.erase(std::remove_if(scheme.cells.begin(), scheme.cells.end(),
                                              [role](const PaneSlot& slot) { return !in_profile(role, slot.pane); }),
                               scheme.cells.end());
            if (scheme.cells.size() != before) fill_empty_cells(&scheme.cells);
            if (!cells_valid(role, scheme.cells)) scheme.cells.clear();
        }
        if (builtin) scheme.name = builtin->name;
        kept.push_back(std::move(scheme));
    }
    // 内置方案排在最前、顺序固定;旧文件少了哪个就补哪个。
    std::vector<UiScheme> ordered;
    for (const UiScheme& builtin : builtin_schemes(role)) {
        const auto found = std::find_if(kept.begin(), kept.end(),
                                        [&](const UiScheme& scheme) { return scheme.id == builtin.id; });
        ordered.push_back(found == kept.end() ? builtin : *found);
    }
    for (const UiScheme& scheme : kept)
        if (!is_builtin_scheme(scheme.id) && ordered.size() < kMaxSchemesPerRole) ordered.push_back(scheme);
    setup->schemes = std::move(ordered);
    const bool active_exists = std::any_of(setup->schemes.begin(), setup->schemes.end(),
                                           [&](const UiScheme& scheme) { return scheme.id == setup->active; });
    if (!active_exists) setup->active = QStringLiteral("standard");
}

void normalize(UiProfileBook* book) {
    for (OperatorProfile& profile : book->operators)
        for (auto& [role, setup] : profile.roles) normalize(&setup, role);
    if (book->operators.empty()) book->operators.push_back(default_operator(default_operator_name()));
    const bool last_exists = std::any_of(book->operators.begin(), book->operators.end(),
                                         [&](const OperatorProfile& p) { return p.name == book->last_operator; });
    if (!last_exists) book->last_operator = book->operators.front().name;
}

const UiScheme& active_scheme(const RoleSetup& setup) {
    for (const UiScheme& scheme : setup.schemes)
        if (scheme.id == setup.active) return scheme;
    return setup.schemes.front();
}

QString name_problem(const QString& name) {
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) return QStringLiteral("名字不能是空的");
    if (trimmed != name) return QStringLiteral("名字前后不能有空格");
    if (name.toUcs4().size() > kMaxUiNameChars)
        return QStringLiteral("名字最多 %1 个字").arg(kMaxUiNameChars);
    for (const QChar c : name)
        if (c.isNull() || c.category() == QChar::Other_Control) return QStringLiteral("名字里有不能显示的字符");
    return {};
}

LayoutProfile apply_hidden(const LayoutProfile& base, const std::set<PaneId>& hidden) {
    LayoutProfile out = base;
    std::vector<PaneSlot> holes;
    out.cells.clear();
    for (const PaneSlot& slot : base.cells)
        (hidden.count(slot.pane) && pane_hideable(slot.pane) ? holes : out.cells).push_back(slot);

    // 先不动地图和花名册横向,补不上再一级一级放开。
    for (int level = 0; level <= 2; ++level)
        for (bool progress = true; progress && !holes.empty();) {
            progress = false;
            for (auto it = holes.begin(); it != holes.end();) {
                if (close_hole(&out.cells, it->units(), level)) {
                    it = holes.erase(it);
                    progress = true;
                } else {
                    ++it;
                }
            }
        }

    return out;
}

LayoutProfile scheme_layout(Role role, const UiScheme& scheme, bool fill_holes) {
    LayoutProfile base = layout_profile(role);
    if (!scheme.cells.empty()) base.cells = scheme.cells;
    const bool video_cell = std::any_of(base.cells.begin(), base.cells.end(),
                                        [](const PaneSlot& slot) { return slot.pane == PaneId::Video; });
    if (video_cell)
        base.embedded.erase(std::remove_if(base.embedded.begin(), base.embedded.end(),
                                           [](const auto& pair) { return pair.first == PaneId::Video; }),
                            base.embedded.end());
    if (fill_holes) {
        LayoutProfile out = apply_hidden(base, scheme.hidden);
        // 全藏光了(旧文件、手改的文件)就按没藏排:画面上不能一块都没有。
        if (out.cells.empty()) return base;
        fill_empty_cells(&out.cells);
        return out;
    }
    base.cells.erase(std::remove_if(base.cells.begin(), base.cells.end(),
                                    [&](const PaneSlot& slot) {
                                        return scheme.hidden.count(slot.pane) && pane_hideable(slot.pane);
                                    }),
                     base.cells.end());
    return base;
}

int unfilled_units(Role role, const UiScheme& scheme) {
    const LayoutProfile layout = scheme_layout(role, scheme, true);
    int empty = 0;
    for (int r = 0; r < kGridRows; ++r)
        for (int c = 0; c < kGridColumns; ++c)
            if (std::none_of(layout.cells.begin(), layout.cells.end(),
                             [&](const PaneSlot& slot) { return slot.units().contains(QPoint(c, r)); }))
                ++empty;
    return empty;
}

bool cells_valid(Role role, const std::vector<PaneSlot>& cells) {
    std::vector<int> owner(static_cast<std::size_t>(kGridColumns * kGridRows), 0);
    std::set<PaneId> seen;
    for (const PaneSlot& slot : cells) {
        if (!in_profile(role, slot.pane) || !seen.insert(slot.pane).second) return false;
        if (slot.column < 0 || slot.row < 0 || slot.column_span < 1 || slot.row_span < 1 ||
            slot.column + slot.column_span > kGridColumns || slot.row + slot.row_span > kGridRows)
            return false;
        for (int r = slot.row; r < slot.row + slot.row_span; ++r)
            for (int c = slot.column; c < slot.column + slot.column_span; ++c)
                if (++owner[static_cast<std::size_t>(r * kGridColumns + c)] > 1) return false;
    }
    return !cells.empty();
}

QJsonObject book_to_json(const UiProfileBook& book) {
    QJsonArray operators;
    for (const OperatorProfile& profile : book.operators) {
        QJsonObject roles;
        for (const auto& [role, setup] : profile.roles) {
            QJsonArray schemes;
            for (const UiScheme& scheme : setup.schemes) {
                QJsonArray hidden;
                for (const PaneId pane : scheme.hidden) hidden.append(pane_name(pane));
                QJsonObject object;
                object.insert(QStringLiteral("id"), scheme.id);
                object.insert(QStringLiteral("name"), scheme.name);
                object.insert(QStringLiteral("hidden"), hidden);
                if (!scheme.cells.empty()) {
                    QJsonArray cells;
                    for (const PaneSlot& slot : scheme.cells)
                        cells.append(QJsonObject{{QStringLiteral("pane"), pane_name(slot.pane)},
                                                 {QStringLiteral("column"), slot.column},
                                                 {QStringLiteral("row"), slot.row},
                                                 {QStringLiteral("width"), slot.column_span},
                                                 {QStringLiteral("height"), slot.row_span}});
                    object.insert(QStringLiteral("cells"), cells);
                }
                object.insert(QStringLiteral("options"), options_to_json(scheme.options));
                schemes.append(object);
            }
            QJsonObject object;
            object.insert(QStringLiteral("active"), setup.active);
            object.insert(QStringLiteral("schemes"), schemes);
            roles.insert(role_name(role), object);
        }
        QJsonObject object;
        object.insert(QStringLiteral("name"), profile.name);
        object.insert(QStringLiteral("roles"), roles);
        operators.append(object);
    }
    QJsonObject root;
    root.insert(QStringLiteral("version"), kUiProfileVersion);
    root.insert(QStringLiteral("last_operator"), book.last_operator);
    root.insert(QStringLiteral("operators"), operators);
    return root;
}

std::optional<UiProfileBook> book_from_json(const QJsonObject& root, QString* error) {
    const auto fail = [error](const QString& why) {
        if (error) *error = why;
        return std::nullopt;
    };
    if (root.value(QStringLiteral("version")).toInt(-1) != kUiProfileVersion)
        return fail(QStringLiteral("unsupported version %1")
                        .arg(root.value(QStringLiteral("version")).toVariant().toString()));
    const auto operators = root.value(QStringLiteral("operators"));
    if (!operators.isArray()) return fail(QStringLiteral("operators must be an array"));
    if (static_cast<std::size_t>(operators.toArray().size()) > kMaxOperators)
        return fail(QStringLiteral("too many operators"));

    UiProfileBook book;
    book.last_operator = root.value(QStringLiteral("last_operator")).toString();
    QSet<QString> names;
    for (const auto value : operators.toArray()) {
        if (!value.isObject()) return fail(QStringLiteral("operator entry must be an object"));
        const QJsonObject object = value.toObject();
        OperatorProfile profile;
        profile.name = object.value(QStringLiteral("name")).toString();
        if (const QString problem = name_problem(profile.name); !problem.isEmpty())
            return fail(QStringLiteral("operator name: %1").arg(problem));
        if (names.contains(profile.name)) return fail(QStringLiteral("duplicate operator %1").arg(profile.name));
        names.insert(profile.name);
        const auto roles = object.value(QStringLiteral("roles"));
        if (!roles.isUndefined() && !roles.isObject()) return fail(QStringLiteral("roles must be an object"));
        const QJsonObject role_map = roles.toObject();
        for (auto it = role_map.begin(); it != role_map.end(); ++it) {
            const std::optional<Role> role = role_from_name(it.key());
            if (!role) continue;
            if (!it.value().isObject()) return fail(QStringLiteral("role setup must be an object"));
            const QJsonObject setup_object = it.value().toObject();
            RoleSetup setup;
            setup.active = setup_object.value(QStringLiteral("active")).toString();
            const auto schemes = setup_object.value(QStringLiteral("schemes"));
            if (!schemes.isArray()) return fail(QStringLiteral("schemes must be an array"));
            for (const auto scheme_value : schemes.toArray()) {
                if (!scheme_value.isObject()) return fail(QStringLiteral("scheme must be an object"));
                const QJsonObject scheme_object = scheme_value.toObject();
                UiScheme scheme;
                scheme.id = scheme_object.value(QStringLiteral("id")).toString();
                scheme.name = scheme_object.value(QStringLiteral("name")).toString();
                if (!is_builtin_scheme(scheme.id)) {
                    if (const QString problem = name_problem(scheme.name); !problem.isEmpty())
                        return fail(QStringLiteral("scheme name: %1").arg(problem));
                }
                const auto hidden = scheme_object.value(QStringLiteral("hidden"));
                if (!hidden.isUndefined() && !hidden.isArray()) return fail(QStringLiteral("hidden must be an array"));
                for (const auto pane : hidden.toArray()) {
                    if (!pane.isString()) return fail(QStringLiteral("hidden entries must be strings"));
                    if (const auto id = pane_from_name(pane.toString())) scheme.hidden.insert(*id);
                }
                const auto cells = scheme_object.value(QStringLiteral("cells"));
                if (!cells.isUndefined() && !cells.isArray()) return fail(QStringLiteral("cells must be an array"));
                for (const auto cell : cells.toArray()) {
                    if (!cell.isObject()) return fail(QStringLiteral("cells entries must be objects"));
                    const QJsonObject object = cell.toObject();
                    const auto pane = pane_from_name(object.value(QStringLiteral("pane")).toString());
                    const auto number = [&object](const char* key) {
                        const auto value = object.value(QLatin1String(key));
                        return value.isDouble() ? value.toInt() : -1;
                    };
                    // 不认识的面板(新版本加的)丢掉;格子读坏了在 normalize 里整份回到默认布局。
                    if (pane)
                        scheme.cells.push_back(
                            {*pane, number("row"), number("column"), number("height"), number("width")});
                }
                const auto options = scheme_object.value(QStringLiteral("options"));
                if (!options.isUndefined()) {
                    if (!options.isObject()) return fail(QStringLiteral("options must be an object"));
                    const auto parsed = options_from_json(options.toObject());
                    if (!parsed) return fail(QStringLiteral("options have wrong field types"));
                    scheme.options = *parsed;
                }
                setup.schemes.push_back(std::move(scheme));
            }
            profile.roles[*role] = std::move(setup);
        }
        book.operators.push_back(std::move(profile));
    }
    normalize(&book);
    return book;
}

}  // namespace rm_terminal
