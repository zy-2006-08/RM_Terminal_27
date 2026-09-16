import QtQuick
import "."

// 花名册整列。行高按可用高度均分,不写死 —— 写死会在 720p 把最后一行画到面板外面
// (这正是「红方 4 号切一半、7 号不显示」那个 bug 的成因)。
Panel {
    id: root

    property var entries: []
    property bool isBlue: false
    property bool isAlly: true
    // 协议没给自机时无法断定哪边是我方,标题退回中立的「红方/蓝方」,绝不猜。
    property bool factionKnown: false

    title: factionKnown ? (isAlly ? "我方机器人" : "敌方机器人")
                        : (isBlue ? "蓝方机器人" : "红方机器人")
    mirrored: !isAlly
    // 标题用去饱和后的阵营色:纯红标题压过下面的数据行,而标题只需被认出、不需抢注意。
    accent: Qt.darker(Theme.factionColor(isBlue), 1.35)

    // 存活计数。分母是「有血量数据的机器人数」而不是固定 5:没有数据的槽位
    // 既不能算活也不能算死,把它算进分母会让「4/5」被读成有一台阵亡。
    badge: {
        var known = 0, alive = 0
        for (var i = 0; i < entries.length; ++i) {
            if (entries[i].live === 0) continue
            known += 1
            if (entries[i].live === 1) alive += 1
        }
        return known > 0 ? "(" + alive + "/" + known + ")" : ""
    }

    Column {
        id: rows
        anchors.fill: parent
        spacing: 5

        // 行高封顶 72px:拉满可用高度会让卡片随下区(战场事件)的高度忽胖忽瘦,
        // 而头像和字号都跟着行高缩放,同一台车在一局里会变形。封顶后余量统一
        // 落在面板底部,是一段安静的留白,而不是十行各自被撑开一点。
        //
        // 不可降到 70 以下:cpp/roster_pane.cpp 的 kRowCompactBelow=70 是紧凑版
        // (去兵种行、缩头像)的触发线,压过去两条绘制路径就会画出不同的卡片。
        readonly property real rowHeight:
            Math.min(72, (height - (Math.max(1, root.entries.length) - 1) * spacing)
                         / Math.max(1, root.entries.length))

        Repeater {
            model: root.entries
            RosterRow {
                width: rows.width
                height: rows.rowHeight
                robotId: modelData.id
                isBlue: root.isBlue
                liveState: modelData.live
                hasHp: modelData.hasHp
                currentHp: modelData.hp
                maxHp: modelData.maxHp
                robotClass: modelData.cls
                hasPosition: modelData.hasPosition
                positionStale: modelData.positionStale
            }
        }
    }
}
