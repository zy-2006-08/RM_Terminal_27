#include "dart_pane.h"

#include "theme.h"

#include <QPaintEvent>
#include <QPainter>

#include <algorithm>
#include <iterator>

namespace rm_terminal
{

    namespace
    {

        constexpr std::uint32_t kGateClosed = 0;
        constexpr std::uint32_t kGateOpening = 1;
        constexpr std::uint32_t kGateOpened = 2;
        constexpr std::uint32_t kStageMatch = 4;
        constexpr std::uint32_t kTargetCount = 5;
        // 规则 5.7.3:比赛 30 秒、4 分钟各给 1 次开闸机会,没用的留着;全开后能发射 30 秒;关闸后冷却 15 秒。
        constexpr std::int32_t kChanceAt[] = {30, 240};
        constexpr std::int32_t kFireWindowSec = 30;
        constexpr std::int32_t kGateCooldownSec = 15;

        template <class T>
        std::optional<T> present(const Field<T>& field)
        {
            if (field.quality != Quality::Valid || !field.value)
                return std::nullopt;
            return field.value;
        }

        bool engaged(std::optional<std::uint32_t> gate) { return gate == kGateOpening || gate == kGateOpened; }

        void observe(DartGateLog& log, const DartView& view)
        {
            if (!view.match_sec || (log.last_sec && *view.match_sec < *log.last_sec))
                log = DartGateLog{};
            if (view.gate)
            {
                if (engaged(view.gate) && !engaged(log.last_gate))
                    ++log.openings;
                // 第一次看到就是已开启(终端开在闸门开着的时候),不知道什么时候开的,不倒计时。
                if (view.gate == kGateOpened && log.last_gate && *log.last_gate != kGateOpened)
                    log.opened_sec = view.match_sec;
                if (view.gate == kGateClosed && engaged(log.last_gate))
                    log.closed_sec = view.match_sec;
                log.last_gate = view.gate;
            }
            log.last_sec = view.match_sec;
        }

        // 官方界面里次数用完、等下一次机会、冷却三种情况替换掉「F 开启飞镖闸门」那一行,顺序照它。
        DartPrompt dart_prompt(const DartView& view, const DartGateLog& log, bool asking)
        {
            if (!view.gate)
                return {DartStep::NoData, std::nullopt};
            if (view.gate == kGateOpening)
                return {DartStep::Opening, std::nullopt};
            if (view.match_sec && view.hits >= kDartsPerRound)
                return {DartStep::Spent, std::nullopt};
            if (view.gate == kGateOpened)
            {
                if (!view.match_sec || !log.opened_sec)
                    return {DartStep::Open, std::nullopt};
                return {DartStep::Open, std::max(0, kFireWindowSec - (*view.match_sec - *log.opened_sec))};
            }
            if (!view.match_sec)
                return {DartStep::NextChance, std::nullopt};
            const std::int32_t now = *view.match_sec;
            int granted = 0;
            std::optional<std::int32_t> next;
            for (const std::int32_t at : kChanceAt)
            {
                if (now >= at)
                    ++granted;
                else if (!next)
                    next = at - now;
            }
            if (log.openings >= static_cast<int>(std::size(kChanceAt)))
                return {DartStep::Exhausted, std::nullopt};
            if (log.openings >= granted)
                return {DartStep::NextChance, next};
            if (log.closed_sec && now < *log.closed_sec + kGateCooldownSec)
                return {DartStep::Cooldown, *log.closed_sec + kGateCooldownSec - now};
            return {asking ? DartStep::Asking : DartStep::Ready, std::nullopt};
        }

        struct KeyInk
        {
            QColor fill, stroke, text;
        };

        KeyInk key_ink(bool live)
        {
            if (live)
                return {theme::kChip, theme::kBorderBright, theme::kTextPrimary};
            return {QColor(255, 255, 255, 8), theme::kRowBorder, theme::kTextMuted};
        }

        void paint_key(QPainter& painter, const QRectF& box, const QString& key, const KeyInk& ink)
        {
            theme::paintGlass(painter, box, 3, false, ink.fill, ink.stroke);
            painter.setFont(theme::labelPx(
                std::max(theme::kMinLabelPx, static_cast<int>(std::lround(box.height() * 0.58))),
                QFont::Bold));
            painter.setPen(ink.text);
            painter.drawText(box, Qt::AlignCenter, key);
        }

