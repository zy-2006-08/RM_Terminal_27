#pragma once

#include "domain.h"

#include <QString>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace rm_terminal {

// 四条链路(裁判数据、车端数据、图传、控制器)各自的状态,以及上场前自检页。
// 纯逻辑:输入是采样好的数值,输出是要显示的文字,不碰网络和界面,所以每条判定
// 都能在单元测试里逐条钉住。
enum class LinkLevel { Ok, Warn, Down, Off };

struct LinkStatus {
    LinkLevel level = LinkLevel::Off;
    // 状态灯旁边的几个字。正常时为空:灯本身是绿的就够了,常驻文字只会被习惯性忽略。
    QString summary;
    // 自检页和悬停提示里的完整说明。
    QString detail;
    // 先查什么。正常时为空。
    QString hint;
};

// 判「没数据」的门槛。
constexpr MonotonicMs kRefereeSilentMs = 2000;   // GameStatus 5Hz,连丢 10 个周期
constexpr MonotonicMs kControllerSilentMs = 200;  // 控制器固件 25Hz,连丢 5 帧

struct RefereeSample {
    bool connected = false;
    // 连着时距离最后一条消息(或刚连上的时刻)过了多久。
    std::optional<MonotonicMs> silent_ms;
    double messages_per_s = 0;
};

struct CarSample {
    Freshness freshness = Freshness::NeverReceived;
    std::optional<MonotonicMs> silent_ms;
    // 车端数据走裁判数据那条 MQTT。那条断了,这里必然也收不到,提示要指向真正的源头。
    bool referee_down = false;
};

struct VideoSample {
    bool enabled = true;
    bool online = false;
    std::optional<MonotonicMs> silent_ms;
    double frames_per_s = 0;
};

struct ControllerSample {
    bool configured = false;
    bool open = false;
    QString error;
    std::optional<MonotonicMs> silent_ms;
    double frames_per_s = 0;
    std::uint64_t bad_frames = 0;
    std::array<float, 6> joints{};
    bool can_forward = false;
};

LinkStatus referee_status(const RefereeSample& sample);
LinkStatus car_status(const CarSample& sample);
LinkStatus video_status(const VideoSample& sample);
LinkStatus controller_status(const ControllerSample& sample);

struct LinkReport {
    LinkStatus referee;
    LinkStatus car;
    LinkStatus video;
    LinkStatus controller;
};

QString link_level_name(LinkLevel level);

// 距离最近一次事件过了多久。`since` 是本轮计时的起点(例如刚连上的时刻):重连之后
// 第一条消息到达前,不能拿断线前的旧时间戳把它算成「已经沉默 12 秒」。
std::optional<MonotonicMs> silence(std::optional<MonotonicMs> last_event,
                                   std::optional<MonotonicMs> since, MonotonicMs now);

// 每满一秒刷新一次速率。按 250ms 的 tick 直接算,25Hz 的控制器会在 24/28 之间来回跳。
class RateMeter {
public:
    double update(std::uint64_t total, MonotonicMs now);

private:
    std::optional<MonotonicMs> base_at_;
    std::uint64_t base_total_ = 0;
    double rate_ = 0;
};

// 只在某条链路的级别或摘要变化时写一条 link_state 日志。按 tick 写会一分钟几百行,
// 把真正的断线淹没;验收脚本也靠这几条记录断言「几秒内变红、几秒内恢复」。
class LinkStateLog {
public:
    void observe(const LinkReport& report);

private:
    std::optional<LinkReport> last_;
};

enum class CheckLevel { Ok, Warn, Fail, Off };

struct CheckRow {
    QString name;
    CheckLevel level = CheckLevel::Off;
    QString detail;
    QString hint;
};

struct SelfCheckInputs {
    int self_robot_id = 0;
    LinkReport links;
    bool recording = false;
    QString recording_error;
    std::optional<std::int64_t> free_bytes;
    bool speech_available = false;
    // 没有语音时告诉操作手怎么装(语音后端的 unavailableReason)。
    QString speech_hint;
};

struct SelfCheck {
    std::vector<CheckRow> rows;
    int failures = 0;
    QString verdict;
};

// 「注意」不拦上场(例如这台电脑没有语音),只有「异常」才算进 failures。
SelfCheck build_self_check(const SelfCheckInputs& inputs);

// 「红方 3 号 步兵」。
QString self_robot_label(int robot_id);

}  // namespace rm_terminal
