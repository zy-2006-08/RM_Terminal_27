import QtQuick
import RmTerminal.Glass 1.0
import "."
import "victory_easing.js" as Anim

// 爆发层:中心放射的能量条纹 + 碎片 + 焰状絮。全部用 Canvas 画,一次性光栅化成
// 一张图,之后只靠 scale/opacity 变换 —— 笔记本集显上每帧重画几百条带渐变的路径
// 会掉到十几帧,而这段动画恰好是全场最需要流畅的 1.5 秒。
//
// 形状用固定种子的伪随机数生成:同一次结算里每次重画都长一样,否则窗口一缩放
// 条纹就全部跳位;而换一种结果(红/蓝/平局)时种子不同,三张图不会一模一样。
Item {
    id: root

    property color tint: Theme.red
    // 动画时钟(毫秒),由 VictoryOverlay 统一驱动。
    property real tMs: 0
    // 平局是银灰调:条纹更短更钝,不要赢家那种外冲的锐利感。
    property bool muted: false

    readonly property real cx: width / 2
    readonly property real cy: height / 2
    // 对角线半径:条纹要能飞出屏幕四角,否则宽屏上两侧会露出空白的扇形。
    readonly property real reach: Math.sqrt(cx * cx + cy * cy)

    // 0 → 1 的爆发进度。1.5 秒,与验收描述一致。
    readonly property real burst: Anim.span(tMs, 120, 1500)

    Canvas {
        id: sheet
        anchors.fill: parent
        renderStrategy: Canvas.Immediate
        renderTarget: Canvas.Image

        // 条纹本身不动,整张图按 burst 外扩并淡出:等价于每条都在往外飞,
        // 但只光栅化一次。
        transform: Scale {
            origin.x: root.cx
            origin.y: root.cy
            xScale: Anim.mix(0.18, 1.0, Anim.outQuint(root.burst))
            yScale: Anim.mix(0.18, 1.0, Anim.outQuint(root.burst))
        }
        // 前 15% 冲到最亮,之后淡出;末尾不归零 —— 官方客户端最终留着一层余光。
        opacity: root.burst < 0.15
            ? Anim.span(root.tMs, 120, 225)
            : Anim.mix(1, 0.12, Anim.outCubic(Anim.span(root.burst, 0.15, 0.85)))

        onPaint: {
            var ctx = getContext("2d")
            ctx.reset()
            if (width <= 0 || height <= 0) return

            var cx = width / 2
            var cy = height / 2
            var reach = Math.sqrt(cx * cx + cy * cy)
            var rnd = Anim.seeded(root.muted ? 97 : (root.tint.r > root.tint.b ? 1337 : 4242))

            // 1) 焰状絮:宽而软的扇形,垫在条纹底下给整团光一点体积。
            var wisps = root.muted ? 14 : 22
            for (var w = 0; w < wisps; ++w) {
                var wa = rnd() * Math.PI * 2
                var wlen = reach * (root.muted ? 0.45 : 0.62) * (0.45 + rnd() * 0.75)
                var wspread = (root.muted ? 0.05 : 0.085) * (0.6 + rnd() * 0.9)
                ctx.beginPath()
                ctx.moveTo(cx, cy)
                ctx.arc(cx, cy, wlen, wa - wspread, wa + wspread)
                ctx.closePath()
                var wg = ctx.createRadialGradient(cx, cy, 0, cx, cy, wlen)
                wg.addColorStop(0, Qt.rgba(root.tint.r, root.tint.g, root.tint.b, 0.30))
                wg.addColorStop(0.45, Qt.rgba(root.tint.r, root.tint.g, root.tint.b, 0.12))
                wg.addColorStop(1, Qt.rgba(root.tint.r, root.tint.g, root.tint.b, 0))
                ctx.fillStyle = wg
                ctx.fill()
            }

            // 2) 条纹:从中心往外收窄的细长三角,末端带一点白芯。
            var streaks = root.muted ? 46 : 84
            for (var i = 0; i < streaks; ++i) {
                var a = rnd() * Math.PI * 2
                // 长短混搭:全都一样长会变成一个规整的星芒,像图标而不像爆炸。
                var tier = rnd()
                var len = reach * (tier > 0.82 ? 1.05 : (tier > 0.5 ? 0.78 : 0.52))
                    * (0.72 + rnd() * 0.5) * (root.muted ? 0.72 : 1)
                var halfw = (root.muted ? 2.2 : 3.4) * (0.5 + rnd() * 1.5)
                var inner = reach * 0.055

                var dx = Math.cos(a)
                var dy = Math.sin(a)
                // 法向量,用来给根部撑出宽度。
                var nx = -dy
                var ny = dx

                ctx.beginPath()
                ctx.moveTo(cx + dx * len, cy + dy * len)
                ctx.lineTo(cx + dx * inner + nx * halfw, cy + dy * inner + ny * halfw)
                ctx.lineTo(cx + dx * inner - nx * halfw, cy + dy * inner - ny * halfw)
                ctx.closePath()

                var g = ctx.createLinearGradient(cx, cy, cx + dx * len, cy + dy * len)
                var hot = tier > 0.82 ? 0.95 : 0.7
                g.addColorStop(0, Qt.rgba(1, 1, 1, hot * 0.55))
                g.addColorStop(0.18, Qt.rgba(
                    Math.min(1, root.tint.r + 0.35), Math.min(1, root.tint.g + 0.3),
                    Math.min(1, root.tint.b + 0.3), hot))
                g.addColorStop(0.62, Qt.rgba(root.tint.r, root.tint.g, root.tint.b, hot * 0.55))
                g.addColorStop(1, Qt.rgba(root.tint.r, root.tint.g, root.tint.b, 0))
                ctx.fillStyle = g
                ctx.fill()
            }

            // 3) 碎片:离散的小菱形,散在中段,给爆炸一点颗粒。
            var shards = root.muted ? 26 : 54
            for (var s = 0; s < shards; ++s) {
                var sa = rnd() * Math.PI * 2
                var sr = reach * (0.2 + rnd() * 0.72)
                var sl = (root.muted ? 7 : 11) * (0.5 + rnd() * 1.6)
                var sw = sl * (0.16 + rnd() * 0.2)
                var sx = cx + Math.cos(sa) * sr
                var sy = cy + Math.sin(sa) * sr
                var sdx = Math.cos(sa)
                var sdy = Math.sin(sa)

                ctx.beginPath()
                ctx.moveTo(sx + sdx * sl, sy + sdy * sl)
                ctx.lineTo(sx - sdy * sw, sy + sdx * sw)
                ctx.lineTo(sx - sdx * sl, sy - sdy * sl)
                ctx.lineTo(sx + sdy * sw, sy - sdx * sw)
                ctx.closePath()
                var fade = 1 - sr / (reach * 1.05)
                ctx.fillStyle = Qt.rgba(
                    Math.min(1, root.tint.r + 0.45), Math.min(1, root.tint.g + 0.4),
                    Math.min(1, root.tint.b + 0.4), Math.max(0.1, fade) * 0.85)
                ctx.fill()
            }
        }

        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        // 同 VictoryBadge:Canvas 是光栅缓存,配色变了不会自己失效。漏了这两行,
        // 蓝胜和平局会拿着上一局红胜的那团红光播。
        Connections {
            target: root
            function onTintChanged() { sheet.requestPaint() }
            function onMutedChanged() { sheet.requestPaint() }
        }
    }

    // 中心球:爆发瞬间的白热核心,很快收掉。半径刻意只到 0.30 ——
    // 再大就会盖掉条纹,整团光糊成一个白球,读不出「炸开」。
    //
    // 用 SoftGlow(QPainter 的 QRadialGradient)而不是带 Gradient 的 Rectangle:
    // QML 的 Gradient 只有**线性**一种,拿它画「球」会在半径处留下一道水平接缝,
    // 看起来像个圆顶,而不是一团光。
    Item {
        readonly property real r: root.reach * Anim.mix(0.04, 0.32, Anim.outQuint(root.burst))
        x: root.cx - r
        y: root.cy - r
        width: r * 2
        height: r * 2
        opacity: root.burst < 0.12
            ? Anim.span(root.tMs, 100, 180)
            : Anim.mix(1, 0, Anim.outQuad(Anim.span(root.burst, 0.12, 0.55)))

        SoftGlow {
            anchors.fill: parent
            color: Theme.alpha(root.tint, 0.50)
        }
        // 白热的芯,比外层小得多:越往里越白,但不盖住条纹。
        SoftGlow {
            anchors.centerIn: parent
            width: parent.width * 0.46
            height: parent.height * 0.46
            color: Qt.rgba(1, 1, 1, 0.70)
        }
    }

    // 冲击波:一圈很细、很快、很淡的波前,只在最初 0.7 秒出现。
    //
    // 刻意压到几乎看不见(峰值 0.18 透明度、线宽 1–2px、0.7s 内散掉):描边圆
    // 在深色背景上非常容易读成一个几何图形(准星、加载圈),而这段动画要像爆炸。
    // 它的作用只是给前几帧一点向外的速度暗示,不是一个要被看见的元素。
    Rectangle {
        readonly property real p: Anim.span(root.tMs, 120, 700)
        readonly property real r: root.reach * Anim.mix(0.10, 1.05, Anim.outQuint(p))
        x: root.cx - r
        y: root.cy - r
        width: r * 2
        height: r * 2
        radius: r
        color: "transparent"
        antialiasing: true
        visible: p > 0.001 && p < 1
        border.width: Math.max(1, root.reach * 0.004 * (1 - p))
        border.color: Qt.rgba(Math.min(1, root.tint.r + 0.35), Math.min(1, root.tint.g + 0.3),
                              Math.min(1, root.tint.b + 0.3),
                              0.18 * (1 - Anim.outQuad(p)))
    }

    // 余光:动画结束后留在徽章背后的一团柔光,官方客户端的最终状态就是这样。
    // 同样走 SoftGlow 的真径向渐变,线性渐变会留下一道接缝。
    SoftGlow {
        readonly property real r: root.reach * 0.56
        x: root.cx - r
        y: root.cy - r
        width: r * 2
        height: r * 2
        opacity: Anim.ramp(root.tMs, 900, 900, 0, 1, Anim.outCubic)
        color: Theme.alpha(root.tint, root.muted ? 0.22 : 0.28)
    }
}
