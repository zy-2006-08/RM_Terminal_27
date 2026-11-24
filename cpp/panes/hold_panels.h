#pragma once

#include "domain.h"

#include <QObject>
#include <QString>
#include <QWidget>
#include <optional>
#include <vector>

namespace rm_terminal {

// 按住 ~ 出现、松开消失的面板,照官方选手端(手册第 20 页):左边「伤害面板」是这一条命里
// 各种原因扣的血和占比(2.2.8,复活后清零),下面「模块状态」是各模块在线没有(2.2.12)。
struct InjuryView {
    struct Row {
        QString name;
        std::optional<std::uint32_t> value;
    };
    struct Module {
        QString name;
        std::optional<std::uint32_t> state;  // 0 离线 1 在线 2 安装不规范,视为离线
    };
    std::optional<std::uint32_t> total;
    std::vector<Row> rows;
    std::vector<Module> modules;
};
InjuryView injury_view(const Snapshot& snapshot);

class InjuryPanel final : public QWidget {
    Q_OBJECT
public:
    explicit InjuryPanel(QWidget* host);
    void setView(const InjuryView& view);
    const InjuryView& view() const { return view_; }
    // 按住时显示在主窗口左侧中间,松开收起。
    void setHeld(bool held);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    InjuryView view_;
};

// 按住 Tab 出现、松开消失的面板,照官方选手端(手册第 19 页):上面是双方总伤害、己方机器人总剩余
// 血量、飞镖命中数;下面红蓝两边各一张表:编号、机器人、血量、底盘功率上限、热量上限、热量冷却、
// 射速上限。协议只给终端自己那台车的上限值,别的车只有血量,那几格写「-」。
struct TeamView {
    struct Row {
        std::uint32_t id = 0;
        QString type;
        std::optional<std::uint32_t> hp, max_hp;
        bool self = false;
        std::optional<std::uint32_t> power, max_heat;
        std::optional<double> cooling;
    };
    std::uint32_t own_faction = kRed;
    std::optional<std::uint32_t> red_damage, blue_damage;
    std::optional<std::uint32_t> own_hp_total;
    std::uint32_t own_dart_hits = 0;
    std::vector<Row> red, blue;
};
TeamView team_view(const Snapshot& snapshot);

class TeamPanel final : public QWidget {
    Q_OBJECT
public:
    explicit TeamPanel(QWidget* host);
    void setView(const TeamView& view);
    const TeamView& view() const { return view_; }
    // 按住时盖在主窗口正中,松开收起。
    void setHeld(bool held);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    TeamView view_;
};

// 看着几个要「按住」的键:Tab、~、F。焦点在打字框里时不管(那是在打字)。窗口不在前台时当作全松开,
// 免得面板停在屏幕上、能量机关在没按着的时候被开。Tab、~ 吃掉,F 照常往下走。
class HoldKeyFilter final : public QObject {
    Q_OBJECT
public:
    explicit HoldKeyFilter(QObject* parent = nullptr);
    void releaseAll();

Q_SIGNALS:
    void tabChanged(bool down);
    void tildeChanged(bool down);
    void runeKeyChanged(bool down);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    bool tab_ = false;
    bool tilde_ = false;
    bool rune_ = false;
};

}  // namespace rm_terminal
