import QtQuick
import "."

// 顶部栏。几何数值取自 ClearWei/RM26_Client 的 TopInfoBar.qml(MIT):中央面板
// 414x106、基地血条 332x30。视觉由切图承担而不是代码描边 ——
// 自绘渐变做不出官方那种斜切/内发光质感。
Item {
    id: root
    width: parent ? parent.width : 1280
    // 血条下移到 y56 后,R 标(直径 57、以血条中心 y71 为心)底边到 y100;
    // 中央面板素材高 106,仍是最高元素,故整栏取 106。
    height: 106

    // 由 C++ 侧 setContextProperty 注入。has*Hp 为假时一律不画填充,
    // 避免把「没有数据」画成满血 —— 前哨站和基地同理。
    property int redHp: 0
    property int redMaxHp: 0
    property bool redHasHp: false
    property int blueHp: 0
    property int blueMaxHp: 0
    property bool blueHasHp: false
    property int redOutpostHp: 0
    property int redOutpostMaxHp: 0
    property bool redOutpostHasHp: false
    property int blueOutpostHp: 0
    property int blueOutpostMaxHp: 0
    property bool blueOutpostHasHp: false
    // 队名缺失时回落到「红方/蓝方」,不编造校名。
    property string redTeamName: ""
    property string blueTeamName: ""
    property string redScore: "--"
    property string blueScore: "--"
    property string clockText: "--:--"
    property string roundText: "-/-"
    property string stageText: "--"
    property bool clockCritical: false
    // 机器人明细由左右花名册面板承担,顶栏只留基地/前哨站/比分/时钟。
    property var redRobots: []
    property var blueRobots: []

    Rectangle { anchors.fill: parent; color: "#08090C" }

    // 前哨站牌放血条外侧、与血条同垂直中心 —— 参考端 TopInfoBar.qml 的
    // OutpostStatusBadge 就是 92x52 + anchors.right: redBar.left + verticalCenter。
    // 之前挪到血条上方是为了躲窗口边缘,结果和队名抢同一段空间;正确解法是收窄血条。
    OutpostBadge {
        id: redOutpost
        isBlue: false
        currentHp: root.redOutpostHp
        maxHp: root.redOutpostMaxHp
        hasData: root.redOutpostHasHp
        anchors.right: redBar.left
        anchors.rightMargin: 10
        anchors.verticalCenter: redBar.verticalCenter
    }

    OutpostBadge {
        id: blueOutpost
        isBlue: true
        currentHp: root.blueOutpostHp
        maxHp: root.blueOutpostMaxHp
        hasData: root.blueOutpostHasHp
        anchors.left: blueBar.right
        anchors.leftMargin: 10
        anchors.verticalCenter: blueBar.verticalCenter
    }

    BaseHealthBar {
        id: redBar
        width: 280
        height: 30
        isBlue: false
        currentHealth: root.redHp
        maxHealth: root.redMaxHp
        hasData: root.redHasHp
        anchors.right: centerPanel.left
        anchors.rightMargin: 32
        // 血条(连同锚在它上面的 R 标)整体下移到 y56,让上方那行校名有独立空间 ——
        // R 标直径大于血条高度,会向上溢出,贴太高就会压住校名。
        anchors.top: parent.top
        anchors.topMargin: 56
    }

    // 队名居中压在血条上方。参考图里这一行是校名+队名,是顶栏的主标题。
    // 协议没下发队名时整行不显示 —— 「红方」三个字血条本身的配色已经说明了阵营,
    // 再写一遍是冗余。
    // 队名在血条上方居中。前哨站已回到血条外侧,上方这段空间只归队名。
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

    // 阵营 R 标。参考图里这是一枚直径约血条高两倍的大圆章,骑在血条内侧端头上、
    // 半个身位探出条外,是整条血条的视觉锚点 —— 做小了塞进条内就变成像误贴的水印。
    // 尺寸绑血条高度,血条改高时不会脱开。素材是 500x500,放大不会糊。
    Image {
        source: "qrc:/images/top_mid/red_team_logo.png"
        width: redBar.height * 1.9
        height: width
        fillMode: Image.PreserveAspectFit
        smooth: true
        // 圆心压在血条端头上:一半盖条、一半探出。
        anchors.horizontalCenter: redBar.right
        anchors.verticalCenter: redBar.verticalCenter
        z: 3
    }

    BaseHealthBar {
        id: blueBar
        width: 280
        height: 30
        isBlue: true
        currentHealth: root.blueHp
        maxHealth: root.blueMaxHp
        hasData: root.blueHasHp
        anchors.left: centerPanel.right
        anchors.leftMargin: 32
        anchors.top: parent.top
        anchors.topMargin: 56
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

    Image {
        source: "qrc:/images/top_mid/blue_team_logo.png"
        width: blueBar.height * 1.9
        height: width
        fillMode: Image.PreserveAspectFit
        smooth: true
        anchors.horizontalCenter: blueBar.left
        anchors.verticalCenter: blueBar.verticalCenter
        z: 3
    }

    // 面板尺寸取素材原始比例 414x106。之前用参考项目的 340x86 配
    // PreserveAspectFit,实际渲染尺寸和素材区域对不上,文字全部错位。
    Item {
        id: centerPanel
        width: 414
        height: 106
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: 0

        Image {
            anchors.fill: parent
            source: "qrc:/images/top_mid/top_middle_background_v2.png"
            fillMode: Image.Stretch
            smooth: true
        }

        // 素材是深色 HUD 面板,文字必须用亮色。坐标按素材实测区域定位:
        // 顶部窄带 y2-22、两侧梯形凹槽中心 x65/x349 y30-78、中央下沉区 y40-95。
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            anchors.topMargin: 26
            text: "Round " + root.roundText + " · " + root.stageText
            color: "#9FB4C0"
            font.pixelSize: 11
            font.bold: true
        }

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            anchors.topMargin: 38
            text: root.clockText
            color: root.clockCritical ? "#FF5C5C" : "#EAF4FA"
            font.pixelSize: 30
            font.bold: true
            font.family: "Menlo"
        }

        // 比分槽心由素材的 alpha 通道实测,而不是靠亮度找槽壁 —— 之前几次都偏,
        // 是因为亮段里混着中央时钟面板的边和数字自己的描边,分不出哪条是槽壁。
        // 关键事实:槽内部是全透明(alpha=0),槽壁才有 alpha。于是逐行取
        // 「宽度约 40px 的透明区间」就是槽本身,不可能被别的元素污染。
        // 槽是斜的,每行中心右移约 0.6px,所以在数字墨迹覆盖的 y=44..60 上取均值:
        //   左槽心 81.5   右槽心 329.5   (镜像校验 414-329.5=84.5,与 81.5 相差 3px,
        //   源于素材本身左右不完全对称 —— 右槽整体略窄)
        // 用 horizontalCenterOffset 而不是锚边距:两位数比分时字面变宽,
        // 锚边会把数字推出槽心,锚中心则始终居中。
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.horizontalCenterOffset: 81.5 - 207
            anchors.verticalCenter: parent.verticalCenter
            text: root.redScore
            color: "#FFFFFF"
            font.pixelSize: 24
            font.bold: true
            style: Text.Outline
            styleColor: "#8E1B1B"
        }

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.horizontalCenterOffset: 329.5 - 207
            anchors.verticalCenter: parent.verticalCenter
            text: root.blueScore
            color: "#FFFFFF"
            font.pixelSize: 24
            font.bold: true
            style: Text.Outline
            styleColor: "#14508E"
        }
    }

}
