#include "pane_details.h"

#include "config.h"
#include "next_step.h"
#include "theme.h"
#include "ui_profile_store.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QVBoxLayout>
#include <algorithm>

namespace rm_terminal {

namespace {

QString details_style() {
    const QString text = theme::css(theme::kTextPrimary);
    const QString secondary = theme::css(theme::kTextSecondary);
    const QString muted = theme::css(theme::kTextMuted);
    const QString border = theme::css(theme::kBorder);
    const QString row_border = theme::css(theme::kRowBorder);
    const QString accent = theme::css(theme::kCyan);
    const QString selected = theme::css(QColor(64, 170, 230, 107));
    const QString chip = theme::css(QColor(0, 0, 0, 77));
    return QStringLiteral(
               "QFrame#paneDetails{background:transparent; border:none;}"
               "QFrame#paneDetails QLabel{background:transparent; border:none; color:%1;}"
               "QFrame#paneDetails QLabel[hint=\"true\"]{color:%2;}"
               "QFrame#paneDetails QLabel[heading=\"true\"]{color:%2; font-weight:700; letter-spacing:1px;"
               " padding:8px 0px 2px 0px; border-top:1px solid %5;}"
               "QFrame#paneDetails QPushButton[segment=\"true\"]{background:%8; color:%2; border:none; padding:7px 12px;}"
               "QFrame#paneDetails QPushButton[segment=\"true\"]:checked{background:%7; color:#ffffff; font-weight:bold;}"
               "QFrame#paneDetails QPushButton[segment=\"true\"]:disabled{color:%3;}"
               "QFrame#paneDetails QListWidget{background:transparent; color:%1; border:none; outline:none;}"
               "QFrame#paneDetails QListWidget::item{padding:6px 8px; margin-bottom:4px; background:rgba(255,255,255,10);}"
               "QFrame#paneDetails QListWidget::item:selected{background:%7;}"
               "QFrame#paneDetails QCheckBox{color:%1; spacing:10px; padding:3px 0px;}"
               "QFrame#paneDetails QCheckBox:disabled{color:%3;}"
               "QFrame#paneDetails QSlider::groove:horizontal{height:4px; background:%4;}"
               "QFrame#paneDetails QSlider::sub-page:horizontal{background:%6;}"
               "QFrame#paneDetails QSlider::handle:horizontal{width:12px; margin:-5px 0; background:%1;}")
        .arg(text, secondary, muted, border, row_border, accent, selected, chip);
}

QLabel* hint(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setProperty("hint", true);
    label->setWordWrap(true);
    label->setFont(theme::labelPx(13));
    return label;
}

// 几选一的一排按钮(「一直显示 / 有急事才弹出 / 关掉」)。
QButtonGroup* segments(QWidget* parent, QVBoxLayout* column, const QString& label, const QStringList& options,
                       const QString& name) {
    auto* row = new QHBoxLayout();
    row->setSpacing(2);
    auto* title = new QLabel(label, parent);
    title->setMinimumWidth(104);
    title->setFont(theme::labelPx(14));
    row->addWidget(title);
    auto* group = new QButtonGroup(parent);
    group->setExclusive(true);
    for (int i = 0; i < options.size(); ++i) {
        auto* button = new QPushButton(options[i], parent);
        button->setCheckable(true);
        button->setProperty("segment", true);
        button->setObjectName(QStringLiteral("%1_%2").arg(name).arg(i));
        button->setFont(theme::labelPx(13));
        group->addButton(button, i);
        row->addWidget(button);
    }
    row->addStretch(1);
    column->addLayout(row);
    return group;
}

QWidget* add_page(QWidget* parent, QVBoxLayout* holder, std::vector<QWidget*>* pages, QVBoxLayout** layout) {
    auto* widget = new QWidget(parent);
    *layout = new QVBoxLayout(widget);
    (*layout)->setContentsMargins(0, 0, 0, 0);
    (*layout)->setSpacing(8);
    widget->setVisible(false);
    holder->addWidget(widget);
    pages->push_back(widget);
    return widget;
}

QString page_title(PaneDetails::Page page) {
    switch (page) {
    case PaneDetails::Page::Map: return QStringLiteral("战术地图 · 细项");
    case PaneDetails::Page::EnemyRoster: return QStringLiteral("敌方机器人 · 细项");
    case PaneDetails::Page::NextStep: return QStringLiteral("下一步 · 细项");
    case PaneDetails::Page::Analysis: return QStringLiteral("战场数据分析 · 细项");
    case PaneDetails::Page::OwnVideo: return QStringLiteral("本车 · 细项");
    case PaneDetails::Page::Video: return QStringLiteral("图传 · 细项");
    case PaneDetails::Page::Alerts: return QStringLiteral("事件提醒");
    }
    return {};
}

}  // namespace

PaneDetails::PaneDetails(UiProfileController* controller, QWidget* parent) : QFrame(parent), controller_(controller) {
    setObjectName(QStringLiteral("paneDetails"));
    setStyleSheet(details_style());
    setFixedWidth(452);
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 0, 16, 14);
    root->setSpacing(6);
    auto* header = new QHBoxLayout();
    title_ = new QLabel(this);
    title_->setFont(theme::labelPx(15, QFont::Bold));
    header->addWidget(title_, 1);
    header->addWidget(hint(QStringLiteral("改了马上生效"), this));
    auto* header_box = new QWidget(this);
    header_box->setFixedHeight(42);
    header_box->setLayout(header);
    header->setContentsMargins(10, 0, 0, 0);
    root->addWidget(header_box);

