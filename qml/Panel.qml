import QtQuick
import RmTerminal.Glass 1.0
import "."

// 玻璃面板外壳,和 C++ 的 GlassCard 是同一种画法:切角底、标题栏(斜竖条 + 标题 +
// 右侧小字)、标题下一道渐隐线。右侧那一列的面板镜像切角。
Item {
    id: root

    property string title: ""
    property bool mirrored: false
    property color accent: Theme.cyan
    // 标题栏右侧:一段说明文字和一个数字,例如「存活」「3/4」。两个都空就不显示。
    property string badgeLabel: ""
    property string badge: ""
    // 右侧小字往左让出多少(界面设置页里给齿轮和叉留地方)。
    property real badgeInset: 0
    default property alias content: body.data

    GlassShape {
        anchors.fill: parent
        cut: 10
        mirrored: root.mirrored
        fillColor: Theme.surface
        strokeColor: Theme.border
        highlight: true
    }

    Rectangle {
        x: 14
        y: 12.5
        width: 3
        height: 15
        color: root.accent
        antialiasing: true
        // CSS skewX(-14deg),绕竖条中心斜切:上端往右、下端往左。
        transform: Matrix4x4 {
            readonly property real k: Math.tan(-14 * Math.PI / 180)
            matrix: Qt.matrix4x4(1, k, 0, -7.5 * k, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)
        }
    }

    Text {
        x: 26
        height: 40
        verticalAlignment: Text.AlignVCenter
        text: root.title
        color: Theme.textPrimary
        font.family: Theme.titleFamily
        font.pixelSize: 15
        font.weight: Font.Bold
        font.letterSpacing: Theme.titleSpacing
    }

    Row {
        anchors.right: parent.right
        anchors.rightMargin: 14 + root.badgeInset
        height: 40
        spacing: 6
        visible: root.badge !== "" || root.badgeLabel !== ""
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: root.badgeLabel
            color: Theme.textSecondary
            font.family: Theme.labelFamily
            font.pixelSize: 13
            font.weight: Font.Medium
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: root.badge
            color: Theme.textBright
            font.family: Theme.numericFamily
            font.pixelSize: 14
            font.weight: Font.DemiBold
            font.features: Theme.tabular
        }
    }

    Rectangle {
        x: 12
        y: 39
        width: parent.width - 24
        height: 1
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0.0; color: Theme.borderBright }
            GradientStop { position: 0.85; color: Theme.alpha(Theme.borderBright, 0) }
        }
    }

    Item {
        id: body
        anchors.fill: parent
        anchors.margins: 8
        anchors.topMargin: 48
    }
}
