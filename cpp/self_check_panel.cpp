#include "self_check_panel.h"

#include "theme.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>

namespace rm_terminal {
namespace {

QString chip_text(CheckLevel level) {
    switch (level) {
    case CheckLevel::Ok: return QStringLiteral("正常");
    case CheckLevel::Warn: return QStringLiteral("注意");
    case CheckLevel::Fail: return QStringLiteral("异常");
    case CheckLevel::Off: return QStringLiteral("未启用");
    }
    return QString();
}

QString chip_style(CheckLevel level) {
    QColor tone = theme::kOffline;
    QColor ink = theme::kTextBright;
    if (level == CheckLevel::Ok) { tone = theme::kOk; ink = theme::kOkText; }
    if (level == CheckLevel::Warn) { tone = theme::kWarn; ink = theme::kWarn; }
    if (level == CheckLevel::Fail) { tone = theme::kDanger; ink = theme::kDangerText; }
    tone.setAlpha(64);
    return QStringLiteral("background:%1; color:%2; font-weight:bold; padding:2px 8px;")
        .arg(theme::css(tone), theme::css(ink));
}

}  // namespace

SelfCheckPanel::SelfCheckPanel(QWidget* parent) : QFrame(parent) {
    setObjectName(QStringLiteral("selfCheckPanel"));
    // 底和边框由 paintEvent 画成切角玻璃,样式表只管里面的字和按钮。
    setStyleSheet(QStringLiteral("QFrame#selfCheckPanel{background:transparent; border:none;}"
                                 "QFrame#selfCheckPanel QLabel{background:transparent; border:none;}"
                                 "QFrame#selfCheckPanel QPushButton{background:%1; color:%2;"
                                 " border:1px solid %3; padding:4px 14px; font-weight:bold;}")
                      .arg(theme::css(theme::kSurfaceRaised), theme::css(theme::kTextPrimary),
                           theme::css(theme::kBorder)));

    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(18, 14, 18, 14);
    column->setSpacing(10);

    auto* header = new QHBoxLayout();
    auto* title = new QLabel(QStringLiteral("上场前自检"), this);
    title->setFont(theme::labelFont(15, true));
    title->setStyleSheet(QStringLiteral("color:%1;").arg(theme::css(theme::kTextPrimary)));
    verdict_ = new QLabel(this);
    verdict_->setObjectName(QStringLiteral("selfCheckVerdict"));
    verdict_->setFont(theme::labelFont(15, true));
    close_ = new QPushButton(QStringLiteral("收起"), this);
    close_->setObjectName(QStringLiteral("selfCheckClose"));
    header->addWidget(title);
    header->addSpacing(16);
    header->addWidget(verdict_, 1);
    header->addWidget(close_);
    column->addLayout(header);

    grid_ = new QGridLayout();
    grid_->setHorizontalSpacing(14);
    grid_->setVerticalSpacing(8);
    grid_->setColumnStretch(2, 1);
    column->addLayout(grid_);

    connect(close_, &QPushButton::clicked, this, &SelfCheckPanel::closeRequested);
}

void SelfCheckPanel::setCheck(const SelfCheck& check) {
    verdict_->setText(check.verdict);
    verdict_->setStyleSheet(QStringLiteral("color:%1;").arg(
        theme::css(check.failures == 0 ? theme::kOkText : theme::kDangerText)));

    while (rows_.size() < check.rows.size()) {
        const int row = static_cast<int>(rows_.size());
        std::array<QLabel*, 3> labels{new QLabel(this), new QLabel(this), new QLabel(this)};
        labels[0]->setAlignment(Qt::AlignCenter);
        labels[0]->setMinimumWidth(56);
        labels[1]->setFont(theme::labelFont(12, true));
        labels[1]->setStyleSheet(QStringLiteral("color:%1;").arg(theme::css(theme::kTextPrimary)));
        labels[2]->setWordWrap(true);
        labels[2]->setTextFormat(Qt::RichText);
        labels[2]->setFont(theme::labelFont(12));
        for (int column = 0; column < 3; ++column)
            grid_->addWidget(labels[static_cast<std::size_t>(column)], row, column,
                             column == 2 ? Qt::AlignVCenter : Qt::AlignVCenter | Qt::AlignLeft);
        rows_.push_back(labels);
    }

    for (std::size_t index = 0; index < rows_.size(); ++index) {
        const bool used = index < check.rows.size();
        for (QLabel* label : rows_[index]) label->setVisible(used);
        if (!used) continue;
        const CheckRow& row = check.rows[index];
        rows_[index][0]->setText(chip_text(row.level));
        rows_[index][0]->setStyleSheet(chip_style(row.level));
        rows_[index][1]->setText(row.name);
        QString detail = QStringLiteral("<span style='color:%1'>%2</span>")
                             .arg(theme::kTextBright.name(), row.detail.toHtmlEscaped());
        if (!row.hint.isEmpty() && row.level != CheckLevel::Ok)
            detail += QStringLiteral("<span style='color:%1'> → %2</span>")
                          .arg(theme::kTextSecondary.name(), row.hint.toHtmlEscaped());
        rows_[index][2]->setText(detail);
    }
    adjustSize();
}

void SelfCheckPanel::paintEvent(QPaintEvent* event) {
    QPainter painter(this);
    theme::paintGlass(painter, QRectF(rect()), 10, false, theme::kOverlay, theme::kBorderBright, true);
    QFrame::paintEvent(event);
}

}  // namespace rm_terminal
