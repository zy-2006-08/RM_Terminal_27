#pragma once

#include "domain.h"

#include <QString>
#include <QStringList>
#include <optional>
#include <vector>

namespace rm_terminal {

// 「下一步」:让慌乱的操作手一眼看到接下来对我方最有利的几件事。
//
// 先后照《下一步先后表》:先看能不能做、做了有没有用,再按表里的先后排。表里的顺序就是 NextRule 的顺序,
// 只有一个例外(表第三节第 2 题):补血和打得掉的集火同时成立时,集火排到补血前面。
// 规则上做不了的事(没弹还叫人开枪、敌方基地有 100% 防御还叫人打)不出卡片。
// 阵亡不出卡片:复活是唯一能做的事,不用占一格。

enum class NextRule {
    AttackBase,           // 1 敌方基地没防御,弹够打掉
    AttackBaseAfterAmmo,  // 2 同上,但没弹:先补弹,补完打基地
    DefendFortress,       // 3 我方堡垒被偷
    DefendBase,           // 4 我方基地在掉血,而且没有 100% 防御
    Heal,                 // 5 本机血少
    BuyAmmo,              // 6 弹少
    InstallLauncher,      // 7 重装有权限还没装上 42mm
    VerifyLauncher,       // 8 重装装上了,还差一发验证弹
    AttackOutpost,        // 9 敌方前哨站弹够打掉
    FocusEnemy,           // 10 集火残血敌人
    OpenRune,             // 11 开能量机关(步兵)
    Assemble,             // 12 重装去装配
    DefendZone,           // 13 我方阵地被敌方临时控制
    HoldZone,             // 14 我方临时控制的阵地(前哨站倒了就写让开 5 秒)
    AttackBaseLongShot,   // 15 敌方基地没防御,但一时打不掉
    TakeZone,             // 16 控制度落后
    Endgame,              // 17 最后 60 秒:现在结束是输是赢
};

struct NextCard {
    NextRule rule = NextRule::AttackBase;
    QString category;  // 卡片左上角的小标签:进攻、防守、补给……
    QString title;     // 动词开头的一句话
    QString reason;    // 带数字的原因
    // 卡片指向界面上的哪里:集火的那台敌方车(花名册那一行标同一个编号),
    // 或者「能量机关 · 按住」按钮。
    std::optional<std::uint32_t> enemy_id;
    // 集火:现在的弹打得掉它(弹够、4 轮热量内打完)。只有这样才排到补血前面。
    bool finish_off = false;
    bool rune_button = false;
    QString speech;    // 第 1 件念出来时念的固定句子(语音是赛前合成好的,不能带实时数字)
    bool urgent = false;
};

// 规则手册表 5-7:50 + ROUNDUP((420 - 剩余秒数) / 60 × 20)。
std::uint32_t remote_heal_cost(std::int32_t remaining_sec);

// 缺省按没有增益算。
struct NextBuffs {
    bool self_weak = false;
    double self_attack = 1.0;
    int enemy_structure_defense = 0;
};

// 从一帧快照里挑出所有成立的事,按 NextRule 的顺序排好。base_hit 是「我方基地这几秒在掉血」,
// 由调用方(NextStepBoard)跨帧判断。
std::vector<NextCard> next_candidates(const Snapshot& snapshot, bool base_hit, const NextBuffs& buffs = {});

// 按表里的先后取前 count 件(带上面那个例外)。顺序固定,不让操作手拖。
std::vector<NextCard> rank_next(std::vector<NextCard> cards, std::size_t count);

// 每张卡至少停 dwell 毫秒,免得来回跳:期间排名变了也不换,除非出了更靠前的事,
// 或者正显示的某件事已经不成立了。同一件事的数字每帧照常更新。
class NextStepBoard {
public:
    std::vector<NextCard> update(const Snapshot& snapshot, MonotonicMs now, std::size_t count,
                                 MonotonicMs dwell_ms, const NextBuffs& buffs = {});

private:
    std::vector<NextCard> shown_;
    MonotonicMs shown_at_ = 0;
    std::optional<std::uint32_t> last_base_hp_;
    MonotonicMs base_hit_until_ = 0;
};

// 所有可能念到的句子,赛前一起合成。
QStringList next_step_phrases();

}  // namespace rm_terminal
