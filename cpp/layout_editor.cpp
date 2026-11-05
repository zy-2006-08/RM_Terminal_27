#include "layout_editor.h"

#include "glass.h"
#include "pane_details.h"
#include "theme.h"
#include "ui_profile_store.h"
#include "unit_grid.h"

#include <QCursor>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLinearGradient>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QHideEvent>
#include <QShortcut>
#include <QWheelEvent>
#include <algorithm>
#include <functional>
#include <cmath>

namespace rm_terminal {

namespace {

constexpr int kHeaderHeight = 60;
constexpr int kStripHeight = 58;
constexpr int kBodyTop = kHeaderHeight + kStripHeight + 12;
constexpr int kLibraryWidth = 248;
constexpr int kToastRow = 52;
constexpr int kTool = 24;
constexpr int kRealSpacing = 8;
constexpr int kTileCaption = 26;
constexpr int kTileGap = 16;
constexpr int kTileMaxHeight = 150;
constexpr int kLibraryTop = 48;
constexpr int kMaxUndo = 50;
constexpr int kTipsWidth = 16 + 168 * 2 + 16;

QString library_title(PaneId pane, bool video_embeddable) {
    switch (pane) {
    case PaneId::SelfCommands: return video_embeddable ? QStringLiteral("本车（指令 + 图传）") : QStringLiteral("本车");
    case PaneId::Video: return QStringLiteral("图传");
    case PaneId::NextStep: return QStringLiteral("下一步（三件事）");
    default: break;
    }
    return pane_title(pane);
}

void dashed(QPainter& painter, const QRectF& box, const QColor& colour, qreal width) {
    QPen pen(colour, width, Qt::DashLine);
    pen.setDashPattern({4, 3});
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(box);
}

void paint_gear(QPainter& painter, const QPointF& c, const QColor& ink) {
    painter.setPen(QPen(ink, 1.3));
    for (int i = -1; i <= 1; ++i) painter.drawLine(c + QPointF(-6, i * 3.5), c + QPointF(6, i * 3.5));
    painter.fillRect(QRectF(c.x() + 1, c.y() - 5, 3, 3), ink);
    painter.fillRect(QRectF(c.x() - 4, c.y() - 1.5, 3, 3), ink);
    painter.fillRect(QRectF(c.x() + 2, c.y() + 2, 3, 3), ink);
}

void paint_cross(QPainter& painter, const QPointF& c, const QColor& ink) {
    painter.setPen(QPen(ink, 1.6));
    painter.drawLine(c + QPointF(-4, -4), c + QPointF(4, 4));
    painter.drawLine(c + QPointF(4, -4), c + QPointF(-4, 4));
}

void paint_arrows(QPainter& painter, const QPointF& c, bool horizontal, const QColor& ink) {
    painter.setPen(QPen(ink, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    QPainterPath path;
    if (horizontal) {
        path.moveTo(c + QPointF(-4, -5));
        path.lineTo(c + QPointF(-8, 0));
        path.lineTo(c + QPointF(-4, 5));
        path.moveTo(c + QPointF(4, -5));
        path.lineTo(c + QPointF(8, 0));
        path.lineTo(c + QPointF(4, 5));
        path.moveTo(c + QPointF(-7.5, 0));
        path.lineTo(c + QPointF(7.5, 0));
    } else {
        path.moveTo(c + QPointF(-5, -4));
        path.lineTo(c + QPointF(0, -8));
        path.lineTo(c + QPointF(5, -4));
        path.moveTo(c + QPointF(-5, 4));
        path.lineTo(c + QPointF(0, 8));
        path.lineTo(c + QPointF(5, 4));
        path.moveTo(c + QPointF(0, -7.5));
        path.lineTo(c + QPointF(0, 7.5));
    }
    painter.drawPath(path);
}

void paint_dots(QPainter& painter, const QPointF& c, const QColor& ink) {
    painter.setPen(Qt::NoPen);
    painter.setBrush(ink);
    for (int row = 0; row < 2; ++row)
        for (int i = 0; i < 3; ++i) painter.drawEllipse(QPointF(c.x() - 4 + i * 4, c.y() - 2 + row * 4), 1.2, 1.2);
}

// 一个方案的按钮:左边布局缩略图,右边名字和标签。
class SchemeTab final : public QPushButton {
public:
    SchemeTab(const QString& name, std::vector<PaneSlot> cells, bool active, const QString& tag, QWidget* parent)
        : QPushButton(name, parent), cells_(std::move(cells)), active_(active), tag_(tag) {
        setCheckable(true);
        setChecked(active);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus);
        setFixedHeight(42);
    }
    QSize sizeHint() const override {
        const qreal name_w = QFontMetricsF(theme::labelPx(15, QFont::DemiBold)).horizontalAdvance(text());
        return QSize(static_cast<int>(std::ceil(8 + 46 + 10 + name_w + tagWidth() + 14)), 42);
    }
    // 方案多、窗口窄时可以挤:名字省略,标签放不下就不画。
    QSize minimumSizeHint() const override {
        const qreal name_w = QFontMetricsF(theme::labelPx(15, QFont::DemiBold)).horizontalAdvance(QStringLiteral("标准…"));
        return QSize(static_cast<int>(std::ceil(8 + 46 + 10 + name_w + 10)), 42);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QColor stroke = active_ ? theme::kCyan : underMouse() ? theme::kBorderBright : theme::kRowBorder;
        theme::paintGlass(painter, QRectF(rect()), 4, false, active_ ? QColor(30, 60, 92, 184) : QColor(30, 44, 70, 102),
                          stroke);
        const bool next = std::any_of(cells_.begin(), cells_.end(), [](const PaneSlot& s) { return s.pane == PaneId::NextStep; });
        paintLayoutThumbnail(painter, QRectF(8, (height() - 28) / 2.0, 46, 28), cells_,
                             next ? std::optional<PaneId>(PaneId::NextStep) : std::nullopt);
        QFont name = theme::labelPx(15, QFont::DemiBold);
        painter.setFont(name);
        painter.setPen(active_ ? QColor(Qt::white) : theme::kTextPrimary);
        const qreal x = 8 + 46 + 10;
        const qreal room = width() - x - 10;
        const qreal name_w = QFontMetricsF(name).horizontalAdvance(text());
        const bool with_tag = tagWidth() > 0 && name_w + tagWidth() <= room + 1;
        const QString shown = QFontMetricsF(name).elidedText(text(), Qt::ElideRight, with_tag ? room - tagWidth() : room);
        painter.drawText(QRectF(x, 0, width() - x, height()), Qt::AlignLeft | Qt::AlignVCenter, shown);
        const QString tag = active_ ? QStringLiteral("正在用") : tag_;
        if (!with_tag) return;
        const qreal tx = x + QFontMetricsF(name).horizontalAdvance(shown) + 8;
        const qreal tw = QFontMetricsF(tagFont()).horizontalAdvance(tag) + 12;
        const QRectF box(tx, height() / 2.0 - 10, tw, 20);
        theme::paintGlass(painter, box, 3, false, active_ ? theme::kAccentBg : QColor(255, 255, 255, 20), Qt::transparent);
        painter.setFont(tagFont());
        painter.setPen(active_ ? theme::kAccentText : theme::kTextBright);
        painter.drawText(box, Qt::AlignCenter, tag);
    }
    void enterEvent(QEnterEvent* event) override {
        update();
        QPushButton::enterEvent(event);
    }
    void leaveEvent(QEvent* event) override {
        update();
        QPushButton::leaveEvent(event);
    }

private:
    static QFont tagFont() {
        QFont font = theme::labelPx(11, QFont::Bold);
        font.setLetterSpacing(QFont::AbsoluteSpacing, 1);
        return font;
    }
    qreal tagWidth() const {
        const QString tag = active_ ? QStringLiteral("正在用") : tag_;
        return tag.isEmpty() ? 0 : QFontMetricsF(tagFont()).horizontalAdvance(tag) + 12 + 8;
    }
    std::vector<PaneSlot> cells_;
    bool active_;
    QString tag_;
};

// 「已删掉「战场事件」 撤销 Ctrl+Z」那一条。
class Toast final : public QFrame {
public:
    using QFrame::QFrame;

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        theme::paintGlass(painter, QRectF(rect()), 4, false, QColor(10, 17, 30, 247), theme::kBorderBright);
    }
};

}  // namespace

LayoutEditor::LayoutEditor(UiProfileController* controller, QWidget* parent)
    : QFrame(parent), controller_(controller), role_(controller->role()) {
    setObjectName(QStringLiteral("layoutEditor"));
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setStyleSheet(QStringLiteral("QLabel{background:transparent; color:%1;}").arg(theme::css(theme::kTextPrimary)));

    top_ = new QWidget(this);
    auto* top = new QHBoxLayout(top_);
    top->setContentsMargins(16, 0, 16, 0);
    top->setSpacing(10);
    auto* title = new QLabel(QStringLiteral("界面设置"), top_);
    QFont title_font = theme::labelPx(20, QFont::Bold);
    title_font.setLetterSpacing(QFont::AbsoluteSpacing, 2);
    title->setFont(title_font);
    top->addWidget(title);
    seat_ = new QLabel(role_title(role_), top_);
    seat_->setFont(theme::labelPx(13));
    seat_->setStyleSheet(QStringLiteral("color:%1;").arg(theme::css(theme::kTextSecondary)));
    top->addWidget(seat_);
    operator_button_ = new GlassButton(QString(), top_, GlassButton::Kind::Chip);
    operator_button_->setObjectName(QStringLiteral("uiOperator"));
    operator_menu_ = new QMenu(operator_button_);
    operator_button_->setMenu(operator_menu_);
    top->addWidget(operator_button_);
    top->addStretch(1);
    auto* lock = new QLabel(QStringLiteral("比赛中自动锁定，只能赛前改"), top_);
    lock->setFont(theme::labelPx(12));
    lock->setStyleSheet(QStringLiteral("color:%1; background:%2; padding:6px 12px;")
                            .arg(theme::css(theme::kTextBright), theme::css(theme::kChip)));
    top->addWidget(lock);
    undo_button_ = new GlassButton(QStringLiteral("↶ 撤销"), top_);
    undo_button_->setObjectName(QStringLiteral("uiUndo"));
    reset_button_ = new GlassButton(QStringLiteral("恢复默认"), top_);
    reset_button_->setObjectName(QStringLiteral("uiReset"));
    more_button_ = new GlassButton(QStringLiteral("更多 ▾"), top_);
    more_button_->setObjectName(QStringLiteral("uiMore"));
    more_menu_ = new QMenu(more_button_);
    more_button_->setMenu(more_menu_);
    done_button_ = new GlassButton(QStringLiteral("完成"), top_, GlassButton::Kind::Accent);
    done_button_->setObjectName(QStringLiteral("uiSettingsDone"));
    for (GlassButton* button : {undo_button_, reset_button_, more_button_, done_button_}) {
        button->setFixedHeight(36);
        top->addWidget(button);
    }
    connect(undo_button_, &QPushButton::clicked, this, &LayoutEditor::undo);
    connect(reset_button_, &QPushButton::clicked, this, &LayoutEditor::resetScheme);
    connect(done_button_, &QPushButton::clicked, this, &LayoutEditor::doneRequested);

    strip_ = new QWidget(this);
    tabs_layout_ = new QHBoxLayout(strip_);
    tabs_layout_->setContentsMargins(16, 8, 16, 8);
    tabs_layout_->setSpacing(8);

    details_ = new PaneDetails(controller_, this);
    details_->setVisible(false);
    connect(details_, &PaneDetails::aboutToChange, this, &LayoutEditor::pushUndo);
    connect(details_, &PaneDetails::removePaneRequested, this, [this](PaneId pane) {
        closeDetails();
        removePane(pane);
    });
    connect(details_, &PaneDetails::videoBackRequested, this, &LayoutEditor::putVideoBack);

    toast_ = new Toast(this);
    toast_->setObjectName(QStringLiteral("uiToast"));
    auto* toast = new QHBoxLayout(toast_);
    toast->setContentsMargins(18, 0, 6, 0);
    toast->setSpacing(14);
    toast_label_ = new QLabel(toast_);
    toast_label_->setFont(theme::labelPx(14));
    toast->addWidget(toast_label_);
    toast_undo_ = new GlassButton(QStringLiteral("↶ 撤销"), toast_, GlassButton::Kind::Accent);
    toast_undo_->setObjectName(QStringLiteral("uiToastUndo"));
    toast_undo_->setFixedHeight(30);
    toast->addWidget(toast_undo_);
    toast_key_ = new QLabel(QStringLiteral("Ctrl+Z"), toast_);
    toast_key_->setFont(theme::numericPx(12));
    toast_key_->setStyleSheet(QStringLiteral("color:%1; margin-right:8px;").arg(theme::css(theme::kTextSecondary)));
    toast->addWidget(toast_key_);
    toast_->setVisible(false);
    toast_timer_.setSingleShot(true);
    connect(&toast_timer_, &QTimer::timeout, toast_, &QWidget::hide);
    connect(toast_undo_, &QPushButton::clicked, this, &LayoutEditor::undo);

    auto* undo_key = new QShortcut(QKeySequence::Undo, this);
    undo_key->setContext(Qt::WidgetWithChildrenShortcut);
    connect(undo_key, &QShortcut::activated, this, &LayoutEditor::undo);

    connect(controller_, &UiProfileController::changed, this, [this] {
        if (isVisible()) reload();
    });
    reload();
}

void LayoutEditor::begin() {
    undo_.clear();
    last_removed_.reset();
    drag_ = Drag{};
    closeDetails();
    toast_->hide();
    reload();
    setFocus();
}

void LayoutEditor::setPreview(const QPixmap& window, const QRect& grid) {
    preview_ = window;
    const QSize size = window.deviceIndependentSize().toSize();
    if ((!size.isEmpty() && size != page_size_) || grid != grid_in_window_) {
        page_size_ = size;
        grid_in_window_ = grid;
        relayout();
    }
    // 缩略图里的数据是活的(图传、本机状态),一秒画一次就够。
    if (!tiles_painted_.isValid() || tiles_painted_.elapsed() > 900) refreshTileImages();
    update();
}

void LayoutEditor::setPaneImageSource(std::function<QPixmap(PaneId, QSize)> source) {
    pane_image_ = std::move(source);
    refreshTileImages();
}

void LayoutEditor::setConfigDisabledAlerts(const QStringList& kinds) { details_->setConfigDisabledAlerts(kinds); }

const PaneSlot* LayoutEditor::cell(PaneId pane) const {
    for (const PaneSlot& slot : working_.cells)
        if (slot.pane == pane) return &slot;
    return nullptr;
}

bool LayoutEditor::videoCell() const { return cell(PaneId::Video) != nullptr; }

bool LayoutEditor::hasDetails(PaneId pane) const {
    return PaneDetails::page_for(pane, videoCell(), embedded_host(layout_profile(role_), PaneId::Video).has_value())
        .has_value();
}

bool LayoutEditor::modified(const UiScheme& scheme) const {
    const std::optional<UiScheme> factory = builtin_scheme(role_, scheme.id);
    if (!factory) return false;
    return scheme_layout(role_, *factory).cells != scheme_layout(role_, scheme).cells || factory->options != scheme.options ||
           factory->hidden.count(PaneId::Video) != scheme.hidden.count(PaneId::Video);
}

void LayoutEditor::reload() {
    const UiScheme& active = controller_->activeScheme();
    working_ = active;
    working_.cells = scheme_layout(role_, active).cells;
    // 格子写明了画面里有哪几块;hidden 只记「图传删掉了」:旧版本存的布局打开时会把图传从本车里拆出来,
    // 删过的不能又冒出来。
    working_.hidden.clear();
    if (active.hidden.count(PaneId::Video)) working_.hidden.insert(PaneId::Video);
    if (details_pane_ && !cell(*details_pane_)) closeDetails();
    hover_seam_.reset();
    hover_tool_.reset();
    hover_pane_.reset();
    if (isVisible() && drag_.kind == DragKind::None) updateHover(mapFromGlobal(QCursor::pos()));
    rebuildTabs();
    rebuildOperatorMenu();
    rebuildMoreMenu();
    rebuildTiles();
    undo_button_->setEnabled(canUndo());
    reset_button_->setEnabled(!is_builtin_scheme(active.id) || modified(active));
    Q_EMIT previewRequested();
    update();
}

void LayoutEditor::pushUndo() {
    undo_.push_back(controller_->setup());
    if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
    undo_button_->setEnabled(true);
}

void LayoutEditor::undo() {
    if (undo_.empty()) return;
    cancelDrag();
    const RoleSetup previous = undo_.back();
    undo_.pop_back();
    controller_->restoreSetup(previous);
    showToast(QStringLiteral("已撤销上一步"), false);
}

void LayoutEditor::commit(const std::vector<PaneSlot>& cells, const std::set<PaneId>& hidden) {
    pushUndo();
    UiScheme next = working_;
    next.cells = cells;
    next.hidden = hidden;
    if (!controller_->saveLayout(next)) {
        undo_.pop_back();
        showToast(QStringLiteral("比赛中不能改"), false);
    }
}

QRect LayoutEditor::unitToCanvas(const QRect& units) const {
    const double scale = canvas_.width() / static_cast<double>(std::max(1, page_size_.width()));
    return unit_rect(grid_canvas_, kGridColumns, kGridRows, static_cast<int>(std::lround(kRealSpacing * scale)), units);
}

QPointF LayoutEditor::unitPos(const QPoint& pixel) const {
    const double scale = canvas_.width() / static_cast<double>(std::max(1, page_size_.width()));
    const double gap = kRealSpacing * scale;
    const double pitch_x = (grid_canvas_.width() + gap) / kGridColumns;
    const double pitch_y = (grid_canvas_.height() + gap) / kGridRows;
    return QPointF((pixel.x() - grid_canvas_.left() + gap / 2) / pitch_x,
                   (pixel.y() - grid_canvas_.top() + gap / 2) / pitch_y);
}

QRect LayoutEditor::seamRect(const Seam& seam) const {
    if (seam.vertical) {
        const QRect left = unitToCanvas(QRect(seam.line - 1, seam.from, 1, seam.to - seam.from));
        const QRect right = unitToCanvas(QRect(seam.line, seam.from, 1, 1));
        const int x = (left.right() + right.left()) / 2;
        return QRect(x - 2, left.top(), 4, left.height());
    }
    const QRect above = unitToCanvas(QRect(seam.from, seam.line - 1, seam.to - seam.from, 1));
    const QRect below = unitToCanvas(QRect(seam.from, seam.line, 1, 1));
    const int y = (above.bottom() + below.top()) / 2;
    return QRect(above.left(), y - 2, above.width(), 4);
}

std::optional<PaneId> LayoutEditor::paneAt(const QPoint& pixel) const {
    for (const PaneSlot& slot : working_.cells)
        if (unitToCanvas(slot.units()).contains(pixel)) return slot.pane;
    return std::nullopt;
}

QRect LayoutEditor::closeRect(PaneId pane) const {
    const PaneSlot* slot = cell(pane);
    if (!slot) return {};
    const QRect box = unitToCanvas(slot->units());
    return QRect(box.right() - 8 - kTool, box.top() + 7, kTool, kTool);
}

QRect LayoutEditor::gearRect(PaneId pane) const {
    if (!cell(pane) || !hasDetails(pane)) return {};
    return closeRect(pane).translated(-(kTool + 4), 0);
}

QRect LayoutEditor::tileRect(PaneId pane) const {
    for (const Tile& tile : tiles_)
        if (tile.pane == pane) return tileOnScreen(tile);
    return {};
}

QString LayoutEditor::toastText() const { return toast_->isVisible() ? toast_label_->text() : QString(); }

bool LayoutEditor::detailsVisible() const { return details_->isVisible(); }

QPushButton* LayoutEditor::schemeButton(const QString& id) const {
    for (QPushButton* tab : tabs_)
        if (tab->property("schemeId").toString() == id) return tab;
    return nullptr;
}

DropSide LayoutEditor::sideAt(const QRect& box, const QPoint& pixel) const {
    const double fx = (pixel.x() - box.left()) / static_cast<double>(std::max(1, box.width()));
    const double fy = (pixel.y() - box.top()) / static_cast<double>(std::max(1, box.height()));
    if (fx > 0.25 && fx < 0.75 && fy > 0.25 && fy < 0.75) return DropSide::Center;
    const double left = fx, right = 1 - fx, top = fy, bottom = 1 - fy;
    const double nearest = std::min({left, right, top, bottom});
    if (nearest == left) return DropSide::Left;
    if (nearest == right) return DropSide::Right;
    if (nearest == top) return DropSide::Top;
    return DropSide::Bottom;
}

void LayoutEditor::placeStrip() {
    // 右边四句怎么改:窗口窄、方案多时让给方案那一排。
    tips_visible_ = width() - kTipsWidth >= tabs_layout_->sizeHint().width();
    strip_->setGeometry(0, kHeaderHeight, tips_visible_ ? width() - kTipsWidth : width(), kStripHeight);
    update(QRect(0, kHeaderHeight, width(), kStripHeight));
}

void LayoutEditor::relayout() {
    top_->setGeometry(0, 0, width(), kHeaderHeight);
    placeStrip();
    library_ = QRect(12, kBodyTop, kLibraryWidth, height() - kBodyTop - 12);
    const QRect room(library_.right() + 13, kBodyTop, width() - library_.right() - 25, height() - kBodyTop - 12 - kToastRow);
    const double aspect = page_size_.width() / static_cast<double>(std::max(1, page_size_.height()));
    int w = room.width();
    int h = static_cast<int>(std::lround(w / aspect));
    if (h > room.height()) {
        h = room.height();
        w = static_cast<int>(std::lround(h * aspect));
    }
    canvas_ = QRect(room.left() + (room.width() - w) / 2, room.top(), w, h);
    const double scale = w / static_cast<double>(std::max(1, page_size_.width()));
    grid_canvas_ = QRect(canvas_.left() + static_cast<int>(std::lround(grid_in_window_.left() * scale)),
                         canvas_.top() + static_cast<int>(std::lround(grid_in_window_.top() * scale)),
                         static_cast<int>(std::lround(grid_in_window_.width() * scale)),
                         static_cast<int>(std::lround(grid_in_window_.height() * scale)));
    rebuildTiles();
    if (toast_->isVisible()) {
        toast_->adjustSize();
        toast_->move(canvas_.center().x() - toast_->width() / 2, canvas_.bottom() + 8);
    }
    update();
}

void LayoutEditor::resizeEvent(QResizeEvent* event) {
    QFrame::resizeEvent(event);
    relayout();
}

void LayoutEditor::rebuildTabs() {
    for (QPushButton* tab : tabs_) {
        tabs_layout_->removeWidget(tab);
        tab->hide();
        tab->deleteLater();
    }
    tabs_.clear();
    while (QLayoutItem* item = tabs_layout_->takeAt(0)) {
        if (item->widget() && item->widget() != save_new_) item->widget()->deleteLater();
        delete item;
    }
    auto* label = new QLabel(QStringLiteral("方案"), strip_);
    QFont font = theme::labelPx(13, QFont::Bold);
    font.setLetterSpacing(QFont::AbsoluteSpacing, 2);
    label->setFont(font);
    label->setStyleSheet(QStringLiteral("color:%1; margin-right:4px;").arg(theme::css(theme::kTextSecondary)));
    tabs_layout_->addWidget(label);
    const RoleSetup& setup = controller_->setup();
    for (const UiScheme& scheme : setup.schemes) {
        const bool active = scheme.id == setup.active;
        const QString tag = !is_builtin_scheme(scheme.id) ? QStringLiteral("自定义")
                            : modified(scheme)             ? QStringLiteral("改过")
                                                           : QString();
        auto* tab = new SchemeTab(scheme.name, scheme_layout(role_, scheme).cells, active, tag, strip_);
        tab->setObjectName(QStringLiteral("scheme_%1").arg(scheme.id));
        tab->setProperty("schemeId", scheme.id);
        connect(tab, &QPushButton::clicked, this, [this, id = scheme.id] {
            closeDetails();
            if (id == controller_->setup().active) {
                rebuildTabs();
                return;
            }
            pushUndo();
            controller_->selectScheme(id);
        });
        tabs_layout_->addWidget(tab);
        tabs_.push_back(tab);
    }
    if (!save_new_) {
        save_new_ = new GlassButton(QStringLiteral("＋ 存成新方案"), strip_);
        save_new_->setObjectName(QStringLiteral("uiSaveAsNew"));
        save_new_->setFixedHeight(40);
        connect(save_new_, &QPushButton::clicked, this, &LayoutEditor::saveAsNew);
    }
    tabs_layout_->addWidget(save_new_);
    tabs_layout_->addStretch(1);
    placeStrip();
}

QRect LayoutEditor::libraryView() const {
    return QRect(library_.left(), library_.top() + kLibraryTop, library_.width(), library_.height() - kLibraryTop - 8);
}

QRect LayoutEditor::tileOnScreen(const Tile& tile) const {
    const QRect view = libraryView();
    return tile.box.translated(view.left(), view.top() - library_scroll_);
}

// 缩略图按这块面板平时的大小画(像主界面上一样),再缩小放进组件库。「下一步」按 6 格宽画,8 格宽缩下来字太小。
QSize LayoutEditor::paneRenderSize(PaneId pane) const {
    QSize units = pane_default_units(pane);
    for (const PaneSlot& slot : layout_profile(role_).cells)
        if (slot.pane == pane) units = slot.units().size();
    if (pane == PaneId::Analysis) units = pane_default_units(pane);
    if (pane == PaneId::NextStep) units = QSize(6, 2);
    const double pitch_x = (grid_in_window_.width() + kRealSpacing) / static_cast<double>(kGridColumns);
    const double pitch_y = (grid_in_window_.height() + kRealSpacing) / static_cast<double>(kGridRows);
    return QSize(static_cast<int>(std::lround(units.width() * pitch_x - kRealSpacing)),
                 static_cast<int>(std::lround(units.height() * pitch_y - kRealSpacing)));
}

void LayoutEditor::rebuildTiles() {
    std::vector<PaneId> missing;
    for (const PaneId pane : available_panes(role_))
        if (!cell(pane)) missing.push_back(pane);
    // 一列:上面名字,下面缩略图(按面板本来的宽高比)。放不下就滚动。
    const std::vector<Tile> before = tiles_;
    tiles_.clear();
    const int inner = library_.width() - 24;
    int y = 0;
    for (const PaneId pane : missing) {
        const QSize real = paneRenderSize(pane);
        const QSize image = real.scaled(QSize(inner, kTileMaxHeight), Qt::KeepAspectRatio);
        tiles_.push_back({pane, QRect(12, y, inner, kTileCaption + image.height()),
                          QRect(12, y + kTileCaption, image.width(), image.height())});
        y += kTileCaption + image.height() + kTileGap;
    }
    library_content_ = std::max(0, y - kTileGap);
    library_scroll_ = std::clamp(library_scroll_, 0, std::max(0, library_content_ - libraryView().height()));
    const bool same = before.size() == tiles_.size() &&
                      std::equal(before.begin(), before.end(), tiles_.begin(),
                                 [](const Tile& a, const Tile& b) { return a.pane == b.pane && a.image == b.image; });
    if (!same) refreshTileImages();
}

void LayoutEditor::refreshTileImages() {
    // 只在这一页开着时画:关着时不该去动主界面的面板(启动时这么画过,花名册的画面会花掉)。
    if (!pane_image_ || !isVisible()) return;
    tiles_painted_.start();
    for (const Tile& tile : tiles_) tile_images_[tile.pane] = pane_image_(tile.pane, paneRenderSize(tile.pane));
}

// 开赛锁定会直接关掉这一页:开着的菜单、对话框、细项跟着收起,拖了一半的也算了。
void LayoutEditor::hideEvent(QHideEvent* event) {
    operator_menu_->close();
    more_menu_->close();
    for (QDialog* dialog : findChildren<QDialog*>())
        if (dialog->isVisible()) dialog->reject();
    closeDetails();
    if (drag_.kind != DragKind::None) cancelDrag();
    QWidget::hideEvent(event);
}

// 鼠标移出窗口:缝和图标的高亮、提示一起收掉。
void LayoutEditor::leaveEvent(QEvent* event) {
    if (drag_.kind == DragKind::None && (hover_seam_ || hover_tool_ || hover_pane_)) {
        hover_seam_.reset();
        hover_tool_.reset();
        hover_pane_.reset();
        unsetCursor();
        update();
    }
    QWidget::leaveEvent(event);
}

void LayoutEditor::wheelEvent(QWheelEvent* event) {
    if (!library_.contains(event->position().toPoint())) return;
    const int most = std::max(0, library_content_ - libraryView().height());
    library_scroll_ = std::clamp(library_scroll_ - event->angleDelta().y() / 2, 0, most);
    update();
}

void LayoutEditor::showToast(const QString& text, bool with_undo) {
    toast_label_->setText(text);
    toast_undo_->setVisible(with_undo);
    toast_key_->setVisible(with_undo);
    toast_->setFixedHeight(42);
    toast_->adjustSize();
    toast_->move(canvas_.center().x() - toast_->width() / 2, canvas_.bottom() + 8);
    toast_->show();
    toast_->raise();
    toast_timer_.start(with_undo ? 10000 : 4000);
}

void LayoutEditor::placeDetails(const QRect& anchor, const QRect& avoid) {
    details_->adjustSize();
    const QSize size = details_->size();
    int x = std::clamp(anchor.right() + 10 - size.width(), 12, width() - 12 - size.width());
    // 先放在这块面板下面,不挡住它(比如「只看控制度」要看得见效果);下面放不下再盖在它上面。
    int y = avoid.bottom() + 10;
    if (y + size.height() > height() - 12) y = anchor.bottom() + 12;
    y = std::clamp(y, kHeaderHeight + 4, std::max(kHeaderHeight + 4, height() - 12 - size.height()));
    details_->move(x, y);
    details_->show();
    details_->raise();
}

void LayoutEditor::openDetails(PaneId pane) {
    const auto page = PaneDetails::page_for(pane, videoCell(), embedded_host(layout_profile(role_), PaneId::Video).has_value());
    if (!page) return;
    if (details_pane_ == pane && details_->isVisible()) {
        closeDetails();
        return;
    }
    details_pane_ = pane;
    details_->showPage(*page);
    details_->refresh();
    placeDetails(gearRect(pane), unitToCanvas(cell(pane)->units()));
    update();
}

void LayoutEditor::openAlerts() {
    details_pane_.reset();
    details_->showPage(PaneDetails::Page::Alerts);
    details_->refresh();
    const QRect more(more_button_->mapTo(this, QPoint(0, 0)), more_button_->size());
    placeDetails(more, more);
}

void LayoutEditor::closeDetails() {
    details_->hide();
    details_pane_.reset();
    update();
}

void LayoutEditor::removePane(PaneId pane) {
    const bool embeddable = embedded_host(layout_profile(role_), PaneId::Video).has_value();
    std::set<PaneId> hidden = working_.hidden;
    if (!cell(pane)) return;
    if (working_.cells.size() <= 1) {
        showToast(QStringLiteral("画面里至少要留一块"), false);
        return;
    }
    // 删掉图传要记一笔,下次打开不会被当成旧布局又拆出来。
    if (pane == PaneId::Video) hidden.insert(PaneId::Video);
    last_removed_ = pane;
    if (details_pane_ == pane) closeDetails();
    commit(layout_remove(working_.cells, pane), hidden);
    showToast(QStringLiteral("已删掉「%1」").arg(library_title(pane, embeddable)), true);
}

void LayoutEditor::putVideoBack() {
    closeDetails();
    if (!cell(PaneId::SelfCommands)) {
        showToast(QStringLiteral("「本车」不在画面里，图传放不回去"), false);
        return;
    }
    std::set<PaneId> hidden = working_.hidden;
    hidden.erase(PaneId::Video);
    commit(layout_remove(working_.cells, PaneId::Video), hidden);
}

void LayoutEditor::resetScheme() {
    closeDetails();
    const UiScheme& active = controller_->activeScheme();
    pushUndo();
    if (is_builtin_scheme(active.id)) {
        controller_->resetScheme(active.id);
    } else {
        UiScheme next = working_;
        next.cells = layout_profile(role_).cells;
        next.hidden.clear();
        controller_->saveLayout(next);
    }
    showToast(QStringLiteral("已恢复默认"), true);
}

// 没装 Qt 的中文翻译,对话框自带的按钮是英文:自己写上中文。
std::optional<QString> LayoutEditor::askName(const QString& title, const QString& label, const QString& text) {
    QInputDialog dialog(this);
    dialog.setWindowTitle(title);
    dialog.setLabelText(label);
    dialog.setTextValue(text);
    dialog.setOkButtonText(QStringLiteral("确定"));
    dialog.setCancelButtonText(QStringLiteral("取消"));
    if (dialog.exec() != QDialog::Accepted) return std::nullopt;
    return dialog.textValue().trimmed();
}

bool LayoutEditor::confirm(const QString& title, const QString& text, const QString& yes) {
    QMessageBox box(QMessageBox::Question, title, text, QMessageBox::NoButton, this);
    QPushButton* accept = box.addButton(yes, QMessageBox::AcceptRole);
    box.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
    box.exec();
    return box.clickedButton() == accept;
}

void LayoutEditor::saveAsNew() {
    closeDetails();
    const auto name = askName(QStringLiteral("存成新方案"), QStringLiteral("新方案的名字（最多 %1 个字）").arg(kMaxUiNameChars),
                              controller_->activeScheme().name + QStringLiteral(" 副本"));
    if (!name) return;
    pushUndo();
    if (const QString problem = controller_->createScheme(*name); !problem.isEmpty()) {
        undo_.pop_back();
        showToast(problem, false);
    }
}

void LayoutEditor::renameScheme() {
    const UiScheme& scheme = controller_->activeScheme();
    const QString id = scheme.id;
    const auto name = askName(QStringLiteral("方案改名"), QStringLiteral("新名字"), scheme.name);
    if (!name) return;
    pushUndo();
    if (const QString problem = controller_->renameScheme(id, *name); !problem.isEmpty()) {
        undo_.pop_back();
        showToast(problem, false);
    }
}

void LayoutEditor::deleteScheme() {
    const UiScheme& scheme = controller_->activeScheme();
    const QString id = scheme.id;
    const QString name = scheme.name;
    pushUndo();
    if (const QString problem = controller_->removeScheme(id); !problem.isEmpty()) {
        undo_.pop_back();
        showToast(problem, false);
        return;
    }
    showToast(QStringLiteral("已删掉方案「%1」").arg(name), true);
}

void LayoutEditor::exportBackup() {
    const QString name = controller_->currentOperator();
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出备份"),
                                                      QDir::home().filePath(QStringLiteral("界面方案-%1.json").arg(name)),
                                                      QStringLiteral("界面方案 (*.json)"));
    if (path.isEmpty()) return;
    QString error;
    showToast(controller_->exportCurrent(path, &error) ? QStringLiteral("已导出到 %1").arg(path)
                                                       : QStringLiteral("导出失败：%1").arg(error),
              false);
}

