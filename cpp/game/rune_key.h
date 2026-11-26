#pragma once

#include "self_command_view.h"

#include <QString>
#include <optional>

namespace rm_terminal
{

    // 长按 F 开能量机关,照官方选手端(手册第 12 页):步兵长按 F 开启,开启后不可取消;状态写成
    // 「长按开启小能量机关 (1)」「已开启，20s后结束」。规则手册 5.6.3:比赛开始和 1 分 30 秒各给一次
    // 小能量机关机会,3 分钟起换成大能量机关,3 分、4 分 15 秒、5 分 30 秒各给一次,机会可累积;开启后
    // 20 秒内没打成就回到未激活。协议只同步能量机关现在的状态(2.2.27),不发还剩几次机会,所以次数
    // 按比赛时间和本机看到的开启次数推算。
    class RuneKey
    {
    public:
        static constexpr MonotonicMs kHoldMs = 1000;
        static constexpr MonotonicMs kActivatingMs = 20000;

        struct Display
        {
            bool visible = false;
            bool lit = false;      // 现在长按就能开
            double progress = 0.0; // 按住的进度 0–1
            QString text;
            QString tooltip;
        };

        void update(const SelfCommandView& view, MonotonicMs now);
        // F 键或状态条那一格按下、松开。不能开的时候按下不算。
        void press(MonotonicMs now);
        void release();
        bool holding() const { return pressed_at_.has_value(); }
        // 按满一秒、这次还没发过:该发开启指令了。发完调 sent()。
        bool due(MonotonicMs now) const;
        void sent(bool ok, MonotonicMs now);

        bool canOpen() const;
        // 推算的剩余机会。
        int chances() const;
        Display display(MonotonicMs now) const;

    private:
        bool bigPhase() const;
        void resetRound();

        SelfCommandView view_;
        std::optional<std::uint32_t> last_status_;
        std::optional<MonotonicMs> pressed_at_;
        bool fired_ = false;
        std::optional<MonotonicMs> sent_at_;
        bool sent_ok_ = false;
        std::optional<MonotonicMs> opened_at_;
        int used_small_ = 0;
        int used_big_ = 0;
        int activated_big_ = 0;
    };

} // namespace rm_terminal