        // 键帽高度是这一行的 55%,标签是行高的 32%:两个数都跟着行高走,格子拖大就一起变大。
        void paint_key_row(QPainter& painter, const QRectF& row, const QString& key, const QString& text,
                           const KeyInk& ink)
        {
            const qreal cap = std::max<qreal>(row.height() * 0.70, 14);
            paint_key(painter, QRectF(row.left(), row.center().y() - cap / 2, cap, cap), key, ink);
            const qreal lead = cap + cap * 0.35;
            // 标签放不下就先缩字号(到 11 为止),不许裁成「切换目」。
            const qreal room = row.width() - lead;
            const int wanted = std::max(theme::kMinLabelPx,
                                        static_cast<int>(std::lround(row.height() * 0.40)));
            painter.setFont(theme::labelPx(
                theme::fitFontPx(text, wanted, theme::kMinLabelPx, room,
                                 [](int px)
                                 { return theme::labelPx(px); })));
            painter.setPen(ink.text);
            painter.drawText(QRectF(row.left() + lead, row.top(), room, row.height()),
                             Qt::AlignLeft | Qt::AlignVCenter, text);
        }

        // 下面全部按「分到的那块地有多高」算字号和尺寸,不按固定设计尺寸乘系数 —— 2560 的
        // 大格子里后者只会留下一行小字和一大片空白。比例取自 BattleAnalysisPane 的做法。
        int px_of(qreal room, qreal fraction, int floor_px)
        {
            return std::max(floor_px, static_cast<int>(std::lround(room * fraction)));
        }

        // 剩余枚数画成四颗棱形:一眼数得出来,比「剩余 4 枚」四个字省地方也更像仪表。
        void paint_pips(QPainter& painter, const QRectF& box, std::uint32_t left)
        {
            const qreal size = std::max<qreal>(box.height() * 0.22, 5);
            const qreal width = size * 2.2;
            const qreal step = width + size * 0.6;
            const qreal x0 = box.right() - step * kDartsPerRound + (step - width);
            for (std::uint32_t i = 0; i < kDartsPerRound; ++i)
            {
                const QRectF pip(x0 + step * i, box.center().y() - size / 2, width, size);
                const bool filled = i < left;
                painter.setPen(filled ? Qt::NoPen : QPen(theme::kRowBorder, 1));
                painter.setBrush(filled ? (left == 1 ? theme::kWarn : theme::kCyan) : Qt::NoBrush);
                painter.drawPath(theme::skewBar(pip, -24));
            }
        }

        qreal pips_width(const QRectF& row)
        {
            const qreal size = std::max<qreal>(row.height() * 0.22, 5);
            return (size * 2.2 + size * 0.6) * kDartsPerRound;
        }

        // 面板的主角:一块切角玻璃,大字当前状态 + 可选的大号倒计时数字。
        struct Hero
        {
            QString title;
            QString caption;
            QColor ink;
            std::optional<std::int32_t> seconds;
            bool spinner = false;
        };

        // 主角块的字号按「整块面板的身子」算(标题 14%),块的高度再按字算出来。玻璃底
        // 几乎透明,块比内容高就会被看成一片空白,所以块只占内容 + 内边距。
        int hero_title_px(const Hero& hero, qreal body_h)
        {
            return px_of(body_h, hero.caption.isEmpty() ? 0.185 : 0.15, theme::kMinLabelPx);
        }
        int hero_caption_px(qreal body_h) { return px_of(body_h, 0.062, theme::kMinLabelPx); }

        qreal hero_height(const Hero& hero, qreal body_h)
        {
            const int title_px = hero_title_px(hero, body_h);
            qreal content = QFontMetricsF(theme::labelPx(title_px, QFont::Bold)).height();
            if (!hero.caption.isEmpty())
                content += QFontMetricsF(theme::labelPx(std::max(hero_caption_px(body_h),
                                                                 px_of(title_px, 0.45, theme::kMinLabelPx))))
                               .height();
            if (hero.seconds)
                content = std::max(content, QFontMetricsF(theme::numericPx(px_of(title_px, 1.45,
                                                                                 theme::kMinNumberPx),
                                                                           QFont::Bold))
                                                .height());
            return content + std::max<qreal>(title_px * 0.45, 8) * 2;
        }

