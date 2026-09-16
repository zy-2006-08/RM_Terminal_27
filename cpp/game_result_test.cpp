#include "game_result.h"

#include <QCoreApplication>
#include <iostream>
#include <stdexcept>

using namespace rm_terminal;

namespace {

void check(bool condition, const char* name) {
    if (!condition) throw std::runtime_error(name);
    std::cout << "PASS " << name << '\n';
}

GameResultInputs settled(std::uint32_t winner) {
    GameResultInputs in;
    in.match_freshness = Freshness::Fresh;
    in.stage = 5;
    in.winner = winner;
    return in;
}

void winner_codes_map_to_the_upstream_result_enum() {
    check(resolve_game_result(settled(1)) == GameResult::RedWin, "winner=1 is RedWin");
    check(resolve_game_result(settled(2)) == GameResult::BlueWin, "winner=2 is BlueWin");
    check(resolve_game_result(settled(0)) == GameResult::Draw, "winner=0 is Draw");
}

// 255 是协议规定的「非结算阶段」占位值,不是一种结果。当成结果会在每局开始前
// 就播一段结算动画。
void the_non_settlement_sentinel_is_not_a_result() {
    check(resolve_game_result(settled(255)) == GameResult::None,
          "winner=255 yields no result");
}

// 结算动画只在阶段真的进入结算后播。否则一个残留的 winner 会在比赛中途盖屏。
void a_result_needs_the_settlement_stage() {
    GameResultInputs mid_match = settled(1);
    mid_match.stage = 4;
    check(resolve_game_result(mid_match) == GameResult::None,
          "a winner during stage 4 does not trigger the animation");

    GameResultInputs no_stage = settled(1);
    no_stage.stage.reset();
    check(resolve_game_result(no_stage) == GameResult::None,
          "an unknown stage never yields a result");
}

// 链路不可信时 winner 是最后一次收到的值。拿它宣布胜负等于把上一局的结果
// 当本局事实,这比不播动画糟得多。
void a_stale_link_never_announces_a_winner() {
    for (Freshness f : {Freshness::Stale, Freshness::NeverReceived}) {
        GameResultInputs in = settled(1);
        in.match_freshness = f;
        check(resolve_game_result(in) == GameResult::None,
              "an untrustworthy link yields no result");
    }
}

// 平局没有逐帧素材(上游同样如此),但仍然必须有可显示的标题,
// 否则平局会变成一个空盒子。
void draw_has_a_title_but_no_frames() {
    check(game_result_frame_dir(GameResult::Draw).isEmpty(), "Draw has no frame directory");
    check(!game_result_title(GameResult::Draw).isEmpty(), "Draw still states the outcome");
}

void every_result_with_frames_names_a_real_upstream_directory() {
    check(game_result_frame_dir(GameResult::RedWin) == QStringLiteral("red_win_zh"),
          "RedWin uses the upstream red_win_zh frames");
    check(game_result_frame_dir(GameResult::BlueWin) == QStringLiteral("blue_win_zh"),
          "BlueWin uses the upstream blue_win_zh frames");
    check(game_result_frame_dir(GameResult::Terminated) == QStringLiteral("abnormal_termination_zh"),
          "Terminated uses the upstream abnormal_termination_zh frames");
    check(game_result_frame_dir(GameResult::None).isEmpty(), "None has no frames");
    check(game_result_title(GameResult::None).isEmpty(), "None has no title");
}

// 目录名对了不等于素材真的取得到。异常终止那 7 帧是唯一进 qrc 的序列,
// 而红胜/蓝胜按设计只在磁盘上 —— 所以这条断言守的是 qrc 分支:注册漏了、
// alias 写错了、或者 entryInfoList 在 ":/" 下取不到文件,都只会在运行期
// 表现为「动画不播」,构建期一声不响。
void the_packaged_termination_frames_resolve_from_qrc() {
    const QStringList urls = game_result_frame_urls(GameResult::Terminated);
    check(urls.size() == 7, "all seven termination frames resolve");
    check(urls.front().startsWith(QStringLiteral("qrc:/")),
          "packaged frames are addressed through qrc, not the disk");

    // 序号排序而非字典序。这个目录是 22..28 单位数尾号,字典序恰好也对,
    // 所以这里只钉住首末帧,真正的排序压力在磁盘上的 (17)..(174)。
    check(urls.front().endsWith(QStringLiteral("termination_Zh (22).png")),
          "the sequence starts at frame 22");
    check(urls.back().endsWith(QStringLiteral("termination_Zh (28).png")),
          "the sequence ends at frame 28");
}

// 平局和 None 没有素材目录,不能因此去枚举整个 resultpanel 根目录。
void results_without_frames_resolve_to_an_empty_list() {
    check(game_result_frame_urls(GameResult::Draw).isEmpty(), "Draw resolves no frames");
    check(game_result_frame_urls(GameResult::None).isEmpty(), "None resolves no frames");
}

}  // namespace

int main(int argc, char** argv) {
    // game_result_frame_urls 会问 applicationDirPath(),没有 QCoreApplication
    // 实例时那是未定义行为。
    QCoreApplication app(argc, argv);
    try {
        winner_codes_map_to_the_upstream_result_enum();
        the_non_settlement_sentinel_is_not_a_result();
        a_result_needs_the_settlement_stage();
        a_stale_link_never_announces_a_winner();
        draw_has_a_title_but_no_frames();
        every_result_with_frames_names_a_real_upstream_directory();
        the_packaged_termination_frames_resolve_from_qrc();
        results_without_frames_resolve_to_an_empty_list();
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    std::cout << "all game_result assertions passed\n";
    return 0;
}