    QVBoxLayout* l = nullptr;
    add_page(this, root, &pages_, &l);  // Map
    {
        auto* row = new QHBoxLayout();
        auto* label = new QLabel(QStringLiteral("底图亮度"), this);
        label->setMinimumWidth(104);
        row->addWidget(label);
        map_background_ = new QSlider(Qt::Horizontal, this);
        map_background_->setObjectName(QStringLiteral("uiMapBackground"));
        map_background_->setRange(kMinMapBackgroundPercent, kMaxMapBackgroundPercent);
        map_background_->setSingleStep(5);
        map_background_->setPageStep(10);
        row->addWidget(map_background_, 1);
        map_background_value_ = new QLabel(this);
        map_background_value_->setMinimumWidth(44);
        map_background_value_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row->addWidget(map_background_value_);
        l->addLayout(row);
        map_enemies_ = new QCheckBox(QStringLiteral("显示敌方机器人"), this);
        map_marks_ = new QCheckBox(QStringLiteral("显示地图标记"), this);
        l->addWidget(map_enemies_);
        l->addWidget(map_marks_);
        l->addWidget(hint(QStringLiteral("藏起敌方机器人只是地图上不画，敌方花名册照常。"), this));
    }
    add_page(this, root, &pages_, &l);  // EnemyRoster
    low_hp_pulse_ = new QCheckBox(QStringLiteral("敌方残血时闪烁"), this);
    l->addWidget(low_hp_pulse_);
    l->addWidget(hint(QStringLiteral("关掉后残血的敌人血条照样变红，只是不闪。"), this));
    add_page(this, root, &pages_, &l);  // NextStep
    next_mode_ = segments(this, l, QStringLiteral("出现方式"),
                          {QStringLiteral("一直显示"), QStringLiteral("有急事才弹出"), QStringLiteral("关掉")},
                          QStringLiteral("nextMode"));
    l->addWidget(hint(QStringLiteral("有急事才弹出：平时不占地方，阵亡、堡垒被偷、基地被打时才弹出来，处理完自动收起。"), this));
    next_count_ = segments(this, l, QStringLiteral("显示几件"), {QStringLiteral("1 件"), QStringLiteral("2 件"), QStringLiteral("3 件")},
                           QStringLiteral("nextCount"));
    next_size_ = segments(this, l, QStringLiteral("卡片大小"), {QStringLiteral("紧凑"), QStringLiteral("标准"), QStringLiteral("大")},
                          QStringLiteral("nextSize"));
    next_voice_ = new QCheckBox(QStringLiteral("第 1 件念出来"), this);
    next_voice_->setObjectName(QStringLiteral("nextVoice"));
    l->addWidget(next_voice_);
    {
        auto* row = new QHBoxLayout();
        auto* label = new QLabel(QStringLiteral("每张至少停留"), this);
        label->setMinimumWidth(104);
        row->addWidget(label);
        next_dwell_ = new QSlider(Qt::Horizontal, this);
        next_dwell_->setObjectName(QStringLiteral("nextDwell"));
        next_dwell_->setRange(1, 10);
        row->addWidget(next_dwell_, 1);
        next_dwell_value_ = new QLabel(this);
        next_dwell_value_->setMinimumWidth(44);
        next_dwell_value_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row->addWidget(next_dwell_value_);
        l->addLayout(row);
    }
    add_page(this, root, &pages_, &l);  // Analysis
    analysis_mode_ = segments(this, l, QStringLiteral("显示内容"), {QStringLiteral("全部"), QStringLiteral("只看控制度")},
                              QStringLiteral("analysisMode"));
    l->addWidget(hint(QStringLiteral("只看控制度：阵地、经济、伤害这些先不显示，控制度数字放大，一眼看清谁领先。"), this));
    add_page(this, root, &pages_, &l);  // OwnVideo
    own_video_ = new QCheckBox(QStringLiteral("下面放图传小窗"), this);
    own_video_->setObjectName(QStringLiteral("ownVideo"));
    l->addWidget(own_video_);
    l->addWidget(hint(QStringLiteral("关掉后指令占满整块。想要大的图传，从左边组件库把「图传（大窗口）」拖进画面。"), this));
    add_page(this, root, &pages_, &l);  // Video
    video_back_ = new QPushButton(QStringLiteral("放回「本车」里，变回小窗"), this);
    video_back_->setObjectName(QStringLiteral("videoBack"));
    video_back_->setProperty("segment", true);
    l->addWidget(video_back_);
    l->addWidget(hint(QStringLiteral("放回去以后，这块地方由旁边的面板补上。"), this));
    add_page(this, root, &pages_, &l);  // Alerts
    alert_voice_ = new QCheckBox(QStringLiteral("提醒时念出来"), this);
    l->addWidget(alert_voice_);
    auto* kinds = new QLabel(QStringLiteral("要提醒的事"), this);
    kinds->setProperty("heading", true);
    l->addWidget(kinds);
    for (const QString& kind : alert_kind_names()) {
        auto* box = new QCheckBox(alert_kind_title(kind), this);
        alert_kinds_.emplace_back(kind, box);
        l->addWidget(box);
        connect(box, &QCheckBox::toggled, this, [this, kind](bool on) {
            editOptions([&](UiOptions& options) {
                options.alerts_muted.removeAll(kind);
                if (!on) options.alerts_muted.append(kind);
            });
        });
    }
    l->addWidget(hint(QStringLiteral("关掉的事不出横幅也不念。战术提醒在状态条的「战术提醒」里改。"), this));