        void paint_hero(QPainter& painter, const QRectF& box, const Hero& hero, qreal body_h,
                        MonotonicMs now)
        {
            theme::paintGlass(painter, box, 6, false, theme::kTile, theme::kRowBorder, true);
            const int title_px = hero_title_px(hero, body_h);
            const int caption_px = hero_caption_px(body_h);
            const qreal pad = std::max<qreal>(title_px * 0.55, 8);
            QRectF text = box.adjusted(pad, 0, -pad, 0);

            if (hero.seconds)
            {
                // 倒计时是全场最大的字,占右侧;状态文字在左,两边都不许压到一起。
                const QFont number = theme::numericPx(px_of(title_px, 1.45, theme::kMinNumberPx), QFont::Bold);
                const QFont unit = theme::labelPx(caption_px);
                const QString digits = QString::number(*hero.seconds);
                const qreal unit_w = QFontMetricsF(unit).horizontalAdvance(QStringLiteral(" 秒"));
                const qreal number_w = QFontMetricsF(number).horizontalAdvance(digits) + unit_w;
                painter.setFont(number);
                painter.setPen(hero.ink);
                painter.drawText(QRectF(text.right() - number_w, text.top(), number_w - unit_w, text.height()),
                                 Qt::AlignRight | Qt::AlignVCenter, digits);
                painter.setFont(unit);
                painter.setPen(theme::kTextMuted);
                painter.drawText(QRectF(text.right() - unit_w, text.top(), unit_w, text.height()),
                                 Qt::AlignRight | Qt::AlignVCenter, QStringLiteral(" 秒"));
                text.setRight(text.right() - number_w - pad * 0.6);
            }
            else if (hero.spinner)
            {
                const qreal ring = std::max<qreal>(title_px * 0.62, 8);
                const QPointF centre(text.right() - ring, text.center().y());
                const QRectF arc(centre.x() - ring, centre.y() - ring, ring * 2, ring * 2);
                const qreal pen = std::max<qreal>(ring * 0.22, 2);
                painter.setBrush(Qt::NoBrush);
                painter.setPen(QPen(theme::kRowBorder, pen));
                painter.drawEllipse(arc);
                painter.setPen(QPen(theme::kCyan, pen, Qt::SolidLine, Qt::RoundCap));
                painter.drawArc(arc, -static_cast<int>((now / 250 * 45) % 360) * 16, 100 * 16);
                text.setRight(arc.left() - pad * 0.6);
            }

            const QFont title = theme::labelPx(
                theme::fitFontPx(hero.title, title_px, theme::kMinLabelPx, text.width(),
                                 [](int px)
                                 { return theme::labelPx(px, QFont::Bold); }),
                QFont::Bold);
            if (hero.caption.isEmpty())
            {
                painter.setFont(title);
                painter.setPen(hero.ink);
                painter.drawText(text, Qt::AlignLeft | Qt::AlignVCenter, hero.title);
                return;
            }
            // 副标题只在它说了别处看不到的事时才有(见 paintEvent),字号不低于标题的 45%。
            const QFont caption = theme::labelPx(std::max(caption_px, px_of(title_px, 0.45, theme::kMinLabelPx)));
            const qreal title_h = QFontMetricsF(title).height();
            const qreal caption_h = QFontMetricsF(caption).height();
            const qreal top = text.center().y() - (title_h + caption_h) / 2;
            painter.setFont(title);
            painter.setPen(hero.ink);
            painter.drawText(QRectF(text.left(), top, text.width(), title_h),
                             Qt::AlignLeft | Qt::AlignVCenter, hero.title);
            painter.setFont(caption);
            painter.setPen(theme::kTextSecondary);
            painter.drawText(QRectF(text.left(), top + title_h, text.width(), caption_h),
                             Qt::AlignLeft | Qt::AlignVCenter, hero.caption);
        }

        void paint_target_row(QPainter& painter, const QRectF& row, std::optional<std::uint32_t> target,
                              std::optional<std::uint32_t> darts_left)
        {
            const qreal h = row.height();
            const int value_px = px_of(h, 0.52, theme::kMinLabelPx);
            const int label_px = px_of(h, 0.28, theme::kMinLabelPx);
            qreal right = row.right();
            if (darts_left)
            {
                const qreal w = pips_width(row);
                paint_pips(painter, QRectF(row.right() - w, row.top(), w, h), *darts_left);
                right -= w + h * 0.2;
            }

            const qreal ring = std::max<qreal>(h * 0.17, 4);
            const QPointF centre(row.left() + ring + 1, row.center().y());
            painter.setPen(QPen(theme::kCyanDim, std::max<qreal>(ring * 0.22, 1.2)));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(centre, ring, ring);
            for (const QPointF step : {QPointF(1, 0), QPointF(-1, 0), QPointF(0, 1), QPointF(0, -1)})
                painter.drawLine(centre + step * (ring * 0.62), centre + step * (ring * 1.4));

            const QFont label = theme::labelPx(label_px);
            painter.setFont(label);
            painter.setPen(theme::kTextMuted);
            const QString caption = QStringLiteral("目标");
            const qreal caption_x = centre.x() + ring + h * 0.18;
            const qreal caption_w = QFontMetricsF(label).horizontalAdvance(caption);
            painter.drawText(QRectF(caption_x, row.top(), caption_w, h), Qt::AlignLeft | Qt::AlignVCenter,
                             caption);

            const qreal name_x = caption_x + caption_w + h * 0.16;
            const QString name = target ? dart_target_text(*target) : QStringLiteral("--");
            const int fitted = theme::fitFontPx(name, value_px, theme::kMinLabelPx, right - name_x,
                                                [](int px)
                                                { return theme::labelPx(px, QFont::Bold); });
            painter.setFont(theme::labelPx(fitted, QFont::Bold));
            painter.setPen(target ? theme::kTextPrimary : theme::kTextMuted);
            painter.drawText(QRectF(name_x, row.top(), right - name_x, h), Qt::AlignLeft | Qt::AlignVCenter,
                             name);
        }

    } // namespace

