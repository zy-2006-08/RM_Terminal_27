import QtQuick
import RmTerminal.Glass 1.0
import "."

// 结算面板的一行对比项:红值在左、标签(+小图标)居中、蓝值在右,中线下方一对
// 向外生长的条。条长按量程归一化,两侧同标尺 —— 不同标尺的两根条等于骗人。
Item {
    id: root

    property string label: ""
    property string icon: ""
    property bool redKnown: false
    property bool blueKnown: false
    property real redValue: 0
    property real blueValue: 0
    // 非空时代替数字(能量机关是状态而不是数)。
    property string redText: ""
    property string blueText: ""
    // 归一化分母。0 = 没有量程,按两侧较大值定标。
    property real scaleMax: 0
    property real grow: 1
    property real k: 1

    readonly property real span: {
        if (scaleMax > 0) return scaleMax
        var hi = Math.max(redKnown ? redValue : 0, blueKnown ? blueValue : 0)
        return hi > 0 ? hi : 0
    }

    function ratio(known, value) {
        if (!known || span <= 0) return 0
        return Math.max(0, Math.min(1, value / span))
    }

    function shown(known, value, text) {
        if (text !== "") return text
        return known ? String(value) : "--"
    }

    readonly property real barY: height * 0.66
    readonly property real barH: Math.max(5, 7 * k)
    readonly property real halfW: width / 2 - 4 * k

    // 红值(左)。缺数据的「--」用更暗的颜色:它不是一个小数值,是没有数值。
    Text {
        id: redLabel
        x: 2 * root.k
        y: root.height * 0.12
        text: root.shown(root.redKnown, root.redValue, root.redText)
        color: (root.redKnown || root.redText !== "") ? Theme.redText : Theme.textMuted
        font.family: root.redText !== "" ? Theme.labelFamily : Theme.numericFamily
        font.pixelSize: Math.round((root.redText !== "" ? 20 : 27) * root.k)
        font.weight: Font.Bold
        font.features: Theme.tabular
    }

    Text {
        id: blueLabel
        x: root.width - implicitWidth - 2 * root.k
        y: root.height * 0.12
        text: root.shown(root.blueKnown, root.blueValue, root.blueText)
        color: (root.blueKnown || root.blueText !== "") ? Theme.blueText : Theme.textMuted
        font.family: root.blueText !== "" ? Theme.labelFamily : Theme.numericFamily
        font.pixelSize: Math.round((root.blueText !== "" ? 20 : 27) * root.k)
        font.weight: Font.Bold
        font.features: Theme.tabular
    }

    // 中间:图标 + 标签。
    Row {
        anchors.horizontalCenter: parent.horizontalCenter
        y: root.height * 0.18
        spacing: 6 * root.k

        Image {
            anchors.verticalCenter: parent.verticalCenter
            visible: root.icon !== "" && status === Image.Ready
            source: root.icon
            width: 17 * root.k
            height: width
            fillMode: Image.PreserveAspectFit
            smooth: true
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: root.label
            color: Theme.textBright
            font.family: Theme.labelFamily
            font.pixelSize: Math.round(17 * root.k)
            font.weight: Font.DemiBold
            font.letterSpacing: 1 * root.k
        }
    }

    // 条的槽:一整条暗底,让「没长出来」也看得出这里本该有条。
    Rectangle {
        x: 0
        y: root.barY
        width: root.width
        height: root.barH
        color: Qt.rgba(1, 1, 1, 0.05)
        antialiasing: true
    }

    // 中线刻度。
    Rectangle {
        x: root.width / 2 - 1
        y: root.barY - 3 * root.k
        width: 2
        height: root.barH + 6 * root.k
        color: Theme.alpha(Theme.borderBright, 0.7)
    }

    // 红条:从中线往左长。
    Rectangle {
        readonly property real w: root.halfW * root.ratio(root.redKnown, root.redValue) * root.grow
        x: root.width / 2 - 2 * root.k - w
        y: root.barY
        width: w
        height: root.barH
        visible: w > 0.5
        antialiasing: true
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0.0; color: Theme.alpha(Theme.fillRed, 0.45) }
            GradientStop { position: 1.0; color: Theme.alpha(Theme.fillRed, 0.95) }
        }
    }

    // 蓝条:从中线往右长。
    Rectangle {
        readonly property real w: root.halfW * root.ratio(root.blueKnown, root.blueValue) * root.grow
        x: root.width / 2 + 2 * root.k
        y: root.barY
        width: w
        height: root.barH
        visible: w > 0.5
        antialiasing: true
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0.0; color: Theme.alpha(Theme.fillBlue, 0.95) }
            GradientStop { position: 1.0; color: Theme.alpha(Theme.fillBlue, 0.45) }
        }
    }
}
