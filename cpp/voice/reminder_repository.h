#pragma once

#include "tactical_reminder.h"

#include <QString>

namespace rm_terminal {

// 加载结果三态。`Missing` 和 `Invalid` 必须分开:文件不存在是全新安装的正常状态,
// 应该静默地给出空配置;文件存在但读不懂是**数据事故**,绝不能静默覆盖 ——
// 操作手赛前填的一整套战术不该因为一次解析失败而被清空。
enum class ReminderLoadResult { Loaded, Missing, Invalid };

struct ReminderLoad {
    ReminderLoadResult result = ReminderLoadResult::Missing;
    ReminderConfig config;
    QString error;
};

// 长期保存战术提醒配置。写入走 QSaveFile 的原子替换:比赛前一秒断电也不会
// 留下半个文件,要么是旧的完整配置,要么是新的完整配置。
class ReminderRepository {
public:
    // 目录可注入,测试因此不会污染真实用户配置,也不需要真的 HOME。
    explicit ReminderRepository(QString directory);

    // 默认落点是 QStandardPaths 的稳定应用目录,升级和重启后路径不变。
    static QString defaultDirectory();

    QString filePath() const;

    ReminderLoad load() const;

    // 校验不通过时**不落盘**并返回 false,`error` 说明原因。校验规则来自
    // tactical_reminder.h 的共享常量,与界面的输入限制是同一组数字。
    bool save(const ReminderConfig& config, QString* error) const;

private:
    QString directory_;
};

}