    QString dart_target_text(std::uint32_t target_id)
    {
        switch (target_id)
        {
        case 1: return QStringLiteral("前哨站");
        case 2: return QStringLiteral("基地固定");
        case 3: return QStringLiteral("基地随机固定");
        case 4: return QStringLiteral("基地随机移动");
        case 5: return QStringLiteral("基地末端移动");
        default: return QStringLiteral("--");
        }
    }

    DartView dart_view(const Snapshot& snapshot)
    {
        DartView view;
        view.gate = present(snapshot.dart.gate);
        view.target_id = present(snapshot.dart.target_id);
        if (present(snapshot.game.current_stage) == kStageMatch)
            view.match_sec = present(snapshot.game.stage_elapsed_sec);
        if (snapshot.self)
            view.hits = faction_of(snapshot.self->value) == kRed ? snapshot.dart_hits.red : snapshot.dart_hits.blue;
        return view;
    }

    DartPane::DartPane(QWidget* parent) : QWidget(parent) {}

    void DartPane::setView(const DartView& view)
    {
        view_ = view;
        observe(log_, view_);
        if (dart_prompt(view_, log_, false).step != DartStep::Ready)
            asking_ = false;
        update();
    }

    void DartPane::setLinkUp(bool up)
    {
        if (link_up_ == up)
            return;
        link_up_ = up;
        if (!up)
            asking_ = false;
        update();
    }

    void DartPane::setSender(std::function<bool(const char* topic, const std::string& payload)> sender)
    {
        sender_ = std::move(sender);
    }

    void DartPane::tick(MonotonicMs now)
    {
        now_ = now;
        update();
    }

    void DartPane::cycleTarget()
    {
        const std::uint32_t next = view_.target_id.value_or(0) % kTargetCount + 1;
        // open 写的是想要的闸门状态:闸门开着时写 false,服务器可能当成关闸。
        send(next, engaged(view_.gate), false);
    }

    void DartPane::requestOpen()
    {
        if (!live() || prompt().step != DartStep::Ready)
            return;
        asking_ = true;
        update();
    }

    void DartPane::confirmOpen()
    {
        if (!live() || prompt().step != DartStep::Asking)
            return;
        if (send(view_.target_id.value_or(0), true, false))
            asking_ = false;
        update();
    }

    void DartPane::cancelOpen()
    {
        if (!asking_)
            return;
        asking_ = false;
        update();
    }

    void DartPane::launch()
    {
        if (prompt().step != DartStep::Open)
            return;
        send(view_.target_id.value_or(0), true, true);
    }

    DartPrompt DartPane::prompt() const { return dart_prompt(view_, log_, asking_); }

    bool DartPane::live() const { return link_up_ && static_cast<bool>(sender_); }

    bool DartPane::send(std::uint32_t target_id, bool open, bool launch_confirm)
    {
        if (!live() || !gate_.allow(now_))
            return false;
        return sender_("DartCommand", encode_dart(target_id, open, launch_confirm));
    }

    QSize DartPane::sizeHint() const { return QSize(380, 250); }

    void DartPane::paintEvent(QPaintEvent*)
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const bool live = this->live();
        const DartPrompt shown = prompt();
        const qreal w = width();
        const qreal h = height();
        if (w <= 0 || h <= 0)
            return;

        // 三块按高度的比例分:目标 22%、主角 50%、按键 28%,块间空隙各 4% 以内。
        // 字号都从各自那块的高度算出来,所以格子越大字越大,不会剩下大片空白。
        const qreal gap = std::min(h * 0.035, 12.0);
        const qreal body = h - gap * 2;
        const QRectF target_row(0, 0, w, body * 0.22);
        // 按键行按 28% 钉在底部,目标行钉在顶部,主角块吃掉中间剩下的全部高度 —— 中间
        // 不留空带。主角块里的字按它自己的高度算,所以块越高字越大。
        const QRectF key_row(0, h - body * 0.28, w, body * 0.28);

