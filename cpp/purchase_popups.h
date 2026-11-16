#pragma once

#include "assembly_flow.h"
#include "purchase.h"

#include <QObject>
#include <QString>
#include <QWidget>
#include <array>
#include <functional>

class QPushButton;

namespace rm_terminal {

// 兑换前的二次确认,照官方选手端:按 Y 确定、按 N 取消,也能用鼠标点。补给区兑换的按钮是
// 「是 / 否」,远程兑换是「确定 / 取消」。开着时每帧按最新状态重算,条件没了就灰掉「确定」。
class PurchaseConfirm final : public QWidget {
    Q_OBJECT
public:
    explicit PurchaseConfirm(QWidget* host);

    void setOffer(const PurchaseOffer& offer);
    const PurchaseOffer& offer() const { return offer_; }
    void setActions(std::function<void()> yes, std::function<void()> no);
    // 框里那句话;买不了时是原因。
    QString questionText() const;
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    PurchaseOffer offer_;
    std::function<void()> yes_action_;
    std::function<void()> no_action_;
    QPushButton* yes_ = nullptr;
    QPushButton* no_ = nullptr;
};

// H 键远程兑换框,屏幕底部正中。三项:回血 Ctrl+1、17mm Ctrl+2、42mm Ctrl+3,能买的亮、
// 不能买的灰并写原因。关着时如果有兑换结果(「已兑换，6 秒后生效」「到账」),只露出底下那条。
// 重装的框:上面多一排装配难度(1–4 选、Enter 开始),下面只有回血 Ctrl+1 和 42mm Ctrl+3,
// 重装没有 17mm 发射机构(规则手册表 3-3)。
class RemoteExchangePopup final : public QWidget {
    Q_OBJECT
public:
    explicit RemoteExchangePopup(QWidget* host);

    void setOffers(const std::array<PurchaseOffer, 3>& offers);
    const std::array<PurchaseOffer, 3>& offers() const { return offers_; }
    void setOpen(bool open);
    bool isOpen() const { return open_; }
    void setStatus(const QString& status, bool problem);
    void setChoose(std::function<void(int index)> choose);
    // 重装:装配难度那一排。row.shown = false 时是步兵的框。
    void setAssembly(const AssemblyRow& row);
    const AssemblyRow& assembly() const { return assembly_; }
    void setAssemblyActions(std::function<void(std::uint32_t level)> choose, std::function<void()> start);
    // 这一项在框里有没有(重装的框里没有 17mm)。
    bool hasItem(int index) const;
    QString barText() const;
    QString tileText(int index) const;
    QRect tileRect(int index) const;
    // 重装框里 1–4 级的键帽和「Enter 开始」。没开着或不是重装时为空。
    QRect levelRect(std::uint32_t level) const;
    QRect startRect() const;
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    void refresh();
    void paintAssembly(QPainter& painter);

    std::array<PurchaseOffer, 3> offers_;
    bool open_ = false;
    QString status_;
    bool problem_ = false;
    std::function<void(int)> choose_;
    AssemblyRow assembly_;
    std::function<void(std::uint32_t)> choose_level_;
    std::function<void()> start_assembly_;
};

// O/I 键补给区补弹面板,屏幕正中。照官方:「已有/可兑换允许发弹量 50/100」,下面一排
// −100 … +100 凑数量(42mm 是 −10 … +10),点「确定」再二次确认。买不了时整排灰掉,红字写原因。
class SupplyPanel final : public QWidget {
    Q_OBJECT
public:
    explicit SupplyPanel(QWidget* host);

    // 换口径时数量清零,档位换成这个口径的。
    void setCaliber(Caliber caliber);
    Caliber caliber() const { return caliber_; }
    void setView(const SelfCommandView& view, bool link_up);
    void setStatus(const QString& status, bool problem);
    void setConfirm(std::function<void()> confirm);
    std::uint32_t quantity() const { return quantity_; }
    PurchaseOffer offer() const;
    // 「50/100」,或者买不了的原因。
    QString infoText() const;
    bool blocked() const;
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void step(std::size_t index);
    void refresh();

    Caliber caliber_ = Caliber::Small;
    SelfCommandView view_;
    bool link_up_ = false;
    std::uint32_t quantity_ = 0;
    QString status_;
    bool problem_ = false;
    std::function<void()> confirm_action_;
    std::array<QPushButton*, 8> steps_{};
    QWidget* quantity_box_ = nullptr;
    QPushButton* confirm_ = nullptr;
};

// 按键、面板、确认框和「到没到账」串成官方选手端那一套,主程序只管喂数据和转按键。
// 阵亡时复活框占屏幕正中,补弹面板和确认框都收起来;远程兑换框在底部,照常能开。
class PurchaseFlow final : public QObject {
    Q_OBJECT
public:
    using Sender = std::function<bool(const PurchaseOffer& offer)>;

    explicit PurchaseFlow(QWidget* host);

    void setSender(Sender sender);
    void update(const SelfCommandView& view, bool link_up, MonotonicMs now);

    void toggleRemote();
    void toggleSupply(Caliber caliber);
    // 重装:框里装配难度那一排和它的鼠标点击。
    void setAssemblyRow(const AssemblyRow& row);
    void setAssemblyActions(std::function<void(std::uint32_t level)> choose, std::function<void()> start);
    // Ctrl+1/2/3 → 0/1/2。远程兑换框开着、那一项能买时弹确认框。
    bool chooseRemote(int index);
    // Y / N。确认框开着时处理并返回 true;没开着返回 false,Y 交给复活。
    bool yes();
    bool no();
    // 本车面板上的「兑换」「远程补弹」「远程回血」。
    void ask(PurchaseKind kind, std::uint32_t rounds);
    // 打开 P 设置面板时收起这里的框:同一时刻只开一个。
    void closeAll();
    // 主界面每帧会把自检页、比赛弹窗、「下一步」急事提到最上面;开着的兑换框要再盖回去。
    void raiseOpen();

    bool remoteOpen() const;
    bool supplyOpen() const;
    bool confirmOpen() const;
    // 本车标题栏和日志用:兑换到哪一步了。
    QString statusText() const;
    bool statusProblem() const;

    PurchaseConfirm* confirmBox() const { return confirm_; }
    RemoteExchangePopup* remotePopup() const { return remote_; }
    SupplyPanel* supplyPanel() const { return supply_; }

Q_SIGNALS:
    // 补弹面板要用鼠标点:开着时主程序暂停键鼠控制,关掉后接着控制。
    void supplyOpenChanged(bool open);
    // H 框开关。重装的 1–4、Enter 只在框开着时归终端,关着时不拦这几个键。
    void remoteOpenChanged(bool open);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    enum class Source { Pane, Remote, Supply };

    void open(const PurchaseOffer& offer, Source source);
    void setSupplyOpen(bool open);
    void setRemoteOpen(bool open);
    void flash(const QString& text);
    void refresh();
    void place();

    QWidget* host_;
    PurchaseConfirm* confirm_;
    RemoteExchangePopup* remote_;
    SupplyPanel* supply_;
    Source source_ = Source::Pane;
    Sender sender_;
    PurchaseWatch watch_;
    SelfCommandView view_;
    bool link_up_ = false;
    MonotonicMs now_ = 0;
    QString hint_;
    MonotonicMs hint_until_ = 0;
};

}  // namespace rm_terminal
