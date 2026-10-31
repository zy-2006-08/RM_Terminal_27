#pragma once

#include "seat.h"

#include <QQuickWidget>
#include <cstdint>
#include <optional>

namespace rm_terminal {

// 开机选席位页的 C++ 宿主。页面本身是 qrc:/qml/SeatPicker.qml,这一层只做三件事:
// 把合法席位表灌进去、把点击翻译成 Seat、用一个局部事件循环把「选席位」变成同步调用。
//
// 规则(哪些编号合法、蓝方 +100、标签文字)一律留在 Seat 里,QML 不复制一份:
// 席位表来自规则手册,两处各写一份就一定会有一处漏改。
class SeatPicker final : public QQuickWidget {
    Q_OBJECT
public:
    explicit SeatPicker(QWidget* parent = nullptr);

    // 阻塞到用户确认或取消。确认返回所选席位,取消/关窗返回 std::nullopt。
    std::optional<Seat> exec();

    // 测试钩子,也是 QML 点击真正走的那条路:界面和测试共用同一套状态迁移,
    // 不存在「测试过了但点击另有一套逻辑」的缝。
    void chooseFaction(std::uint32_t f);  // 1 红 2 蓝
    void chooseSeat(int number);          // 1 / 3 / 4 / 6
    bool canConfirm() const { return seat().has_value(); }
    void confirm();
    void cancel();

Q_SIGNALS:
    void accepted(int robot_id);
    void cancelled();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private Q_SLOTS:
    // QML 的点击信号没有编译期类型可绑,只能按签名连到声明过的槽上;
    // 槽体只是转调上面那四个公开入口,点击和测试钩子因此走同一条路径。
    void onFactionRequested(int faction);
    void onSeatRequested(int number);
    void onConfirmRequested();
    void onCancelRequested();

private:
    // 两步都选过且编号合法时给出席位,否则 nullopt。canConfirm 和摘要都由它派生,
    // 「能不能确认」和「摘要写什么」因此不可能不一致。
    std::optional<Seat> seat() const;
    void publish();

    std::uint32_t faction_ = 0;
    int seat_number_ = 0;
    std::optional<Seat> result_;
    bool finished_ = false;
};

}  // namespace rm_terminal
