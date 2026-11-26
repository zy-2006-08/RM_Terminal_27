#include "match_report.h"

#include "seat.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QStringList>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <functional>

namespace rm_terminal
{
    namespace
    {

        constexpr int kChartWidth = 960;
        constexpr int kChartHeight = 220;
        constexpr int kPad = 40;

        const char* kRedColor = "#ff6b6b";
        const char* kBlueColor = "#5aa9ff";
        const char* kGoldColor = "#ecaa46";

        QString clock(std::int64_t seconds)
        {
            seconds = std::max<std::int64_t>(0, seconds);
            return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
        }

        std::int32_t match_length(const std::vector<MatchSample>& samples)
        {
            for (auto it = samples.rbegin(); it != samples.rend(); ++it)
                if (it->countdown)
                    return it->elapsed + *it->countdown;
            return 420;
        }

        QString countdown_of(const MatchSample& sample, std::int32_t length)
        {
            return clock(sample.countdown ? *sample.countdown : length - sample.elapsed);
        }

        struct Series
        {
            QString label;
            const char* color;
            std::function<std::optional<std::uint32_t>(const MatchSample&)> value;
        };

        // 一张折线图:横轴是比赛倒计时(从 7:00 到 0:00),纵轴从 0 到这几条线的最大值。
        QString chart(const std::vector<MatchSample>& samples, const std::vector<Series>& series, std::int32_t length)
        {
            double top = 1;
            for (const MatchSample& sample : samples)
                for (const Series& s : series)
                    if (const auto v = s.value(sample))
                        top = std::max(top, static_cast<double>(*v));
            const double w = kChartWidth - 2 * kPad;
            const double h = kChartHeight - 2 * kPad;
            const auto x_of = [&](std::int32_t elapsed)
            { return kPad + w * elapsed / std::max(1, length); };
            const auto y_of = [&](double value)
            { return kPad + h * (1 - value / top); };
            QString svg = QStringLiteral("<svg viewBox='0 0 %1 %2' width='100%' role='img'>").arg(kChartWidth).arg(kChartHeight);
            for (std::int32_t t = 0; t <= length; t += 60)
            {
                svg += QStringLiteral("<line x1='%1' y1='%2' x2='%1' y2='%3' stroke='#23344d'/>")
                           .arg(x_of(t))
                           .arg(kPad)
                           .arg(kPad + h);
                svg += QStringLiteral("<text x='%1' y='%2' fill='#7f93a8' font-size='12' text-anchor='middle'>%3</text>")
                           .arg(x_of(t))
                           .arg(kChartHeight - 12)
                           .arg(clock(length - t));
            }
            svg += QStringLiteral("<text x='4' y='%1' fill='#7f93a8' font-size='12'>%2</text>").arg(kPad + 4).arg(top, 0, 'f', 0);
            svg += QStringLiteral("<text x='4' y='%1' fill='#7f93a8' font-size='12'>0</text>").arg(kPad + h);
            int legend = kPad;
            for (const Series& s : series)
            {
                QStringList points;
                for (const MatchSample& sample : samples)
                    if (const auto v = s.value(sample))
                        points << QStringLiteral("%1,%2").arg(x_of(sample.elapsed), 0, 'f', 1).arg(y_of(*v), 0, 'f', 1);
                svg += QStringLiteral("<polyline fill='none' stroke='%1' stroke-width='2' points='%2'/>")
                           .arg(QString::fromLatin1(s.color), points.join(QLatin1Char(' ')));
                svg += QStringLiteral("<text x='%1' y='20' fill='%2' font-size='13'>— %3</text>")
                           .arg(legend)
                           .arg(QString::fromLatin1(s.color), s.label.toHtmlEscaped());
                legend += 24 + 14 * static_cast<int>(s.label.size());
            }
            return svg + QStringLiteral("</svg>");
        }

        QString zone_name(std::size_t index)
        {
            static const char* names[] = {"红方前哨区", "蓝方前哨区", "红方基地区", "蓝方基地区"};
            return QString::fromUtf8(names[index]);
        }

        QString zone_state(std::uint32_t code)
        {
            static const char* states[] = {"未控", "红方临时", "红方完全", "蓝方临时", "蓝方完全"};
            return code < 5 ? QString::fromUtf8(states[code]) : QStringLiteral("?");
        }

        QString side_name(std::uint32_t faction) { return faction == kBlue ? QStringLiteral("蓝方") : QStringLiteral("红方"); }

