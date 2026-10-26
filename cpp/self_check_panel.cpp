#include "self_check_panel.h"

#include "theme.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
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
    QColor background = theme::kOffline;
    if (level == CheckLevel::Ok) background = theme::kOk;
    if (level == CheckLevel::Warn) background = theme::kWarn;
    if (level == CheckLevel::Fail) background = theme::kDanger;
    return QStringLiteral("background:%1; color:#f4f6fa; font-weight:bold; padding:2px 8px;"
                          " border-radius:3px;")
        .arg(background.name());
}

}  // namespace

SelfCheckPanel::SelfCheckPanel(QWidget* parent) : QFrame(parent) {
    setObjectName(QStringLiteral("selfCheckPanel"));
    setStyleSheet(QStringLiteral(
        "QFrame#selfCheckPanel{background:#0e121a; border:1px solid #3a4e6c; border-radius:6px;}"
        "QFrame#selfCheckPanel QLabel{background:transparent; border:none;}"
        "QFrame#selfCheckPanel QPushButton{background:#1b2230; color:#e8e8ee;"
        " border:1px solid #2c3648; border-radius:3px; padding:4px 14px; font-weight:bold;}"));

    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(18, 14, 18, 14);
    column->setSpacing(10);

    auto* header = new QHBoxLayout();
    auto* title = new QLabel(QStringLiteral("上场前自检"), this);
    title->setFont(theme::labelFont(15, true));
    title->setStyleSheet(QStringLiteral("color:#e2e8f0;"));
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
    verdict_->setStyleSheet(QStringLiteral("color:%1;")
                                .arg(check.failures == 0 ? QStringLiteral("#3ecf8e")
                                                         : QStringLiteral("#ff7b7b")));

    while (rows_.size() < check.rows.size()) {
        const int row = static_cast<int>(rows_.size());
        std::array<QLabel*, 3> labels{new QLabel(this), new QLabel(this), new QLabel(this)};
        labels[0]->setAlignment(Qt::AlignCenter);
        labels[0]->setMinimumWidth(56);
        labels[1]->setFont(theme::labelFont(12, true));
        labels[1]->setStyleSheet(QStringLiteral("color:#e2e8f0;"));
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
        QString detail = QStringLiteral("<span style='color:#c8d0dc'>%1</span>")
                             .arg(row.detail.toHtmlEscaped());
        if (!row.hint.isEmpty() && row.level != CheckLevel::Ok)
            detail += QStringLiteral("<span style='color:#94a3b8'> → %1</span>")
                          .arg(row.hint.toHtmlEscaped());
        rows_[index][2]->setText(detail);
    }
    adjustSize();
}

}  // namespace rm_terminal
