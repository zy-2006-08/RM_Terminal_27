import QtQuick
import RmTerminal.Glass 1.0
import "."

// 结算面板两侧的机器人列:每台车一行,头像 + 编号兵种 + 剩余血量 / 上限 + 一条血条。
//
// 列标题是「剩余血量」而不是「伤害」:协议没有逐台伤害统计(见 SettlementRobot),
// 写「伤害」就只能填 0 或编数。
Item {
    id: root

    property bool blue: false
    property var robots: []
    property real k: 1

    readonly property color tint: root.blue ? Theme.blue : Theme.red

    // 列头。
    Item {
        id: head
        width: parent.width
        height: 30 * root.k

        Rectangle {
            x: root.blue ? parent.width - width : 0
            y: 6 * root.k
            width: 3 * root.k
            height: 15 * root.k
            color: root.tint
            antialiasing: true
        }

        Text {
            x: root.blue ? parent.width - implicitWidth - 10 * root.k : 10 * root.k
            y: 4 * root.k
            text: "剩余血量"
            color: Theme.textSecondary
            font.family: Theme.labelFamily
            font.pixelSize: Math.round(15 * root.k)
            font.weight: Font.DemiBold
            font.letterSpacing: 1 * root.k
        }
    }

    // 没有一台车的数据时说出来,不留一列空白 —— 空白读不出是「没有数据」
    // 还是「界面没画」。
    Text {
        visible: !root.robots || root.robots.length === 0
        anchors.horizontalCenter: parent.horizontalCenter
        y: head.height + 20 * root.k
        text: "无机器人数据"
        color: Theme.textMuted
        font.family: Theme.labelFamily
        font.pixelSize: Math.round(14 * root.k)
    }

    Column {
        y: head.height + 4 * root.k
        width: parent.width
        spacing: 6 * root.k

        Repeater {
            model: root.robots

            Item {
                id: row
                width: parent.width
                height: 54 * root.k

                readonly property bool hasHp: modelData.hasHp
                readonly property real ratio: (hasHp && modelData.maxHp > 0)
                    ? Math.max(0, Math.min(1, modelData.hp / modelData.maxHp)) : 0
                readonly property bool dead: hasHp && modelData.hp === 0

                GlassShape {
                    anchors.fill: parent
                    cut: 5 * root.k
                    mirrored: root.blue
                    fillColor: Theme.row
                }

                // 头像。和花名册共用 Theme.avatarFor 的映射,两处不能各写一份。
                GlassImage {
                    id: face
                    x: root.blue ? parent.width - width - 6 * root.k : 6 * root.k
                    y: (parent.height - height) / 2
                    width: 48 * root.k
                    height: 41 * root.k
                    source: Theme.avatarFor(modelData.cls, root.blue)
                    gray: row.dead
                    opacity: row.hasHp ? (row.dead ? 0.5 : 1) : 0.42
                }

                Item {
                    x: root.blue ? 8 * root.k : face.width + 12 * root.k
                    width: parent.width - face.width - 20 * root.k
                    height: parent.height

                    Row {
                        y: 7 * root.k
                        spacing: 5 * root.k
                        layoutDirection: root.blue ? Qt.RightToLeft : Qt.LeftToRight
                        width: parent.width

                        Text {
                            text: modelData.number
                            color: Theme.textPrimary
                            font.family: Theme.numericFamily
                            font.pixelSize: Math.round(19 * root.k)
                            font.weight: Font.Bold
                            font.features: Theme.tabular
                        }
                        Text {
                            anchors.baseline: undefined
                            y: 3 * root.k
                            text: modelData.cls
                            color: Theme.textSecondary
                            font.family: Theme.labelFamily
                            font.pixelSize: Math.round(13 * root.k)
                            font.weight: Font.Medium
                        }
                    }

                    // 血量读数。缺数据写「--」,绝不写 0:0 会被读成阵亡。
                    Row {
                        y: 7 * root.k
                        spacing: 1
                        x: root.blue ? 0 : parent.width - width
                        Text {
                            text: row.hasHp ? modelData.hp : "--"
                            color: row.hasHp
                                ? (row.dead ? Theme.textMuted : Theme.healthColor(row.ratio))
                                : Theme.textMuted
                            font.family: Theme.numericFamily
                            font.pixelSize: Math.round(19 * root.k)
                            font.weight: Font.Bold
                            font.features: Theme.tabular
                        }
                        Text {
                            anchors.baseline: parent.children[0].baseline
                            visible: row.hasHp
                            text: "/" + modelData.maxHp
                            color: Theme.textMuted
                            font.family: Theme.numericFamily
                            font.pixelSize: Math.round(12 * root.k)
                            font.weight: Font.Medium
                            font.features: Theme.tabular
                        }
                    }

                    // 血条。没有血量数据时整条不画 —— 空条会被读成血量 0 即阵亡。
                    Item {
                        visible: row.hasHp
                        y: 34 * root.k
                        width: parent.width
                        height: Math.max(4, 5 * root.k)

                        Rectangle {
                            anchors.fill: parent
                            color: Qt.rgba(1, 1, 1, 0.07)
                            antialiasing: true
                        }
                        Rectangle {
                            x: root.blue ? parent.width - width : 0
                            width: parent.width * row.ratio
                            height: parent.height
                            antialiasing: true
                            visible: width > 0.5
                            color: Theme.healthFill(row.ratio, root.blue)
                        }
                    }

                    Text {
                        visible: row.dead
                        y: 30 * root.k
                        x: root.blue ? 0 : parent.width - implicitWidth
                        text: "阵亡"
                        color: Theme.textMuted
                        font.family: Theme.labelFamily
                        font.pixelSize: Math.round(12 * root.k)
                        font.weight: Font.Medium
                    }
                }
            }
        }
    }
}
