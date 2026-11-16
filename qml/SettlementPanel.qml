import QtQuick
import RmTerminal.Glass 1.0
import "."
import "victory_easing.js" as Anim

// 结算面板。官方选手端那一版的结构:两侧队标 + 中间大比分,下面一列双向对比条,
// 最外两列是双方逐台机器人的剩余血量,底部一行关闭提示。
//
// 缺数据一律显示「--」。这一屏是赛后复盘的唯一依据,一个编出来的 0 会被当成
// 「真的打成了 0」记进战术笔记。
Item {
    id: root

    // cpp/game_result.cpp 的 settlement_variant()。
    property var model: ({})
    property string outcome: ""
    property color tint: Theme.red
    // 0 → 1 的进场进度,由 VictoryOverlay 的时钟驱动。
    property real progress: 0
    // 1440x900 为基准的等比缩放系数。
    property real scaleK: 1

    visible: progress > 0.001

    readonly property bool redWin: outcome === "red"
    readonly property bool blueWin: outcome === "blue"
    readonly property real k: scaleK

    function num(key) {
        if (!model || model[key + "Known"] !== true) return "--"
        return String(model[key])
    }

    // mm:ss。协议在结算阶段给不出比赛阶段的用时,所以这里通常是「--」,
    // 见 SettlementModel::duration_sec 的注释。
    readonly property string durationText: {
        if (!model || model.durationSecKnown !== true) return "--"
        var total = Math.max(0, model.durationSec)
        var m = Math.floor(total / 60)
        var sec = total % 60
        return m + ":" + (sec < 10 ? "0" + sec : sec)
    }

    readonly property string roundText: {
        if (!model) return "Round --/--"
        var cur = model.roundKnown === true ? model.round : "--"
        var all = model.totalRoundsKnown === true ? model.totalRounds : "--"
        return "Round " + cur + "/" + all
    }

    // 面板自身:约 70% 窗宽,从下方滑入并淡入。
    Item {
        id: panel
        width: Math.min(root.width * 0.70, 1480 * root.k)
        height: Math.min(root.height * 0.74, 660 * root.k)
        x: (root.width - width) / 2
        y: (root.height - height) / 2 + Anim.mix(70 * root.k, 0, Anim.outCubic(root.progress))
        opacity: Anim.outQuad(root.progress)

        GlassShape {
            anchors.fill: parent
            cut: 18 * root.k
            fillColor: Theme.overlay
            strokeColor: Theme.borderBright
            highlight: true
        }

        // 获胜方那一侧的大片底光:谁赢了要在一眼之内读出来。
        Rectangle {
            visible: !root.model || !root.model.draw
            x: root.blueWin ? parent.width / 2 : 0
            width: parent.width / 2
            height: parent.height
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop {
                    position: 0.0
                    color: Theme.alpha(root.tint, root.blueWin ? 0 : 0.14)
                }
                GradientStop {
                    position: 1.0
                    color: Theme.alpha(root.tint, root.blueWin ? 0.14 : 0)
                }
            }
        }

        // ================= 头部:队标 + 比分 =================
        Item {
            id: header
            x: 28 * root.k
            y: 20 * root.k
            width: parent.width - 56 * root.k
            height: 136 * root.k

            // 一侧的队标 + 名称 + WINNER。
            component SideHead: Item {
                id: side
                property bool blue: false
                property bool winner: false
                readonly property color sideTint: side.blue ? Theme.blue : Theme.red

                Image {
                    id: badge
                    source: side.blue ? "qrc:/images/top_mid/blue_team_logo.png"
                                      : "qrc:/images/top_mid/red_team_logo.png"
                    width: 74 * root.k
                    height: width
                    x: side.blue ? side.width - width : 0
                    y: 8 * root.k
                    fillMode: Image.PreserveAspectFit
                    smooth: true
                }

                Text {
                    x: side.blue ? side.width - badge.width - 14 * root.k - implicitWidth
                                 : badge.width + 14 * root.k
                    y: badge.y + 12 * root.k
                    // 队伍名协议不发。写阵营而不是编一个队名:错的队名比没有队名糟。
                    text: side.blue ? "蓝方" : "红方"
                    color: side.blue ? Theme.blueText : Theme.redText
                    font.family: Theme.titleFamily
                    font.pixelSize: Math.round(30 * root.k)
                    font.weight: Font.Black
                    font.letterSpacing: 2 * root.k
                }

                // 大而淡的 WINNER,压在获胜方那一侧。
                Text {
                    visible: side.winner
                    x: side.blue ? side.width - badge.width - 14 * root.k - implicitWidth
                                 : badge.width + 14 * root.k
                    y: badge.y + 46 * root.k
                    text: "WINNER"
                    color: Theme.alpha(side.sideTint, 0.42)
                    font.family: Theme.numericFamily
                    font.pixelSize: Math.round(44 * root.k)
                    font.weight: Font.Black
                    font.italic: true
                    font.letterSpacing: 5 * root.k
                }
            }

            SideHead {
                width: parent.width * 0.33
                height: parent.height
                blue: false
                winner: root.redWin
            }
            SideHead {
                x: parent.width * 0.67
                width: parent.width * 0.33
                height: parent.height
                blue: true
                winner: root.blueWin
            }

            // 中间:Round x/y → 比分 → 用时。
            Column {
                x: parent.width * 0.33
                width: parent.width * 0.34
                spacing: 2 * root.k

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: root.roundText
                    color: Theme.textSecondary
                    font.family: Theme.numericFamily
                    font.pixelSize: Math.round(19 * root.k)
                    font.weight: Font.DemiBold
                    font.letterSpacing: 2 * root.k
                    font.features: Theme.tabular
                }

                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: 14 * root.k

                    Text {
                        text: root.num("redScore")
                        color: Theme.redText
                        font.family: Theme.numericFamily
                        font.pixelSize: Math.round(64 * root.k)
                        font.weight: Font.Black
                        font.features: Theme.tabular
                    }
                    Text {
                        anchors.baseline: undefined
                        text: ":"
                        color: Theme.textMuted
                        font.family: Theme.numericFamily
                        font.pixelSize: Math.round(50 * root.k)
                        font.weight: Font.Bold
                    }
                    Text {
                        text: root.num("blueScore")
                        color: Theme.blueText
                        font.family: Theme.numericFamily
                        font.pixelSize: Math.round(64 * root.k)
                        font.weight: Font.Black
                        font.features: Theme.tabular
                    }
                }

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: root.durationText
                    color: Theme.textMuted
                    font.family: Theme.numericFamily
                    font.pixelSize: Math.round(20 * root.k)
                    font.weight: Font.Medium
                    font.features: Theme.tabular
                }
            }
        }

        Rectangle {
            x: 28 * root.k
            y: header.y + header.height
            width: parent.width - 56 * root.k
            height: 1
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0.0; color: Theme.alpha(Theme.borderBright, 0) }
                GradientStop { position: 0.5; color: Theme.borderBright }
                GradientStop { position: 1.0; color: Theme.alpha(Theme.borderBright, 0) }
            }
        }

        // ================= 中间:对比行 + 两侧机器人列 =================
        Item {
            id: body
            x: 24 * root.k
            y: header.y + header.height + 14 * root.k
            width: parent.width - 48 * root.k
            height: parent.height - y - 46 * root.k

            readonly property real colW: Math.max(150 * root.k, width * 0.195)

            RobotColumn {
                width: body.colW
                height: body.height
                blue: false
                robots: root.model ? root.model.redRobots : []
                k: root.k
            }

            RobotColumn {
                x: body.width - body.colW
                width: body.colW
                height: body.height
                blue: true
                robots: root.model ? root.model.blueRobots : []
                k: root.k
            }

            // 对比行。每行:红值 | 标签(+图标) | 蓝值,下面一对从中线往外长的条。
            Column {
                x: body.colW + 18 * root.k
                width: body.width - 2 * body.colW - 36 * root.k
                height: body.height
                spacing: Math.max(4 * root.k, (body.height - 6 * rowH) / 6)

                readonly property real rowH: 56 * root.k

                Repeater {
                    model: root.model ? root.model.rows : []

                    SettlementRow {
                        width: parent.width
                        height: parent.rowH
                        label: modelData.label
                        icon: modelData.icon
                        redKnown: modelData.redKnown
                        blueKnown: modelData.blueKnown
                        redValue: modelData.red
                        blueValue: modelData.blue
                        redText: modelData.redText
                        blueText: modelData.blueText
                        scaleMax: modelData.scale
                        k: root.k
                        // 条从中线往外长:0.35s 内长到位,错开一点出场更有节奏。
                        grow: Anim.outCubic(Math.max(0, Math.min(1,
                            (root.progress - 0.25 - index * 0.055) / 0.5)))
                    }
                }
            }
        }

        // ================= 底部提示 =================
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            y: parent.height - 32 * root.k
            text: "按 Esc 关闭"
            color: Theme.textMuted
            font.family: Theme.labelFamily
            font.pixelSize: Math.round(17 * root.k)
            font.weight: Font.Medium
            font.letterSpacing: 1.5 * root.k
        }
    }
}
