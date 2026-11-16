#pragma once

#include "buff_ledger.h"
#include "domain.h"

#include <QString>
#include <QStringList>

#include <array>
#include <optional>
#include <vector>

namespace rm_terminal {

// 大局:指挥手看趋势用的推算,规则见 大局判断表.md(按规则手册 5.6.2 控制度、5.8 胜负判定)。
// 纯逻辑:输入快照和这一局记下来的每秒样本,输出几句话和一条趋势线,不碰界面。

// 每个比赛秒一笔,全按红蓝存。终端中途打开的从打开那一秒记起。
struct MatchSample {
    std::int32_t elapsed = 0;
    std::optional<std::int32_t> countdown;
    std::optional<std::uint32_t> red_control, blue_control;
    std::optional<std::uint32_t> red_base, blue_base;
    std::optional<std::uint32_t> red_outpost, blue_outpost;
    std::optional<std::uint32_t> red_damage, blue_damage;
    std::optional<std::uint32_t> red_hp, blue_hp;  // 4 台车血量加起来
    std::optional<std::uint32_t> economy;          // 我方剩余金币
    std::optional<std::uint32_t> enemy_gold;       // 雷达情报里的敌方剩余金币
    // 红前哨、蓝前哨、红基地、蓝基地阵地,编码同 GameFields:1 红临时 2 红完全 3 蓝临时 4 蓝完全
    std::array<std::optional<std::uint32_t>, 4> zones{};
};

struct MatchNote {
    std::int32_t elapsed = 0;
    std::optional<std::int32_t> countdown;
    QString text;
};

// 一局的流水:每秒一笔样本,加上战场事件和提醒。换局或回到准备阶段就清掉重记。
class MatchLog {
public:
    // 每个刷新周期调一次。返回 true 表示这一刻刚进入结算,该出复盘报告了(一局只返回一次)。
    bool observe(const Snapshot& snapshot);
    // 提醒横幅也记进流水(复盘报告的关键事件里有)。
    void note(const QString& text);

    const std::vector<MatchSample>& samples() const { return samples_; }
    const std::vector<MatchNote>& notes() const { return notes_; }
    std::optional<std::uint32_t> round() const { return round_; }
    std::optional<std::uint32_t> self() const { return self_; }
    // 进入结算那一刻的裁判系统结果:0 平局 1 红胜 2 蓝胜;end_reason 见 proto。
    std::optional<std::uint32_t> winner() const { return winner_; }
    std::optional<std::uint32_t> endReason() const { return end_reason_; }

private:
    void reset();

    std::vector<MatchSample> samples_;
    std::vector<MatchNote> notes_;
    std::optional<std::uint32_t> round_;
    std::optional<std::uint32_t> self_;
    std::optional<std::uint32_t> stage_;
    std::optional<std::uint32_t> winner_, end_reason_;
    std::optional<std::int32_t> countdown_;
    MonotonicMs last_event_at_ = 0;
    bool reported_ = false;
};

enum class Leader { Unknown, Ours, Theirs, Even };

struct Verdict {
    Leader leader = Leader::Unknown;
    QString text;  // 「我方赢 · 控制度 120:85」,数都是我方在前
};

struct Overview {
    bool in_match = false;
    Verdict now;      // 现在结束
    Verdict at_end;   // 照这样打到 7:00
    QString flip;     // 翻盘点
    bool flip_urgent = false;  // 照这样我方输:标红
    QString buffs;    // 双方能量机关还剩几秒、堡垒有没有人偷
    QString next_mark;
    // 控制度差(我方减敌方),每秒一个点:(比赛第几秒, 差)。projection 是照现在的速度每秒多多少。
    std::vector<std::pair<std::int32_t, std::int64_t>> trend;
    std::optional<double> projection;
    std::int32_t elapsed = 0;
    std::int32_t match_sec = 420;
    // 标题栏右边:「控制度每秒 我 +1 · 敌 +0」。
    QString rates;
};

// 按 4 个阵地算这一刻双方控制度每秒涨多少:{红, 蓝}。缺阵地数据时为空。
std::optional<std::pair<int, int>> control_rates(const std::array<std::optional<std::uint32_t>, 4>& zones);

// 「现在结束谁赢」按规则 5.8 的顺序比。复盘报告用最后一笔样本算同一句话。
Verdict judge(const MatchSample& sample, std::uint32_t ours);

Overview build_overview(const Snapshot& snapshot, const BuffBoard& buffs, const MatchLog& log, MonotonicMs now);

}  // namespace rm_terminal
