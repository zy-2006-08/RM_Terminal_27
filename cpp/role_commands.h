#pragma once

#include "domain.h"

#include <QColor>
#include <QRect>
#include <QString>
#include <cstdint>
#include <string>

class QPainter;

namespace rm_terminal {

// 角色面板的发送侧纯逻辑:编码、限频、按住确认、回显判定。
//
// 刻意不碰 QWidget,也不读时钟:面板把 `now` 从 tick() 传进来,测试才能把一秒
// 的按住过程写成三行断言,而不是真的 sleep 一秒。
//
// 编码函数返回序列化后的 protobuf 字节,topic 名与消息名相同
// ("AssemblyCommand" / "DartCommand" / "SentryCtrlCommand" / "CommonCommand" /
// "RobotPerformanceSelectionCommand" / "RuneActivateCommand")。

// rm::AssemblyCommand(2.2.21)。operation:0 开始兑换 1 确认装配 2 取消装配。
// difficulty 只在 operation 0 有意义,但一律写进报文:proto3 的 optional 带显式
// 存在性,少写一个字段和写 0 在服务器侧是两件事,统一写满少一类联调歧义。
std::string encode_assembly(std::uint32_t operation, std::uint32_t difficulty);

// rm::DartCommand(2.2.29)。target_id:1 前哨站 2 基地固定 3 基地随机固定
// 4 基地随机移动 5 基地末端移动。
std::string encode_dart(std::uint32_t target_id, bool open, bool launch_confirm);

// rm::SentryCtrlCommand(2.2.31)。command_id:1 补血点补弹 3 远程补弹 4 远程回血
// 5 确认复活 6 确认花费金币复活 7/8/9 进攻/防御/移动 10/11/12 强化进攻/防御/移动。
std::string encode_sentry(std::uint32_t command_id);

// rm::CommonCommand(2.2.25)。cmd_type:1 兑换 17mm 发弹量(param 为发数,10 的倍数)
// 2 兑换 42mm 发弹量(param 为发数) 3 确认复活 4 兑换立即复活 5 远程兑换发弹量
// 6 远程兑换血量。3/4/6 协议没写 param,一律写 0。
std::string encode_common(std::uint32_t cmd_type, std::uint32_t param);

// rm::RobotPerformanceSelectionCommand(2.2.23),步兵赛前选性能体系。枚举见 RobotStaticStatus:
// shooter 1 冷却优先 2 爆发优先;chassis 1 血量优先 2 功率优先。两项一起写满。
// sentry_control 不写:它是指挥手给哨兵选的,写 0 在服务器侧就是「自动控制」。
std::string encode_performance(std::uint32_t shooter, std::uint32_t chassis);

// rm::RuneActivateCommand(2.2.26)。协议只定义了 activate=1(开启)。
std::string encode_rune_activate();

// 协议 2.1:AssemblyCommand、DartCommand、SentryCtrlCommand、CommonCommand、
// RobotPerformanceSelectionCommand、RuneActivateCommand 都是触发式,最高 10Hz。
// 每一类指令一个闸门 —— 全局共用一个会让飞镖的选目标把哨兵的复活吞掉。
class CommandGate {
public:
    explicit CommandGate(MonotonicMs min_interval_ms = 100);

    // 放行则记下这一刻并返回 true;被限频则什么都不改,调用方直接丢弃这次点击。
    bool allow(MonotonicMs now);
    void reset();
    MonotonicMs interval() const { return interval_ms_; }

private:
    MonotonicMs interval_ms_;
    MonotonicMs last_sent_ = 0;
    bool sent_once_ = false;
};

// 按住一秒才生效的按钮(确认发射、花金币复活、取消装配,见 role-layouts D4)。
// 时长没到就松手一律不触发:这类操作撤不回,误触一次就是一局。
class HoldGate {
public:
    explicit HoldGate(MonotonicMs hold_ms = 1000);

    void press(MonotonicMs now);
    // 返回是否触发。按住时长不足 hold_ms 时返回 false,且不留残留状态。
    bool release(MonotonicMs now);
    void cancel();

    bool pressed() const { return pressed_; }
    // 0..1,未按住时为 0。面板用它画按钮上的进度。
    double progress(MonotonicMs now) const;
    // 已经按够时长,松手即发。按钮据此改写提示文字。
    bool ready(MonotonicMs now) const;
    MonotonicMs holdMs() const { return hold_ms_; }

private:
    MonotonicMs hold_ms_;
    MonotonicMs pressed_at_ = 0;
    bool pressed_ = false;
};

enum class EchoStatus {
    Idle,         // 还没发过
    Pending,      // 已发出,等服务器回显
    Confirmed,    // 状态变了(或显式结果成功)
    Unconfirmed,  // 超时仍无变化
    Failed,       // 显式结果报了失败码
};

struct EchoResult {
    EchoStatus status = EchoStatus::Idle;
    std::uint32_t code = 0;  // 仅 Failed 有意义
};

// 回显判定。两种回显源:
//   1. 隐式 —— 由快照算出的指纹变了就算确认(飞镖看 gate/target,装配看装配步骤和
//      最近一条 Event 15 的时刻)。
//   2. 显式 —— SentryCtrlResult 带 command_id 和 result_code。
// 两者都没有的情况下,超过 timeout_ms 报「未确认」,而不是假装成功。
class EchoWatch {
public:
    explicit EchoWatch(MonotonicMs timeout_ms = 2000);

    // baseline 是发送当时的状态指纹;之后指纹与它不同即视为服务器已响应。
    void sent(MonotonicMs now, std::uint32_t tag, std::uint64_t baseline);
    // 每帧调用。已经有结论就保持不变,直到下一次 sent()。
    EchoResult update(MonotonicMs now, std::uint64_t state);
    // 显式结果。tag 对不上说明是别的指令的回复,忽略。
    EchoResult applyResult(MonotonicMs now, std::uint32_t tag, std::uint32_t code);

    EchoResult result() const { return result_; }
    std::uint32_t tag() const { return tag_; }
    void clear();

private:
    MonotonicMs timeout_ms_;
    MonotonicMs sent_at_ = 0;
    std::uint64_t baseline_ = 0;
    std::uint32_t tag_ = 0;
    EchoResult result_;
};

QString echo_status_text(const EchoResult& echo);

// 三个面板共用的按钮外观。颜色字号全部取自 cpp/theme.h —— 这是面板里唯一的
// 按钮样式来源,写第二份就会和现有卡片的配色漂移。
QString command_button_style();
// 按住确认按钮:progress 为 0..1,用渐变把已按住的比例填出来。
QString hold_button_style(double progress);

// 三个面板共用的卡片画法,照 BattleAnalysisPane 的 skewPath + kSurfaceRaised。
// 放在这里而不是各自的 .cpp 里:同一套「标签在上、数值在下」的格子出现在三个
// 面板上,写三份必然漂移。只用 QtGui 的 QPainter,不碰 QWidget。
namespace role_paint {

void tile(QPainter& painter, const QRect& box, const QString& label, const QString& value,
          const QColor& ink);
void row(QPainter& painter, const QRect& box, const QString& label, const QString& value,
         const QColor& ink);
// done 为真时用 accent 填底,否则只留描边 —— 三步进度就靠这个区分已完成和待完成。
void chip(QPainter& painter, const QRect& box, const QString& text, bool done,
          const QColor& accent);

}  // namespace role_paint

}  // namespace rm_terminal