        QString end_reason_text(std::uint32_t reason)
        {
            switch (reason)
            {
            case 1: return QStringLiteral("基地被打掉");
            case 2: return QStringLiteral("控制度");
            case 3: return QStringLiteral("总伤害");
            case 4: return QStringLiteral("总剩余血量");
            case 5: return QStringLiteral("平局");
            case 6: return QStringLiteral("异常终止");
            case 7: return QStringLiteral("主裁判定");
            default: return QString();
            }
        }

        QString pair_cell(std::optional<std::uint32_t> red, std::optional<std::uint32_t> blue)
        {
            const auto text = [](std::optional<std::uint32_t> v)
            { return v ? QString::number(*v) : QStringLiteral("--"); };
            return QStringLiteral("<span class='red'>%1</span> : <span class='blue'>%2</span>").arg(text(red), text(blue));
        }

    } // namespace

    QString match_report_html(const MatchLog& log, const ReportContext& context)
    {
        const std::vector<MatchSample>& samples = log.samples();
        const std::uint32_t ours = faction_of(context.self_id);
        const std::int32_t length = match_length(samples);
        const QString round = log.round() ? QString::number(*log.round()) : QStringLiteral("?");
        const auto seat_of = Seat::fromRobotId(static_cast<int>(context.self_id));
        const QString seat = seat_of ? seat_of->label() : side_name(ours);
        QString html = QStringLiteral(
                           "<!doctype html><html lang='zh'><head><meta charset='utf-8'><title>第 %1 局复盘 · %2</title><style>"
                           "body{background:#0b1220;color:#dce7f2;font-family:'Noto Sans CJK SC','PingFang SC',sans-serif;margin:24px auto;max-width:1040px}"
                           "h1{color:#8ed2f2;margin-bottom:4px}h2{font-size:18px;margin:0 0 10px}"
                           ".card{background:#121c2c;border:1px solid #23344d;border-radius:8px;padding:16px 20px;margin:14px 0}"
                           ".muted{color:#7f93a8}.red{color:#ff6b6b}.blue{color:#5aa9ff}.big{font-size:22px;font-weight:700}"
                           "table{border-collapse:collapse;width:100%}td,th{padding:5px 10px;border-bottom:1px solid #23344d;text-align:left}"
                           "th{color:#7f93a8;font-weight:500}code{color:#8ed2f2}a{color:#8ed2f2}</style></head><body>")
                           .arg(round, seat.toHtmlEscaped());
        html += QStringLiteral("<h1>第 %1 局复盘</h1><div class='muted'>%2的终端 · %3 结束 · 数据是这台终端自己记的%4</div>")
                    .arg(round, seat.toHtmlEscaped(), context.finished_at.toString(QStringLiteral("yyyy-MM-dd HH:mm")),
                         samples.empty() || samples.front().elapsed <= 2
                             ? QString()
                             : QStringLiteral("（终端在倒计时 %1 才打开，之前的没有）").arg(countdown_of(samples.front(), length)));

        html += QStringLiteral("<div class='card'><h2>结果</h2>");
        if (log.winner())
        {
            const std::uint32_t w = *log.winner();
            const QString who = w == 1 ? QStringLiteral("红方胜") : w == 2 ? QStringLiteral("蓝方胜")
                                                                           : QStringLiteral("平局");
            const QString why = log.endReason() ? end_reason_text(*log.endReason()) : QString();
            html += QStringLiteral("<p class='big'>裁判系统：%1%2</p>").arg(who, why.isEmpty() ? QString() : QStringLiteral("（%1）").arg(why));
        }
        if (!samples.empty())
        {
            const MatchSample& last = samples.back();
            html += QStringLiteral("<p>按规则 5.8 的顺序算：%1</p>").arg(judge(last, ours).text.toHtmlEscaped());
            html += QStringLiteral("<table><tr><th></th><th>红 : 蓝</th></tr>");
            html += QStringLiteral("<tr><td>控制度</td><td>%1</td></tr>").arg(pair_cell(last.red_control, last.blue_control));
            html += QStringLiteral("<tr><td>总伤害</td><td>%1</td></tr>").arg(pair_cell(last.red_damage, last.blue_damage));
            html += QStringLiteral("<tr><td>基地血量</td><td>%1</td></tr>").arg(pair_cell(last.red_base, last.blue_base));
            html += QStringLiteral("<tr><td>前哨站血量</td><td>%1</td></tr>").arg(pair_cell(last.red_outpost, last.blue_outpost));
            html += QStringLiteral("<tr><td>4 台车剩余血量</td><td>%1</td></tr></table>").arg(pair_cell(last.red_hp, last.blue_hp));
        }
        html += QStringLiteral("</div>");

        html += QStringLiteral("<div class='card'><h2>控制度</h2>%1</div>")
                    .arg(chart(samples,
                               {{QStringLiteral("红方"), kRedColor, [](const MatchSample& s)
                                 { return s.red_control; }},
                                {QStringLiteral("蓝方"), kBlueColor, [](const MatchSample& s)
                                 { return s.blue_control; }}},
                               length));
        html += QStringLiteral("<div class='card'><h2>基地血量</h2>%1</div>")
                    .arg(chart(samples,
                               {{QStringLiteral("红方基地"), kRedColor, [](const MatchSample& s)
                                 { return s.red_base; }},
                                {QStringLiteral("蓝方基地"), kBlueColor, [](const MatchSample& s)
                                 { return s.blue_base; }}},
                               length));
        std::vector<Series> money{{QStringLiteral("我方剩余金币"), ours == kBlue ? kBlueColor : kRedColor,
                                   [](const MatchSample& s)
                                   { return s.economy; }}};
        if (std::any_of(samples.begin(), samples.end(), [](const MatchSample& s)
                        { return s.enemy_gold.has_value(); }))
            money.push_back({QStringLiteral("敌方剩余金币（雷达情报）"), kGoldColor, [](const MatchSample& s)
                             { return s.enemy_gold; }});
        html += QStringLiteral("<div class='card'><h2>经济</h2>%1</div>").arg(chart(samples, money, length));

        html += QStringLiteral("<div class='card'><h2>阵地换手</h2><table><tr><th>倒计时</th><th>阵地</th><th>变化</th></tr>");
        int changes = 0;
        for (std::size_t i = 1; i < samples.size(); ++i)
            for (std::size_t zone = 0; zone < 4; ++zone)
            {
                const auto before = samples[i - 1].zones[zone];
                const auto after = samples[i].zones[zone];
                if (!before || !after || *before == *after)
                    continue;
                ++changes;
                html += QStringLiteral("<tr><td>%1</td><td>%2</td><td>%3 → %4</td></tr>")
                            .arg(countdown_of(samples[i], length), zone_name(zone), zone_state(*before), zone_state(*after));
            }
        if (changes == 0)
            html += QStringLiteral("<tr><td colspan='3' class='muted'>这一局四个阵地没换过手</td></tr>");
        html += QStringLiteral("</table></div>");

        const std::vector<ReportClip> clips = report_clips(log, context);
        html += QStringLiteral("<div class='card'><h2>关键事件</h2>");
        if (!clips.empty())
            html += QStringLiteral(
                        "<p class='muted'>每件事切了录像里前 %1 秒到后 %2 秒。在终端里放：<code>rm_terminal --replay 录像 --from 秒 --to 秒</code>；"
                        "要 MP4 先跑 <code>bash scripts/export_clips.sh %3.json</code>，导完点「MP4」。</p>")
                        .arg(kClipBeforeSec, 0, 'f', 0)
                        .arg(kClipAfterSec, 0, 'f', 0)
                        .arg(context.stem.toHtmlEscaped());
        html += QStringLiteral("<table><tr><th>倒计时</th><th>事件</th><th>片段</th></tr>");
        std::size_t clip = 0;
        for (const MatchNote& note : log.notes())
        {
            QString cell = QStringLiteral("<span class='muted'>没录像</span>");
            if (clip < clips.size() && note.recording_ms && !context.recording.isEmpty())
            {
                const ReportClip& c = clips[clip++];
                cell = QStringLiteral("<a href='%1'>MP4</a> · <code>--from %2 --to %3</code>")
                           .arg(c.mp4.toHtmlEscaped())
                           .arg(c.from_s, 0, 'f', 0)
                           .arg(c.to_s, 0, 'f', 0);
            }
            html += QStringLiteral("<tr><td>%1</td><td>%2</td><td>%3</td></tr>")
                        .arg(clock(note.countdown ? *note.countdown : length - note.elapsed), note.text.toHtmlEscaped(), cell);
        }
        if (log.notes().empty())
            html += QStringLiteral("<tr><td colspan='3' class='muted'>没有记到事件</td></tr>");
        html += QStringLiteral("</table></div>");

        html += QStringLiteral("<div class='card'><h2>录像</h2><p>%1</p></div>")
                    .arg(context.recording.isEmpty() ? QStringLiteral("这一局没录像（--no-record 或者录像没开起来）")
                                                     : context.recording.toHtmlEscaped());
        return html + QStringLiteral("</body></html>\n");
    }

