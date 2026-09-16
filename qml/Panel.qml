import QtQuick
import "."

// 面板外壳:渐变卡面 + 斜切标题 + 细边框。五个面板共用同一个壳,视觉节奏才一致。
// 顶栏用的是 PNG 素材,这里用渐变+边框在纯 QML 里取得同一种深色 HUD 质感,
// 不引入新素材(协议面板尺寸随窗口变,拉伸位图会糊)。
Rectangle {
    id: root

    property string title: ""
    property bool mirrored: false
    // 标题右侧的计数徽标(如存活「5/6」)。空串则不显示。
    property string badge: ""
    // 标题竖条/文字/分隔线的强调色。默认青,阵营面板传入红或蓝。
    property color accent: Theme.cyan
    default property alias content: body.data

    color: Theme.surface
    border.color: Theme.border
    border.width: 1
    radius: 3

    gradient: Gradient {
        GradientStop { position: 0.0; color: Theme.surfaceRaised }
        GradientStop { position: 1.0; color: Theme.surface }
    }

    // 标题栏。标题要明确压过正文一档:字号 15、字距 2.4、专用字族,
    // 左侧再加一条阵营/强调色竖条 —— 竖条是让标题「有分量」的关键,
    // 单纯加粗只会显得脏。
    Row {
        id: heading
        anchors.left: root.mirrored ? undefined : parent.left
        anchors.right: root.mirrored ? parent.right : undefined
        anchors.leftMargin: 10
        anchors.rightMargin: 10
        anchors.top: parent.top
        anchors.topMargin: 8
        spacing: 7
        layoutDirection: root.mirrored ? Qt.RightToLeft : Qt.LeftToRight

        Rectangle {
            width: 3
            height: titleText.font.pixelSize + 2
            radius: 1
            color: root.accent
            anchors.verticalCenter: parent.verticalCenter
        }

        Text {
            id: titleText
            text: root.title
            color: root.accent
            font.family: Theme.titleFamily
            font.pixelSize: 15
            font.bold: true
            font.letterSpacing: Theme.titleSpacing
        }

        // 存活计数。数字用等宽体,数值跳动时标题不会左右抖。
        Text {
            visible: root.badge !== ""
            text: root.badge
            color: Theme.textSecondary
            font.family: Theme.numericFamily
            font.pixelSize: 13
            font.bold: true
            anchors.verticalCenter: parent.verticalCenter
        }
    }

    // 标题下的分隔线,和顶栏中央面板的窄带呼应。
    Rectangle {
        id: rule
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: 10
        anchors.rightMargin: 10
        anchors.top: heading.bottom
        anchors.topMargin: 6
        height: 1
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop {
                position: 0.0
                color: root.mirrored ? "transparent" : Qt.darker(root.accent, 2.2)
            }
            GradientStop {
                position: 1.0
                color: root.mirrored ? Qt.darker(root.accent, 2.2) : "transparent"
            }
        }
    }

    Item {
        id: body
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: rule.bottom
        anchors.bottom: parent.bottom
        anchors.margins: 6
        anchors.topMargin: 6
    }
}
