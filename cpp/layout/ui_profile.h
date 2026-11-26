#pragma once

#include "layout_profile.h"
#include "seat.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace rm_terminal
{

    // 操作手个性化界面。层级和扫地机 App 的「清洁模式 → 模式里的开关 → 每项的细项」一样:
    // 操作手(人) → 席位 → 方案 → 方案里藏哪些面板 + 各面板的细项。
    //
    // 只存「和席位默认布局不同的地方」:藏了哪几块、细项改成了什么。面板放在哪一格仍由
    // layout_profile.cpp 的席位布局决定,藏掉的格子由 apply_hidden 让相邻面板补上,所以
    // 换显示器、换分辨率都不会让保存的方案失效。

    constexpr int kUiProfileVersion = 1;
    constexpr std::size_t kMaxOperators = 20;
    constexpr std::size_t kMaxSchemesPerRole = 12;
    // 按 Unicode 码点数算,和战术提醒的文字上限一样不按字节。
    constexpr int kMaxUiNameChars = 12;
    constexpr int kMinMapBackgroundPercent = 20;
    constexpr int kMaxMapBackgroundPercent = 100;
    constexpr int kDefaultMapBackgroundPercent = 90;

    // 每块面板的细项。全部有默认值:旧文件少了某个键时取默认,而不是判成读不懂。
    struct UiOptions
    {
        int map_background_percent = kDefaultMapBackgroundPercent;
        bool map_show_enemies = true;
        bool map_show_marks = true;
        bool enemy_low_hp_pulse = true;
        // 战场数据分析只画控制度,数字放大。
        bool analysis_control_only = false;
        bool alert_voice = true;
        // alert_kind_names() 里的名字。只能在配置文件 alerts_disabled 之外**再多关**几种,
        // 配置文件关掉的这里打不开:那是队里统一定的。
        QStringList alerts_muted;
        // 「下一步」。关掉它是在面板开关里藏掉;这里是开着时怎么显示:
        // next_mode 0 一直显示,1 有急事才弹出(阵亡、堡垒被偷、基地被打时才弹到画面中间)。
        int next_mode = 0;
        int next_count = 3;     // 显示几件,1~3
        int next_size = 1;      // 卡片大小,0 紧凑 1 标准 2 大
        bool next_voice = true; // 第 1 件换了就念出来
        int next_dwell_sec = 3; // 每张至少停几秒,1~10

        bool operator==(const UiOptions& other) const;
        bool operator!=(const UiOptions& other) const { return !(*this == other); }
    };

    struct UiScheme
    {
        // 内置方案用固定 id(standard / novice / minimal / video),自建方案是 custom-N。
        QString id;
        QString name;
        std::set<PaneId> hidden;
        UiOptions options;
        // 界面设置里排出来的位置(16 × 9 格)。空 = 用席位的默认布局。藏起来的面板也留着它的格子,
        // 再打开时回到原处;主界面上它空出来的地方由旁边的面板补上。
        std::vector<PaneSlot> cells;
    };

    struct RoleSetup
    {
        QString active;
        // 内置方案也在这里存一份:操作手改过的内置方案要记住改动,「恢复默认」再改回来。
        std::vector<UiScheme> schemes;
    };

    struct OperatorProfile
    {
        QString name;
        std::map<Role, RoleSetup> roles;
    };

    struct UiProfileBook
    {
        QString last_operator;
        std::vector<OperatorProfile> operators;
    };

    // 能不能藏:花名册、地图和要按的指令面板(本车、装配、飞镖、哨兵)一律不能藏,
    // 比赛中找不到这些比画面挤更要命。只有战场事件、战场数据分析和图传小窗能藏。
    bool pane_hideable(PaneId pane);
    // 设置页里的中文名。
    QString pane_title(PaneId pane);
    std::optional<PaneId> pane_from_name(const QString& name);

    // 事件提醒种类的中文名,顺序同 alert_kind_names()。
    QString alert_kind_title(const QString& kind);

    bool is_builtin_scheme(const QString& id);
    std::vector<UiScheme> builtin_schemes(Role role);
    std::optional<UiScheme> builtin_scheme(Role role, const QString& id);
    QString scheme_description(const UiScheme& scheme);
    RoleSetup default_role_setup(Role role);
    OperatorProfile default_operator(const QString& name);
    QString default_operator_name();

    // 补齐内置方案、清掉不认识的面板、修正越界的细项,保证 active 指向存在的方案。
    // 读文件和导入都走它:程序更新加了新方案、删了旧面板时,旧文件照样能用。
    void normalize(RoleSetup* setup, Role role);
    void normalize(UiProfileBook* book);

    const UiScheme& active_scheme(const RoleSetup& setup);

    // 名字合法时返回空串,否则返回给操作手看的原因。
    QString name_problem(const QString& name);

    // 把藏掉的格子交给相邻面板(close_hole):贴着空位一边、正好把这条边铺满的面板一起长过去,
    // 先不动地图、不让花名册横着长,补不上再放开。
    LayoutProfile apply_hidden(const LayoutProfile& base, const std::set<PaneId>& hidden);
    // 这个方案在主界面上的布局:拖过位置就按拖的排,没拖过就按席位默认的排,再按 hidden 补空。
    // 图传单独占了一格时就不再装进「本车」面板里。全藏光了按没藏排。
    // fill_holes 为假时不补空(只给测试看藏掉以后空出哪些格子)。
    LayoutProfile scheme_layout(Role role, const UiScheme& scheme, bool fill_holes = true);
    // 一份格子能不能用:都在 16 × 9 以内、互不重叠、都是这个席位有的面板、至少一块。
    bool cells_valid(Role role, const std::vector<PaneSlot>& cells);
    // 主界面按这个方案排完还空着几格(边对不齐、旁边谁都补不上的)。测试用它确认画面铺满。
    int unfilled_units(Role role, const UiScheme& scheme);

    QJsonObject book_to_json(const UiProfileBook& book);
    // 读不懂(版本不对、类型不对、名字不合法)时返回 nullopt 并写 error。
    // 不认识的面板名、提醒种类在 normalize 里丢掉,不算读不懂。
    std::optional<UiProfileBook> book_from_json(const QJsonObject& root, QString* error);

} // namespace rm_terminal
