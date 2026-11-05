import QtQuick
import "."

// 顶部栏(效果图 v5)。中间的计时器切图、轮次、比分和两侧的前哨站切图保持官方原样;
// 基地血条换成 B 款扁平实色,和 R 标、防御增益一起比原来上移 17 像素,和计时器同高。
// 控制度挪进了「战场数据分析」面板,顶栏不再画。
Item {
    id: root
    width: parent ? parent.width : 1280
    height: 106

    // 窗口背景里这一块(见 glass_items.h),顶栏底下透出的是同一张背景。
    property string backdropSource: ""

    // 由 C++ 侧注入。has*Hp 为假时一律不画填充,避免把「没有数据」画成满血。
    property int redHp: 0
    property int redMaxHp: 0
    property bool redHasHp: false
    property int blueHp: 0
    property int blueMaxHp: 0
    property bool blueHasHp: false
    property int redOutpostHp: 0
    property int redOutpostMaxHp: 0
    property bool redOutpostHasHp: false
    property bool redOutpostBlue: false
    property int blueOutpostHp: 0
    property int blueOutpostMaxHp: 0
    property bool blueOutpostHasHp: false
    property bool blueOutpostBlue: true
    // 队名缺失时整行不显示,不编造校名。
    property string redTeamName: ""
    property string blueTeamName: ""
    property string redScore: "--"
    property string blueScore: "--"
    property string clockText: "--:--"
    property string roundText: "-/-"
    property string stageText: "--"
    property bool clockCritical: false
    // 基地防御状态(GlobalUnitStatus):0 有 100% 防御增益,1 无增益且护甲未展开,
    // 2 无增益且护甲展开;-1 没有数据。控制度同理,-1 = 没有数据。
    property int redBaseStatus: -1
    property int blueBaseStatus: -1
    property int redControl: -1
    property int blueControl: -1
    property var redRobots: []
    property var blueRobots: []

    function defenseLabel(status) {
        if (status === 0) return "防御增益"
        if (status === 1) return "无防御增益 · 护甲未展开"
        if (status === 2) return "无防御增益 · 护甲已展开"
        return ""
    }
    function defenseColor(status) {
        if (status === 0) return Theme.textSecondary
        if (status === 1) return Theme.warn
        return Theme.danger
    }

    Image {
        anchors.fill: parent
        source: root.backdropSource
        cache: false
        smooth: false
        visible: root.backdropSource !== ""
    }

    // 前哨站牌放血条外侧,照参考端 OutpostStatusBadge:92x52,离血条 10 像素。
    OutpostBadge {
        isBlue: root.redOutpostBlue
        mirrored: false
        currentHp: root.redOutpostHp
        maxHp: root.redOutpostMaxHp
        hasData: root.redOutpostHasHp
        anchors.right: redBar.left
        anchors.rightMargin: 10
        y: 22
    }

    OutpostBadge {
        isBlue: root.blueOutpostBlue
        mirrored: true
        currentHp: root.blueOutpostHp
        maxHp: root.blueOutpostMaxHp
        hasData: root.blueOutpostHasHp
        anchors.left: blueBar.right
        anchors.leftMargin: 10
        y: 22
    }

    BaseHealthBar {
        id: redBar
        width: 280
        height: 24
        y: 42
        isBlue: false
        currentHealth: root.redHp
        maxHealth: root.redMaxHp
        hasData: root.redHasHp
        anchors.right: centerPanel.left
        anchors.rightMargin: 32
    }

    BaseHealthBar {
        id: blueBar
        width: 280
        height: 24
        y: 42
        isBlue: true
        currentHealth: root.blueHp
        maxHealth: root.blueMaxHp
        hasData: root.blueHasHp
        anchors.left: centerPanel.right
        anchors.leftMargin: 32
    }

    Text {
        visible: root.redTeamName !== ""
        anchors.horizontalCenter: redBar.horizontalCenter
        anchors.bottom: redBar.top
        anchors.bottomMargin: 4
        text: root.redTeamName
        color: "#FFFFFF"
        font.family: Theme.titleFamily
        font.pixelSize: 15
        font.bold: true
        font.letterSpacing: 0.8
        style: Text.Outline
        styleColor: "#3A0C0C"
    }

    Text {
        visible: root.blueTeamName !== ""
        anchors.horizontalCenter: blueBar.horizontalCenter
        anchors.bottom: blueBar.top
        anchors.bottomMargin: 4
        text: root.blueTeamName
        color: "#FFFFFF"
        font.family: Theme.titleFamily
        font.pixelSize: 15
        font.bold: true
        font.letterSpacing: 0.8
        style: Text.Outline
        styleColor: "#0A2647"
    }

    // 阵营 R 标:直径 57 的圆章,圆心压在血条内侧端头上,一半盖条、一半探出。
    Image {
        source: "qrc:/images/top_mid/red_team_logo.png"
        width: 57
        height: 57
        fillMode: Image.PreserveAspectFit
        smooth: true
        mipmap: true
        x: redBar.x + redBar.width - width / 2
        y: redBar.y + redBar.height / 2 - height / 2
        z: 3
    }

    Image {
        source: "qrc:/images/top_mid/blue_team_logo.png"
        width: 57
        height: 57
        fillMode: Image.PreserveAspectFit
        smooth: true
        mipmap: true
        x: blueBar.x - width / 2
        y: blueBar.y + blueBar.height / 2 - height / 2
        z: 3
    }

    // 防御状态居中写在血量数字正下方:数字躲开了 R 标,这一行跟着一起躲。
    Row {
        visible: root.redBaseStatus >= 0
        y: 72
        x: redBar.x + (redBar.width - redBar.logoInset - width) / 2
        spacing: 3
        Text {
            text: root.defenseLabel(root.redBaseStatus)
            color: root.defenseColor(root.redBaseStatus)
            font.family: Theme.labelFamily
            font.pixelSize: 12
            font.weight: Font.Medium
        }
        Text {
            visible: root.redBaseStatus === 0
            anchors.baseline: parent.children[0].baseline
            text: "100%"
            color: Theme.textBright
            font.family: Theme.numericFamily
            font.pixelSize: 13
            font.weight: Font.DemiBold
            font.features: Theme.tabular
        }
    }

    Row {
        visible: root.blueBaseStatus >= 0
        y: 72
        x: blueBar.x + blueBar.logoInset + (blueBar.width - blueBar.logoInset - width) / 2
        spacing: 3
        Text {
            text: root.defenseLabel(root.blueBaseStatus)
            color: root.defenseColor(root.blueBaseStatus)
            font.family: Theme.labelFamily
            font.pixelSize: 12
            font.weight: Font.Medium
        }
        Text {
            visible: root.blueBaseStatus === 0
            anchors.baseline: parent.children[0].baseline
            text: "100%"
            color: Theme.textBright
            font.family: Theme.numericFamily
            font.pixelSize: 13
            font.weight: Font.DemiBold
            font.features: Theme.tabular
        }
    }

    // 中央面板取素材原始比例 414x106,轮次、计时器、比分的坐标按素材实测区域定位。
    Item {
        id: centerPanel
        width: 414
        height: 106
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top

        Image {
            anchors.fill: parent
            source: "qrc:/images/top_mid/top_middle_background_v2.png"
            fillMode: Image.Stretch
            smooth: true
        }

        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            y: 25
            Text {
                text: "ROUND " + root.roundText + " · "
                color: "#9FB4C0"
                font.family: Theme.numericFamily
                font.pixelSize: 12
                font.weight: Font.DemiBold
                font.letterSpacing: 1.4
                font.features: Theme.tabular
            }
            Text {
                anchors.baseline: parent.children[0].baseline
                text: root.stageText
                color: "#9FB4C0"
                font.family: Theme.labelFamily
                font.pixelSize: 12
                font.weight: Font.Medium
                font.letterSpacing: 1
            }
        }

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            y: 38
            text: root.clockText
            color: root.clockCritical ? "#FF5C5C" : "#EAF4FA"
            font.family: Theme.numericFamily
            font.pixelSize: 36
            font.weight: Font.Bold
            font.letterSpacing: 0.7
            font.features: Theme.tabular
            style: Text.Raised
            styleColor: Qt.rgba(0, 0, 0, 0.45)
        }

        // 比分槽心由素材的 alpha 通道实测:左槽心 81.5,右槽心 329.5。锚中心而不是锚边距,
        // 两位数比分时字面变宽也始终居中。
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.horizontalCenterOffset: 81.5 - 207
            y: 37
            text: root.redScore
            color: "#FFFFFF"
            font.family: Theme.numericFamily
            font.pixelSize: 28
            font.weight: Font.Bold
            font.features: Theme.tabular
            style: Text.Outline
            styleColor: "#8E1B1B"
        }

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.horizontalCenterOffset: 329.5 - 207
            y: 37
            text: root.blueScore
            color: "#FFFFFF"
            font.family: Theme.numericFamily
            font.pixelSize: 28
            font.weight: Font.Bold
            font.features: Theme.tabular
            style: Text.Outline
            styleColor: "#14508E"
        }
    }
}
