#include "game_result.h"

#include "presentation.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFileInfoList>
#include <QRegularExpression>
#include <QUrl>
#include <QVariantList>

#include <algorithm>

namespace rm_terminal
{

    namespace
    {

        constexpr std::uint32_t kStageSettlement = 5;

        constexpr std::uint32_t kWinnerDraw = 0;
        constexpr std::uint32_t kWinnerRed = 1;
        constexpr std::uint32_t kWinnerBlue = 2;

        template <class T>
        std::optional<T> valid(const Field<T>& field)
        {
            if (field.quality != Quality::Valid)
                return std::nullopt;
            return field.value;
        }

        template <class T>
        std::optional<std::int64_t> valid_int(const Field<T>& field)
        {
            const std::optional<T> value = valid(field);
            if (!value)
                return std::nullopt;
            return static_cast<std::int64_t>(*value);
        }

        // nullopt 和 0 在 QVariantMap 里长得一样,所以成对写:值 + hasValue。
        void put(QVariantMap& map, const QString& key, std::optional<std::int64_t> value)
        {
            map[key] = value ? QVariant(static_cast<qlonglong>(*value)) : QVariant(0);
            map[key + QStringLiteral("Known")] = value.has_value();
        }

        // 和 roster_pane.h 的 self_faction 同一条规则。不直接用它:那个声明在
        // terminal_visual 里,而本文件属于 terminal_presentation,反向依赖会成环。
        std::optional<std::uint32_t> settlement_self_faction(const Snapshot& snapshot)
        {
            if (!snapshot.self)
                return std::nullopt;
            return faction_of(snapshot.self->value);
        }

        QString rune_state_text(const Field<std::uint32_t>& status)
        {
            const std::optional<std::uint32_t> value = valid(status);
            if (!value)
                return QString();
            switch (*value)
            {
            case 1: return QStringLiteral("未激活");
            case 2: return QStringLiteral("激活中");
            case 3: return QStringLiteral("已激活");
            default: return QString();
            }
        }

        // 这一方所有在场机器人当前血量之和。协议只发 1/3/4/7 的血量,所以这是「花名册上
        // 那几台」的和,不是全队。一台都没收到时返回 nullopt:0 会被读成「全队覆灭」。
        std::optional<std::int64_t> faction_hp_sum(const Snapshot& snapshot, std::uint32_t faction)
        {
            std::optional<std::int64_t> sum;
            for (const RobotHealth& robot : snapshot.robot_health)
            {
                if (robot.faction != faction)
                    continue;
                const std::optional<std::int64_t> hp = valid_int(robot.current_hp);
                if (!hp)
                    continue;
                sum = sum.value_or(0) + *hp;
            }
            return sum;
        }

        std::vector<SettlementRobot> faction_robots(const Snapshot& snapshot, std::uint32_t faction)
        {
            std::vector<SettlementRobot> out;
            for (const RobotHealth& robot : snapshot.robot_health)
            {
                if (robot.faction != faction)
                    continue;
                SettlementRobot entry;
                entry.id = robot.id.value;
                entry.number = display_robot_number(robot.id.value);
                entry.cls = robot_class_name(robot.id.value);
                entry.hp = valid(robot.current_hp);
                entry.max_hp = valid(robot.max_hp);
                out.push_back(entry);
            }
            std::sort(out.begin(), out.end(), [](const SettlementRobot& a, const SettlementRobot& b)
                      { return a.number < b.number; });
            return out;
        }

        QVariantList robot_variants(const std::vector<SettlementRobot>& robots)
        {
            QVariantList out;
            for (const SettlementRobot& robot : robots)
            {
                QVariantMap map;
                map[QStringLiteral("id")] = robot.id;
                map[QStringLiteral("number")] = robot.number;
                map[QStringLiteral("cls")] = robot.cls;
                map[QStringLiteral("hasHp")] = robot.hp.has_value() && robot.max_hp.has_value();
                map[QStringLiteral("hp")] = robot.hp ? *robot.hp : 0u;
                map[QStringLiteral("maxHp")] = robot.max_hp ? *robot.max_hp : 0u;
                out.append(map);
            }
            return out;
        }

    } // namespace

    GameResultInputs game_result_inputs(const Snapshot& snapshot)
    {
        GameResultInputs inputs;
        inputs.match_freshness = snapshot.game.current_stage.freshness;
        inputs.stage = snapshot.game.current_stage.value;
        inputs.winner = snapshot.game.winner.value;
        return inputs;
    }

    GameResult resolve_game_result(const GameResultInputs& inputs)
    {
        if (inputs.match_freshness != Freshness::Fresh)
            return GameResult::None;
        if (!inputs.stage.has_value() || *inputs.stage != kStageSettlement)
            return GameResult::None;

        if (!inputs.winner.has_value())
            return GameResult::None;
        switch (*inputs.winner)
        {
        case kWinnerRed: return GameResult::RedWin;
        case kWinnerBlue: return GameResult::BlueWin;
        case kWinnerDraw: return GameResult::Draw;
        default: return GameResult::None;
        }
    }

