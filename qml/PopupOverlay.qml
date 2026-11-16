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
    // 窗口比 1600×900 大时整张卡片和字一起放大(和顶栏同一个倍数)。
    property real uiScale: 1.0
    // 赛前卡片:C++ 算好卡片、上面一段(倒计时和标题)和每个大按钮的位置,QML 只照着画;
    // 点击由叠在同一位置的透明按钮接(这一层不接鼠标)。cardRect 宽为 0 时用普通弹窗的样子。
    property rect cardRect: Qt.rect(0, 0, 0, 0)
    property rect headingRect: Qt.rect(0, 0, 0, 0)
    property var buttons: []
    property int hovered: -1
    readonly property bool hasCard: cardRect.width > 0

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
        width: root.hasCard ? root.cardRect.width : Math.min(root.width - 120, 650 * root.uiScale)
        height: root.hasCard ? root.cardRect.height : 250 * root.uiScale
        x: root.hasCard ? root.cardRect.x : (root.width - width) / 2
        y: root.hasCard ? root.cardRect.y : (root.height - height) / 2

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

        // 赛前卡片大,边框切图中间只有六成不透明,底下面板的字会透上来和按钮抢眼:垫一层深色。
        Rectangle {
            anchors.fill: parent
            anchors.leftMargin: parent.width * 0.04
            anchors.rightMargin: parent.width * 0.04
            anchors.topMargin: 4 * root.uiScale
            anchors.bottomMargin: 4 * root.uiScale
            color: "#F5080D15"
            visible: root.hasCard
            z: -1
        }

        ColumnLayout {
            x: root.hasCard ? root.headingRect.x : 26 * root.uiScale
            y: root.hasCard ? root.headingRect.y : 26 * root.uiScale
            width: root.hasCard ? root.headingRect.width : parent.width - 52 * root.uiScale
            height: root.hasCard ? root.headingRect.height : parent.height - 52 * root.uiScale
            spacing: 10 * root.uiScale

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
                font.pixelSize: Math.round(40 * root.uiScale)
                font.bold: true
            }

            Text {
                Layout.alignment: Qt.AlignHCenter
                text: root.title
                color: Theme.textPrimary
                font.family: Theme.titleFamily
                font.pixelSize: Math.round(26 * root.uiScale)
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
                font.pixelSize: Math.round(15 * root.uiScale)
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
            }

            // 警告条,复刻上游 warning_box.png 的用法。只在链路故障时出现:
            // 这是唯一一种「屏幕在骗你」的状态,值得额外一层视觉强调。
            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: 44 * root.uiScale
                Layout.leftMargin: 40 * root.uiScale
                Layout.rightMargin: 40 * root.uiScale
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
                    font.pixelSize: Math.round(15 * root.uiScale)
                    font.bold: true
                }
            }

            Item { Layout.fillHeight: true }
        }
    }

    // 赛前卡片下半截的大按钮:第一行是名字和快捷键,第二行是现在的设置。鼠标指着的那个亮起来。
    Repeater {
        model: root.hasCard ? root.buttons : []
        delegate: Item {
            id: button
            required property var modelData
            readonly property bool hot: root.hovered === modelData.key
            x: modelData.x
            y: modelData.y
            width: modelData.w
            height: modelData.h

            Rectangle {
                anchors.fill: parent
                anchors.margins: -4 * root.uiScale
                radius: 10 * root.uiScale
                color: "transparent"
                border.color: Theme.cyan
                border.width: 3 * root.uiScale
                opacity: button.hot ? 0.45 : 0
            }

            Rectangle {
                anchors.fill: parent
                radius: 7 * root.uiScale
                border.color: button.hot ? Theme.cyan : Qt.rgba(142 / 255, 210 / 255, 242 / 255, 0.6)
                border.width: Math.max(2, 2 * root.uiScale)
                gradient: Gradient {
                    GradientStop { position: 0.0; color: button.hot ? "#2E5A80" : "#1E3A58" }
                    GradientStop { position: 1.0; color: button.hot ? "#1F4264" : "#142840" }
                }
            }

            Column {
                anchors.centerIn: parent
                width: parent.width - 20 * root.uiScale
                spacing: 4 * root.uiScale

                Text {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: modelData.title
                    color: "#FFFFFF"
                    font.family: Theme.labelFamily
                    font.pixelSize: Math.round(18 * root.uiScale)
                    font.bold: true
                    fontSizeMode: Text.HorizontalFit
                    minimumPixelSize: 12
                }

                Text {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: modelData.value
                    color: modelData.warn ? Theme.warn : Theme.accentText
                    font.family: Theme.labelFamily
                    font.pixelSize: Math.round(14 * root.uiScale)
                    font.weight: modelData.warn ? Font.Bold : Font.Medium
                    fontSizeMode: Text.HorizontalFit
                    minimumPixelSize: 11
                }
            }
        }
    }
}