    connect(map_background_, &QSlider::valueChanged, this, [this](int value) {
        map_background_value_->setText(QStringLiteral("%1%").arg(value));
        if (!map_background_->isSliderDown())
            editOptions([value](UiOptions& options) { options.map_background_percent = value; });
    });
    connect(map_background_, &QSlider::sliderReleased, this, [this] {
        const int value = map_background_->value();
        editOptions([value](UiOptions& options) { options.map_background_percent = value; });
    });
    connect(map_enemies_, &QCheckBox::toggled, this,
            [this](bool on) { editOptions([on](UiOptions& options) { options.map_show_enemies = on; }); });
    connect(map_marks_, &QCheckBox::toggled, this,
            [this](bool on) { editOptions([on](UiOptions& options) { options.map_show_marks = on; }); });
    connect(low_hp_pulse_, &QCheckBox::toggled, this,
            [this](bool on) { editOptions([on](UiOptions& options) { options.enemy_low_hp_pulse = on; }); });
    connect(alert_voice_, &QCheckBox::toggled, this,
            [this](bool on) { editOptions([on](UiOptions& options) { options.alert_voice = on; }); });
    connect(next_mode_, &QButtonGroup::idClicked, this, [this](int id) {
        if (id == 2) {
            Q_EMIT removePaneRequested(PaneId::NextStep);
            return;
        }
        editOptions([id](UiOptions& options) { options.next_mode = id; });
    });
    connect(next_count_, &QButtonGroup::idClicked, this,
            [this](int id) { editOptions([id](UiOptions& options) { options.next_count = id + 1; }); });
    connect(next_size_, &QButtonGroup::idClicked, this,
            [this](int id) { editOptions([id](UiOptions& options) { options.next_size = id; }); });
    connect(next_voice_, &QCheckBox::toggled, this,
            [this](bool on) { editOptions([on](UiOptions& options) { options.next_voice = on; }); });
    connect(next_dwell_, &QSlider::valueChanged, this, [this](int value) {
        next_dwell_value_->setText(QStringLiteral("%1 秒").arg(value));
        if (!next_dwell_->isSliderDown()) editOptions([value](UiOptions& options) { options.next_dwell_sec = value; });
    });
    connect(next_dwell_, &QSlider::sliderReleased, this, [this] {
        const int value = next_dwell_->value();
        editOptions([value](UiOptions& options) { options.next_dwell_sec = value; });
    });
    connect(analysis_mode_, &QButtonGroup::idClicked, this,
            [this](int id) { editOptions([id](UiOptions& options) { options.analysis_control_only = id == 1; }); });
    connect(own_video_, &QCheckBox::toggled, this, [this](bool on) {
        Q_EMIT aboutToChange();
        controller_->setPaneHidden(PaneId::Video, !on);
    });
    connect(video_back_, &QPushButton::clicked, this, &PaneDetails::videoBackRequested);
    // 排队连接:点开关的那一下也会改设置,当场重建「先后顺序」会删掉正在发信号的那一行。
    connect(controller_, &UiProfileController::changed, this, &PaneDetails::refresh, Qt::QueuedConnection);
    refresh();
}

