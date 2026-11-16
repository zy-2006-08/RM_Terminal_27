import QtQuick
import "."

// 花名册整列。行高按可用高度均分,不写死 —— 写死会在小窗口把最后一行画到面板外面。
Item {
    id: root

    // 窗口背景里这一块(见 glass_items.h)。QQuickWidget 看不到下面 Dashboard 画的背景,
    // 自己垫一份,半透明的面板底下才透得出同样的光晕。
    property string backdropSource: ""
    property var entries: []
    property bool isBlue: false
    property bool isAlly: true
    // 协议没给自机时无法断定哪边是我方,标题退回中立的「红方/蓝方」,绝不猜。
    property bool factionKnown: false
    property bool lowHealthPulse: true
    property bool mirrored: !isAlly
    // 「下一步」给某台车标的编号:{机器人 id: "1"}。
    property var ranks: ({})
    // 界面设置页开着:标题栏右边的「存活」往左让,给齿轮和叉留地方。
    property real toolInset: 0

    Image {
        anchors.fill: parent
        source: root.backdropSource
        cache: false
        smooth: false
        visible: root.backdropSource !== ""
    }

    Panel {
        id: panel
        anchors.fill: parent
        title: root.factionKnown ? (root.isAlly ? "我方机器人" : "敌方机器人")
                                 : (root.isBlue ? "蓝方机器人" : "红方机器人")
        mirrored: root.mirrored
        accent: Theme.factionColor(root.isBlue)

        // 存活计数。分母是「有血量数据的机器人数」:没有数据的槽位既不能算活也不能算死。
        readonly property var counts: {
            var known = 0, alive = 0
            for (var i = 0; i < root.entries.length; ++i) {
                if (root.entries[i].live === 0) continue
                known += 1
                if (root.entries[i].live === 1) alive += 1
            }
            return { known: known, alive: alive }
        }
        badgeLabel: counts.known > 0 ? "存活" : ""
        badge: counts.known > 0 ? counts.alive + "/" + counts.known : ""
        badgeInset: root.toolInset

        Column {
            id: rows
            anchors.fill: parent
            // 行高按 62 设计,但不设上限:几行就把面板的高度均分成几份,全部用掉。
            // 以前封在 104 并把余量留在面板底部,四行的花名册在高面板里下面空掉一大半。
            // 行间距跟着面板高度走(最多 12),但绝不吃掉剩余高度 —— 余量进行高,不进空隙。
            readonly property int rowCount: Math.max(1, root.entries.length)
            readonly property real spacingFor: rowCount > 1 ? Math.max(4, Math.min(12, height / (rowCount * 8))) : 0
            readonly property real rowHeight: Math.max(1, (height - (rowCount - 1) * spacingFor) / rowCount)
            spacing: spacingFor

            Repeater {
                // Snapshot lists are replaced every tick; a count model preserves pulse phase
                // and the running hit animations.
                model: root.entries.length
                RosterRow {
                    readonly property var entry: root.entries[index]
                    width: rows.width
                    height: rows.rowHeight
                    robotId: entry.id
                    isBlue: root.isBlue
                    isAlly: root.isAlly
                    isSelf: entry.self === true
                    factionKnown: root.factionKnown
                    pulseEnabled: root.lowHealthPulse
                    liveState: entry.live
                    hasHp: entry.hasHp
                    currentHp: entry.hp
                    maxHp: entry.maxHp
                    robotClass: entry.cls
                    hasPosition: entry.hasPosition
                    positionStale: entry.positionStale
                    bullets: entry.bullets
                    buffs: entry.buffs !== undefined ? entry.buffs : []
                    rank: root.ranks[entry.id] !== undefined ? root.ranks[entry.id] : ""
                }
            }
        }
    }
}
