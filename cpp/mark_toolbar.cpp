#include "mark_toolbar.h"

#include "glass.h"
#include "role_commands.h"
#include "theme.h"

#include <QButtonGroup>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>

#include <algorithm>
#include <vector>

namespace rm_terminal {

namespace {

// 发送范围:协议的 is_send_all 只有 0 指定 / 1 除哨兵 / 2 含哨兵;「只发某台」走「指定」。
// 默认是「除哨兵」:转发给哨兵要单独开那颗开关,免得每点一下地图都悄悄花 50 金。
struct ScopeChoice {
    const char* label;
    MarkScope scope;
    std::uint32_t only;  // 指定时的那台车(本方编号,不含 +100)
};
constexpr ScopeChoice kScopes[] = {
    {"全体", MarkScope::ExceptSentry, 0},
    {"重装", MarkScope::Specific, 1},
    {"3 号", MarkScope::Specific, 3},
    {"4 号", MarkScope::Specific, 4},
};
constexpr std::uint32_t kSentrySeat = 7;

const QString kHintLinkDown = QStringLiteral("断线，标记不会发送");

std::vector<char> letters() {
    std::vector<char> list;
    for (char c = 'A'; c <= 'Z'; ++c)
        if (valid_mark_letter(c)) list.push_back(c);
    return list;
}

GlassButton* chip(QWidget* parent, const QString& text, QHBoxLayout* row, int height) {
    auto* button = new GlassButton(text, parent);
    button->setCheckable(true);
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::NoFocus);
    button->setMinimumHeight(height);
    // 窄窗口里一起收一点,而不是让工具条顶宽地图那一列(1280 时地图卡片只有约 550)。
    button->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    row->addWidget(button);
    return button;
}

}  // namespace

MarkToolbar::MarkToolbar(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("markToolbar"));
    // 一行装下类型、范围和哨兵开关:挤成两行会把 1280 宽下的地图列撑宽,窗口就放不下了。
    // 状态只在出事时才占第二行(setStatus 把空字符串当隐藏)。
    constexpr int kChipHeight = 26;
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(0, 4, 0, 0);
    column->setSpacing(3);
    auto* row = new QHBoxLayout();
    row->setSpacing(4);
    column->addLayout(row);

    kinds_ = new QButtonGroup(this);
    kinds_->setExclusive(true);
    const struct { const char* text; MarkKind kind; } kinds[] = {
        {"攻击", MarkKind::Attack}, {"防御", MarkKind::Defend},
        {"警戒", MarkKind::Alert},  {"自定义", MarkKind::Custom}};
    for (const auto& kind : kinds)
        kinds_->addButton(chip(this, QString::fromUtf8(kind.text), row, kChipHeight),
                          static_cast<int>(kind.kind));
    kinds_->button(static_cast<int>(MarkKind::Attack))->setChecked(true);

    // 自定义字母用一颗按钮点着循环,不用 QComboBox:原生下拉框在玻璃界面里是一块白底方盒。
    letter_ = chip(this, QString(QChar(letters().front())), row, kChipHeight);
    letter_->setObjectName(QStringLiteral("markLetter"));
    letter_->setCheckable(false);
    letter_->setFixedWidth(30);
    letter_->setEnabled(false);
    QObject::connect(letter_, &QPushButton::clicked, this, [this] {
        letter_index_ = (letter_index_ + 1) % static_cast<std::uint32_t>(letters().size());
        letter_->setText(QString(QChar(letters()[letter_index_])));
    });
    QObject::connect(kinds_, &QButtonGroup::idClicked, this, [this](int id) {
        letter_->setEnabled(id == static_cast<int>(MarkKind::Custom));
    });

    // 没有「发给」那两个字:1280 窗口的地图卡片只有约 550 宽,一行里每一个像素都要省。
    // 收件人那几颗挨着类型那几颗,中间多留一点空当就分得开。
    row->addSpacing(8);

    scopes_ = new QButtonGroup(this);
    scopes_->setExclusive(true);
    for (int i = 0; i < static_cast<int>(std::size(kScopes)); ++i)
        scopes_->addButton(chip(this, QString::fromUtf8(kScopes[i].label), row, kChipHeight), i);
    scopes_->button(0)->setChecked(true);
    QObject::connect(scopes_, &QButtonGroup::idClicked, this, [this](int) {
        // 换收件人不该顺手取消哨兵开关,但武装中的二次确认要作废。
        if (arm_sentry_) {
            arm_sentry_ = false;
            dressSentry();
        }
    });

    row->addSpacing(6);
    sentry_ = chip(this, QString(), row, kChipHeight);
    sentry_->setObjectName(QStringLiteral("markSentry"));
    // 宽度只够最长的那条短文字(「含哨兵 50金」),整条工具条才塞得进 1280 窗口的地图
    // 卡片;开关在开/关/待确认之间切换时右边一排也不会跟着跳。随字号在 resizeEvent 里重算。
    QObject::connect(sentry_, &QPushButton::clicked, this, [this] { clickSentry(); });
    row->addStretch(1);

    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("markStatus"));
    status_->setFont(theme::labelPx(12, QFont::DemiBold));
    // 状态文字长短不定,不许它反过来撑宽工具条。
    status_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    column->addWidget(status_);

    dressSentry();
    setLinkUp(false);
}

