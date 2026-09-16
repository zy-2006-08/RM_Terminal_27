import QtQuick
import "."

// 前哨站牌。整体照搬 ClearWei/RM26_Client 的 OutpostStatusBadge.qml(MIT):
// 92x52 底板用 top_robots 的 teammate_bg 斜切卡面,血槽用 tab_bar 切图,
// 「前哨站」字样和图标用 top_outpost 的 name/avatar。
//
// 之前自己用 outpost_bg + 自绘血条拼,质感对不上 —— 官方那条血槽是带内发光的
// 切图,Rectangle 画不出来。镜像逻辑也照搬:红方左对齐、蓝方右对齐,
// 两块牌分列血条外侧时才会朝向中央对称。
//
// 前哨站是赛制关键中立目标,血量必须来自协议 GameStatus;
// hasData 为假时只显示占位符,绝不画成满血。
Item {
    id: root
    width: 92
    height: 52
    clip: true

    property int currentHp: 0
    property int maxHp: 0
    property bool hasData: false
    property bool isBlue: false

    // 参考端按 92x52 等比缩放全部内部尺寸,改外框时内部自动跟随。
    readonly property real contentScale: Math.min(width / 92, height / 52)
    readonly property string team: isBlue ? "blue" : "red"
    readonly property int safeMax: Math.max(1, maxHp)
    readonly property int clamped: Math.max(0, Math.min(safeMax, currentHp))
    readonly property real ratio: hasData ? clamped / safeMax : 0
    readonly property bool fallen: hasData && clamped === 0

    Image {
        id: bg
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        width: Math.round(92 * root.contentScale)
        height: Math.round(46 * root.contentScale)
        source: "qrc:/images/top_robots/" + root.team + "_teammate_bg.png"
        fillMode: Image.Stretch
        smooth: true
        opacity: root.fallen ? 0.6 : 1.0
    }

    Image {
        id: nameTag
        source: "qrc:/images/top_outpost/" + root.team + "_outpost_name.png"
        fillMode: Image.PreserveAspectFit
        smooth: true
        width: Math.round(50 * root.contentScale)
        height: Math.round(15 * root.contentScale)
        anchors.left: root.isBlue ? undefined : bg.left
        anchors.right: root.isBlue ? bg.right : undefined
        anchors.leftMargin: Math.round(10 * root.contentScale)
        anchors.rightMargin: Math.round(10 * root.contentScale)
        anchors.top: bg.top
        anchors.topMargin: Math.round(5 * root.contentScale)
        opacity: root.fallen ? 0.7 : 1.0
    }

    // 已击毁时显示状态词而不是 0:一个孤零零的 0 容易被误读成数据缺失。
    Text {
        id: hpText
        anchors.left: root.isBlue ? undefined : bg.left
        anchors.right: root.isBlue ? bg.right : undefined
        anchors.leftMargin: Math.round(24 * root.contentScale)
        anchors.rightMargin: Math.round(24 * root.contentScale)
        anchors.bottom: hpBar.top
        anchors.bottomMargin: Math.round(2 * root.contentScale)
        width: Math.round(34 * root.contentScale)
        text: !root.hasData ? "--" : (root.fallen ? "已击毁" : root.clamped)
        color: root.hasData ? "#FFFFFF" : Theme.textMuted
        font.pixelSize: Math.round((root.fallen ? 10 : 13) * root.contentScale)
        font.bold: true
        horizontalAlignment: root.isBlue ? Text.AlignRight : Text.AlignLeft
        elide: Text.ElideRight
        style: Text.Outline
        styleColor: "#101214"
    }

    Image {
        source: "qrc:/images/top_outpost/" + root.team + "_outpost_avatar.png"
        fillMode: Image.PreserveAspectFit
        smooth: true
        width: Math.round(12 * root.contentScale)
        height: Math.round(14 * root.contentScale)
        anchors.left: root.isBlue ? undefined : hpText.right
        anchors.right: root.isBlue ? hpText.left : undefined
        anchors.leftMargin: Math.round(6 * root.contentScale)
        anchors.rightMargin: Math.round(6 * root.contentScale)
        anchors.verticalCenter: hpText.verticalCenter
        anchors.verticalCenterOffset: Math.round(root.contentScale)
        opacity: root.fallen ? 0.7 : 1.0
    }

    Item {
        id: hpBar
        anchors.bottom: bg.bottom
        anchors.left: root.isBlue ? bg.left : undefined
        anchors.right: root.isBlue ? undefined : bg.right
        anchors.leftMargin: Math.round(3 * root.contentScale)
        anchors.rightMargin: Math.round(3 * root.contentScale)
        width: Math.round(64 * root.contentScale)
        height: Math.round(6 * root.contentScale)

        Image {
            anchors.fill: parent
            source: "qrc:/images/top_robots/" + root.team + "_tab_bar.png"
            fillMode: Image.Stretch
            smooth: true
            opacity: root.fallen ? 0.5 : 0.95
        }

        Rectangle {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            width: root.hasData ? parent.width * root.ratio : 0
            height: Math.max(2, Math.round(2 * root.contentScale))
            radius: height / 2
            color: root.fallen ? "#999999" : (root.isBlue ? "#31BAFF" : "#FF2E45")
            opacity: root.fallen ? 0.6 : 0.95
        }
    }

    Rectangle {
        anchors.fill: bg
        color: "#44000000"
        visible: root.fallen
    }
}
