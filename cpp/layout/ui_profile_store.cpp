#include "ui_profile_store.h"

#include "logging.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <algorithm>

namespace rm_terminal
{
    namespace
    {

        constexpr auto kFileName = "ui_profiles.json";

        UiProfileLoad read_file(const QString& path)
        {
            UiProfileLoad out;
            QFile file(path);
            if (!file.exists())
                return out;
            out.result = UiProfileLoadResult::Invalid;
            if (!file.open(QIODevice::ReadOnly))
            {
                out.error = QStringLiteral("cannot open %1: %2").arg(path, file.errorString());
                return out;
            }
            QJsonParseError parse{};
            const auto document = QJsonDocument::fromJson(file.readAll(), &parse);
            if (parse.error != QJsonParseError::NoError || !document.isObject())
            {
                out.error = QStringLiteral("malformed JSON in %1: %2").arg(path, parse.errorString());
                return out;
            }
            QString error;
            const auto book = book_from_json(document.object(), &error);
            if (!book)
            {
                out.error = QStringLiteral("%1 in %2").arg(error, path);
                return out;
            }
            out.result = UiProfileLoadResult::Loaded;
            out.book = *book;
            return out;
        }

        QString load_result_name(UiProfileLoadResult result)
        {
            switch (result)
            {
            case UiProfileLoadResult::Loaded: return QStringLiteral("loaded");
            case UiProfileLoadResult::Missing: return QStringLiteral("missing");
            case UiProfileLoadResult::Invalid: return QStringLiteral("invalid");
            }
            return QString();
        }

    } // namespace

    UiProfileRepository::UiProfileRepository(QString directory) : directory_(std::move(directory)) {}

    QString UiProfileRepository::defaultDirectory()
    {
        return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    }

    QString UiProfileRepository::filePath() const { return QDir(directory_).filePath(QString::fromLatin1(kFileName)); }

    UiProfileLoad UiProfileRepository::load() const { return read_file(filePath()); }

    UiProfileLoad UiProfileRepository::loadFrom(const QString& path) { return read_file(path); }

    bool UiProfileRepository::save(const UiProfileBook& book, QString* error) const
    {
        if (!QDir().mkpath(directory_))
        {
            if (error)
                *error = QStringLiteral("cannot create %1").arg(directory_);
            return false;
        }
        return saveTo(filePath(), book, error);
    }

