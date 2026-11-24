#include "rune_key.h"

#include <algorithm>

namespace rm_terminal {

namespace {

constexpr std::uint32_t kStageMatch = 4;
constexpr std::uint32_t kIdle = 1;
constexpr std::uint32_t kActivating = 2;
constexpr std::uint32_t kActivated = 3;
constexpr int kMatchSec = 420;
constexpr int kBigFromSec = 180;
constexpr MonotonicMs kSentShowMs = 3000;

}  // namespace

bool RuneKey::bigPhase() const {
    return view_.remaining && kMatchSec - *view_.remaining >= kBigFromSec;
}

void RuneKey::resetRound() {
    last_status_.reset();
    opened_at_.reset();
    used_small_ = 0;
    used_big_ = 0;
    activated_big_ = 0;
}

void RuneKey::update(const SelfCommandView& view, MonotonicMs now) {
    const bool playing = view.stage == std::optional<std::uint32_t>(kStageMatch);
    // 换局:剩余时间往回跳了一大截。
    const bool new_round = view_.remaining && view.remaining && *view.remaining > *view_.remaining + 5;
    if (!playing || new_round) resetRound();
    view_ = view;
    if (!playing) {
        pressed_at_.reset();
        sent_at_.reset();
        return;
    }
    const auto status = view.rune_status;
    if (status == std::optional<std::uint32_t>(kActivating) && last_status_ != status) {
        // 本队谁开的都算用掉一次:状态同步是全队的。
        if (bigPhase()) ++used_big_;
        else ++used_small_;
        opened_at_ = now;
    }
    if (status == std::optional<std::uint32_t>(kActivated) &&
        last_status_ == std::optional<std::uint32_t>(kActivating) && bigPhase())
        ++activated_big_;
    if (status != std::optional<std::uint32_t>(kActivating)) opened_at_.reset();
    if (sent_at_ && (status == std::optional<std::uint32_t>(kActivating) || now - *sent_at_ >= kSentShowMs))
        sent_at_.reset();
    if (status) last_status_ = status;
}

int RuneKey::chances() const {
    if (!view_.remaining) return 0;
    const int elapsed = kMatchSec - *view_.remaining;
    if (elapsed < kBigFromSec) return std::max(0, (elapsed >= 0 ? 1 : 0) + (elapsed >= 90 ? 1 : 0) - used_small_);
    return std::max(0, 1 + (elapsed >= 255 ? 1 : 0) + (elapsed >= 330 ? 1 : 0) - used_big_);
}

bool RuneKey::canOpen() const {
    return view_.infantry && view_.stage == std::optional<std::uint32_t>(kStageMatch) &&
           view_.rune_status == std::optional<std::uint32_t>(kIdle) && chances() > 0;
}

void RuneKey::press(MonotonicMs now) {
    if (pressed_at_ || !canOpen()) return;
    pressed_at_ = now;
    fired_ = false;
}

void RuneKey::release() {
    pressed_at_.reset();
    fired_ = false;
}

bool RuneKey::due(MonotonicMs now) const {
    return pressed_at_ && !fired_ && now - *pressed_at_ >= kHoldMs && canOpen();
}

void RuneKey::sent(bool ok, MonotonicMs now) {
    fired_ = true;
    sent_at_ = now;
    sent_ok_ = ok;
}

RuneKey::Display RuneKey::display(MonotonicMs now) const {
    Display shown;
    shown.visible = view_.infantry && view_.stage == std::optional<std::uint32_t>(kStageMatch);
    if (!shown.visible) return shown;
    const bool big = bigPhase();
    const QString name = big ? QStringLiteral("大能量机关") : QStringLiteral("小能量机关");
    const QString activated = big ? QStringLiteral("  已激活(%1)").arg(activated_big_) : QString();
    shown.tooltip = QStringLiteral("长按 F 一秒开启，开启后撤不回。剩余次数按比赛时间和本机看到的开启次数推算，"
                                   "终端中途打开时可能偏多");
    const auto status = view_.rune_status;
    if (sent_at_ && status != std::optional<std::uint32_t>(kActivating)) {
        shown.text = sent_ok_ ? QStringLiteral("F · 开启指令已发出") : QStringLiteral("F · 没发出去（断线或按得太快）");
        return shown;
    }
    if (!status) {
        shown.text = QStringLiteral("F · 能量机关 --");
        return shown;
    }
    if (*status == kActivating) {
        QString text = QStringLiteral("F · 已开启");
        if (opened_at_) {
            const MonotonicMs left = std::max<MonotonicMs>(0, kActivatingMs - (now - *opened_at_));
            text = QStringLiteral("F · 已开启，%1s后结束").arg((left + 999) / 1000);
        }
        if (big && view_.rune_rings && view_.rune_arms)
            text += QStringLiteral("  平均环数/总臂数 %1 / %2").arg(*view_.rune_rings, 0, 'f', 1).arg(*view_.rune_arms);
        shown.text = text + activated;
        return shown;
    }
    if (*status == kActivated) {
        shown.text = QStringLiteral("F · %1已激活，增益生效中").arg(name) + activated;
        return shown;
    }
    shown.lit = canOpen();
    shown.text = QStringLiteral("F · 长按开启%1 (%2)").arg(name).arg(chances()) + activated;
    if (pressed_at_) shown.progress = std::clamp(static_cast<double>(now - *pressed_at_) / kHoldMs, 0.0, 1.0);
    return shown;
}

}  // namespace rm_terminal