// 牌子高度跟着地图卡片的宽度走:clamp(宽/30, 26, 44),字是牌子高的 50%。
// 2560 的地图有 1250 宽,牌子就从 26 长到 41,不再是一排 13 像素的小字。
void MarkToolbar::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    // 牌子一共九颗,外加开关那颗要塞「含哨兵 50金」。字号先按宽度 /30 试,但整行
    // 排不下时按「这一行还剩多少宽」往回收 —— 1280 的地图只有约 530 宽,宁可字小一点
    // 也不能让最后那颗被卡片边裁掉。
    constexpr int kChips = 9;
    const int budget = std::max(width() / (kChips + 3), 9);
    const int height = std::clamp(width() / 24, 26, 44);
    const int px = std::clamp(static_cast<int>(std::lround(height * 0.50)), theme::kMinLabelPx,
                              std::max(theme::kMinLabelPx, budget));
    if (height == chip_height_ && px == chip_px_) return;
    chip_height_ = height;
    chip_px_ = px;
    for (QPushButton* button : findChildren<QPushButton*>()) {
        button->setMinimumHeight(height);
        static_cast<GlassButton*>(button)->setTextPx(px);
    }
    status_->setFont(theme::labelPx(std::max(theme::kMinLabelPx, px - 1), QFont::DemiBold));
    // 其余九颗牌子各有 48 的硬下限(GlassButton::minimumSizeHint),开关这一颗是唯一
    // 还能让的:它的宽度封在「这一行减掉那九颗」之内,整条才不会超过地图卡片。
    const int others = 48 * 9 + 4 * 10 + 8;
    const int wanted = QFontMetrics(theme::labelPx(px, QFont::Bold))
                           .horizontalAdvance(QStringLiteral("含哨兵 50金")) +
                       px * 2 + 8;
    sentry_->setFixedWidth(std::clamp(width() - others, 48, wanted));
}

void MarkToolbar::setSentryAutomatic(bool automatic) {
    if (automatic_ == automatic) return;
    automatic_ = automatic;
    dressSentry();
}

// 开关的三种样子:关(只描边)、待确认(点缀色 + 报价)、开(点缀色实心 + 单价)。
void MarkToolbar::dressSentry() {
    const std::uint32_t price = sentry_mark_cost(automatic_);
    if (arm_sentry_) {
        sentry_->setText(price > 0 ? QStringLiteral("确认 %1金").arg(price)
                                   : QStringLiteral("确认开启"));
        sentry_->setToolTip(QStringLiteral("再点一次开启，之后每条标记都转发给哨兵"));
    } else if (forward_sentry_) {
        sentry_->setText(price > 0 ? QStringLiteral("含哨兵 %1金").arg(price)
                                   : QStringLiteral("含哨兵"));
        sentry_->setToolTip(QStringLiteral("点一下关闭转发"));
    } else {
        sentry_->setText(QStringLiteral("含哨兵"));
        sentry_->setToolTip(price > 0 ? QStringLiteral("开启后每条标记花 %1 金币").arg(price)
                                     : QStringLiteral("半自动哨兵，转发不花钱"));
    }
    sentry_->setChecked(forward_sentry_ || arm_sentry_);
    auto* glass = static_cast<GlassButton*>(sentry_);
    glass->setKind(arm_sentry_ ? GlassButton::Kind::Accent : GlassButton::Kind::Command);
    glass->setGlow(arm_sentry_);
    glass->setDimTail(forward_sentry_ && !arm_sentry_ && price > 0);
    glass->setDot(forward_sentry_ && !arm_sentry_ ? theme::kOk : QColor(Qt::transparent));
}

void MarkToolbar::clickSentry() {
    if (forward_sentry_) {
        // 关掉不用确认:少花钱的方向不该拦着操作手。
        forward_sentry_ = false;
        arm_sentry_ = false;
    } else if (arm_sentry_) {
        forward_sentry_ = true;
        arm_sentry_ = false;
    } else {
        arm_sentry_ = true;
    }
    dressSentry();
}

MarkDraft MarkToolbar::draft(std::uint32_t own_faction) const {
    MarkDraft draft;
    draft.kind = static_cast<MarkKind>(kinds_->checkedId());
    if (draft.kind == MarkKind::Custom)
        draft.ascii = static_cast<std::uint32_t>(letters()[letter_index_]);
    const ScopeChoice& choice = kScopes[std::max(0, scopes_->checkedId())];
    const std::uint32_t offset = own_faction == 2 ? 100u : 0u;
    if (choice.scope == MarkScope::Specific) {
        // 指定某台车时,哨兵转发只能靠把哨兵也写进收件人列表。
        draft.scope = MarkScope::Specific;
        draft.recipients[0] = choice.only + offset;
        if (forward_sentry_) draft.recipients[1] = kSentrySeat + offset;
    } else {
        draft.scope = forward_sentry_ ? MarkScope::IncludeSentry : MarkScope::ExceptSentry;
    }
    return draft;
}

void MarkToolbar::setStatus(const QString& text, bool error) {
    status_->setText(text);
    status_->setStyleSheet(
        QStringLiteral("color:%1;")
            .arg(theme::css(error ? theme::kDangerText : theme::kTextSecondary)));
    status_->setVisible(!text.isEmpty());
}

void MarkToolbar::setLinkUp(bool up) {
    if (up == link_up_ && !status_->text().isEmpty()) return;
    link_up_ = up;
    if (!up) setStatus(kHintLinkDown, true);
    else if (status_->text() == kHintLinkDown) setStatus(QString(), false);
}

QString MarkToolbar::statusText() const { return status_->text(); }

}  // namespace rm_terminal
