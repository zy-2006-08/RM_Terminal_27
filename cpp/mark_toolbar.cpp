#include "mark_toolbar.h"

#include "theme.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>

namespace rm_terminal {

namespace {

// 发送范围:协议的 is_send_all 只有 0 指定 / 1 除哨兵 / 2 含哨兵;「只发某台」走「指定」。
struct ScopeChoice {
    const char* label;
    MarkScope scope;
    std::uint32_t only;  // 指定时的那台车(本方编号,不含 +100)
};
constexpr ScopeChoice kScopes[] = {
    {"发给全体（含哨兵）", MarkScope::IncludeSentry, 0},
    {"发给全体（除哨兵）", MarkScope::ExceptSentry, 0},
    {"只发哨兵", MarkScope::Specific, 7},
    {"只发重装", MarkScope::Specific, 1},
    {"只发 3 号步兵", MarkScope::Specific, 3},
    {"只发 4 号步兵", MarkScope::Specific, 4},
};

}  // namespace

MarkToolbar::MarkToolbar(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("markToolbar"));
    // 两行:选项一行,状态一行。挤在一行会把 1280 宽下的地图列撑宽,窗口就放不下了。
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(0, 2, 0, 0);
    column->setSpacing(2);
    auto* row = new QHBoxLayout();
    row->setSpacing(6);
    column->addLayout(row);

    auto* title = new QLabel(QStringLiteral("标记"), this);
    title->setFont(theme::labelFont(11, true));
    row->addWidget(title);

    kinds_ = new QButtonGroup(this);
    kinds_->setExclusive(true);
    const struct { const char* text; MarkKind kind; } kinds[] = {
        {"攻击", MarkKind::Attack}, {"防御", MarkKind::Defend},
        {"警戒", MarkKind::Alert},  {"自定义", MarkKind::Custom}};
    for (const auto& kind : kinds) {
        auto* button = new QPushButton(QString::fromUtf8(kind.text), this);
        button->setCheckable(true);
        button->setMinimumHeight(30);
        button->setFont(theme::labelFont(11, true));
        kinds_->addButton(button, static_cast<int>(kind.kind));
        row->addWidget(button);
    }
    kinds_->button(static_cast<int>(MarkKind::Attack))->setChecked(true);

    letter_ = new QComboBox(this);
    letter_->setObjectName(QStringLiteral("markLetter"));
    for (char c = 'A'; c <= 'Z'; ++c)
        if (valid_mark_letter(c)) letter_->addItem(QString(QChar(c)), static_cast<int>(c));
    letter_->setEnabled(false);
    letter_->setMinimumHeight(30);
    row->addWidget(letter_);
    QObject::connect(kinds_, &QButtonGroup::idClicked, this, [this](int id) {
        letter_->setEnabled(id == static_cast<int>(MarkKind::Custom));
    });

    scope_ = new QComboBox(this);
    scope_->setObjectName(QStringLiteral("markScope"));
    for (const ScopeChoice& choice : kScopes) scope_->addItem(QString::fromUtf8(choice.label));
    scope_->setMinimumHeight(30);
    row->addWidget(scope_);

    row->addStretch(1);

    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("markStatus"));
    status_->setFont(theme::labelFont(10));
    // 状态文字长短不定,不许它反过来撑宽工具条。
    status_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    column->addWidget(status_);
    setLinkUp(false);
}

MarkDraft MarkToolbar::draft(std::uint32_t own_faction) const {
    MarkDraft draft;
    draft.kind = static_cast<MarkKind>(kinds_->checkedId());
    if (draft.kind == MarkKind::Custom) draft.ascii = static_cast<std::uint32_t>(letter_->currentData().toInt());
    const ScopeChoice& choice = kScopes[std::max(0, scope_->currentIndex())];
    draft.scope = choice.scope;
    if (choice.scope == MarkScope::Specific)
        draft.recipients[0] = own_faction == 2 ? choice.only + 100 : choice.only;
    return draft;
}

void MarkToolbar::setStatus(const QString& text, bool error) {
    status_->setText(text);
    status_->setStyleSheet(error ? QStringLiteral("color:%1;").arg(theme::kDanger.name())
                                 : QStringLiteral("color:%1;").arg(theme::kTextSecondary.name()));
}

void MarkToolbar::setLinkUp(bool up) {
    if (up == link_up_ && !status_->text().isEmpty()) return;
    link_up_ = up;
    if (!up) setStatus(QStringLiteral("断线，标记不会发送"), true);
    else if (status_->text() == QStringLiteral("断线，标记不会发送") || status_->text().isEmpty())
        setStatus(QStringLiteral("点空地标位置，点敌方车标目标（每次干预哨兵花 50 金币）"), false);
}

QString MarkToolbar::statusText() const { return status_->text(); }

}  // namespace rm_terminal
