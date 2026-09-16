#pragma once

#include "domain.h"

#include <QString>
#include <QStringList>
#include <cstdint>
#include <optional>

namespace rm_terminal {

// 结算动画的四种结果,取值语义与上游 GameResultWidget::GameResult 对齐,
// 以便复用同一批逐帧素材目录。
// Terminated 目前无法由本协议判定:proto 只说明 end_reason「非结算阶段为 255」,
// 没有定义裁判终止的具体取值,靠猜一个编码会把普通结束播成终止动画。枚举与素材
// 目录保留,等协议补齐语义后接上。
enum class GameResult {
    None,
    RedWin,
    BlueWin,
    Draw,
    Terminated,
};

struct GameResultInputs {
    Freshness match_freshness = Freshness::NeverReceived;
    std::optional<std::uint32_t> stage;
    std::optional<std::uint32_t> winner;
};

GameResultInputs game_result_inputs(const Snapshot& snapshot);

// 协议 winner: 0平局 1红胜 2蓝胜 255非结算。
//
// 只有阶段真的进了结算(5)且链路可信才出结果:链路不可信时 winner 是最后一次
// 收到的值,拿它播一段「红方胜利」会把上一局的结果当本局事实宣布。
GameResult resolve_game_result(const GameResultInputs& inputs);

// 逐帧素材的目录名。Draw 返回空 —— 上游没有平局动画,平局只出静态结果。
QString game_result_frame_dir(GameResult result);

QString game_result_title(GameResult result);

// 按序号排出目录里真实存在的帧,返回可直接喂给 QML Image.source 的 URL。
//
// 必须枚举真实文件而不是按 first..last 拼序号:素材序号不连续(红胜 17~174
// 之间缺 27 个),拼出来的路径会指向不存在的文件并卡住动画。
//
// 找不到素材时返回空列表,调用方退回静态标题 —— 逐帧素材不进二进制,按设计
// 就可能在某些部署下缺失,那不是错误。
QStringList game_result_frame_urls(GameResult result);

}  // namespace rm_terminal
