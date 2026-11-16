import QtQuick
import "."

// 大标题「红方胜利」/「蓝方胜利」/「平局」:白→浅阵营色的竖向渐变 + 多层发光描边。
//
// 用 Canvas 而不是 Text + 效果:本机没有 Qt5Compat.GraphicalEffects,而
// QtQuick.Effects 的 MultiEffect 是着色器效果 —— 按 glass_items.h 里记下的结论,
// 着色器效果在离屏平台和没有显卡的机器上画不出来,而结算截图正是离屏跑的。
// Canvas 的 renderTarget: Image 走 QPainter,离屏一样画得出渐变字和描边。
Item {
    id: root

    property string text: ""
    property color tint: Theme.red
    property int pixelSize: 96
    // 字间距。Canvas 没有 letterSpacing,逐字排版时自己加(见 layout())。
    property real spacing: 8
    // 轻微斜切,官方那种电竞字的倾斜感。0 = 正体。
    property real slant: 0.10

    // 字形占的实际宽度,给调用方对齐用。
    readonly property real inkWidth: sheet.inkWidth
    implicitHeight: pixelSize * 1.42
    implicitWidth: sheet.inkWidth

    onTextChanged: sheet.requestPaint()
    onTintChanged: sheet.requestPaint()
    onPixelSizeChanged: sheet.requestPaint()

    Canvas {
        id: sheet
        anchors.fill: parent
        renderStrategy: Canvas.Immediate
        renderTarget: Canvas.Image

        property real inkWidth: 0

        // 逐字算位置,顺手把总宽报出去。
        function layout(ctx) {
            var chars = []
            var total = 0
            for (var i = 0; i < root.text.length; ++i) {
                var ch = root.text.charAt(i)
                var w = ctx.measureText(ch).width
                chars.push({ ch: ch, w: w })
                total += w + (i < root.text.length - 1 ? root.spacing : 0)
            }
            inkWidth = total
            return chars
        }

        onPaint: {
            var ctx = getContext("2d")
            ctx.reset()
            if (width <= 0 || height <= 0 || root.text === "") return

            ctx.font = "bold " + root.pixelSize + "px \"" + Theme.titleFamily + "\""
            ctx.textBaseline = "alphabetic"
            var chars = layout(ctx)

            var baseline = height / 2 + root.pixelSize * 0.36
            var startX = (width - inkWidth) / 2
            var t = root.tint

            // 斜切绕基线做,否则字会整体平移出框。
            ctx.save()
            if (root.slant !== 0) {
                ctx.translate(0, baseline)
                ctx.transform(1, 0, -root.slant, 1, 0, 0)
                ctx.translate(0, -baseline)
            }

            function eachChar(draw) {
                var x = startX
                for (var i = 0; i < chars.length; ++i) {
                    draw(chars[i].ch, x)
                    x += chars[i].w + root.spacing
                }
            }

            // 1) 发光:由外到内三层阵营色描边,层层收窄加亮。
            var glow = [
                { w: root.pixelSize * 0.21, a: 0.16 },
                { w: root.pixelSize * 0.13, a: 0.26 },
                { w: root.pixelSize * 0.07, a: 0.42 }
            ]
            ctx.lineJoin = "round"
            ctx.lineCap = "round"
            for (var g = 0; g < glow.length; ++g) {
                ctx.lineWidth = glow[g].w
                ctx.strokeStyle = Qt.rgba(Math.min(1, t.r + 0.25), Math.min(1, t.g + 0.2),
                                          Math.min(1, t.b + 0.2), glow[g].a)
                eachChar(function (ch, x) { ctx.strokeText(ch, x, baseline) })
            }

            // 2) 深色压边:白字压在亮爆发层上会糊掉,先垫一圈暗轮廓。
            ctx.lineWidth = Math.max(2, root.pixelSize * 0.035)
            ctx.strokeStyle = Qt.rgba(0.03, 0.05, 0.09, 0.88)
            eachChar(function (ch, x) { ctx.strokeText(ch, x, baseline) })

            // 3) 字面:白 → 浅阵营色的竖向渐变。
            var top = baseline - root.pixelSize * 0.82
            var bottom = baseline + root.pixelSize * 0.12
            var fill = ctx.createLinearGradient(0, top, 0, bottom)
            fill.addColorStop(0, "#FFFFFF")
            fill.addColorStop(0.44, "#FFFFFF")
            fill.addColorStop(1, Qt.rgba(Math.min(1, t.r * 0.42 + 0.56),
                                         Math.min(1, t.g * 0.42 + 0.60),
                                         Math.min(1, t.b * 0.42 + 0.63), 1))
            ctx.fillStyle = fill
            eachChar(function (ch, x) { ctx.fillText(ch, x, baseline) })

            // 4) 上缘一道高光细边:金属字的迎光边。
            ctx.lineWidth = Math.max(1, root.pixelSize * 0.012)
            ctx.strokeStyle = Qt.rgba(1, 1, 1, 0.55)
            eachChar(function (ch, x) { ctx.strokeText(ch, x, baseline) })

            ctx.restore()
        }

        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
    }
}
