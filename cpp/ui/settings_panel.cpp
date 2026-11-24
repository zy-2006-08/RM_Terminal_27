#include "settings_panel.h"

#include "glass.h"
#include "theme.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QVBoxLayout>

#include <algorithm>

namespace rm_terminal {

namespace {

constexpr int kWidth = 1180;
constexpr int kHeight = 470;

QLabel* text_label(QWidget* parent, const QString& text, const QColor& ink, int px = 13,
                   int weight = QFont::Medium) {
    auto* label = new QLabel(text, parent);
    label->setFont(theme::labelPx(px, weight));
    label->setStyleSheet(QStringLiteral("color:%1;").arg(theme::css(ink)));
    return label;
}

GlassButton* toggle_button(QWidget* parent, const QString& text, const char* name) {
    auto* button = new GlassButton(text, parent);
    button->setObjectName(QString::fromLatin1(name));
    button->setCheckable(true);
    return button;
}

GlassCard* block(QWidget* parent, const QString& title) {
    auto* card = new GlassCard(title, parent);
    card->setFill(QColor(255, 255, 255, 6));
    card->body()->setSpacing(10);
    return card;
}

QHBoxLayout* row(GlassCard* card, const QString& name) {
    auto* line = new QHBoxLayout;
    line->setSpacing(6);
    line->addWidget(text_label(card, name, theme::kTextBright));
    line->addStretch(1);
    card->body()->addLayout(line);
    return line;
}

// 终端做不到的一项照样放在官方的位置:灰着,鼠标指上去写原因。
void unavailable_row(GlassCard* card, const QString& name, const QString& value, const QString& why) {
    auto* line = new QHBoxLayout;
    QLabel* label = text_label(card, name, theme::kTextMuted);
    QLabel* text = text_label(card, value, theme::kTextMuted, 12);
    label->setToolTip(why);
    text->setToolTip(why);
    line->addWidget(label);
    line->addStretch(1);
    line->addWidget(text);
    card->body()->addLayout(line);
}

// 点在滑条上哪里就跳到哪里(QSlider 默认只挪一页,细滑条还很难点中),然后照常按住拖。
class JumpSlider final : public QSlider {
public:
    using QSlider::QSlider;

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && isEnabled()) {
            QStyleOptionSlider option;
            initStyleOption(&option);
            const QRect handle = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);
            if (!handle.contains(event->position().toPoint())) {
                const int span = std::max(1, width() - handle.width());
                const int x = static_cast<int>(event->position().x()) - handle.width() / 2;
                setValue(QStyle::sliderValueFromPosition(minimum(), maximum(), std::clamp(x, 0, span), span));
            }
        }
        QSlider::mousePressEvent(event);
    }
};

QSlider* slider(QWidget* parent, const char* name, int low, int high) {
    auto* bar = new JumpSlider(Qt::Horizontal, parent);
    bar->setObjectName(QString::fromLatin1(name));
    bar->setRange(low, high);
    bar->setFixedWidth(130);
    // 不拿键盘焦点:面板开着时按 P 关面板,方向键也不该拨它。
    bar->setFocusPolicy(Qt::NoFocus);
    bar->setStyleSheet(QStringLiteral("QSlider::groove:horizontal{height:4px;background:%1;}"
                                      "QSlider::sub-page:horizontal{background:%2;}"
                                      "QSlider::handle:horizontal{width:12px;margin:-5px 0;background:%3;}"
                                      "QSlider::sub-page:horizontal:disabled{background:%4;}"
                                      "QSlider::handle:horizontal:disabled{background:%4;}")
                           .arg(theme::css(theme::kRowBorder), theme::css(theme::kCyan),
                                theme::css(theme::kTextPrimary), theme::css(theme::kTextMuted)));
    return bar;
}

void set_checked(QPushButton* button, bool on) {
    if (button->isChecked() != on) button->setChecked(on);
}

// 面板每秒刷新 4 次:颜色没变就不重设样式表,免得每次都重排版。
void set_ink(QLabel* label, const QColor& ink) {
    const QString style = QStringLiteral("color:%1;").arg(theme::css(ink));
    if (label->styleSheet() != style) label->setStyleSheet(style);
}

void set_slider(QSlider* bar, int value) {
    if (bar->value() == value) return;
    const bool blocked = bar->blockSignals(true);
    bar->setValue(value);
    bar->blockSignals(blocked);
}

}  // namespace

