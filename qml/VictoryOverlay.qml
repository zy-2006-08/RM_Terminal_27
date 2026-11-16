import QtQuick
import "."
import "victory_easing.js" as Anim

// 结算画面:约 3 秒的自绘动画,之后换成结算面板。
//
// 时间线(tMs 毫秒):
//   0–300    压暗层淡入
//   120–1620 爆发:中心球 + 冲击环 + 放射条纹/碎片/焰状絮,末尾留一层余光
//   260–1060 徽章弹入(0.5 → 1.08 → 1.0)
//   520–1120 标题横压在徽章上,白→浅阵营色竖向渐变 + 描边发光
//   900–1400 结束原因那一行淡入
//   2850–3250 徽章与标题缩小上移并淡出,结算面板同步滑入
//
// 动画由一个显式的 tMs 时钟驱动而不是 NumberAnimation:验收要截 t≈0.3/1.0/2.0/3.5s
// 四个时刻的图,墙上时间驱动下 grab() 抓到哪一帧取决于调度抖动,出不了可复现的图。
// 见 victory_easing.js 顶部。
Item {
    id: root

    // C++ 注入。result 决定配色与素材;空串表示没有结果,整层隐藏。
    property string title: ""
    property string reason: ""
    // "red" / "blue" / "draw" / ""
    property string outcome: ""
    // 结算面板的数据,见 cpp/game_result.cpp 的 settlement_variant()。
    property var model: ({})
    property bool shown: false

    // 动画时钟。C++ 可以直接写它来取某一时刻的画面(验收截图、测试),
    // 平时由下面的 ticker 推进。
    property real tMs: 0
    // ticker 跑不跑。截图工具把它关掉后自己设 tMs,就能拿到确定的那一帧。
    property bool autoPlay: true

    // 徽章+标题退场、面板进场的时刻。
    readonly property int handoffMs: 2850
    readonly property int handoffDur: 400

    visible: shown

    readonly property bool draw: outcome === "draw"
    readonly property color tint: draw ? Qt.rgba(0.72, 0.76, 0.82, 1)
                                      : (outcome === "blue" ? Theme.blue : Theme.red)
    readonly property string logo: draw ? ""
        : (outcome === "blue" ? "qrc:/images/top_mid/blue_team_logo.png"
                              : "qrc:/images/top_mid/red_team_logo.png")

    // 1440x900 为基准等比缩放,1280x720 到 2560x1440 都按同一套比例排。
    // 取宽高两个方向的较小比例:只按宽度缩放时,矮窗口上标题会顶到面板。
    readonly property real s: Math.min(width / 1440, height / 900)

    // 退场进度:0 动画在台上,1 完全让位给面板。
    readonly property real exit: Anim.span(tMs, handoffMs, handoffDur)

    // 压暗层。结算是全场唯一「可以完全接管屏幕」的时刻,所以压得比普通弹窗更狠。
    Rectangle {
        anchors.fill: parent
        color: "#000000"
        opacity: 0.70 * Anim.span(root.tMs, 0, 300)
    }

    VictoryBurst {
        anchors.fill: parent
        tint: root.tint
        muted: root.draw
        tMs: root.tMs
    }

    // 徽章 + 标题。整块一起退场:缩小、上移、淡出。
    Item {
        id: emblem
        width: parent.width
        height: parent.height
        // 退场时上移约 7% 窗高并缩到 0.82,面板从下方滑入接替。
        y: -root.height * 0.07 * Anim.outCubic(root.exit)
        opacity: 1 - Anim.outQuad(root.exit)
        scale: Anim.mix(1, 0.82, Anim.outCubic(root.exit))
        visible: opacity > 0.01

        Item {
            id: badgeBox
            width: 760 * root.s
            height: 300 * root.s
            x: (parent.width - width) / 2
            y: (parent.height - height) / 2 - 18 * root.s
            scale: Anim.popScale(root.tMs, 260, 560, 240)
            opacity: Anim.span(root.tMs, 260, 220)

            VictoryBadge {
                anchors.fill: parent
                tint: root.tint
                muted: root.draw
                logo: root.logo
            }
        }

        // 标题。横压在徽章中部 —— 官方客户端就是压着的,不是摆在徽章下方。
        VictoryTitle {
            id: titleBox
            width: parent.width
            height: implicitHeight
            y: badgeBox.y + badgeBox.height * 0.60 - height / 2
            text: root.title
            tint: root.tint
            pixelSize: Math.round(96 * root.s)
            spacing: 8 * root.s
            opacity: Anim.span(root.tMs, 520, 400)
            scale: Anim.ramp(root.tMs, 520, 600, 1.12, 1.0, Anim.outCubic)
        }

        // 结束原因。协议给不出可信编码时 reason 为空串,整行不画 ——
        // 结算屏上一句编造的理由比没有理由糟得多。
        Item {
            id: reasonBox
            visible: root.reason !== ""
            width: parent.width
            height: 30 * root.s
            y: titleBox.y + titleBox.height + 16 * root.s
            opacity: Anim.span(root.tMs, 900, 500)

            // 两侧各一道渐隐细线,把这行字收进一个横向的「条」里。
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                x: parent.width / 2 - reasonText.implicitWidth / 2 - 24 * root.s - width
                width: 110 * root.s
                height: 1
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0.0; color: Theme.alpha(root.tint, 0) }
                    GradientStop { position: 1.0; color: Theme.alpha(root.tint, 0.65) }
                }
            }
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                x: parent.width / 2 + reasonText.implicitWidth / 2 + 24 * root.s
                width: 110 * root.s
                height: 1
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0.0; color: Theme.alpha(root.tint, 0.65) }
                    GradientStop { position: 1.0; color: Theme.alpha(root.tint, 0) }
                }
            }

            Text {
                id: reasonText
                anchors.centerIn: parent
                text: root.reason
                color: Theme.textPrimary
                style: Text.Outline
                styleColor: Qt.rgba(0.04, 0.06, 0.11, 0.9)
                font.family: Theme.labelFamily
                font.pixelSize: Math.round(23 * root.s)
                font.weight: Font.DemiBold
                font.letterSpacing: 1.5 * root.s
            }
        }
    }

    SettlementPanel {
        anchors.fill: parent
        model: root.model
        outcome: root.outcome
        tint: root.tint
        scaleK: root.s
        // 徽章退场的同时滑入:两段动作重叠才像「交接」,前后脚会出现一帧空屏。
        progress: Anim.span(root.tMs, root.handoffMs, root.handoffDur + 160)
    }

    // 16ms 一跳,跟着 60Hz 走。到面板完全就位后停下 —— 结算面板是静态的,
    // 继续空转只在压榨 CPU。
    Timer {
        id: ticker
        interval: 16
        repeat: true
        running: false
        onTriggered: {
            root.tMs += interval
            if (root.tMs >= root.handoffMs + root.handoffDur + 400) ticker.stop()
        }
    }

    onShownChanged: {
        tMs = 0
        if (shown && autoPlay) ticker.restart()
        else ticker.stop()
    }
}
