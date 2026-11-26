#include "match_report.h"
#include "overview.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>

#include <cstdio>

namespace
{

    using rm_terminal::Leader;
    using rm_terminal::MatchLog;
    using rm_terminal::MatchSample;
    using rm_terminal::RobotId;
    using rm_terminal::Snapshot;

    int failures = 0;

    void check(bool condition, const char* label)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL %s\n", label);
        ++failures;
    }

    template <class T>
    void set(rm_terminal::Field<T>& field, T value)
    {
        field.value = value;
        field.quality = rm_terminal::Quality::Valid;
        field.freshness = rm_terminal::Freshness::Fresh;
    }

    // 红方 3 号的终端。阵地编码:1 红临时 2 红完全 3 蓝临时 4 蓝完全。
    Snapshot at(std::uint32_t stage, std::int32_t elapsed, std::uint32_t red_control, std::uint32_t blue_control,
                std::array<std::uint32_t, 4> zones = {2, 4, 2, 4}, std::uint32_t red_base = 10000)
    {
        Snapshot s;
        s.self = RobotId{3};
        set<std::uint32_t>(s.game.current_round, 1);
        set(s.game.current_stage, stage);
        set(s.game.stage_elapsed_sec, elapsed);
        set<std::int32_t>(s.game.stage_countdown_sec, 420 - elapsed);
        set(s.game.red_control_points, red_control);
        set(s.game.blue_control_points, blue_control);
        set(s.game.red_base_hp, red_base);
        set<std::uint32_t>(s.game.blue_base_hp, 10000);
        set<std::uint32_t>(s.game.red_total_damage, 300);
        set<std::uint32_t>(s.game.blue_total_damage, 500);
        set(s.game.red_outpost_zone, zones[0]);
        set(s.game.blue_outpost_zone, zones[1]);
        set(s.game.red_base_zone, zones[2]);
        set(s.game.blue_base_zone, zones[3]);
        return s;
    }

    void rates_follow_the_zones()
    {
        using rm_terminal::control_rates;
        check(control_rates({2u, 4u, 2u, 4u}) == std::make_pair(0, 0), "everyone on their own zones: nobody scores");
        check(control_rates({2u, 1u, 2u, 4u}) == std::make_pair(1, 0), "red holds both outposts: +1/s");
        check(control_rates({2u, 1u, 2u, 1u}) == std::make_pair(4, 0), "red holds all four: +4/s");
        check(!control_rates({2u, std::nullopt, 2u, 4u}), "missing zone: no rate");
    }

    void judge_follows_rule_5_8()
    {
        MatchSample s;
        s.red_control = 120;
        s.blue_control = 85;
        s.red_base = 9000;
        s.blue_base = 8000;
        check(rm_terminal::judge(s, rm_terminal::kRed).text == QStringLiteral("我方赢 · 控制度 120:85"), "control decides");
        check(rm_terminal::judge(s, rm_terminal::kBlue).leader == Leader::Theirs, "same sample from blue's side");
        s.blue_control = 120;
        s.red_damage = 400;
        s.blue_damage = 600;
        check(rm_terminal::judge(s, rm_terminal::kRed).text == QStringLiteral("敌方赢 · 总伤害 400:600"), "tie: damage decides");
        s.blue_damage = 400;
        s.red_hp = 500;
        s.blue_hp = 700;
        check(rm_terminal::judge(s, rm_terminal::kRed).leader == Leader::Theirs, "tie again: remaining hp");
        s.blue_hp = 500;
        check(rm_terminal::judge(s, rm_terminal::kRed).leader == Leader::Even, "all equal: even");
        s.blue_base = 0;
        check(rm_terminal::judge(s, rm_terminal::kRed).text == QStringLiteral("我方赢 · 敌方基地被打掉"), "base first");
    }

    void log_reports_once_per_round()
    {
        MatchLog log;
        log.observe(at(1, 0, 0, 0));
        for (int t = 0; t < 5; ++t)
            log.observe(at(4, t, 0, 0));
        log.observe(at(4, 4, 0, 0));
        check(log.samples().size() == 5, "one sample per match second");
        log.note(QStringLiteral("敌方 3 号步兵从隧道进入我方半场"));
        Snapshot end = at(5, 5, 10, 0);
        set<std::uint32_t>(end.game.winner, 1);
        set<std::uint32_t>(end.game.end_reason, 2);
        check(log.observe(end), "entering settlement asks for the report");
        check(!log.observe(end), "only once");
        check(log.winner() == 1u && log.endReason() == 2u, "the referee result is kept");
        check(log.notes().size() == 1, "alerts are in the log");
        log.observe(at(1, 0, 0, 0));
        check(log.samples().empty(), "back to preparation: a new log");
    }

    void projection_and_flip()
    {
        // 第 100 秒,蓝方占着两个前哨阵地(每秒 +1),控制度 50:80。
        MatchLog log;
        log.observe(at(1, 0, 0, 0));
        for (int t = 90; t <= 100; ++t)
            log.observe(at(4, t, 50, 70 + (t - 90), {3, 4, 2, 4}));
        const Snapshot now = at(4, 100, 50, 80, {3, 4, 2, 4});
        log.observe(now);
        const rm_terminal::Overview o = rm_terminal::build_overview(now, rm_terminal::BuffBoard{}, log, 0);
        check(o.now.text == QStringLiteral("敌方赢 · 控制度 50:80"), "now: behind on control");
        check(o.at_end.text == QStringLiteral("敌方赢 · 控制度 50:400"), "at 7:00: 80 + 320 x 1");
        check(o.flip.contains(QStringLiteral("临时控制敌方基地阵地")) && o.flip.contains(QStringLiteral("1:57")),
              "the base zone flips it with the most room: 350 / 3 + 1 = 117 s");
        check(o.flip_urgent, "losing is urgent");
        check(o.projection == -1.0 && o.trend.size() == 11 && o.trend.back().second == -30, "trend and slope");
        check(o.rates == QStringLiteral("控制度每秒 我 +0 · 敌 +1"), "header rates");
        check(o.next_mark.startsWith(QStringLiteral("下一个：")), "next timeline mark");
    }

    void base_falls_first()
    {
        // 我方基地 60 秒掉了 1200,还剩 2000:100 秒后打掉,比 7:00 早。
        MatchLog log;
        log.observe(at(1, 0, 0, 0));
        for (int t = 200; t <= 260; t += 10)
            log.observe(at(4, t, 100, 0, {2, 1, 2, 4}, static_cast<std::uint32_t>(3200 - (t - 200) * 20)));
        const Snapshot now = at(4, 260, 100, 0, {2, 1, 2, 4}, 2000);
        const rm_terminal::Overview o = rm_terminal::build_overview(now, rm_terminal::BuffBoard{}, log, 0);
        check(o.at_end.leader == Leader::Theirs && o.at_end.text.contains(QStringLiteral("我方基地被打掉")),
              "projected base loss beats the control lead");
        check(o.flip.startsWith(QStringLiteral("守基地：照现在 100 秒后被打掉")), "defend the base");
    }

    void report_page()
    {
        MatchLog log;
        log.observe(at(1, 0, 0, 0));
        log.observe(at(4, 0, 0, 0));
        log.observe(at(4, 1, 0, 0, {2, 1, 2, 4}));
        log.note(QStringLiteral("敌方 3 号步兵从隧道进入我方半场"));
        Snapshot end = at(5, 2, 2, 0, {2, 1, 2, 4});
        set<std::uint32_t>(end.game.winner, 1);
        set<std::uint32_t>(end.game.end_reason, 2);
        log.observe(end);
        rm_terminal::ReportContext context;
        context.self_id = 3;
        context.recording = QStringLiteral("/tmp/x.rmrec");
        const QString html = rm_terminal::match_report_html(log, context);
        check(html.contains(QStringLiteral("第 1 局复盘")) && html.contains(QStringLiteral("裁判系统：红方胜（控制度）")),
              "report header and referee result");
        check(html.contains(QStringLiteral("蓝方前哨区")) && html.contains(QStringLiteral("蓝方完全 → 红方临时")),
              "zone change listed");
        check(html.contains(QStringLiteral("从隧道进入我方半场")) && html.contains(QStringLiteral("<svg")), "notes and charts");
        check(html.contains(QStringLiteral("/tmp/x.rmrec")), "recording path");
    }

    void clips_json_and_index()
    {
        MatchLog log;
        log.observe(at(1, 0, 0, 0));
        log.setRecordingMs(3000);
        log.observe(at(4, 0, 0, 0));
        log.setRecordingMs(25500);
        log.observe(at(4, 1, 0, 0, {2, 1, 2, 4}));
        log.note(QStringLiteral("敌方 3 号步兵从隧道进入我方半场"));
        Snapshot end = at(5, 2, 2, 0, {2, 1, 2, 4});
        log.observe(end);
        rm_terminal::ReportContext context;
        context.self_id = 3;
        context.recording = QStringLiteral("/tmp/x.rmrec");
        context.finished_at = QDateTime(QDate(2026, 11, 16), QTime(14, 35, 42));
        const auto clips = rm_terminal::report_clips(log, context);
        check(clips.size() == 1 && clips[0].from_s == 15 && clips[0].to_s == 31 && clips[0].mp4 == QStringLiteral("clips/片段-01.mp4"),
              "a clip from 10 s before to 5 s after, file name numbered");
        context.recording.clear();
        check(rm_terminal::report_clips(log, context).empty(), "no recording: no clips");
        context.recording = QStringLiteral("/tmp/x.rmrec");

        const QString dir = QDir::temp().filePath(QStringLiteral("rm-overview-test-%1").arg(QCoreApplication::applicationPid()));
        QDir(dir).removeRecursively();
        QString error;
        const QString page = rm_terminal::write_match_report(dir, log, context, &error);
        check(page.endsWith(QStringLiteral("2026-11-16_14-35-42_第1局.html")), "report file name");
        QFile json(QDir(dir).filePath(QStringLiteral("2026-11-16_14-35-42_第1局.json")));
        check(json.open(QIODevice::ReadOnly) && QString::fromUtf8(json.readAll()).contains(QStringLiteral("clips/2026-11-16_14-35-42_第1局-01.mp4")),
              "the summary lists the clip under the report's own name");
        QFile index(QDir(dir).filePath(QStringLiteral("index.html")));
        check(index.open(QIODevice::ReadOnly) && QString::fromUtf8(index.readAll()).contains(QStringLiteral("2026-11-16_14-35-42_第1局.html")),
              "the comparison page links the report");
        QDir(dir).removeRecursively();
    }

} // namespace

int main()
{
    rates_follow_the_zones();
    judge_follows_rule_5_8();
    log_reports_once_per_round();
    projection_and_flip();
    base_falls_first();
    report_page();
    clips_json_and_index();
    if (failures == 0)
        std::puts("overview_test: all passed");
    return failures == 0 ? 0 : 1;
}