void LayoutEditor::importBackup() {
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("导入方案"), QDir::homePath(),
                                                      QStringLiteral("界面方案 (*.json)"));
    if (path.isEmpty()) return;
    QString error;
    const QStringList incoming = UiProfileController::operatorsIn(path, &error);
    if (incoming.isEmpty()) {
        showToast(error.isEmpty() ? QStringLiteral("文件里没有操作手") : error, false);
        return;
    }
    QStringList clash;
    for (const QString& name : incoming)
        if (controller_->operatorNames().contains(name)) clash << name;
    if (!clash.isEmpty() &&
        !confirm(QStringLiteral("导入方案"),
                 QStringLiteral("这台电脑上已经有「%1」，用文件里的替换掉吗？").arg(clash.join(QStringLiteral("」「"))),
                 QStringLiteral("替换")))
        return;
    undo_.clear();
    QStringList replaced;
    const QString problem = controller_->importFrom(path, &replaced);
    showToast(problem.isEmpty() ? QStringLiteral("已导入 %1").arg(incoming.join(QStringLiteral("、"))) : problem, false);
}

void LayoutEditor::addOperator() {
    const auto name = askName(QStringLiteral("新建操作手"), QStringLiteral("名字（最多 %1 个字）").arg(kMaxUiNameChars), {});
    if (!name) return;
    undo_.clear();
    if (const QString problem = controller_->addOperator(*name); !problem.isEmpty()) showToast(problem, false);
}

