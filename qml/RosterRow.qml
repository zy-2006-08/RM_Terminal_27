import QtQuick
import RmTerminal.Glass 1.0
import "."

// 花名册单行(效果图 v5):头像 + 编号兵种 + 标签 + 弹丸 + 血量,下面一条斜切的发光血条。
// 三态(Alive/Kia/NoData)必须视觉可分:「阵亡」是可利用的战机,「无数据」是情报缺失,
// 画成同一种灰会让操作手做出相反决策。
//
// 掉血:数字和血条主体在受击那一帧就更新,掉的那段留白色残影 0.35 秒,再用 0.45 秒缩回,
// 数字闪红 0.3 秒。回血:血条 0.3 秒长到位,新增那段绿色提示 0.6 秒淡出。
Item {
    id: root

    property int robotId: 0
    property bool isBlue: false
    property bool isAlly: true
    property bool isSelf: false
    property bool factionKnown: false
    property int liveState: 0        // 0=NoData 1=Alive 2=Kia
    property bool hasHp: false
    property int currentHp: 0
    property int maxHp: 0
    property string robotClass: ""
    property bool hasPosition: false
    property bool positionStale: false
    // 只有己方有:剩余累计发弹量(GlobalUnitStatus.robot_bullets),-1 = 没有数据。
    property int bullets: -1
    // 「下一步」给这台车标的编号,空串不画。
    property string rank: ""
    // 车身上的增益:[{text, tone}],tone 0 有利 1 不利 2 中性。要紧的在前,放不下的不画。
    property var buffs: []
    // 操作手可以在界面设置里关掉残血呼吸;标签、红色数字和红条照常显示。
    property bool pulseEnabled: true

    readonly property bool kia: liveState === 2
    readonly property bool noData: liveState === 0
    readonly property bool online: liveState !== 0
    readonly property bool lowHealth: factionKnown && !isAlly && liveState === 1
        && hasHp && maxHp > 0 && currentHp > 0 && currentHp / maxHp <= 0.25
    readonly property bool lowHealthAlert: pulseEnabled && lowHealth
    readonly property int shownId: robotId > 100 ? robotId - 100 : robotId
    readonly property real ratio: (hasHp && maxHp > 0)
        ? Math.max(0, Math.min(1, currentHp / maxHp)) : 0
    readonly property color faction: Theme.factionColor(isBlue)
    readonly property color barColor: Theme.healthFill(ratio, isBlue)

    // 按 62 像素高设计。行高变了,上下位置跟着高度缩放;头像、文字的大小还要看宽度够不够
    // (一行要约 290 像素宽才放得下 1 倍的字),行变高、宽度不变时字不会挤到一起。
    readonly property real k: height / 62
    // 宽度允许放到多大:一行要约 290 像素宽才放得下 1 倍的字。
    readonly property real kw: Math.max(0.85, (width - 12) / 290)
    // 内容块的大小:跟着行高长,但被宽度兜住 —— 行被拖高、宽度没变时字不会挤到一起或溢出。
    // 上限 2.5:再大就比面板标题还抢眼。
    readonly property real kxBase: Math.max(0.85, Math.min(2.5, Math.min(k, kw)))
    // 增益标签优先放在血条下面一行,整块(编号行 + 血条 + 标签)按 60 高排;行太矮时
    // 挤进编号那一行、血量数字左边,放不下的不画。
    readonly property bool hasBuffs: buffs.length > 0 && liveState === 1
    readonly property bool buffsBelow: hasBuffs && (height - 4) / 60 >= 0.85
    readonly property real kx: buffsBelow ? Math.min(kxBase, (height - 4) / 60) : kxBase
    // 字号同一个系数:行拖大字就变大、拖小就变小(以前封在 1.25,行里空一大半)。
    readonly property real fk: kx
    // 文字和血条排成一块(62 高时从 12 到 48),在行里上下居中:行变高时上下多出来的空一样多。
    readonly property real blockTop: (height - (buffsBelow ? 60 : 36) * kx) / 2

    property real shownRatio: 0
    property real trailHi: 0
    property real healLo: 0
    property real healHi: 0
    property real healOpacity: 0
    property real hpFlash: 0
    property real lastRatio: -1

    function resetBar() {
        trailAnim.stop()
        growAnim.stop()
        healFade.stop()
        flashAnim.stop()
        shownRatio = ratio
        trailHi = ratio
        healOpacity = 0
        hpFlash = 0
        lastRatio = (hasHp && !noData) ? ratio : -1
    }

    function onValue() {
        if (!hasHp || noData || lastRatio < 0) {
            resetBar()
            return
        }
        if (ratio < lastRatio - 0.0001) {
            growAnim.stop()
            healFade.stop()
            healOpacity = 0
            shownRatio = ratio
            trailHi = Math.max(trailHi, lastRatio)
            trailAnim.restart()
            flashAnim.restart()
        } else if (ratio > lastRatio + 0.0001) {
            trailAnim.stop()
            trailHi = ratio
            healLo = shownRatio
            healHi = ratio
            healOpacity = 0.6
            growAnim.from = shownRatio
            growAnim.to = ratio
            growAnim.restart()
            healFade.restart()
        }
        lastRatio = ratio
    }

    onRatioChanged: onValue()
    onHasHpChanged: resetBar()
    onRobotIdChanged: resetBar()
    Component.onCompleted: resetBar()

    SequentialAnimation {
        id: trailAnim
        PauseAnimation { duration: 350 }
        NumberAnimation { target: root; property: "trailHi"; to: root.ratio; duration: 450; easing.type: Easing.OutCubic }
    }
    NumberAnimation { id: growAnim; target: root; property: "shownRatio"; duration: 300; easing.type: Easing.OutCubic }
    SequentialAnimation {
        id: healFade
        PauseAnimation { duration: 300 }
        NumberAnimation { target: root; property: "healOpacity"; to: 0; duration: 600 }
    }
    NumberAnimation { id: flashAnim; target: root; property: "hpFlash"; from: 1; to: 0; duration: 300 }

    component Tag: Item {
        id: tag
        property string label: ""
        property color ink: Theme.accentText
        property color fill: Theme.accentBg
        property bool rankStyle: false
        property real zoom: 1
        implicitWidth: tagText.implicitWidth + Math.round((rankStyle ? 14 : 12) * zoom)
        implicitHeight: Math.round(19 * zoom)
        GlassShape { anchors.fill: parent; cut: 4; fillColor: tag.fill }
        Text {
            id: tagText
            anchors.centerIn: parent
            anchors.verticalCenterOffset: 0.5
            text: tag.label
            color: tag.ink
            font.family: tag.rankStyle ? Theme.numericFamily : Theme.labelFamily
            font.pixelSize: Math.round((tag.rankStyle ? 13 : 12) * tag.zoom)
            font.weight: Font.Bold
            font.italic: tag.rankStyle
            font.letterSpacing: tag.rankStyle ? 0 : 1
        }
    }

    GlassShape {
        anchors.fill: parent
        cut: 6
        fillColor: root.isSelf ? Theme.rowSelf : Theme.row
    }

    // 敌方残血:左侧红竖条 + 淡红底色,一起慢慢呼吸(1.0 ↔ 0.55,半周期 650ms)。
    Item {
        id: lowGlow
        objectName: "lowHealthGlow"
        anchors.fill: parent
        visible: root.lowHealth
        opacity: root.lowHealthAlert ? pulse : 1
        property real pulse: 1
        SequentialAnimation on pulse {
            running: root.lowHealthAlert
            loops: Animation.Infinite
            NumberAnimation { to: 0.55; duration: 650; easing.type: Easing.InOutSine }
            NumberAnimation { to: 1.0; duration: 650; easing.type: Easing.InOutSine }
        }
        GlassShape {
            anchors.fill: parent
            cut: 6
            fillColor: Qt.rgba(1, 100 / 255, 100 / 255, 0.26)
            fillEnd: Qt.rgba(1, 100 / 255, 100 / 255, 0)
            fillEndAt: 0.8
        }
        Rectangle {
            x: -2; y: 9 * root.k - 2
            width: 7; height: parent.height - 18 * root.k + 4
            color: Qt.rgba(1, 100 / 255, 100 / 255, 0.28)
            radius: 3
        }
        Rectangle {
            y: 9 * root.k
            width: 3
            height: parent.height - 18 * root.k
            color: Theme.danger
        }
    }

    Rectangle {
        visible: root.isSelf && !root.lowHealth
        y: 9 * root.k
        width: 3
        height: parent.height - 18 * root.k
        color: Theme.cyan
    }

    Item {
        id: portrait
        // 头像再按 kx 放大也不能超出行:行很矮时(面板只有一两格高)按行高收,不画到行外面。
        readonly property real zoom: Math.min(root.kx, (root.height - 8) / 48)
        x: 8 * root.kx
        y: (root.height - height) / 2
        width: 56 * zoom
        height: 48 * zoom
        opacity: root.kia ? 0.5 : (root.noData ? 0.4 : 1)

        SoftGlow {
            x: 2
            width: parent.width - 4
            height: 18 * portrait.zoom
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 1
            color: Theme.alpha(Theme.factionFill(root.isBlue), 0.42)
        }
        GlassImage {
            anchors.fill: parent
            source: Theme.avatarFor(root.robotClass, root.isBlue)
            gray: root.kia
        }
    }

    Item {
        id: textArea
        // 跟着头像实际占的宽度走:头像被行高压小了,字就往左挪,不留一条空当。
        x: portrait.x + portrait.width + 10 * root.kx
        width: parent.width - x - 12
        height: parent.height

        Row {
            id: lineA
            y: root.blockTop
            height: 22 * root.kx
            spacing: 7
            opacity: root.kia ? 0.55 : 1

            Text {
                id: idText
                anchors.verticalCenter: parent.verticalCenter
                text: root.shownId
                color: root.online ? Theme.textPrimary : Theme.textMuted
                font.family: Theme.numericFamily
                font.pixelSize: Math.round(21 * root.fk)
                font.weight: Font.Bold
                font.features: Theme.tabular
            }
            Text {
                anchors.baseline: idText.baseline
                text: root.robotClass
                color: root.online ? Theme.textSecondary : Theme.textMuted
                font.family: Theme.labelFamily
                font.pixelSize: Math.round(13 * root.fk)
                font.weight: Font.Medium
            }
            Tag {
                anchors.verticalCenter: parent.verticalCenter
                visible: root.isSelf
                label: "本机"
                zoom: root.fk
            }
            Tag {
                anchors.verticalCenter: parent.verticalCenter
                visible: root.lowHealth
                label: "残血"
                ink: "#FFD6D6"
                fill: Qt.rgba(1, 100 / 255, 100 / 255, 0.32)
                zoom: root.fk
            }
            Tag {
                anchors.verticalCenter: parent.verticalCenter
                visible: root.rank !== ""
                label: root.rank
                ink: "#FFFFFF"
                fill: Qt.rgba(64 / 255, 170 / 255, 230 / 255, 0.55)
                rankStyle: true
                zoom: root.fk
            }
            Row {
                objectName: "bulletsText"
                anchors.baseline: idText.baseline
                baselineOffset: ammoValue.y + ammoValue.baselineOffset
                visible: root.bullets >= 0 && !root.noData
                leftPadding: 3
                spacing: 4
                Text {
                    anchors.baseline: ammoValue.baseline
                    text: "弹丸"
                    color: Theme.textMuted
                    font.family: Theme.labelFamily
                    font.pixelSize: Math.round(12 * root.fk)
                    font.weight: Font.Medium
                }
                Text {
                    id: ammoValue
                    text: root.bullets
                    color: root.bullets === 0 ? Theme.warn : Theme.textBright
                    font.family: Theme.numericFamily
                    font.pixelSize: Math.round(14 * root.fk)
                    font.weight: Font.DemiBold
                    font.features: Theme.tabular
                }
            }
        }

        Row {
            id: hpRow
            anchors.right: parent.right
            y: lineA.y + idText.y + idText.baselineOffset - hpValue.baselineOffset
            visible: root.hasHp && !root.noData && !root.kia
            spacing: 1
            Text {
                id: hpValue
                text: root.currentHp
                readonly property color base: root.lowHealth ? Theme.dangerText : Theme.textPrimary
                color: root.hpFlash > 0 ? Qt.tint(base, Qt.rgba(1, 196 / 255, 196 / 255, root.hpFlash)) : base
                font.family: Theme.numericFamily
                font.pixelSize: Math.round(21 * root.fk)
                font.weight: Font.Bold
                font.features: Theme.tabular
            }
            Text {
                anchors.baseline: hpValue.baseline
                text: "/" + root.maxHp
                color: Theme.textMuted
                font.family: Theme.numericFamily
                font.pixelSize: Math.round(12.5 * root.fk)
                font.weight: Font.Medium
                font.features: Theme.tabular
            }
        }

        Tag {
            anchors.right: parent.right
            y: lineA.y + (lineA.height - height) / 2
            visible: root.kia
            label: "阵亡"
            ink: Theme.textBright
            fill: Qt.rgba(1, 1, 1, 0.09)
            zoom: root.fk
        }

        // 增益标签按顺序排,放不下的整个不画,不画半个。
        Item {
            id: buffRow
            objectName: "buffChips"
            readonly property int chipPx: Math.max(11, Math.round(12 * root.kx))
            readonly property real pad: Math.round(6 * root.kx)
            readonly property real gap: Math.round(5 * root.kx)
            readonly property real start: root.buffsBelow ? 0 : lineA.width + Math.round(8 * root.kx)
            readonly property real room: root.buffsBelow
                ? textArea.width
                : Math.max(0, (hpRow.visible ? hpRow.x : textArea.width) - start - Math.round(8 * root.kx))
            readonly property int fit: {
                var used = 0
                var n = 0
                for (var i = 0; i < root.buffs.length; ++i) {
                    var w = chipMetrics.advanceWidth(root.buffs[i].text) + 2 * pad
                    if (n > 0) w += gap
                    if (used + w > room) break
                    used += w
                    n += 1
                }
                return n
            }
            visible: root.hasBuffs && fit > 0
            x: start
            y: root.buffsBelow ? root.blockTop + 41 * root.kx : lineA.y + (lineA.height - height) / 2
            width: room
            height: Math.round(19 * root.kx)

            FontMetrics {
                id: chipMetrics
                font.family: Theme.labelFamily
                font.pixelSize: buffRow.chipPx
                font.weight: Font.Bold
            }
            Row {
                spacing: buffRow.gap
                Repeater {
                    model: buffRow.fit
                    Item {
                        readonly property var chip: root.buffs[index]
                        width: chipText.implicitWidth + 2 * buffRow.pad
                        height: buffRow.height
                        GlassShape {
                            anchors.fill: parent
                            cut: 4
                            fillColor: chip.tone === 0 ? Qt.rgba(62 / 255, 207 / 255, 142 / 255, 0.20)
                                     : chip.tone === 1 ? Qt.rgba(1, 100 / 255, 100 / 255, 0.26)
                                                       : Theme.accentBg
                        }
                        Text {
                            id: chipText
                            anchors.centerIn: parent
                            anchors.verticalCenterOffset: 0.5
                            text: chip.text
                            color: chip.tone === 0 ? Theme.okText : chip.tone === 1 ? Theme.dangerText : Theme.accentText
                            font.family: Theme.labelFamily
                            font.pixelSize: buffRow.chipPx
                            font.weight: Font.Bold
                        }
                    }
                }
            }
        }

        // 无数据:状态点 + 文字,绝不画空血条 —— 空条会被读成血量 0 即阵亡。
        Row {
            visible: root.noData
            y: root.blockTop + 26 * root.kx
            spacing: 6
            Rectangle {
                width: 7; height: 7; radius: 3.5
                anchors.verticalCenter: parent.verticalCenter
                color: root.hasPosition ? (root.positionStale ? Theme.warn : Theme.ok) : Theme.offline
            }
            Text {
                text: root.hasPosition
                    ? (root.positionStale ? "仅位置 · 已过期" : "仅位置 · 无血量")
                    : "无数据"
                color: Theme.textMuted
                font.family: Theme.labelFamily
                font.pixelSize: Math.round(12 * root.fk)
            }
        }
    }

    // 血条:CSS skewX(-24deg) 的平行四边形,半透明渐变 + 一圈同色光晕,右端一道白色读数线。
    Item {
        id: bar
        visible: !root.noData
        x: textArea.x + 3
        y: root.blockTop + 30 * root.kx
        width: textArea.width - 6
        height: Math.max(4, Math.round(6 * root.kx))
        transform: Matrix4x4 {
            readonly property real s: Math.tan(-24 * Math.PI / 180)
            matrix: Qt.matrix4x4(1, s, 0, -s * bar.height / 2, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)
        }

        Rectangle { anchors.fill: parent; color: Qt.rgba(1, 1, 1, 0.07); antialiasing: true }
        Rectangle {
            visible: root.healOpacity > 0 && root.healHi > root.healLo
            x: root.healLo * bar.width
            width: (root.healHi - root.healLo) * bar.width
            height: parent.height
            color: Qt.rgba(110 / 255, 224 / 255, 164 / 255, root.healOpacity)
            antialiasing: true
        }
        Rectangle {
            visible: root.trailHi > root.ratio + 0.002
            x: root.ratio * bar.width
            width: (root.trailHi - root.ratio) * bar.width
            height: parent.height
            color: Qt.rgba(1, 1, 1, 0.5)
            antialiasing: true
        }
        Item {
            id: fill
            visible: root.shownRatio > 0.001
            width: root.shownRatio * bar.width
            height: parent.height
            // 光晕:同色半透明,四周各宽 2 像素。
            Rectangle {
                x: -2; y: -2
                width: parent.width + 4
                height: parent.height + 4
                radius: 2
                color: Theme.alpha(root.barColor, 0.2)
                antialiasing: true
            }
            Rectangle {
                anchors.fill: parent
                antialiasing: true
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0.0; color: Theme.alpha(root.barColor, 0.45) }
                    GradientStop { position: 1.0; color: Theme.alpha(root.barColor, 0.88) }
                }
            }
            Rectangle {
                anchors.right: parent.right
                width: 2
                height: parent.height
                color: Qt.rgba(1, 1, 1, 0.75)
            }
        }
    }
}