    bool UiProfileRepository::saveTo(const QString& path, const UiProfileBook& book, QString* error)
    {
        QSaveFile file(path);
        // 不允许退回到就地截断再写:写一半失败时所有人的方案就没了。
        file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly))
        {
            if (error)
                *error = QStringLiteral("cannot open %1: %2").arg(path, file.errorString());
            return false;
        }
        const QByteArray payload = QJsonDocument(book_to_json(book)).toJson(QJsonDocument::Indented);
        if (file.write(payload) != payload.size())
        {
            file.cancelWriting();
            if (error)
                *error = QStringLiteral("short write to %1").arg(path);
            return false;
        }
        if (!file.commit())
        {
            if (error)
                *error = QStringLiteral("cannot commit %1: %2").arg(path, file.errorString());
            return false;
        }
        return true;
    }

    QString UiProfileRepository::setAsideUnreadable(QString* error) const
    {
        if (!QFile::exists(filePath()))
            return QString();
        for (int n = 1; n < 1000; ++n)
        {
            const QString target = QDir(directory_).filePath(QStringLiteral("ui_profiles.bad-%1.json").arg(n));
            if (QFile::exists(target))
                continue;
            if (QFile::rename(filePath(), target))
                return target;
            if (error)
                *error = QStringLiteral("cannot rename %1 to %2").arg(filePath(), target);
            return QString();
        }
        if (error)
            *error = QStringLiteral("too many ui_profiles.bad-N.json files");
        return QString();
    }

    UiProfileController::UiProfileController(UiProfileRepository repository, Role role, QObject* parent)
        : QObject(parent), repository_(std::move(repository)), role_(role)
    {
        UiProfileLoad loaded = repository_.load();
        load_result_ = loaded.result;
        if (loaded.result == UiProfileLoadResult::Loaded)
        {
            book_ = std::move(loaded.book);
        }
        else
        {
            normalize(&book_);
        }
        if (loaded.result == UiProfileLoadResult::Invalid)
        {
            set_aside_pending_ = true;
            problem_ = QStringLiteral("设置文件读不懂，先用默认画面。原文件没动，第一次保存时会改名留底。");
        }
        StructuredLog::write(loaded.result == UiProfileLoadResult::Invalid ? LogLevel::warning : LogLevel::info,
                             QStringLiteral("ui_profile"),
                             {QStringLiteral("state=%1").arg(load_result_name(loaded.result)),
                              QStringLiteral("file=%1").arg(repository_.filePath()),
                              QStringLiteral("operator=%1").arg(book_.last_operator),
                              QStringLiteral("error=%1").arg(loaded.error)});
    }

    QStringList UiProfileController::operatorNames() const
    {
        QStringList names;
        for (const OperatorProfile& profile : book_.operators)
            names << profile.name;
        return names;
    }

    OperatorProfile& UiProfileController::current()
    {
        for (OperatorProfile& profile : book_.operators)
            if (profile.name == book_.last_operator)
                return profile;
        return book_.operators.front();
    }

    const OperatorProfile& UiProfileController::current() const
    {
        for (const OperatorProfile& profile : book_.operators)
            if (profile.name == book_.last_operator)
                return profile;
        return book_.operators.front();
    }

    const RoleSetup& UiProfileController::setup() const
    {
        static const RoleSetup infantry = default_role_setup(Role::Infantry);
        static const RoleSetup hero = default_role_setup(Role::Hero);
        static const RoleSetup commander = default_role_setup(Role::Commander);
        const auto found = current().roles.find(role_);
        if (found != current().roles.end())
            return found->second;
        switch (role_)
        {
        case Role::Hero: return hero;
        case Role::Commander: return commander;
        case Role::Infantry: break;
        }
        return infantry;
    }

    RoleSetup& UiProfileController::mutableSetup()
    {
        auto& roles = current().roles;
        auto found = roles.find(role_);
        if (found == roles.end())
            found = roles.emplace(role_, default_role_setup(role_)).first;
        return found->second;
    }

    UiScheme& UiProfileController::mutableActive()
    {
        RoleSetup& setup = mutableSetup();
        for (UiScheme& scheme : setup.schemes)
            if (scheme.id == setup.active)
                return scheme;
        return setup.schemes.front();
    }

    void UiProfileController::commit()
    {
        QString error;
        if (set_aside_pending_)
        {
            const QString aside = repository_.setAsideUnreadable(&error);
            if (!error.isEmpty())
            {
                problem_ = QStringLiteral("没能给读不懂的旧设置文件改名留底，这次改动没有保存：%1").arg(error);
                Q_EMIT changed();
                return;
            }
            set_aside_pending_ = false;
            StructuredLog::write(LogLevel::warning, QStringLiteral("ui_profile"),
                                 {QStringLiteral("state=set_aside"), QStringLiteral("file=%1").arg(aside)});
        }
        if (repository_.save(book_, &error))
        {
            problem_.clear();
        }
        else
        {
            problem_ = QStringLiteral("保存失败：%1").arg(error);
            StructuredLog::write(LogLevel::warning, QStringLiteral("ui_profile"),
                                 {QStringLiteral("state=save_failed"), QStringLiteral("error=%1").arg(error)});
        }
        Q_EMIT changed();
    }

    bool UiProfileController::switchOperator(const QString& name)
    {
        if (locked_ || name == book_.last_operator || !operatorNames().contains(name))
            return false;
        book_.last_operator = name;
        StructuredLog::write(LogLevel::info, QStringLiteral("ui_profile"),
                             {QStringLiteral("state=operator"), QStringLiteral("operator=%1").arg(name)});
        commit();
        return true;
    }

    QString UiProfileController::addOperator(const QString& name)
    {
        if (locked_)
            return QStringLiteral("比赛中不能改");
        if (const QString problem = name_problem(name); !problem.isEmpty())
            return problem;
        if (operatorNames().contains(name))
            return QStringLiteral("已经有叫「%1」的操作手了").arg(name);
        if (book_.operators.size() >= kMaxOperators)
            return QStringLiteral("最多 %1 个操作手").arg(kMaxOperators);
        book_.operators.push_back(default_operator(name));
        book_.last_operator = name;
        commit();
        return {};
    }

    QString UiProfileController::renameOperator(const QString& from, const QString& to)
    {
        if (locked_)
            return QStringLiteral("比赛中不能改");
        if (from == to)
            return {};
        if (const QString problem = name_problem(to); !problem.isEmpty())
            return problem;
        if (operatorNames().contains(to))
            return QStringLiteral("已经有叫「%1」的操作手了").arg(to);
        for (OperatorProfile& profile : book_.operators)
        {
            if (profile.name != from)
                continue;
            profile.name = to;
            if (book_.last_operator == from)
                book_.last_operator = to;
            commit();
            return {};
        }
        return QStringLiteral("没有叫「%1」的操作手").arg(from);
    }

    QString UiProfileController::removeOperator(const QString& name)
    {
        if (locked_)
            return QStringLiteral("比赛中不能改");
        if (book_.operators.size() <= 1)
            return QStringLiteral("至少要留一个操作手");
        const auto found = std::find_if(book_.operators.begin(), book_.operators.end(),
                                        [&](const OperatorProfile& p)
                                        { return p.name == name; });
        if (found == book_.operators.end())
            return QStringLiteral("没有叫「%1」的操作手").arg(name);
        book_.operators.erase(found);
        if (book_.last_operator == name)
            book_.last_operator = book_.operators.front().name;
        commit();
        return {};
    }

    bool UiProfileController::selectScheme(const QString& id)
    {
        if (locked_)
            return false;
        RoleSetup& setup = mutableSetup();
        if (setup.active == id)
            return false;
        const bool exists = std::any_of(setup.schemes.begin(), setup.schemes.end(),
                                        [&](const UiScheme& scheme)
                                        { return scheme.id == id; });
        if (!exists)
            return false;
        setup.active = id;
        commit();
        return true;
    }

    void UiProfileController::setPaneHidden(PaneId pane, bool hidden)
    {
        if (locked_ || !pane_hideable(pane))
            return;
        UiScheme& scheme = mutableActive();
        if (static_cast<bool>(scheme.hidden.count(pane)) == hidden)
            return;
        if (hidden)
            scheme.hidden.insert(pane);
        else
            scheme.hidden.erase(pane);
        normalize(&mutableSetup(), role_);
        commit();
    }

    void UiProfileController::setOptions(const UiOptions& options)
    {
        if (locked_)
            return;
        UiScheme& scheme = mutableActive();
        if (scheme.options == options)
            return;
        scheme.options = options;
        normalize(&mutableSetup(), role_);
        commit();
    }

    bool UiProfileController::saveLayout(const UiScheme& draft)
    {
        if (locked_)
            return false;
        UiScheme& scheme = mutableActive();
        scheme.hidden = draft.hidden;
        scheme.options = draft.options;
        scheme.cells = draft.cells == layout_profile(role_).cells ? std::vector<PaneSlot>{} : draft.cells;
        normalize(&mutableSetup(), role_);
        commit();
        return true;
    }

    bool UiProfileController::restoreSetup(const RoleSetup& setup)
    {
        if (locked_)
            return false;
        mutableSetup() = setup;
        normalize(&mutableSetup(), role_);
        commit();
        return true;
    }

    QString UiProfileController::createScheme(const QString& name)
    {
        if (locked_)
            return QStringLiteral("比赛中不能改");
        if (const QString problem = name_problem(name); !problem.isEmpty())
            return problem;
        RoleSetup& setup = mutableSetup();
        if (setup.schemes.size() >= kMaxSchemesPerRole)
            return QStringLiteral("最多 %1 个方案").arg(kMaxSchemesPerRole);
        for (const UiScheme& scheme : setup.schemes)
            if (scheme.name == name)
                return QStringLiteral("已经有叫「%1」的方案了").arg(name);
        int n = 1;
        const auto taken = [&](const QString& id)
        {
            return std::any_of(setup.schemes.begin(), setup.schemes.end(),
                               [&](const UiScheme& scheme)
                               { return scheme.id == id; });
        };
        while (taken(QStringLiteral("custom-%1").arg(n)))
            ++n;
        UiScheme copy = active_scheme(setup);
        copy.id = QStringLiteral("custom-%1").arg(n);
        copy.name = name;
        setup.schemes.push_back(copy);
        setup.active = copy.id;
        commit();
        return {};
    }

    QString UiProfileController::renameScheme(const QString& id, const QString& name)
    {
        if (locked_)
            return QStringLiteral("比赛中不能改");
        if (is_builtin_scheme(id))
            return QStringLiteral("自带的方案不能改名，可以复制一份再改");
        if (const QString problem = name_problem(name); !problem.isEmpty())
            return problem;
        RoleSetup& setup = mutableSetup();
        for (const UiScheme& scheme : setup.schemes)
            if (scheme.name == name && scheme.id != id)
                return QStringLiteral("已经有叫「%1」的方案了").arg(name);
        for (UiScheme& scheme : setup.schemes)
        {
            if (scheme.id != id)
                continue;
            scheme.name = name;
            commit();
            return {};
        }
        return QStringLiteral("没有这个方案");
    }

    QString UiProfileController::removeScheme(const QString& id)
    {
        if (locked_)
            return QStringLiteral("比赛中不能改");
        if (is_builtin_scheme(id))
            return QStringLiteral("自带的方案不能删，可以恢复默认");
        RoleSetup& setup = mutableSetup();
        const auto found = std::find_if(setup.schemes.begin(), setup.schemes.end(),
                                        [&](const UiScheme& scheme)
                                        { return scheme.id == id; });
        if (found == setup.schemes.end())
            return QStringLiteral("没有这个方案");
        setup.schemes.erase(found);
        if (setup.active == id)
            setup.active = QStringLiteral("standard");
        commit();
        return {};
    }

    QString UiProfileController::resetScheme(const QString& id)
    {
        if (locked_)
            return QStringLiteral("比赛中不能改");
        const std::optional<UiScheme> factory = builtin_scheme(role_, id);
        if (!factory)
            return QStringLiteral("自己建的方案没有出厂设置");
        for (UiScheme& scheme : mutableSetup().schemes)
        {
            if (scheme.id != id)
                continue;
            scheme = *factory;
            commit();
            return {};
        }
        return QStringLiteral("没有这个方案");
    }

    bool UiProfileController::exportCurrent(const QString& path, QString* error) const
    {
        UiProfileBook one;
        one.last_operator = current().name;
        one.operators.push_back(current());
        return UiProfileRepository::saveTo(path, one, error);
    }

    QStringList UiProfileController::operatorsIn(const QString& path, QString* error)
    {
        const UiProfileLoad loaded = UiProfileRepository::loadFrom(path);
        if (loaded.result != UiProfileLoadResult::Loaded)
        {
            if (error)
                *error = loaded.result == UiProfileLoadResult::Missing ? QStringLiteral("文件不存在")
                                                                       : QStringLiteral("文件读不懂：%1").arg(loaded.error);
            return {};
        }
        QStringList names;
        for (const OperatorProfile& profile : loaded.book.operators)
            names << profile.name;
        return names;
    }

    QString UiProfileController::importFrom(const QString& path, QStringList* replaced)
    {
        if (locked_)
            return QStringLiteral("比赛中不能改");
        const UiProfileLoad loaded = UiProfileRepository::loadFrom(path);
        if (loaded.result == UiProfileLoadResult::Missing)
            return QStringLiteral("文件不存在");
        if (loaded.result == UiProfileLoadResult::Invalid)
            return QStringLiteral("文件读不懂：%1").arg(loaded.error);
        UiProfileBook merged = book_;
        QStringList swapped;
        for (const OperatorProfile& incoming : loaded.book.operators)
        {
            const auto found = std::find_if(merged.operators.begin(), merged.operators.end(),
                                            [&](const OperatorProfile& p)
                                            { return p.name == incoming.name; });
            if (found != merged.operators.end())
            {
                *found = incoming;
                swapped << incoming.name;
            }
            else
            {
                merged.operators.push_back(incoming);
            }
        }
        if (merged.operators.size() > kMaxOperators)
            return QStringLiteral("导入后超过 %1 个操作手，先删掉几个再导入").arg(kMaxOperators);
        // 只导入了一个人时直接切过去:导入通常就是「把我的方案拷到这台电脑」。
        if (loaded.book.operators.size() == 1)
            merged.last_operator = loaded.book.operators.front().name;
        normalize(&merged);
        book_ = std::move(merged);
        if (replaced)
            *replaced = swapped;
        StructuredLog::write(LogLevel::info, QStringLiteral("ui_profile"),
                             {QStringLiteral("state=imported"), QStringLiteral("file=%1").arg(path),
                              QStringLiteral("count=%1").arg(loaded.book.operators.size())});
        commit();
        return {};
    }

    void UiProfileController::setLocked(bool locked)
    {
        if (locked_ == locked)
            return;
        locked_ = locked;
        Q_EMIT changed();
    }

} // namespace rm_terminal
