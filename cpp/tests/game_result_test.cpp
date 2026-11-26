#include "game_result.h"

#include <QCoreApplication>
#include <iostream>
#include <stdexcept>

using namespace rm_terminal;

namespace
{

    void check(bool condition, const char* name)
    {
        if (!condition)
            throw std::runtime_error(name);
        std::cout << "PASS " << name << '\n';
    }

    GameResultInputs settled(std::uint32_t winner)
    {
        GameResultInputs in;
        in.match_freshness = Freshness::Fresh;
        in.stage = 5;
        in.winner = winner;
        return in;
    }

    void winner_codes_map_to_the_upstream_result_enum()
    {
        check(resolve_game_result(settled(1)) == GameResult::RedWin, "winner=1 is RedWin");
        check(resolve_game_result(settled(2)) == GameResult::BlueWin, "winner=2 is BlueWin");
        check(resolve_game_result(settled(0)) == GameResult::Draw, "winner=0 is Draw");
    }

    // 255 是协议规定的「非结算阶段」占位值,不是一种结果。当成结果会在每局开始前
    // 就播一段结算动画。
    void the_non_settlement_sentinel_is_not_a_result()
    {
        check(resolve_game_result(settled(255)) == GameResult::None,
              "winner=255 yields no result");
    }

    // 结算动画只在阶段真的进入结算后播。否则一个残留的 winner 会在比赛中途盖屏。
    void a_result_needs_the_settlement_stage()
    {
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
    void a_stale_link_never_announces_a_winner()
    {
        for (Freshness f : {Freshness::Stale, Freshness::NeverReceived})
        {
            GameResultInputs in = settled(1);
            in.match_freshness = f;
            check(resolve_game_result(in) == GameResult::None,
                  "an untrustworthy link yields no result");
        }
    }

    // 平局没有逐帧素材(上游同样如此),但仍然必须有可显示的标题,
    // 否则平局会变成一个空盒子。
    void draw_has_a_title_but_no_frames()
    {
        check(game_result_frame_dir(GameResult::Draw).isEmpty(), "Draw has no frame directory");
        check(!game_result_title(GameResult::Draw).isEmpty(), "Draw still states the outcome");
    }

    void every_result_with_frames_names_a_real_upstream_directory()
    {
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
    void the_packaged_termination_frames_resolve_from_qrc()
    {
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
    void results_without_frames_resolve_to_an_empty_list()
    {
        check(game_result_frame_urls(GameResult::Draw).isEmpty(), "Draw resolves no frames");
        check(game_result_frame_urls(GameResult::None).isEmpty(), "None resolves no frames");
    }

    template <typename T>
    Field<T> present(T value)
    {
        Field<T> field;
        field.value = value;
        field.quality = Quality::Valid;
        field.freshness = Freshness::Fresh;
        return field;
    }

    Field<std::uint32_t> reason_code(std::uint32_t code) { return present<std::uint32_t>(code); }

    // 同一个 end_reason 对红胜和蓝胜说的是相反的事。编码 1 是「有一方基地被击毁」,
    // 按规则手册 5.8 第 1 条获胜方是基地血量高的那方 —— 倒下的是**败方**。
    // 不看 winner 直接写死一方,有一半的局会把结论说反。
    void the_base_destroyed_reason_names_the_loser()
    {
        check(game_result_reason(GameResult::RedWin, reason_code(1)) == QStringLiteral("蓝方基地被击毁"),
              "red winning by base destruction names the blue base");
        check(game_result_reason(GameResult::BlueWin, reason_code(1)) == QStringLiteral("红方基地被击毁"),
              "blue winning by base destruction names the red base");
    }

    // 时间到的三条判定(控制度 / 总伤害 / 总剩余血量)说的是获胜方的优势项。
    void the_timeout_reasons_name_the_winner_and_the_deciding_metric()
    {
        check(game_result_reason(GameResult::BlueWin, reason_code(2)) == QStringLiteral("比赛时间到，蓝方累计控制度更高"),
              "code 2 is decided on control points");
        check(game_result_reason(GameResult::RedWin, reason_code(3)) == QStringLiteral("比赛时间到，控制度持平，红方总伤害更高"),
              "code 3 states that control points were level");
        check(game_result_reason(GameResult::RedWin, reason_code(4)) == QStringLiteral("比赛时间到，控制度与总伤害持平，红方总剩余血量更高"),
              "code 4 states that both earlier criteria were level");
        check(game_result_reason(GameResult::Draw, reason_code(5)) == QStringLiteral("比赛时间到，双方各项条件均持平"),
              "code 5 is the draw");
        check(game_result_reason(GameResult::RedWin, reason_code(6)) == QStringLiteral("比赛异常终止"),
              "code 6 is the abnormal end");
        check(game_result_reason(GameResult::RedWin, reason_code(7)) == QStringLiteral("主裁判定红方获胜"),
              "code 7 is the head referee's call");
        check(game_result_reason(GameResult::Draw, reason_code(7)) == QStringLiteral("主裁判定为平局"),
              "the head referee can also call a draw");
    }

    // 编码和 winner 自相矛盾时宁可不说:平局却报「基地被击毁」,或分出了胜负却报
    // 「平局」,两个字段里必有一个是错的,替服务器编一个理由会把错的那个当事实播出去。
    void contradictory_codes_say_nothing()
    {
        for (std::uint32_t code : {1u, 2u, 3u, 4u})
        {
            check(game_result_reason(GameResult::Draw, reason_code(code)).isEmpty(),
                  "a draw never carries a win reason");
        }
        check(game_result_reason(GameResult::RedWin, reason_code(5)).isEmpty(),
              "a decided round never carries the draw reason");
    }

    // 255 是协议规定的「非结算阶段」占位值,不是一种原因。连同未定义编码、缺失字段、
    // 不可信字段一起,一律整行不画 —— 结算屏上一句编造的理由比没有理由糟得多。
    void unusable_codes_produce_no_line()
    {
        check(game_result_reason(GameResult::RedWin, reason_code(255)).isEmpty(),
              "the non-settlement sentinel is not a reason");
        check(game_result_reason(GameResult::RedWin, reason_code(0)).isEmpty(),
              "code 0 is not defined by the protocol");
        check(game_result_reason(GameResult::RedWin, reason_code(99)).isEmpty(),
              "codes beyond the defined range are not guessed at");
        check(game_result_reason(GameResult::RedWin, Field<std::uint32_t>{}).isEmpty(),
              "a missing end_reason produces no line");
        check(game_result_reason(GameResult::None, reason_code(1)).isEmpty(),
              "no result means no reason");

        Field<std::uint32_t> invalid;
        invalid.value = 1;
        invalid.quality = Quality::Invalid;
        check(game_result_reason(GameResult::RedWin, invalid).isEmpty(),
              "an invalid end_reason is treated as missing, not as code 1");
    }

    Snapshot settled_snapshot(std::uint32_t winner)
    {
        Snapshot snapshot;
        snapshot.self = RobotId{3};
        snapshot.game.current_stage = present<std::uint32_t>(5);
        snapshot.game.winner = present<std::uint32_t>(winner);
        return snapshot;
    }

    RobotHealth health(std::uint32_t id, std::uint32_t faction, std::optional<std::uint32_t> hp,
                       std::optional<std::uint32_t> max_hp)
    {
        RobotHealth robot;
        robot.id = RobotId{id};
        robot.faction = faction;
        if (hp)
            robot.current_hp = present<std::uint32_t>(*hp);
        if (max_hp)
            robot.max_hp = present<std::uint32_t>(*max_hp);
        return robot;
    }

    const SettlementRow& row_named(const SettlementModel& model, const QString& label)
    {
        for (const SettlementRow& row : model.rows)
            if (row.label == label)
                return row;
        throw std::runtime_error(("no settlement row named " + label).toStdString());
    }

    // 结算面板的核心风险是把「服务器没给」画成「真的是 0」。基地血量 0 是一条已经
    // 被击毁的基地(真实测量),缺失是情报空白,两者必须在模型层就分开 ——
    // 到了 QML 再想区分已经没有信息可用了。
    void a_real_zero_stays_distinct_from_missing_data()
    {
        Snapshot snapshot = settled_snapshot(1);
        snapshot.game.red_base_hp = present<std::uint32_t>(0);
        // 蓝方基地血量刻意不设:协议这一帧没给。
        snapshot.game.red_total_damage = present<std::uint32_t>(0);

        const SettlementModel model = settlement_model(snapshot);
        const SettlementRow& base = row_named(model, QStringLiteral("基地血量"));
        check(base.red.has_value() && *base.red == 0,
              "a measured zero base HP is carried as a real 0");
        check(!base.blue.has_value(), "an absent base HP stays absent, not 0");

        const SettlementRow& damage = row_named(model, QStringLiteral("总伤害"));
        check(damage.red.has_value() && *damage.red == 0, "zero damage dealt is a real 0");
        check(!damage.blue.has_value(), "an absent damage total stays absent");

        const QVariantMap variant = settlement_variant(model);
        const QVariantList rows = variant[QStringLiteral("rows")].toList();
        const QVariantMap first = rows.front().toMap();
        check(first[QStringLiteral("redKnown")].toBool(),
              "the variant marks a measured zero as known");
        check(!first[QStringLiteral("blueKnown")].toBool(),
              "the variant marks missing data as unknown so QML can draw --");
    }

    // 击杀数在本机累计:终端中途启动时还没开始数,那时的 0 是「不知道」而不是
    // 「一个都没杀」。没收到过比赛状态就必须报缺失。
    void kills_are_unknown_until_the_match_state_arrives()
    {
        Snapshot never_received;
        never_received.self = RobotId{3};
        const SettlementRow& cold = row_named(settlement_model(never_received),
                                              QStringLiteral("击杀数"));
        check(!cold.red.has_value() && !cold.blue.has_value(),
              "kills are unknown before any match state is received");

        Snapshot counting = settled_snapshot(1);
        counting.kills = KillTally{0, 3};
        const SettlementRow& warm = row_named(settlement_model(counting), QStringLiteral("击杀数"));
        check(warm.red.has_value() && *warm.red == 0,
              "once counting, zero kills is a real 0");
        check(warm.blue.has_value() && *warm.blue == 3, "the blue tally is carried through");
    }

    // 机器人剩余血量是「花名册上那几台」的和。一台都没有数据时必须是缺失:
    // 报 0 会被读成全队覆灭。有数据的那几台则只加它们自己。
    void the_robot_hp_sum_skips_missing_robots_and_stays_absent_when_empty()
    {
        Snapshot empty = settled_snapshot(1);
        const SettlementRow& none = row_named(settlement_model(empty),
                                              QStringLiteral("机器人剩余血量"));
        check(!none.red.has_value() && !none.blue.has_value(),
              "with no robot health at all the sum is absent, not 0");

        Snapshot snapshot = settled_snapshot(1);
        snapshot.robot_health.push_back(health(1, kRed, 0, 400));
        snapshot.robot_health.push_back(health(3, kRed, 412, 600));
        // 这台没有血量:不能把它当 0 加进和里,也不能让它把整个和变成缺失。
        snapshot.robot_health.push_back(health(4, kRed, std::nullopt, 600));
        snapshot.robot_health.push_back(health(101, kBlue, 205, 400));

        const SettlementModel model = settlement_model(snapshot);
        const SettlementRow& sum = row_named(model, QStringLiteral("机器人剩余血量"));
        check(sum.red.has_value() && *sum.red == 412,
              "the sum counts the measured robots only (0 + 412)");
        check(sum.blue.has_value() && *sum.blue == 205, "the blue side sums its own robots");

        check(model.red_robots.size() == 3, "every red robot in the snapshot gets a column row");
        check(model.blue_robots.size() == 1, "the blue column holds only the blue robots");
        check(!model.red_robots[2].hp.has_value(),
              "the robot with no HP keeps its missing HP for the -- in its row");
        check(model.red_robots[0].cls == QStringLiteral("重装"),
              "the column rows carry the robot class for the roster picture");
    }

    // 能量机关协议只发我方(RuneStatusSync 不含对方)。所以这一行只有我方那一侧
    // 写得出状态,另一侧必须留空显示「--」,不能借对方的数填。
    void the_rune_row_only_speaks_for_our_own_side()
    {
        Snapshot red_seat = settled_snapshot(1);
        red_seat.game.rune_status = present<std::uint32_t>(3);
        const SettlementRow& red_row = row_named(settlement_model(red_seat),
                                                 QStringLiteral("能量机关"));
        check(red_row.red_text == QStringLiteral("已激活"), "a red seat fills the red side");
        check(red_row.blue_text.isEmpty(), "the enemy rune state is never invented");

        Snapshot blue_seat = settled_snapshot(2);
        blue_seat.self = RobotId{103};
        blue_seat.game.rune_status = present<std::uint32_t>(2);
        const SettlementRow& blue_row = row_named(settlement_model(blue_seat),
                                                  QStringLiteral("能量机关"));
        check(blue_row.blue_text == QStringLiteral("激活中"), "a blue seat fills the blue side");
        check(blue_row.red_text.isEmpty(), "the enemy side stays empty for a blue seat");
    }

    // 比赛时长:协议在结算阶段只给结算阶段自己的 stage_elapsed_sec,拿它当比赛用时
    // 会把「结算开始 3 秒」显示成「比赛打了 3 秒」。所以必须一直是缺失,界面显示「--」。
    void the_match_duration_is_never_guessed_from_the_settlement_stage()
    {
        Snapshot snapshot = settled_snapshot(1);
        snapshot.game.stage_elapsed_sec = present<std::int32_t>(3);
        const SettlementModel model = settlement_model(snapshot);
        check(!model.duration_sec.has_value(),
              "the settlement stage's own elapsed time is not reported as the match duration");
        check(!settlement_variant(model)[QStringLiteral("durationSecKnown")].toBool(),
              "QML is told the duration is unknown so it prints --");
    }

    // 比分、轮次与结果三者都要能独立缺失。界面上每一个「--」都必须是真的没有数据。
    void the_scoreboard_fields_report_their_own_absence()
    {
        const SettlementModel bare = settlement_model(settled_snapshot(1));
        check(!bare.round.has_value() && !bare.total_rounds.has_value(),
              "an unknown round is absent rather than 0");
        check(!bare.red_score.has_value() && !bare.blue_score.has_value(),
              "an unknown score is absent rather than 0");

        Snapshot full = settled_snapshot(2);
        full.game.current_round = present<std::uint32_t>(2);
        full.game.total_rounds = present<std::uint32_t>(3);
        full.game.red_score = present<std::uint32_t>(0);
        full.game.blue_score = present<std::uint32_t>(1);
        const SettlementModel model = settlement_model(full);
        check(model.red_score.has_value() && *model.red_score == 0,
              "a real 0 score is carried as a value");
        check(model.title == QStringLiteral("蓝方胜利"), "the model carries the result title");

        const QVariantMap variant = settlement_variant(model);
        check(variant[QStringLiteral("blueWin")].toBool(), "the variant names the winning side");
        check(!variant[QStringLiteral("redWin")].toBool() && !variant[QStringLiteral("draw")].toBool(),
              "only one outcome flag is set");
        check(variant[QStringLiteral("redScoreKnown")].toBool() && variant[QStringLiteral("redScore")].toInt() == 0,
              "the variant distinguishes a 0 score from a missing one");
    }

    // QML 按字符串配色,枚举的整数取值不该漏到 QML 里。
    void the_outcome_key_names_the_colour_scheme()
    {
        check(game_result_outcome(GameResult::RedWin) == QStringLiteral("red"), "RedWin is red");
        check(game_result_outcome(GameResult::BlueWin) == QStringLiteral("blue"), "BlueWin is blue");
        check(game_result_outcome(GameResult::Draw) == QStringLiteral("draw"), "Draw is draw");
        check(game_result_outcome(GameResult::None).isEmpty(), "None has no colour scheme");
    }

    // 没有结果时模型必须整体是空的:一个残留的标题会让覆盖层在比赛中途显示出来。
    void no_result_produces_an_empty_model()
    {
        Snapshot mid_match;
        mid_match.self = RobotId{3};
        mid_match.game.current_stage = present<std::uint32_t>(4);
        mid_match.game.winner = present<std::uint32_t>(1);
        const SettlementModel model = settlement_model(mid_match);
        check(model.result == GameResult::None, "stage 4 yields no result");
        check(model.title.isEmpty() && model.reason.isEmpty(),
              "no result means no title and no reason");
    }

} // namespace

int main(int argc, char** argv)
{
    // game_result_frame_urls 会问 applicationDirPath(),没有 QCoreApplication
    // 实例时那是未定义行为。
    QCoreApplication app(argc, argv);
    try
    {
        winner_codes_map_to_the_upstream_result_enum();
        the_non_settlement_sentinel_is_not_a_result();
        a_result_needs_the_settlement_stage();
        a_stale_link_never_announces_a_winner();
        draw_has_a_title_but_no_frames();
        every_result_with_frames_names_a_real_upstream_directory();
        the_packaged_termination_frames_resolve_from_qrc();
        results_without_frames_resolve_to_an_empty_list();
        the_base_destroyed_reason_names_the_loser();
        the_timeout_reasons_name_the_winner_and_the_deciding_metric();
        contradictory_codes_say_nothing();
        unusable_codes_produce_no_line();
        a_real_zero_stays_distinct_from_missing_data();
        kills_are_unknown_until_the_match_state_arrives();
        the_robot_hp_sum_skips_missing_robots_and_stays_absent_when_empty();
        the_rune_row_only_speaks_for_our_own_side();
        the_match_duration_is_never_guessed_from_the_settlement_stage();
        the_scoreboard_fields_report_their_own_absence();
        the_outcome_key_names_the_colour_scheme();
        no_result_produces_an_empty_model();
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    std::cout << "all game_result assertions passed\n";
    return 0;
}