    QString game_result_frame_dir(GameResult result)
    {
        switch (result)
        {
        case GameResult::RedWin: return QStringLiteral("red_win_zh");
        case GameResult::BlueWin: return QStringLiteral("blue_win_zh");
        case GameResult::Terminated: return QStringLiteral("abnormal_termination_zh");
        case GameResult::Draw:
        case GameResult::None:
            return QString();
        }
        return QString();
    }

    QStringList game_result_frame_urls(GameResult result)
    {
        const QString subdir = game_result_frame_dir(result);
        if (subdir.isEmpty())
            return {};

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
        for (const QString& candidate : candidates)
        {
            dir.setPath(candidate);
            if (dir.exists())
            {
                found = true;
                break;
            }
        }
        if (!found)
            return {};

        dir.setNameFilters({QStringLiteral("*.png")});
        QFileInfoList files = dir.entryInfoList(QDir::Files);
        if (files.isEmpty())
            return {};

        // 按文件名里的序号排,而不是按字典序:字典序会把 (100) 排到 (2) 前面,
        // 动画会从中段开始播。
        static const QRegularExpression number(QStringLiteral("(\\d+)"));
        const auto frame_number = [](const QString& name)
        {
            const QRegularExpressionMatch match = number.match(name);
            return match.hasMatch() ? match.captured(1).toInt() : 0;
        };
        std::sort(files.begin(), files.end(),
                  [&frame_number](const QFileInfo& a, const QFileInfo& b)
                  {
                      return frame_number(a.fileName()) < frame_number(b.fileName());
                  });

        QStringList urls;
        urls.reserve(files.size());
        for (const QFileInfo& file : files)
        {
            const QString path = file.filePath();
            urls.append(path.startsWith(QLatin1Char(':'))
                            ? QStringLiteral("qrc") + path
                            : QUrl::fromLocalFile(file.absoluteFilePath()).toString());
        }
        return urls;
    }

    QString game_result_title(GameResult result)
    {
        switch (result)
        {
        case GameResult::RedWin: return QStringLiteral("红方胜利");
        case GameResult::BlueWin: return QStringLiteral("蓝方胜利");
        case GameResult::Draw: return QStringLiteral("平局");
        case GameResult::Terminated: return QStringLiteral("比赛终止");
        case GameResult::None:
            return QString();
        }
        return QString();
    }

    QString game_result_outcome(GameResult result)
    {
        switch (result)
        {
        case GameResult::RedWin: return QStringLiteral("red");
        case GameResult::BlueWin: return QStringLiteral("blue");
        case GameResult::Draw: return QStringLiteral("draw");
        case GameResult::Terminated:
        case GameResult::None:
            return QString();
        }
        return QString();
    }

    QString game_result_reason(GameResult result, const Field<std::uint32_t>& end_reason)
    {
        if (result == GameResult::None)
            return QString();
        const std::optional<std::uint32_t> code = valid(end_reason);
        if (!code)
            return QString();

        const bool red_won = result == GameResult::RedWin;
        const QString winner = red_won ? QStringLiteral("红方") : QStringLiteral("蓝方");
        const QString loser = red_won ? QStringLiteral("蓝方") : QStringLiteral("红方");

        switch (*code)
        {
        case 1:
            // 规则 5.8 第 1 条:基地被击毁时基地剩余血量高的一方获胜,所以倒下的是败方。
            if (result == GameResult::Draw)
                return QString();
            return QStringLiteral("%1基地被击毁").arg(loser);
        case 2:
            if (result == GameResult::Draw)
                return QString();
            return QStringLiteral("比赛时间到，%1累计控制度更高").arg(winner);
        case 3:
            if (result == GameResult::Draw)
                return QString();
            return QStringLiteral("比赛时间到，控制度持平，%1总伤害更高").arg(winner);
        case 4:
            if (result == GameResult::Draw)
                return QString();
            return QStringLiteral("比赛时间到，控制度与总伤害持平，%1总剩余血量更高").arg(winner);
        case 5:
            // 编码 5 本身就是「平局」。分出了胜负却报 5,两个字段互相矛盾,
            // 宁可不说也不要替服务器编一个理由。
            if (result != GameResult::Draw)
                return QString();
            return QStringLiteral("比赛时间到，双方各项条件均持平");
        case 6:
            return QStringLiteral("比赛异常终止");
        case 7:
            if (result == GameResult::Draw)
                return QStringLiteral("主裁判定为平局");
            return QStringLiteral("主裁判定%1获胜").arg(winner);
        default:
            // 255 是协议规定的「非结算阶段」占位值,其余编码协议没有定义。
            return QString();
        }
    }