std::optional<PaneDetails::Page> PaneDetails::page_for(PaneId pane, bool video_cell, bool video_embeddable) {
    switch (pane) {
    case PaneId::Map: return Page::Map;
    case PaneId::EnemyRoster: return Page::EnemyRoster;
    case PaneId::NextStep: return Page::NextStep;
    case PaneId::Analysis: return Page::Analysis;
    case PaneId::SelfCommands:
        if (!video_cell && video_embeddable) return Page::OwnVideo;
        return std::nullopt;
    case PaneId::Video:
        if (video_embeddable) return Page::Video;
        return std::nullopt;
    default: break;
    }
    return std::nullopt;
}

void PaneDetails::showPage(Page page) {
    page_ = page;
    title_->setText(page_title(page));
    for (std::size_t i = 0; i < pages_.size(); ++i) pages_[i]->setVisible(i == static_cast<std::size_t>(page));
    // 高度跟着这一页走:没有空白的下半截。
    layout()->activate();
    adjustSize();
}

void PaneDetails::setConfigDisabledAlerts(const QStringList& kinds) {
    config_disabled_ = kinds;
    refresh();
}

void PaneDetails::editOptions(const std::function<void(UiOptions&)>& edit) {
    UiOptions options = controller_->activeScheme().options;
    edit(options);
    if (options == controller_->activeScheme().options) return;
    Q_EMIT aboutToChange();
    controller_->setOptions(options);
}

void PaneDetails::refresh() {
    const UiScheme& scheme = controller_->activeScheme();
    const UiOptions& options = scheme.options;
    // 「先后顺序」的列表不能拦信号:拦了以后列表收不到「多了几行」,重排后整块是空的。
    // 清空、加行不会发 rowsMoved,不会被当成用户拖了一行。
    const QSignalBlocker a(map_background_), b(map_enemies_), c(map_marks_), d(low_hp_pulse_), e(alert_voice_),
        f(next_voice_), g(next_dwell_), i(own_video_);
    map_background_->setValue(options.map_background_percent);
    map_background_value_->setText(QStringLiteral("%1%").arg(options.map_background_percent));
    map_enemies_->setChecked(options.map_show_enemies);
    map_marks_->setChecked(options.map_show_marks);
    low_hp_pulse_->setChecked(options.enemy_low_hp_pulse);
    alert_voice_->setChecked(options.alert_voice);
    next_mode_->button(std::clamp(options.next_mode, 0, 1))->setChecked(true);
    next_count_->button(std::clamp(options.next_count, 1, 3) - 1)->setChecked(true);
    next_size_->button(std::clamp(options.next_size, 0, 2))->setChecked(true);
    next_voice_->setChecked(options.next_voice);
    next_dwell_->setValue(options.next_dwell_sec);
    next_dwell_value_->setText(QStringLiteral("%1 秒").arg(options.next_dwell_sec));
    analysis_mode_->button(options.analysis_control_only ? 1 : 0)->setChecked(true);
    own_video_->setChecked(!scheme.hidden.count(PaneId::Video));
    for (auto& [kind, box] : alert_kinds_) {
        const QSignalBlocker blocker(box);
        const bool by_config = config_disabled_.contains(kind);
        box->setEnabled(!by_config);
        box->setText(alert_kind_title(kind) + (by_config ? QStringLiteral("（配置文件已关）") : QString()));
        box->setChecked(!by_config && !options.alerts_muted.contains(kind));
    }
}

void PaneDetails::paintEvent(QPaintEvent* event) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    theme::paintGlass(painter, QRectF(rect()), 10, false, theme::kOverlay, theme::kCyan, true);
    QFrame::paintEvent(event);
}

}  // namespace rm_terminal
