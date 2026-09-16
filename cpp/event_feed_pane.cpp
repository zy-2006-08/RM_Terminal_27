#include "event_feed_pane.h"

#include "theme.h"

#include <QDateTime>
#include <QFontMetrics>
#include <QHash>
#include <QPainter>
#include <QPaintEvent>
#include <QPixmap>
#include <cmath>

namespace rm_terminal {

namespace {

constexpr int kRowGap = 10;
constexpr int kTimeWidth = 68;
constexpr int kTextInset = 8;
constexpr int kBannerPadding = 6;
// 光晕原图在窄横幅下偏艳,压到七成让文字成为主体而非底色。
constexpr qreal kBannerOpacity = 0.7;
// 横幅素材是 976x132 的光晕(中心不透明、四周衰减到全透),不是带边框的底图。
// 因此按行高整体缩放即可,任意宽度都不会露出硬边;九宫格拉伸在这里没有意义。
// 源图 976x132 是一团光晕。实测 alpha 分布:纵向只在 y=33..99 成规模,横向亮度
// 达峰值 50% 的区间是 x=299..676。横幅被压到贴合文字的窄尺寸后,若把两端近乎
// 透明的部分一起缩进来,亮带会被冲淡到几乎看不见,故两个方向都只取核心区。
constexpr int kBannerCoreLeft = 299;
constexpr int kBannerCoreRight = 676;
constexpr int kBannerCoreTop = 33;
constexpr int kBannerCoreBottom = 99;

QString banner_text_colour_key(EventBanner banner) {
    switch (banner) {
        case EventBanner::Red: return QStringLiteral("red");
        case EventBanner::Blue: return QStringLiteral("blue");
        case EventBanner::Positive: return QStringLiteral("green");
        case EventBanner::Neutral: break;
    }
    return QStringLiteral("neutral");
}

// 光晕的可见高度只占素材中段(见 alpha 行分布:边缘 0-3,中心 145+),整张塞进
// 22px 行高会让实际发光带细到看不见。放大到行高的 2.4 倍再居中裁切,取的就是
// 那条中段。
QPixmap banner_pixmap(EventBanner banner, int row_w, int row_h, qreal dpr) {
    static QHash<QString, QPixmap> cache;
    const QString key = QStringLiteral("%1|%2|%3|%4")
                            .arg(banner_text_colour_key(banner))
                            .arg(row_w)
                            .arg(row_h)
                            .arg(dpr);
    const auto hit = cache.constFind(key);
    if (hit != cache.constEnd()) return *hit;

    // 横幅宽度随每条文字长度变化,键的取值空间是像素级的。不设上限的话一场比赛
    // 下来会缓存上千张位图,故满额即整体丢弃重建。
    if (cache.size() > 256) cache.clear();

    const QString asset = event_banner_asset(banner);
    if (asset.isEmpty()) return {};
    QPixmap source(asset);
    if (source.isNull()) return {};

    const int target_w = static_cast<int>(std::lround(row_w * dpr));
    const int target_h = static_cast<int>(std::lround(row_h * dpr));

    const QRect core(kBannerCoreLeft, kBannerCoreTop, kBannerCoreRight - kBannerCoreLeft,
                     kBannerCoreBottom - kBannerCoreTop);
    QPixmap band = source.copy(core).scaled(target_w, target_h, Qt::IgnoreAspectRatio,
                                            Qt::SmoothTransformation);

    QPixmap out(target_w, target_h);
    out.setDevicePixelRatio(dpr);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.drawPixmap(0, 0, band);
    p.end();

    cache.insert(key, out);
    return out;
}

QColor banner_text_colour(EventBanner banner) {
    switch (banner) {
        case EventBanner::Red: return QColor(255, 228, 228);
        case EventBanner::Blue: return QColor(228, 240, 255);
        case EventBanner::Positive: return QColor(228, 255, 240);
        case EventBanner::Neutral: break;
    }
    return theme::kTextPrimary;
}

QFont row_font() {
    QFont font(QStringLiteral("Menlo"));
    font.setStyleHint(QFont::Monospace);
    font.setPointSize(13);
    font.setWeight(QFont::DemiBold);
    return font;
}

}  // namespace

// 能量机关按事件语义走绿色,与激活方无关:红方激活也不该渲染成红色警示横幅。
// 文本匹配是唯一可用的判据 —— 协议只给了 level 与 faction,没有事件类型编码。
EventBanner event_banner(const EventRecord& record) {
    const QString text = QString::fromStdString(record.text);
    if (text.contains(QStringLiteral("能量机关")) || text.contains(QStringLiteral("激活")))
        return EventBanner::Positive;
    switch (record.faction) {
        case 1: return EventBanner::Red;
        case 2: return EventBanner::Blue;
        default: break;
    }
    return EventBanner::Neutral;
}

// 协议里没有事件类型编码,只能按文本判别。清单集中在此处而不是散落在调用点,
// 新增本地指令时只改这一个数组。
bool is_local_control_echo(const EventRecord& record) {
    static const QString prefixes[] = {
        QStringLiteral("底盘模式切换为"),
        QStringLiteral("自瞄"),
        QStringLiteral("终端下发紧急停止"),
        QStringLiteral("云台回中"),
    };
    const QString text = QString::fromStdString(record.text);
    for (const QString& prefix : prefixes) {
        if (text.startsWith(prefix)) return true;
    }
    return false;
}

QString event_banner_asset(EventBanner banner) {
    switch (banner) {
        case EventBanner::Red: return QStringLiteral(":/images/message/message_red.png");
        case EventBanner::Blue: return QStringLiteral(":/images/message/message_blue.png");
        case EventBanner::Positive:
            return QStringLiteral(":/images/message/message_green.png");
        case EventBanner::Neutral: break;
    }
    return QString();
}

std::vector<EventFeedRow> build_event_rows(const Snapshot& snapshot, std::size_t max) {
    // 先取整段历史再过滤,最后才截断:若先按 max 取,本地控制回显会占掉配额,
    // 面板实际显示的赛事事件就会少于 max 条。
    const std::vector<EventRecord> records = snapshot.events.recent(snapshot.events.size());
    std::vector<EventFeedRow> rows;
    rows.reserve(std::min(records.size(), max));
    for (const EventRecord& record : records) {
        if (rows.size() >= max) break;
        if (is_local_control_echo(record)) continue;
        const QString time =
            record.timestamp_ms == 0
                ? QStringLiteral("--")
                : QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(record.timestamp_ms))
                      .toString(QStringLiteral("HH:mm:ss"));
        rows.push_back({time, QString::fromStdString(record.text), event_banner(record)});
    }
    return rows;
}

