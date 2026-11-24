#pragma once

#include "domain.h"

#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <cstdint>
#include <optional>
#include <vector>

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

// QML 侧的配色键:"red" / "blue" / "draw",没有结果时空串。传字符串而不是枚举值,
// 是为了让 QML 不必知道 C++ 枚举的整数取值。
QString game_result_outcome(GameResult result);

// 结束原因的中文说法。协议 end_reason:1 基地摧毁 2 控制度 3 总伤害 4 总剩余血量
// 5 平局 6 异常终止 7 主裁判定;非结算阶段为 255。
//
// 措辞必须跟着 winner 走,因为同一个编码对两方说的是相反的事:编码 1 是「有一方
// 基地被击毁」,按规则手册 5.8 第 1 条,获胜方是基地剩余血量高的那方,也就是**败方**
// 基地被击毁 —— 直接写「红方基地被击毁」而不看 winner,有一半的局会说反。
//
// 返回空串表示「说不出来」:编码缺失、是 255 这个非结算占位值、超出已定义范围,
// 或者编码与 winner 自相矛盾(平局却报了分胜负的编码)。调用方据此整行不画 ——
// 结算屏上一句编造的理由比没有理由糟得多。
QString game_result_reason(GameResult result, const Field<std::uint32_t>& end_reason);

// 结算面板里的一行对比项。红蓝两侧分开存,缺失(nullopt)与真实的 0 必须可分:
// 0 是「没打出伤害」,缺失是「服务器没给这一帧」,画成同一根空条会被读成前者。
struct SettlementRow {
    QString label;
    std::optional<std::int64_t> red;
    std::optional<std::int64_t> blue;
    // 非空时代替数字显示(能量机关是状态而不是数)。
    QString red_text;
    QString blue_text;
    // 条长的归一化分母。0 表示「没有量程」,由两侧较大值自己定标。
    std::int64_t scale = 0;
    // qrc 路径,空 = 这一行不配图标。
    QString icon;
};

// 侧栏里的一台机器人。协议没有逐台伤害统计,所以侧栏只讲剩余血量。
struct SettlementRobot {
    std::uint32_t id = 0;      // 协议编号(蓝方 +100)
    std::uint32_t number = 0;  // 显示编号 1–7
    QString cls;               // 重装 / 工程 / 步兵 / 空中 / 哨兵
    std::optional<std::uint32_t> hp;
    std::optional<std::uint32_t> max_hp;
};

struct SettlementModel {
    GameResult result = GameResult::None;
    QString title;
    QString reason;
    std::optional<std::uint32_t> round;
    std::optional<std::uint32_t> total_rounds;
    std::optional<std::uint32_t> red_score;
    std::optional<std::uint32_t> blue_score;
    // 本局比赛用时。协议在结算阶段只给**结算阶段**自己的 stage_elapsed_sec,
    // 拿它当比赛时长会把「结算开始 3 秒」显示成「比赛打了 3 秒」,所以这里一直是
    // nullopt,界面显示「--」。等协议补上比赛阶段用时再接。
    std::optional<std::int32_t> duration_sec;
    std::vector<SettlementRow> rows;
    std::vector<SettlementRobot> red_robots;
    std::vector<SettlementRobot> blue_robots;
};

// 结算面板的全部数据。只读快照,不猜任何数:每个 Field 非 Valid 一律落成 nullopt。
SettlementModel settlement_model(const Snapshot& snapshot);

// 喂给 QML 根对象的那一份。缺失用 hasX=false 表达而不是用 0 顶替 ——
// QML 侧据此画「--」。
QVariantMap settlement_variant(const SettlementModel& model);

// 按序号排出目录里真实存在的帧,返回可直接喂给 QML Image.source 的 URL。
//
// 必须枚举真实文件而不是按 first..last 拼序号:素材序号不连续(红胜 17~174
// 之间缺 27 个),拼出来的路径会指向不存在的文件并卡住动画。
//
// 找不到素材时返回空列表,调用方退回静态标题 —— 逐帧素材不进二进制,按设计
// 就可能在某些部署下缺失,那不是错误。
QStringList game_result_frame_urls(GameResult result);

}  // namespace rm_terminal
