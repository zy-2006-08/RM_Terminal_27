#include "purchase_popups.h"

#include "glass.h"
#include "theme.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

namespace rm_terminal
{

    namespace
    {

        constexpr int kPad = 16;
        constexpr MonotonicMs kHintMs = 2000;

        constexpr int kConfirmWidth = 400;
        constexpr int kConfirmHeight = 150;

        constexpr int kRemoteWidth = 480;
        constexpr int kTileHeight = 116;
        constexpr int kTileGap = 10;
        constexpr int kBarHeight = 38;
        constexpr int kBottomMargin = 24;
        // 重装框上面那一排:「装配难度 [1 一级] … [4 四级]  [Enter 开始]」,下面一到两行写选中那级的收益。
        constexpr int kLevelRowHeight = 34;
        constexpr int kLevelNoteHeight = 36;
        constexpr int kAssemblyHeight = kLevelRowHeight + 4 + kLevelNoteHeight;
        constexpr int kAssemblyLabelWidth = 58;
        constexpr int kLevelCapWidth = 64;
        constexpr int kLevelGap = 6;
        constexpr int kStartCapWidth = 92;

        constexpr int kSupplyWidth = 540;
        constexpr int kSupplyHeight = 254;
        constexpr int kTitleHeight = 44;
        constexpr int kInfoHeight = 76;
        constexpr int kStepWidth = 46;
        constexpr int kQuantityWidth = 84;
        constexpr int kRowHeight = 34;
        constexpr int kStatusHeight = 28;

        GlassButton* make_button(QWidget* parent, const QString& text, const QString& name)
        {
            auto* button = new GlassButton(text, parent);
            button->setObjectName(name);
            return button;
        }

        // 官方选手端的键帽:绿色表示这一项现在能按,灰色表示不能。
        void paint_keycap(QPainter& painter, const QRectF& box, const QString& text, bool lit)
        {
            theme::paintGlass(painter, box, 3, false, lit ? QColor(28, 64, 50, 200) : QColor(255, 255, 255, 8),
                              lit ? theme::kOk : theme::kRowBorder);
            painter.setFont(theme::numericPx(13, QFont::DemiBold));
            painter.setPen(lit ? theme::kOkText : theme::kTextMuted);
            painter.drawText(box, Qt::AlignCenter, text);
        }

        // 补弹面板 8 个按钮从左到右:−100 −50 −20 −10 +10 +20 +50 +100(42mm 是 10 5 2 1)。
        std::uint32_t step_size(Caliber caliber, std::size_t index)
        {
            const auto steps = supply_steps(caliber);
            return index < 4 ? steps[3 - index] : steps[index - 4];
        }

    } // namespace

    PurchaseConfirm::PurchaseConfirm(QWidget* host) : QWidget(host)
    {
        setObjectName(QStringLiteral("purchaseConfirm"));
        auto* column = new QVBoxLayout(this);
        column->setContentsMargins(kPad, kPad, kPad, kPad);
        column->addStretch(1);
        auto* row = new QHBoxLayout;
        row->setSpacing(12);
        yes_ = make_button(this, QString(), QStringLiteral("purchaseYes"));
        no_ = make_button(this, QString(), QStringLiteral("purchaseNo"));
        yes_->setMinimumWidth(120);
        no_->setMinimumWidth(120);
        row->addStretch(1);
        row->addWidget(yes_);
        row->addWidget(no_);
        row->addStretch(1);
        column->addLayout(row);
        QObject::connect(yes_, &QPushButton::clicked, this, [this]
                         {
        if (yes_action_) yes_action_(); });
        QObject::connect(no_, &QPushButton::clicked, this, [this]
                         {
        if (no_action_) no_action_(); });
        setVisible(false);
        setOffer(offer_);
    }

    void PurchaseConfirm::setOffer(const PurchaseOffer& offer)
    {
        offer_ = offer;
        const bool supply = offer.kind == PurchaseKind::Supply;
        yes_->setText(supply ? QStringLiteral("是 [Y]") : QStringLiteral("确定 [Y]"));
        no_->setText(supply ? QStringLiteral("否 [N]") : QStringLiteral("取消 [N]"));
        yes_->setEnabled(offer.available);
        update();
    }

    void PurchaseConfirm::setActions(std::function<void()> yes, std::function<void()> no)
    {
        yes_action_ = std::move(yes);
        no_action_ = std::move(no);
    }