EventFeedPane::EventFeedPane(QWidget* parent) : QWidget(parent) { setFont(row_font()); }

void EventFeedPane::setRows(const std::vector<EventFeedRow>& rows) {
    rows_ = rows;
    update();
}

void EventFeedPane::setDroppedCount(std::size_t dropped) {
    dropped_ = dropped;
    update();
}

int EventFeedPane::rowHeight() { return QFontMetrics(row_font()).lineSpacing() + 8; }

// 宽度提示不能超过花名册列宽:报 460 会把左列顶宽,右列却只有图传的提示,画面
// 就左右不对称。事件文字过长时由 elidedText 收尾,不需要靠宽提示撑开。
QSize EventFeedPane::sizeHint() const { return QSize(kRosterColumnWidth, rowHeight()); }

void EventFeedPane::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setFont(row_font());

    const int row_h = rowHeight();
    if (rows_.empty() && dropped_ == 0) {
        painter.setPen(theme::kTextMuted);
        painter.drawText(QRect(0, 0, width(), row_h), Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("暂无赛事事件"));
        return;
    }

    int y = 0;
    for (const EventFeedRow& row : rows_) {
        if (y + row_h > height()) break;
        const QRect line(0, y, width(), row_h);

        const int text_left = line.left() + kTimeWidth + kTextInset;
        const int text_avail = line.right() - kTextInset - text_left;
        const QString shown =
            painter.fontMetrics().elidedText(row.text, Qt::ElideRight, text_avail);

        // 横幅贴合文字宽度而非铺满整行:短事件配长横幅会让面板看起来全是色块,
        // 也读不出哪条事件更重要。
        const int text_w = painter.fontMetrics().horizontalAdvance(shown);
        const QRect banner_box(text_left - kBannerPadding, line.top(),
                               std::min(text_w + kBannerPadding * 2, text_avail + kBannerPadding * 2),
                               row_h);
        const QPixmap banner = banner_pixmap(row.banner, banner_box.width(),
                                             banner_box.height(), devicePixelRatioF());
        if (!banner.isNull()) {
            const qreal prev_opacity = painter.opacity();
            painter.setOpacity(kBannerOpacity);
            painter.drawPixmap(banner_box.topLeft(), banner);
            painter.setOpacity(prev_opacity);
        }

        painter.setPen(theme::kTextSecondary);
        painter.drawText(QRect(line.left(), line.top(), kTimeWidth, row_h),
                         Qt::AlignLeft | Qt::AlignVCenter, row.time);

        painter.setPen(banner_text_colour(row.banner));
        painter.drawText(QRect(text_left, line.top(), text_avail, row_h),
                         Qt::AlignLeft | Qt::AlignVCenter, shown);
        y += row_h + kRowGap;
    }

    if (dropped_ > 0 && y + row_h <= height()) {
        painter.setPen(theme::kTextMuted);
        painter.drawText(QRect(kTimeWidth + kTextInset, y, width() - kTimeWidth - kTextInset,
                               row_h),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("… 更早 %1 条已滚出缓冲").arg(dropped_));
    }
}

}  // namespace rm_terminal
