import QtQuick
import "."

// 带翼六角徽章。官方客户端那枚徽章:中间一块六边形盾,两侧各一只翼,金属质感 +
// 阵营色内发光,盾的上半部压着 R 标。
//
// 用 Canvas 自绘而不是贴图:用户明确要求代码绘制,不引入外部帧素材。盾和翼都是
// 直边多边形,Canvas 的路径 + 渐变足够画出金属感,且只在尺寸变化时重画一次。
Item {
    id: root

    property color tint: Theme.red
    // 平局:银灰盘,不用阵营色。
    property bool muted: false
    // R 标。空串时盾面只有金属底,不画占位方块。
    property string logo: ""

    readonly property real k: height / 220

    Canvas {
        id: plate
        anchors.fill: parent
        renderStrategy: Canvas.Immediate
        renderTarget: Canvas.Image

        onPaint: {
            var ctx = getContext("2d")
            ctx.reset()
            if (width <= 0 || height <= 0) return

            var cx = width / 2
            var cy = height / 2
            // 盾宽:整体宽度的中间一段,两侧留给翼。
            var sw = height * 0.78
            var sh = height * 0.92
            var t = root.tint
            // 金属底色:平局走银灰,胜方走「阵营色压暗后掺灰」,不是纯阵营色 ——
            // 纯色盘会糊成一块,读不出金属。
            var baseHi = root.muted ? Qt.rgba(0.82, 0.85, 0.90, 1)
                                    : Qt.rgba(Math.min(1, t.r * 0.55 + 0.46),
                                              Math.min(1, t.g * 0.55 + 0.46),
                                              Math.min(1, t.b * 0.55 + 0.48), 1)
            // 暗端刻意不压到接近黑:盾在爆发层正上方,一块近黑的板会把中心的光
            // 挖成一个黑洞(弹入的前几帧尤其明显),读起来像缺图而不像金属。
            var baseLo = root.muted ? Qt.rgba(0.30, 0.33, 0.39, 1)
                                    : Qt.rgba(t.r * 0.34 + 0.16, t.g * 0.34 + 0.17,
                                              t.b * 0.34 + 0.21, 1)

            // ---- 翼:每侧四根羽片,越往下越短,整排向外上方扬起。 ----
            //
            // 羽片画成「根部厚、外端收成一个上翘的尖」的四点曲边形:矩形加个斜角
            // 只会读成一块金属板,而翼要能读出羽毛的方向。
            var feathers = [
                { len: 1.00, top: -0.250, thick: 0.115, rise: 0.105, a: 0.98 },
                { len: 0.89, top: -0.105, thick: 0.105, rise: 0.070, a: 0.88 },
                { len: 0.74, top: 0.025, thick: 0.095, rise: 0.040, a: 0.74 },
                { len: 0.57, top: 0.140, thick: 0.082, rise: 0.016, a: 0.58 }
            ]
            for (var side = -1; side <= 1; side += 2) {
                for (var f = 0; f < feathers.length; ++f) {
                    var fe = feathers[f]
                    var rx = cx + side * sw * 0.40
                    var y0 = cy + height * fe.top
                    var span = (width / 2 - sw * 0.40) * fe.len
                    var tipx = rx + side * span
                    // 外端往上翘:翼是扬起来的,不是平伸的。
                    var tipy = y0 - height * fe.rise
                    var th = height * fe.thick

                    ctx.beginPath()
                    ctx.moveTo(rx, y0 - th * 0.52)
                    // 上沿:往外端拱一下再收到尖,羽片的前缘是弧线。
                    ctx.quadraticCurveTo(rx + side * span * 0.55, y0 - th * 0.72 - height * fe.rise * 0.5,
                                         tipx, tipy)
                    // 下沿收回根部,收口处比上沿更陡 —— 羽片末端是个尖,不是个平头。
                    ctx.quadraticCurveTo(rx + side * span * 0.42, y0 + th * 0.28,
                                         rx, y0 + th * 0.52)
                    ctx.closePath()

                    var fg = ctx.createLinearGradient(rx, y0, tipx, tipy)
                    fg.addColorStop(0, Qt.rgba(baseHi.r, baseHi.g, baseHi.b, fe.a))
                    fg.addColorStop(0.5, Qt.rgba(
                        (baseHi.r + baseLo.r) / 2, (baseHi.g + baseLo.g) / 2,
                        (baseHi.b + baseLo.b) / 2, fe.a * 0.9))
                    fg.addColorStop(1, Qt.rgba(baseLo.r, baseLo.g, baseLo.b, fe.a * 0.28))
                    ctx.fillStyle = fg
                    ctx.fill()

                    // 前缘一道亮边。金属的高光只落在迎光的那条边上,整片描边会变成贴纸。
                    ctx.beginPath()
                    ctx.moveTo(rx, y0 - th * 0.52)
                    ctx.quadraticCurveTo(rx + side * span * 0.55, y0 - th * 0.72 - height * fe.rise * 0.5,
                                         tipx, tipy)
                    ctx.lineWidth = Math.max(1, height * 0.0055)
                    ctx.strokeStyle = Qt.rgba(1, 1, 1, 0.5 * fe.a)
                    ctx.stroke()
                }
            }

            // ---- 盾:六边形(上下各一个尖,两侧竖边),官方那枚的轮廓。 ----
            var hx = sw / 2
            var hy = sh / 2
            // 竖边占总高的比例。0.52 才读得出「六边形盾」:再小就变成钻石/菱形,
            // 官方那枚的两侧是明显的竖边。
            var flank = 0.52
            function shieldPath() {
                ctx.beginPath()
                ctx.moveTo(cx, cy - hy)
                ctx.lineTo(cx + hx, cy - hy * flank)
                ctx.lineTo(cx + hx, cy + hy * flank)
                ctx.lineTo(cx, cy + hy)
                ctx.lineTo(cx - hx, cy + hy * flank)
                ctx.lineTo(cx - hx, cy - hy * flank)
                ctx.closePath()
            }

            // 外发光:盾外一圈阵营色,让徽章从压暗的仪表盘上浮起来。
            ctx.save()
            shieldPath()
            var og = ctx.createRadialGradient(cx, cy, hy * 0.5, cx, cy, hy * 1.5)
            og.addColorStop(0, Qt.rgba(t.r, t.g, t.b, 0.0))
            og.addColorStop(1, Qt.rgba(t.r, t.g, t.b, 0.55))
            ctx.lineWidth = Math.max(2, height * 0.05)
            ctx.strokeStyle = og
            ctx.stroke()
            ctx.restore()

            // 盾面:斜向金属渐变,上亮下暗。
            shieldPath()
            var sg = ctx.createLinearGradient(cx - hx, cy - hy, cx + hx * 0.4, cy + hy)
            sg.addColorStop(0, Qt.rgba(baseHi.r, baseHi.g, baseHi.b, 1))
            sg.addColorStop(0.34, Qt.rgba(
                baseHi.r * 0.72 + baseLo.r * 0.28, baseHi.g * 0.72 + baseLo.g * 0.28,
                baseHi.b * 0.72 + baseLo.b * 0.28, 1))
            sg.addColorStop(0.52, Qt.rgba(baseLo.r * 1.2, baseLo.g * 1.2, baseLo.b * 1.25, 1))
            sg.addColorStop(1, Qt.rgba(baseLo.r * 0.86, baseLo.g * 0.86, baseLo.b * 0.92, 1))
            ctx.fillStyle = sg
            ctx.fill()

            // 内发光:沿盾内缘一圈阵营色,金属里透出的那层光。
            ctx.save()
            shieldPath()
            ctx.clip()
            shieldPath()
            var ig = ctx.createRadialGradient(cx, cy, hy * 0.35, cx, cy, hy * 1.05)
            ig.addColorStop(0, Qt.rgba(t.r, t.g, t.b, 0))
            ig.addColorStop(0.72, Qt.rgba(Math.min(1, t.r + 0.1), Math.min(1, t.g + 0.08),
                                          Math.min(1, t.b + 0.08), 0.35))
            ig.addColorStop(1, Qt.rgba(1, 1, 1, 0.5))
            ctx.lineWidth = Math.max(2, height * 0.055)
            ctx.strokeStyle = ig
            ctx.stroke()
            ctx.restore()

            // 盾的描边:外侧一道亮金属边。
            shieldPath()
            ctx.lineWidth = Math.max(1.5, height * 0.011)
            var eg = ctx.createLinearGradient(cx, cy - hy, cx, cy + hy)
            eg.addColorStop(0, Qt.rgba(1, 1, 1, 0.92))
            eg.addColorStop(0.5, Qt.rgba(baseHi.r, baseHi.g, baseHi.b, 0.7))
            eg.addColorStop(1, Qt.rgba(1, 1, 1, 0.3))
            ctx.strokeStyle = eg
            ctx.stroke()

            // 上半部的一道斜切高光:金属盘的「反射带」,只覆盖上 40%。
            ctx.save()
            shieldPath()
            ctx.clip()
            ctx.beginPath()
            ctx.moveTo(cx - hx, cy - hy * 0.1)
            ctx.lineTo(cx + hx, cy - hy * 0.42)
            ctx.lineTo(cx + hx, cy - hy)
            ctx.lineTo(cx - hx, cy - hy)
            ctx.closePath()
            ctx.fillStyle = Qt.rgba(1, 1, 1, 0.13)
            ctx.fill()
            ctx.restore()

            // 盾腰的一道分隔线:R 标区和标题区的分界,官方那枚也有。
            ctx.save()
            shieldPath()
            ctx.clip()
            ctx.beginPath()
            ctx.moveTo(cx - hx, cy + hy * 0.02)
            ctx.lineTo(cx + hx, cy + hy * 0.02)
            ctx.lineWidth = Math.max(1, height * 0.007)
            ctx.strokeStyle = Qt.rgba(1, 1, 1, 0.22)
            ctx.stroke()
            ctx.restore()
        }

        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        // 配色变了必须重画。Canvas 的内容是光栅缓存,不会因为绑定的属性变化自己失效 ——
        // 漏了这两行,上一局红胜留下的红盘会被蓝胜/平局直接拿去用。
        Connections {
            target: root
            function onTintChanged() { plate.requestPaint() }
            function onMutedChanged() { plate.requestPaint() }
        }
    }

    // R 标。压在盾的上半部 —— 标题随后横压在盾腰上,两者不能叠在一起。
    Image {
        source: root.logo
        visible: root.logo !== "" && status === Image.Ready
        width: root.height * 0.30
        height: width
        x: (root.width - width) / 2
        y: root.height * 0.5 - root.height * 0.26 - height / 2
        fillMode: Image.PreserveAspectFit
        smooth: true
    }
}