        std::optional<std::uint32_t> darts_left;
        if (view_.match_sec)
            darts_left = kDartsPerRound - std::min(view_.hits, kDartsPerRound);
        paint_target_row(painter, target_row, view_.target_id, darts_left);

        Hero hero;
        switch (shown.step)
        {
        case DartStep::NoData:
            hero = {QStringLiteral("等待飞镖数据"), QString(), theme::kTextMuted, std::nullopt, false};
            break;
        case DartStep::Ready:
            hero = {QStringLiteral("可以开闸"), QString(), theme::kAccentText, std::nullopt, false};
            break;
        case DartStep::Asking:
            hero = {QStringLiteral("确认开闸？"), QString(), theme::kWarn, std::nullopt, false};
            break;
        case DartStep::Opening:
            hero = {QStringLiteral("闸门运行中"), QString(), theme::kTextPrimary, std::nullopt, true};
            break;
        case DartStep::Open:
            hero = {QStringLiteral("闸门已开"), QString(), theme::kOkText, shown.seconds, false};
            break;
        case DartStep::Cooldown:
            hero = {QStringLiteral("发射站冷却"), QString(), theme::kWarn, shown.seconds, false};
            break;
        case DartStep::NextChance:
            // 副标题说的是「下一次机会是第几次」,这事别处看不到,留着。
            hero = {shown.seconds ? QStringLiteral("等下一次开闸") : QStringLiteral("比赛中才能开闸"),
                    shown.seconds ? (log_.openings == 0 ? QStringLiteral("本局第一次机会")
                                                        : QStringLiteral("本局第二次机会"))
                                  : QString(),
                    theme::kTextSecondary, shown.seconds, false};
            break;
        case DartStep::Exhausted:
            hero = {QStringLiteral("开闸次数用完"), QStringLiteral("本局两次机会都用过了"),
                    theme::kDangerText, std::nullopt, false};
            break;
        case DartStep::Spent:
            hero = {QStringLiteral("本局飞镖用完"), QStringLiteral("四枚都已命中"),
                    theme::kDangerText, std::nullopt, false};
            break;
        }
        // 主角块只占内容那么高,放在目标行和按键行中间居中:玻璃底几乎透明,块比内容
        // 高的那部分看起来就是空白。
        const qreal mid_top = target_row.bottom() + gap;
        const qreal mid_h = key_row.top() - gap - mid_top;
        const qreal hero_h = std::min(mid_h, hero_height(hero, body));
        paint_hero(painter, QRectF(0, mid_top + (mid_h - hero_h) / 2, w, hero_h), hero, body, now_);

        // 只画当前这一步能按的键,J 永远能按。主角块里不再重复这些提示(以前「Y 确定 ·
        // N 取消」在主角和按键行各写一遍),所以这一行就是键位的唯一出处。
        std::vector<std::pair<QString, QString>> keys{{QStringLiteral("J"), QStringLiteral("切换目标")}};
        const bool asking = shown.step == DartStep::Asking;
        if (asking)
        {
            keys.push_back({QStringLiteral("Y"), QStringLiteral("确定")});
            keys.push_back({QStringLiteral("N"), QStringLiteral("取消")});
        }
        else if (shown.step == DartStep::Open)
        {
            keys.push_back({QStringLiteral("L"), QStringLiteral("发射飞镖")});
        }
        else if (shown.step == DartStep::Ready)
        {
            keys.push_back({QStringLiteral("F"), QStringLiteral("开启闸门")});
        }
        const qreal slot = (w - gap * static_cast<qreal>(keys.size() - 1)) / keys.size();
        for (std::size_t i = 0; i < keys.size(); ++i)
        {
            const auto& [key, label] = keys[i];
            KeyInk ink = key_ink(live);
            // Y / N 用成败色:确认框里按错一颗的代价不一样,颜色要替操作手分开。
            if (asking && live && key == QStringLiteral("Y"))
                ink = {QColor(28, 64, 50, 200), theme::kOk, theme::kOkText};
            if (asking && live && key == QStringLiteral("N"))
                ink = {QColor(72, 22, 30, 200), theme::kDanger, theme::kDangerText};
            paint_key_row(painter, QRectF(i * (slot + gap), key_row.top(), slot, key_row.height()), key,
                          label, ink);
        }
    }

} // namespace rm_terminal