    std::vector<ReportClip> report_clips(const MatchLog& log, const ReportContext& context)
    {
        std::vector<ReportClip> clips;
        if (context.recording.isEmpty())
            return clips;
        const std::int32_t length = match_length(log.samples());
        const QString stem = context.stem.isEmpty() ? QStringLiteral("片段") : context.stem;
        for (const MatchNote& note : log.notes())
        {
            if (!note.recording_ms)
                continue;
            ReportClip clip;
            clip.index = static_cast<int>(clips.size()) + 1;
            clip.text = note.text;
            clip.countdown = clock(note.countdown ? *note.countdown : length - note.elapsed);
            const double at = static_cast<double>(*note.recording_ms) / 1000.0;
            clip.from_s = std::max(0.0, std::floor(at - kClipBeforeSec));
            clip.to_s = std::ceil(at + kClipAfterSec);
            clip.mp4 = QStringLiteral("clips/%1-%2.mp4").arg(stem).arg(clip.index, 2, 10, QLatin1Char('0'));
            clips.push_back(clip);
        }
        return clips;
    }

    QString match_report_json(const MatchLog& log, const ReportContext& context)
    {
        const std::vector<MatchSample>& samples = log.samples();
        const std::uint32_t ours = faction_of(context.self_id);
        QJsonObject root;
        const auto seat_of = Seat::fromRobotId(static_cast<int>(context.self_id));
        root[QStringLiteral("seat")] = seat_of ? seat_of->label() : side_name(ours);
        root[QStringLiteral("self_id")] = static_cast<int>(context.self_id);
        root[QStringLiteral("round")] = log.round() ? static_cast<int>(*log.round()) : 0;
        root[QStringLiteral("finished")] = context.finished_at.toString(QStringLiteral("yyyy-MM-dd HH:mm"));
        root[QStringLiteral("report")] = context.stem + QStringLiteral(".html");
        root[QStringLiteral("recording")] = context.recording;
        if (log.winner())
            root[QStringLiteral("winner")] = static_cast<int>(*log.winner());
        if (log.endReason())
            root[QStringLiteral("end_reason")] = static_cast<int>(*log.endReason());
        if (!samples.empty())
        {
            const MatchSample& last = samples.back();
            root[QStringLiteral("verdict")] = judge(last, ours).text;
            const auto put = [&root](const char* key, std::optional<std::uint32_t> value)
            {
                if (value)
                    root[QString::fromLatin1(key)] = static_cast<double>(*value);
            };
            put("red_control", last.red_control);
            put("blue_control", last.blue_control);
            put("red_damage", last.red_damage);
            put("blue_damage", last.blue_damage);
            put("red_base", last.red_base);
            put("blue_base", last.blue_base);
            QJsonArray trend;
            for (const MatchSample& sample : samples)
                if (sample.red_control && sample.blue_control)
                {
                    const double diff = static_cast<double>(*sample.red_control) - static_cast<double>(*sample.blue_control);
                    trend.append(QJsonArray{sample.elapsed, ours == kRed ? diff : -diff});
                }
            root[QStringLiteral("trend")] = trend;
        }
        QJsonArray clips;
        for (const ReportClip& clip : report_clips(log, context))
            clips.append(QJsonObject{{QStringLiteral("index"), clip.index},
                                     {QStringLiteral("text"), clip.text},
                                     {QStringLiteral("countdown"), clip.countdown},
                                     {QStringLiteral("from"), clip.from_s},
                                     {QStringLiteral("to"), clip.to_s},
                                     {QStringLiteral("mp4"), clip.mp4}});
        root[QStringLiteral("clips")] = clips;
        return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
    }

