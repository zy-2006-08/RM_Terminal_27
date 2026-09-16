#pragma once

#include "domain.h"
#include "top_bar_model.h"

#include <QQuickWidget>
#include <QVariantList>
#include <QWidget>

namespace rm_terminal {

// QML 顶部栏在 Widgets 树里的宿主。几何取自素材:整栏 132px 高。
//
// 选 QQuickWidget 而不是重写成自绘控件,是因为官方那种斜切/内发光质感由切图承担,
// 自绘做不出来;Qml/Quick 依赖本来就已经在构建里。
class TopBar final : public QQuickWidget {
    Q_OBJECT
public:
    explicit TopBar(QWidget* parent = nullptr);

    void setSnapshot(const Snapshot& snapshot);

    bool loaded() const { return rootObject() != nullptr; }

private:
    void apply(const TopBarModel& model);
};

// QML 侧 Repeater 消费的形状。导出以便测试断言注入内容,而不必启动 GUI 去读属性。
QVariantList top_bar_cards(const TopBarSide& side);

}  // namespace rm_terminal