SettingsPanel::SettingsPanel(QWidget* host) : QFrame(host) {
    setObjectName(QStringLiteral("settingsPanel"));
    setStyleSheet(QStringLiteral("QFrame#settingsPanel{background:transparent; border:none;}"
                                 "QFrame#settingsPanel QLabel{background:transparent; border:none;}"));
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(20, 14, 20, 14);
    column->setSpacing(12);

    auto* header = new QHBoxLayout;
    header->setSpacing(10);
    header->addWidget(text_label(this, QStringLiteral("设置面板"), theme::kTextPrimary, 18, QFont::Bold));
    header->addSpacing(24);
    reset_button_ = new GlassButton(QStringLiteral("一键重置"), this);
    reset_button_->setObjectName(QStringLiteral("settingsReset"));
    header->addWidget(reset_button_);
    header->addWidget(text_label(this, QStringLiteral("一键重置所有设置项，请谨慎使用"), theme::kTextSecondary, 12));
    header->addStretch(1);
    header->addWidget(text_label(this, QStringLiteral("P 键关闭面板"), theme::kTextMuted, 12));
    column->addLayout(header);

    auto* blocks = new QHBoxLayout;
    blocks->setSpacing(12);

    GlassCard* login = block(this, QStringLiteral("登录"));
    seat_ = text_label(login, QString(), theme::kTextPrimary, 15, QFont::Bold);
    login_ = text_label(login, QString(), theme::kTextBright);
    login_->setWordWrap(true);
    QLabel* login_note = text_label(login, QStringLiteral("换车要关掉终端重新打开：终端连服务器用的名字就是机器人编号"),
                                    theme::kTextMuted, 12);
    login_note->setWordWrap(true);
    login->body()->addWidget(seat_);
    login->body()->addWidget(login_);
    login->body()->addWidget(login_note);
    login->body()->addStretch(1);
    blocks->addWidget(login, 3);

    auto* middle = new QVBoxLayout;
    middle->setSpacing(12);
    GlassCard* performance = block(this, QStringLiteral("性能设置"));
    QHBoxLayout* chassis = row(performance, QStringLiteral("底盘类型"));
    chassis_hp_ = toggle_button(performance, QStringLiteral("血量优先"), "settingsChassisHp");
    chassis_power_ = toggle_button(performance, QStringLiteral("功率优先"), "settingsChassisPower");
    chassis->addWidget(chassis_hp_);
    chassis->addWidget(chassis_power_);
    QHBoxLayout* shooter = row(performance, QStringLiteral("发射机构类型"));
    shooter_cooling_ = toggle_button(performance, QStringLiteral("冷却优先"), "settingsShooterCooling");
    shooter_burst_ = toggle_button(performance, QStringLiteral("爆发优先"), "settingsShooterBurst");
    shooter->addWidget(shooter_cooling_);
    shooter->addWidget(shooter_burst_);
    performance_note_ = text_label(performance, QString(), theme::kTextSecondary, 12);
    performance_note_->setObjectName(QStringLiteral("settingsPerformanceNote"));
    performance_note_->setWordWrap(true);
    performance->body()->addWidget(performance_note_);
    middle->addWidget(performance);

    GlassCard* video = block(this, QStringLiteral("图传"));
    QHBoxLayout* direction = row(video, QStringLiteral("图传方向"));
    video_forward_ = toggle_button(video, QStringLiteral("正向"), "settingsVideoForward");
    video_reverse_ = toggle_button(video, QStringLiteral("反向"), "settingsVideoReverse");
    video_reverse_->setToolTip(QStringLiteral("画面转 180°，相机倒着装时用"));
    direction->addWidget(video_forward_);
    direction->addWidget(video_reverse_);
    unavailable_row(video, QStringLiteral("图传解码方式"), QStringLiteral("软件解码"),
                    QStringLiteral("终端只用软件解码，没有别的可选"));
    unavailable_row(video, QStringLiteral("信道配置"), QStringLiteral("终端不用配"),
                    QStringLiteral("终端的图传是服务器从网线发来的（UDP 3334），不经过图传接收机，没有信道要配"));
    middle->addWidget(video);
    blocks->addLayout(middle, 4);

    GlassCard* hardware = block(this, QStringLiteral("硬件设置 / 图形设置"));
    QHBoxLayout* sense = row(hardware, QStringLiteral("控制灵敏度"));
    sensitivity_ = slider(hardware, "settingsSensitivity", 1, 100);
    sensitivity_->setToolTip(QStringLiteral("键鼠控制时鼠标移动乘的倍数：50 是原样，100 是两倍"));
    sensitivity_value_ = text_label(hardware, QString(), theme::kTextPrimary, 13, QFont::Bold);
    sensitivity_value_->setFixedWidth(34);
    sensitivity_value_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    sense->addWidget(sensitivity_);
    sense->addWidget(sensitivity_value_);
    QHBoxLayout* loud = row(hardware, QStringLiteral("音量设置"));
    volume_ = slider(hardware, "settingsVolume", 0, 100);
    volume_value_ = text_label(hardware, QString(), theme::kTextPrimary, 13, QFont::Bold);
    volume_value_->setFixedWidth(34);
    volume_value_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    loud->addWidget(volume_);
    loud->addWidget(volume_value_);
    unavailable_row(hardware, QStringLiteral("背景音量设置"), QStringLiteral("终端没有"),
                    QStringLiteral("终端没有背景音乐，只有提醒和语音，音量在上一行"));
    unavailable_row(hardware, QStringLiteral("垂直同步"), QStringLiteral("终端用不上"),
                    QStringLiteral("终端的画面不按帧率刷新，这一项用不上"));
    unavailable_row(hardware, QStringLiteral("帧率设置"), QStringLiteral("终端用不上"),
                    QStringLiteral("终端的画面不按帧率刷新，这一项用不上"));
    hardware->body()->addStretch(1);
    blocks->addWidget(hardware, 4);

    GlassCard* ui = block(this, QStringLiteral("UI 设置 / 操作设置"));
    unavailable_row(ui, QStringLiteral("自定义UI"), QStringLiteral("终端收不到"),
                    QStringLiteral("机器人画给官方选手端的自定义 UI，协议里不发给自定义终端"));
    unavailable_row(ui, QStringLiteral("准心显示"), QStringLiteral("终端没有准心"), QStringLiteral("终端没做准心"));
    unavailable_row(ui, QStringLiteral("小地图设置"), QStringLiteral("在界面设置里调"),
                    QStringLiteral("终端的战术地图是一块面板，不能隐藏；位置和大小在「界面设置」里调"));
    QHBoxLayout* display = row(ui, QStringLiteral("显示设置"));
    fullscreen_ = toggle_button(ui, QStringLiteral("全屏"), "settingsFullscreen");
    windowed_ = toggle_button(ui, QStringLiteral("窗口"), "settingsWindowed");
    display->addWidget(fullscreen_);
    display->addWidget(windowed_);
    unavailable_row(ui, QStringLiteral("键鼠控制指令来源"), QStringLiteral("在官方选手端选"),
                    QStringLiteral("只有官方选手端能改。要用终端的键鼠控制，先在官方选手端这里选「自定义客户端」"));
    ui->body()->addStretch(1);
    blocks->addWidget(ui, 4);
    column->addLayout(blocks, 1);

    video_status_ = text_label(this, QString(), theme::kTextSecondary, 13);
    video_status_->setObjectName(QStringLiteral("settingsVideoStatus"));
    column->addWidget(video_status_);

    const auto pick = [this](std::optional<std::uint32_t> shooter_choice, std::optional<std::uint32_t> chassis_choice) {
        if (performance_) performance_(shooter_choice, chassis_choice);
        refresh();
    };
    QObject::connect(chassis_hp_, &QPushButton::clicked, this, [pick] { pick(std::nullopt, 1); });
    QObject::connect(chassis_power_, &QPushButton::clicked, this, [pick] { pick(std::nullopt, 2); });
    QObject::connect(shooter_cooling_, &QPushButton::clicked, this, [pick] { pick(1, std::nullopt); });
    QObject::connect(shooter_burst_, &QPushButton::clicked, this, [pick] { pick(2, std::nullopt); });
    QObject::connect(video_forward_, &QPushButton::clicked, this, [this] {
        settings_.video_reversed = false;
        emitSettings();
    });
    QObject::connect(video_reverse_, &QPushButton::clicked, this, [this] {
        settings_.video_reversed = true;
        emitSettings();
    });
    QObject::connect(sensitivity_, &QSlider::valueChanged, this, [this](int value) {
        settings_.sensitivity = value;
        emitSettings();
    });
    QObject::connect(volume_, &QSlider::valueChanged, this, [this](int value) {
        settings_.volume = value;
        emitSettings();
    });
    QObject::connect(fullscreen_, &QPushButton::clicked, this, [this] {
        if (display_) display_(true);
        refresh();
    });
    QObject::connect(windowed_, &QPushButton::clicked, this, [this] {
        if (display_) display_(false);
        refresh();
    });
    QObject::connect(reset_button_, &QPushButton::clicked, this, [this] {
        if (resetAllowed() && reset_) reset_();
    });

    if (host) host->installEventFilter(this);
    setVisible(false);
    refresh();
}