    namespace
    {

        bool write_text(const QString& path, const QString& text, QString* error)
        {
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            {
                if (error)
                    *error = file.errorString();
                return false;
            }
            file.write(text.toUtf8());
            return true;
        }

    } // namespace

    QString write_match_report(const QString& directory, const MatchLog& log, const ReportContext& context,
                               QString* error)
    {
        if (!QDir().mkpath(directory))
        {
            if (error)
                *error = QStringLiteral("建不了目录 %1").arg(directory);
            return QString();
        }
        ReportContext named = context;
        named.stem = context.finished_at.toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss")) +
                     QStringLiteral("_第%1局").arg(log.round() ? QString::number(*log.round()) : QStringLiteral("x"));
        const QDir dir(directory);
        const QString path = dir.filePath(named.stem + QStringLiteral(".html"));
        if (!write_text(path, match_report_html(log, named), error))
            return QString();
        // PDF 是后台另起浏览器画的,对比页写的时候它多半还没好:摘要里记一笔「在出」,对比页照样挂链接。
        QString summary = match_report_json(log, named);
        if (context.pdf && start_report_pdf(path))
        {
            QJsonObject object = QJsonDocument::fromJson(summary.toUtf8()).object();
            object[QStringLiteral("pdf")] = named.stem + QStringLiteral(".pdf");
            summary = QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Indented));
        }
        if (!write_text(dir.filePath(named.stem + QStringLiteral(".json")), summary, error))
            return QString();
        write_report_index(directory, nullptr);
        return path;
    }

    QString write_report_index(const QString& directory, QString* error)
    {
        const QDir dir(directory);
        struct Row
        {
            QJsonObject json;
            QString stem;
        };
        std::vector<Row> rows;
        for (const QFileInfo& info : dir.entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Name))
        {
            QFile file(info.filePath());
            if (!file.open(QIODevice::ReadOnly))
                continue;
            const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
            if (doc.isObject() && doc.object().contains(QStringLiteral("report")))
                rows.push_back({doc.object(), info.completeBaseName()});
        }
        static const char* palette[] = {"#8ed2f2", "#ecaa46", "#5fd39a", "#ff9a9a", "#c792ea", "#f2e08e", "#84c2ff", "#ff6b6b"};
        QString html = QStringLiteral(
            "<!doctype html><html lang='zh'><head><meta charset='utf-8'><title>多局对比</title><style>"
            "body{background:#0b1220;color:#dce7f2;font-family:'Noto Sans CJK SC','PingFang SC',sans-serif;margin:24px auto;max-width:1100px}"
            "h1{color:#8ed2f2}.card{background:#121c2c;border:1px solid #23344d;border-radius:8px;padding:16px 20px;margin:14px 0}"
            ".muted{color:#7f93a8}.red{color:#ff6b6b}.blue{color:#5aa9ff}a{color:#8ed2f2}"
            "table{border-collapse:collapse;width:100%}td,th{padding:5px 8px;border-bottom:1px solid #23344d;text-align:left}"
            "th{color:#7f93a8;font-weight:500}</style></head><body><h1>多局对比</h1>"
            "<div class='muted'>这个文件夹里的每一份复盘，一局一行。线是我方控制度减敌方控制度，在 0 上面是我方领先。</div>");
        // 一张图:每局一条控制度差的线。
        constexpr int w = 1060, h = 300, pad = 40;
        double span = 10;
        for (const Row& row : rows)
            for (const auto& point : row.json.value(QStringLiteral("trend")).toArray())
                span = std::max(span, std::fabs(point.toArray().at(1).toDouble()));
        const auto x_of = [&](double t)
        { return pad + (w - 2 * pad) * t / 420.0; };
        const auto y_of = [&](double v)
        { return h / 2.0 - (h / 2.0 - pad) * v / span; };
        QString svg = QStringLiteral("<svg viewBox='0 0 %1 %2' width='100%'>").arg(w).arg(h);
        svg += QStringLiteral("<line x1='%1' y1='%2' x2='%3' y2='%2' stroke='#7f93a8' stroke-dasharray='4 4'/>")
                   .arg(pad)
                   .arg(h / 2)
                   .arg(w - pad);
        for (int t = 0; t <= 420; t += 60)
            svg += QStringLiteral("<text x='%1' y='%2' fill='#7f93a8' font-size='12' text-anchor='middle'>%3</text>")
                       .arg(x_of(t))
                       .arg(h - 10)
                       .arg(clock(420 - t));
        svg += QStringLiteral("<text x='4' y='%1' fill='#7f93a8' font-size='12'>+%2</text>").arg(pad).arg(span, 0, 'f', 0);
        svg += QStringLiteral("<text x='4' y='%1' fill='#7f93a8' font-size='12'>-%2</text>").arg(h - pad).arg(span, 0, 'f', 0);
        for (std::size_t i = 0; i < rows.size(); ++i)
        {
            QStringList points;
            for (const auto& point : rows[i].json.value(QStringLiteral("trend")).toArray())
                points << QStringLiteral("%1,%2").arg(x_of(point.toArray().at(0).toDouble()), 0, 'f', 1).arg(y_of(point.toArray().at(1).toDouble()), 0, 'f', 1);
            svg += QStringLiteral("<polyline fill='none' stroke='%1' stroke-width='2' points='%2'/>")
                       .arg(QString::fromLatin1(palette[i % 8]), points.join(QLatin1Char(' ')));
        }
        html += QStringLiteral("<div class='card'>%1</div>").arg(svg + QStringLiteral("</svg>"));
        html += QStringLiteral("<div class='card'><table><tr><th></th><th>结束</th><th>第几局</th><th>席位</th><th>结果</th>"
                               "<th>控制度 红:蓝</th><th>总伤害 红:蓝</th><th>基地 红:蓝</th><th>报告</th></tr>");
        const auto pair = [](const QJsonObject& o, const char* red, const char* blue)
        {
            const auto text = [&o](const char* key)
            {
                return o.contains(QString::fromLatin1(key)) ? QString::number(o.value(QString::fromLatin1(key)).toDouble(), 'f', 0)
                                                            : QStringLiteral("--");
            };
            return QStringLiteral("<span class='red'>%1</span> : <span class='blue'>%2</span>").arg(text(red), text(blue));
        };
        for (std::size_t i = 0; i < rows.size(); ++i)
        {
            const QJsonObject& o = rows[i].json;
            QString links = QStringLiteral("<a href='%1'>网页</a>").arg(o.value(QStringLiteral("report")).toString().toHtmlEscaped());
            if (o.contains(QStringLiteral("pdf")) || dir.exists(rows[i].stem + QStringLiteral(".pdf")))
                links += QStringLiteral(" · <a href='%1.pdf'>PDF</a>").arg(rows[i].stem.toHtmlEscaped());
            html += QStringLiteral("<tr><td><span style='color:%1'>━</span> %2</td><td>%3</td><td>%4</td><td>%5</td><td>%6</td>"
                                   "<td>%7</td><td>%8</td><td>%9</td><td>%10</td></tr>")
                        .arg(QString::fromLatin1(palette[i % 8]))
                        .arg(i + 1)
                        .arg(o.value(QStringLiteral("finished")).toString().toHtmlEscaped())
                        .arg(o.value(QStringLiteral("round")).toInt())
                        .arg(o.value(QStringLiteral("seat")).toString().toHtmlEscaped())
                        .arg(o.value(QStringLiteral("verdict")).toString().toHtmlEscaped())
                        .arg(pair(o, "red_control", "blue_control"))
                        .arg(pair(o, "red_damage", "blue_damage"))
                        .arg(pair(o, "red_base", "blue_base"))
                        .arg(links);
        }
        if (rows.empty())
            html += QStringLiteral("<tr><td colspan='9' class='muted'>还没有复盘</td></tr>");
        html += QStringLiteral("</table></div></body></html>\n");
        const QString path = dir.filePath(QStringLiteral("index.html"));
        return write_text(path, html, error) ? path : QString();
    }

    bool start_report_pdf(const QString& html_path)
    {
        QString browser;
        for (const QString& name : {QStringLiteral("google-chrome"), QStringLiteral("chromium"), QStringLiteral("chromium-browser"),
                                    QStringLiteral("microsoft-edge")})
            if (browser = QStandardPaths::findExecutable(name); !browser.isEmpty())
                break;
        if (browser.isEmpty())
        {
            const QString mac = QStringLiteral("/Applications/Google Chrome.app/Contents/MacOS/Google Chrome");
            if (QFileInfo(mac).isExecutable())
                browser = mac;
        }
        if (browser.isEmpty())
            return false;
        const QFileInfo html(html_path);
        const QString pdf = html.absoluteDir().filePath(html.completeBaseName() + QStringLiteral(".pdf"));
        // 单独一个空的用户目录:操作手自己开着浏览器时不和它抢配置。
        const QString profile = QDir(QDir::tempPath()).filePath(QStringLiteral("rm-terminal-report-pdf"));
        return QProcess::startDetached(browser, {QStringLiteral("--headless=new"), QStringLiteral("--disable-gpu"),
                                                 QStringLiteral("--no-pdf-header-footer"),
                                                 QStringLiteral("--user-data-dir=") + profile,
                                                 QStringLiteral("--print-to-pdf=") + pdf,
                                                 QUrl::fromLocalFile(html.absoluteFilePath()).toString()});
    }

} // namespace rm_terminal
