import QtQuick
import RmTerminal.Glass 1.0
import "."

// 基地血条(效果图 B 款):扁平实色,颜色取自 R 标,和 R 标是同一种红、蓝;一半以下变橙黄,
// 四分之一以下变红。两端按 CSS skewX(±24deg) 斜切。血量数字用斜体金属字,躲开 R 标盖住的
// 那一头居中。掉血时掉的那段留白色残影 0.35 秒,再 0.45 秒缩回。
Item {
    id: root
    property int currentHealth: 0
    property int maxHealth: 0
    property bool isBlue: false
    // GameStatus 的基地血量字段缺失时整条不画填充,只留空槽 + 说明文字,避免把
    // 「没有数据」画成满血或空血。
    property bool hasData: false
    // R 标盖住的那一头留出的宽度:红方在右,蓝方在左。
    property real logoInset: 29

    readonly property int safeMax: Math.max(1, maxHealth)
    readonly property int clamped: Math.max(0, Math.min(safeMax, currentHealth))
    readonly property real ratio: hasData ? clamped / safeMax : 0
    readonly property color barColor: ratio >= 0.5 ? (isBlue ? Theme.logoBlue : Theme.logoRed)
                                    : (ratio > 0.25 ? Theme.baseWarn : Theme.baseDanger)

    property real trailHi: 0
    property real lastRatio: -1
    onRatioChanged: {
        if (!hasData || lastRatio < 0) {
            trailAnim.stop()
            trailHi = ratio
        } else if (ratio < lastRatio - 0.0001) {
            trailHi = Math.max(trailHi, lastRatio)
            trailAnim.restart()
        } else if (ratio > lastRatio) {
            trailAnim.stop()
            trailHi = ratio
        }
        lastRatio = hasData ? ratio : -1
    }
    onHasDataChanged: { trailAnim.stop(); trailHi = ratio; lastRatio = hasData ? ratio : -1 }

    SequentialAnimation {
        id: trailAnim
        PauseAnimation { duration: 350 }
        NumberAnimation { target: root; property: "trailHi"; to: root.ratio; duration: 450; easing.type: Easing.OutCubic }
    }

    Item {
        id: track
        anchors.fill: parent
        transform: Matrix4x4 {
            readonly property real s: Math.tan((root.isBlue ? -24 : 24) * Math.PI / 180)
            matrix: Qt.matrix4x4(1, s, 0, -s * track.height / 2, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)
        }
        Rectangle {
            anchors.fill: parent
            color: Qt.rgba(0, 0, 0, 0.36)
            border.color: Qt.rgba(1, 1, 1, 0.06)
            border.width: 1
            antialiasing: true
        }
        Rectangle {
            visible: root.trailHi > root.ratio + 0.002
            width: (root.trailHi - root.ratio) * parent.width
            height: parent.height
            x: root.isBlue ? root.ratio * parent.width : parent.width * (1 - root.trailHi)
            color: Qt.rgba(1, 1, 1, 0.6)
            antialiasing: true
        }
        Rectangle {
            visible: root.hasData && root.ratio > 0
            width: root.ratio * parent.width
            height: parent.height
            x: root.isBlue ? 0 : parent.width - width
            color: Theme.alpha(root.barColor, 0.9)
            antialiasing: true
            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                height: 1
                color: Qt.rgba(1, 1, 1, 0.2)
            }
        }
    }

    MetalText {
        anchors.fill: parent
        anchors.leftMargin: root.isBlue ? root.logoInset : 0
        anchors.rightMargin: root.isBlue ? 0 : root.logoInset
        visible: root.hasData
        text: root.clamped
        pixelSize: 22
        italic: true
    }

    Text {
        anchors.fill: parent
        anchors.leftMargin: root.isBlue ? root.logoInset : 0
        anchors.rightMargin: root.isBlue ? 0 : root.logoInset
        visible: !root.hasData
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        text: "无基地血量"
        color: Theme.textMuted
        font.family: Theme.labelFamily
        font.pixelSize: 12
    }
}
