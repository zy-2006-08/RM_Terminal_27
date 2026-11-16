#pragma once

#include "overview.h"

#include <QDateTime>
#include <QString>

namespace rm_terminal {

// 赛后复盘报告(大局判断表.md 第 6 节):一局一份网页,图是内嵌的 SVG,离线也能打开。
// 数据全是这台终端在这一局里自己记的(MatchLog)。
struct ReportContext {
    std::uint32_t self_id = 0;
    QString recording;  // 这一局所在的录像文件;没录像时为空
    QDateTime finished_at;
};

QString match_report_html(const MatchLog& log, const ReportContext& context);
// 写进 directory,返回文件路径;写不了时返回空,原因放进 error。
QString write_match_report(const QString& directory, const MatchLog& log, const ReportContext& context,
                           QString* error);

}  // namespace rm_terminal