void LayoutEditor::renameOperator() {
    const QString from = controller_->currentOperator();
    const auto name = askName(QStringLiteral("操作手改名"), QStringLiteral("新名字"), from);
    if (!name) return;
    if (const QString problem = controller_->renameOperator(from, *name); !problem.isEmpty()) showToast(problem, false);
}

void LayoutEditor::removeOperator() {
    const QString name = controller_->currentOperator();
    if (!confirm(QStringLiteral("删除操作手"),
                 QStringLiteral("删掉「%1」和他所有席位的方案？删了找不回来，可以先导出备份。").arg(name),
                 QStringLiteral("删除")))
        return;
    undo_.clear();
    if (const QString problem = controller_->removeOperator(name); !problem.isEmpty()) showToast(problem, false);
}

void LayoutEditor::rebuildOperatorMenu() {
    const QString current = controller_->currentOperator();
    operator_button_->setText(QStringLiteral("操作手 %1 ▾").arg(current));
    operator_menu_->clear();
    const QStringList names = controller_->operatorNames();
    for (const QString& name : names) {
        QAction* action = operator_menu_->addAction(name);
        action->setCheckable(true);
        action->setChecked(name == current);
        connect(action, &QAction::triggered, this, [this, name] {
            if (name == controller_->currentOperator()) return;
            undo_.clear();
            controller_->switchOperator(name);
        });
    }
    operator_menu_->addSeparator();
    connect(operator_menu_->addAction(QStringLiteral("新建操作手…")), &QAction::triggered, this, &LayoutEditor::addOperator);
    connect(operator_menu_->addAction(QStringLiteral("给「%1」改名…").arg(current)), &QAction::triggered, this,
            &LayoutEditor::renameOperator);
    QAction* remove = operator_menu_->addAction(QStringLiteral("删除「%1」…").arg(current));
    remove->setEnabled(names.size() > 1);
    connect(remove, &QAction::triggered, this, &LayoutEditor::removeOperator);
}