void SettingsPanel::setSettings(const TerminalSettings& settings) {
    settings_ = normalized(settings);
    refresh();
}

void SettingsPanel::setView(const SettingsView& view) {
    view_ = view;
    refresh();
}

void SettingsPanel::setOpen(bool open) {
    if (open == isOpen()) return;
    setVisible(open);
    if (open) {
        place();
        raise();
    }
    Q_EMIT openChanged(open);
}

void SettingsPanel::setActions(SettingsChanged changed, PerformancePicked performance, std::function<void(bool)> display,
                               std::function<void()> reset) {
    changed_ = std::move(changed);
    performance_ = std::move(performance);
    display_ = std::move(display);
    reset_ = std::move(reset);
}

bool SettingsPanel::resetAllowed() const { return !view_.stage || *view_.stage <= 1; }

QSize SettingsPanel::sizeHint() const { return QSize(kWidth, kHeight); }

void SettingsPanel::emitSettings() {
    if (changed_) changed_(settings_);
    refresh();
}

void SettingsPanel::refresh() {
    seat_->setText(view_.seat);
    login_->setText(view_.login);
    set_ink(login_, view_.login_problem ? theme::kDangerText : theme::kOkText);

    const bool reset_ok = resetAllowed();
    reset_button_->setEnabled(reset_ok);
    reset_button_->setToolTip(reset_ok ? QStringLiteral("把灵敏度、音量、图传方向恢复成默认，不动性能体系")
                                       : QStringLiteral("只能在自由阶段和三分钟准备阶段用"));

    const bool choosing = view_.has_performance && view_.performance_open;
    for (QPushButton* button : {chassis_hp_, chassis_power_, shooter_cooling_, shooter_burst_})
        button->setEnabled(choosing);
    set_checked(chassis_hp_, view_.has_performance && view_.chassis == std::optional<std::uint32_t>(1));
    set_checked(chassis_power_, view_.has_performance && view_.chassis == std::optional<std::uint32_t>(2));
    set_checked(shooter_cooling_, view_.has_performance && view_.shooter == std::optional<std::uint32_t>(1));
    set_checked(shooter_burst_, view_.has_performance && view_.shooter == std::optional<std::uint32_t>(2));
    performance_note_->setText(view_.performance_note);
    set_ink(performance_note_, view_.performance_warn ? theme::kWarn : theme::kTextSecondary);

    set_checked(video_forward_, !settings_.video_reversed);
    set_checked(video_reverse_, settings_.video_reversed);
    set_slider(sensitivity_, settings_.sensitivity);
    sensitivity_value_->setText(QString::number(settings_.sensitivity));
    set_slider(volume_, settings_.volume);
    volume_value_->setText(QString::number(settings_.volume));
    volume_->setEnabled(view_.volume_adjustable);
    volume_->setToolTip(view_.volume_adjustable ? QStringLiteral("提醒和语音的音量")
                                                : QStringLiteral("这台电脑的播放程序调不了音量"));

    set_checked(fullscreen_, view_.fullscreen);
    set_checked(windowed_, !view_.fullscreen);
    video_status_->setText(QStringLiteral("图传 · %1").arg(view_.video.isEmpty() ? QStringLiteral("--") : view_.video));
    update();
}

void SettingsPanel::place() {
    QWidget* host = parentWidget();
    if (!host) return;
    const int width = std::min(kWidth, host->width() - 40);
    const int height = std::min(kHeight, host->height() - 40);
    setGeometry((host->width() - width) / 2, (host->height() - height) / 2, width, height);
}

bool SettingsPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == parentWidget() && event->type() == QEvent::Resize && isOpen()) place();
    return false;
}

void SettingsPanel::paintEvent(QPaintEvent* event) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    theme::paintGlass(painter, QRectF(rect()).adjusted(1, 1, -1, -1), 10, false, theme::kOverlay,
                      theme::kBorderBright, true);
    QFrame::paintEvent(event);
}

}  // namespace rm_terminal
