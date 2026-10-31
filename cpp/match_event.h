#pragma once

#include "domain.h"

namespace rm_terminal {

// 一条 Event(协议 2.2.7)翻译成事件栏里的一行,外加 Store 要据此更新的状态。
// 纯函数,不碰 Qt:文字用 UTF-8 的 std::string。
struct MatchEvent {
    std::uint32_t level = 0;    // 0 信息 1 提示 2 警告 3 严重
    std::uint32_t faction = 0;  // 横幅配色
    std::string text;
    // 编号未知或参数解析不了时为假:照样进事件栏(原样显示),但不产生副作用。
    bool understood = true;
    // 1 击杀:击杀数记给哪一方
    std::optional<std::uint32_t> killer_faction;
    // 9 飞镖命中:命中方阵营和目标 1–5
    std::optional<std::uint32_t> dart_hitter;
    std::uint32_t dart_target = 0;
    // 5–8 阵地易手:0 红方前哨阵地 1 蓝方前哨阵地 2 红方基地阵地 3 蓝方基地阵地,
    // zone_code 为 GameFields 的绝对编码(0 未控 1 红临时 2 红完全 3 蓝临时 4 蓝完全)
    std::optional<std::size_t> zone_index;
    std::uint32_t zone_code = 0;
};

// self_faction 用来把红蓝写成「我方/敌方」。参数解析不了时返回一条 level 0 的原样记录,
// 不产生任何副作用。
MatchEvent describe_event(std::int32_t event_id, const std::string& param,
                          std::uint32_t self_faction);

// 规则手册 5.7.3:命中前哨站、基地固定目标、基地随机固定目标时,第 1–4 次分别遮挡
// 10/5/3/2 秒;命中基地随机移动目标、末端移动目标每次 10 秒;连续命中时间叠加。
// prior_fixed_hits 是本局此前已被命中前三类目标的次数。第 5 次及以后手册没写,按 2 秒算:
// 多遮挡一会儿只是多看一会儿图传,少算则会在操作手看不见时切回信息页。
MonotonicMs dart_blind_ms(std::uint32_t target, int prior_fixed_hits);

}