void LayoutEditor::rebuildMoreMenu() {
    more_menu_->clear();
    connect(more_menu_->addAction(QStringLiteral("事件提醒（横幅和语音）…")), &QAction::triggered, this,
            &LayoutEditor::openAlerts);
    more_menu_->addSeparator();
    connect(more_menu_->addAction(QStringLiteral("导出备份…")), &QAction::triggered, this, &LayoutEditor::exportBackup);
    connect(more_menu_->addAction(QStringLiteral("导入…")), &QAction::triggered, this, &LayoutEditor::importBackup);
    more_menu_->addSeparator();
    const bool custom = !is_builtin_scheme(controller_->activeScheme().id);
    QAction* rename = more_menu_->addAction(QStringLiteral("给这个方案改名…"));
    rename->setEnabled(custom);
    connect(rename, &QAction::triggered, this, &LayoutEditor::renameScheme);
    QAction* remove = more_menu_->addAction(QStringLiteral("删除这个方案"));
    remove->setEnabled(custom);
    connect(remove, &QAction::triggered, this, &LayoutEditor::deleteScheme);
}

void LayoutEditor::cancelDrag() {
    if (drag_.kind == DragKind::Seam && drag_.line != drag_.seam.line) {
        working_.cells = drag_.before;
        Q_EMIT previewRequested();
    }
    drag_ = Drag{};
    unsetCursor();
    update();
}

