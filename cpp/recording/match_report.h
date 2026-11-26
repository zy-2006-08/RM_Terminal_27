#pragma once

#include "overview.h"

#include <QDateTime>
#include <QString>

namespace rm_terminal
{

    // 赛后复盘报告(大局判断表.md 第 6 节):一局一份网页,图是内嵌的 SVG,离线也能打开。
    // 数据全是这台终端在这一局里自己记的(MatchLog)。
    struct ReportContext
    {
        std::uint32_t self_id = 0;
        QString recording; // 这一局所在的录像文件;没录像时为空
        QDateTime finished_at;
        QString stem;     // 报告的文件名(不带扩展名);片段 MP4 叫 clips/<stem>-01.mp4
        bool pdf = false; // 写完网页顺手在后台另存 PDF(要 Chrome / Chromium / Edge)
    };

    // 关键事件的片段:录像里从事件前 10 秒到后 5 秒(录像开头之前的截掉)。没录像就没有片段。
    constexpr double kClipBeforeSec = 10.0;
    constexpr double kClipAfterSec = 5.0;
    struct ReportClip
    {
        int index = 0;
        QString text;
        QString countdown;
        double from_s = 0;
        double to_s = 0;
        QString mp4; // 相对报告所在目录
    };
    std::vector<ReportClip> report_clips(const MatchLog& log, const ReportContext& context);

    QString match_report_html(const MatchLog& log, const ReportContext& context);
    // 多局对比和导片段脚本用的摘要:结果、几个总数、控制度差每秒一个点、片段。
    QString match_report_json(const MatchLog& log, const ReportContext& context);
    // 写网页和摘要(<stem>.html、<stem>.json)进 directory,再把多局对比页重出一遍。
    // 返回网页路径;写不了时返回空,原因放进 error。
    QString write_match_report(const QString& directory, const MatchLog& log, const ReportContext& context,
                               QString* error);
    // 把 directory 里所有摘要汇成 index.html:每局一行,控制度差的线叠在一张图上。
    QString write_report_index(const QString& directory, QString* error);
    // 这台电脑上有 Chrome / Chromium / Edge 就在后台把网页另存成同名 PDF,返回是否开始了。
    bool start_report_pdf(const QString& html_path);

} // namespace rm_terminal
