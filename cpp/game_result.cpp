#include "game_result.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFileInfoList>
#include <QRegularExpression>
#include <QUrl>

#include <algorithm>

namespace rm_terminal {

namespace {

constexpr std::uint32_t kStageSettlement = 5;

constexpr std::uint32_t kWinnerDraw = 0;
constexpr std::uint32_t kWinnerRed = 1;
constexpr std::uint32_t kWinnerBlue = 2;

}  // namespace

GameResultInputs game_result_inputs(const Snapshot& snapshot) {
    GameResultInputs inputs;
    inputs.match_freshness = snapshot.game.current_stage.freshness;
    inputs.stage = snapshot.game.current_stage.value;
    inputs.winner = snapshot.game.winner.value;
    return inputs;
}

GameResult resolve_game_result(const GameResultInputs& inputs) {
    if (inputs.match_freshness != Freshness::Fresh) return GameResult::None;
    if (!inputs.stage.has_value() || *inputs.stage != kStageSettlement)
        return GameResult::None;

    if (!inputs.winner.has_value()) return GameResult::None;
    switch (*inputs.winner) {
        case kWinnerRed:  return GameResult::RedWin;
        case kWinnerBlue: return GameResult::BlueWin;
        case kWinnerDraw: return GameResult::Draw;
        default:          return GameResult::None;
    }
}

QString game_result_frame_dir(GameResult result) {
    switch (result) {
        case GameResult::RedWin:     return QStringLiteral("red_win_zh");
        case GameResult::BlueWin:    return QStringLiteral("blue_win_zh");
        case GameResult::Terminated: return QStringLiteral("abnormal_termination_zh");
        case GameResult::Draw:
        case GameResult::None:
            return QString();
    }
    return QString();
}

QStringList game_result_frame_urls(GameResult result) {
    const QString subdir = game_result_frame_dir(result);
    if (subdir.isEmpty()) return {};

    // 与上游同样的候选路径顺序:先 qrc(打包版),再可执行文件同级与上溯若干层
    // (开发构建),最后工作目录。红胜/蓝胜共 550MB,刻意不进 qrc,所以磁盘兜底
    // 不是可选项而是主路径。
    const QString app_dir = QCoreApplication::applicationDirPath();
    const QStringList candidates{
        QStringLiteral(":/images/resultpanel/") + subdir,
        app_dir + QStringLiteral("/resources/images/resultpanel/") + subdir,
        app_dir + QStringLiteral("/../resources/images/resultpanel/") + subdir,
        app_dir + QStringLiteral("/../../resources/images/resultpanel/") + subdir,
        app_dir + QStringLiteral("/../../../resources/images/resultpanel/") + subdir,
        QDir::currentPath() + QStringLiteral("/resources/images/resultpanel/") + subdir,
    };

    QDir dir;
    bool found = false;
    for (const QString& candidate : candidates) {
        dir.setPath(candidate);
        if (dir.exists()) {
            found = true;
            break;
        }
    }
    if (!found) return {};

    dir.setNameFilters({QStringLiteral("*.png")});
    QFileInfoList files = dir.entryInfoList(QDir::Files);
    if (files.isEmpty()) return {};

    // 按文件名里的序号排,而不是按字典序:字典序会把 (100) 排到 (2) 前面,
    // 动画会从中段开始播。
    static const QRegularExpression number(QStringLiteral("(\\d+)"));
    const auto frame_number = [](const QString& name) {
        const QRegularExpressionMatch match = number.match(name);
        return match.hasMatch() ? match.captured(1).toInt() : 0;
    };
    std::sort(files.begin(), files.end(),
              [&frame_number](const QFileInfo& a, const QFileInfo& b) {
                  return frame_number(a.fileName()) < frame_number(b.fileName());
              });

    QStringList urls;
    urls.reserve(files.size());
    for (const QFileInfo& file : files) {
        const QString path = file.filePath();
        urls.append(path.startsWith(QLatin1Char(':'))
                        ? QStringLiteral("qrc") + path
                        : QUrl::fromLocalFile(file.absoluteFilePath()).toString());
    }
    return urls;
}

QString game_result_title(GameResult result) {
    switch (result) {
        case GameResult::RedWin:     return QStringLiteral("红方胜利");
        case GameResult::BlueWin:    return QStringLiteral("蓝方胜利");
        case GameResult::Draw:       return QStringLiteral("平局");
        case GameResult::Terminated: return QStringLiteral("比赛终止");
        case GameResult::None:
            return QString();
    }
    return QString();
}

}  // namespace rm_terminal