void LayoutEditor::updateDrag() {
    drag_.target.reset();
    drag_.result.reset();
    drag_.action = DropAction::None;
    // 画面里的拖回组件库:拿掉。
    if (drag_.kind == DragKind::Move && library_.contains(drag_.mouse)) {
        drag_.action = working_.cells.size() > 1 ? DropAction::Remove : DropAction::None;
        return;
    }
    if (!grid_canvas_.contains(drag_.mouse)) return;
    const auto under = paneAt(drag_.mouse);
    if (!under || (drag_.kind == DragKind::Move && *under == drag_.pane)) return;
    drag_.target = under;
    const QRect units = cell(*under)->units();
    drag_.side = sideAt(unitToCanvas(units), drag_.mouse);
    // 图传拖到「本车」上:放回本车里,变回下面的小窗。
    if (drag_.pane == PaneId::Video && *under == PaneId::SelfCommands &&
        embedded_host(layout_profile(role_), PaneId::Video).has_value()) {
        drag_.action = DropAction::Embed;
        drag_.result = layout_remove(working_.cells, PaneId::Video);
        return;
    }
    // 能插就插;插不下,画面里的就换位置,组件库来的就替换掉那一块。
    const auto attempt = [&](DropAction action, std::optional<std::vector<PaneSlot>> cells) {
        if (drag_.result || !cells) return;
        drag_.result = std::move(cells);
        drag_.action = action;
    };
    const DropSide longer = units.width() >= units.height() ? DropSide::Right : DropSide::Bottom;
    if (drag_.side != DropSide::Center)
        attempt(DropAction::Insert, layout_drop(working_.cells, drag_.pane, *under, drag_.side));
    if (drag_.kind == DragKind::Move)
        attempt(DropAction::Swap, layout_drop(working_.cells, drag_.pane, *under, DropSide::Center));
    if (drag_.side == DropSide::Center) {
        // 放在正中间又换不了:先插到离原来位置远的那一边(上下挨着的两块就是调个先后),再试长的那边和别的边。
        std::vector<DropSide> order;
        if (drag_.kind == DragKind::Move) {
            const QRect from = cell(drag_.pane)->units();
            if (from.top() > units.bottom()) order.push_back(DropSide::Top);
            else if (from.bottom() < units.top()) order.push_back(DropSide::Bottom);
            else if (from.left() > units.right()) order.push_back(DropSide::Left);
            else if (from.right() < units.left()) order.push_back(DropSide::Right);
        }
        for (const DropSide side : {longer, DropSide::Right, DropSide::Bottom, DropSide::Left, DropSide::Top})
            if (std::find(order.begin(), order.end(), side) == order.end()) order.push_back(side);
        for (const DropSide side : order) attempt(DropAction::Insert, layout_drop(working_.cells, drag_.pane, *under, side));
    }
    if (drag_.kind == DragKind::Add) attempt(DropAction::Replace, layout_replace(working_.cells, drag_.pane, *under));
}

// 放不下时说是哪一块太大:拖的这块本身放得进那一块,卡住的就是那一块挪不过去。
QString LayoutEditor::dropProblem() const {
    PaneId tight = drag_.pane;
    if (drag_.target) {
        const QRect target = cell(*drag_.target)->units();
        const QSize need = pane_min_units(drag_.pane);
        if (target.width() >= need.width() && target.height() >= need.height()) tight = *drag_.target;
    }
    const QSize need = pane_min_units(tight);
    return QStringLiteral("「%1」最小要 %2 × %3 格").arg(pane_title(tight)).arg(need.width()).arg(need.height());
}