    SettlementModel settlement_model(const Snapshot& snapshot)
    {
        const GameFields<Field>& g = snapshot.game;
        SettlementModel model;
        model.result = resolve_game_result(game_result_inputs(snapshot));
        model.title = game_result_title(model.result);
        model.reason = game_result_reason(model.result, g.end_reason);
        model.round = valid(g.current_round);
        model.total_rounds = valid(g.total_rounds);
        model.red_score = valid(g.red_score);
        model.blue_score = valid(g.blue_score);

        const auto row = [](QString label, std::optional<std::int64_t> red,
                            std::optional<std::int64_t> blue, std::int64_t scale,
                            QString icon = QString())
        {
            SettlementRow out;
            out.label = std::move(label);
            out.red = red;
            out.blue = blue;
            out.scale = scale;
            out.icon = std::move(icon);
            return out;
        };

        // 量程取双方上限里的大值,两侧的条才同标尺;上限缺失时传 0,让 QML 按两侧
        // 较大值自己定标 —— 否则一侧缺上限会把两根条画成不同比例。
        const std::int64_t base_scale =
            std::max(valid_int(g.red_base_max_hp).value_or(0), valid_int(g.blue_base_max_hp).value_or(0));
        const std::int64_t outpost_scale = std::max(valid_int(g.red_outpost_max_hp).value_or(0),
                                                    valid_int(g.blue_outpost_max_hp).value_or(0));

        model.rows.push_back(row(QStringLiteral("基地血量"), valid_int(g.red_base_hp),
                                 valid_int(g.blue_base_hp), base_scale));
        model.rows.push_back(row(QStringLiteral("前哨站血量"), valid_int(g.red_outpost_hp),
                                 valid_int(g.blue_outpost_hp), outpost_scale));
        model.rows.push_back(row(QStringLiteral("总伤害"), valid_int(g.red_total_damage),
                                 valid_int(g.blue_total_damage), 0));

        // 能量机关只有我方:协议的 RuneStatusSync 不含对方。所以这一行的两侧里,
        // 只有我方那一侧写得出状态,另一侧留空显示「--」,不借对方的数填。
        SettlementRow rune;
        rune.label = QStringLiteral("能量机关");
        rune.icon = QStringLiteral("qrc:/images/message/validate_icon_rune.png");
        if (const std::optional<std::uint32_t> self = settlement_self_faction(snapshot))
        {
            const QString text = rune_state_text(g.rune_status);
            if (*self == kRed)
                rune.red_text = text;
            else
                rune.blue_text = text;
        }
        model.rows.push_back(rune);

        // 击杀在本机累计(Event 1),终端收到过比赛状态才算在数;没收到过时 0 只是
        // 「还没开始数」,报成 0 会把它说成「一个都没杀」。
        const bool counting = g.current_stage.quality == Quality::Valid;
        model.rows.push_back(row(QStringLiteral("击杀数"),
                                 counting ? std::optional<std::int64_t>(snapshot.kills.red) : std::nullopt,
                                 counting ? std::optional<std::int64_t>(snapshot.kills.blue) : std::nullopt,
                                 0));
        model.rows.push_back(row(QStringLiteral("机器人剩余血量"), faction_hp_sum(snapshot, kRed),
                                 faction_hp_sum(snapshot, kBlue), 0));

        model.red_robots = faction_robots(snapshot, kRed);
        model.blue_robots = faction_robots(snapshot, kBlue);
        return model;
    }

    QVariantMap settlement_variant(const SettlementModel& model)
    {
        QVariantMap map;
        map[QStringLiteral("title")] = model.title;
        map[QStringLiteral("reason")] = model.reason;
        map[QStringLiteral("redWin")] = model.result == GameResult::RedWin;
        map[QStringLiteral("blueWin")] = model.result == GameResult::BlueWin;
        map[QStringLiteral("draw")] = model.result == GameResult::Draw;
        put(map, QStringLiteral("round"), model.round ? std::optional<std::int64_t>(*model.round) : std::nullopt);
        put(map, QStringLiteral("totalRounds"),
            model.total_rounds ? std::optional<std::int64_t>(*model.total_rounds) : std::nullopt);
        put(map, QStringLiteral("redScore"),
            model.red_score ? std::optional<std::int64_t>(*model.red_score) : std::nullopt);
        put(map, QStringLiteral("blueScore"),
            model.blue_score ? std::optional<std::int64_t>(*model.blue_score) : std::nullopt);
        put(map, QStringLiteral("durationSec"),
            model.duration_sec ? std::optional<std::int64_t>(*model.duration_sec) : std::nullopt);

        QVariantList rows;
        for (const SettlementRow& row : model.rows)
        {
            QVariantMap entry;
            entry[QStringLiteral("label")] = row.label;
            entry[QStringLiteral("icon")] = row.icon;
            entry[QStringLiteral("redText")] = row.red_text;
            entry[QStringLiteral("blueText")] = row.blue_text;
            entry[QStringLiteral("scale")] = static_cast<qlonglong>(row.scale);
            put(entry, QStringLiteral("red"), row.red);
            put(entry, QStringLiteral("blue"), row.blue);
            rows.append(entry);
        }
        map[QStringLiteral("rows")] = rows;
        map[QStringLiteral("redRobots")] = robot_variants(model.red_robots);
        map[QStringLiteral("blueRobots")] = robot_variants(model.blue_robots);
        return map;
    }

} // namespace rm_terminal
