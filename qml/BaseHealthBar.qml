import QtQuick
import "."

// 基地血条。分层结构照 ClearWei/RM26_Client 的 BaseHealthBar.qml(MIT):
// bottom(空槽) → middle(深层) → top(浅层) → border(边框) → light(高光)。
// 官方把血量分深浅两层,所以掉血时先掉浅层再掉深层,而不是一条单色缩短。
Item {
    id: root
    property int currentHealth: 0
    property int maxHealth: 0
    property bool isBlue: false
    // GameStatus 的基地血量字段缺失时整条不画填充,只留空槽 + 说明文字,避免把
    // 「没有数据」画成满血或空血。
    property bool hasData: false

    readonly property string prefix: isBlue ? "qrc:/images/blue_base_blood/blue_base_blood_"
                                            : "qrc:/images/red_base_blood/red_base_blood_"
    readonly property int safeMax: Math.max(1, maxHealth)
    readonly property int clamped: Math.max(0, Math.min(safeMax, currentHealth))
    readonly property real healthRatio: hasData ? clamped / safeMax : 0

    // 深层容量取上限的 60%:官方深/浅两层的分界。
    // 空槽底图相对边框图的尺寸比。红方两张同尺寸(比例 1),蓝方 880x104 对 837x72,
    // 所以需要外扩 880/837、104/72 才能让可视内容和边框对齐。
    readonly property real slotOverscanX: isBlue ? 880 / 837 : 1
    readonly property real slotOverscanY: isBlue ? 104 / 72 : 1

    readonly property real deepCapacity: safeMax * 0.6
    readonly property real deepRatio: Math.min(clamped, deepCapacity) / deepCapacity
    readonly property real lightRatio: safeMax > deepCapacity
        ? Math.max(0, clamped - deepCapacity) / (safeMax - deepCapacity) : 0

    // 空槽底图。蓝方的 bottom.png 是 880x104,比 border.png(837x72)四周多出一圈
    // 透明发光边;红方那张就是 837x72。直接 Stretch 铺满会把蓝方那圈边压进可视区,
    // 于是蓝方血条外沿多一圈黑影、红蓝不对称。这里按两张图的比例把绘制区外扩,
    // 让多余的边溢出到可视范围之外,两侧就一致了。
    Image {
        id: slot
        source: root.prefix + "bottom.png"
        fillMode: Image.Stretch
        anchors.centerIn: parent
        width: parent.width * (root.slotOverscanX)
        height: parent.height * (root.slotOverscanY)
    }

    Item {
        width: root.width * root.deepRatio * (root.hasData ? 1 : 0)
        height: root.height
        clip: true
        anchors.left: root.isBlue ? parent.left : undefined
        anchors.right: root.isBlue ? undefined : parent.right
        Image {
            width: root.width
            height: root.height
            source: root.prefix + "middle.png"
            fillMode: Image.Stretch
            anchors.left: root.isBlue ? parent.left : undefined
            anchors.right: root.isBlue ? undefined : parent.right
        }
    }

    Item {
        width: root.width * root.lightRatio * (root.hasData ? 1 : 0)
        height: root.height
        clip: true
        anchors.left: root.isBlue ? parent.left : undefined
        anchors.right: root.isBlue ? undefined : parent.right
        Image {
            width: root.width
            height: root.height
            source: root.prefix + "top.png"
            fillMode: Image.Stretch
            anchors.left: root.isBlue ? parent.left : undefined
            anchors.right: root.isBlue ? undefined : parent.right
        }
    }

    Image {
        anchors.fill: parent
        source: root.prefix + "border.png"
        fillMode: Image.Stretch
    }

    // 血量数字居中。只显示当前值:上限是常量(5000),每帧重复它没有信息量,
    // 而且「581 / 5000」在这个宽度里挤成一团。描边足以让白字在亮红/亮蓝填充上读清,
    // 不需要再垫深色底板 —— 那块半透明方框在条上是个突兀的补丁。
    Text {
        id: healthText
        anchors.centerIn: parent
        text: root.hasData ? root.clamped : "无基地血量"
        color: root.hasData ? "#FFFFFF" : "#8A929C"
        font.pixelSize: root.hasData ? 19 : 10
        font.bold: root.hasData
        font.family: root.hasData ? Theme.numericFamily : Theme.labelFamily
        font.letterSpacing: root.hasData ? 0.6 : 0
        style: root.hasData ? Text.Outline : Text.Normal
        styleColor: "#0B1013"
    }
}