void LayoutEditor::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        if (drag_.kind != DragKind::None) cancelDrag();
        else closeDetails();
        return;
    }
    QFrame::keyPressEvent(event);
}

void LayoutEditor::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    const QPoint pos = event->position().toPoint();
    setFocus();
    if (details_->isVisible() && !details_->geometry().contains(pos)) {
        const bool own_gear = details_pane_ && gearRect(*details_pane_).contains(pos);
        closeDetails();
        if (own_gear) return;
    }
    if (grid_canvas_.contains(pos)) {
        for (const PaneSlot& slot : working_.cells) {
            if (gearRect(slot.pane).contains(pos)) {
                openDetails(slot.pane);
                return;
            }
            if (closeRect(slot.pane).contains(pos)) {
                removePane(slot.pane);
                return;
            }
        }
        const QPointF unit = unitPos(pos);
        if (const auto seam = seam_at(working_.cells, unit.x(), unit.y(), 0.24)) {
            drag_ = Drag{};
            drag_.kind = DragKind::Seam;
            drag_.seam = *seam;
            drag_.line = seam->line;
            drag_.before = working_.cells;
            drag_.press = drag_.mouse = pos;
            update();
            return;
        }
        if (const auto pane = paneAt(pos)) {
            drag_ = Drag{};
            drag_.kind = DragKind::Move;
            drag_.pane = *pane;
            drag_.press = drag_.mouse = pos;
        }
        return;
    }
    for (const Tile& tile : tiles_) {
        if (!libraryView().contains(pos) || !tileOnScreen(tile).contains(pos)) continue;
        drag_ = Drag{};
        drag_.kind = DragKind::Add;
        drag_.pane = tile.pane;
        drag_.press = drag_.mouse = pos;
        return;
    }
}

void LayoutEditor::updateHover(const QPoint& pos) {
    {
        std::optional<Seam> seam;
        std::optional<QRect> tool;
        Qt::CursorShape shape = Qt::ArrowCursor;
        if (grid_canvas_.contains(pos)) {
            const QPointF unit = unitPos(pos);
            seam = seam_at(working_.cells, unit.x(), unit.y(), 0.24);
            for (const PaneSlot& slot : working_.cells)
                for (const QRect& box : {gearRect(slot.pane), closeRect(slot.pane)})
                    if (box.contains(pos)) tool = box;
            if (tool) {
                seam.reset();
                shape = Qt::PointingHandCursor;
            } else if (seam) {
                shape = seam->vertical ? Qt::SplitHCursor : Qt::SplitVCursor;
            } else if (paneAt(pos)) {
                shape = Qt::OpenHandCursor;
            }
        }
        for (const Tile& tile : tiles_)
            if (libraryView().contains(pos) && tileOnScreen(tile).contains(pos)) shape = Qt::OpenHandCursor;
        setCursor(shape);
        const std::optional<PaneId> pane = grid_canvas_.contains(pos) ? paneAt(pos) : std::nullopt;
        if (seam != hover_seam_ || tool != hover_tool_ || pane != hover_pane_) {
            hover_seam_ = seam;
            hover_tool_ = tool;
            hover_pane_ = pane;
            update();
        }
    }
}

void LayoutEditor::mouseMoveEvent(QMouseEvent* event) {
    const QPoint pos = event->position().toPoint();
    if (drag_.kind == DragKind::None) {
        updateHover(pos);
        return;
    }
    drag_.mouse = pos;
    if ((pos - drag_.press).manhattanLength() > 4) drag_.moved = true;
    if (drag_.kind == DragKind::Seam) {
        const QPointF unit = unitPos(pos);
        const int limit = drag_.seam.vertical ? kGridColumns - 1 : kGridRows - 1;
        const int line = std::clamp(static_cast<int>(std::lround(drag_.seam.vertical ? unit.x() : unit.y())), 1, limit);
        if (line != drag_.line) {
            if (const auto moved = layout_move_seam(drag_.before, drag_.seam, line)) {
                working_.cells = *moved;
                drag_.line = line;
                Q_EMIT previewRequested();
            }
        }
        setCursor(drag_.seam.vertical ? Qt::SplitHCursor : Qt::SplitVCursor);
        update();
        return;
    }
    if (drag_.moved) {
        setCursor(Qt::ClosedHandCursor);
        updateDrag();
    }
    update();
}

void LayoutEditor::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || drag_.kind == DragKind::None) return;
    drag_.mouse = event->position().toPoint();
    const Drag drag = drag_;
    drag_ = Drag{};
    unsetCursor();
    if (drag.kind == DragKind::Seam) {
        if (drag.line != drag.seam.line) {
            const std::vector<PaneSlot> moved = working_.cells;
            working_.cells = drag.before;
            commit(moved, working_.hidden);
        }
        update();
        return;
    }
    if (!drag.moved) {
        update();
        return;
    }
    drag_ = drag;
    updateDrag();
    const Drag done = drag_;
    drag_ = Drag{};
    const bool embeddable = embedded_host(layout_profile(role_), PaneId::Video).has_value();
    std::set<PaneId> hidden = working_.hidden;
    switch (done.action) {
    case DropAction::Remove:
        removePane(done.pane);
        break;
    case DropAction::Embed:
        hidden.erase(PaneId::Video);
        if (last_removed_ == done.pane) last_removed_.reset();
        commit(*done.result, hidden);
        showToast(QStringLiteral("图传放回「本车」里了"), true);
        break;
    case DropAction::Insert:
    case DropAction::Swap:
    case DropAction::Replace:
        if (done.pane == PaneId::Video) hidden.erase(PaneId::Video);
        // 换下来的是图传:和点叉删掉一样记一笔。
        if (done.action == DropAction::Replace && done.target == PaneId::Video) hidden.insert(PaneId::Video);
        if (last_removed_ == done.pane) last_removed_.reset();
        if (done.action == DropAction::Replace) last_removed_ = done.target;
        commit(*done.result, hidden);
        if (done.action == DropAction::Replace)
            showToast(QStringLiteral("「%1」换下来了，在组件库里").arg(library_title(*done.target, embeddable)), true);
        break;
    case DropAction::None:
        if (done.target) {
            drag_ = done;
            showToast(QStringLiteral("放不下：%1，换个大一点的地方").arg(dropProblem()), false);
            drag_ = Drag{};
        } else if (done.kind == DragKind::Move && library_.contains(done.mouse)) {
            showToast(QStringLiteral("画面里至少要留一块"), false);
        }
        break;
    }
    update();
}

