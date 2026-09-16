import QtQuick
import "."

// 机器人卡。尺寸取自 ClearWei/RM26_Client 的 RobotStatusItem.qml(MIT):
// 卡 82x72、底板 82x44、头像 62x50、血条条带 59x6。
Item {
    id: root
    width: 82
    height: width * 72 / 82

    // 所有内部尺寸按 82 宽的设计稿等比缩放,这样调用方只给宽度就能整行对齐血条,
    // 不会因为写死 82 而溢出窗口。
    readonly property real k: width / 82

    property int robotId: 1
    property int currentHP: 0
    property int maxHP: 0
    property bool hasHp: false
    property bool online: false
    property string robotClass: ""
    property bool isBlue: false
    // 花名册行自己画血条和编号文字,复用本卡片只为拿到底板+头像的立体质感。
    // 顶栏宽度紧、只能靠角标编号;花名册有「3 号 步兵」整段文字,卡内再画一个
    // 编号就是同一信息出现两次。两条血条并排同理会互相抢读。
    property bool showBar: true
    property bool showId: true

    readonly property int shownId: robotId > 100 ? robotId - 100 : robotId
    readonly property bool dead: hasHp && currentHP <= 0
    readonly property real cardOpacity: online ? (dead ? 0.65 : 1.0) : 0.5

    readonly property string avatar: Theme.avatarFor(robotClass, isBlue)

    Image {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        width: 82 * root.k
        height: 44 * root.k
        source: Theme.pedestalFor(root.isBlue)
        fillMode: Image.PreserveAspectFit
        opacity: root.cardOpacity
    }

    Image {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 10 * root.k
        width: 62 * root.k
        height: 50 * root.k
        source: root.avatar
        fillMode: Image.PreserveAspectFit
        // 离线槽位也要画头像:否则卡片是一片纯黑,看不出这个位置是什么兵种。
        // 用低透明度表达「不在线」,而不是隐藏。
        visible: root.avatar !== ""
        opacity: !root.online ? 0.3 : (root.dead ? 0.45 : 1.0)
    }

    // 编号放卡片顶部:空中机器人的头像素材右下角自带「无人机」字样,编号若也
    // 放底角会和它叠在一起糊成一团。顶部是素材的空白区。
    Text {
        visible: root.showId
        anchors.left: root.isBlue ? undefined : parent.left
        anchors.right: root.isBlue ? parent.right : undefined
        anchors.leftMargin: 2
        anchors.rightMargin: 2
        anchors.top: parent.top
        anchors.topMargin: 1
        text: root.shownId
        color: root.online ? "#F5F7FA" : "#7D848C"
        font.pixelSize: Math.max(9, Math.round(14 * root.k))
        font.bold: true
        style: Text.Outline
        styleColor: "#101214"
    }

    Item {
        visible: root.showBar
        width: 59 * root.k
        height: Math.max(3, 6 * root.k)
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 1

        Rectangle { anchors.fill: parent; color: "#1A1D22" }

        // 没有 RobotDynamicStatus 时不画填充 —— 画满格会和「满血」混淆。
        // 高光用渐变现画:tab_bar.png 中间是一条不透明的亮色实心条,叠在填充上
        // 会在条子正中透出一道异色线(花名册那条「绿条中间一根红线」就是它)。
        Rectangle {
            visible: root.hasHp
            height: parent.height
            width: root.hasHp
                ? Math.max(0, Math.min(parent.width,
                    parent.width * (root.currentHP / Math.max(1, root.maxHP))))
                : 0
            gradient: Gradient {
                GradientStop {
                    position: 0.0
                    color: root.isBlue ? "#7FBEFF" : "#FF8A8A"
                }
                GradientStop {
                    position: 0.5
                    color: root.isBlue ? "#4A9BFF" : "#FF5555"
                }
                GradientStop {
                    position: 1.0
                    color: root.isBlue ? "#2C6FC4" : "#C63A3A"
                }
            }
        }
    }
}
