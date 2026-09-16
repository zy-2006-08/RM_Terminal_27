import QtQuick
import QtQuick.Layouts
import "."

// 弹窗遮罩。视觉规范复刻 ClearWei/RM26_Client 的 PrepPhasePopup/BattlePausePopup:
// 官方风格的三色发光边框图 + 半透明压暗遮罩 + 倒计时/标题/说明三层字号。
//
// 用 QML 而不是 QWidget 样式表,是因为边框发光、渐变梯形这些是 PNG 素材,
// 样式表只能画出纯色圆角矩形,和顶栏、花名册(均为 QML)明显不同档次。
Item {
    id: root

    // C++ 侧注入。kind 决定配色语义,不由 QML 自己猜。
    property string kind: ""          // linkLost / waiting / settlement / eliminated / paused / preMatch
    property string title: ""
    property string detail: ""
    property int countdownSec: -1
    property bool shown: false

    visible: shown

    // 三色语义沿用上游:红=紧急(链路断/阵亡)、黄=警示(暂停/结算)、青=常规(备战/等待)。
    // 颜色承载警示等级,操作手先读颜色再读字。
    readonly property string frameSource: {
        if (kind === "linkLost" || kind === "eliminated")
            return "qrc:/images/gamephase/gamestatus_red.png"
        if (kind === "paused" || kind === "settlement")
            return "qrc:/images/gamephase/gamestatus_yellow.png"
        return "qrc:/images/gamephase/gamestatus_cyan.png"
    }

    readonly property color accent: {
        if (kind === "linkLost" || kind === "eliminated") return Theme.danger
        if (kind === "paused" || kind === "settlement") return Theme.warn
        return Theme.cyan
    }

    // 链路类弹窗要额外警告:屏幕上的数值已经不可信,照着它决策会出事。
    readonly property bool linkFault: kind === "linkLost" || kind === "waiting"

    // 半透明压暗层。铺满整屏但不改变任何控件几何 —— 弹窗推位整屏控件会在最紧张的
    // 时刻废掉肌肉记忆,所以只压暗,不重排。
    Rectangle {
        anchors.fill: parent
        color: "#66000000"
    }

    Item {
        id: dialog
        width: Math.min(root.width - 120, 650)
        height: 250
        anchors.centerIn: parent

        Image {
            id: frameImage
            anchors.fill: parent
            source: root.frameSource
            fillMode: Image.Stretch
        }

        // 素材缺失时的兜底,不能让弹窗变成透明空盒:文字浮在画面上会读不清。
        Rectangle {
            anchors.fill: parent
            color: "#DD0E121A"
            radius: 6
            border.color: root.accent
            border.width: 2
            visible: frameImage.status !== Image.Ready
            z: -1
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 26
            spacing: 10

            Item { Layout.fillHeight: true }

            // 倒计时。只有备战阶段有意义,其余状态隐藏而不是显示 00:00 —— 假数字
            // 比留空更糟。
            Text {
                Layout.alignment: Qt.AlignHCenter
                visible: root.countdownSec >= 0
                text: {
                    var m = Math.floor(root.countdownSec / 60)
                    var s = root.countdownSec % 60
                    return (m < 10 ? "0" + m : m) + ":" + (s < 10 ? "0" + s : s)
                }
                color: root.accent
                font.family: Theme.numericFamily
                font.pixelSize: 40
                font.bold: true
            }

            Text {
                Layout.alignment: Qt.AlignHCenter
                text: root.title
                color: Theme.textPrimary
                font.family: Theme.titleFamily
                font.pixelSize: 26
                font.bold: true
                font.letterSpacing: Theme.titleSpacing
            }

            Text {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignHCenter
                visible: root.detail !== ""
                text: root.detail
                color: Theme.textSecondary
                font.family: Theme.labelFamily
                font.pixelSize: 15
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
            }

            // 警告条,复刻上游 warning_box.png 的用法。只在链路故障时出现:
            // 这是唯一一种「屏幕在骗你」的状态,值得额外一层视觉强调。
            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: 44
                Layout.leftMargin: 40
                Layout.rightMargin: 40
                visible: root.linkFault

                Image {
                    id: warnBg
                    anchors.fill: parent
                    source: "qrc:/images/message/warning_box.png"
                    fillMode: Image.Stretch
                }

                Rectangle {
                    anchors.fill: parent
                    color: "transparent"
                    border.color: Theme.danger
                    border.width: 2
                    radius: 4
                    visible: warnBg.status !== Image.Ready
                }

                Text {
                    anchors.centerIn: parent
                    text: "数值为最后一次有效值"
                    color: "#FF8080"
                    font.family: Theme.labelFamily
                    font.pixelSize: 15
                    font.bold: true
                }
            }

            Item { Layout.fillHeight: true }
        }
    }
}