void LayoutEditor::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.drawImage(QPoint(0, 0), theme::backdrop(size(), devicePixelRatioF()));
    const bool embeddable = embedded_host(layout_profile(role_), PaneId::Video).has_value();

    // 顶上一条和方案那一排。
    painter.fillRect(QRect(0, 0, width(), kHeaderHeight), QColor(6, 10, 18, 184));
    painter.fillRect(QRect(0, kHeaderHeight - 1, width(), 1), theme::kBorder);
    painter.fillRect(QRect(0, kHeaderHeight, width(), kStripHeight), QColor(6, 10, 18, 107));
    painter.fillRect(QRect(0, kHeaderHeight + kStripHeight - 1, width(), 1), theme::kRowBorder);
    if (tips_visible_) {
        // 方案那一排右边:四句怎么改。
        const struct {
            int kind;
            QString text;
        } tips[] = {{0, QStringLiteral("拖面板：换位置")}, {1, QStringLiteral("拖缝隙：改大小")},
                    {2, QStringLiteral("删掉，能撤销")}, {3, QStringLiteral("调这一块的细项")}};
        const int col_w = 168;
        const int x0 = width() - 16 - col_w * 2;
        for (int i = 0; i < 4; ++i) {
            const int x = x0 + (i % 2) * col_w;
            const int y = kHeaderHeight + 10 + (i / 2) * 22;
            const QRectF key(x, y, 18, 18);
            theme::paintGlass(painter, key, 3, false, QColor(8, 16, 28, 242), Qt::transparent);
            const QPointF c = key.center();
            if (tips[i].kind == 0) paint_dots(painter, c, theme::kAccentText);
            if (tips[i].kind == 1) paint_arrows(painter, c, true, theme::kTextPrimary);
            if (tips[i].kind == 2) paint_cross(painter, c, theme::kTextPrimary);
            if (tips[i].kind == 3) paint_gear(painter, c, theme::kTextPrimary);
            painter.setFont(theme::labelPx(12));
            painter.setPen(theme::kTextBright);
            painter.drawText(QRectF(x + 24, y, col_w - 28, 18), Qt::AlignLeft | Qt::AlignVCenter, tips[i].text);
        }
    }

    // 左边组件库:只放画面里没有的,每个是真样子的缩略图。
    theme::paintGlass(painter, QRectF(library_), 10, false, theme::kSurface, theme::kBorder, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(theme::kCyan);
    painter.drawPath(theme::skewBar(QRectF(library_.left() + 14, library_.top() + 12.5, 3, 15), -14));
    QFont card_title = theme::labelPx(15, QFont::Bold);
    card_title.setLetterSpacing(QFont::AbsoluteSpacing, 2);
    painter.setFont(card_title);
    painter.setPen(theme::kTextPrimary);
    painter.drawText(QRect(library_.left() + 26, library_.top(), 120, 40), Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("组件库"));
    painter.setFont(theme::labelPx(13));
    painter.setPen(theme::kTextSecondary);
    painter.drawText(QRect(library_.left(), library_.top(), library_.width() - 14, 40), Qt::AlignRight | Qt::AlignVCenter,
                     QStringLiteral("拖进画面就有"));
    painter.fillRect(QRect(library_.left() + 12, library_.top() + 39, library_.width() - 24, 1), theme::kBorderBright);
    const QRect view = libraryView();
    if (tiles_.empty()) {
        painter.setFont(theme::labelPx(13));
        painter.setPen(theme::kTextMuted);
        painter.drawText(view.adjusted(16, 8, -16, 0), Qt::TextWordWrap,
                         QStringLiteral("能放的都已经在画面里了。删掉一块，或者把画面里的拖到这里，它就回到这里。"));
    }
    painter.save();
    painter.setClipRect(view);
    for (const Tile& tile : tiles_) {
        const QRect box = tileOnScreen(tile);
        if (!box.intersects(view)) continue;
        const bool dragging = drag_.kind == DragKind::Add && drag_.pane == tile.pane && drag_.moved;
        // 上面一行名字,右边小字「刚删掉」「拖动中」。
        const QFont name_font = theme::labelPx(14, QFont::DemiBold);
        const QFont tag_font = theme::labelPx(12);
        const QRect caption(box.left() + 1, box.top(), box.width() - 2, kTileCaption - 4);
        const QString tag = dragging ? QStringLiteral("拖动中") : last_removed_ == tile.pane ? QStringLiteral("刚删掉") : QString();
        const QString name = library_title(tile.pane, embeddable);
        const int tag_w = tag.isEmpty() ? 0 : QFontMetrics(tag_font).horizontalAdvance(tag) + 6;
        const bool tag_fits = !tag.isEmpty() && QFontMetrics(name_font).horizontalAdvance(name) + tag_w <= caption.width();
        painter.setFont(name_font);
        painter.setPen(theme::kTextPrimary);
        painter.drawText(caption, Qt::AlignLeft | Qt::AlignVCenter,
                         QFontMetrics(name_font).elidedText(name, Qt::ElideRight, caption.width() - (tag_fits ? tag_w : 0)));
        if (tag_fits) {
            painter.setFont(tag_font);
            painter.setPen(theme::kAccentText);
            painter.drawText(caption, Qt::AlignRight | Qt::AlignVCenter, tag);
        }
        const QRect image = tile.image.translated(view.left(), view.top() - library_scroll_);
        painter.save();
        if (dragging) painter.setOpacity(0.35);
        painter.fillRect(image, QColor(0, 0, 0, 64));
        const auto found = tile_images_.find(tile.pane);
        if (found != tile_images_.end() && !found->second.isNull()) {
            // 花名册这类从主画面上截下来的,宽高比可能和格子不一样:等比缩小,靠左上放。
            const QSizeF fit = found->second.deviceIndependentSize().scaled(QSizeF(image.size()), Qt::KeepAspectRatio);
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            painter.drawPixmap(QRectF(QPointF(image.topLeft()), fit), found->second,
                               QRectF(QPointF(0, 0), found->second.size()));
        }
        painter.restore();
    }
    painter.restore();
    // 放不下就滚动:右边一条细滚动条,下面一道渐隐提示还有。
    if (library_content_ > view.height()) {
        const double shown = view.height() / static_cast<double>(library_content_);
        const int bar_h = std::max(24, static_cast<int>(view.height() * shown));
        const int most = library_content_ - view.height();
        const int bar_y = view.top() + static_cast<int>((view.height() - bar_h) * (library_scroll_ / static_cast<double>(most)));
        painter.fillRect(QRect(view.right() - 5, view.top(), 3, view.height()), QColor(255, 255, 255, 14));
        painter.fillRect(QRect(view.right() - 5, bar_y, 3, bar_h), QColor(196, 233, 251, 120));
        if (library_scroll_ < most) {
            QLinearGradient fade(0, view.bottom() - 28, 0, view.bottom());
            fade.setColorAt(0, QColor(13, 20, 34, 0));
            fade.setColorAt(1, QColor(13, 20, 34, 230));
            painter.fillRect(QRect(view.left() + 4, view.bottom() - 28, view.width() - 12, 28), fade);
        }
    }
    // 画面里的面板正被拖到这里:整块组件库亮起来,字在后面跟着鼠标的那一块之后画。
    const bool over_library = drag_.kind == DragKind::Move && drag_.moved && library_.contains(drag_.mouse);
    if (over_library) {
        const bool can = drag_.action == DropAction::Remove;
        painter.fillRect(QRectF(library_).adjusted(2, 2, -2, -2), can ? QColor(40, 120, 180, 110) : QColor(255, 100, 100, 50));
        painter.setPen(QPen(can ? theme::kCyan : theme::kDanger, 3));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(QRectF(library_).adjusted(1.5, 1.5, -1.5, -1.5));
    }

    // 中间:主界面信息页的真画面。
    painter.fillRect(canvas_, theme::kVoid);
    if (!preview_.isNull()) {
        painter.save();
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.drawPixmap(canvas_, preview_);
        painter.restore();
    }
    painter.setPen(QPen(theme::kBorder, 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(QRectF(canvas_).adjusted(-0.5, -0.5, 0.5, 0.5));

    const bool moving = (drag_.kind == DragKind::Move || drag_.kind == DragKind::Add) && drag_.moved;
    for (const PaneSlot& slot : working_.cells) {
        const QRect box = unitToCanvas(slot.units());
        const bool picked = details_pane_ == slot.pane && details_->isVisible();
        const bool lifted = moving && drag_.kind == DragKind::Move && drag_.pane == slot.pane;
        const bool hovered = !moving && hover_pane_ == slot.pane;
        painter.fillRect(box, QColor(142, 210, 242, picked ? 20 : hovered ? 12 : 0));
        if (picked) {
            painter.setPen(QPen(theme::kCyan, 3));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(QRectF(box).adjusted(1.5, 1.5, -1.5, -1.5));
        } else {
            dashed(painter, QRectF(box).adjusted(1, 1, -1, -1), QColor(142, 210, 242, lifted ? 220 : hovered ? 170 : 75),
                   lifted || hovered ? 2 : 1.2);
        }
        if (lifted) continue;
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(196, 233, 251, hovered ? 217 : 120));
        for (int row = 0; row < 2; ++row)
            for (int i = 0; i < 6; ++i) painter.drawEllipse(QPointF(box.center().x() - 12.5 + i * 5, box.top() + 6 + row * 4), 1.3, 1.3);
        for (const QRect& tool : {gearRect(slot.pane), closeRect(slot.pane)}) {
            if (tool.isNull()) continue;
            const bool gear = tool == gearRect(slot.pane);
            const bool on = gear && picked;
            const bool hot = hover_tool_ && *hover_tool_ == tool;
            if (on || hot)
                theme::paintGlass(painter, QRectF(tool), 4, false, on ? theme::kCyan : QColor(255, 255, 255, 36),
                                  Qt::transparent);
            if (gear) paint_gear(painter, QRectF(tool).center(), on ? QColor(4, 18, 28) : theme::kTextBright);
            else paint_cross(painter, QRectF(tool).center(), hot ? theme::kDangerText : theme::kTextBright);
        }
    }

    // 缝上的小把手;指着的那条整条亮起来,旁边写怎么用。
    if (!moving) {
        const std::optional<Seam> hot = drag_.kind == DragKind::Seam
                                            ? std::optional<Seam>(drag_.seam)
                                            : hover_seam_;
        for (const Seam& seam : all_seams(working_.cells)) {
            const QRect line = seamRect(seam);
            const QPointF c = QRectF(line).center();
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(196, 233, 251, 95));
            if (seam.vertical) painter.drawRoundedRect(QRectF(c.x() - 2, c.y() - 16, 4, 32), 2, 2);
            else painter.drawRoundedRect(QRectF(c.x() - 16, c.y() - 2, 32, 4), 2, 2);
        }
        if (hot) {
            Seam seam = *hot;
            if (drag_.kind == DragKind::Seam) seam.line = drag_.line;
            const QRect line = seamRect(seam);
            painter.fillRect(line, theme::kCyan);
            const QPointF c = QRectF(line).center();
            const QRectF knob = seam.vertical ? QRectF(c.x() - 13, c.y() - 22, 26, 44) : QRectF(c.x() - 22, c.y() - 13, 44, 26);
            theme::paintGlass(painter, knob, 4, false, theme::kCyan, Qt::transparent);
            paint_arrows(painter, knob.center(), seam.vertical, QColor(4, 18, 28));
            if (drag_.kind != DragKind::Seam) {
                const QString tip = seam.vertical ? QStringLiteral("拖这条缝：一边变宽，一边变窄")
                                                  : QStringLiteral("拖这条缝：一边变高，一边变矮");
                QFont font = theme::labelPx(14, QFont::Bold);
                const qreal tw = QFontMetricsF(font).horizontalAdvance(tip) + 24;
                QRectF box = seam.vertical ? QRectF(knob.right() + 8, c.y() - 15, tw, 30) : QRectF(c.x() + 30, knob.top() - 2, tw, 30);
                if (box.right() > width() - 12) box.moveRight(seam.vertical ? knob.left() - 8 : c.x() - 30);
                theme::paintGlass(painter, box, 4, false, theme::kCyan, Qt::transparent);
                painter.setFont(font);
                painter.setPen(QColor(4, 18, 28));
                painter.drawText(box, Qt::AlignCenter, tip);
            }
        }
    }

    // 正在拖:松手以后它会在哪(能放是蓝框,放不下是红框),旁边谁让出地方、原来的地方谁补。
    if (moving) {
        // 字放不下就缩小一号,还放不下就省略,不画出框。
        const auto fitted = [&painter](const QRect& box, const QString& text, int px, int min_px, int weight,
                                       const QColor& ink) {
            QFont font = theme::labelPx(px, weight);
            while (px > min_px && QFontMetrics(font).horizontalAdvance(text) > box.width()) font = theme::labelPx(--px, weight);
            painter.setFont(font);
            painter.setPen(ink);
            painter.drawText(box, Qt::AlignCenter, QFontMetrics(font).elidedText(text, Qt::ElideRight, box.width()));
        };
        // 字最后画,压在跟着鼠标的那一块上面,拖到正中间时也看得见「两块换位置」。
        std::vector<std::function<void()>> labels;
        const auto label = [&](const QRect& box, const QString& big, const QString& small, const QColor& ink) {
            labels.push_back([=, &fitted] {
                fitted(QRect(box.left() + 6, box.center().y() - 26, box.width() - 12, 26), big, 18, 12, QFont::Bold, ink);
                if (!small.isEmpty())
                    fitted(QRect(box.left() + 6, box.center().y() + 4, box.width() - 12, 22), small, 13, 11,
                           QFont::Normal, theme::kAccentText);
            });
        };
        const auto find = [&](PaneId pane) -> QRect {
            if (!drag_.result) return {};
            for (const PaneSlot& slot : *drag_.result)
                if (slot.pane == pane) return unitToCanvas(slot.units());
            return {};
        };
        const auto mark = [&](const QRect& box, bool good) {
            painter.fillRect(box, good ? QColor(40, 120, 180, 92) : QColor(255, 100, 100, 40));
            painter.setPen(QPen(good ? theme::kCyan : theme::kDanger, 3));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(QRectF(box).adjusted(1.5, 1.5, -1.5, -1.5));
        };
        // 画面里的面板挪走以后,它原来的地方由旁边补上。
        const auto old_place = [&](const QRect& placed, const QRect& other) {
            if (drag_.kind != DragKind::Move) return;
            const QRect old = unitToCanvas(cell(drag_.pane)->units());
            if (old.intersects(placed) || old.intersects(other)) return;
            painter.fillRect(old, QColor(4, 8, 14, 170));
            dashed(painter, QRectF(old).adjusted(2, 2, -2, -2), QColor(142, 210, 242, 140), 2);
            label(old, QStringLiteral("原来的位置"), QStringLiteral("松手后旁边的面板补过来"), theme::kTextBright);
        };
        if (over_library) {
            // 字放在鼠标上面一点,不让跟着鼠标的那一块挡住。
            const bool can = drag_.action == DropAction::Remove;
            const int y = std::clamp(drag_.mouse.y() - 96, library_.top() + 48, library_.bottom() - 70);
            const QRect band(library_.left() + 8, y, library_.width() - 16, 62);
            labels.push_back([=, &painter] {
                theme::paintGlass(painter, QRectF(band), 6, false, QColor(6, 12, 22, 230), can ? theme::kCyan : theme::kDanger);
            });
            label(band.translated(0, 4), can ? QStringLiteral("松手放回组件库") : QStringLiteral("至少要留一块"),
                  can ? QStringLiteral("从画面里拿掉，能撤销") : QString(), can ? QColor(Qt::white) : theme::kDangerText);
        }
        if (drag_.target) {
            const QString target_name = pane_title(*drag_.target);
            const QRect target_box = unitToCanvas(cell(*drag_.target)->units());
            switch (drag_.action) {
            case DropAction::Insert: {
                const QRect placed = find(drag_.pane);
                const QRect target_after = find(*drag_.target);
                // 两块大小都没变,只是调了个先后(比如图传拖到本车上)。
                const auto units_after = [&](PaneId pane) {
                    for (const PaneSlot& slot : *drag_.result)
                        if (slot.pane == pane) return slot.units();
                    return QRect();
                };
                const bool reorder = drag_.kind == DragKind::Move &&
                                     units_after(*drag_.target).size() == cell(*drag_.target)->units().size() &&
                                     units_after(drag_.pane).size() == cell(drag_.pane)->units().size();
                painter.fillRect(target_after, QColor(4, 8, 14, 120));
                label(target_after,
                      (reorder ? QStringLiteral("「%1」挪到这里") : QStringLiteral("「%1」让出一块")).arg(target_name), QString(),
                      theme::kTextPrimary);
                old_place(placed, target_after);
                mark(placed, true);
                label(placed, QStringLiteral("松手放在这里"),
                      reorder ? QStringLiteral("两块调个先后，大小不变") : QStringLiteral("旁边自动让开"), QColor(Qt::white));
                break;
            }
            case DropAction::Swap: {
                const QRect placed = find(drag_.pane);
                const QRect target_after = find(*drag_.target);
                painter.fillRect(target_after, QColor(4, 8, 14, 150));
                dashed(painter, QRectF(target_after).adjusted(2, 2, -2, -2), theme::kCyan, 2);
                label(target_after, QStringLiteral("「%1」换到这里").arg(target_name), QString(), theme::kTextPrimary);
                mark(placed, true);
                label(placed, QStringLiteral("松手放在这里"), QStringLiteral("两块换位置"), QColor(Qt::white));
                break;
            }
            case DropAction::Replace:
                mark(target_box, true);
                label(target_box, QStringLiteral("松手替换「%1」").arg(target_name),
                      QStringLiteral("「%1」回到组件库").arg(target_name), QColor(Qt::white));
                break;
            case DropAction::Embed:
                if (drag_.kind == DragKind::Move) old_place(target_box, QRect());
                mark(target_box, true);
                label(target_box, QStringLiteral("松手放回「本车」里"), QStringLiteral("变回本车下面的小窗"), QColor(Qt::white));
                break;
            case DropAction::None:
            case DropAction::Remove:
                mark(target_box, false);
                label(target_box, QStringLiteral("放不下"), dropProblem(), theme::kDangerText);
                break;
            }
        }
        // 跟着鼠标的那一块。
        QPixmap ghost;
        QSize ghost_size;
        if (drag_.kind == DragKind::Move && !preview_.isNull()) {
            const QRect from = unitToCanvas(cell(drag_.pane)->units());
            const double sx = preview_.width() / static_cast<double>(canvas_.width());
            const double sy = preview_.height() / static_cast<double>(canvas_.height());
            ghost = preview_.copy(QRect(static_cast<int>((from.left() - canvas_.left()) * sx),
                                        static_cast<int>((from.top() - canvas_.top()) * sy),
                                        static_cast<int>(from.width() * sx), static_cast<int>(from.height() * sy)));
            ghost_size = from.size() * 0.55;
        } else if (const auto found = tile_images_.find(drag_.pane); found != tile_images_.end()) {
            ghost = found->second;
            ghost_size = found->second.deviceIndependentSize().toSize().scaled(QSize(200, 140), Qt::KeepAspectRatio);
        }
        if (!ghost.isNull()) {
            const QRect box(drag_.mouse - QPoint(ghost_size.width() / 2, 14), ghost_size);
            painter.save();
            painter.setOpacity(0.85);
            for (int i = 1; i <= 3; ++i) painter.fillRect(box.translated(0, 5 * i), QColor(0, 0, 0, 34));
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            painter.drawPixmap(box, ghost);
            painter.setPen(QPen(theme::kCyan, 2));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(QRectF(box).adjusted(1, 1, -1, -1));
            painter.restore();
        }
        for (const auto& draw : labels) draw();
    }
}

}  // namespace rm_terminal
