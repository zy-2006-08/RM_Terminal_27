#pragma once

#include "ui_profile.h"

#include <QObject>
#include <QString>
#include <QStringList>

namespace rm_terminal {

enum class UiProfileLoadResult { Loaded, Missing, Invalid };

struct UiProfileLoad {
    UiProfileLoadResult result = UiProfileLoadResult::Missing;
    UiProfileBook book;
    QString error;
};

// 个性化设置的文件。和战术提醒同一套规矩:写入走 QSaveFile 原子替换;文件不存在是
// 第一次用,给默认;文件在但读不懂是事故,绝不静默覆盖 —— 下次保存前先把它改名留底。
class UiProfileRepository {
public:
    explicit UiProfileRepository(QString directory);
    // QStandardPaths::AppConfigLocation,Linux 上是 ~/.config/rm_terminal。
    static QString defaultDirectory();

    QString filePath() const;
    UiProfileLoad load() const;
    // 读一个任意路径的文件(导入用),规则同 load。
    static UiProfileLoad loadFrom(const QString& path);
    bool save(const UiProfileBook& book, QString* error) const;
    static bool saveTo(const QString& path, const UiProfileBook& book, QString* error);

    // 读不懂的旧文件改名成 ui_profiles.bad-N.json,返回新路径;没有旧文件返回空串。
    QString setAsideUnreadable(QString* error) const;

private:
    QString directory_;
};

// 设置页和主界面共用的那一份设置。每次改动立刻存盘:比赛前改完直接关终端也不会丢。
class UiProfileController final : public QObject {
    Q_OBJECT
public:
    UiProfileController(UiProfileRepository repository, Role role, QObject* parent = nullptr);

    Role role() const { return role_; }
    UiProfileLoadResult loadResult() const { return load_result_; }
    // 给操作手看的状态:读不懂、保存失败时说清楚,正常时是空串。
    QString problem() const { return problem_; }

    QStringList operatorNames() const;
    QString currentOperator() const { return book_.last_operator; }
    bool switchOperator(const QString& name);
    // 新建的操作手从「标准」开始;成功后切过去。失败返回原因。
    QString addOperator(const QString& name);
    QString renameOperator(const QString& from, const QString& to);
    // 至少留一个操作手。
    QString removeOperator(const QString& name);

    const RoleSetup& setup() const;
    const UiScheme& activeScheme() const { return active_scheme(setup()); }
    bool selectScheme(const QString& id);
    // 改当前方案:藏哪些面板、细项。
    void setPaneHidden(PaneId pane, bool hidden);
    void setOptions(const UiOptions& options);
    // 界面设置页每改一下:格子、开关和细项一起换成 draft 里的,方案的名字和 id 不变。
    bool saveLayout(const UiScheme& draft);
    // 「撤销」:把这个席位的方案整份换回之前记下的那份(包括当时用的是哪个方案)。
    bool restoreSetup(const RoleSetup& setup);
    // 复制当前方案,起新名字并切过去。
    QString createScheme(const QString& name);
    QString renameScheme(const QString& id, const QString& name);
    QString removeScheme(const QString& id);
    // 内置方案改回出厂值;自建方案没有出厂值,返回原因。
    QString resetScheme(const QString& id);

    // 导出当前操作手(所有席位的方案)。
    bool exportCurrent(const QString& path, QString* error) const;
    // 导入一个导出文件或整份备份。同名操作手会被替换,`replaced` 列出被替换的名字。
    QString importFrom(const QString& path, QStringList* replaced);
    // 导入前先看看文件里有谁,设置页据此问「要替换吗」。
    static QStringList operatorsIn(const QString& path, QString* error);

    // 是否锁定(比赛中)。锁定时所有修改直接拒绝,和界面置灰是同一条规则。
    void setLocked(bool locked);
    bool locked() const { return locked_; }

Q_SIGNALS:
    // 当前方案或操作手变了,主界面据此重排。
    void changed();

private:
    OperatorProfile& current();
    const OperatorProfile& current() const;
    RoleSetup& mutableSetup();
    UiScheme& mutableActive();
    void commit();

    UiProfileRepository repository_;
    Role role_;
    UiProfileBook book_;
    UiProfileLoadResult load_result_ = UiProfileLoadResult::Missing;
    QString problem_;
    bool set_aside_pending_ = false;
    bool locked_ = false;
};

}  // namespace rm_terminal