    QString PurchaseConfirm::questionText() const
    {
        return offer_.available ? offer_.question : QStringLiteral("现在买不了：%1").arg(offer_.reason);
    }

    QSize PurchaseConfirm::sizeHint() const { return QSize(kConfirmWidth, kConfirmHeight); }

    void PurchaseConfirm::paintEvent(QPaintEvent*)
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        theme::paintGlass(painter, QRectF(rect()).adjusted(1, 1, -1, -1), 10, false, theme::kOverlay,
                          theme::kBorderBright, true);
        const QRect text_box(kPad, kPad, width() - kPad * 2, std::max(0, yes_->geometry().top() - kPad - 8));
        painter.setFont(theme::labelPx(16, QFont::DemiBold));
        painter.setPen(offer_.available ? theme::kTextPrimary : theme::kDangerText);
        painter.drawText(text_box, Qt::AlignCenter | Qt::TextWordWrap, questionText());
    }

    RemoteExchangePopup::RemoteExchangePopup(QWidget* host) : QWidget(host)
    {
        setObjectName(QStringLiteral("remoteExchangePopup"));
        setCursor(Qt::PointingHandCursor);
        setVisible(false);
    }

    void RemoteExchangePopup::setOffers(const std::array<PurchaseOffer, 3>& offers)
    {
        offers_ = offers;
        update();
    }

    void RemoteExchangePopup::setOpen(bool open)
    {
        if (open_ == open)
            return;
        open_ = open;
        refresh();
    }

    void RemoteExchangePopup::setStatus(const QString& status, bool problem)
    {
        if (status_ == status && problem_ == problem)
            return;
        status_ = status;
        problem_ = problem;
        refresh();
    }

    void RemoteExchangePopup::setChoose(std::function<void(int index)> choose) { choose_ = std::move(choose); }

    void RemoteExchangePopup::setAssembly(const AssemblyRow& row)
    {
        if (row == assembly_)
            return;
        const bool resized = row.shown != assembly_.shown;
        assembly_ = row;
        if (resized)
            updateGeometry();
        update();
    }

    void RemoteExchangePopup::setAssemblyActions(std::function<void(std::uint32_t level)> choose,
                                                 std::function<void()> start)
    {
        choose_level_ = std::move(choose);
        start_assembly_ = std::move(start);
    }

    bool RemoteExchangePopup::hasItem(int index) const
    {
        if (index < 0 || index > 2)
            return false;
        return !assembly_.shown || index != 1;
    }

    QRect RemoteExchangePopup::levelRect(std::uint32_t level) const
    {
        if (!open_ || !assembly_.shown || level < 1 || level > 4)
            return QRect();
        const int x = kPad + kAssemblyLabelWidth + static_cast<int>(level - 1) * (kLevelCapWidth + kLevelGap);
        return QRect(x, kPad, kLevelCapWidth, kLevelRowHeight);
    }

    QRect RemoteExchangePopup::startRect() const
    {
        if (!open_ || !assembly_.shown)
            return QRect();
        return QRect(width() - kPad - kStartCapWidth, kPad, kStartCapWidth, kLevelRowHeight);
    }

    void RemoteExchangePopup::refresh()
    {
        const bool show = open_ || !status_.isEmpty();
        updateGeometry();
        if (show == isHidden())
            setVisible(show);
        if (show)
            raise();
        update();
    }

    QString RemoteExchangePopup::barText() const
    {
        if (!status_.isEmpty())
            return status_;
        if (!open_)
            return QString();
        if (std::any_of(offers_.begin(), offers_.end(), [](const PurchaseOffer& offer)
                        { return offer.available; }))
            return assembly_.shown ? QStringLiteral("按 H 关闭") : QStringLiteral("按 H 关闭远程兑换面板");
        // 都买不了:先说本车这种弹丸为什么买不了,再说回血。
        const Caliber own = offers_[0].caliber;
        const PurchaseOffer& ammo = own == Caliber::Small ? offers_[1] : offers_[2];
        return ammo.reason.isEmpty() ? offers_[0].reason : ammo.reason;
    }

    QString RemoteExchangePopup::tileText(int index) const
    {
        const PurchaseOffer& offer = offers_[static_cast<std::size_t>(index)];
        if (!offer.available)
            return offer.reason;
        if (offer.kind == PurchaseKind::RemoteHeal)
            return offer.heal > 0 ? QStringLiteral("+%1 血 · %2 金").arg(offer.heal).arg(offer.cost)
                                  : QStringLiteral("60% 血量 · %1 金").arg(offer.cost);
        return QStringLiteral("%1 发 · %2 金").arg(offer.rounds).arg(offer.cost);
    }

    QRect RemoteExchangePopup::tileRect(int index) const
    {
        if (!open_ || !hasItem(index))
            return QRect();
        if (assembly_.shown)
        {
            // 重装:回血、42mm 两格,放在装配那一排下面。
            const int tile_width = (width() - kPad * 2 - kTileGap) / 2;
            const int column = index == 0 ? 0 : 1;
            return QRect(kPad + column * (tile_width + kTileGap), kPad + kAssemblyHeight + kTileGap, tile_width,
                         kTileHeight);
        }
        const int tile_width = (width() - kPad * 2 - kTileGap * 2) / 3;
        return QRect(kPad + index * (tile_width + kTileGap), kPad, tile_width, kTileHeight);
    }

    QSize RemoteExchangePopup::sizeHint() const
    {
        const int assembly = assembly_.shown ? kAssemblyHeight + kTileGap : 0;
        if (open_)
            return QSize(kRemoteWidth, kPad + assembly + kTileHeight + kTileGap + kBarHeight + kPad);
        return QSize(kRemoteWidth, kBarHeight + 16);
    }

    void RemoteExchangePopup::paintEvent(QPaintEvent*)
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        theme::paintGlass(painter, QRectF(rect()).adjusted(1, 1, -1, -1), 10, false, theme::kOverlay,
                          theme::kBorderBright, true);
        if (open_)
        {
            if (assembly_.shown)
                paintAssembly(painter);
            const std::array<QString, 3> titles{QStringLiteral("回血"), QStringLiteral("17mm 弹丸"),
                                                QStringLiteral("42mm 弹丸")};
            const std::array<QString, 3> marks{QStringLiteral("+"), QStringLiteral("17"), QStringLiteral("42")};
            const std::array<QColor, 3> inks{theme::kOkText, theme::kCyan, theme::kWarn};
            for (int index = 0; index < 3; ++index)
            {
                if (!hasItem(index))
                    continue;
                const auto slot = static_cast<std::size_t>(index);
                const QRectF tile = tileRect(index);
                const bool lit = offers_[slot].available;
                theme::paintGlass(painter, tile, 6, false, theme::kTile, lit ? theme::kBorder : theme::kRowBorder);
                const QRectF icon(tile.center().x() - 15, tile.top() + 8, 30, 30);
                painter.setPen(QPen(lit ? inks[slot] : theme::kTextMuted, 1.6));
                painter.setBrush(Qt::NoBrush);
                painter.drawEllipse(icon);
                painter.setFont(theme::numericPx(index == 0 ? 18 : 13, QFont::Bold));
                painter.drawText(icon, Qt::AlignCenter, marks[slot]);
                painter.setFont(theme::labelPx(14, QFont::DemiBold));
                painter.setPen(lit ? theme::kTextPrimary : theme::kTextMuted);
                painter.drawText(QRectF(tile.left(), tile.top() + 40, tile.width(), 20), Qt::AlignCenter, titles[slot]);
                painter.setFont(theme::labelPx(12));
                painter.setPen(lit ? theme::kTextSecondary : theme::kTextMuted);
                const QRectF detail(tile.left() + 6, tile.top() + 60, tile.width() - 12, 18);
                painter.drawText(detail, Qt::AlignCenter,
                                 painter.fontMetrics().elidedText(tileText(index), Qt::ElideRight,
                                                                  static_cast<int>(detail.width())));
                paint_keycap(painter, QRectF(tile.center().x() - 34, tile.bottom() - 30, 68, 22),
                             QStringLiteral("Ctrl+%1").arg(index + 1), lit);
            }
        }
        const QRectF bar = open_ ? QRectF(kPad, height() - kPad - kBarHeight, width() - kPad * 2, kBarHeight)
                                 : QRectF(8, 8, width() - 16, height() - 16);
        theme::paintGlass(painter, bar, 6, false, theme::kChip, theme::kRowBorder);
        painter.setFont(theme::labelPx(14, QFont::DemiBold));
        painter.setPen(problem_ ? theme::kWarn : theme::kTextPrimary);
        painter.drawText(bar, Qt::AlignCenter,
                         painter.fontMetrics().elidedText(barText(), Qt::ElideRight, static_cast<int>(bar.width()) - 16));
    }

    void RemoteExchangePopup::paintAssembly(QPainter& painter)
    {
        painter.setFont(theme::labelPx(13, QFont::DemiBold));
        painter.setPen(assembly_.blocked ? theme::kTextMuted : theme::kTextPrimary);
        painter.drawText(QRectF(kPad, kPad, kAssemblyLabelWidth, kLevelRowHeight), Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("装配难度"));
        for (std::uint32_t level = 1; level <= 4; ++level)
        {
            const QRectF box = levelRect(level);
            const bool can = assembly_.selectable[level - 1];
            const bool picked = assembly_.selected == level;
            theme::paintGlass(painter, box, 3, false,
                              picked ? theme::kAccentBg : (can ? theme::kChip : QColor(255, 255, 255, 8)),
                              picked ? theme::kCyan : (can ? theme::kBorder : theme::kRowBorder));
            painter.setFont(theme::labelPx(13, picked ? QFont::Bold : QFont::DemiBold));
            painter.setPen(picked ? theme::kAccentText : (can ? theme::kTextPrimary : theme::kTextMuted));
            painter.drawText(box, Qt::AlignCenter, QStringLiteral("%1 %2").arg(level).arg(assembly_level_name(level)));
        }
        paint_keycap(painter, startRect(), QStringLiteral("Enter 开始"), !assembly_.blocked && assembly_.selected);
        const QRectF note(kPad, kPad + kLevelRowHeight + 4, width() - kPad * 2, kLevelNoteHeight);
        painter.setFont(theme::labelPx(12));
        painter.setPen(assembly_.blocked ? theme::kWarn : theme::kTextSecondary);
        painter.drawText(note, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap, assembly_.note);
    }

    void RemoteExchangePopup::mouseReleaseEvent(QMouseEvent* event)
    {
        if (!open_ || event->button() != Qt::LeftButton)
            return;
        const QPoint at = event->position().toPoint();
        if (assembly_.shown)
        {
            for (std::uint32_t level = 1; level <= 4; ++level)
            {
                if (levelRect(level).contains(at))
                {
                    if (choose_level_)
                        choose_level_(level);
                    return;
                }
            }
            if (startRect().contains(at))
            {
                if (start_assembly_)
                    start_assembly_();
                return;
            }
        }
        if (!choose_)
            return;
        for (int index = 0; index < 3; ++index)
        {
            if (tileRect(index).contains(at))
            {
                choose_(index);
                return;
            }
        }
    }

    SupplyPanel::SupplyPanel(QWidget* host) : QWidget(host)
    {
        setObjectName(QStringLiteral("supplyPanel"));
        auto* column = new QVBoxLayout(this);
        column->setContentsMargins(20, kTitleHeight + kInfoHeight + 14, 20, kStatusHeight + 8);
        column->setSpacing(16);
        auto* row = new QHBoxLayout;
        row->setSpacing(4);
        row->addStretch(1);
        for (std::size_t index = 0; index < steps_.size(); ++index)
        {
            if (index == 4)
            {
                quantity_box_ = new QWidget(this);
                quantity_box_->setFixedSize(kQuantityWidth, kRowHeight);
                row->addWidget(quantity_box_);
            }
            auto* button = make_button(this, QString(), QStringLiteral("supplyStep%1").arg(index));
            button->setFixedWidth(kStepWidth);
            QObject::connect(button, &QPushButton::clicked, this, [this, index]
                             { step(index); });
            steps_[index] = button;
            row->addWidget(button);
        }
        row->addStretch(1);
        column->addLayout(row);
        confirm_ = make_button(this, QStringLiteral("确定"), QStringLiteral("supplyConfirm"));
        confirm_->setFixedWidth(140);
        auto* bottom = new QHBoxLayout;
        bottom->addStretch(1);
        bottom->addWidget(confirm_);
        bottom->addStretch(1);
        column->addLayout(bottom);
        QObject::connect(confirm_, &QPushButton::clicked, this, [this]
                         {
        if (confirm_action_) confirm_action_(); });
        setVisible(false);
        setCaliber(Caliber::Small);
    }

    void SupplyPanel::setCaliber(Caliber caliber)
    {
        caliber_ = caliber;
        quantity_ = 0;
        for (std::size_t index = 0; index < steps_.size(); ++index)
            steps_[index]->setText(
                (index < 4 ? QStringLiteral("−%1") : QStringLiteral("+%1")).arg(step_size(caliber, index)));
        refresh();
    }

    void SupplyPanel::setView(const SelfCommandView& view, bool link_up)
    {
        view_ = view;
        link_up_ = link_up;
        refresh();
    }

    void SupplyPanel::setStatus(const QString& status, bool problem)
    {
        if (status_ == status && problem_ == problem)
            return;
        status_ = status;
        problem_ = problem;
        update();
    }

    void SupplyPanel::setConfirm(std::function<void()> confirm) { confirm_action_ = std::move(confirm); }

    PurchaseOffer SupplyPanel::offer() const { return supply_offer(view_, link_up_, caliber_, quantity_); }

    bool SupplyPanel::blocked() const
    {
        return !supply_offer(view_, link_up_, caliber_, supply_unit(caliber_)).available;
    }

    QString SupplyPanel::infoText() const
    {
        const PurchaseOffer least = supply_offer(view_, link_up_, caliber_, supply_unit(caliber_));
        if (!least.available)
            return least.reason;
        return QStringLiteral("%1/%2")
            .arg(view_.ammo ? QString::number(*view_.ammo) : QStringLiteral("--"))
            .arg(supply_max_rounds(view_, caliber_));
    }

    QSize SupplyPanel::sizeHint() const { return QSize(kSupplyWidth, kSupplyHeight); }

    void SupplyPanel::step(std::size_t index)
    {
        const std::uint32_t size = step_size(caliber_, index);
        const std::uint32_t most = supply_max_rounds(view_, caliber_);
        quantity_ = index < 4 ? (quantity_ > size ? quantity_ - size : 0) : std::min(quantity_ + size, most);
        refresh();
    }

    void SupplyPanel::refresh()
    {
        const bool stopped = blocked();
        // 官方选手端:数量最多调到现在的钱能买的发数。
        const std::uint32_t most = stopped ? 0 : supply_max_rounds(view_, caliber_);
        quantity_ = std::min(quantity_, most);
        for (std::size_t index = 0; index < steps_.size(); ++index)
            steps_[index]->setEnabled(!stopped && (index < 4 ? quantity_ > 0 : quantity_ < most));
        confirm_->setEnabled(offer().available);
        update();
    }

    void SupplyPanel::paintEvent(QPaintEvent*)
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        theme::paintGlass(painter, QRectF(rect()).adjusted(1, 1, -1, -1), 10, false, theme::kOverlay,
                          theme::kBorderBright, true);
        const QRectF title(20, 0, width() - 40, kTitleHeight);
        painter.setFont(theme::labelPx(17, QFont::Bold));
        painter.setPen(theme::kTextPrimary);
        painter.drawText(title, Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("%1 弹丸补给面板").arg(caliber_text(caliber_)));
        painter.setFont(theme::labelPx(12));
        painter.setPen(theme::kTextMuted);
        painter.drawText(title, Qt::AlignRight | Qt::AlignVCenter,
                         caliber_ == Caliber::Small ? QStringLiteral("按 O 关闭") : QStringLiteral("按 I 关闭"));

        const QRect quantity = quantity_box_->geometry();
        const QRectF inner(12, kTitleHeight, width() - 24, quantity.bottom() + 8 - kTitleHeight);
        theme::paintGlass(painter, inner, 6, false, theme::kTile, theme::kRowBorder);
        const QString label = QStringLiteral("已有/可兑换允许发弹量");
        painter.setFont(theme::labelPx(12));
        const qreal label_width = painter.fontMetrics().horizontalAdvance(label);
        const QRectF label_box(inner.left(), inner.top() + 8, inner.width(), 18);
        painter.setPen(theme::kTextSecondary);
        painter.drawText(label_box, Qt::AlignCenter, label);
        const qreal mid = label_box.center().y();
        const qreal left = label_box.center().x() - label_width / 2 - 8;
        const qreal right = label_box.center().x() + label_width / 2 + 8;
        painter.setPen(QPen(theme::kBorder, 1));
        painter.drawLine(QPointF(left - 60, mid), QPointF(left, mid));
        painter.drawLine(QPointF(right, mid), QPointF(right + 60, mid));
        const QRectF value(inner.left(), inner.top() + 28, inner.width(), 38);
        if (blocked())
        {
            painter.setFont(theme::labelPx(15, QFont::DemiBold));
            painter.setPen(theme::kDangerText);
            painter.drawText(value, Qt::AlignCenter, infoText());
        }
        else
        {
            painter.setFont(theme::numericPx(28, QFont::Bold));
            painter.setPen(theme::kTextPrimary);
            painter.drawText(value, Qt::AlignCenter, infoText());
        }
        theme::paintGlass(painter, QRectF(quantity), 4, false, QColor(4, 8, 14, 230), theme::kBorder);
        painter.setFont(theme::numericPx(20, QFont::Bold));
        painter.setPen(blocked() ? theme::kTextMuted : theme::kTextPrimary);
        painter.drawText(QRectF(quantity), Qt::AlignCenter, QString::number(quantity_));

        if (!status_.isEmpty())
        {
            const QRectF status(20, height() - kStatusHeight - 6, width() - 40, kStatusHeight);
            painter.setFont(theme::labelPx(13, QFont::DemiBold));
            painter.setPen(problem_ ? theme::kWarn : theme::kTextSecondary);
            painter.drawText(status, Qt::AlignCenter,
                             painter.fontMetrics().elidedText(status_, Qt::ElideRight, static_cast<int>(status.width())));
        }
    }

    PurchaseFlow::PurchaseFlow(QWidget* host)
        : QObject(host),
          host_(host),
          confirm_(new PurchaseConfirm(host)),
          remote_(new RemoteExchangePopup(host)),
          supply_(new SupplyPanel(host))
    {
        remote_->setChoose([this](int index)
                           { chooseRemote(index); });
        supply_->setConfirm([this]
                            {
        const PurchaseOffer offer = supply_->offer();
        if (offer.available) open(offer, Source::Supply); });
        confirm_->setActions([this]
                             { yes(); }, [this]
                             { no(); });
        host->installEventFilter(this);
        refresh();
    }

    void PurchaseFlow::setSender(Sender sender) { sender_ = std::move(sender); }

    void PurchaseFlow::update(const SelfCommandView& view, bool link_up, MonotonicMs now)
    {
        view_ = view;
        link_up_ = link_up;
        now_ = now;
        watch_.update(view, now);
        if (view.dead.value_or(false))
        {
            setSupplyOpen(false);
            confirm_->hide();
        }
        refresh();
    }

    void PurchaseFlow::toggleRemote()
    {
        if (remote_->isOpen())
        {
            setRemoteOpen(false);
            if (source_ == Source::Remote)
                confirm_->hide();
        }
        else
        {
            setSupplyOpen(false);
            if (source_ == Source::Supply)
                confirm_->hide();
            setRemoteOpen(true);
        }
        refresh();
    }

    void PurchaseFlow::setAssemblyRow(const AssemblyRow& row)
    {
        remote_->setAssembly(row);
        place();
    }

    void PurchaseFlow::setAssemblyActions(std::function<void(std::uint32_t level)> choose, std::function<void()> start)
    {
        remote_->setAssemblyActions(std::move(choose), std::move(start));
    }

    void PurchaseFlow::toggleSupply(Caliber caliber)
    {
        if (supplyOpen() && supply_->caliber() == caliber)
        {
            setSupplyOpen(false);
            if (source_ == Source::Supply)
                confirm_->hide();
        }
        else if (view_.dead.value_or(false))
        {
            flash(QStringLiteral("阵亡，复活后才能兑换"));
        }
        else
        {
            setRemoteOpen(false);
            if (source_ != Source::Pane)
                confirm_->hide();
            supply_->setCaliber(caliber);
            setSupplyOpen(true);
        }
        refresh();
    }

    bool PurchaseFlow::chooseRemote(int index)
    {
        if (!remote_->isOpen() || confirmOpen() || !remote_->hasItem(index))
            return false;
        const PurchaseOffer offer = remote_->offers()[static_cast<std::size_t>(index)];
        if (!offer.available)
            return false;
        open(offer, Source::Remote);
        return true;
    }

    bool PurchaseFlow::yes()
    {
        if (!confirmOpen())
            return false;
        const PurchaseOffer& asked = confirm_->offer();
        const PurchaseOffer offer = purchase_offer(asked.kind, asked.caliber, asked.rounds, view_, link_up_);
        confirm_->setOffer(offer);
        // 确认框开着时 Y 只给它:买不了或没发出去时框留着,不落到复活上。
        if (!offer.available || !sender_ || !sender_(offer))
            return true;
        watch_.sent(offer, view_, now_);
        confirm_->hide();
        refresh();
        return true;
    }

    bool PurchaseFlow::no()
    {
        if (!confirmOpen())
            return false;
        confirm_->hide();
        refresh();
        return true;
    }

    void PurchaseFlow::ask(PurchaseKind kind, std::uint32_t rounds)
    {
        const PurchaseOffer offer = purchase_offer(kind, own_caliber(view_), rounds, view_, link_up_);
        if (offer.available)
            open(offer, Source::Pane);
    }

    void PurchaseFlow::closeAll()
    {
        setRemoteOpen(false);
        setSupplyOpen(false);
        confirm_->hide();
        refresh();
    }

    void PurchaseFlow::raiseOpen()
    {
        if (!remote_->isHidden())
            remote_->raise();
        if (supplyOpen())
            supply_->raise();
        if (confirmOpen())
            confirm_->raise();
    }

    bool PurchaseFlow::remoteOpen() const { return remote_->isOpen(); }

    bool PurchaseFlow::supplyOpen() const { return !supply_->isHidden(); }

    bool PurchaseFlow::confirmOpen() const { return !confirm_->isHidden(); }

    QString PurchaseFlow::statusText() const { return watch_.text(now_); }

    bool PurchaseFlow::statusProblem() const { return watch_.problem(); }

    bool PurchaseFlow::eventFilter(QObject* watched, QEvent* event)
    {
        if (watched == host_ && event->type() == QEvent::Resize)
            place();
        return false;
    }

    void PurchaseFlow::open(const PurchaseOffer& offer, Source source)
    {
        source_ = source;
        confirm_->setOffer(offer);
        confirm_->show();
        place();
        confirm_->raise();
    }

    void PurchaseFlow::setRemoteOpen(bool open)
    {
        if (remote_->isOpen() == open)
            return;
        remote_->setOpen(open);
        Q_EMIT remoteOpenChanged(open);
    }

    void PurchaseFlow::setSupplyOpen(bool open)
    {
        if (supplyOpen() == open)
            return;
        supply_->setVisible(open);
        if (open)
            supply_->raise();
        Q_EMIT supplyOpenChanged(open);
    }

    void PurchaseFlow::flash(const QString& text)
    {
        hint_ = text;
        hint_until_ = now_ + kHintMs;
    }

    void PurchaseFlow::refresh()
    {
        remote_->setOffers({remote_heal_offer(view_, link_up_), remote_ammo_offer(view_, link_up_, Caliber::Small),
                            remote_ammo_offer(view_, link_up_, Caliber::Large)});
        supply_->setView(view_, link_up_);
        if (confirmOpen())
        {
            const PurchaseOffer& asked = confirm_->offer();
            confirm_->setOffer(purchase_offer(asked.kind, asked.caliber, asked.rounds, view_, link_up_));
        }
        const QString result = watch_.text(now_);
        const bool supply_result = watch_.kind() == PurchaseKind::Supply;
        supply_->setStatus(supply_result ? result : QString(), watch_.problem());
        if (now_ < hint_until_)
        {
            remote_->setStatus(hint_, true);
        }
        else
        {
            // 补弹面板开着时它自己那一行写结果,底下那条不重复。
            remote_->setStatus(supplyOpen() && supply_result ? QString() : result, watch_.problem());
        }
        place();
    }

    void PurchaseFlow::place()
    {
        const QRect area = host_->rect();
        const QSize remote = remote_->sizeHint();
        remote_->setGeometry((area.width() - remote.width()) / 2,
                             std::max(0, area.height() - remote.height() - kBottomMargin), remote.width(),
                             remote.height());
        const QSize supply = supply_->sizeHint();
        supply_->setGeometry((area.width() - supply.width()) / 2, std::max(0, (area.height() - supply.height()) / 2),
                             supply.width(), supply.height());
        const QRect anchor = source_ == Source::Remote   ? remote_->geometry()
                             : source_ == Source::Supply ? supply_->geometry()
                                                         : area;
        QRect box(QPoint(), confirm_->sizeHint());
        box.moveCenter(anchor.center());
        box.moveLeft(std::clamp(box.left(), 0, std::max(0, area.width() - box.width())));
        box.moveTop(std::clamp(box.top(), 0, std::max(0, area.height() - box.height())));
        confirm_->setGeometry(box);
    }

} // namespace rm_terminal
