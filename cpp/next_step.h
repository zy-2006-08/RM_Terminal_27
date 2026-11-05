#pragma once

#include "domain.h"

#include <QString>
#include <QStringList>
#include <optional>
#include <vector>

namespace rm_terminal {

// 「下一步」:让慌乱的操作手一眼看到接下来对我方最有利的几件事。
//
// 不是算出来的最优解,是一张固定的先后顺序表:先保命,再守家,再抓机会,最后补给和抢分。
// 满足条件的事按这个顺序挑出前几件。五组的先后可以在界面设置里改,组内的先后固定。
// 雷达看不到的敌人不推荐去打,也不写距离。

enum class NextGroup { Survive, Defend, Opportunity, Supply, Score };
constexpr int kNextGroupCount = 5;
std::vector<NextGroup> default_next_order();
QString next_group_name(NextGroup group);   // 保命 / 守家 / 抓机会 / 补给 / 抢分
QString next_group_hint(NextGroup group);   // 「阵亡先复活,血少先回血」
QString next_group_key(NextGroup group);    // 存盘用的英文名
std::optional<NextGroup> next_group_from_key(const QString& key);

enum class NextRule {
    FreeRevive,
    PaidRevive,
    WaitRevive,
    RemoteHeal,
    LeaveCombat,
    DefendFortress,
    DefendBase,
    DefendZone,
    FocusEnemy,
    OpenRune,
    HoldZone,
    BuyAmmo,
    TakeZone,
};

struct NextCard {
    NextRule rule = NextRule::FreeRevive;
    NextGroup group = NextGroup::Survive;
    QString category;  // 卡片左上角的小标签:复活、防守、进攻……
    QString title;     // 动词开头的一句话
    QString reason;    // 带数字的原因
    // 卡片指向界面上的哪里:集火的那台敌方车(花名册那一行标同一个编号),
    // 或者「能量机关 · 按住」按钮。
    std::optional<std::uint32_t> enemy_id;
    bool rune_button = false;
    QString speech;    // 第 1 件念出来时念的固定句子(语音是赛前合成好的,不能带实时数字)
    // 阵亡、堡垒被偷、基地被打:「有急事才弹出」只在有这种事时弹出来。
    bool urgent = false;
};

// 规则手册表 5-7:50 + ROUNDUP((420 - 剩余秒数) / 60 × 20)。
std::uint32_t remote_heal_cost(std::int32_t remaining_sec);

// 从一帧快照里挑出所有满足条件的事,还没排序。base_hit 是「我方基地这几秒在掉血」,
// 由调用方(NextStepBoard)跨帧判断。
std::vector<NextCard> next_candidates(const Snapshot& snapshot, bool base_hit);

// 按组的先后排序,组内按规则的固定先后,取前 count 件。
std::vector<NextCard> rank_next(std::vector<NextCard> cards, const std::vector<NextGroup>& order,
                                std::size_t count);

// 每张卡至少停 dwell 毫秒,免得来回跳:期间排名变了也不换,除非出了更急的事(组更靠前),
// 或者正显示的某件事已经不成立了(复活了、堡垒没人偷了)。同一件事的数字每帧照常更新。
class NextStepBoard {
public:
    std::vector<NextCard> update(const Snapshot& snapshot, MonotonicMs now, const std::vector<NextGroup>& order,
                                 std::size_t count, MonotonicMs dwell_ms);

private:
    std::vector<NextCard> shown_;
    MonotonicMs shown_at_ = 0;
    std::optional<std::uint32_t> last_base_hp_;
    MonotonicMs base_hit_until_ = 0;
};

// 所有可能念到的句子,赛前一起合成。
QStringList next_step_phrases();

}  // namespace rm_terminal
